#include "PianoRollClipboard.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static bool sameNote(const PianoRollNote& a, const PianoRollNote& b) {
    return a.id == b.id && a.pitch == b.pitch && a.startStep == b.startStep
        && a.lengthSteps == b.lengthSteps && a.velocity == b.velocity && a.probability == b.probability;
}

static void copyPasteTests() {
    PianoRollPattern source;
    const auto first = source.add(60, 4.125, 2.25, .6f, .35f);
    const auto middle = source.add(67, 4.125, 1, .8f);
    const auto late = source.add(64, 7.5, .25, .3f);
    const auto other = source.add(42, 0, 4, .7f);
    PianoRollSelection selected;
    selected.add(late); selected.add(first); selected.add(middle); selected.add(99999);
    const auto original = source;
    PianoRollClipboard clipboard;
    require(clipboard.empty() && clipboard.size() == 0 && clipboard.lengthSteps() == 0,
            "new clipboard is empty");
    const auto copied = clipboard.copy(source, selected);
    require(copied.status == ClipboardStatus::copied && !copied.changed()
        && copied.selection.count == 3 && copied.startStep == 4.125 && copied.endStep == 7.75,
        "copy filters stale IDs and returns original selected block bounds");
    require(!clipboard.empty() && clipboard.size() == 3 && clipboard.lengthSteps() == 3.625
        && samePianoRollPattern(source, original), "copy snapshots selected data without editing source");

    const auto pasted = clipboard.paste(source, 12.5);
    require(pasted.changed() && source.valid() && source.count == 7 && pasted.selection.count == 3
        && pasted.startStep == 12.5 && pasted.endStep == 16.125,
        "paste returns only fresh pasted IDs and placed block bounds");
    for (std::size_t i = 0; i < original.count; ++i)
        require(sameNote(source.notes[i], original.notes[i]), "paste preserves all existing notes");
    const auto* a = source.find(pasted.selection.ids[0]);
    const auto* b = source.find(pasted.selection.ids[1]);
    const auto* c = source.find(pasted.selection.ids[2]);
    require(a && b && c && a->id != first && b->id != middle && c->id != late
        && a->pitch == 60 && a->startStep == 12.5 && a->lengthSteps == 2.25 && a->velocity == .6f
        && b->pitch == 67 && b->startStep == 12.5 && b->lengthSteps == 1 && b->velocity == .8f
        && c->pitch == 64 && c->startStep == 15.875 && c->lengthSteps == .25 && c->velocity == .3f,
        "paste preserves chord timing, relative intervals, duration, velocity and source order");
    require(!pasted.selection.contains(first) && !pasted.selection.contains(other),
            "source notes are not selected by paste");
    const auto again = clipboard.paste(source, 12.5);
    require(again.changed() && source.count == 10 && source.valid(), "one snapshot can be pasted repeatedly");
    for (std::size_t i = 0; i < again.selection.count; ++i)
        require(!pasted.selection.contains(again.selection.ids[i]), "repeated paste always allocates new stable IDs");

    // Deletion and editing of the originals cannot change already copied data.
    source.clear();
    const auto afterDelete = clipboard.paste(source, 0);
    require(afterDelete.changed() && source.count == 3 && source.notes[0].startStep == 0
        && source.notes[2].startStep == 3.375 && source.notes[2].pitch == 64,
        "clipboard remains usable after all source notes are deleted");
    PianoRollPattern empty;
    require(clipboard.paste(empty, 32).changed() && empty.valid() && empty.count == 3,
            "copied notes can be pasted into a separate empty clip");
    clipboard.clear();
    require(clipboard.empty() && clipboard.size() == 0 && clipboard.lengthSteps() == 0,
            "clipboard clear discards the snapshot");
    require(clipboard.paste(empty, 0).status == ClipboardStatus::emptyClipboard,
            "pasting an empty clipboard is a no-op");
}

