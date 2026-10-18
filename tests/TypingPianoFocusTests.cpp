#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "CompactWorkspace.h"
#include "SessionClipButton.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }
#include "MidiInputPanelTests.h"
struct Counter : juce::MidiKeyboardState::Listener {
    int ons{}, offs{};
    void handleNoteOn(juce::MidiKeyboardState*, int, int, float) override { ++ons; }
    void handleNoteOff(juce::MidiKeyboardState*, int, int, float) override { ++offs; }
};

static void checkPianoRollEditor() {
    juce::Component host;
    std::vector<std::pair<int, float>> audition;
    alien::PianoRollEditor roll;
    juce::TextButton nextControl { "Next control" };
    host.setBounds(0, 0, 1050, 510);
    host.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    host.addAndMakeVisible(roll);
    host.addAndMakeVisible(nextControl);
    roll.setBounds(0, 0, 1050, 460);
    roll.setExplicitFocusOrder(1);
    nextControl.setBounds(10, 470, 140, 30);
    nextControl.setExplicitFocusOrder(2);
    host.addToDesktop(0);
    host.setVisible(true);
    host.getPeer()->handleFocusGain();
    roll.grabKeyboardFocus();
    pump();
    require(roll.hasKeyboardFocus(false), "piano roll must accept editor focus");

    // Public mouse gestures use actual editor coordinates, independently of
    // model mutation tests. At this size the 64-step grid has 15 pixels/step.
    constexpr float gridX = 64.0f, gridY = 68.0f, stepWidth = 15.0f, rowHeight = 266.0f / 24.0f;
    auto point = [](int pitch, float step) {
        return juce::Point<float>(gridX + step * stepWidth,
                                 gridY + (71 - pitch + 0.5f) * rowHeight);
    };
    auto event = [&](juce::Point<float> p, bool dragged = false,
                     juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier) {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods,
                               1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &roll, &roll,
                               now, p, now, 2, dragged);
    };
    int publications = 0;
    roll.onChange = [&] { ++publications; };
    roll.onAudition = [&](int pitch, float velocity) { audition.emplace_back(pitch, velocity); };
    roll.mouseDown(event(point(60, 8.5f)));
    roll.mouseUp(event(point(60, 8.5f)));
    require(roll.getPattern().count == 1, "one left-click must create a piano roll note");
    const auto firstId = roll.getPattern().notes[0].id;
    auto first = [&]() -> const alien::PianoRollNote& { return *roll.getPattern().find(firstId); };
    require(first().pitch == 60 && first().startStep == 8 && first().lengthSteps == 1,
            "one left-click must use the clicked pitch and sixteenth grid");
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && first().startStep == 9,
            "focused piano roll arrow must move the selected note in time");
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && first().pitch == 61,
            "focused piano roll arrow must move the selected note in pitch");
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0))
            && first().lengthSteps == 2, "Shift Right must resize the selected note");
    require(!roll.keyPressed(juce::KeyPress(juce::KeyPress::tabKey))
            && !roll.keyPressed(juce::KeyPress('A')) && !roll.keyPressed(juce::KeyPress('X')),
            "piano roll must leave Tab and computer piano letters unhandled");
    require(!roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::ctrlModifier, 0))
            && first().startStep == 9, "piano roll must leave modified shortcuts unhandled");

    host.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey));
    pump();
    require(!roll.hasKeyboardFocus(false) && roll.hasKeyboardFocus(true),
            "Tab must move from the piano roll into its accessible toolbar controls");
    require(!roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && first().startStep == 9,
            "toolbar arrow navigation must not edit the selected piano roll note");

    juce::Slider* selectedVelocity = nullptr;
    juce::ComboBox* snap = nullptr;
    for (auto* child : roll.getChildren()) {
        if (child->getName() == "Selected note velocity") selectedVelocity = dynamic_cast<juce::Slider*>(child);
        if (child->getName() == "Piano roll grid snap") snap = dynamic_cast<juce::ComboBox*>(child);
    }
    require(selectedVelocity != nullptr && selectedVelocity->isEnabled() && snap != nullptr,
            "piano roll toolbar controls must have accessible names");
    selectedVelocity->setValue(32, juce::sendNotificationSync);
    require(std::abs(first().velocity - 32.0f / 127.0f) < 0.00001f,
            "selected velocity control must publish the edited note velocity");
    roll.grabKeyboardFocus();
    pump();
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && roll.getPattern().count == 0,
            "Delete must erase only the selected piano roll note");

    snap->setSelectedId(2, juce::sendNotificationSync);
    roll.mouseDown(event(point(60, 9.5f)));
    roll.mouseUp(event(point(60, 9.5f)));
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].startStep == 8
            && roll.getPattern().notes[0].lengthSteps == 2,
            "eighth-note snap must quantize both note start and initial length");

    alien::PianoRollPattern dragPattern;
    const auto dragId = dragPattern.add(60, 8, 4, 0.8f);
    roll.setPattern(dragPattern);
    roll.mouseDown(event(point(60, 8.5f)));
    roll.mouseDrag(event(point(61, 10.5f), true));
    roll.mouseUp(event(point(61, 10.5f)));
    const auto* moved = roll.getPattern().find(dragId);
    require(moved != nullptr && moved->pitch == 61 && moved->startStep == 10 && moved->lengthSteps == 4,
            "mouse drag must move pitch and snapped time while preserving duration");
    auto rightEdge = point(61, 14.0f);
    rightEdge.x -= 3.0f;
    roll.mouseDown(event(rightEdge));
    roll.mouseDrag(event(rightEdge + juce::Point<float>(2.0f * stepWidth, 0.0f), true));
    roll.mouseUp(event(rightEdge));
    require(roll.getPattern().find(dragId)->lengthSteps == 6, "right-edge drag must resize the selected note");

    roll.mouseDown(event({ gridX + 10.0f * stepWidth + 3.0f, 379.0f }));
    require(std::abs(roll.getPattern().find(dragId)->velocity - 0.5f) < 0.00001f,
            "velocity lane must edit the selected stem");
    roll.mouseDrag(event({ gridX + 10.0f * stepWidth + 3.0f, 350.0f }, true));
    roll.mouseUp(event({ gridX + 10.0f * stepWidth + 3.0f, 350.0f }));
    require(roll.getPattern().find(dragId)->velocity == 1.0f, "velocity lane drag must clamp at full velocity");
    roll.mouseDown(event(point(61, 10.5f), false, juce::ModifierKeys::rightButtonModifier));
    require(roll.getPattern().count == 0, "right-click must erase the clicked note");
    roll.mouseDown(event(point(65, 20.5f)));
    roll.mouseUp(event(point(65, 20.5f)));
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].pitch == 65
            && roll.getPattern().notes[0].lengthSteps == 6 && roll.getPattern().notes[0].velocity == 1.0f,
            "new notes must inherit the last edited note length and velocity");
    roll.undo();
    require(roll.getPattern().count == 0, "one undo must remove one click-created note");
    roll.redo();
    require(roll.getPattern().count == 1, "redo must restore the click-created note and its inherited properties");
    roll.mouseDoubleClick(event(point(65, 20.5f)));
    require(roll.getPattern().count == 1, "double-click callback must not add a duplicate note");
    roll.mouseDown(event(point(65, 20.5f), false, juce::ModifierKeys::rightButtonModifier));
    require(roll.getPattern().count == 0, "inherited note remains immediately erasable");
    snap->setSelectedId(4, juce::sendNotificationSync);
    alien::PianoRollPattern boundaryPattern;
    const auto boundaryId = boundaryPattern.add(60, 8, 3, 0.8f);
    roll.setPattern(boundaryPattern);
    roll.mouseDown(event(point(60, 8.5f)));
    roll.mouseDrag(event(point(60, 80.5f), true));
    roll.mouseUp(event(point(60, 80.5f)));
    require(roll.getPattern().find(boundaryId)->startStep == 60
            && roll.getPattern().find(boundaryId)->lengthSteps == 3,
            "dragging to the loop boundary must preserve both grid snap and note duration");
    roll.mouseDown(event(point(60, 60.5f), false, juce::ModifierKeys::rightButtonModifier));
    require(roll.getPattern().count == 0, "boundary drag note must remain editable");
    require(publications >= 10, "piano roll edits must publish their pattern changes");

    // Preview ownership must close on every lifecycle path, and a held pointer
    // must not restart a note after cancellation without a fresh mouse-down.
    const auto piano = juce::Point<float>(30.0f, point(60, 0).y);
    roll.mouseDown(event(piano));
    require(audition.size() == 1 && audition.back().first == 60 && audition.back().second > 0,
            "left piano mouse-down must start an audition note");
    nextControl.grabKeyboardFocus();
    pump();
    require(audition.size() == 2 && audition.back().first == 60 && audition.back().second == 0,
            "editor focus loss must release the audition note");
    roll.mouseDrag(event(piano, true));
    require(audition.size() == 2, "held audition drag must not restart after focus loss");
    roll.mouseDown(event(piano));
    roll.setVisible(false);
    require(audition.size() == 4 && audition.back().second == 0, "hiding the piano roll must release audition");
    roll.setVisible(true);
    roll.mouseDrag(event(piano, true));
    require(audition.size() == 4, "showing a hidden roll must not restart a held audition");
    roll.mouseDown(event(piano));
    roll.releaseAudition();
    roll.mouseDrag(event(piano, true));
    require(audition.size() == 6 && audition.back().second == 0,
            "transport or preset panic must release audition and cancel held drag");
    roll.mouseDown(event(piano));
    const auto nextPianoKey = piano - juce::Point<float>(0.0f, rowHeight);
    roll.mouseDrag(event(nextPianoKey, true));
    roll.mouseUp(event(nextPianoKey));
    require(audition.size() == 10 && audition[7].first == 60 && audition[7].second == 0
            && audition[8].first == 61 && audition[8].second > 0 && audition[9].second == 0,
            "piano preview glissando and mouse-up must release each original pitch");
    roll.mouseDown(event(piano));
    roll.mouseExit(event({ -10.0f, piano.y }));
    roll.mouseDrag(event(piano, true));
    require(audition.size() == 12 && audition.back().second == 0,
            "leaving the piano roll must release audition without drag retrigger");

    juce::MouseWheelDetails wheel;
    wheel.deltaY = 1.0f;
    roll.mouseWheelMove(event(point(60, 0)), wheel);
    roll.mouseDown(event({ gridX + 0.5f * stepWidth, gridY + 0.5f * rowHeight }));
    roll.mouseUp(event({ gridX + 0.5f * stepWidth, gridY + 0.5f * rowHeight }));
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].pitch == 74,
            "mouse wheel must scroll the editable pitch range");
    host.removeFromDesktop();
    std::cout << "PASS: piano roll drawing, editing, snap, velocity, focus and safe preview release\n";
}

