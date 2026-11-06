#pragma once
#include "StockDsp.h"
#include <complex>

namespace alien::dsp {
// Uniform overlap-add FIR convolution. All spectra/history storage is allocated
// in prepare; arbitrary host blocks feed a fixed internal partition clock.
class ConvolutionReverb {
public:
    static constexpr int partition = 512, fftSize = partition * 2, latency = partition;
    static constexpr double responseSeconds = .3;
    struct Settings { double toneHz{10000}, outputDb{}, mix{.25}; };
    using Spectrum = std::array<std::complex<double>, fftSize>;
    void prepare(double sampleRate) {
        ready = false;
        rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 192000.0) : 44100;
        for (int i = 0; i < fftSize; ++i) {
            unsigned x = static_cast<unsigned>(i), reversed = 0;
            for (int bits = fftSize; bits > 1; bits >>= 1) { reversed = (reversed << 1) | (x & 1); x >>= 1; }
            reversal[i] = reversed;
        }
        for (int i = 0; i < partition; ++i) roots[i] = std::polar(1.0, -2 * pi * i / fftSize);
        const int frames = static_cast<int>(std::ceil(rate * responseSeconds));
        partitions = (frames + partition - 1) / partition;
        for (int c = 0; c < 2; ++c) {
            impulse[c].resize(frames); kernels[c].assign(partitions, {}); history[c].assign(partitions, {});
            for (int f = 0; f < frames; ++f) {
                double at = f * 48000.0 / rate;
                const double end = (f + 1) * 48000.0 / rate;
                double area = 0;
                // Integrate canonical coefficient bins: narrow early reflections
                // retain their area even when the target rate skips source frames.
                while (at < end) {
                    const int frame = static_cast<int>(std::floor(at));
                    const double next = std::min(end, static_cast<double>(frame + 1));
                    area += chamberSample(c, frame) * (next - at); at = next;
                }
                impulse[c][f] = area;
                kernels[c][f / partition][f % partition] = impulse[c][f];
            }
            for (auto& spectrum : kernels[c]) fft(spectrum, false);
        }
        for (auto* control : {&tone, &output, &mix}) control->configure(rate, 5);
        ready = true; reset();
    }
    void reset() noexcept {
        for (auto& channel : history) for (auto& spectrum : channel) spectrum.fill({});
        input = {}; wet = {}; overlap = {}; dryDelay = {}; toneState = {}; cursor = 0; head = 0; snapNext = true;
    }
    double tailSeconds() const noexcept { return responseSeconds + .05 + latency / rate; }
    const std::vector<double>& impulseResponse(int channel) const noexcept { return impulse[channel == 1 ? 1 : 0]; }
    void process(float* left, float* right, int count, Settings s) noexcept {
        if (!ready || count <= 0) return;
        const auto bounded = [](double v, double lo, double hi, double def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        s.toneHz = bounded(s.toneHz, 500, std::min(16000.0, rate * .45), std::min(10000.0, rate * .45));
        s.outputDb = bounded(s.outputDb, -12, 12, 0); s.mix = bounded(s.mix, 0, 1, .25);
        if (snapNext) { tone.reset(s.toneHz); output.reset(s.outputDb); mix.reset(s.mix); snapNext = false; }
        tone.setTarget(s.toneHz); output.setTarget(s.outputDb); mix.setTarget(s.mix);
        for (int i = 0; i < count; ++i) {
            const double cutoff = tone.advance(1), coefficient = 1 - std::exp(-2 * pi * cutoff / rate);
            const double amount = mix.advance(1), gain = dbToGain(output.advance(1));
            for (int c = 0; c < 2; ++c) {
                float& sample = c == 0 ? left[i] : right[i];
                const double value = finite(sample) ? sample : 0, dry = dryDelay[c][cursor];
                input[c][cursor] = value; dryDelay[c][cursor] = value;
                toneState[c] += coefficient * (wet[c][cursor] - toneState[c]);
                if (std::abs(toneState[c]) < 1e-20) toneState[c] = 0;
                constexpr double limit = std::numeric_limits<float>::max();
                sample = static_cast<float>(std::clamp(dry + amount * (toneState[c] * gain - dry), -limit, limit));
            }
            if (++cursor == partition) { renderPartition(); cursor = 0; }
        }
    }
private:
    // Original synthetic chamber: sparse early reflections plus decorrelated,
    // exponentially decaying noise. No recorded or third-party impulse assets.
    static double chamberSample(int channel, int frame) noexcept {
        if (frame < 0 || frame >= 14400) return 0;
        std::uint32_t hash = static_cast<std::uint32_t>(frame) + 0x9e3779b9u * static_cast<std::uint32_t>(channel + 1);
        hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15; hash *= 0x846ca68bu; hash ^= hash >> 16;
        const double random = static_cast<double>(hash & 0xffffffu) / 8388607.5 - 1;
        double result = frame >= 240 ? random * .012 * std::exp(-6.9 * frame / 14400.0) : 0;
        constexpr std::array<int, 7> reflections{336, 528, 816, 1488, 2064, 2928, 4272};
        for (int i = 0; i < 7; ++i) if (frame == reflections[i] + channel * (17 + i * 3)) result += .28 * std::pow(.72, i);
        return result;
    }
    void fft(Spectrum& values, bool inverse) const noexcept {
        for (int i = 0; i < fftSize; ++i) if (static_cast<unsigned>(i) < reversal[i]) std::swap(values[i], values[reversal[i]]);
        for (int length = 2; length <= fftSize; length *= 2) {
            const int half = length / 2, stride = fftSize / length;
            for (int at = 0; at < fftSize; at += length) for (int j = 0; j < half; ++j) {
                const auto root = inverse ? std::conj(roots[j * stride]) : roots[j * stride];
                const auto even = values[at + j], odd = values[at + j + half] * root;
                values[at + j] = even + odd; values[at + j + half] = even - odd;
            }
        }
        if (inverse) for (auto& value : values) value /= fftSize;
    }
    void renderPartition() noexcept {
        for (int c = 0; c < 2; ++c) {
            auto& current = history[c][head]; current.fill({});
            for (int i = 0; i < partition; ++i) current[i] = input[c][i];
            fft(current, false); scratch.fill({});
            for (int p = 0; p < partitions; ++p) {
                const auto& past = history[c][(head + partitions - p) % partitions];
                const auto& kernel = kernels[c][p];
                for (int bin = 0; bin < fftSize; ++bin) scratch[bin] += past[bin] * kernel[bin];
            }
            fft(scratch, true);
            for (int i = 0; i < partition; ++i) { wet[c][i] = scratch[i].real() + overlap[c][i]; overlap[c][i] = scratch[i + partition].real(); }
        }
        head = (head + 1) % partitions;
    }
    std::array<std::vector<double>, 2> impulse;
    std::array<std::vector<Spectrum>, 2> kernels, history;
    std::array<std::array<double, partition>, 2> input{}, wet{}, overlap{}, dryDelay{};
    std::array<double, 2> toneState{};
    std::array<unsigned, fftSize> reversal{};
    std::array<std::complex<double>, partition> roots{};
    Spectrum scratch{};
    Smoother tone, output, mix;
    double rate{44100}; int partitions{}, cursor{}, head{};
    bool ready{}, snapNext{true};
};
} // namespace alien::dsp