static void exactTickAndVelocityTests() {
    PianoRollPattern source;
    source.add(0, source.fromTicks(3), source.fromTicks(1), .4f);
    source.add(127, source.fromTicks(7), source.fromTicks(11), .7f);
    // A valid model can contain velocities below the normal editor minimum.
    source.notes[0].velocity = .0001f;
    PianoRollSelection selected; selected.selectAll(source);
    PianoRollClipboard clipboard; clipboard.copy(source, selected);
    PianoRollPattern target;
    const auto pasted = clipboard.paste(target, target.fromTicks(100));
    require(pasted.changed() && target.valid() && target.toTicks(target.notes[0].startStep) == 100
        && target.toTicks(target.notes[1].startStep) == 104
        && target.toTicks(target.notes[1].lengthSteps) == 11
        && target.notes[0].velocity == .0001f && target.notes[0].pitch == 0 && target.notes[1].pitch == 127,
        "paste preserves one-tick precision and the full valid velocity/pitch range");
    const auto original = target;
    for (int i = 0; i < 30; ++i) {
        const auto result = clipboard.paste(target, target.fromTicks(100 + i * 20));
        require(result.changed() && target.toTicks(target.find(result.selection.ids[1])->startStep) == 104 + i * 20,
                "repeated fractional paste does not accumulate rounding drift");
    }
    require(sameNote(target.notes[0], original.notes[0]), "repeated paste does not alter an earlier copy");
}

static void failureTests() {
    PianoRollPattern source;
    source.add(60, 2, 2); source.add(64, 4, 1);
    PianoRollSelection selected; selected.selectAll(source);
    PianoRollClipboard clipboard; clipboard.copy(source, selected);
    PianoRollPattern target;
    const auto existing = target.add(48, 0, 1);
    const auto original = target;
    require(clipboard.paste(target, 62).status == ClipboardStatus::outOfBounds
        && samePianoRollPattern(target, original), "end overflow rejects the entire paste without truncation");
    require(clipboard.paste(target, 64).status == ClipboardStatus::outOfBounds
        && clipboard.paste(target, -1).status == ClipboardStatus::outOfBounds,
        "paste cannot start beyond either clip boundary");
    require(clipboard.paste(target, .001).status == ClipboardStatus::invalidInput
        && clipboard.paste(target, std::numeric_limits<double>::infinity()).status == ClipboardStatus::invalidInput
        && clipboard.paste(target, std::numeric_limits<double>::quiet_NaN()).status == ClipboardStatus::invalidInput,
        "paste rejects non-tick-aligned and non-finite insertion positions");
    auto expected = original;
    require(target.add(70, 8, 1) == expected.add(70, 8, 1), "failed paste does not consume future IDs");
    require(target.find(existing), "failure retains existing IDs");

    PianoRollPattern full;
    for (std::size_t i = 0; i < full.maxNotes - 1; ++i) full.add(static_cast<int>(i), 0, 1);
    const auto fullBefore = full;
    require(clipboard.paste(full, 8).status == ClipboardStatus::capacityExceeded
        && samePianoRollPattern(full, fullBefore), "capacity preflight rejects all notes before any mutation");
    auto future = fullBefore;
    require(full.add(90, 6, 1) == future.add(90, 6, 1), "capacity failure does not advance ID allocation");

    // An exact end-boundary fit succeeds without changing any durations.
    PianoRollPattern exact;
    const auto result = clipboard.paste(exact, 61);
    require(result.changed() && exact.valid() && result.endStep == 64
        && exact.notes[1].startStep == 63 && exact.notes[1].lengthSteps == 1,
        "paste accepts exact clip-end fits");

    PianoRollSelection none;
    require(clipboard.copy(source, none).status == ClipboardStatus::emptySelection && clipboard.size() == 2,
            "copy with no selection preserves the previous snapshot");
    none.add(99999);
    require(clipboard.copy(source, none).status == ClipboardStatus::emptySelection && clipboard.size() == 2,
            "copy with stale IDs preserves the previous snapshot");
    none.ids[1] = none.ids[0]; none.count = 2;
    require(clipboard.copy(source, none).status == ClipboardStatus::invalidInput && clipboard.size() == 2,
            "malformed selection cannot replace copied material");
    auto malformed = source; malformed.count = malformed.maxNotes + 1;
    require(clipboard.copy(malformed, selected).status == ClipboardStatus::invalidInput
        && clipboard.paste(malformed, 0).status == ClipboardStatus::invalidInput && clipboard.size() == 2,
        "invalid clip models are rejected safely");
}

