#pragma once
// Capture MIDI (MIDI-04): get back what was just played without having pressed Record.
//
// The audio thread stamps every note it is given to play live (the typing piano, the on-screen keyboard and
// external MIDI input, after sustain) with the engine's running clock in seconds and the project tempo, and
// pushes it into a fixed-size ring (CaptureRing): no allocation, no locks, a full ring drops the note and counts
// it. Piano-roll previews and notes that clips and patterns play are never captured.
//
// The GUI drains the ring into a fixed-size history (CaptureHistory, the most recent 4,096 note-ons and
// note-offs) and, when asked, turns the last phrase into piano-roll notes (CaptureHistory::capture):
// - Notes are paired per source (the typing piano, or each MIDI channel) and key: a second note-on on a key that
//   is already down ends the first there, a source's all-notes-off ends everything it holds, and a key still down
//   at the moment of capture ends then, so a captured note is never left open.
// - The phrase is the notes since the last pause of at least `phraseGapSeconds` in which no key was held.
// - Seconds become ticks (960 per beat) at the tempo that was set when the phrase's first note was played; the
//   first note starts the block. Timing is as fine as the audio block the note arrived in.
// - A phrase longer than the longest clip (256 sixteenths) keeps its most recent part.
// - The block's length is whole 4/4 bars that hold it, as MIDI clip import counts them (MidiClipImport.h).
#include "PianoRollNoteInsert.h"
#include "PianoRollSequence.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alien::capture {

inline constexpr int typingSource = 0;          // the typing piano and on-screen keyboard; MIDI channels are 1..16
inline constexpr int sourceCount = 17;
inline constexpr double phraseGapSeconds = 2.0; // this long with no key down starts a new phrase
inline constexpr int minimumLengthTicks = 24;   // as RecordingAssembler: about a tenth of a sixteenth
inline constexpr int barTicks = PianoRollPattern::ticksPerStep * 16;
inline constexpr int maxTicks = PianoRollPattern::maxLoopSteps * PianoRollPattern::ticksPerStep;

struct Event {
    double seconds{};    // the engine's capture clock when the note arrived
    float bpm{120.0f};   // the project tempo then
    float velocity{};    // 0 = note-off
    std::int16_t pitch{};   // 0..127, or -1 = every note of this source ends
    std::uint8_t source{};  // typingSource, or a MIDI channel 1..16
};

// Single producer (the audio thread), single consumer (the GUI). Never blocks or allocates after construction.
class CaptureRing {
public:
    static constexpr std::size_t capacity = 4096;
    CaptureRing() : events(std::make_unique<Event[]>(capacity)) {}
    bool push(const Event& event) noexcept {
        const auto w = write.load(std::memory_order_relaxed);
        const auto next = (w + 1) % capacity;
        if (next == read.load(std::memory_order_acquire)) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        events[w] = event;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(Event& event) noexcept {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        event = events[r];
        read.store((r + 1) % capacity, std::memory_order_release);
        return true;
    }
    std::uint64_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }
private:
    std::unique_ptr<Event[]> events;
    std::atomic<std::size_t> read{0}, write{0};
    std::atomic<std::uint64_t> dropped{0};
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
};

struct Phrase {
    std::vector<PianoRollNote> notes;   // timed in sixteenths from the first note; ids are left for the pattern to give
    int lengthTicks{};                  // whole 4/4 bars that hold the notes; 0 when nothing was played
    double bpm{};                       // the tempo the phrase was timed at
    int leftOut{};                      // earlier notes of a phrase longer than the longest clip
    int stillHeld{};                    // notes whose key was still down, ended at the moment of capture
    bool empty() const noexcept { return notes.empty(); }
    double lengthSteps() const noexcept { return PianoRollPattern::fromTicks(lengthTicks); }
};

// GUI side. Keeps the most recent `capacity` events; the oldest go first.
class CaptureHistory {
public:
    static constexpr std::size_t capacity = 4096;
    CaptureHistory() { events.reserve(capacity); }

    void add(const Event& event) {
        if (!std::isfinite(event.seconds) || event.source >= sourceCount || event.pitch > 127 || event.pitch < -1) return;
        if (events.size() < capacity) { events.push_back(event); return; }
        events[oldest] = event;
        oldest = (oldest + 1) % capacity;
    }
    void drain(CaptureRing& ring) { Event event; while (ring.pop(event)) add(event); }
    void clear() { events.clear(); oldest = 0; }
    std::size_t size() const noexcept { return events.size(); }

