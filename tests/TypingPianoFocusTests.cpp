#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }
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
    roll.mouseDoubleClick(event(point(60, 8.5f)));
    require(roll.getPattern().count == 1, "double-click must create a piano roll note");
    const auto firstId = roll.getPattern().notes[0].id;
    auto first = [&]() -> const alien::PianoRollNote& { return *roll.getPattern().find(firstId); };
    require(first().pitch == 60 && first().startStep == 8 && first().lengthSteps == 1,
            "double-click must use the clicked pitch and sixteenth grid");
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
    roll.mouseDoubleClick(event(point(60, 9.5f)));
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
    roll.mouseDoubleClick(event({ gridX + 0.5f * stepWidth, gridY + 0.5f * rowHeight }));
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].pitch == 74,
            "mouse wheel must scroll the editable pitch range");
    host.removeFromDesktop();
    std::cout << "PASS: piano roll drawing, editing, snap, velocity, focus and safe preview release\n";
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
    std::cout << "PASS: Tab routing, editor exclusion, octave recovery, release and listener lifecycle\n";
}