static void checkPianoRollProcessing() {
    juce::Component host;
    alien::PianoRollEditor roll;
    host.setBounds(0, 0, 1050, 510);
    host.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    host.addAndMakeVisible(roll);
    roll.setBounds(0, 0, 1050, 460);
    host.addToDesktop(0);
    host.setVisible(true);
    host.getPeer()->handleFocusGain();
    juce::MidiKeyboardState keyboard;
    Counter notes;
    keyboard.addListener(&notes);
    std::set<int> held;
    TypingPianoRouter router(host, keyboard, [&](int k) { return held.count(k) != 0; });
    roll.grabKeyboardFocus();
    pump();

    auto control = [&](const juce::String& name) -> juce::Component* {
        for (auto* child : roll.getChildren()) if (child->getName() == name) return child;
        require(false, "missing accessible processing control");
        return nullptr;
    };
    auto click = [&](const juce::String& name) {
        auto* button = dynamic_cast<juce::Button*>(control(name));
        require(button != nullptr && button->isShowing() && button->isEnabled(),
            "processing action must be available to users");
        button->triggerClick();
        pump();
    };
    auto setValue = [&](const juce::String& name, double value) {
        auto* slider = dynamic_cast<juce::Slider*>(control(name));
        require(slider != nullptr && slider->isShowing(), "processing parameter must be visible");
        slider->setValue(value, juce::sendNotificationSync);
    };
    auto shortcut = [&](int key, juce::ModifierKeys modifiers) {
        host.getPeer()->handleKeyPress(juce::KeyPress(key, modifiers, static_cast<juce::juce_wchar>(key)));
        pump();
    };
    constexpr float gridX = 64, gridY = 68, stepWidth = 15, rowHeight = 266.0f / 24;
    auto point = [](int pitch, float step) {
        return juce::Point<float>(gridX + step * stepWidth, gridY + (71 - pitch + 0.5f) * rowHeight);
    };
    auto event = [&](juce::Point<float> p, bool dragged = false,
                     juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier) {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods,
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &roll, &roll, now, p, now, 1, dragged);
    };
    alien::PianoRollPattern source;
    const auto low = source.add(60, 4, 4, 0.4f);
    const auto high = source.add(64, 4, 4, 0.8f);
    const auto untouched = source.add(67, 12, 4, 0.6f);
    roll.setPattern(source);
    require(!roll.canUndo(), "loading a replacement clip starts fresh history");
    roll.mouseDown(event(point(60, 4.5f)));
    roll.mouseUp(event(point(60, 4.5f)));
    roll.mouseDown(event(point(64, 4.5f), false,
        juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier));
    roll.mouseUp(event(point(64, 4.5f)));
    require(roll.getSelectedIds().size() == 2 && !roll.canUndo(),
        "Ctrl-click selects two notes without creating edit history");
    roll.mouseDown(event(point(60, 4.5f)));
    roll.mouseDrag(event(point(61, 5.5f), true));
    roll.mouseDrag(event(point(61, 6.5f), true));
    roll.mouseUp(event(point(61, 6.5f)));
    require(roll.getPattern().find(low)->startStep == 6 && roll.getPattern().find(high)->startStep == 6
        && roll.getPattern().find(low)->pitch == 61 && roll.getPattern().find(high)->pitch == 65
        && roll.getPattern().find(untouched)->startStep == 12,
        "dragging a selection preserves its intervals and leaves other notes untouched");
    shortcut('z', juce::ModifierKeys::ctrlModifier);
    require(alien::samePianoRollPattern(roll.getPattern(), source) && !roll.canUndo()
        && roll.getSelectedIds().size() == 2, "one undo restores an entire drag and its selection");
    shortcut('y', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().find(low)->startStep == 6, "Ctrl-Y redoes group drag");
    roll.undo();

    const auto shiftDrag = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier;
    roll.mouseDown(event(point(69, 3), false, shiftDrag));
    roll.mouseDrag(event(point(59, 9), true, shiftDrag));
    roll.mouseUp(event(point(59, 9), false, shiftDrag));
    require(roll.getSelectedIds().size() == 2 && !roll.canUndo(),
        "empty-grid marquee selects its enclosed chord without editing notes");
    setValue("Selected note velocity", 32);
    require(std::abs(roll.getPattern().find(low)->velocity - 32.0f / 127) < 0.00001f
        && roll.getPattern().find(high)->velocity == roll.getPattern().find(low)->velocity
        && roll.getPattern().find(untouched)->velocity == source.find(untouched)->velocity,
        "velocity slider affects only selected notes");
    roll.undo();
    require(alien::samePianoRollPattern(roll.getPattern(), source), "undo restores batch velocities");
    setValue("Selected note probability", 35);
    require(std::abs(roll.getPattern().find(low)->probability - .35f) < .00001f
        && roll.getPattern().find(high)->probability == roll.getPattern().find(low)->probability
        && roll.getPattern().find(untouched)->probability == 1.0f,
        "probability slider affects only selected notes");
    roll.undo();
    require(alien::samePianoRollPattern(roll.getPattern(), source), "probability change is one undoable edit");

    shortcut('i', juce::ModifierKeys::altModifier);
    require(roll.getPattern().find(low)->pitch == 72 && roll.getPattern().find(high)->pitch == 64,
        "Alt-I inverts the selected chord upward");
    roll.undo();
    shortcut('i', juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier);
    require(roll.getPattern().find(high)->pitch == 52 && roll.getPattern().find(low)->pitch == 60,
        "Alt-Shift-I inverts the selected chord downward");
    roll.undo();
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut('v', juce::ModifierKeys::altModifier);
    require(roll.getPattern().find(untouched)->startStep == 4 && roll.getPattern().find(low)->startStep == 12,
        "Alt-V reverses selected note extents in time");
    roll.undo();
    const std::array<std::uint32_t, 2> chordOnly{low, high};
    roll.setSelection(chordOnly);
    shortcut('a', juce::ModifierKeys::altModifier);
    setValue("Arpeggiate rate ticks", 120);
    setValue("Arpeggiate gate percent", 50);
    click("Apply piano roll processing");
    require(roll.getPattern().find(low)->startStep == 4 && roll.getPattern().find(high)->startStep == 4.5
        && roll.getPattern().find(low)->lengthSteps == .25,
        "Alt-A arpeggiates a selected chord with rate and gate controls");
    roll.undo();

    // Open from a toolbar descendant, not just from the grid itself.
    control("Selected note velocity")->grabKeyboardFocus();
    shortcut('s', juce::ModifierKeys::altModifier);
    require(roll.isProcessingPanelOpen(), "Alt-S opens Strum from a focused toolbar control");
    const auto beforeCancel = roll.getPattern();
    setValue("Strum delay ticks", 30);
    setValue("Strum velocity variation percent", 0);
    click("Cancel piano roll processing");
    require(!roll.isProcessingPanelOpen() && !roll.canUndo()
        && alien::samePianoRollPattern(beforeCancel, roll.getPattern()), "Cancel changes neither notes nor history");
    roll.grabKeyboardFocus();
    shortcut('s', juce::ModifierKeys::altModifier);
    setValue("Strum delay ticks", 30);
    setValue("Strum velocity variation percent", 0);
    click("Apply piano roll processing");
    require(!roll.isProcessingPanelOpen() && roll.getPattern().find(low)->startStep == 4
        && roll.getPattern().find(high)->startStep == 4.125
        && roll.getPattern().find(untouched)->startStep == 12, "Apply strums only the selected chord at substep timing");
    const auto strummed = roll.getPattern();
    roll.undo();
    require(alien::samePianoRollPattern(roll.getPattern(), source) && !roll.canUndo(), "Strum Apply is one undo step");
    roll.redo();
    require(alien::samePianoRollPattern(roll.getPattern(), strummed), "redo exactly restores strummed timing");

    roll.grabKeyboardFocus();
    shortcut('u', juce::ModifierKeys::altModifier);
    setValue("Chop slice ticks", 120);
    setValue("Chop gate percent", 50);
    click("Apply piano roll processing");
    require(roll.getPattern().count == 17 && roll.getSelectedIds().size() == 16
        && roll.getPattern().find(untouched)->lengthSteps == 4, "Chop selects all fragments and preserves unselected melody");
    roll.undo();
    require(alien::samePianoRollPattern(roll.getPattern(), strummed) && roll.getSelectedIds().size() == 2,
        "one Chop undo restores source notes and selection");
    roll.undo();

    auto randomize = [&] {
        roll.grabKeyboardFocus();
        shortcut('r', juce::ModifierKeys::altModifier);
        setValue("Randomize timing ticks", 30);
        setValue("Randomize velocity variation percent", 12);
        setValue("Randomize pitch semitones", 2);
        setValue("Processing random seed", 73);
        click("Apply piano roll processing");
    };
    randomize();
    const auto randomized = roll.getPattern();
    require(!alien::samePianoRollPattern(randomized, source), "Randomize changes selected notes");
    roll.undo();
    randomize();
    require(alien::samePianoRollPattern(randomized, roll.getPattern()), "same GUI settings and seed reproduce random edits after undo");

    roll.grabKeyboardFocus();
    shortcut('q', juce::ModifierKeys::altModifier);
    require(roll.isProcessingPanelOpen(), "Alt-Q opens Quantize");
    setValue("Quantize grid ticks", 240);
    setValue("Quantize strength percent", 100);
    setValue("Quantize swing percent", 25);
    shortcut(juce::KeyPress::tabKey, {});
    require(roll.hasKeyboardFocus(true), "Tab remains inside the editor's accessible processing controls");
    click("Apply piano roll processing");
    require(roll.getPattern().find(low)->startStep == 4 && roll.getPattern().find(high)->startStep == 4,
        "Quantize applies grid settings to selected randomized starts");
    roll.undo();
    require(alien::samePianoRollPattern(roll.getPattern(), randomized), "Quantize undo restores fractional timing");

    // Text fields keep their editing shortcuts; unrelated notes must not change.
    roll.grabKeyboardFocus();
    shortcut('r', juce::ModifierKeys::altModifier);
    auto* seed = dynamic_cast<juce::Slider*>(control("Processing random seed"));
    juce::Label* textBox = nullptr;
    for (auto* child : seed->getChildren()) if (auto* label = dynamic_cast<juce::Label*>(child)) textBox = label;
    require(textBox != nullptr, "seed slider has an editable numeric field");
    textBox->showEditor();
    pump();
    auto* text = textBox->getCurrentTextEditor();
    require(text != nullptr, "seed text editor opens for accessible text entry");
    text->grabKeyboardFocus();
    require(!roll.keyPressed(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 'z')),
        "piano-roll undo must not consume text editor undo");
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    require(alien::samePianoRollPattern(roll.getPattern(), randomized)
        && roll.getSelectedIds().size() == 2 && notes.ons == 0, "text shortcuts and Alt tools do not play typing notes or select the whole clip");
    textBox->hideEditor(true);
    click("Cancel piano roll processing");

    roll.grabKeyboardFocus();
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    require(roll.getSelectedIds().size() == 3, "Ctrl-A selects the entire clip outside text fields");
    shortcut(juce::KeyPress::deleteKey, {});
    require(roll.getPattern().count == 0, "Delete removes the selected group");
    shortcut('z', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 3, "group deletion is one undo step");
    held.insert('A');
    host.getPeer()->handleKeyUpOrDown(true);
    shortcut('a', {});
    require(keyboard.isNoteOn(2, 60), "typing piano works again after processing and Tab navigation");
    held.clear();
    host.getPeer()->handleKeyUpOrDown(false);
    require(!keyboard.isNoteOn(2, 60) && notes.ons == notes.offs,
        "typing note-off remains balanced after processing shortcuts");

    alien::PianoRollPattern full;
    for (int pitch = 0; pitch < 128; ++pitch) full.add(pitch, 0, 4);
    roll.setPattern(full);
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut('u', juce::ModifierKeys::altModifier);
    setValue("Chop slice ticks", 60);
    click("Apply piano roll processing");
    require(roll.isProcessingPanelOpen() && !roll.canUndo()
        && alien::samePianoRollPattern(roll.getPattern(), full),
        "over-capacity Chop stays open and leaves the complete clip and history intact");
    click("Cancel piano roll processing");
    roll.setPattern(source);
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut('q', juce::ModifierKeys::altModifier);
    setValue("Quantize grid ticks", 240);
    setValue("Quantize strength percent", 100);
    setValue("Quantize swing percent", 0);
    click("Apply piano roll processing");
    require(roll.isProcessingPanelOpen() && !roll.canUndo()
        && alien::samePianoRollPattern(roll.getPattern(), source), "no-op Apply adds no undo entry");
    shortcut(juce::KeyPress::escapeKey, {});
    require(!roll.isProcessingPanelOpen(), "Escape closes tool settings from a focused parameter");

    // Chord stamping uses the same accessible panel but creates a new selected
    // chord at the ruler cursor instead of processing an existing selection.
    alien::PianoRollPattern empty;
    roll.setPattern(empty);
    roll.setInsertionStep(8);
    std::set<int> previewHeld;
    roll.onAudition = [&](int pitch, float velocity) {
        if (velocity > 0) previewHeld.insert(pitch); else previewHeld.erase(pitch);
    };
    roll.grabKeyboardFocus();
    shortcut('c', juce::ModifierKeys::altModifier);
    require(roll.isProcessingPanelOpen(), "Alt-C opens chord stamp settings without selected notes");
    auto* chordType = dynamic_cast<juce::ComboBox*>(control("Chord stamp type"));
    auto* previewChord = dynamic_cast<juce::Button*>(control("Preview chord stamp"));
    require(chordType != nullptr && previewChord != nullptr && previewChord->isShowing(),
        "chord type and preview controls are accessible and visible");
    chordType->setSelectedId(2, juce::sendNotificationSync); // Minor.
    setValue("Chord root MIDI note", 60);
    setValue("Chord stamp length ticks", 960);
    setValue("Chord stamp velocity", 90);
    setValue("Chord inversion", 1);
    previewChord->triggerClick();
    pump();
    require(previewHeld == std::set<int>({63, 67, 72}), "preview auditions the selected minor first inversion");
    previewChord->triggerClick();
    pump();
    require(previewHeld.empty(), "turning preview off releases every chord tone");
    previewChord->triggerClick();
    click("Apply piano roll processing");
    require(previewHeld.empty() && !roll.isProcessingPanelOpen() && roll.getPattern().count == 3
        && roll.getSelectedIds().size() == 3, "Apply releases preview and selects the complete stamped chord");
    require(roll.getPattern().notes[0].pitch == 63 && roll.getPattern().notes[1].pitch == 67
        && roll.getPattern().notes[2].pitch == 72 && roll.getPattern().notes[0].startStep == 8
        && roll.getPattern().notes[0].lengthSteps == 4
        && std::abs(roll.getPattern().notes[0].velocity - 90.0f / 127.0f) < 0.00001f,
        "stamp preserves its root voicing, cursor position, length and velocity");
    roll.undo();
    require(roll.getPattern().count == 0, "one undo removes the entire stamped chord");
    roll.redo();
    require(roll.getPattern().count == 3, "redo restores the same stamped chord");

    // Scale guides change the background immediately and snap only new pitch
    // entry or deliberate pitch moves while the option is enabled.
    roll.setPattern(empty);
    roll.grabKeyboardFocus();
    shortcut('l', juce::ModifierKeys::altModifier);
    auto* scaleRoot = dynamic_cast<juce::ComboBox*>(control("Scale root note"));
    auto* scaleType = dynamic_cast<juce::ComboBox*>(control("Scale type"));
    auto* scaleHighlight = dynamic_cast<juce::Button*>(control("Highlight selected scale"));
    auto* scaleSnap = dynamic_cast<juce::Button*>(control("Snap piano roll notes to scale"));
    require(scaleRoot != nullptr && scaleType != nullptr && scaleHighlight != nullptr && scaleSnap != nullptr,
            "scale root, type, highlight and snap controls are accessible");
    scaleRoot->setSelectedId(3, juce::sendNotificationSync); // D.
    scaleType->setSelectedId(2, juce::sendNotificationSync); // Natural minor.
    if (scaleHighlight->getToggleState()) scaleHighlight->triggerClick();
    if (!scaleSnap->getToggleState()) scaleSnap->triggerClick();
    click("Apply piano roll processing"); // Done.
    const auto plainScaleImage = roll.createComponentSnapshot(roll.getLocalBounds());
    roll.grabKeyboardFocus();
    shortcut('l', juce::ModifierKeys::altModifier);
    scaleHighlight = dynamic_cast<juce::Button*>(control("Highlight selected scale"));
    scaleHighlight->triggerClick();
    click("Apply piano roll processing");
    const auto scaleImage = roll.createComponentSnapshot(roll.getLocalBounds());
    const auto tonicPixel = (point(62, 20) + juce::Point<float>(5, 0)).roundToInt();
    require(scaleImage.getPixelAt(tonicPixel.x, tonicPixel.y)
            != plainScaleImage.getPixelAt(tonicPixel.x, tonicPixel.y),
            "enabling scale highlighting visibly changes the tonic row background");
    roll.mouseDown(event(point(61, 20.5f)));
    roll.mouseUp(event(point(61, 20.5f)));
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].pitch == 60,
            "drawing with scale snap chooses the nearest D-natural-minor pitch with a stable lower tie");
    roll.mouseDown(event(point(60, 20.5f)));
    roll.mouseDrag(event(point(61, 20.5f), true));
    roll.mouseUp(event(point(61, 20.5f)));
    require(roll.getPattern().notes[0].pitch == 62, "vertical mouse movement advances to the next scale tone");
    roll.undo();
    require(roll.getPattern().notes[0].pitch == 60, "one undo restores a scale-snapped mouse move");
    require(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && roll.getPattern().notes[0].pitch == 62,
            "upward pitch movement advances to the next scale tone");
    roll.grabKeyboardFocus();
    shortcut('l', juce::ModifierKeys::altModifier);
    scaleSnap = dynamic_cast<juce::Button*>(control("Snap piano roll notes to scale"));
    require(scaleSnap->getToggleState(), "scale snap setting remains active when reopening its panel");
    scaleSnap->triggerClick();
    click("Apply piano roll processing");
    roll.mouseDown(event(point(61, 24.5f)));
    roll.mouseUp(event(point(61, 24.5f)));
    require(roll.getPattern().count == 2 && roll.getPattern().notes[1].pitch == 61,
            "disabling scale snap restores chromatic note entry without changing existing notes");

    const std::array<std::uint32_t, 1> oneNote{low};
    roll.setPattern(source);
    roll.setSelection(oneNote);
    roll.grabKeyboardFocus();
    for (int i = 0; i < 65; ++i) roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    for (int i = 0; i < 64; ++i) roll.undo();
    require(!roll.canUndo() && roll.getPattern().find(low)->pitch == 61,
        "bounded history retains the latest 64 edits");
    require(roll.canRedo(), "undo retains a redo branch");
    roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    require(!roll.canRedo(), "new editing discards the redo branch");

    // Note clipboard shortcuts are dispatched through the native peer, like
    // the processing shortcuts above, including focus in toolbar descendants.
    alien::PianoRollPattern phrase;
    const auto phraseLow = phrase.add(60, 4.125, 1.5, 0.4f);
    const auto phraseHigh = phrase.add(64, 6.625, 2.25, 0.8f);
    const auto other = phrase.add(67, 16, 4, 0.6f);
    roll.setPattern(phrase);
    auto* pitchRange = dynamic_cast<juce::ScrollBar*>(control("Piano roll pitch range"));
    require(pitchRange != nullptr, "pitch range control is available");
    pitchRange->setCurrentRange(56, 24, juce::sendNotificationSync); // Restore C3-B4 after history pitch tests.
    roll.grabKeyboardFocus();
    const auto sample = point(60, 4.5f).roundToInt();
    const auto plain = roll.createComponentSnapshot(roll.getLocalBounds()).getPixelAt(sample.x, sample.y);
    roll.mouseDown(event(point(60, 4.5f)));
    roll.mouseUp(event(point(60, 4.5f)));
    roll.mouseDown(event(point(64, 7.0f), false,
        juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier));
    roll.mouseUp(event(point(64, 7.0f)));
    require(roll.getSelectedIds().size() == 2 && !roll.canUndo(), "highlighting notes does not edit the clip");
    const auto highlighted = roll.createComponentSnapshot(roll.getLocalBounds()).getPixelAt(sample.x, sample.y);
    require(plain != highlighted, "selected notes must visibly highlight in the grid");
    int clipboardPublications = 0;
    roll.onChange = [&] { ++clipboardPublications; };
    control("Selected note velocity")->grabKeyboardFocus();
    shortcut('c', juce::ModifierKeys::ctrlModifier);
    require(clipboardPublications == 0 && !roll.canUndo()
        && alien::samePianoRollPattern(roll.getPattern(), phrase), "Copy changes neither notes, history nor audio snapshot");
    // The ruler positions Paste without discarding highlighted source notes.
    roll.mouseDown(event({gridX + 12.3f * stepWidth, 54}));
    roll.mouseUp(event({gridX + 12.3f * stepWidth, 54}));
    require(roll.getInsertionStep() == 12 && roll.getSelectedIds().size() == 2,
        "ruler click snaps the insertion cursor without changing selection");
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 5 && roll.getSelectedIds().size() == 2
        && clipboardPublications == 1 && roll.getInsertionStep() == 17, "Paste selects new notes and advances to the next phrase boundary");
    const auto pastedLow = roll.getSelectedIds()[0], pastedHigh = roll.getSelectedIds()[1];
    require(pastedLow != phraseLow && pastedHigh != phraseHigh
        && roll.getPattern().find(pastedLow)->startStep == 12
        && roll.getPattern().find(pastedHigh)->startStep == 14.5
        && roll.getPattern().find(pastedLow)->lengthSteps == 1.5
        && roll.getPattern().find(pastedHigh)->velocity == 0.8f
        && roll.getPattern().find(other)->startStep == 16, "Paste preserves exact timing, duration, velocity and unselected notes with new IDs");
    const auto pastedPattern = roll.getPattern();
    shortcut('z', juce::ModifierKeys::ctrlModifier);
    require(alien::samePianoRollPattern(roll.getPattern(), phrase) && !roll.canUndo()
        && roll.getInsertionStep() == 12 && roll.getSelectedIds().size() == 2, "one undo restores pre-paste notes, selection and cursor");
    shortcut('y', juce::ModifierKeys::ctrlModifier);
    require(alien::samePianoRollPattern(roll.getPattern(), pastedPattern)
        && roll.getInsertionStep() == 17, "redo restores the same pasted IDs and cursor");
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 7 && roll.getPattern().find(roll.getSelectedIds()[0])->startStep == 17
        && roll.getInsertionStep() == 22, "repeated Paste advances and creates another distinct phrase");
    shortcut('z', juce::ModifierKeys::ctrlModifier);
    shortcut('d', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 7 && roll.getPattern().find(roll.getSelectedIds()[0])->startStep == 17
        && roll.getPattern().find(roll.getSelectedIds()[1])->startStep == 19.5,
        "Duplicate starts after the selected phrase and preserves internal spacing");
    shortcut('z', juce::ModifierKeys::ctrlModifier);
    require(alien::samePianoRollPattern(roll.getPattern(), pastedPattern), "Duplicate is one undo step");

    const std::array<std::uint32_t, 1> otherOnly{other};
    roll.setSelection(otherOnly);
    shortcut('d', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 6 && roll.getPattern().find(roll.getSelectedIds()[0])->startStep == 20,
        "Duplicate uses the current selection rather than copied material");
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 8 && roll.getSelectedIds().size() == 2
        && roll.getPattern().find(roll.getSelectedIds()[0])->pitch == 60
        && roll.getPattern().find(roll.getSelectedIds()[0])->startStep == 24,
        "Duplicate leaves the previous Copy available for Paste");

    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut(juce::KeyPress::deleteKey, {});
    shortcut('c', juce::ModifierKeys::ctrlModifier); // Empty copy retains the last phrase.
    roll.setInsertionStep(0);
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(roll.getPattern().count == 2 && roll.getPattern().find(roll.getSelectedIds()[0])->startStep == 0
        && roll.getPattern().find(roll.getSelectedIds()[1])->startStep == 2.5,
        "copied notes survive deleting originals and an empty-selection Copy");

    // Text editing must use the OS text clipboard, independent of note Copy.
    shortcut('r', juce::ModifierKeys::altModifier);
    textBox->showEditor();
    pump();
    text = textBox->getCurrentTextEditor();
    require(text != nullptr, "numeric field opens for clipboard focus test");
    const auto previousTextClipboard = juce::SystemClipboard::getTextFromClipboard();
    text->setText("12345");
    text->grabKeyboardFocus();
    const auto beforeTextClipboard = roll.getPattern();
    require(!roll.keyPressed(juce::KeyPress('c', juce::ModifierKeys::ctrlModifier, 'c'))
        && !roll.keyPressed(juce::KeyPress('v', juce::ModifierKeys::ctrlModifier, 'v'))
        && !roll.keyPressed(juce::KeyPress('d', juce::ModifierKeys::ctrlModifier, 'd')),
        "note actions must leave text-field Copy, Paste and Duplicate shortcuts unhandled");
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut('c', juce::ModifierKeys::ctrlModifier);
    require(juce::SystemClipboard::getTextFromClipboard() == "12345", "numeric text Copy retains normal OS clipboard behavior");
    text->setText("0");
    text->grabKeyboardFocus();
    shortcut('a', juce::ModifierKeys::ctrlModifier);
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(text->getText() == "12345" && alien::samePianoRollPattern(beforeTextClipboard, roll.getPattern()),
        "numeric text Paste must not insert piano-roll notes");
    textBox->hideEditor(true);
    click("Cancel piano roll processing");
    juce::SystemClipboard::copyTextToClipboard(previousTextClipboard);
    roll.setPattern(full);
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(!roll.canUndo() && alien::samePianoRollPattern(roll.getPattern(), full), "capacity failure leaves all notes and history untouched");
    roll.setPattern(phrase);
    roll.setInsertionStep(63);
    shortcut('v', juce::ModifierKeys::ctrlModifier);
    require(!roll.canUndo() && roll.getInsertionStep() == 63
        && alien::samePianoRollPattern(roll.getPattern(), phrase), "loop-edge failure does not truncate notes or move the cursor");
    auto* editMenu = dynamic_cast<juce::Button*>(control("Piano roll edit actions"));
    require(editMenu != nullptr && editMenu->getWantsKeyboardFocus(), "Edit menu is accessible by keyboard");
    editMenu->triggerClick();
    pump();
    require(juce::Component::getNumCurrentlyModalComponents() > 0, "Edit button opens an actual actions popup");
    juce::PopupMenu::dismissAllActiveMenus();
    pump();
    roll.onChange = {};
    roll.setSize(914, 334); // Piano-roll content inside the smallest app window.
    for (const auto* name : {"Undo piano roll edit", "Redo piano roll edit", "Open piano roll processing settings",
                             "Piano roll edit actions", "Selected note velocity", "Selected note probability"})
        require(roll.getLocalBounds().contains(control(name)->getBounds()), "toolbar actions fit the minimum window width");
    shortcut('r', juce::ModifierKeys::altModifier);
    for (const auto* name : {"Apply piano roll processing", "Cancel piano roll processing", "Randomize pitch semitones"})
        require(roll.getLocalBounds().contains(control(name)->getBounds()), "processing controls fit the minimum window size");
    shortcut(juce::KeyPress::escapeKey, {});
    shortcut('a', juce::ModifierKeys::altModifier);
    for (const auto* name : {"Arpeggiate rate ticks", "Arpeggiate gate percent", "Apply piano roll processing"})
        require(control(name)->isShowing() && roll.getLocalBounds().contains(control(name)->getBounds()),
                "arpeggiate controls fit the minimum window size");
    shortcut(juce::KeyPress::escapeKey, {});
    shortcut('c', juce::ModifierKeys::altModifier);
    for (const auto* name : {"Chord stamp type", "Chord root MIDI note", "Chord inversion", "Preview chord stamp",
                             "Apply piano roll processing", "Cancel piano roll processing"})
        require(control(name)->isShowing() && roll.getLocalBounds().contains(control(name)->getBounds()),
                "chord stamp controls fit the minimum window size");
    shortcut(juce::KeyPress::escapeKey, {});
    shortcut('l', juce::ModifierKeys::altModifier);
    for (const auto* name : {"Scale root note", "Scale type", "Highlight selected scale",
                             "Snap piano roll notes to scale", "Apply piano roll processing"})
        require(control(name)->isShowing() && roll.getLocalBounds().contains(control(name)->getBounds()),
                "scale guide controls fit the minimum window size");
    shortcut(juce::KeyPress::escapeKey, {});
    keyboard.removeListener(&notes);
    host.removeFromDesktop();
    std::cout << "PASS: selection, batch edits, gesture undo, processing settings and shortcut focus\n";
}

