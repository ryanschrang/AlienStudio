#pragma once
// DSP kernels for Alien Studio's stock effects. Header-only and free of JUCE so
// the maths can be tested exactly. Nothing here allocates after prepare(); every
// process() call is real-time safe (no locks, no I/O, no heap use).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace alien::dsp {

inline constexpr double pi = 3.14159265358979323846;

inline double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) noexcept { return 20.0 * std::log10(std::max(gain, 1.0e-9)); }
inline bool finite(float x) noexcept { return x - x == 0.0f; } // false for NaN and +/-infinity

// One-pole smoother for control values, advanced a block at a time. After a
// block it snaps to the target when it is closer than any audible step, so a
// settled control is exactly its target (no denormal tail, exact pass-through).
class Smoother {
public:
    void configure(double sampleRate, double milliseconds) noexcept {
        perSample = 1.0 - std::exp(-1.0 / (std::max(milliseconds, 0.01) * 0.001 * sampleRate));
    }
    void reset(double v) noexcept { current = target = v; }
    void setTarget(double v) noexcept { target = v; }
    double advance(int samples) noexcept {
        current = target + (current - target) * std::pow(1.0 - perSample, samples);
        if (std::abs(current - target) < 1.0e-9 * (1.0 + std::abs(target))) current = target;
        return current;
    }
    double value() const noexcept { return current; }
    bool settled() const noexcept { return current == target; }
private:
    double perSample{0.01}, current{}, target{};
};

// ---- Equaliser --------------------------------------------------------------

enum class BiquadKind { lowShelf, peak, highShelf };

struct BiquadCoefficients {
    double b0{1}, b1{}, b2{}, a1{}, a2{}; // normalised so a0 == 1
    bool operator==(const BiquadCoefficients&) const = default;
};

// RBJ audio-EQ-cookbook filters. A band with no gain is an exact identity.
inline BiquadCoefficients designBiquad(BiquadKind kind, double frequency, double gainDb, double q, double sampleRate) noexcept {
    if (gainDb == 0.0) return {};
    const double f = std::clamp(frequency, 10.0, sampleRate * 0.45);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * pi * f / sampleRate, cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2.0 * std::max(q, 0.05));
    double b0, b1, b2, a0, a1, a2;
    switch (kind) {
        case BiquadKind::peak:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case BiquadKind::lowShelf: {
            const double r = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + r); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - r);
            a0 = (A + 1) + (A - 1) * cw + r; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - r;
            break;
        }
        default: {
            const double r = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + r); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - r);
            a0 = (A + 1) - (A - 1) * cw + r; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - r;
            break;
        }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Magnitude of the filter at one frequency, in dB. Used by the editor's curve.
inline double biquadMagnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) noexcept {
    const double w = 2.0 * pi * frequency / sampleRate;
    const double cw = std::cos(w), sw = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = c.b0 + c.b1 * cw + c.b2 * c2, ni = -(c.b1 * sw + c.b2 * s2);
    const double dr = 1 + c.a1 * cw + c.a2 * c2, di = -(c.a1 * sw + c.a2 * s2);
    const double num = nr * nr + ni * ni, den = dr * dr + di * di;
    return 10.0 * std::log10(std::max(num, 1.0e-30) / std::max(den, 1.0e-30));
}

