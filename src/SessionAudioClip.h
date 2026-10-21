#pragma once

#include "DiskAudioStream.h"
#include "PianoRollSequence.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace alien {

struct AudioWaveformPeak {
    float leftMin{}, leftMax{}, rightMin{}, rightMax{};
};

// Immutable after publication. Short clips keep decoded samples; long Session
// clips retain file metadata and are read by DiskAudioStreamService.
struct SessionAudioClip {
    static constexpr std::size_t maxDecodedBytes = 128u * 1024u * 1024u;
    static constexpr std::size_t waveformPeakCount = 1024;

    double sampleRate{};
    int channels{};
    std::size_t frames{};
    std::vector<float> left, right;
    std::array<AudioWaveformPeak, waveformPeakCount> waveform{};
    std::size_t waveformCount{};
    std::shared_ptr<const DiskAudioFile> streamSource;

    bool streamed() const noexcept { return streamSource != nullptr; }
    std::size_t decodedBytes() const noexcept {
        return (left.size() + right.size()) * sizeof(float);
    }
    bool valid() const noexcept {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 192000.0
            || channels < 1 || channels > 2 || frames == 0) return false;
        if (streamed())
            return streamSource->valid() && streamSource->sampleRate == sampleRate
                && streamSource->channels == channels
                && streamSource->frames == static_cast<std::int64_t>(frames)
                && left.empty() && right.empty();
        if (frames > maxDecodedBytes / (static_cast<std::size_t>(channels) * sizeof(float))
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
    PianoRollPattern pattern{};
    const SessionAudioClip* audio{};
    int clipIndex{-1};
    SessionClipLaunch() = default;
    SessionClipLaunch(const PianoRollPattern& midiPattern) : pattern(midiPattern), clipIndex(-2) {}
};
static_assert(std::is_trivially_copyable_v<SessionClipLaunch>);

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

// Audio-thread-owned player with separate active and prebuffer voices. A late
// stream waits for the next quantized boundary while the current clip continues.
class SessionAudioPlayer {
public:
    void bind(DiskAudioStreamService::Voice* first, DiskAudioStreamService::Voice* second) noexcept {
        activeVoice = first; pendingVoice = second;
    }
    bool queue(const SessionAudioClip* clip, int quantizationSteps, double startFrame = 0.0) noexcept {
        pendingClip = clip;
        failure.store(false, std::memory_order_relaxed);
        pendingQuantization = quantizationSteps > 0 ? quantizationSteps : 16;
        pendingStartFrame = std::isfinite(startFrame) && startFrame > 0.0 ? startFrame : 0.0;
        pendingGeneration = 0;
        launchRequested = false;
        if (clip && clip->streamed()) {
            if (!pendingVoice) return false;
            pendingGeneration = pendingVoice->request(clip->streamSource.get(),
                static_cast<std::int64_t>(pendingStartFrame));
        }
        pending = true;
        return true;
    }
    bool hasPending() const noexcept { return pending; }
    bool pendingIsAudio() const noexcept { return pendingClip != nullptr; }
    int pendingQuantizationSteps() const noexcept { return pendingQuantization; }
    bool pendingReady() const noexcept {
        return pending && (!pendingClip || !pendingClip->streamed()
            || (pendingVoice && pendingVoice->ready(pendingGeneration)));
    }
    bool pendingFailed() const noexcept {
        return pending && pendingClip && pendingClip->streamed()
            && (!pendingVoice || pendingVoice->failed(pendingGeneration));
    }

    // Requests a launch at the caller-selected boundary. Streamed clips can
    // finish prebuffering after that boundary; nextSample starts them as soon as
    // the requested range is ready without blocking the audio thread.
    bool launchPendingNow(double outputRate, double startFrame = 0.0, int fadeInSamples = 0) noexcept {
        if (!pending) return false;
        const double requestedStart = std::isfinite(startFrame) && startFrame > 0.0 ? startFrame : 0.0;
        pendingOutputRate = std::isfinite(outputRate) && outputRate > 0 ? outputRate : 44100.0;
        pendingFadeSamples = std::max(0, fadeInSamples);
        launchRequested = true;
        if (pendingClip && pendingClip->streamed() && std::abs(requestedStart - pendingStartFrame) > 0.5) {
            if (!pendingVoice) return false;
            pendingStartFrame = requestedStart;
            pendingGeneration = pendingVoice->request(pendingClip->streamSource.get(),
                static_cast<std::int64_t>(pendingStartFrame));
        } else {
            pendingStartFrame = requestedStart;
        }
        if (pendingFailed()) {
            failure.store(true, std::memory_order_relaxed);
            pendingClip = nullptr; pending = false; pendingGeneration = 0; launchRequested = false;
            return false;
        }
        if (!pendingReady()) return false;
        activeClip = pendingClip;
        pendingClip = nullptr;
        pending = false;
        cursor = pendingStartFrame;
        outputSampleRate = pendingOutputRate;
        fadeInTotal = cursor > 0.5 ? pendingFadeSamples : 0;
        fadeInDone = 0; releaseTotal = releaseLeft = 0;
        if (activeClip && activeClip->streamed()) {
            std::swap(activeVoice, pendingVoice);
            activeGeneration = pendingGeneration;
        } else {
            activeGeneration = 0;
        }
        pendingGeneration = 0;
        launchRequested = false;
        if (activeClip && cursor >= static_cast<double>(activeClip->frames)) clearActive();
        return activeClip != nullptr;
    }

