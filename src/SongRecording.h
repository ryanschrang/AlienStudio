#pragma once
#include "RecordingQueue.h"
#include "SessionCapacity.h"
#include "Timeline.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace alien {

// Song-view audio takes, shared between the GUI and the audio thread. The GUI attaches one
// capture sink per armed track and sets the punch range before it asks RecordControls to start
// (with audioTake set); it detaches the sinks, under the audio callback lock, only after the
// take's end event. The audio thread writes the "reported" fields before it pushes that event.
struct SongTakeCapture {
    enum Ended { notEnded = 0, stopped = 1, punchedOut = 2, loopWrapped = 3, jumped = 4 };
    std::array<std::atomic<AudioCaptureSink*>, static_cast<std::size_t>(maxSessionTracks)> sinks{};
    std::atomic<bool> punch{false};
    std::atomic<std::int64_t> punchIn{0}, punchOut{0}; // ticks; used only with punch

    // Reported by the audio thread.
    std::atomic<double> firstTick{-1.0};    // the song position of the first captured frame, -1 before one
    std::atomic<std::uint64_t> frames{0};   // frames given to every sink so far
    std::atomic<int> latencySamples{0};     // device input + output latency when the take began
    std::atomic<int> ended{notEnded};
    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::int64_t>::is_always_lock_free);

    void resetReport() noexcept {
        firstTick.store(-1.0); frames.store(0); latencySamples.store(0); ended.store(notEnded);
    }
    bool anySink() const noexcept {
        for (const auto& sink : sinks) if (sink.load(std::memory_order_relaxed) != nullptr) return true;
        return false;
    }
};

// Where a finished take goes on the timeline. Frame k of the take was played, by someone hearing
// the song, at tick  firstTick - latency + k * ticksPerSample:  the input reaches the engine
// `latency` late, measured from the moment the song was heard. The region starts on the first whole
// tick at or after that (and not before `notBefore`, the punch-in) and skips the audio before it.
struct TakePlacement {
    bool valid{};
    std::int64_t start{}, length{};
    double audioOffset{}; // seconds into the recorded file
};

inline TakePlacement placeTake(double firstTick, std::uint64_t frames, double sampleRate, double bpm,
                               double latencySamples, std::int64_t notBefore = 0, std::int64_t notAfter = -1) noexcept {
    TakePlacement p;
    if (!(firstTick >= 0.0) || frames == 0 || !(sampleRate > 0.0) || !(bpm > 0.0) || !std::isfinite(latencySamples)) return p;
    const double perSample = 16.0 * bpm / sampleRate; // 960 ticks per beat
    const double played = firstTick - latencySamples * perSample;
    const double end = played + static_cast<double>(frames) * perSample;
    const double from = std::max({played, 0.0, static_cast<double>(std::max<std::int64_t>(0, notBefore))});
    // A tick barely past a whole one (rounding in the playhead) still starts on it.
    p.start = static_cast<std::int64_t>(std::ceil(from - 1.0e-6));
    std::int64_t last = static_cast<std::int64_t>(std::floor(end + 1.0e-6));
    if (notAfter >= 0) last = std::min(last, notAfter);
    p.length = last - p.start;
    if (p.length < 1) return p;
    p.audioOffset = std::max(0.0, (static_cast<double>(p.start) - played) / perSample / sampleRate);
    p.valid = true;
    return p;
}

// The same placement on a song with tempo changes: the latency and the take's
// length are real time, so they cover more or fewer ticks where the tempo differs.
inline TakePlacement placeTake(const timeline::TempoMap& tempo, double firstTick, std::uint64_t frames, double sampleRate,
                               double latencySamples, std::int64_t notBefore = 0, std::int64_t notAfter = -1) noexcept {
    TakePlacement p;
    if (!(firstTick >= 0.0) || frames == 0 || !(sampleRate > 0.0) || !std::isfinite(latencySamples)) return p;
    const double played = tempo.tickAfter(firstTick, -latencySamples / sampleRate);
    const double end = tempo.tickAfter(played, static_cast<double>(frames) / sampleRate);
    const double from = std::max({played, 0.0, static_cast<double>(std::max<std::int64_t>(0, notBefore))});
    p.start = static_cast<std::int64_t>(std::ceil(from - 1.0e-6));
    std::int64_t last = static_cast<std::int64_t>(std::floor(end + 1.0e-6));
    if (notAfter >= 0) last = std::min(last, notAfter);
    p.length = last - p.start;
    if (p.length < 1) return p;
    p.audioOffset = std::max(0.0, tempo.secondsBetween(played, static_cast<double>(p.start)));
    p.valid = true;
    return p;
}

} // namespace alien
