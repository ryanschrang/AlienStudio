// Piano-roll pitch rows (PROD-06, part 2): All, Used and Scale rows, scale shading shared by keys and rows, and
// drawing, dragging, auditioning and arrow keys landing on the right pitches when rows are hidden. Hiding rows never
// changes a note.
#include "PianoRollEditor.h"
#include "PianoRollPitchRows.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, p, now, 1, false);
}
// A 900 x 520 editor: 24 rows from y 68, 326 px tall; a 16-step loop over 810 px from x 64.
constexpr float rowHeight = 326.0f / 24.0f, stepWidth = 810.0f / 16.0f;
// The centre of screen row (0 is the top) at step.
static juce::Point<float> cell(int screenRow, float step) { return {64.0f + step * stepWidth, 68.0f + (screenRow + 0.5f) * rowHeight}; }
static juce::Point<float> key(int screenRow) { return {30.0f, cell(screenRow, 0).y}; }

static void rowLists() {
    std::bitset<128> used;
    require(pitchRows(PitchView::all, used, {}).count == 128, "All shows every pitch");
    require(pitchRows(PitchView::used, used, {}).count == 128, "Used with no notes shows every pitch rather than none");
    used.set(60); used.set(64); used.set(67); used.set(61);
    const auto inUse = pitchRows(PitchView::used, used, {});
    require(inUse.count == 4 && inUse.pitches[0] == 60 && inUse.pitches[3] == 67, "Used shows only the pitches notes use, lowest first");
    require(inUse.rowOf(64) == 2 && inUse.rowOf(62) == -1 && inUse.rowAtOrAbove(62) == 2, "rows are found by pitch");
    const ScaleSettings cMajor{0, ScaleType::major};
    const auto scale = pitchRows(PitchView::scale, used, cMajor);
    require(scale.count == 75 + 1 && scale.rowOf(61) >= 0 && scale.rowOf(63) == -1 && scale.rowOf(62) >= 0,
            "Scale shows the scale's notes, and a note outside the scale keeps its row");
    require(scaleRole(60, cMajor) == ScaleRole::root && scaleRole(62, cMajor) == ScaleRole::tone && scaleRole(61, cMajor) == ScaleRole::outside
            && scaleRole(57, {9, ScaleType::naturalMinor}) == ScaleRole::root, "roles follow the chosen key and scale");
    std::puts("PASS: row lists for All, Used and Scale");
}

static void views() {
    PianoRollEditor editor;
    editor.setSize(900, 520);
    PianoRollPattern p; p.loopSteps = 16;
    p.add(60, 0, 2); p.add(64, 2, 2); p.add(67, 4, 2); p.add(61, 6, 2);
    editor.setPattern(p);
    const auto before = editor.getPattern();
    require(editor.getPitchView() == PitchView::all && editor.shownPitches().size() == 24 && editor.shownPitches().back() == 48,
            "All rows start at C3, as before");

    editor.setPitchView(PitchView::used);
    require(editor.shownPitches() == std::vector<int>({67, 64, 61, 60}), "Used shows the four pitches in use");
    require(samePianoRollPattern(before, editor.getPattern()), "switching rows changes no note");

    // C major: notes keep their rows, 61 included, and keys and rows share the root and scale shading.
    editor.setScaleSettings({0, ScaleType::major, false, false});
    require(editor.keyScale(60) == ScaleRole::none, "no scale shading until it is highlighted or its rows are shown");
    editor.setPitchView(PitchView::scale);
    const auto shown = editor.shownPitches();
    const auto has = [&](int pitch) { return std::find(shown.begin(), shown.end(), pitch) != shown.end(); };
    require(has(61) && !has(63) && !has(66) && has(62) && has(60), "Scale rows: C major plus the out-of-scale 61");
    require(editor.keyScale(60) == ScaleRole::root && editor.keyScale(62) == ScaleRole::tone && editor.keyScale(61) == ScaleRole::outside,
            "root, scale and outside notes are told apart");
    // Scrolling Scale rows moves through scale notes only, and a key still plays the row it shows.
    const auto bottom = editor.shownPitches().back();
    juce::MouseWheelDetails wheel{0.0f, 1.0f, false, false, false};
    editor.mouseWheelMove(mouse(editor, cell(10, 1.0f)), wheel);
    const auto scrolled = editor.shownPitches();
    require(scrolled.size() == 24 && scrolled.back() > bottom && editor.keyScale(scrolled.back()) != ScaleRole::outside,
            "the wheel scrolls up by scale rows");
    int heard = -1;
    editor.onAudition = [&](int note, float velocity) { if (velocity > 0) heard = note; };
    editor.mouseDown(mouse(editor, key(0)));
    editor.mouseUp(mouse(editor, key(0)));
    require(heard == scrolled.front(), "after scrolling, the top key plays the top row's pitch");
    editor.onAudition = nullptr;
    // The key changes: rows follow it, and the notes still are not touched.
    editor.setScaleSettings({9, ScaleType::naturalMinor, false, false});
    require(editor.keyScale(57) == ScaleRole::root && editor.keyScale(60) == ScaleRole::tone, "A minor puts the root on A");
    require(samePianoRollPattern(before, editor.getPattern()), "nor does changing the scale");

    // An empty clip in Used rows shows every row, so there is somewhere to draw.
    editor.setPitchView(PitchView::used);
    editor.clearAllNotes();
    require(editor.shownPitches().size() == 24, "Used rows of an empty clip show every pitch");
    editor.undo();
    require(editor.shownPitches() == std::vector<int>({67, 64, 61, 60}) && samePianoRollPattern(before, editor.getPattern()),
            "undo brings back the notes and their rows");
    editor.setPitchView(PitchView::all);
    require(editor.shownPitches().size() == 24 && samePianoRollPattern(before, editor.getPattern()), "and back to All");
    std::puts("PASS: All, Used and Scale rows never change notes");
}

