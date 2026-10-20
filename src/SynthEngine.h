#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace alien {
enum class Waveform { sine, saw, square };
struct Patch {
    Waveform waveform;
    double attack, decay, sustain, release, cutoff;
};
// Factory sounds. Indices 0-2 are stored in existing projects and must keep
// their sound; add new presets only at the end.
inline constexpr std::array<Patch, 12> presets{{
    // waveform          attack  decay  sustain release cutoff
    {Waveform::saw,      0.01,   0.12,  0.7,    0.18,   8000.0}, // Lead (saw)
    {Waveform::square,   0.005,  0.12,  0.6,    0.08,   900.0},  // Bass (square)
    {Waveform::sine,     0.002,  0.18,  0.0,    0.12,   6000.0}, // Pluck (sine)
    {Waveform::sine,     0.004,  0.30,  0.85,   0.10,   400.0},  // Sub Bass
    {Waveform::saw,      0.002,  0.16,  0.15,   0.08,   700.0},  // Acid Bass
    {Waveform::saw,      0.45,   0.80,  0.75,   1.20,   2200.0}, // Soft Pad
    {Waveform::saw,      0.30,   0.40,  0.85,   0.70,   5200.0}, // Strings
    {Waveform::saw,      0.06,   0.25,  0.70,   0.25,   3200.0}, // Brass
    {Waveform::square,   0.01,   0.10,  0.65,   0.15,   4200.0}, // Square Lead
    {Waveform::square,   0.005,  0.05,  1.00,   0.06,   2600.0}, // Organ
    {Waveform::sine,     0.003,  0.90,  0.25,   0.60,   7000.0}, // Electric Keys
    {Waveform::sine,     0.001,  1.40,  0.00,   1.20,   9000.0}  // Bell
}};
// Display names, index-aligned with presets.
inline constexpr std::array<const char*, presets.size()> presetNames{{
    "Lead (saw)", "Bass (square)", "Pluck (sine)", "Sub Bass", "Acid Bass", "Soft Pad",
    "Strings", "Brass", "Square Lead", "Organ", "Electric Keys", "Bell"
}};
inline constexpr int presetCount = static_cast<int>(presets.size());

class Envelope {
public:
    enum class Stage { idle, attack, decay, sustain, release };
    void start(const Patch& p, double rate) noexcept {
        patch = p; sampleRate = rate; level = 0; remaining = samples(patch.attack); stage = Stage::attack;
    }
    void stop() noexcept {
        if (stage != Stage::idle && stage != Stage::release) {
            stage = Stage::release;
            remaining = samples(patch.release);
            releaseStep = level / static_cast<double>(remaining);
        }
    }
    double next() noexcept {
        switch (stage) {
        case Stage::attack:
            level += 1.0 / static_cast<double>(samples(patch.attack));
            if (--remaining == 0) { level = 1.0; remaining = samples(patch.decay); stage = Stage::decay; }
            break;
        case Stage::decay:
            level -= (1.0 - patch.sustain) / static_cast<double>(samples(patch.decay));
            if (--remaining == 0) {
                level = patch.sustain;
                stage = level == 0.0 ? Stage::idle : Stage::sustain;
            }
            break;
        case Stage::release:
            level -= releaseStep;
            if (--remaining == 0) { level = 0; stage = Stage::idle; }
            break;
        default: break;
        }
        return level;
    }
    Stage state() const noexcept { return stage; }
private:
    std::uint64_t samples(double seconds) const noexcept {
        return static_cast<std::uint64_t>(std::max(1.0, std::ceil(seconds * sampleRate)));
    }
    std::uint64_t remaining{};
    Patch patch{presets[0]};
    double sampleRate{44100}, level{}, releaseStep{};
    Stage stage{Stage::idle};
};