    // The last phrase played up to `nowSeconds` (the engine's capture clock now).
    Phrase capture(double nowSeconds) const {
        struct Played { double start, end; float velocity, bpm; int pitch; bool held; };
        std::vector<Played> played;
        struct Open { bool down{}; double start{}; float velocity{}, bpm{}; };
        std::array<std::array<Open, 128>, sourceCount> open{};
        double last = 0.0;
        const auto close = [&](Open& o, int pitch, double at, bool held) {
            if (!o.down) return;
            o.down = false;
            played.push_back({o.start, std::max(at, o.start), o.velocity, o.bpm, pitch, held});
        };
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[(oldest + i) % events.size()];
            last = std::max(last, e.seconds);
            auto& keys = open[e.source];
            if (e.pitch < 0) { for (int p = 0; p < 128; ++p) close(keys[static_cast<std::size_t>(p)], p, e.seconds, false); continue; }
            auto& key = keys[static_cast<std::size_t>(e.pitch)];
            close(key, e.pitch, e.seconds, false);   // a note-off, or a retrigger of a key already down
            if (e.velocity > 0.0f) key = {true, e.seconds, std::clamp(e.velocity, 1.0f / 127.0f, 1.0f), e.bpm};
        }
        const double now = std::isfinite(nowSeconds) ? std::max(nowSeconds, last) : last;
        for (auto& keys : open) for (int p = 0; p < 128; ++p) close(keys[static_cast<std::size_t>(p)], p, now, true);
        Phrase phrase;
        if (played.empty()) return phrase;
        std::stable_sort(played.begin(), played.end(), [](const Played& a, const Played& b) {
            return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
        });
        // The phrase begins after the last pause with no key down.
        std::size_t begin = 0;
        double heldUntil = played.front().end;
        for (std::size_t i = 1; i < played.size(); ++i) {
            if (played[i].start - heldUntil >= phraseGapSeconds) begin = i;
            heldUntil = std::max(heldUntil, played[i].end);
        }
        const float bpm = played[begin].bpm;
        phrase.bpm = std::isfinite(bpm) && bpm > 0.0f ? bpm : 120.0;
        const double ticksPerSecond = phrase.bpm / 60.0 * (PianoRollPattern::ticksPerStep * 4);
        const auto tickOf = [&](double seconds) {
            return static_cast<long long>(std::llround((seconds - played[begin].start) * ticksPerSecond));
        };
        // Too long for one clip: keep the part that ends with the last note, so the last note has a sixteenth.
        long long first = 0;
        const long long lastStart = tickOf(played.back().start);
        if (lastStart > maxTicks - PianoRollPattern::ticksPerStep) first = lastStart - (maxTicks - PianoRollPattern::ticksPerStep);
        long long origin = -1;
        for (std::size_t i = begin; i < played.size(); ++i) {
            const auto& p = played[i];
            const long long start = tickOf(p.start);
            if (start < first) { ++phrase.leftOut; continue; }
            if (origin < 0) origin = start;
            const long long at = start - origin;
            const long long length = std::max<long long>(tickOf(p.end) - start, minimumLengthTicks);
            phrase.notes.push_back({0, p.pitch, PianoRollPattern::fromTicks(static_cast<int>(at)),
                                    PianoRollPattern::fromTicks(static_cast<int>(std::min<long long>(length, maxTicks))), p.velocity, 1.0f});
            if (p.held) ++phrase.stillHeld;
        }
        // Whole bars: a note that starts in a bar is in it, and one held less than a sixteenth past a bar line
        // does not make another bar (as MIDI clip import counts them).
        int bars = 1;
        for (const auto& n : phrase.notes) {
            const int start = PianoRollPattern::toTicks(n.startStep), length = PianoRollPattern::toTicks(n.lengthSteps);
            bars = std::max({bars, start / barTicks + 1, (start + length - PianoRollPattern::ticksPerStep + barTicks - 1) / barTicks});
        }
        phrase.lengthTicks = std::min(bars * barTicks, maxTicks);
        return phrase;
    }

private:
    std::vector<Event> events;
    std::size_t oldest{};
};

// How a capture into the piano roll may lengthen its loop: to whole 4/4 bars that hold the phrase.
inline LoopGrowth loopGrowth(const Phrase& phrase) noexcept { return {phrase.lengthSteps(), barTicks}; }

// One line for the editor's status bar.
inline std::string describe(const Phrase& phrase, const NoteInsertResult& result) {
    if (phrase.empty()) return "No changes: nothing has been played on the keyboard or a MIDI controller to capture yet.";
    if (!result.changed()) return "No changes: none of the captured notes fit after the cursor. Move the cursor earlier.";
    std::string text = "Captured " + std::to_string(result.added) + (result.added == 1 ? " note" : " notes") + " played at "
                     + std::to_string(static_cast<int>(std::lround(phrase.bpm))) + " BPM";
    if (result.grewLoop()) {
        const auto bars = [](int steps) {
            const int ticks = steps * PianoRollPattern::ticksPerStep;
            if (ticks % barTicks == 0) return std::to_string(ticks / barTicks) + (ticks == barTicks ? " bar" : " bars");
            return std::to_string(steps) + " sixteenths";
        };
        text += "; the clip was lengthened from " + bars(result.loopFrom) + " to " + bars(result.loopTo) + " to hold them";
    }
    if (phrase.leftOut != 0) text += "; " + std::to_string(phrase.leftOut) + " earlier notes were longer ago than the longest clip";
    if (result.skipped != 0) text += "; " + std::to_string(result.skipped) + " past the clip's end were left out";
    if (result.trimmed != 0) text += "; " + std::to_string(result.trimmed) + " were cut at the clip's end";
    if (phrase.stillHeld != 0) text += "; " + std::to_string(phrase.stillHeld) + " still held were ended now";
    return text + ".";
}

} // namespace alien::capture
