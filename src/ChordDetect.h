#pragma once
// Chord detection from MIDI (CONV-03, part 1): names the harmony a Song MIDI clip plays, bar by bar, so it can be
// written to the Song chord track. No JUCE and no allocation-sensitive code: it runs on the message thread when
// asked, never on the audio thread.
//
// - Each window (a bar of the song's meter, or half a bar, see detectRange) weighs the 12 pitch classes by how
//   long each note sounds inside it, a little more for louder notes.
// - Every root and every chord the chord track knows (major, minor, diminished, augmented, sus2, sus4, dominant 7,
//   major 7, minor 7) is scored against those weights: chord tones count for it, other tones against it, a chord
//   tone that is not heard costs a little, a chord whose root is not heard is never chosen, and a chord whose root
//   is the lowest sustained note gets a small bonus, which decides between chords that share their tones (C sus4
//   and F sus2 are both C, F and G).
// - A window needs its root and at least one more chord tone, and a clear enough score, to be named; otherwise it
//   is left without a chord. Ties go to the simpler chord (the order above), then the lower root, so the same notes
//   always give the same answer.
// - A bar whose two halves are clearly different chords (each fits its half better than the bar's chord fits it)
//   is named per half; neighbouring windows with the same chord are joined into one chord.
#include "PianoRollSequence.h"
#include "SongChord.h"
#include "Timeline.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace alien::chord_detect {

struct TimedNote { std::int64_t start{}, end{}; int pitch{60}; float velocity{0.8f}; };   // song ticks
struct Guess { int root{}; ChordType quality{ChordType::major}; double score{}; };
struct Chord { std::int64_t start{}, end{}; int root{}; ChordType quality{ChordType::major}; };

inline constexpr int qualityCount = 9;
inline constexpr double minimumScore = 0.45;     // below this a window is left without a chord
inline constexpr double splitAdvantage = 0.1;    // how much better two half-bar chords must fit than one bar chord
inline constexpr std::size_t maxNotes = 200000;  // a longer projection is refused rather than analysed slowly

inline std::array<int, 4> intervalsOf(ChordType quality, int& count) noexcept {
    switch (quality) {
    case ChordType::major: count = 3; return {0, 4, 7, 0};
    case ChordType::minor: count = 3; return {0, 3, 7, 0};
    case ChordType::diminished: count = 3; return {0, 3, 6, 0};
    case ChordType::augmented: count = 3; return {0, 4, 8, 0};
    case ChordType::suspended2: count = 3; return {0, 2, 7, 0};
    case ChordType::suspended4: count = 3; return {0, 5, 7, 0};
    case ChordType::dominant7: count = 4; return {0, 4, 7, 10};
    case ChordType::major7: count = 4; return {0, 4, 7, 11};
    case ChordType::minor7: count = 4; return {0, 3, 7, 10};
    }
    count = 0;
    return {};
}

// The pitch-class weights of `notes` in [from, to), and the lowest note that sounds for a real part of it.
struct Window {
    std::array<double, 12> weight{};
    double total{}, loudest{};
    int bass{128};
    bool empty() const noexcept { return total <= 0.0; }
};
inline Window weigh(const std::vector<TimedNote>& notes, std::int64_t from, std::int64_t to) {
    Window w;
    if (to <= from) return w;
    const std::int64_t window = to - from;
    int anyLow = 128;
    for (const auto& n : notes) {
        const std::int64_t overlap = std::min(n.end, to) - std::max(n.start, from);
        if (overlap <= 0 || n.pitch < 0 || n.pitch > 127) continue;
        const double loud = 0.5 + 0.5 * std::clamp(static_cast<double>(n.velocity), 0.0, 1.0);
        w.weight[static_cast<std::size_t>(n.pitch % 12)] += static_cast<double>(overlap) * loud;
        anyLow = std::min(anyLow, n.pitch);
        // The bass is the lowest note that sounds for a real part of the window, not a passing grace note.
        if (overlap * 8 >= window || overlap >= timeline::ticksPerStep * 4) w.bass = std::min(w.bass, n.pitch);
    }
    for (const double x : w.weight) { w.total += x; w.loudest = std::max(w.loudest, x); }
    if (w.bass > 127) w.bass = anyLow;
    return w;
}

