#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include "SessionCapacity.h"
#include "SynthEngine.h"

namespace alien {
// Audio input channels a track can listen to, counted among the device's enabled inputs.
inline constexpr int maxTrackInputs = 32;
// When a track plays what comes in on its input: while it is armed, always, or never.
enum class InputMonitor { automatic = 0, always = 1, off = 2 };

struct TrackSettings {
    int preset{};
    float gain{1.0f}, pan{};
    bool mute{}, solo{};
    int instrument{}; // 0 synth, 1 drum sampler
    // Recording: armed for the next take; its input (one channel, or that one and the next); input monitoring.
    bool armed{};
    int input{};
    bool stereoInput{};
    int monitor{static_cast<int>(InputMonitor::automatic)};
    // How much goes to each return track, 0 (off) to 1 (0 dB), and whether it is taken before the fader.
    // A pre-fader send ignores the fader and pan but still follows mute and solo.
    std::array<float, maxReturns> send{};
    std::array<bool, maxReturns> sendPre{};
    bool valid() const noexcept {
        for (const float level : send) if (!std::isfinite(level) || level < 0 || level > 1) return false;
        return instrument >= 0 && instrument <= 1 && preset >= 0 && preset < presetCount && std::isfinite(gain) && gain >= 0 && gain <= 2
            && std::isfinite(pan) && pan >= -1 && pan <= 1
            && input >= 0 && input + (stereoInput ? 1 : 0) < maxTrackInputs && monitor >= 0 && monitor <= 2;
    }
    // Whether the track plays its input now.
    bool monitoring() const noexcept {
        return monitor == static_cast<int>(InputMonitor::always) || (monitor == static_cast<int>(InputMonitor::automatic) && armed);
    }
};

static_assert(std::atomic<float>::is_always_lock_free, "Track audio controls require lock-free floats");
static_assert(std::atomic<int>::is_always_lock_free, "Track audio controls require lock-free integers");

struct TrackControls {
    std::atomic<int> preset{0}, instrument{0}, input{0}, monitor{0};
    std::atomic<float> gain{1.0f}, pan{0.0f}, peak{0.0f};
    std::atomic<float> inputPeak{0.0f};   // loudest input sample since the GUI last read it (armed or monitoring tracks)
    std::atomic<bool> mute{false}, solo{false}, armed{false}, stereoInput{false};
    std::array<std::atomic<float>, maxReturns> send{};
    std::array<std::atomic<bool>, maxReturns> sendPre{};
    TrackSettings snapshot() const noexcept {
        TrackSettings s{preset.load(), gain.load(), pan.load(), mute.load(), solo.load(), instrument.load(),
                        armed.load(), input.load(), stereoInput.load(), monitor.load()};
        for (std::size_t r = 0; r < s.send.size(); ++r) { s.send[r] = send[r].load(); s.sendPre[r] = sendPre[r].load(); }
        return s;
    }
    void set(const TrackSettings& s) noexcept {
        preset.store(s.preset); gain.store(s.gain); pan.store(s.pan);
        mute.store(s.mute); solo.store(s.solo); instrument.store(s.instrument);
        armed.store(s.armed); input.store(s.input); stereoInput.store(s.stereoInput); monitor.store(s.monitor);
        for (std::size_t r = 0; r < s.send.size(); ++r) { send[r].store(s.send[r]); sendPre[r].store(s.sendPre[r]); }
    }
};

// A return track's own strip: level (0..2, like a track fader), pan and mute. Track solo never
// silences a return, so a soloed track is still heard through its sends.
struct ReturnSettings {
    float gain{1.0f}, pan{};
    bool mute{};
    bool operator==(const ReturnSettings&) const = default;
    bool valid() const noexcept { return std::isfinite(gain) && gain >= 0 && gain <= 2 && std::isfinite(pan) && pan >= -1 && pan <= 1; }
    // The same balance law and fader range as a track, so the same mixer runs it.
    TrackSettings asTrack() const noexcept { TrackSettings t; t.gain = gain; t.pan = pan; t.mute = mute; return t; }
};

struct ReturnControls {
    std::atomic<float> gain{1.0f}, pan{0.0f}, peak{0.0f};
    std::atomic<bool> mute{false};
    ReturnSettings snapshot() const noexcept { return {gain.load(), pan.load(), mute.load()}; }
    void set(const ReturnSettings& s) noexcept { gain.store(s.gain); pan.store(s.pan); mute.store(s.mute); }
};

// One send's level, glided over 5 ms like the fader, so turning a send knob (or muting the
// track) never clicks. Audio thread only.
class SendRamp {
public:
    void prepare(double sampleRate) noexcept {
        coefficient = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * 0.005)));
        level = target;
    }
    void setTarget(float value) noexcept { target = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; }
    float next() noexcept {
        level += coefficient * (target - level);
        if (std::abs(level - target) < 1.0e-7f) level = target;
        return level;
    }
    // Off and finished gliding: next() would return 0 until the target changes.
    bool idle() const noexcept { return level == 0.0f && target == 0.0f; }
private:
    float level{}, target{}, coefficient{1};
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