struct BiquadState {
    double z1{}, z2{};
    double process(const BiquadCoefficients& c, double x) noexcept { // transposed direct form II
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

// Four bands (low shelf, two peaking bells, high shelf) plus an output trim.
class ParametricEq {
public:
    static constexpr int bandCount = 4, chunk = 32;
    static constexpr double shelfQ = 0.7071067811865476;
    static constexpr std::array<BiquadKind, 4> kinds{BiquadKind::lowShelf, BiquadKind::peak, BiquadKind::peak, BiquadKind::highShelf};
    struct Band { double frequency{1000}, gainDb{0}, q{1}; };
    struct Settings { std::array<Band, 4> band{}; double outputDb{0}; };

    void prepare(double sampleRate) noexcept {
        rate = sampleRate;
        for (auto& s : frequency) s.configure(rate, 25.0);
        for (auto& s : gain) s.configure(rate, 25.0);
        for (auto& s : quality) s.configure(rate, 25.0);
        output.configure(rate, 25.0);
        reset();
    }
    void reset() noexcept { for (auto& c : channels) for (auto& s : c) s = {}; snapNext = true; }

    // Where the response currently stands (smoothed), for drawing and tests.
    BiquadCoefficients coefficients(int band, const Settings& s) const noexcept {
        const auto& b = s.band[static_cast<std::size_t>(band)];
        return designBiquad(kinds[static_cast<std::size_t>(band)], b.frequency, b.gainDb, q(band, b), rate);
    }

    void process(float* left, float* right, int count, const Settings& settings) noexcept {
        if (snapNext) {
            for (int b = 0; b < bandCount; ++b) {
                const auto& band = settings.band[static_cast<std::size_t>(b)];
                frequency[b].reset(std::log(band.frequency)); gain[b].reset(band.gainDb); quality[b].reset(q(b, band));
            }
            output.reset(settings.outputDb);
            redesign(); snapNext = false;
        }
        for (int b = 0; b < bandCount; ++b) {
            const auto& band = settings.band[static_cast<std::size_t>(b)];
            frequency[b].setTarget(std::log(band.frequency)); gain[b].setTarget(band.gainDb); quality[b].setTarget(q(b, band));
        }
        output.setTarget(settings.outputDb);
        for (int done = 0; done < count;) {
            const int length = std::min(chunk, count - done);
            bool moved = false;
            for (int b = 0; b < bandCount; ++b) {
                moved = moved || !frequency[b].settled() || !gain[b].settled() || !quality[b].settled();
                frequency[b].advance(length); gain[b].advance(length); quality[b].advance(length);
            }
            if (moved) redesign();
            const double from = dbToGain(output.value());
            output.advance(length);
            const double to = dbToGain(output.value());
            for (int i = 0; i < length; ++i) {
                const double g = length > 1 ? from + (to - from) * (i + 1) / length : to;
                double x0 = left[done + i], x1 = right[done + i];
                for (int b = 0; b < bandCount; ++b) {
                    if (coeffs[b] == BiquadCoefficients{}) { // flat band: leave the signal untouched, keep no stale state
                        channels[0][static_cast<std::size_t>(b)] = {}; channels[1][static_cast<std::size_t>(b)] = {};
                        continue;
                    }
                    x0 = channels[0][static_cast<std::size_t>(b)].process(coeffs[b], x0);
                    x1 = channels[1][static_cast<std::size_t>(b)].process(coeffs[b], x1);
                }
                left[done + i] = static_cast<float>(x0 * g);
                right[done + i] = static_cast<float>(x1 * g);
            }
            done += length;
        }
    }

private:
    static double q(int band, const Band& b) noexcept { return kinds[static_cast<std::size_t>(band)] == BiquadKind::peak ? b.q : shelfQ; }
    void redesign() noexcept {
        for (int b = 0; b < bandCount; ++b)
            coeffs[b] = designBiquad(kinds[static_cast<std::size_t>(b)], std::exp(frequency[b].value()), gain[b].value(), quality[b].value(), rate);
    }
    double rate{44100};
    std::array<Smoother, 4> frequency, gain, quality;
    Smoother output;
    std::array<BiquadCoefficients, 4> coeffs{};
    std::array<std::array<BiquadState, 4>, 2> channels{};
    bool snapNext{true};
};

// ---- Filter -----------------------------------------------------------------

enum class FilterMode { lowPass, highPass, bandPass, notch };

// RBJ audio-EQ-cookbook low-pass, high-pass, band-pass (0 dB at the centre) and notch.
inline BiquadCoefficients designFilter(FilterMode mode, double frequency, double q, double sampleRate) noexcept {
    const double f = std::clamp(frequency, 10.0, sampleRate * 0.45);
    const double w = 2.0 * pi * f / sampleRate, cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2.0 * std::max(q, 0.05));
    double b0, b1, b2;
    switch (mode) {
        case FilterMode::lowPass: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; break;
        case FilterMode::highPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; break;
        case FilterMode::bandPass: b0 = alpha; b1 = 0; b2 = -alpha; break;
        default: b0 = 1; b1 = -2 * cw; b2 = 1; break;
    }
    const double a0 = 1 + alpha;
    return {b0 / a0, b1 / a0, b2 / a0, -2 * cw / a0, (1 - alpha) / a0};
}

// Resonant multimode filter: 12 dB/oct (one biquad) or 24 dB/oct (two in series), with a
// dry/wet mix and an output trim. At 24 dB/oct the first stage keeps the fourth-order
// Butterworth Q and the second carries the resonance, so the default resonance (0.707)
// is an exact Butterworth response at either slope. Cutoff and resonance glide; a mode
// or slope change fades the filtered signal out over 3 ms, switches with cleared state
// and fades back in, so switching never clicks.
class MultimodeFilter {
public:
    static constexpr int chunk = 32;
    static constexpr double butterworthQ = 0.7071067811865476, switchMs = 3.0;
    static constexpr std::array<double, 2> fourthOrderQ{0.5411961001461970, 1.3065629648763766};
    struct Settings {
        FilterMode mode{FilterMode::lowPass};
        bool steep{false};
        double frequency{1000}, resonance{butterworthQ}, mix{1}, outputDb{0};
    };

