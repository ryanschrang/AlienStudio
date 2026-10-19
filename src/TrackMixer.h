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
    bool valid() const noexcept {
        return preset >= 0 && preset < presetCount && std::isfinite(gain) && gain >= 0 && gain <= 2
            && std::isfinite(pan) && pan >= -1 && pan <= 1;
    }
};

static_assert(std::atomic<float>::is_always_lock_free, "Track audio controls require lock-free floats");
static_assert(std::atomic<int>::is_always_lock_free, "Track audio controls require lock-free integers");

struct TrackControls {
    std::atomic<int> preset{0};
    std::atomic<float> gain{1.0f}, pan{0.0f}, peak{0.0f};
    std::atomic<bool> mute{false}, solo{false};
    TrackSettings snapshot() const noexcept {
        return {preset.load(), gain.load(), pan.load(), mute.load(), solo.load()};
    }
    void set(const TrackSettings& s) noexcept {
        preset.store(s.preset); gain.store(s.gain); pan.store(s.pan);
        mute.store(s.mute); solo.store(s.solo);
    }
};

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
