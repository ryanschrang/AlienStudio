#pragma once
#include "PianoRollProcessing.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace alien {

enum class ClipboardStatus {
    copied, pasted, emptySelection, emptyClipboard, capacityExceeded, outOfBounds, invalidInput
};

struct ClipboardResult {
    ClipboardStatus status{ClipboardStatus::emptySelection};
    PianoRollSelection selection{};
    double startStep{}, endStep{};
    // Only successful paste/duplicate operations edit the pattern or history.
    bool changed() const noexcept { return status == ClipboardStatus::pasted; }
};

// An editor-owned, session-only clipboard. This fixed snapshot is independent
// of the source notes and the operating-system clipboard. Copying never edits
// the clip; empty/stale selections leave the previous snapshot available.
class PianoRollClipboard {
public:
    bool empty() const noexcept { return count == 0; }
    std::size_t size() const noexcept { return count; }
    double lengthSteps() const noexcept { return PianoRollPattern::fromTicks(extentTicks); }
    void clear() noexcept { notes = {}; count = 0; extentTicks = 0; }

    ClipboardResult copy(const PianoRollPattern& pattern, const PianoRollSelection& selection) noexcept {
        if (!pattern.valid() || !selection.valid()) return {ClipboardStatus::invalidInput};
        PianoRollSelection retained = selection;
        retained.retain(pattern);
        if (retained.count == 0) return {ClipboardStatus::emptySelection};

        int first = std::numeric_limits<int>::max();
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (retained.contains(pattern.notes[i].id))
                first = std::min(first, pattern.toTicks(pattern.notes[i].startStep));
        PianoRollClipboard replacement;
        for (std::size_t i = 0; i < pattern.count; ++i) if (retained.contains(pattern.notes[i].id)) {
            const auto& note = pattern.notes[i];
            const int offset = pattern.toTicks(note.startStep) - first;
            const int length = pattern.toTicks(note.lengthSteps);
            replacement.notes[replacement.count++] = {note.pitch, offset, length, note.velocity};
            replacement.extentTicks = std::max(replacement.extentTicks, offset + length);
        }
        *this = replacement;
        return {ClipboardStatus::copied, retained, pattern.fromTicks(first),
                pattern.fromTicks(first + extentTicks)};
    }

    // insertionStep is an explicit absolute position on the stored tick grid.
    // All notes must fit unchanged. Capacity and end-boundary failures leave
    // the pattern, selection and private future-ID allocator untouched.
    ClipboardResult paste(PianoRollPattern& pattern, double insertionStep) const noexcept {
        if (!pattern.valid() || !std::isfinite(insertionStep)) return {ClipboardStatus::invalidInput};
        if (insertionStep < 0 || insertionStep > pattern.loopSteps) return {ClipboardStatus::outOfBounds};
        const int insertion = pattern.toTicks(insertionStep);
        if (std::abs(insertionStep * pattern.ticksPerStep - insertion) > 1e-7)
            return {ClipboardStatus::invalidInput};
        if (empty()) return {ClipboardStatus::emptyClipboard};
        if (count > pattern.maxNotes - pattern.count) return {ClipboardStatus::capacityExceeded};
        if (extentTicks > pattern.loopSteps * pattern.ticksPerStep - insertion)
            return {ClipboardStatus::outOfBounds};

        auto edited = pattern;
        PianoRollSelection pasted;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& note = notes[i];
            const auto id = edited.add(note.pitch, pattern.fromTicks(insertion + note.offsetTicks),
                                       pattern.fromTicks(note.lengthTicks), note.velocity);
            if (id == 0 || !pasted.add(id)) return {ClipboardStatus::invalidInput};
            // The model accepts every positive finite velocity. add() applies
            // the editor's usual minimum; a clipboard preserves the source
            // value exactly, including valid externally supplied lower values.
            edited.find(id)->velocity = note.velocity;
        }
        if (!edited.valid()) return {ClipboardStatus::invalidInput};
        pattern = edited;
        return {ClipboardStatus::pasted, pasted, pattern.fromTicks(insertion),
                pattern.fromTicks(insertion + extentTicks)};
    }

private:
    struct Note { int pitch{}, offsetTicks{}, lengthTicks{}; float velocity{}; };
    std::array<Note, PianoRollPattern::maxNotes> notes{};
    std::size_t count{};
    int extentTicks{};
};

// Duplicate starts the selected block at the first snap boundary at or after
// its latest end. Relative offsets and lengths stay exact, including sub-step
// timing. A temporary clipboard leaves the editor's copied material intact.
inline ClipboardResult duplicatePianoRollSelection(PianoRollPattern& pattern,
                                                   const PianoRollSelection& selection,
                                                   double gridSteps = 1.0) noexcept {
    if (!std::isfinite(gridSteps) || gridSteps < pattern.timeQuantum || gridSteps > pattern.maxLoopSteps)
        return {ClipboardStatus::invalidInput};
    const int grid = pattern.toTicks(gridSteps);
    if (std::abs(gridSteps * pattern.ticksPerStep - grid) > 1e-7)
        return {ClipboardStatus::invalidInput};
    PianoRollClipboard temporary;
    const auto copied = temporary.copy(pattern, selection);
    if (copied.status != ClipboardStatus::copied) return copied;
    const int end = pattern.toTicks(copied.endStep);
    const int insertion = ((end + grid - 1) / grid) * grid;
    return temporary.paste(pattern, pattern.fromTicks(insertion));
}
}
