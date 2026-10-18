#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace alien {

// Hosts two full-size work areas and exposes one window-level shortcut for
// changing views. Ordinary Tab remains available for accessible traversal.
class CompactWorkspace final : public juce::Component,
                               private juce::KeyListener,
                               private juce::ComponentListener {
public:
    CompactWorkspace(juce::Component& editorContent, juce::Component& deviceContent)
        : editor(editorContent), devices(deviceContent) {
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        addAndMakeVisible(editor);
        addChildComponent(devices);
        addAndMakeVisible(toggle);
        toggle.setName("Workspace view switch");
        toggle.setTooltip("Switch between the editor and devices (Shift+Tab)");
        toggle.onClick = [this] { showDevices(editor.isVisible()); };
        addComponentListener(this);
        updateToggle();
    }

    ~CompactWorkspace() override {
        removeComponentListener(this);
        if (eventSource != nullptr) eventSource->removeKeyListener(this);
    }

    bool isDeviceView() const noexcept { return devices.isVisible(); }

    void showDevices(bool shouldShow) {
        if (shouldShow == isDeviceView()) return;
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
            focused != nullptr && ((shouldShow && (focused == &editor || editor.isParentOf(focused)))
                                   || (!shouldShow && (focused == &devices || devices.isParentOf(focused)))))
            toggle.grabKeyboardFocus();
        editor.setVisible(!shouldShow);
        devices.setVisible(shouldShow);
        updateToggle();
        resized();
        if (onViewChanged) onViewChanged(shouldShow);
    }

    void resized() override {
        auto area = getLocalBounds();
        auto header = area.removeFromTop(42);
        toggle.setBounds(header.removeFromRight(150).reduced(2, 4));
        editor.setBounds(area);
        devices.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xffb9ced8));
        g.setFont(15.0f);
        g.drawText(isDeviceView() ? "Synth & MIDI devices" : "Pattern editor",
                   12, 0, getWidth() - 174, 42, juce::Justification::centredLeft);
    }

    std::function<void(bool)> onViewChanged;

private:
    void componentParentHierarchyChanged(juce::Component&) override { attachToWindow(); }

    void attachToWindow() {
        auto* window = getTopLevelComponent();
        if (window == eventSource.getComponent()) return;
        if (eventSource != nullptr) eventSource->removeKeyListener(this);
        eventSource = window;
        window->addKeyListener(this);
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        const auto modifiers = key.getModifiers();
        if (key.getKeyCode() != juce::KeyPress::tabKey || !modifiers.isShiftDown()
            || modifiers.isCtrlDown() || modifiers.isAltDown() || modifiers.isCommandDown())
            return false;
        showDevices(!isDeviceView());
        return true;
    }

    void updateToggle() {
        toggle.setButtonText(isDeviceView() ? "Show editor" : "Show devices");
        toggle.setToggleState(isDeviceView(), juce::dontSendNotification);
        toggle.setTitle(isDeviceView() ? "Current view: devices" : "Current view: editor");
        repaint();
    }

    juce::Component& editor;
    juce::Component& devices;
    juce::TextButton toggle;
    juce::Component::SafePointer<juce::Component> eventSource;
};

} // namespace alien
