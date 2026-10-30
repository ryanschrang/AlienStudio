#pragma once
#include "SynthEngine.h"
#include <atomic>
#include <algorithm>
#include <cmath>

namespace alien {
// Latest-request mailbox: GUI publishes only an index/stop token. The callback
// owns all synth/envelope state. No queue growth, allocation, locks or file I/O.
class FactoryPresetPreview {
public:
    bool request(int preset) noexcept {
        if (preset < 0 || preset >= presetCount) return false;
        pending.store(preset, std::memory_order_release); return true;
    }
    void stop() noexcept { pending.store(stopRequest, std::memory_order_release); }
    void prepare(double rate) noexcept { sampleRate = std::isfinite(rate) && rate > 0 ? std::clamp(rate, 1.0, 1000000.0) : 44100.0; synth.prepare(sampleRate); reset(); }
    void reset() noexcept { pending.store(noRequest); silence(); }
    void beginBlock(bool panic = false) noexcept {
        const int request = pending.exchange(noRequest, std::memory_order_acquire);
        if (panic || request == stopRequest) { silence(); return; }
        if (request >= 0 && request < presetCount) {
            silence(); synth.setPatch(presets[static_cast<std::size_t>(request)]);
            synth.noteOn(60, .65f); hold = static_cast<int>(sampleRate * .8); tail = static_cast<int>(sampleRate * 1.5);
        }
    }
    float nextSample() noexcept {
        if (hold > 0) { const auto value = synth.nextSample(); if (--hold == 0) synth.noteOff(60); return value * .6f; }
        if (tail > 0) { const auto value = synth.nextSample(); if (--tail == 0) silence(); return value * .6f; }
        return 0;
    }
private:
    void silence() noexcept { synth.allSoundOff(); hold = tail = 0; }
    static constexpr int noRequest = -1, stopRequest = -2;
    std::atomic<int> pending{noRequest};
    SynthEngine synth;
    double sampleRate{44100};
    int hold{}, tail{};
};
} // namespace alien