static void duplicateTests() {
    PianoRollPattern pattern;
    const auto a = pattern.add(60, 1.125, 2.25, .6f);
    const auto b = pattern.add(64, 4.5, .25, .8f);
    const auto other = pattern.add(80, 10, 1);
    PianoRollSelection selected; selected.add(a); selected.add(b);
    const auto original = pattern;
    const auto duplicated = duplicatePianoRollSelection(pattern, selected, 1);
    require(duplicated.changed() && duplicated.startStep == 5 && duplicated.endStep == 8.625
        && duplicated.selection.count == 2 && pattern.count == 5 && pattern.valid(),
        "duplicate begins on the first snap boundary at or after latest selected end");
    require(pattern.find(duplicated.selection.ids[0])->startStep == 5
        && pattern.find(duplicated.selection.ids[0])->lengthSteps == 2.25
        && pattern.find(duplicated.selection.ids[1])->startStep == 8.375
        && pattern.find(duplicated.selection.ids[1])->lengthSteps == .25,
        "duplicate preserves fractional relative offsets and durations");
    for (std::size_t i = 0; i < original.count; ++i)
        require(sameNote(pattern.notes[i], original.notes[i]), "duplicate preserves originals and unselected notes");
    require(!duplicated.selection.contains(a) && !duplicated.selection.contains(b)
        && !duplicated.selection.contains(other), "duplicate selects only fresh notes");

    auto eighths = original;
    const auto eighthResult = duplicatePianoRollSelection(eighths, selected, 2);
    require(eighthResult.changed() && eighthResult.startStep == 6, "eighth-note snap rounds duplicate insertion up");
    auto quarters = original;
    const auto quarterResult = duplicatePianoRollSelection(quarters, selected, 4);
    require(quarterResult.changed() && quarterResult.startStep == 8, "quarter-note snap rounds duplicate insertion up");
    PianoRollPattern exact;
    exact.add(60, 2, 2); selected.selectAll(exact);
    require(duplicatePianoRollSelection(exact, selected, 4).startStep == 4,
            "an end already on the snap boundary does not add a silent extra grid cell");
    PianoRollPattern tiny;
    tiny.add(60, 0, tiny.timeQuantum); selected.selectAll(tiny);
    require(duplicatePianoRollSelection(tiny, selected, 1).startStep == 1,
            "a sub-grid selection advances by at least the next grid boundary");

    PianoRollClipboard saved;
    selected.clear(); selected.add(a); saved.copy(original, selected);
    auto independent = original;
    selected.clear(); selected.add(b);
    require(duplicatePianoRollSelection(independent, selected, 1).changed(), "duplicate operation succeeds independently");
    PianoRollPattern pasted;
    require(saved.paste(pasted, 0).changed() && pasted.count == 1 && pasted.notes[0].pitch == 60,
            "duplicate does not replace previously copied material");
}

static void duplicateFailureTests() {
    PianoRollPattern edge;
    edge.add(60, 62, 2); PianoRollSelection selected; selected.selectAll(edge);
    const auto original = edge;
    require(duplicatePianoRollSelection(edge, selected, 1).status == ClipboardStatus::outOfBounds
        && samePianoRollPattern(edge, original), "duplicate beyond clip end is rejected without wrapping");
    auto expected = original;
    require(edge.add(70, 4, 1) == expected.add(70, 4, 1), "failed duplicate does not consume future IDs");
    const auto before = edge;
    require(duplicatePianoRollSelection(edge, selected, 0).status == ClipboardStatus::invalidInput
        && duplicatePianoRollSelection(edge, selected, .001).status == ClipboardStatus::invalidInput
        && duplicatePianoRollSelection(edge, selected, std::numeric_limits<double>::infinity()).status
            == ClipboardStatus::invalidInput && samePianoRollPattern(edge, before),
        "duplicate rejects invalid grids without changing the clip");
    selected.clear();
    require(duplicatePianoRollSelection(edge, selected).status == ClipboardStatus::emptySelection,
            "duplicate with empty selection is a no-op");
    selected.add(99999);
    require(duplicatePianoRollSelection(edge, selected).status == ClipboardStatus::emptySelection,
            "duplicate with only stale IDs is a no-op");
    PianoRollPattern full;
    for (std::size_t i = 0; i < full.maxNotes; ++i) full.add(static_cast<int>(i), 0, 1);
    selected.clear(); selected.add(full.notes[0].id);
    const auto fullBefore = full;
    require(duplicatePianoRollSelection(full, selected).status == ClipboardStatus::capacityExceeded
        && samePianoRollPattern(full, fullBefore), "full clip rejects duplicate atomically");
}

int main() {
    copyPasteTests(); exactTickAndVelocityTests(); failureTests(); duplicateTests(); duplicateFailureTests();
    std::cout << "Piano-roll clipboard tests passed\n";
}
