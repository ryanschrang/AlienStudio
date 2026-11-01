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
    bool changed() const noexcept { return added != 0; }
};

// Adds `notes` (times in sixteenths from the block's start; ids are ignored) `atStep` sixteenths into
// `pattern`, keeping the notes already there. A note that would start at or past the loop's end is left out
// and one that crosses it is cut there, so nothing lands outside the clip and the loop's length never changes.
// Stops adding at the pattern's note limit.
inline NoteInsertResult insertNotes(PianoRollPattern& pattern, std::span<const PianoRollNote> notes, double atStep) noexcept {
    NoteInsertResult result;
    if (!pattern.valid() || !std::isfinite(atStep)) return result;
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
    return result;
}

} // namespace alien
