#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace alien {

enum class Waveform { sine, saw, square };
enum class PresetId { lead, bass, pluck };

struct EnvelopeParameters {
    float attack{0.01f}, decay{0.1f}, sustain{0.7f}, release{0.15f};
};

struct Preset {
    Waveform waveform;
    EnvelopeParameters envelope;
    float gain;
    float cutoffHz;
};

inline constexpr Preset presetFor(PresetId id) noexcept {
    switch (id) {
        case PresetId::bass:  return {Waveform::square, {0.004f, 0.12f, 0.65f, 0.08f}, 0.22f, 1400.0f};
        case PresetId::pluck: return {Waveform::sine, {0.002f, 0.18f, 0.0f, 0.08f}, 0.35f, 9000.0f};
        default:             return {Waveform::saw, {0.012f, 0.10f, 0.65f, 0.20f}, 0.20f, 6500.0f};
    }
}

// Owned and accessed only by the audio thread. No dynamic storage is used.
class Envelope {
public:
    enum class Stage { idle, attack, decay, sustain, release };

    void start(double rate, EnvelopeParameters p) noexcept {
        sampleRate = rate;
        parameters = p;
        level = 0.0f;
        begin(Stage::attack, 1.0f, p.attack);
    }

    void noteOff() noexcept {
        if (stage != Stage::idle && stage != Stage::release)
            begin(Stage::release, 0.0f, parameters.release);
    }

    void reset() noexcept { stage = Stage::idle; level = 0.0f; remaining = 0; }
    bool active() const noexcept { return stage != Stage::idle; }
    Stage getStage() const noexcept { return stage; }
    float getLevel() const noexcept { return level; }

    float next() noexcept {
        // At most attack -> decay -> sustain or release -> idle; bounded even
        // for zero-duration stages. The last ramp sample lands on its target.
        for (int transitions = 0; transitions < 3; ++transitions) {
            if (stage == Stage::idle) return 0.0f;
            if (stage == Stage::sustain) return level;
            if (remaining > 0) {
                level += increment;
                if (--remaining == 0) level = target;
                return std::clamp(level, 0.0f, 1.0f);
            }
            if (stage == Stage::attack) begin(Stage::decay, parameters.sustain, parameters.decay);
            else if (stage == Stage::decay) { stage = Stage::sustain; level = parameters.sustain; }
            else reset();
        }
        return level;
    }

private:
    void begin(Stage nextStage, float nextTarget, float seconds) noexcept {
        stage = nextStage;
        target = nextTarget;
        remaining = static_cast<int>(std::max(0.0, std::round(seconds * sampleRate)));
        increment = remaining > 0 ? (target - level) / static_cast<float>(remaining) : 0.0f;
        if (remaining == 0) level = target;
    }
    EnvelopeParameters parameters;
    Stage stage{Stage::idle};
    double sampleRate{44100.0};
    int remaining{0};
    float level{0.0f}, target{0.0f}, increment{0.0f};
};

class Oscillator {
public:
    void start(double rate, int note, Waveform shape) noexcept {
        waveform = shape;
        phase = 0.0;
        delta = std::min(0.45, 440.0 * std::exp2((note - 69) / 12.0) / rate);
    }

    float next() noexcept {
        double value;
        switch (waveform) {
            case Waveform::saw:
                value = 2.0 * phase - 1.0 - polyBlep(phase);
                break;
            case Waveform::square: {
                double shifted = phase + 0.5;
                if (shifted >= 1.0) shifted -= 1.0;
                value = (phase < 0.5 ? 1.0 : -1.0) + polyBlep(phase) - polyBlep(shifted);
                break;
            }
            default:
                value = std::sin(2.0 * std::numbers::pi * phase);
        }
        phase += delta;
        if (phase >= 1.0) phase -= 1.0;
        return static_cast<float>(value);
    }

private:
    // Polynomial band-limited steps soften saw/square discontinuities.
    double polyBlep(double t) const noexcept {
        if (t < delta) { t /= delta; return t + t - t * t - 1.0; }
        if (t > 1.0 - delta) { t = (t - 1.0) / delta; return t * t + t + t + 1.0; }
        return 0.0;
    }
    Waveform waveform{Waveform::sine};
    double phase{0.0}, delta{0.01};
};

class SynthEngine {
public:
    static constexpr std::size_t maxVoices = 16;

    void prepare(double rate) noexcept {
        sampleRate = std::isfinite(rate) && rate >= 8000.0 ? rate : 44100.0;
        reset();
    }
    void setPreset(PresetId id) noexcept { preset = id; }
    void reset() noexcept {
        for (auto& voice : voices) voice = Voice{};
        serial = 0;
    }