// How well `root` `quality` fits the window, or nothing when its root or all but one of its tones are not heard.
inline std::optional<double> score(const Window& w, int root, ChordType quality) noexcept {
    if (w.empty() || root < 0 || root > 11) return std::nullopt;
    const double heard = 0.05 * w.loudest;   // a tone this faint counts as not heard
    if (w.weight[static_cast<std::size_t>(root)] < heard) return std::nullopt;   // never a chord without its root
    int count = 0;
    const auto intervals = intervalsOf(quality, count);
    double inChord = 0.0;
    int present = 0;
    for (int i = 0; i < count; ++i) {
        const auto pc = static_cast<std::size_t>((root + intervals[static_cast<std::size_t>(i)]) % 12);
        inChord += w.weight[pc];
        if (w.weight[pc] >= heard) ++present;
    }
    if (present < 2) return std::nullopt;
    return inChord / w.total - 0.6 * (w.total - inChord) / w.total - 0.25 * (count - present) + (w.bass % 12 == root ? 0.15 : 0.0);
}

// The chord `notes` play in [from, to), or nothing when they are silent there or name no chord clearly.
inline std::optional<Guess> detect(const std::vector<TimedNote>& notes, std::int64_t from, std::int64_t to) {
    const auto w = weigh(notes, from, to);
    if (w.empty()) return std::nullopt;
    std::optional<Guess> best;
    for (int q = 0; q < qualityCount; ++q)
        for (int root = 0; root < 12; ++root)
            if (const auto s = score(w, root, static_cast<ChordType>(q)); s && (!best || *s > best->score + 1.0e-9))
                best = Guess{root, static_cast<ChordType>(q), *s};
    if (!best || best->score < minimumScore) return std::nullopt;
    return best;
}

// `region`'s notes in song ticks, from the clip `pattern` it plays: the clip's loop repeats from the region's
// offset and every note is cut at the region's edges. Empty when there would be more than maxNotes.
inline std::vector<TimedNote> regionNotes(const timeline::Region& region, const PianoRollPattern& pattern) {
    std::vector<TimedNote> out;
    if (pattern.count == 0 || region.length <= 0 || pattern.loopSteps < 1) return out;
    const std::int64_t loop = static_cast<std::int64_t>(pattern.loopSteps) * timeline::ticksPerStep;
    const std::int64_t offset = std::max<std::int64_t>(0, region.offset);
    const std::int64_t firstCycle = offset / loop, lastCycle = (offset + region.length - 1) / loop;
    if (static_cast<double>(lastCycle - firstCycle + 1) * static_cast<double>(pattern.count) > static_cast<double>(maxNotes)) return out;
    for (std::int64_t cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        const std::int64_t cycleStart = region.start + cycle * loop - offset;
        for (std::size_t i = 0; i < pattern.count; ++i) {
            const auto& n = pattern.notes[i];
            const std::int64_t a = cycleStart + std::llround(n.startStep * timeline::ticksPerStep);
            const std::int64_t b = a + std::llround(n.lengthSteps * timeline::ticksPerStep);
            const std::int64_t from = std::max(a, region.start), to = std::min(b, region.end());
            if (to > from) out.push_back({from, to, n.pitch, n.velocity});
        }
    }
    return out;
}

