#pragma once
#include "RecordingQueue.h"
#include "SessionCapacity.h"
#include "Timeline.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

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
    // Loop recording: when the loop wraps, the take goes on as its next pass. Pass 0 begins at frame 0
    // and firstTick; pass k > 0 at frame passFrame[k], where the song was at passTick[k] (the loop's
    // start). With no room for another pass the take ends where the loop wraps.
    static constexpr int maxPasses = 64;
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(maxPasses)> passFrame{};
    std::array<std::atomic<double>, static_cast<std::size_t>(maxPasses)> passTick{};
    std::atomic<int> passes{1};
    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::int64_t>::is_always_lock_free);

    void resetReport() noexcept {
        firstTick.store(-1.0); frames.store(0); latencySamples.store(0); ended.store(notEnded); passes.store(1);
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

// One pass of a loop recording: where it begins in the take's file and on the song.
struct TakePass {
    std::uint64_t firstFrame{};
    double firstTick{};
};
struct PassPlacement {
    TakePlacement placement;   // audioOffset counts from the start of the whole file
    bool complete{};           // the loop wrapped after it (the last pass was stopped part-way)
};
// Places every pass of a take of `frames` frames. A pass's last `latency` frames were heard before
// the loop wrapped, so they are recorded at the start of the next pass: each pass takes them from
// there, and each later pass starts at the loop's start, skipping them. The first pass starts no
// earlier than `notBefore`, the last ends no later than `notAfter` (-1: anywhere), and every pass
// the loop wrapped after ends at `loopEnd`.
inline std::vector<PassPlacement> placePasses(const timeline::TempoMap& tempo, const std::vector<TakePass>& passes,
                                              std::uint64_t frames, double sampleRate, double latencySamples,
                                              std::int64_t notBefore, std::int64_t notAfter,
                                              std::int64_t loopStart, std::int64_t loopEnd) {
    std::vector<PassPlacement> out;
    const auto carry = static_cast<std::uint64_t>(std::ceil(std::max(0.0, latencySamples)));
    for (std::size_t k = 0; k < passes.size(); ++k) {
        const bool last = k + 1 == passes.size();
        const auto begin = passes[k].firstFrame;
        if (begin >= frames) break;
        auto end = last ? frames : std::min(frames, passes[k + 1].firstFrame + carry);
        if (end <= begin) continue;
        PassPlacement pass;
        pass.complete = !last;
        pass.placement = placeTake(tempo, passes[k].firstTick, end - begin, sampleRate, latencySamples,
                                   k == 0 ? notBefore : loopStart, last ? notAfter : loopEnd);
        pass.placement.audioOffset += static_cast<double>(begin) / sampleRate;
        out.push_back(pass);
    }
    return out;
}

} // namespace alien
