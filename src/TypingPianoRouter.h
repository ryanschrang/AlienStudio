#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "TypingPiano.h"
#include <functional>
#include <vector>
#include <algorithm>

// One window-scoped message-thread listener routes unhandled keys. Editable controls
// keep their typing keys; Tab/Shift-Tab and other navigation are never consumed.
class TypingPianoRouter final : private juce::KeyListener, private juce::FocusChangeListener, private juce::ComponentListener {
public:
    using KeyDown = std::function<bool(int)>;
    TypingPianoRouter(juce::Component& owner, juce::MidiKeyboardState& notes,
                      KeyDown keyDown = juce::KeyPress::isKeyCurrentlyDown)
        : root(owner), state(notes), isDown(std::move(keyDown)) {
        root.addComponentListener(this);
        attachToWindow();
        juce::Desktop::getInstance().addFocusChangeListener(this);
        globalFocusChanged(juce::Component::getCurrentlyFocusedComponent());
    }
    ~TypingPianoRouter() override {
        juce::Desktop::getInstance().removeFocusChangeListener(this);
        root.removeComponentListener(this);
        if (eventSource != nullptr) eventSource->removeKeyListener(this);
        for(auto& window:auxiliaryRoots)if(window)window->removeKeyListener(this);
        release();
    }
    TypingPiano typing;
    std::function<void()> octaveChanged;
    void addAuxiliaryRoot(juce::Component& window) {
        for(const auto& root:auxiliaryRoots)if(root.getComponent()==&window)return;
        auxiliaryRoots.emplace_back(&window);window.addKeyListener(this);
    }
    void removeAuxiliaryRoot(juce::Component& window) {
        window.removeKeyListener(this);
        if(target==&window||window.isParentOf(target.getComponent())) {release();target=nullptr;}
        std::erase_if(auxiliaryRoots,[&](const auto& item){return item==nullptr||item.getComponent()==&window;});
    }
    void release() {
        typing.releaseAll([this](int n, bool on) { send(n, on); });
        // A key held through blur/panic must be lifted before repeats can start it again.
        for (std::size_t i = 0; i < watched.size(); ++i) blocked[i] = isDown(watched[i]);
    }
private:
    static constexpr std::array<int, 14> watched{'A','W','S','E','D','F','T','G','Y','H','U','J','Z','X'};
    void attachToWindow() {
        auto* window = root.getTopLevelComponent();
        if (window == eventSource.getComponent()) return;
        if (eventSource != nullptr) eventSource->removeKeyListener(this);
        eventSource = window;
        window->addKeyListener(this);
    }
    void componentParentHierarchyChanged(juce::Component&) override { attachToWindow(); }
    bool accepts(juce::Component* c) const {
        if(c==nullptr||c->isCurrentlyBlockedByAnotherModalComponent())return false;
        bool owned=c==&root||c==root.getTopLevelComponent()||root.isParentOf(c);
        for(const auto& window:auxiliaryRoots)
            if(window&&(c==window.getComponent()||window->isParentOf(c)))owned=true;
        if(!owned)return false;
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
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override {
        refreshFocus();
        if (!accepts(target != nullptr ? target.getComponent() : origin) || key.getModifiers().isAnyModifierKeyDown()) return false;
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
    juce::Component::SafePointer<juce::Component> target, eventSource;
    std::vector<juce::Component::SafePointer<juce::Component>> auxiliaryRoots;
    std::array<bool, watched.size()> blocked{};
};