    std::array<float, 2> nextSample() noexcept {
        if (launchRequested && pending)
            launchPendingNow(pendingOutputRate, pendingStartFrame, pendingFadeSamples);
        if (!activeClip) return {};
        if (cursor >= static_cast<double>(activeClip->frames)) { clearActive(); return {}; }
        const auto first = static_cast<std::int64_t>(cursor);
        const auto second = std::min(first + 1, static_cast<std::int64_t>(activeClip->frames - 1));
        const float fraction = static_cast<float>(cursor - static_cast<double>(first));
        float left{}, right{};
        if (activeClip->streamed()) {
            if (!activeVoice || !activeVoice->sample(activeGeneration, first, second, fraction, left, right)) {
                underruns.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
            activeVoice->releaseBefore(activeGeneration, first);
        } else {
            left = interpolate(activeClip->left[static_cast<std::size_t>(first)],
                activeClip->left[static_cast<std::size_t>(second)], fraction);
            right = activeClip->channels == 2
                ? interpolate(activeClip->right[static_cast<std::size_t>(first)],
                    activeClip->right[static_cast<std::size_t>(second)], fraction) : left;
        }
        float gain = 1.0f;
        if (fadeInDone < fadeInTotal)
            gain = static_cast<float>(++fadeInDone) / static_cast<float>(fadeInTotal);
        bool finished = false;
        if (releaseTotal > 0) {
            gain *= static_cast<float>(releaseLeft) / static_cast<float>(releaseTotal);
            finished = --releaseLeft <= 0;
        }
        cursor += activeClip->sampleRate / outputSampleRate;
        if (finished || cursor >= static_cast<double>(activeClip->frames)) clearActive();
        return {left * gain, right * gain};
    }

    void release(int fadeSamples) noexcept {
        if (!activeClip) return;
        if (fadeSamples <= 0) { stop(); return; }
        if (releaseTotal == 0) releaseTotal = releaseLeft = fadeSamples;
    }

    void stop() noexcept {
        if (activeVoice) activeVoice->request(nullptr);
        if (pendingVoice) pendingVoice->request(nullptr);
        activeClip = pendingClip = nullptr;
        pending = false; launchRequested = false; cursor = pendingStartFrame = 0.0;
        activeGeneration = pendingGeneration = 0;
        fadeInTotal = fadeInDone = releaseTotal = releaseLeft = pendingFadeSamples = 0;
    }
    bool isPlaying() const noexcept { return activeClip != nullptr; }
    std::uint64_t underrunCount() const noexcept { return underruns.load(std::memory_order_relaxed); }
    bool streamFailed() const noexcept { return failure.load(std::memory_order_relaxed); }

private:
    void clearActive() noexcept { activeClip = nullptr; cursor = 0.0; activeGeneration = 0; }
    static float interpolate(float a, float b, float amount) noexcept { return a + (b - a) * amount; }
    const SessionAudioClip* activeClip{};
    const SessionAudioClip* pendingClip{};
    DiskAudioStreamService::Voice* activeVoice{};
    DiskAudioStreamService::Voice* pendingVoice{};
    std::uint64_t activeGeneration{}, pendingGeneration{};
    std::atomic<std::uint64_t> underruns{};
    std::atomic<bool> failure{};
    double cursor{}, outputSampleRate{44100.0};
    double pendingStartFrame{}, pendingOutputRate{44100.0};
    int pendingQuantization{16};
    int fadeInTotal{}, fadeInDone{}, releaseTotal{}, releaseLeft{}, pendingFadeSamples{};
    bool pending{}, launchRequested{};
};

static_assert(std::atomic<unsigned>::is_always_lock_free);

} // namespace alien
