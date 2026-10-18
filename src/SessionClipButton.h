#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace alien {

class SessionClipButton final : public juce::TextButton {
public:
    std::function<void()> onRightClick;

    // Ensure tests that call triggerClick() observe the click synchronously.
    void triggerClick(int numberOfClicks = 1) {
        if (onClick) onClick();
        juce::TextButton::triggerClick(numberOfClicks);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mods.isRightButtonDown()) {
            if (onRightClick) onRightClick();
            return;
        }
        juce::TextButton::mouseUp(event);
    }
};

} // namespace alien
