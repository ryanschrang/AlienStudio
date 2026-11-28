#pragma once
#include "Timeline.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace alien::sample_preview {
// `beats` of the sample (quarter notes at its source BPM) that play at `bpm`.
struct TempoSpan {
    double beats{};
    double bpm{120};
    bool operator==(const TempoSpan&) const = default;
};

struct TempoTarget {
    double bpm{120};
    bool song{};
    // Song only: the tick a preview handed over now would start on (the next quarter note while
    // playing, the cursor while stopped), the tempo course from there for `courseSeconds`, and a
    // fingerprint of the tempo map and loop it was read from, so an edit can be told apart from the
    // playhead crossing a change.
    double startTick{-1};
    std::vector<TempoSpan> course;
    std::uint64_t map{};
};

inline constexpr double courseSeconds = 5;      // the preview's output bound
inline constexpr std::size_t maxCourseSpans = 256;

// Where a preview would start. Song playback hands a matched preview over on the next quarter
// note (the first one past a published block start); at the loop end that is the loop start.
inline double previewStartTick(const timeline::Timeline& song, bool playing, std::int64_t position, std::int64_t cursor) noexcept {
    if (!playing || position < 0) return static_cast<double>(std::max<std::int64_t>(0, cursor));
    const auto beat = static_cast<double>(timeline::ticksPerBeat);
    double start = (std::floor(static_cast<double>(position) / beat + 1.0e-9) + 1) * beat;
    if (song.loopEnabled && position < song.loopEnd && start >= static_cast<double>(song.loopEnd))
        start = static_cast<double>(song.loopStart);
    return start;
}

// The tempo spans a preview starting at `startTick` plays through in its first `seconds`: tempo
// changes split it, and an enabled loop it reaches sends it back to the loop start. Adjacent spans
// at one tempo are merged; past `maxCourseSpans` the last tempo carries on.
inline std::vector<TempoSpan> tempoCourse(const timeline::Timeline& song, double projectBpm, double startTick,
                                          double seconds = courseSeconds) {
    std::vector<TempoSpan> course;
    const timeline::TempoMap map{song.tempoChanges, projectBpm};
    const auto loopStart = static_cast<double>(song.loopStart), loopEnd = static_cast<double>(song.loopEnd);
    const bool loops = song.loopEnabled && loopEnd > loopStart;
    double tick = std::max(0.0, startTick), left = seconds;
    while (left > 1.0e-9) {
        const double bpm = map.tempoAt(tick);
        double end = map.nextChange(tick);
        const bool wraps = loops && tick < loopEnd && loopEnd <= end;
        if (wraps) end = loopEnd;
        const double spanSeconds = timeline::secondsForTicks(end - tick, bpm);
        const bool last = !(spanSeconds < left) || course.size() + 1 >= maxCourseSpans;
        const double beats = last ? left * bpm / 60 : (end - tick) / static_cast<double>(timeline::ticksPerBeat);
        if (!course.empty() && course.back().bpm == bpm) course.back().beats += beats;
        else course.push_back({beats, bpm});
        if (last) break;
        left -= spanSeconds;
        tick = wraps ? loopStart : end;
    }
    return course;
}

inline std::uint64_t tempoMapFingerprint(const timeline::Timeline& song, double projectBpm) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    const auto bits = [](double value) { std::uint64_t out{}; std::memcpy(&out, &value, sizeof out); return out; };
    mix(bits(projectBpm));
    for (const auto& change : song.tempoChanges) { mix(static_cast<std::uint64_t>(change.tick)); mix(bits(change.bpm)); }
    mix(song.loopEnabled ? 1 : 0);
    mix(static_cast<std::uint64_t>(song.loopStart)); mix(static_cast<std::uint64_t>(song.loopEnd));
    return hash;
}

// Message-thread context for a bounded preview. Song playback publishes its wrapped playhead;
// while stopped (or before the first position arrives), use the editor cursor. `bpm` is the tempo
// at that position; the course is the tempo the preview itself would play through.
inline TempoTarget tempoTarget(const timeline::Timeline& song, double projectBpm, bool songMode,
    bool playing, std::int64_t position, std::int64_t cursor) {
    if (!songMode) return {projectBpm, false};
    const auto tick = std::max<std::int64_t>(0, playing && position >= 0 ? position : cursor);
    TempoTarget target{timeline::TempoMap{song.tempoChanges, projectBpm}.tempoAt(static_cast<double>(tick)), true};
    target.startTick = previewStartTick(song, playing, position, cursor);
    target.course = tempoCourse(song, projectBpm, target.startTick);
    target.map = tempoMapFingerprint(song, projectBpm);
    return target;
}
} // namespace alien::sample_preview
