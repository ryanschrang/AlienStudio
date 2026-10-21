#pragma once
#include "Arrangement.h"
#include "PluginRuntime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace alien {

// Optional worker-thread input used only by offline rendering. The normal audio
// callback leaves this null, so disk I/O can never enter real-time playback.
struct OfflineTrackAudioSource {
    virtual ~OfflineTrackAudioSource() = default;
    virtual void addBlock(std::array<juce::AudioBuffer<float>, maxSessionTracks>& tracks,
                          int samples) = 0;
};

inline std::int64_t arrangementFrameCount(int bars, double bpm, double sampleRate) noexcept {
    if (bars <= 0 || !std::isfinite(bpm) || bpm <= 0
        || !std::isfinite(sampleRate) || sampleRate <= 0) return 0;
    const long double frames = static_cast<long double>(bars) * 4.0L * 60.0L
        * static_cast<long double>(sampleRate) / static_cast<long double>(bpm);
    if (frames >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::ceil(frames - 1.0e-9L));
}

// Owns one reader per streamed arrangement region. Reads happen synchronously
// on the export worker and are addressed from the absolute export frame, so
// output does not depend on prebuffer state, thread scheduling, or block size.
class DeterministicStreamedArrangement final : public OfflineTrackAudioSource {
public:
    static std::unique_ptr<DeterministicStreamedArrangement> create(
        const ArrangementPlayback& song, double bpm, double outputRate,
        std::int64_t fromTick, std::int64_t toTick, float gain,
        const std::atomic<bool>& cancelled, juce::String& error) {
        auto result = std::unique_ptr<DeterministicStreamedArrangement>(
            new DeterministicStreamedArrangement(outputRate, gain, cancelled));
        if (!std::isfinite(bpm) || bpm <= 0.0 || !std::isfinite(outputRate) || outputRate <= 0.0) {
            error = "Invalid export tempo or sample rate.";
            return {};
        }
        const long double framesPerTick = 60.0L * outputRate
            / (static_cast<long double>(timeline::ticksPerBeat) * bpm);
        for (const auto& region : song.timeline.regions) {
            if (region.isPattern() || region.muted) continue;
            const auto* clip = song.clips[static_cast<std::size_t>(region.source)]
                [static_cast<std::size_t>(region.row)].audio;
            if (!clip || !clip->streamed()) continue;
            const auto firstTick = std::max(fromTick, region.start);
            const auto lastTick = std::min(toTick, region.end());
            if (firstTick >= lastTick) continue;
            Voice voice;
            voice.track = region.row;
            voice.scene = region.source;
            voice.start = static_cast<std::int64_t>(std::ceil(
                static_cast<long double>(firstTick - fromTick) * framesPerTick - 1.0e-9L));
            voice.end = static_cast<std::int64_t>(std::ceil(
                static_cast<long double>(lastTick - fromTick) * framesPerTick - 1.0e-9L));
            voice.sourceOffset = (region.audioOffset
                + timeline::secondsForTicks(firstTick - region.start, bpm)) * clip->sampleRate;
            voice.source = clip->streamSource;
            voice.reader = voice.source ? voice.source->open() : nullptr;
            if (!voice.source || !voice.source->valid() || !voice.reader) {
                error = "Cannot open streamed source for track " + juce::String(region.row + 1)
                    + ", scene " + juce::String(region.source + 1)
                    + ". The file may be missing or unreadable.";
                return {};
            }
            result->voices.push_back(std::move(voice));
        }
        error.clear();
        return result;
    }

    void addBlock(std::array<juce::AudioBuffer<float>, maxSessionTracks>& tracks,
                  int samples) override {
        if (failedFlag || samples <= 0) return;
        if (cancelled.load(std::memory_order_relaxed)) {
            cancelledFlag = true;
            return;
        }
        const auto blockEnd = frame + samples;
        for (auto& voice : voices) {
            const auto firstFrame = std::max(frame, voice.start);
            const auto lastFrame = std::min(blockEnd, voice.end);
            if (firstFrame >= lastFrame) continue;
            auto* left = tracks[static_cast<std::size_t>(voice.track)].getWritePointer(0);
            auto* right = tracks[static_cast<std::size_t>(voice.track)].getWritePointer(1);
            for (auto outputFrame = firstFrame; outputFrame < lastFrame; ++outputFrame) {
                const long double sourcePosition = voice.sourceOffset
                    + static_cast<long double>(outputFrame - voice.start)
                        * voice.source->sampleRate / outputRate;
                if (sourcePosition >= voice.source->frames) break;
                const auto a = static_cast<std::int64_t>(sourcePosition);
                const auto b = std::min(a + 1, voice.source->frames - 1);
                const float amount = static_cast<float>(sourcePosition - a);
                float al{}, ar{}, bl{}, br{};
                if (!sample(voice, a, al, ar) || !sample(voice, b, bl, br)) {
                    fail(voice, "Reading streamed source failed for ");
                    return;
                }
                const auto index = static_cast<int>(outputFrame - frame);
                left[index] += (al + (bl - al) * amount) * gain;
                right[index] += (ar + (br - ar) * amount) * gain;
            }
        }
        frame = blockEnd;
    }

    bool failed() const noexcept { return failedFlag; }
    bool wasCancelled() const noexcept { return cancelledFlag; }
    const juce::String& failureMessage() const noexcept { return message; }
    std::int64_t renderedFrames() const noexcept { return frame; }

private:
    static constexpr int cacheFrames = 4096;
    struct Voice {
        int track{}, scene{};
        std::int64_t start{}, end{}, cacheStart{-1};
        long double sourceOffset{};
        int cacheCount{};
        std::shared_ptr<const DiskAudioFile> source;
        std::unique_ptr<DiskAudioReader> reader;
        std::array<float, cacheFrames> left{}, right{};
    };

    DeterministicStreamedArrangement(double rate, float level,
        const std::atomic<bool>& stop) : outputRate(rate), gain(level), cancelled(stop) {}

    bool sample(Voice& voice, std::int64_t position, float& left, float& right) {
        if (position < voice.cacheStart || position >= voice.cacheStart + voice.cacheCount) {
            voice.cacheStart = position;
            voice.cacheCount = static_cast<int>(std::min<std::int64_t>(
                cacheFrames, voice.source->frames - position));
            if (voice.cacheCount <= 0 || !voice.reader->read(position, voice.left.data(),
                voice.source->channels == 2 ? voice.right.data() : nullptr, voice.cacheCount))
                return false;
            if (voice.source->channels == 1)
                std::copy_n(voice.left.begin(), voice.cacheCount, voice.right.begin());
            for (int i = 0; i < voice.cacheCount; ++i) {
                if (!std::isfinite(voice.left[static_cast<std::size_t>(i)]))
                    voice.left[static_cast<std::size_t>(i)] = 0.0f;
                if (!std::isfinite(voice.right[static_cast<std::size_t>(i)]))
                    voice.right[static_cast<std::size_t>(i)] = 0.0f;
            }
        }
        const auto index = static_cast<std::size_t>(position - voice.cacheStart);
        left = voice.left[index]; right = voice.right[index];
        return true;
    }

    void fail(const Voice& voice, const juce::String& prefix) {
        failedFlag = true;
        message = prefix + "track " + juce::String(voice.track + 1)
            + ", scene " + juce::String(voice.scene + 1) + ".";
    }

    std::vector<Voice> voices;
    std::int64_t frame{};
    double outputRate{};
    float gain{};
    const std::atomic<bool>& cancelled;
    bool failedFlag{}, cancelledFlag{};
    juce::String message;
};

} // namespace alien