    void noteOn(int channel, int note, float velocity) noexcept {
        if (!valid(channel, note) || !std::isfinite(velocity)) return;
        if (velocity <= 0.0f) { noteOff(channel, note); return; }

        // Retrigger an existing key first. Otherwise free -> oldest released
        // -> oldest held. This makes stealing deterministic and bounded.
        Voice* chosen = nullptr;
        for (auto& v : voices)
            if (v.envelope.active() && v.channel == channel && v.note == note) { chosen = &v; break; }
        if (chosen == nullptr)
            for (auto& v : voices)
                if (!v.envelope.active()) { chosen = &v; break; }
        if (chosen == nullptr) {
            for (auto& v : voices)
                if (v.envelope.getStage() == Envelope::Stage::release && (chosen == nullptr || v.age < chosen->age)) chosen = &v;
        }
        if (chosen == nullptr) {
            chosen = &voices[0];
            for (auto& v : voices) if (v.age < chosen->age) chosen = &v;
        }

        const auto settings = presetFor(preset);
        // A short residual ramp reduces the discontinuity when stealing.
        chosen->residual = chosen->last;
        chosen->residualSamples = 32;
        chosen->note = note;
        chosen->channel = channel;
        chosen->age = ++serial;
        chosen->gain = std::clamp(velocity, 0.0f, 1.0f) * settings.gain;
        chosen->filter = 0.0f;
        const auto cutoff = std::min(static_cast<double>(settings.cutoffHz), sampleRate * 0.45);
        chosen->filterCoefficient = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * cutoff / sampleRate));
        chosen->oscillator.start(sampleRate, note, settings.waveform);
        chosen->envelope.start(sampleRate, settings.envelope);
    }

    void noteOff(int channel, int note) noexcept {
        for (auto& v : voices)
            if (v.envelope.active() && v.channel == channel && v.note == note) v.envelope.noteOff();
    }

    void allNotesOff(int channel = 0, bool immediately = false) noexcept {
        for (auto& v : voices) {
            if (channel == 0 || channel == v.channel) {
                if (immediately) v = Voice{};
                else v.envelope.noteOff();
            }
        }
    }

    // Short MIDI messages only. Called by the audio owner, never a MIDI input
    // callback directly. Hardware delivery and sustain are a later milestone.
    void handleMidi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {
        if (data1 > 127 || data2 > 127) return;
        const int channel = (status & 0x0f) + 1;
        switch (status & 0xf0) {
            case 0x90: noteOn(channel, data1, data2 / 127.0f); break;
            case 0x80: noteOff(channel, data1); break;
            case 0xb0:
                if (data1 == 120) allNotesOff(channel, true);
                else if (data1 == 123) allNotesOff(channel);
                break;
            default: break;
        }
    }

    float nextSample() noexcept {
        float output = 0.0f;
        for (auto& v : voices) {
            if (!v.envelope.active()) { v.last = 0.0f; continue; }
            const float envelope = v.envelope.next();
            v.filter += v.filterCoefficient * (v.oscillator.next() - v.filter);
            // Prevent a long decaying filter state from entering denormal range.
            if (std::abs(v.filter) < 1.0e-20f) v.filter = 0.0f;
            float sample = v.filter * envelope * v.gain;
            if (v.residualSamples > 0)
                sample += v.residual * static_cast<float>(v.residualSamples--) / 32.0f;
            v.last = sample;
            output += sample;
        }
        return std::tanh(output * 0.7f);
    }

    std::size_t activeVoiceCount() const noexcept {
        std::size_t count = 0;
        for (const auto& v : voices) if (v.envelope.active()) ++count;
        return count;
    }
    bool isNoteActive(int channel, int note) const noexcept {
        for (const auto& v : voices)
            if (v.envelope.active() && v.channel == channel && v.note == note) return true;
        return false;
    }

private:
    static bool valid(int channel, int note) noexcept { return channel >= 1 && channel <= 16 && note >= 0 && note <= 127; }
    struct Voice {
        Oscillator oscillator;
        Envelope envelope;
        int note{-1}, channel{0}, residualSamples{0};
        std::uint64_t age{0};
        float gain{0.0f}, filter{0.0f}, filterCoefficient{1.0f}, residual{0.0f}, last{0.0f};
    };
    std::array<Voice, maxVoices> voices{};
    double sampleRate{44100.0};
    std::uint64_t serial{0};
    PresetId preset{PresetId::lead};
};

} // namespace alien