static void checkCompactWorkspace() {
    juce::Component host, editor, devices;
    juce::TextButton editorControl{"Editor control"}, deviceControl{"Device control"};
    editor.addAndMakeVisible(editorControl);
    devices.addAndMakeVisible(deviceControl);
    editorControl.setBounds(10, 10, 140, 30);
    deviceControl.setBounds(10, 10, 140, 30);
    alien::CompactWorkspace workspace(editor, devices);
    host.setBounds(0, 0, 700, 420);
    host.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    host.addAndMakeVisible(workspace);
    workspace.setBounds(host.getLocalBounds());
    host.addToDesktop(0);
    host.setVisible(true);
    host.getPeer()->handleFocusGain();
    int changes = 0;
    bool latestDeviceView = false;
    workspace.onViewChanged = [&](bool deviceView) { ++changes; latestDeviceView = deviceView; };

    editorControl.grabKeyboardFocus();
    pump();
    require(editor.isShowing() && !devices.isVisible(), "compact workspace starts in editor view");
    host.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey));
    pump();
    require(editor.isShowing() && !devices.isVisible(), "ordinary Tab does not change workspace view");
    host.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey,
                                                 juce::ModifierKeys::shiftModifier, 0));
    pump();
    require(!editor.isVisible() && devices.isShowing() && workspace.isDeviceView()
            && changes == 1 && latestDeviceView,
            "Shift-Tab switches to devices and publishes one release callback");

    juce::Button* toggle = nullptr;
    for (auto* child : workspace.getChildren())
        if (child->getName() == "Workspace view switch") toggle = dynamic_cast<juce::Button*>(child);
    require(toggle != nullptr && toggle->isShowing() && toggle->getButtonText() == "Show editor",
            "workspace switch is visible, named and describes its next action");
    toggle->triggerClick();
    pump();
    require(editor.isShowing() && !devices.isVisible() && !workspace.isDeviceView()
            && changes == 2 && !latestDeviceView,
            "on-screen switch returns to the editor and publishes the view change");

    host.removeFromDesktop();
    std::cout << "PASS: compact editor/device workspace shortcut, focus and accessible switch\n";
}

