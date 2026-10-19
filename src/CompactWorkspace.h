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
        toggle.getProperties().set("style", "ghost");
        toggle.onClick = [this] { showDevices(editor.isVisible()); };
        addComponentListener(this);
        updateToggle();
    }

    ~CompactWorkspace() override {
        removeComponentListener(this);
        if (eventSource != nullptr) eventSource->removeKeyListener(this);
    }

    bool isDeviceView() const noexcept { return devices.isVisible(); }

    // Title and subtitle drawn in the header while the editor is showing.
    void setEditorTitle(const juce::String& newTitle, const juce::String& newSubtitle = {}) {
        if (editorTitle == newTitle && editorSubtitle == newSubtitle) return;
        editorTitle = newTitle;
        editorSubtitle = newSubtitle;
        repaint(0, 0, getWidth(), headerHeight);
    }

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
        auto header = area.removeFromTop(headerHeight);
        toggle.setBounds(header.removeFromRight(132).reduced(4, 5));
        editor.setBounds(area);
        devices.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        auto header = juce::Rectangle<int>(0, 0, getWidth(), headerHeight);
        g.setColour(juce::Colour(0xff1d1f24));
        g.fillRect(header);
        g.setColour(juce::Colour(0xff26282d));
        g.fillRect(header.removeFromBottom(1));
        const auto title = isDeviceView() ? juce::String("Devices") : editorTitle;
        const auto subtitle = isDeviceView() ? juce::String("Live synth and MIDI input") : editorSubtitle;
        auto f = juce::Font(juce::FontOptions(13.0f, juce::Font::bold));
        g.setFont(f);
        g.setColour(juce::Colour(0xffe8eaed));
        const int titleWidth = static_cast<int>(juce::GlyphArrangement::getStringWidth(f, title)) + 2;
        g.drawText(title, 14, 0, titleWidth, headerHeight, juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(12.5f));
        g.setColour(juce::Colour(0xff686d75));
        g.drawText(subtitle, 14 + titleWidth + 12, 0, juce::jmax(0, getWidth() - titleWidth - 180), headerHeight,
                   juce::Justification::centredLeft, true);
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

    static constexpr int headerHeight = 36;
    juce::String editorTitle{"Pattern editor"}, editorSubtitle;
    juce::Component& editor;
    juce::Component& devices;
    juce::TextButton toggle;
    juce::Component::SafePointer<juce::Component> eventSource;
};

} // namespace alien
