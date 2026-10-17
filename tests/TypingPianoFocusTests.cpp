#include "TypingPianoRouter.h"
#include <cstdlib>
#include <iostream>
#include <set>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }
struct Counter : juce::MidiKeyboardState::Listener {
    int ons{}, offs{};
    void handleNoteOn(juce::MidiKeyboardState*, int, int, float) override { ++ons; }
    void handleNoteOff(juce::MidiKeyboardState*, int, int, float) override { ++offs; }
};
int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    juce::Component root, piano, outside;
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
    root.addToDesktop(0);
    root.setVisible(true);
    root.getPeer()->handleFocusGain();
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
        focus(piano);
        press('A'); press('A');
        root.getPeer()->handleKeyPress(juce::KeyPress('A')); // upper-case peer codes too
        pump();
        require(state.isNoteOn(2, 60) && count.ons == 1, "initial note/repeat suppression");
        root.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey)); pump();
        require(play.hasKeyboardFocus(false), "Tab must traverse, not be swallowed");
        require(!state.isNoteOn(2, 60), "Tab must release old held notes");
        press('A'); require(count.ons == 1, "held key repeat after focus change must not restart"); lift('A');
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
        focus(play); const int before = count.ons;
        press('A'); require(count.ons == before + 1, "listener lifecycle duplicates handlers"); lift('A');
    }
    state.removeListener(&count);
    root.removeFromDesktop();
    outside.removeFromDesktop();
    std::cout << "PASS: Tab routing, editor exclusion, octave recovery, release and listener lifecycle\n";
}