static void checkSessionClipButton() {
    alien::SessionClipButton button;
    int launches = 0, edits = 0;
    button.onClick = [&] { ++launches; };
    button.onRightClick = [&] { ++edits; };
    const auto now = juce::Time::getCurrentTime();
    auto event = [&](juce::ModifierKeys modifiers) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {10.0f, 10.0f}, modifiers,
                                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &button, &button,
                                now, {10.0f, 10.0f}, now, 1, false);
    };
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
    require(launches == 1 && edits == 0, "left-click launches a Session clip");
    button.mouseUp(event(juce::ModifierKeys::rightButtonModifier));
    pump();
    require(launches == 1 && edits == 1, "right-click edits without launching a Session clip");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    juce::Component shell, root, piano, outside;
    juce::TextButton play{"Play"}, octaveUp{"Octave Up"};
    juce::TextEditor editor;
    juce::ComboBox presets;
    root.setBounds(0, 0, 600, 300);
    outside.setBounds(650, 0, 100, 100);
    outside.setWantsKeyboardFocus(true);
    outside.addToDesktop(0);
    outside.setVisible(true);
    int index = 1;
    for (auto* c : std::initializer_list<juce::Component*>{&piano, &play, &octaveUp, &editor, &presets}) {
        root.addAndMakeVisible(c);
        c->setBounds(10, index * 40, 200, 30);
        c->setWantsKeyboardFocus(true);
        c->setExplicitFocusOrder(index++);
    }
    shell.setBounds(0, 0, 600, 300);
    shell.setWantsKeyboardFocus(true);
    shell.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    shell.addAndMakeVisible(root);
    shell.addToDesktop(0);
    shell.setVisible(true);
    shell.getPeer()->handleFocusGain();
    juce::MidiKeyboardState state;
    Counter count;
    state.addListener(&count);
    std::set<int> held;
    auto focus = [&](juce::Component& c) { c.grabKeyboardFocus(); pump(); require(c.hasKeyboardFocus(false), "focus failed"); };
    auto press = [&](int k, juce::ModifierKeys mods = {}) {
        held.insert(k);
        root.getPeer()->handleKeyUpOrDown(true);
        const int nativeCode = k >= 'A' && k <= 'Z' ? k - 'A' + 'a' : k;
        root.getPeer()->handleKeyPress(juce::KeyPress(nativeCode, mods, static_cast<juce::juce_wchar>(nativeCode)));
        pump();
    };
    auto lift = [&](int k) { held.erase(k); root.getPeer()->handleKeyUpOrDown(false); pump(); };
    {
        TypingPianoRouter router(root, state, [&](int k) { return held.count(k) != 0; });
        octaveUp.onClick = [&] { router.typing.shift(1); };
        focus(shell);
        press('A'); require(state.isNoteOn(2, 60), "notes with native window itself focused"); lift('A');
        shell.setWantsKeyboardFocus(false); // like a DocumentWindow, only content joins Tab order
        const int initialOns = count.ons;
        focus(piano);
        press('A'); press('A');
        root.getPeer()->handleKeyPress(juce::KeyPress('A')); // upper-case peer codes too
        pump();
        require(state.isNoteOn(2, 60) && count.ons == initialOns + 1, "initial note/repeat suppression");
        root.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey)); pump();
        require(play.hasKeyboardFocus(false), "Tab must traverse, not be swallowed");
        require(!state.isNoteOn(2, 60), "Tab must release old held notes");
        press('A'); require(count.ons == initialOns + 1, "held key repeat after focus change must not restart"); lift('A');
        press('A'); require(state.isNoteOn(2, 60), "piano input must recover on ordinary control after Tab"); lift('A');
        root.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0)); pump();
        require(piano.hasKeyboardFocus(false), "Shift-Tab must traverse backwards");
        focus(octaveUp);
        const int before = router.typing.octave();
        root.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::returnKey)); pump();
        require(router.typing.octave() == before + 1 && octaveUp.hasKeyboardFocus(false), "accessible octave activation must preserve focus");
        press('A'); require(state.isNoteOn(2, 72), "notes after octave activation"); lift('A');
        press('X'); press('X'); require(router.typing.octave() == before + 2, "octave repeat suppression"); lift('X');
        focus(editor);
        const int onBeforeText = count.ons;
        press('A'); lift('A'); press('X'); lift('X');
        require(count.ons == onBeforeText && editor.getText().containsIgnoreCase("a"), "editor must retain typing without notes");
        focus(presets); press('A'); lift('A'); require(count.ons == onBeforeText, "combo typeahead must not play");
        focus(play);
        press('A', juce::ModifierKeys::ctrlModifier); lift('A');
        require(count.ons == onBeforeText, "modified shortcuts must not play");
        press('A'); press('D'); require(state.isNoteOn(2, 84) && state.isNoteOn(2, 88), "chord after editor recovery");
        // Move focus out of the window, then back, while physical keys remain down.
        focus(outside); require(!state.isNoteOnForChannels(2, 84) && !state.isNoteOn(2, 88), "blur releases chord");
        focus(play);
        press('A'); require(!state.isNoteOn(2, 84), "blurred held key must not retrigger");
        lift('A'); lift('D');
        press('A'); lift('A');
        require(count.ons == count.offs, "every note-on must have one note-off");
    }
    // Destroy/recreate must detach the old listener, not duplicate deliveries.
    {
        TypingPianoRouter router(root, state, [&](int k) { return held.count(k) != 0; });
        shell.removeChildComponent(&root);
        shell.addAndMakeVisible(root); // production content attachment/reparenting path
        pump();
        focus(play); const int before = count.ons;
        press('A'); require(count.ons == before + 1, "listener lifecycle duplicates handlers"); lift('A');
    }
    state.removeListener(&count);
    shell.removeFromDesktop();
    outside.removeFromDesktop();
    checkPianoRollEditor();
    checkPianoRollProcessing();
    checkCompactWorkspace();
    checkSessionClipButton();
    checkMidiInputPanel();
    std::cout << "PASS: Tab routing, editor exclusion, octave recovery, release and listener lifecycle\n";
}