// With hidden rows, every gesture lands on the pitch of the row under the mouse.
static void gestures() {
    PianoRollEditor editor;
    editor.setSize(900, 520);
    PianoRollPattern p; p.loopSteps = 16;
    p.add(60, 0, 2); p.add(64, 2, 2); p.add(67, 4, 2); p.add(61, 6, 2);
    editor.setPattern(p);
    editor.setPitchView(PitchView::used);
    // Four rows at the bottom of the grid: screen rows 20-23 are 67, 64, 61, 60.
    std::vector<std::pair<int, float>> heard;
    editor.onAudition = [&](int note, float velocity) { heard.emplace_back(note, velocity); };
    editor.mouseDown(mouse(editor, key(21)));
    require(!heard.empty() && heard.back().first == 64 && editor.keyLight(64).state == PianoRollEditor::KeyState::auditioned,
            "a key plays its row's pitch");
    editor.mouseUp(mouse(editor, key(21)));

    // Drawing on the 61 row makes a 61; dragging it up two rows lands on 67, across the hidden pitches.
    editor.mouseDown(mouse(editor, cell(22, 10.2f)));
    const auto drawn = editor.getSelectedIds();
    require(drawn.size() == 1 && editor.getPattern().find(drawn[0])->pitch == 61, "drawing on a row makes that row's pitch");
    const auto id = drawn[0];
    editor.mouseDrag(mouse(editor, cell(20, 10.2f)));
    require(editor.getPattern().find(id)->pitch == 67, "dragging up two rows moves to the pitch two rows up");
    editor.mouseDrag(mouse(editor, cell(23, 10.2f)));
    editor.mouseUp(mouse(editor, cell(23, 10.2f)));
    require(editor.getPattern().find(id)->pitch == 60, "and down to the bottom row");
    editor.undo();
    require(editor.getPattern().count == 4 && editor.shownPitches() == std::vector<int>({67, 64, 61, 60}), "undo takes back the drawn note");

    // A note dragged off a pitch nothing else uses leaves that row to fold away when the drag ends.
    editor.mouseDown(mouse(editor, cell(23, 0.5f)));   // the 60
    editor.mouseDrag(mouse(editor, cell(22, 0.5f)));
    editor.mouseUp(mouse(editor, cell(22, 0.5f)));
    require(editor.getPattern().notes[0].pitch == 61, "the 60 moved up a row, to 61");
    require(editor.shownPitches() == std::vector<int>({67, 64, 61}), "the empty 60 row folds away once the drag ends");
    editor.undo();

    // The marquee selects what is drawn on the rows it covers.
    editor.mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), cell(20, 0.1f),
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier), 1.0f, 0, 0, 0, 0,
        &editor, &editor, juce::Time::getCurrentTime(), cell(20, 0.1f), juce::Time::getCurrentTime(), 1, false));
    editor.mouseDrag(mouse(editor, cell(21, 15.0f)));
    editor.mouseUp(mouse(editor, cell(21, 15.0f)));
    std::vector<int> selected;
    for (const auto chosen : editor.getSelectedIds()) selected.push_back(editor.getPattern().find(chosen)->pitch);
    std::sort(selected.begin(), selected.end());
    require(selected == std::vector<int>({64, 67}), "a marquee over two rows selects those rows' notes");
    std::puts("PASS: drawing, dragging, selecting and auditioning with hidden rows");
}

// Arrow keys step through the rows that are shown: in Scale rows, up from E is F, not a semitone that is hidden.
static void arrows() {
    juce::Component host;
    PianoRollEditor roll;
    host.setBounds(0, 0, 900, 520);
    host.addAndMakeVisible(roll);
    roll.setBounds(0, 0, 900, 520);
    host.addToDesktop(0);
    host.setVisible(true);
    host.getPeer()->handleFocusGain();
    roll.grabKeyboardFocus();
    for (int i = 0; i < 5 && !roll.hasKeyboardFocus(false); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(roll.hasKeyboardFocus(false), "the piano roll takes focus");
    PianoRollPattern p; p.loopSteps = 16;
    const auto e = p.add(64, 0, 2);
    roll.setPattern(p);
    const std::uint32_t chosen[]{e};
    roll.setSelection(chosen);
    roll.setScaleSettings({0, ScaleType::major, false, false});
    roll.setPitchView(PitchView::scale);
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && roll.getPattern().find(e)->pitch == 65, "up from E is F");
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && roll.getPattern().find(e)->pitch == 67, "up from F skips F#");
    roll.setPitchView(PitchView::all);
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && roll.getPattern().find(e)->pitch == 68, "with All rows up is a semitone");
    roll.undo(); roll.undo(); roll.undo();
    require(roll.getPattern().find(e)->pitch == 64, "each step undoes");
    host.removeFromDesktop();
    std::puts("PASS: arrow keys step through the shown rows");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    rowLists();
    views();
    gestures();
    arrows();
    return 0;
}