// The chords `notes` play from `from` to `to`, one per bar of `meters` (or per half bar where the harmony changes
// in the middle of a bar with an even number of beats), each cut to [from, to), neighbours with the same chord
// joined. Windows that name no chord are left out.
inline std::vector<Chord> detectRange(const std::vector<TimedNote>& notes, std::int64_t from, std::int64_t to,
                                      const timeline::MeterMap& meters) {
    std::vector<Chord> chords;
    if (to <= from || from < 0) return chords;
    const auto add = [&](std::int64_t a, std::int64_t b, const Guess& g) {
        if (!chords.empty() && chords.back().end == a && chords.back().root == g.root && chords.back().quality == g.quality)
            chords.back().end = b;
        else chords.push_back({a, b, g.root, g.quality});
    };
    for (auto bar = meters.barAt(static_cast<double>(from)); bar.start < to;) {
        const std::int64_t barEnd = bar.start + bar.meter.barTicks();
        const std::int64_t a = std::max(bar.start, from), b = std::min(barEnd, to);
        const auto whole = detect(notes, a, b);
        bool split = false;
        if (bar.meter.numerator % 2 == 0) {
            const std::int64_t middle = bar.start + bar.meter.barTicks() / 2;
            if (middle > a && middle < b) {
                // Split when each half's own chord fits it clearly better than the bar's chord does: A minor then F
                // is not one F major 7, though F major 7 holds every note of both.
                const auto first = detect(notes, a, middle), second = detect(notes, middle, b);
                const auto wholeIn = [&](std::int64_t x, std::int64_t y) {
                    return whole ? score(weigh(notes, x, y), whole->root, whole->quality).value_or(-1.0) : -1.0;
                };
                if (first && second && (first->root != second->root || first->quality != second->quality)
                    && (first->score + second->score) / 2.0 > (wholeIn(a, middle) + wholeIn(middle, b)) / 2.0 + splitAdvantage) {
                    add(a, middle, *first);
                    add(middle, b, *second);
                    split = true;
                }
            }
        }
        if (!split && whole) add(a, b, *whole);
        bar = {bar.index + 1, barEnd, meters.meterAtBar(bar.index + 1)};
    }
    chords.erase(std::remove_if(chords.begin(), chords.end(), [](const Chord& c) { return c.end - c.start < timeline::ticksPerStep; }),
                 chords.end());
    return chords;
}

// What writing `detected` over [from, to) does to the chord track's `existing` chords (ascending, never
// overlapping): chords inside the range are removed, ones that cross an edge are cut there (one that spans the
// whole range keeps both outer parts), and the detected chords are added. A part left shorter than a sixteenth
// goes too, as the chord track holds nothing shorter.
struct Plan {
    std::vector<std::uint32_t> removed;
    std::vector<timeline::SongChord> edited;   // the chord's id with its new start and end
    std::vector<Chord> added;                  // outer parts of a spanning chord first, then the detected chords
    bool changes() const noexcept { return !removed.empty() || !edited.empty() || !added.empty(); }
};
inline Plan plan(const std::vector<timeline::SongChord>& existing, std::int64_t from, std::int64_t to, const std::vector<Chord>& detected) {
    Plan p;
    for (const auto& c : existing) {
        if (c.end <= from || c.start >= to) continue;
        const bool keepLeft = c.start < from && from - c.start >= timeline::ticksPerStep;
        const bool keepRight = c.end > to && c.end - to >= timeline::ticksPerStep;
        if (keepLeft) { auto left = c; left.end = from; p.edited.push_back(left); }
        else if (keepRight) { auto right = c; right.start = to; p.edited.push_back(right); }
        else p.removed.push_back(c.id);
        if (keepLeft && keepRight) p.added.push_back({to, c.end, c.root, c.quality});
    }
    p.added.insert(p.added.end(), detected.begin(), detected.end());
    // Nothing to do when the range already holds exactly these chords.
    std::vector<Chord> inside;
    for (const auto& c : existing) if (c.start >= from && c.end <= to) inside.push_back({c.start, c.end, c.root, c.quality});
    const bool same = p.edited.empty() && inside.size() == detected.size()
        && std::equal(inside.begin(), inside.end(), detected.begin(), [](const Chord& x, const Chord& y) {
               return x.start == y.start && x.end == y.end && x.root == y.root && x.quality == y.quality;
           });
    if (same) return {};
    return p;
}

} // namespace alien::chord_detect