    // The Q of each stage for a resonance setting.
    static std::array<double, 2> stageQ(bool steep, double resonance) noexcept {
        const double r = std::max(resonance, 0.05);
        if (!steep) return {r, r};
        return {fourthOrderQ[0], fourthOrderQ[1] * r / butterworthQ};
    }
    // Response of the filtered path (before mix and output) at one frequency, in dB.
    static double responseDb(const Settings& s, double hz, double sampleRate) noexcept {
        const auto q = stageQ(s.steep, s.resonance);
        double db = biquadMagnitudeDb(designFilter(s.mode, s.frequency, q[0], sampleRate), hz, sampleRate);
        if (s.steep) db += biquadMagnitudeDb(designFilter(s.mode, s.frequency, q[1], sampleRate), hz, sampleRate);
        return db;
    }

    void prepare(double sampleRate) noexcept {
        rate = sampleRate;
        frequency.configure(rate, 25.0); resonance.configure(rate, 25.0); mix.configure(rate, 25.0); output.configure(rate, 25.0);
        fadeStep = 1.0 / std::max(1.0, switchMs * 0.001 * rate);
        reset();
    }
    void reset() noexcept { clearState(); snapNext = true; }
    // The mode and slope currently heard (they change only after the switching fade).
    FilterMode activeMode() const noexcept { return mode; }
    bool activeSteep() const noexcept { return steep; }

