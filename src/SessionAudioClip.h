#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include "PianoRollSequence.h"

namespace alien {

struct AudioWaveformPeak {
    float leftMin{}, leftMax{}, rightMin{}, rightMax{};
};

// Fully decoded, immutable after publication. Construction and waveform
// analysis happen on a worker thread; the audio callback only reads samples.
struct SessionAudioClip {
    static constexpr std::size_t maxDecodedBytes = 128u * 1024u * 1024u;
    static constexpr std::size_t waveformPeakCount = 1024;

    double sampleRate{};
    int channels{};
    std::size_t frames{};
    std::vector<float> left, right;
    std::array<AudioWaveformPeak, waveformPeakCount> waveform{};
    std::size_t waveformCount{};

    bool valid() const noexcept {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 192000.0
            || channels < 1 || channels > 2 || frames == 0
            || frames > maxDecodedBytes / (static_cast<std::size_t>(channels) * sizeof(float))
            || left.size() != frames || (channels == 2 && right.size() != frames)
            || (channels == 1 && !right.empty()) || waveformCount == 0
            || waveformCount > waveform.size()) return false;
        for (std::size_t i = 0; i < frames; ++i)
            if (!std::isfinite(left[i]) || (channels == 2 && !std::isfinite(right[i]))) return false;
        return true;
    }

    void buildWaveform() noexcept {
        waveformCount = frames == 0 ? 0 : std::min(waveform.size(), frames);
        for (std::size_t bucket = 0; bucket < waveformCount; ++bucket) {
            const auto begin = bucket * frames / waveformCount;
            const auto end = std::max(begin + 1, (bucket + 1) * frames / waveformCount);
            AudioWaveformPeak peak{1.0f, -1.0f, 1.0f, -1.0f};
            for (std::size_t i = begin; i < end; ++i) {
                const float l = left[i];
                const float r = channels == 2 ? right[i] : l;
                peak.leftMin = std::min(peak.leftMin, l);
                peak.leftMax = std::max(peak.leftMax, l);
                peak.rightMin = std::min(peak.rightMin, r);
                peak.rightMax = std::max(peak.rightMax, r);
            }
            waveform[bucket] = peak;
        }
    }
};

struct SessionClipLaunch {
    // A null audio pointer selects the MIDI pattern; a non-null pointer selects
    // audio. The pointed-to clip must outlive playback and is never mutated.
    PianoRollPattern pattern{};
    const SessionAudioClip* audio{};
    int clipIndex{-1};

    SessionClipLaunch() = default;
    // Compatibility for audio-engine tests and MIDI-only callers that publish
    // a pattern while keeping the requested clip index in its existing atomic.
    SessionClipLaunch(const PianoRollPattern& midiPattern) : pattern(midiPattern), clipIndex(-2) {}
};
static_assert(std::is_trivially_copyable_v<SessionClipLaunch>);

// One UI producer and one audio consumer. Audio clip pointers are immutable
// and retained by the session owner for at least as long as the engine lives.
class SessionClipMailbox {
public:
    bool publish(const SessionClipLaunch& clip) noexcept {
        if (!clip.pattern.valid() || clip.clipIndex < -2) return false;
        buffers[back] = clip;
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & indexMask;
        return true;
    }
    bool consume(SessionClipLaunch& destination) noexcept {
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return false;
        front = middle.exchange(front, std::memory_order_acq_rel) & indexMask;
        destination = buffers[front];
        return true;
    }

private:
    static constexpr unsigned dirty = 4, indexMask = 3;
    std::array<SessionClipLaunch, 3> buffers{};
    std::atomic<unsigned> middle{1};
    unsigned front{}, back{2};
};

// Audio-thread-owned one-shot sample player. Launch state is changed only at a
// caller-selected quantized boundary. It performs bounded linear interpolation
// and never allocates, locks, decodes or accesses the file system.
class SessionAudioPlayer {
public:
    bool queue(const SessionAudioClip* clip, int quantizationSteps) noexcept {
        pendingClip = clip;
        pendingQuantization = quantizationSteps > 0 ? quantizationSteps : 16;
        pending = true;
        return true;
    }

    bool hasPending() const noexcept { return pending; }
    bool pendingIsAudio() const noexcept { return pendingClip != nullptr; }
    int pendingQuantizationSteps() const noexcept { return pendingQuantization; }

    bool launchPendingNow(double outputRate) noexcept {
        if (!pending) return false;
        activeClip = pendingClip;
        pendingClip = nullptr;
        pending = false;
        cursor = 0.0;
        outputSampleRate = std::isfinite(outputRate) && outputRate > 0 ? outputRate : 44100.0;
        return true;
    }

    std::array<float, 2> nextSample() noexcept {
        if (activeClip == nullptr) return {};
        if (cursor >= static_cast<double>(activeClip->frames)) {
            activeClip = nullptr;
            cursor = 0.0;
            return {};
        }
        const auto first = static_cast<std::size_t>(cursor);
        const auto second = std::min(first + 1, activeClip->frames - 1);
        const float fraction = static_cast<float>(cursor - static_cast<double>(first));
        const float left = interpolate(activeClip->left[first], activeClip->left[second], fraction);
        const float right = activeClip->channels == 2
            ? interpolate(activeClip->right[first], activeClip->right[second], fraction) : left;
        cursor += activeClip->sampleRate / outputSampleRate;
        if (cursor >= static_cast<double>(activeClip->frames)) activeClip = nullptr;
        return {left, right};
    }

    void stop() noexcept {
        activeClip = pendingClip = nullptr;
        pending = false;
        cursor = 0.0;
    }
    bool isPlaying() const noexcept { return activeClip != nullptr; }

private:
    static float interpolate(float a, float b, float amount) noexcept { return a + (b - a) * amount; }
    const SessionAudioClip* activeClip{};
    const SessionAudioClip* pendingClip{};
    double cursor{}, outputSampleRate{44100.0};
    int pendingQuantization{16};
    bool pending{};
};

static_assert(std::atomic<unsigned>::is_always_lock_free);

} // namespace alien
