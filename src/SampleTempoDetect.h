#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <vector>

// Estimates a sample's tempo from its onsets. Worker-only: it allocates and reads files.
// It prefers to find nothing over guessing, so one-shots and audio without a steady
// pulse return no tempo. A steady beat fits its half and double tempo about equally well,
// so it reports the one nearest 125 BPM; the caller must say it may be off by an octave.
namespace alien {
struct TempoEstimate { double bpm{}; float confidence{}; };

namespace tempodetect {
inline constexpr double envelopeRate = 200, minSeconds = 3, maxSeconds = 60, minBpm = 60, maxBpm = 200;
inline constexpr double preferredBpm = 125;
inline constexpr double minConfidence = .3, octaveMargin = .75;
inline constexpr std::size_t minOnsets = 4;

// Collects energy per hop in two bands, so a kick and a hat both count as onsets.
class OnsetEnvelope {
public:
    explicit OnsetEnvelope(double sampleRate) : hop(std::max(1, juce::roundToInt(sampleRate / envelopeRate))), rate(sampleRate / hop) {}
    double hopRate() const noexcept { return rate; }
    void push(const float* const* channels, int numChannels, int count) {
        for (int n = 0; n < count; ++n) {
            float mono = 0;
            for (int c = 0; c < numChannels; ++c) mono += channels[c][n];
            mono /= static_cast<float>(numChannels);
            if (!std::isfinite(mono)) mono = 0;
            const float high = mono - previous; previous = mono;
            fullSum += mono * mono; highSum += high * high;
            if (++filled == hop) {
                full.push_back(fullSum / static_cast<float>(hop)); highs.push_back(highSum / static_cast<float>(hop));
                fullSum = highSum = 0; filled = 0;
            }
        }
    }
    // Onset strength per hop; empty when the audio is silent.
    std::vector<float> onsets() const {
        std::vector<float> result(full.size(), 0.f);
        const auto peak = full.empty() ? 0.f : *std::max_element(full.begin(), full.end());
        if (peak < 1.0e-6f) return {};
        addBand(full, result); addBand(highs, result);
        return result;
    }
private:
    static void addBand(const std::vector<float>& energy, std::vector<float>& result) {
        const auto peak = *std::max_element(energy.begin(), energy.end());
        if (peak <= 0) return;
        // A 20 ms average hides the waveform's own cycles, so a steady low note has no onsets.
        constexpr std::size_t window = 4;
        float sum = 0, last = 0;
        for (std::size_t i = 0; i < energy.size(); ++i) {
            sum += energy[i]; if (i >= window) sum -= energy[i - window];
            const auto average = std::max(0.f, sum) / static_cast<float>(window);
            const auto level = std::log1p(100.f * std::sqrt(average / peak)) / std::log(101.f);
            if (i > 0) result[i] += std::max(0.f, level - last) * .5f;
            last = level;
        }
    }
    int hop; double rate;
    std::vector<float> full, highs;
    float previous{}, fullSum{}, highSum{}; int filled{};
};

inline std::size_t countOnsets(const std::vector<float>& flux, double rate) {
    const auto peak = *std::max_element(flux.begin(), flux.end());
    if (peak < .02f) return 0;
    const auto gap = static_cast<std::size_t>(std::ceil(rate * .06));
    std::size_t count = 0, last = 0;
    for (std::size_t i = 1; i + 1 < flux.size(); ++i)
        if (flux[i] >= peak * .25f && flux[i] >= flux[i - 1] && flux[i] > flux[i + 1] && (count == 0 || i - last >= gap)) { ++count; last = i; }
    return count;
}

inline std::optional<TempoEstimate> estimateFromOnsets(std::vector<float> flux, double rate, const std::atomic<bool>& cancel) {
    if (flux.empty() || static_cast<double>(flux.size()) < minSeconds * rate || countOnsets(flux, rate) < minOnsets) return {};
    float mean = 0; for (const auto value : flux) mean += value;
    mean /= static_cast<float>(flux.size());
    double power = 0; for (auto& value : flux) { value -= mean; power += double(value) * value; }
    power /= static_cast<double>(flux.size());
    if (power <= 1.0e-12) return {};

    // Correlation at each lag, up to eight beats at the slowest tempo.
    const auto size = flux.size();
    const auto maxLag = std::min(size * 3 / 4, static_cast<std::size_t>(std::ceil(8 * 60 * rate / minBpm)) + 2);
    std::vector<float> correlation(maxLag, 0.f);
    for (std::size_t lag = 0; lag < maxLag; ++lag) {
        if (cancel.load(std::memory_order_relaxed)) return {};
        double sum = 0;
        for (std::size_t i = 0; i + lag < size; ++i) sum += double(flux[i]) * flux[i + lag];
        correlation[lag] = static_cast<float>(sum / static_cast<double>(size - lag) / power);
    }

    // Score each tempo over several beats: the further multiples give finer resolution than one hop.
    static constexpr std::array<int, 6> beats{1, 2, 3, 4, 6, 8};
    const int first = juce::roundToInt(minBpm * 10), last = juce::roundToInt(maxBpm * 10);
    std::vector<double> scores(static_cast<std::size_t>(last - first + 1), -1.0);
    double top = 0;
    for (int tenths = first; tenths <= last; ++tenths) {
        const double period = 60 * rate * 10 / tenths;
        double score = 0; int used = 0;
        for (const auto beat : beats) {
            const double lag = beat * period;
            const auto below = static_cast<std::size_t>(lag);
            if (below + 1 >= maxLag) break;
            const double fraction = lag - static_cast<double>(below);
            score += correlation[below] * (1 - fraction) + correlation[below + 1] * fraction; ++used;
        }
        if (used < 2) continue;
        scores[static_cast<std::size_t>(tenths - first)] = score / used;
        top = std::max(top, score / used);
    }
    if (top < minConfidence) return {};

    // Slower octaves always fit a pulse about as well, so among the tempos that nearly
    // match the best fit take the one nearest the preferred tempo, then climb to the top
    // of that tempo's own peak so the preference does not pull the value.
    std::size_t chosen = 0; double nearest = 1.0e9;
    for (std::size_t i = 0; i < scores.size(); ++i) {
        if (scores[i] < top * octaveMargin) continue;
        const double distance = std::abs(std::log2((first + static_cast<double>(i)) / 10 / preferredBpm));
        if (distance < nearest) { nearest = distance; chosen = i; }
    }
    while (chosen > 0 && scores[chosen - 1] > scores[chosen]) --chosen;
    while (chosen + 1 < scores.size() && scores[chosen + 1] > scores[chosen]) ++chosen;
    return TempoEstimate{(first + static_cast<double>(chosen)) / 10, static_cast<float>(juce::jlimit(0.0, 1.0, scores[chosen]))};
}
} // namespace tempodetect

// Mono float samples, for tests and in-memory audio.
inline std::optional<TempoEstimate> estimateTempo(const float* mono, std::size_t frames, double sampleRate, const std::atomic<bool>& cancel) {
    if (mono == nullptr || !std::isfinite(sampleRate) || sampleRate < 1000) return {};
    tempodetect::OnsetEnvelope envelope(sampleRate);
    const float* const channels[1]{mono};
    for (std::size_t done = 0; done < frames;) {
        const int count = static_cast<int>(std::min<std::size_t>(frames - done, 65536));
        const float* const part[1]{channels[0] + done};
        envelope.push(part, 1, count); done += static_cast<std::size_t>(count);
    }
    return tempodetect::estimateFromOnsets(envelope.onsets(), envelope.hopRate(), cancel);
}

struct SampleTempoResult { std::optional<TempoEstimate> tempo; juce::String message; };

// Worker-only. Reads at most the first minute of the file and changes nothing.
inline SampleTempoResult detectSampleTempo(const juce::File& file, const std::atomic<bool>& cancel) try {
    if (!file.hasFileExtension("wav;mp3")) return {{}, "Choose a WAV or MP3 sample."};
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate)
        || reader->sampleRate < 8000 || reader->sampleRate > 192000 || reader->lengthInSamples <= 0)
        return {{}, "Cannot read this audio file."};
    if (static_cast<double>(reader->lengthInSamples) < tempodetect::minSeconds * reader->sampleRate)
        return {{}, "This sample is too short to find a tempo. Source BPM not changed."};
    const auto total = std::min<std::int64_t>(reader->lengthInSamples, static_cast<std::int64_t>(tempodetect::maxSeconds * reader->sampleRate));
    const int channels = static_cast<int>(reader->numChannels);
    juce::AudioBuffer<float> block(channels, 32768);
    tempodetect::OnsetEnvelope envelope(reader->sampleRate);
    for (std::int64_t start = 0; start < total;) {
        if (cancel.load(std::memory_order_relaxed)) return {{}, "Tempo detection stopped."};
        const int count = static_cast<int>(std::min<std::int64_t>(total - start, block.getNumSamples()));
        if (!reader->read(block.getArrayOfWritePointers(), channels, start, count)) return {{}, "Cannot read this audio file."};
        envelope.push(block.getArrayOfReadPointers(), channels, count); start += count;
    }
    auto tempo = tempodetect::estimateFromOnsets(envelope.onsets(), envelope.hopRate(), cancel);
    if (cancel.load(std::memory_order_relaxed)) return {{}, "Tempo detection stopped."};
    if (!tempo) return {{}, "No steady tempo found. Source BPM not changed."};
    return {tempo, {}};
} catch (const std::bad_alloc&) { return {{}, "Not enough memory to detect the tempo."}; }
} // namespace alien
