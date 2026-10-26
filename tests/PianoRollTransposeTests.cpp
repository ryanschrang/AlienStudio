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
    const auto a = pattern.add(60, 0, 2), b = pattern.add(67, 0, 2);
    const std::array ids{a, b};
    editor.setPattern(pattern);
    editor.setSelection(ids);
    int commits = 0;
    editor.onEditCommitted = [&] { ++commits; };
    editor.transposeSelectedNotes(12);
    require(commits == 1 && editor.canUndo() && editor.getPattern().find(a)->pitch == 72
        && editor.getPattern().find(b)->pitch == 79, "one transpose commits every selected note once");
    editor.undo();
    require(!editor.canUndo() && editor.canRedo() && alien::samePianoRollPattern(editor.getPattern(), pattern)
        && editor.getSelectedIds().size() == 2, "one undo restores all pitches and selection");
    editor.redo();
    require(commits == 3 && editor.getPattern().find(a)->pitch == 72, "redo restores the transposition");
    const auto edited = editor.getPattern();
    editor.transposeSelectedNotes(127);
    require(commits == 3 && alien::samePianoRollPattern(editor.getPattern(), edited),
        "out-of-range transpose publishes nothing");
    editor.undo();
    require(!editor.canUndo() && alien::samePianoRollPattern(editor.getPattern(), pattern),
        "rejected transpose creates no extra undo step");
    editor.setLocked(true);
    editor.transposeSelectedNotes(1);
    require(alien::samePianoRollPattern(editor.getPattern(), pattern) && !editor.canUndo(),
        "locked editor refuses a stale menu action");
    std::cout << "Piano-roll transpose history tests passed\n";
}