    void process(float* left, float* right, int count, const Settings& settings) noexcept {
        if (snapNext) {
            frequency.reset(std::log(clampedFrequency(settings.frequency))); resonance.reset(clampedResonance(settings.resonance));
            mix.reset(std::clamp(settings.mix, 0.0, 1.0)); output.reset(settings.outputDb);
            mode = settings.mode; steep = settings.steep; fade = 1.0;
            redesign(); snapNext = false;
        }
        frequency.setTarget(std::log(clampedFrequency(settings.frequency))); resonance.setTarget(clampedResonance(settings.resonance));
        mix.setTarget(std::clamp(settings.mix, 0.0, 1.0)); output.setTarget(settings.outputDb);
        for (int done = 0; done < count;) {
            const int length = std::min(chunk, count - done);
            const bool moved = !frequency.settled() || !resonance.settled();
            frequency.advance(length); resonance.advance(length);
            if (moved) redesign();
            const double mixFrom = mix.value(), gainFrom = dbToGain(output.value());
            mix.advance(length); output.advance(length);
            const double mixTo = mix.value(), gainTo = dbToGain(output.value());
            for (int i = 0; i < length; ++i) {
                if (mode != settings.mode || steep != settings.steep) {
                    fade -= fadeStep;
                    if (fade <= 0.0) { fade = 0.0; mode = settings.mode; steep = settings.steep; clearState(); redesign(); }
                } else if (fade < 1.0) {
                    fade = std::min(1.0, fade + fadeStep);
                }
                const double t = length > 1 ? static_cast<double>(i + 1) / length : 1.0;
                const double m = mixFrom + (mixTo - mixFrom) * t, g = gainFrom + (gainTo - gainFrom) * t;
                const double dryLeft = left[done + i], dryRight = right[done + i];
                double wetLeft = state[0][0].process(coeffs[0], dryLeft), wetRight = state[1][0].process(coeffs[0], dryRight);
                if (steep) { wetLeft = state[0][1].process(coeffs[1], wetLeft); wetRight = state[1][1].process(coeffs[1], wetRight); }
                left[done + i] = static_cast<float>((dryLeft * (1.0 - m) + wetLeft * m * fade) * g);
                right[done + i] = static_cast<float>((dryRight * (1.0 - m) + wetRight * m * fade) * g);
            }
            done += length;
        }
    }

private:
    double clampedFrequency(double hz) const noexcept { return std::clamp(hz, 10.0, rate * 0.45); }
    static double clampedResonance(double q) noexcept { return std::clamp(q, 0.1, 20.0); }
    void clearState() noexcept { for (auto& c : state) for (auto& s : c) s = {}; }
    void redesign() noexcept {
        const auto q = stageQ(steep, resonance.value());
        const double hz = std::exp(frequency.value());
        coeffs[0] = designFilter(mode, hz, q[0], rate);
        coeffs[1] = designFilter(mode, hz, q[1], rate);
    }
    double rate{44100}, fadeStep{0.01}, fade{1.0};
    Smoother frequency, resonance, mix, output;
    FilterMode mode{FilterMode::lowPass};
    bool steep{false}, snapNext{true};
    std::array<BiquadCoefficients, 2> coeffs{};
    std::array<std::array<BiquadState, 2>, 2> state{};
};

// How long a resonant filter keeps ringing after its input stops: the envelope of a
// two-pole resonance decays with time constant 2Q / w0, and falls 60 dB in ln(1000) of them.
inline double filterTailSeconds(double frequency, double resonance, bool steep) noexcept {
    const auto q = MultimodeFilter::stageQ(steep, resonance);
    const double w0 = 2.0 * pi * std::max(frequency, 10.0);
    return std::clamp(std::log(1000.0) * 2.0 * std::max(q[0], q[1]) / w0, 0.05, 5.0);
}

// ---- Compressor -------------------------------------------------------------

// Static curve (Giannoulis et al.): how many dB the level is pulled down.
inline double compressorReductionDb(double levelDb, double thresholdDb, double ratio, double kneeDb) noexcept {
    const double over = levelDb - thresholdDb, slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    if (2.0 * over < -kneeDb) return 0.0;                                   // below the knee
    if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb) {                    // inside the knee
        const double x = over + kneeDb * 0.5;
        return slope * x * x / (2.0 * kneeDb);
    }
    return over * slope;                                                     // above the knee
}

// Feed-forward, stereo-linked compressor. Level detection is a peak follower
// (instant attack, 10 ms fall) so steady tones read as their peak; attack and
// release then act on the gain reduction itself, in dB.
class Compressor {
public:
    struct Settings { double thresholdDb{-18}, ratio{4}, attackMs{10}, releaseMs{120}, kneeDb{6}, makeupDb{0}, mix{1}; };
    static constexpr double detectorFallMs = 10.0;

