#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include "SynthEngine.h"

namespace alien {
struct TrackSettings {
    int preset{};
    float gain{1.0f}, pan{};
    bool mute{}, solo{};
    int instrument{}; // 0 synth, 1 drum sampler
    bool valid() const noexcept {
        return instrument >= 0 && instrument <= 1 && preset >= 0 && preset < presetCount && std::isfinite(gain) && gain >= 0 && gain <= 2
            && std::isfinite(pan) && pan >= -1 && pan <= 1;
    }
};

static_assert(std::atomic<float>::is_always_lock_free, "Track audio controls require lock-free floats");
static_assert(std::atomic<int>::is_always_lock_free, "Track audio controls require lock-free integers");

struct TrackControls {
    std::atomic<int> preset{0}, instrument{0};
    std::atomic<float> gain{1.0f}, pan{0.0f}, peak{0.0f};
    std::atomic<bool> mute{false}, solo{false};
    TrackSettings snapshot() const noexcept {
        return {preset.load(), gain.load(), pan.load(), mute.load(), solo.load(), instrument.load()};
    }
    void set(const TrackSettings& s) noexcept {
        preset.store(s.preset); gain.store(s.gain); pan.store(s.pan);
        mute.store(s.mute); solo.store(s.solo); instrument.store(s.instrument);
    }
};

// Whole-mix level, applied after the master FX chain. 0..4 is silence to +12 dB;
// the headroom above the track faders (+6 dB) lets a quiet mix be brought up
// without touching every track.
inline constexpr float maxMasterGain = 4.0f;
inline bool validMasterGain(float gain) noexcept { return std::isfinite(gain) && gain >= 0.0f && gain <= maxMasterGain; }
inline float sanitizedMasterGain(float gain) noexcept { return std::isfinite(gain) ? std::clamp(gain, 0.0f, maxMasterGain) : 0.0f; }

struct MasterControls {
    std::atomic<float> gain{1.0f};
    // Output level before the soft clipper, so values above 1.0 show that it is working.
    std::atomic<float> peak{0.0f};
};

// Final output stage. Untouched below -2 dBFS (0.8); above that it bends smoothly
// toward full scale instead of hard-clipping, so a hot mix distorts gently and
// the output can never exceed +/-1. The curve is continuous with slope 1 at the knee.
inline float softLimit(float x) noexcept {
    constexpr float knee = 0.8f;
    const float magnitude = std::abs(x);
    if (magnitude <= knee) return x;
    const float shaped = std::min(1.0f, knee + (1.0f - knee) * std::tanh((magnitude - knee) / (1.0f - knee)));
    return x < 0.0f ? -shaped : shaped;
}

// Audio-thread-owned balance and gain ramp. Centre preserves the existing level.
// Stereo clips use balance (no channel crossfeed); mono synths use the same law.
class TrackMixer {
public:
    void prepare(double sampleRate) noexcept {
        coefficient = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * 0.005)));
        left = right = 1.0f;
    }
    void setTarget(const TrackSettings& s, bool anySolo) noexcept {
        const float gain = std::isfinite(s.gain) ? std::clamp(s.gain, 0.0f, 2.0f) : 0.0f;
        const float pan = std::isfinite(s.pan) ? std::clamp(s.pan, -1.0f, 1.0f) : 0.0f;
        const float level = s.mute || (anySolo && !s.solo) ? 0.0f : gain;
        targetLeft = level * (pan > 0 ? 1.0f - pan : 1.0f);
        targetRight = level * (pan < 0 ? 1.0f + pan : 1.0f);
    }
    // Automation already supplies the sample's exact interpolated value. Do not
    // pass it through the manual fader's 5 ms smoothing ramp.
    void processAutomated(float& l, float& r, const TrackSettings& settings, bool anySolo) noexcept {
        setTarget(settings, anySolo);
        left = targetLeft; right = targetRight;
        l *= left; r *= right;
    }
    void process(float& l, float& r) noexcept {
        left += coefficient * (targetLeft - left);
        right += coefficient * (targetRight - right);
        if (std::abs(left - targetLeft) < 1.0e-7f) left = targetLeft;
        if (std::abs(right - targetRight) < 1.0e-7f) right = targetRight;
        l *= left; r *= right;
    }
private:
    float left{1}, right{1}, targetLeft{1}, targetRight{1}, coefficient{1};
};
} // namespace alien
