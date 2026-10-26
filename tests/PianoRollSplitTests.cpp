#include "PianoRollEditor.h"
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    alien::PianoRollEditor editor;
    alien::PianoRollPattern pattern;
    const auto a = pattern.add(60, 1, 4, .6f, .7f), b = pattern.add(67, 2, 4, .8f, .9f);
    const auto other = pattern.add(72, 0, 8);
    const std::array ids{a, b};
    editor.setPattern(pattern);
    editor.setSelection(ids);
    editor.setInsertionStep(2.5);
    int commits = 0;
    editor.onEditCommitted = [&] { ++commits; };
    editor.splitSelectedNotes();
    const auto split = editor.getPattern();
    require(commits == 1 && editor.canUndo() && split.count == 5 && editor.getSelectedIds().size() == 4
        && split.find(a)->lengthSteps == 1.5 && split.find(b)->lengthSteps == .5
        && split.find(other)->lengthSteps == 8 && editor.getInsertionStep() == 2.5,
        "editor splits every selected crossing note in one commit, preserving cursor and unselected notes");
    editor.undo();
    require(!editor.canUndo() && editor.canRedo() && alien::samePianoRollPattern(editor.getPattern(), pattern)
        && editor.getSelectedIds().size() == 2 && editor.getSelectedIds()[0] == a
        && editor.getSelectedIds()[1] == b && editor.getInsertionStep() == 2.5,
        "one undo restores notes, selection and edit cursor");
    editor.redo();
    require(commits == 3 && alien::samePianoRollPattern(editor.getPattern(), split)
        && editor.getSelectedIds().size() == 4, "redo restores the same fragment IDs and selection");
    editor.splitSelectedNotes();
    require(commits == 3, "splitting an existing boundary publishes nothing");
    editor.undo();
    require(!editor.canUndo() && alien::samePianoRollPattern(editor.getPattern(), pattern),
        "boundary no-op adds no undo step");
    editor.setSelection({});
    editor.splitSelectedNotes();
    require(commits == 4 && editor.canRedo() && !editor.canUndo(), "empty split preserves redo history");
    editor.setSelection(ids);
    editor.setLocked(true);
    editor.splitSelectedNotes();
    require(commits == 4 && !editor.canUndo(), "locked editor refuses a stale split action");
    editor.setLocked(false);

    alien::PianoRollPattern full;
    for (std::size_t i = 0; i < full.maxNotes; ++i) full.add(60, 0, 2);
    editor.setPattern(full);
    const std::array one{full.notes[0].id};
    editor.setSelection(one);
    editor.setInsertionStep(1);
    editor.splitSelectedNotes();
    require(commits == 4 && !editor.canUndo() && alien::samePianoRollPattern(editor.getPattern(), full)
        && editor.getSelectedIds().size() == 1, "capacity refusal publishes nothing and leaves selection/history intact");
    std::cout << "Piano-roll split editor history tests passed\n";
}