    void prepare(double sampleRate) noexcept {
        rate = sampleRate;
        fall = std::exp(-1.0 / (detectorFallMs * 0.001 * rate));
        reset();
    }
    void reset() noexcept { envelope = 0; reductionDb = 0; lastReduction = 0; }
    // Largest gain reduction applied during the last block, in dB (>= 0).
    double lastBlockReductionDb() const noexcept { return lastReduction; }

    void process(float* left, float* right, int count, const Settings& s) noexcept {
        const double attack = std::exp(-1.0 / (std::max(s.attackMs, 0.05) * 0.001 * rate));
        const double release = std::exp(-1.0 / (std::max(s.releaseMs, 0.5) * 0.001 * rate));
        const double mix = std::clamp(s.mix, 0.0, 1.0);
        double worst = 0;
        for (int i = 0; i < count; ++i) {
            const double l = left[i], r = right[i];
            envelope = std::max(std::max(std::abs(l), std::abs(r)), envelope * fall);
            const double levelDb = envelope > 1.0e-6 ? gainToDb(envelope) : -120.0;
            const double wanted = compressorReductionDb(levelDb, s.thresholdDb, s.ratio, s.kneeDb);
            const double coefficient = wanted > reductionDb ? attack : release;
            reductionDb = coefficient * reductionDb + (1.0 - coefficient) * wanted;
            if (reductionDb < 1.0e-9) reductionDb = 0;
            worst = std::max(worst, reductionDb);
            const double gain = dbToGain(s.makeupDb - reductionDb);
            left[i] = static_cast<float>(l * (1.0 - mix) + l * gain * mix);
            right[i] = static_cast<float>(r * (1.0 - mix) + r * gain * mix);
        }
        lastReduction = worst;
    }

private:
    double rate{44100}, fall{0.99}, envelope{}, reductionDb{}, lastReduction{};
};

// ---- Delay ------------------------------------------------------------------

// Circular buffer with a four-point (Catmull-Rom) fractional read.
class FractionalDelayLine {
public:
    void prepare(int maxSamples) { data.assign(static_cast<std::size_t>(maxSamples) + 4, 0.0f); write = 0; }
    void clear() noexcept { std::fill(data.begin(), data.end(), 0.0f); write = 0; }
    int capacity() const noexcept { return static_cast<int>(data.size()) - 4; }
    void push(float x) noexcept { data[static_cast<std::size_t>(write)] = x; write = write + 1 == size() ? 0 : write + 1; }
    // The signal `delay` samples before the sample about to be pushed: read(1)
    // is the most recent push. Valid for 2 <= delay <= capacity().
    float read(double delay) const noexcept {
        delay = std::clamp(delay, 2.0, static_cast<double>(capacity()));
        const int i = static_cast<int>(delay);
        const double t = delay - i;
        const double p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        const double c1 = 0.5 * (p2 - p0), c2 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3, c3 = 0.5 * (p3 - p0) + 1.5 * (p1 - p2);
        return static_cast<float>(((c3 * t + c2) * t + c1) * t + p1);
    }
private:
    int size() const noexcept { return static_cast<int>(data.size()); }
    double at(int k) const noexcept { int index = write - k; if (index < 0) index += size(); return data[static_cast<std::size_t>(index)]; }
    std::vector<float> data;
    int write{};
};

// Note divisions for tempo sync, as fractions of a beat (quarter note).
inline constexpr std::array<double, 8> delayBeats{0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0};
inline double syncedDelayMs(int division, double bpm) noexcept {
    const double beats = delayBeats[static_cast<std::size_t>(std::clamp(division, 0, static_cast<int>(delayBeats.size()) - 1))];
    return 60000.0 / std::clamp(bpm, 20.0, 400.0) * beats;
}
// Time until the echoes fall 60 dB below the first, capped like the host's tail limit.
inline double delayTailSeconds(double timeMs, double feedback) noexcept {
    const double fb = std::clamp(feedback, 0.0, 0.95), t = std::max(timeMs, 0.0) * 0.001;
    if (fb < 0.01) return t;
    return std::min(30.0, t * (std::ceil(std::log(0.001) / std::log(fb)) + 1.0));
}