// Audio-thread owned. All storage is fixed; note and render operations never allocate.
class SynthEngine {
public:
    static constexpr std::size_t maxVoices = 16;
    // Each voice is scaled by this before summing. It used to be 1/maxVoices (-24 dB for
    // a single note), which left the built-in synth far quieter than any plugin or WAV;
    // 1/8 keeps a few simultaneous notes under full scale and the master stage handles the rest.
    static constexpr double voiceMixScale = 1.0 / 8.0;
    void prepare(double rate) noexcept {
        sampleRate = rate > 0 && std::isfinite(rate) ? rate : 44100;
        allSoundOff();
    }
    void setPatch(Patch p) noexcept {
        auto seconds = [](double x) { return std::isfinite(x) ? std::clamp(x, 0.0, 10.0) : 0.0; };
        p.attack = seconds(p.attack); p.decay = seconds(p.decay); p.release = seconds(p.release);
        p.sustain = std::isfinite(p.sustain) ? std::clamp(p.sustain, 0.0, 1.0) : 0.0;
        p.cutoff = std::isfinite(p.cutoff) ? std::clamp(p.cutoff, 20.0, sampleRate * 0.45) : 1000;
        patch = p;
    }
    void noteOn(int note, float velocity, int channel = 0) noexcept {
        if (note < 0 || note > 127 || !std::isfinite(velocity)) return;
        if (velocity <= 0) { noteOff(note, channel); return; }
        Voice* chosen = nullptr;
        for (auto& v : voices) if (!v.active()) { chosen = &v; break; }
        // Prefer the oldest released voice, then the oldest held voice.
        if (!chosen) for (auto& v : voices)
            if (v.envelope.state() == Envelope::Stage::release && (!chosen || v.age < chosen->age)) chosen = &v;
        if (!chosen) for (auto& v : voices) if (!chosen || v.age < chosen->age) chosen = &v;
        *chosen = Voice{};
        chosen->note = note; chosen->channel = channel; chosen->age = ++age;
        chosen->velocity = std::clamp(velocity, 0.0f, 1.0f);
        chosen->increment = std::min(0.45, 440.0 * std::exp2((note - 69) / 12.0) / sampleRate);
        chosen->waveform = patch.waveform;
        chosen->filterCoefficient = 1.0 - std::exp(-6.283185307179586 * patch.cutoff / sampleRate);
        chosen->envelope.start(patch, sampleRate);
    }
    void noteOff(int note, int channel = 0) noexcept {
        for (auto& v : voices) if (v.note == note && v.channel == channel) v.envelope.stop();
    }
    void allSoundOff() noexcept { voices = {}; age = 0; }
    void allSoundOff(int channel) noexcept {
        for (auto& v : voices) if (v.channel == channel) v = {};
    }
    std::size_t activeVoices() const noexcept {
        std::size_t n = 0; for (const auto& v : voices) if (v.active()) ++n; return n;
    }
    bool isNoteActive(int note) const noexcept {
        for (const auto& v : voices) if (v.active() && v.note == note) return true;
        return false;
    }
    float nextSample() noexcept {
        double sum = 0;
        for (auto& v : voices) {
            if (!v.active()) continue;
            double value = std::sin(6.283185307179586 * v.phase);
            if (v.waveform == Waveform::saw) value = 2 * v.phase - 1 - blep(v.phase, v.increment);
            if (v.waveform == Waveform::square) {
                value = (v.phase < 0.5 ? 1.0 : -1.0) + blep(v.phase, v.increment)
                    - blep(std::fmod(v.phase + 0.5, 1.0), v.increment);
            }
            v.phase += v.increment; if (v.phase >= 1) v.phase -= 1;
            v.filtered += v.filterCoefficient * (value - v.filtered);
            // Avoid denormal tails without changing global floating point state.
            if (std::abs(v.filtered) < 1e-20) v.filtered = 0;
            sum += v.filtered * v.envelope.next() * v.velocity;
        }
        return static_cast<float>(sum * voiceMixScale);
    }
private:
    static double blep(double t, double dt) noexcept {
        if (t < dt) { t /= dt; return t + t - t * t - 1; }
        if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
        return 0;
    }
    struct Voice {
        int note{-1}, channel{}; std::uint64_t age{};
        double phase{}, increment{}, filtered{}, filterCoefficient{};
        float velocity{}; Waveform waveform{Waveform::sine}; Envelope envelope;
        bool active() const noexcept { return envelope.state() != Envelope::Stage::idle; }
    };
    std::array<Voice, maxVoices> voices{};
    double sampleRate{44100}; std::uint64_t age{}; Patch patch{presets[0]};
};
}
