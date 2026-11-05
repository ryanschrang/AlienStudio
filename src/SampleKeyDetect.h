#pragma once
#include "SampleTempoDetect.h"   // readSampleForAnalysis

// Estimates a sample's key from how much of each pitch class it holds, against the
// Krumhansl-Kessler major and minor key profiles. Worker-only: it allocates and reads
// files. It prefers to find nothing over guessing, so silence, drums, noise and a lone
// tone return no key. Relative keys share their notes, so the caller must say the
// result can be the relative major or minor instead.
namespace alien {
struct KeyEstimate {
    int root{};          // pitch class, 0 is C
    bool minor{};
    float confidence{};
    int relativeRoot() const noexcept { return (root + (minor ? 3 : 9)) % 12; }
};

namespace keydetect {
inline constexpr double analysisRate = 11025, windowSeconds = .37, minSeconds = 2;
inline constexpr int lowestNote = 45, highestNote = 96;   // A2 to C7, as MIDI notes
inline constexpr int minVoicedWindows = 3, minPitchClasses = 3;
inline constexpr float minConfidence = .6f, silentEnergy = 1.0e-6f;

// Builds a 12-note chroma from Hann windows of low-passed mono audio.
class ChromaCollector {
public:
    explicit ChromaCollector(double sampleRate)
        : factor(std::max(1, static_cast<int>(sampleRate / analysisRate))), rate(sampleRate / factor) {
        const auto size = static_cast<std::size_t>(std::max(64, juce::roundToInt(windowSeconds * rate)));
        window.resize(size); samples.reserve(size);
        for (std::size_t n = 0; n < size; ++n)
            window[n] = static_cast<float>(.5 - .5 * std::cos(juce::MathConstants<double>::twoPi * static_cast<double>(n) / static_cast<double>(size)));
        for (int note = lowestNote; note <= highestNote; ++note) {
            const double frequency = 440 * std::pow(2.0, (note - 69) / 12.0);
            if (frequency >= rate * .45) break;
            notes.push_back({note % 12, static_cast<float>(2 * std::cos(juce::MathConstants<double>::twoPi * frequency / rate))});
        }
    }
    void push(const float* const* channels, int numChannels, int count) {
        for (int n = 0; n < count; ++n) {
            float mono = 0;
            for (int c = 0; c < numChannels; ++c) mono += channels[c][n];
            mono /= static_cast<float>(numChannels);
            if (!std::isfinite(mono)) mono = 0;
            sum += mono;
            if (++summed < factor) continue;
            samples.push_back(sum / static_cast<float>(factor)); sum = 0; summed = 0;
            if (samples.size() == window.size()) { analyse(); samples.clear(); }
        }
    }
    int voicedWindows() const noexcept { return voiced; }
    const std::array<double, 12>& values() const noexcept { return chroma; }
private:
    struct Bin { int pitchClass; float coefficient; };
    void analyse() {
        float energy = 0;
        for (const auto sample : samples) energy += sample * sample;
        if (energy / static_cast<float>(samples.size()) < silentEnergy) return;
        // Each window counts equally, so a loud hit does not outweigh the rest of the sample.
        std::array<double, 12> frame{}; double total = 0;
        for (const auto& bin : notes) {
            float previous = 0, older = 0;
            for (std::size_t n = 0; n < samples.size(); ++n) {
                const float next = samples[n] * window[n] + bin.coefficient * previous - older;
                older = previous; previous = next;
            }
            const double power = double(previous) * previous + double(older) * older - double(bin.coefficient) * previous * older;
            const double magnitude = std::sqrt(std::max(0.0, power));
            frame[static_cast<std::size_t>(bin.pitchClass)] += magnitude; total += magnitude;
        }
        if (total <= 0) return;
        for (std::size_t i = 0; i < chroma.size(); ++i) chroma[i] += frame[i] / total;
        ++voiced;
    }
    int factor; double rate;
    std::vector<float> window, samples;
    std::vector<Bin> notes;
    std::array<double, 12> chroma{};
    float sum{}; int summed{}, voiced{};
};

inline double correlation(const std::array<double, 12>& chroma, const std::array<double, 12>& profile, int root) {
    double meanChroma = 0, meanProfile = 0;
    for (int i = 0; i < 12; ++i) { meanChroma += chroma[static_cast<std::size_t>(i)]; meanProfile += profile[static_cast<std::size_t>(i)]; }
    meanChroma /= 12; meanProfile /= 12;
    double product = 0, chromaPower = 0, profilePower = 0;
    for (int i = 0; i < 12; ++i) {
        const double a = chroma[static_cast<std::size_t>((i + root) % 12)] - meanChroma, b = profile[static_cast<std::size_t>(i)] - meanProfile;
        product += a * b; chromaPower += a * a; profilePower += b * b;
    }
    return chromaPower > 0 && profilePower > 0 ? product / std::sqrt(chromaPower * profilePower) : 0;
}

inline std::optional<KeyEstimate> estimateFromChroma(const ChromaCollector& collected) {
    if (collected.voicedWindows() < minVoicedWindows) return {};
    const auto& chroma = collected.values();
    const auto [low, high] = std::minmax_element(chroma.begin(), chroma.end());
    if (*high <= *low) return {};
    // A lone tone has too few pitch classes for a key to mean anything.
    const auto strong = std::count_if(chroma.begin(), chroma.end(), [low = *low, high = *high](double value) { return value - low >= (high - low) * .25; });
    if (strong < minPitchClasses) return {};
    static constexpr std::array<double, 12> major{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static constexpr std::array<double, 12> minor{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    KeyEstimate best; double bestScore = -2;
    for (int root = 0; root < 12; ++root) {
        if (const auto score = correlation(chroma, major, root); score > bestScore) { bestScore = score; best = {root, false, static_cast<float>(score)}; }
        if (const auto score = correlation(chroma, minor, root); score > bestScore) { bestScore = score; best = {root, true, static_cast<float>(score)}; }
    }
    if (best.confidence < minConfidence) return {};
    return best;
}
} // namespace keydetect

// Mono float samples, for tests and in-memory audio.
inline std::optional<KeyEstimate> estimateKey(const float* mono, std::size_t frames, double sampleRate) {
    if (mono == nullptr || !std::isfinite(sampleRate) || sampleRate < 1000) return {};
    if (static_cast<double>(frames) < keydetect::minSeconds * sampleRate) return {};
    keydetect::ChromaCollector collector(sampleRate);
    for (std::size_t done = 0; done < frames;) {
        const int count = static_cast<int>(std::min<std::size_t>(frames - done, 65536));
        const float* const part[1]{mono + done};
        collector.push(part, 1, count); done += static_cast<std::size_t>(count);
    }
    return keydetect::estimateFromChroma(collector);
}

struct SampleKeyResult { std::optional<KeyEstimate> key; juce::String message; };

// Worker-only. Reads at most the first minute of the file and changes nothing.
inline SampleKeyResult detectSampleKey(const juce::File& file, const std::atomic<bool>& cancel) try {
    std::unique_ptr<keydetect::ChromaCollector> collector;
    const auto problem = readSampleForAnalysis(file, cancel, keydetect::minSeconds, "key",
        [&](double rate) { collector = std::make_unique<keydetect::ChromaCollector>(rate); },
        [&](const float* const* channels, int numChannels, int count) { collector->push(channels, numChannels, count); });
    if (problem.isNotEmpty()) return {{}, problem};
    const auto key = keydetect::estimateFromChroma(*collector);
    if (!key) return {{}, "No clear key found. Source key not changed."};
    return {key, {}};
} catch (const std::bad_alloc&) { return {{}, "Not enough memory to detect the key."}; }
} // namespace alien