class StereoDelay {
public:
    struct Settings { double timeMs{350}, feedback{0.4}, toneHz{12000}, mix{0.3}; bool pingPong{false}; };
    static constexpr double maxSeconds = 4.0, openToneHz = 19999.0;

    void prepare(double sampleRate) {
        rate = sampleRate;
        const int capacity = static_cast<int>(std::ceil(maxSeconds * rate)) + 8;
        lineLeft.prepare(capacity); lineRight.prepare(capacity);
        timeStep = 1.0 - std::exp(-1.0 / (0.04 * rate));
        reset();
    }
    void reset() noexcept { lineLeft.clear(); lineRight.clear(); toneLeft = toneRight = 0; samples = -1; }

    void process(float* left, float* right, int count, const Settings& s) noexcept {
        const double target = std::clamp(s.timeMs * 0.001 * rate, 2.0, static_cast<double>(lineLeft.capacity()));
        if (samples < 0) samples = target;
        const double feedback = std::clamp(s.feedback, 0.0, 0.95), mix = std::clamp(s.mix, 0.0, 1.0);
        const bool open = s.toneHz >= openToneHz;
        const double tone = open ? 1.0 : std::min(1.0, 1.0 - std::exp(-2.0 * pi * s.toneHz / rate));
        for (int i = 0; i < count; ++i) {
            samples += (target - samples) * timeStep;
            if (std::abs(target - samples) < 1.0e-4) samples = target;
            const double wetLeft = lineLeft.read(samples), wetRight = lineRight.read(samples);
            if (open) { toneLeft = wetLeft; toneRight = wetRight; }
            else { toneLeft += tone * (wetLeft - toneLeft); toneRight += tone * (wetRight - toneRight); }
            const double inLeft = left[i], inRight = right[i];
            if (s.pingPong) {
                // The mono sum enters the left line; each line feeds the other.
                lineLeft.push(static_cast<float>(0.5 * (inLeft + inRight) + feedback * toneRight));
                lineRight.push(static_cast<float>(feedback * toneLeft));
            } else {
                lineLeft.push(static_cast<float>(inLeft + feedback * toneLeft));
                lineRight.push(static_cast<float>(inRight + feedback * toneRight));
            }
            left[i] = static_cast<float>(inLeft * (1.0 - mix) + wetLeft * mix);
            right[i] = static_cast<float>(inRight * (1.0 - mix) + wetRight * mix);
        }
    }

private:
    double rate{44100}, samples{-1}, timeStep{0.001}, toneLeft{}, toneRight{};
    FractionalDelayLine lineLeft, lineRight;
};

// ---- Limiter ----------------------------------------------------------------

// Minimum of the most recent `window` values in O(1) amortised time (monotonic
// queue in a preallocated ring).
class SlidingMinimum {
public:
    void prepare(int windowLength) {
        window = std::max(1, windowLength); capacity = window + 1;
        value.assign(static_cast<std::size_t>(capacity), 1.0); index.assign(static_cast<std::size_t>(capacity), 0);
        reset();
    }
    void reset() noexcept { head = tail = count = 0; counter = 0; }
    double push(double v) noexcept {
        while (count > 0) {
            const int last = tail == 0 ? capacity - 1 : tail - 1;
            if (value[static_cast<std::size_t>(last)] < v) break;
            tail = last; --count;
        }
        value[static_cast<std::size_t>(tail)] = v; index[static_cast<std::size_t>(tail)] = counter;
        tail = tail + 1 == capacity ? 0 : tail + 1; ++count;
        while (index[static_cast<std::size_t>(head)] <= counter - window) { head = head + 1 == capacity ? 0 : head + 1; --count; }
        ++counter;
        return value[static_cast<std::size_t>(head)];
    }
private:
    std::vector<double> value;
    std::vector<std::int64_t> index;
    int window{1}, capacity{2}, head{}, tail{}, count{};
    std::int64_t counter{};
};

