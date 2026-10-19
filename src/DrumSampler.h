#pragma once
#include "SessionAudioClip.h"
#include <array>
#include <atomic>

namespace alien {
inline constexpr int drumPadCount = 8, drumBaseNote = 36;
struct DrumKitControls {
    std::array<std::atomic<const SessionAudioClip*>, drumPadCount> samples{};
};

// Audio-thread owned. Immutable samples are retained by the session owner.
// One-shot note-offs do not truncate a drum; panic/device reset always stops it.
class DrumSampler {
public:
    void prepare(double rate) noexcept { sampleRate = rate; stop(); }
    void setSample(int pad, const SessionAudioClip* clip) noexcept {
        if (pad < 0 || pad >= drumPadCount || samples[static_cast<std::size_t>(pad)] == clip) return;
        for (auto& voice : voices) if (voice.pad == pad) voice.player.stop();
        samples[static_cast<std::size_t>(pad)] = clip;
    }
    void noteOn(int note, float velocity, int source = 0) noexcept {
        const int pad = note - drumBaseNote;
        if (pad < 0 || pad >= drumPadCount || !std::isfinite(velocity) || velocity <= 0) return;
        const auto* sample = samples[static_cast<std::size_t>(pad)];
        if (sample == nullptr) return;
        // Pads 3 and 4 form a closed/open hat choke pair, within the same source.
        if (pad == 2 || pad == 3)
            for (auto& voice : voices)
                if (voice.source == source && (voice.pad == 2 || voice.pad == 3)) voice.player.stop();
        Voice* chosen = nullptr;
        for (auto& voice : voices) if (!voice.player.isPlaying()) { chosen = &voice; break; }
        if (chosen == nullptr) {
            chosen = &voices[0];
            for (auto& voice : voices) if (voice.age < chosen->age) chosen = &voice;
        }
        chosen->player.stop();
        chosen->pad = pad; chosen->source = source; chosen->age = ++age;
        chosen->velocity = std::clamp(velocity, 0.0f, 1.0f);
        chosen->player.queue(sample, 1); chosen->player.launchPendingNow(sampleRate);
    }
    std::array<float, 2> nextSample() noexcept {
        std::array<float, 2> result{};
        for (auto& voice : voices) {
            const auto sample = voice.player.nextSample();
            result[0] += sample[0] * voice.velocity * 0.25f;
            result[1] += sample[1] * voice.velocity * 0.25f;
        }
        return result;
    }
    void stop(int source = -1) noexcept {
        for (auto& voice : voices) if (source < 0 || source == voice.source) voice.player.stop();
    }
private:
    struct Voice {
        SessionAudioPlayer player;
        int pad{-1}, source{};
        std::uint64_t age{};
        float velocity{};
    };
    std::array<Voice, 16> voices{};
    std::array<const SessionAudioClip*, drumPadCount> samples{};
    std::uint64_t age{};
    double sampleRate{44100};
};
} // namespace alien
