#pragma once
// Adds a block of notes to a pattern at a position, as MIDI clip import does (MidiClipImport.h). No JUCE:
// the editor and its tests use it without the MIDI file reader.
#include "PianoRollProcessing.h"
#include "PianoRollSequence.h"
#include <algorithm>
#include <cmath>
#include <span>

namespace alien {

struct NoteInsertResult {
    PianoRollSelection selection;          // the notes that were added
    int added{}, trimmed{}, skipped{};     // trimmed: cut at the loop's end; skipped: started past it, or no room
    int loopFrom{}, loopTo{};              // the loop's length before and after the insert lengthened it; 0 when it did not
    bool changed() const noexcept { return added != 0; }
    bool grewLoop() const noexcept { return loopFrom != 0; }
};

// Lets an insert lengthen the loop to hold a block `blockSteps` long: to the first whole bar of `barTicks`
// (counted from the loop's start) at or after the block's end, and never past the longest loop.
struct LoopGrowth {
    double blockSteps{};
    int barTicks{PianoRollPattern::ticksPerStep * 16};
};

// The loop length, in sixteenths, that an insert at `atStep` grows `pattern` to; the current length when it
// already holds the block. Only ever longer, so no note already in the loop moves or is cut.
inline int grownLoopSteps(const PianoRollPattern& pattern, double atStep, const LoopGrowth& grow) noexcept {
    if (!std::isfinite(atStep) || !std::isfinite(grow.blockSteps) || grow.blockSteps <= 0 || grow.barTicks < 1) return pattern.loopSteps;
    const long long loopTicks = static_cast<long long>(pattern.loopSteps) * PianoRollPattern::ticksPerStep;
    const long long end = std::clamp<long long>(PianoRollPattern::toTicks(atStep), 0, loopTicks) + PianoRollPattern::toTicks(grow.blockSteps);
    const long long bars = (end + grow.barTicks - 1) / grow.barTicks;
    const long long steps = (bars * grow.barTicks + PianoRollPattern::ticksPerStep - 1) / PianoRollPattern::ticksPerStep;
    return static_cast<int>(std::clamp<long long>(steps, pattern.loopSteps, std::max(pattern.loopSteps, PianoRollPattern::maxLoopSteps)));
}

// Adds `notes` (times in sixteenths from the block's start; ids are ignored) `atStep` sixteenths into
// `pattern`, keeping the notes already there. With `grow`, the loop first lengthens to hold the block (see
// LoopGrowth); without it the loop's length never changes. A note that would start at or past the loop's end is
// left out and one that crosses it is cut there, so nothing lands outside the clip. Stops adding at the
// pattern's note limit. When nothing is added the loop keeps its length.
inline NoteInsertResult insertNotes(PianoRollPattern& pattern, std::span<const PianoRollNote> notes, double atStep,
                                    const LoopGrowth* grow = nullptr) noexcept {
    NoteInsertResult result;
    if (!pattern.valid() || !std::isfinite(atStep)) return result;
    const int originalLoop = pattern.loopSteps;
    if (grow != nullptr) pattern.loopSteps = grownLoopSteps(pattern, atStep, *grow);   // longer only: every note still fits
    const int loopTicks = pattern.loopSteps * PianoRollPattern::ticksPerStep;
    const int offset = std::clamp(PianoRollPattern::toTicks(atStep), 0, loopTicks);
    for (const auto& note : notes) {
        const int start = offset + PianoRollPattern::toTicks(note.startStep);
        const int length = std::max(1, PianoRollPattern::toTicks(note.lengthSteps));
        if (start < offset || start >= loopTicks || pattern.count == PianoRollPattern::maxNotes) { ++result.skipped; continue; }
        const int kept = std::min(length, loopTicks - start);
        if (kept < length) ++result.trimmed;
        const auto id = pattern.add(note.pitch, PianoRollPattern::fromTicks(start), PianoRollPattern::fromTicks(kept),
                                    note.velocity, note.probability);
        if (id == 0) { ++result.skipped; continue; }
        result.selection.add(id);
        ++result.added;
    }
    if (!result.changed()) pattern.loopSteps = originalLoop;
    else if (pattern.loopSteps != originalLoop) { result.loopFrom = originalLoop; result.loopTo = pattern.loopSteps; }
    return result;
}

} // namespace alien