// Look-ahead peak limiter. The gain needed for each upcoming sample is held at
// its minimum over the look-ahead window and then smoothed by a window-long
// average, so the gain is already low when a peak reaches the output: the
// output never exceeds the ceiling (sample peaks; not an inter-sample/true-peak
// limiter). Release is applied afterwards and can only raise the gain slowly.
class Limiter {
public:
    struct Settings { double inputGainDb{0}, ceilingDb{-0.3}, releaseMs{100}; };
    static constexpr double lookaheadMs = 2.0;

    void prepare(double sampleRate) {
        rate = sampleRate;
        window = std::max(2, static_cast<int>(std::lround(lookaheadMs * 0.001 * rate)));
        delayLeft.assign(static_cast<std::size_t>(window), 0.0); delayRight.assign(static_cast<std::size_t>(window), 0.0);
        box.assign(static_cast<std::size_t>(window), 1.0);
        minimum.prepare(window);
        reset();
    }
    int latencySamples() const noexcept { return window - 1; }
    void reset() noexcept {
        std::fill(delayLeft.begin(), delayLeft.end(), 0.0); std::fill(delayRight.begin(), delayRight.end(), 0.0);
        std::fill(box.begin(), box.end(), 1.0);
        boxSum = window; boxPosition = 0; delayPosition = 0; gain = 1.0; lastReduction = 0;
        minimum.reset();
    }
    // Largest gain reduction applied during the last block, in dB (>= 0).
    double lastBlockReductionDb() const noexcept { return lastReduction; }

    void process(float* left, float* right, int count, const Settings& s) noexcept {
        const double drive = dbToGain(s.inputGainDb);
        const double ceiling = dbToGain(std::min(s.ceilingDb, 0.0)) * 0.999999;
        const double release = std::exp(-1.0 / (std::max(s.releaseMs, 1.0) * 0.001 * rate));
        double lowest = 1.0;
        for (int i = 0; i < count; ++i) {
            const double xl = left[i] * drive, xr = right[i] * drive;
            const double peak = std::max(std::abs(xl), std::abs(xr));
            const double needed = peak > ceiling ? ceiling / peak : 1.0;
            const double held = minimum.push(needed);
            boxSum += held - box[static_cast<std::size_t>(boxPosition)];
            box[static_cast<std::size_t>(boxPosition)] = held;
            if (++boxPosition == window) { // recompute exactly once per window so rounding never accumulates
                boxPosition = 0; boxSum = 0; for (double v : box) boxSum += v;
            }
            const double target = std::min(1.0, boxSum / window);
            gain = target < gain ? target : gain + (target - gain) * (1.0 - release);
            delayLeft[static_cast<std::size_t>(delayPosition)] = xl; delayRight[static_cast<std::size_t>(delayPosition)] = xr;
            delayPosition = delayPosition + 1 == window ? 0 : delayPosition + 1; // the oldest entry is `window - 1` samples old
            left[i] = static_cast<float>(delayLeft[static_cast<std::size_t>(delayPosition)] * gain);
            right[i] = static_cast<float>(delayRight[static_cast<std::size_t>(delayPosition)] * gain);
            lowest = std::min(lowest, gain);
        }
        lastReduction = -gainToDb(lowest);
    }

private:
    double rate{44100}, boxSum{}, gain{1}, lastReduction{};
    int window{2}, boxPosition{}, delayPosition{};
    std::vector<double> delayLeft, delayRight, box;
    SlidingMinimum minimum;
};

} // namespace alien::dsp
