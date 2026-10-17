#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "TypingPiano.h"
#include <functional>

// One window-scoped message-thread listener routes unhandled keys. Editable controls
// keep their typing keys; Tab/Shift-Tab and other navigation are never consumed.
class TypingPianoRouter final : private juce::KeyListener, private juce::FocusChangeListener {
public:
    using KeyDown = std::function<bool(int)>;
    TypingPianoRouter(juce::Component& owner, juce::MidiKeyboardState& notes,
                      KeyDown keyDown = juce::KeyPress::isKeyCurrentlyDown)
        : root(owner), state(notes), isDown(std::move(keyDown)) {
        root.addKeyListener(this);
        juce::Desktop::getInstance().addFocusChangeListener(this);
        globalFocusChanged(juce::Component::getCurrentlyFocusedComponent());
    }
    ~TypingPianoRouter() override {
        juce::Desktop::getInstance().removeFocusChangeListener(this);
        root.removeKeyListener(this);
        release();
    }
    TypingPiano typing;
    std::function<void()> octaveChanged;
    void release() {
        typing.releaseAll([this](int n, bool on) { send(n, on); });
        // A key held through blur/panic must be lifted before repeats can start it again.
        for (std::size_t i = 0; i < watched.size(); ++i) blocked[i] = isDown(watched[i]);
    }
private:
    static constexpr std::array<int, 14> watched{'A','W','S','E','D','F','T','G','Y','H','U','J','Z','X'};
    bool accepts(juce::Component* c) const {
        if (c == nullptr || !(c == &root || root.isParentOf(c))
            || c->isCurrentlyBlockedByAnotherModalComponent()) return false;
        for (auto* p = c; p != nullptr; p = p->getParentComponent())
            if (dynamic_cast<juce::TextEditor*>(p) != nullptr
                || dynamic_cast<juce::ComboBox*>(p) != nullptr) return false;
        return true;
    }
    void refreshFocus() {
        auto* current = juce::Component::getCurrentlyFocusedComponent();
        if (current == target.getComponent()) return;
        release();
        target = current;
    }
    void globalFocusChanged(juce::Component*) override { refreshFocus(); }
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        refreshFocus();
        if (!accepts(target.getComponent()) || key.getModifiers().isAnyModifierKeyDown()) return false;
        const int code = key.getKeyCode();
        // Native peers may report unmodified letters as lower-case key codes.
        const int k = code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
        for (std::size_t i = 0; i < watched.size(); ++i) {
            if (!isDown(watched[i])) blocked[i] = false;
            if (watched[i] != k) continue;
            if (blocked[i]) return true;
            if (k == 'Z' || k == 'X') {
                blocked[i] = true; // suppress octave auto-repeat as well
                typing.shift(k == 'Z' ? -1 : 1);
                if (octaveChanged) octaveChanged();
                return true;
            }
            return typing.press(k, [this](int n, bool on) { send(n, on); });
        }
        return false;
    }
    bool keyStateChanged(bool, juce::Component*) override {
        refreshFocus();
        typing.releaseMissing(isDown, [this](int n, bool on) { send(n, on); });
        for (std::size_t i = 0; i < watched.size(); ++i)
            if (!isDown(watched[i])) blocked[i] = false;
        return false; // key-state observation must not block control navigation
    }
    void send(int note, bool on) {
        if (on) state.noteOn(2, note, .8f); else state.noteOff(2, note, 0);
    }
    juce::Component& root;
    juce::MidiKeyboardState& state;
    KeyDown isDown;
    juce::Component::SafePointer<juce::Component> target;
    std::array<bool, watched.size()> blocked{};
};
