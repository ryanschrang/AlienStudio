#pragma once
#include "StudioWidgets.h"
#include <functional>
#include <memory>

namespace alien {

// A permanent tab host. Only its existing editor component moves to a native
// window; tab indices, project/controller references and DSP ownership stay put.
class DetachableView final : public juce::Component {
public:
    DetachableView(juce::String title,juce::Component& content):name(std::move(title)),view(content) {
        addAndMakeVisible(view);
        addChildComponent(message);message.setText(name+" is open in a separate window",juce::dontSendNotification);
        message.setJustificationType(juce::Justification::centred);
        addChildComponent(show);show.setButtonText("Show detached window");show.onClick=[this]{bringToFront();};
        addChildComponent(returnHere);returnHere.setButtonText("Dock back here");returnHere.onClick=[this]{dock();};
    }
    ~DetachableView() override { closeWindow(); }
    std::function<void(juce::Component&)> onWindowOpened,onWindowClosed;
    std::function<void()> onDocked,onWindowDeactivated;
    bool isDetached() const noexcept {return window!=nullptr;}
    juce::DocumentWindow* detachedWindow() const noexcept {return window.get();}
    void detach() {
        if(window){bringToFront();return;}
        juce::Component::SafePointer<DetachableView> safe(this);
        window=std::make_unique<Window>(name+" - Alien Studio",[safe]{if(safe)safe->dock();},
            [safe]{if(safe&&safe->onWindowDeactivated)safe->onWindowDeactivated();});
        window->setLookAndFeel(&getLookAndFeel());
        window->setContentOwned(new FloatingContent(view,[safe]{if(safe)safe->dock();}),false);
        window->setResizable(true,true);window->setResizeLimits(960,360,3840,2160);
        window->centreAroundComponent(isShowing()?this:nullptr,juce::jmax(1000,getWidth()),juce::jmax(480,getHeight()+34));
        if(onWindowOpened)onWindowOpened(*window);
        setPlaceholder(true);resized();
        window->setVisible(true);window->toFront(true);
    }
    void dock() {
        if(!window)return;
        closeWindow();addAndMakeVisible(view);setPlaceholder(false);resized();
        if(onDocked)onDocked();
    }
    void bringToFront() {if(window){window->setVisible(true);window->setMinimised(false);window->toFront(true);}}
    void resized() override {
        if(!window)view.setBounds(getLocalBounds());
        auto r=getLocalBounds().withSizeKeepingCentre(juce::jmin(560,getWidth()),132);
        message.setBounds(r.removeFromTop(52));r.removeFromTop(10);
        auto buttons=r.removeFromTop(30).withSizeKeepingCentre(382,30);
        show.setBounds(buttons.removeFromLeft(190));buttons.removeFromLeft(8);returnHere.setBounds(buttons);
    }
    void paint(juce::Graphics& g) override {g.fillAll(ui::colours::surface);}
private:
    class FloatingContent final : public juce::Component, public juce::DragAndDropContainer {
    public:
        FloatingContent(juce::Component& c,std::function<void()> close):content(c) {
            addAndMakeVisible(content);addAndMakeVisible(dockButton);
            dockButton.setButtonText("Dock into main window");dockButton.onClick=std::move(close);
        }
        void resized() override {
            auto r=getLocalBounds();auto header=r.removeFromTop(34);
            dockButton.setBounds(header.removeFromRight(180).reduced(4,3));content.setBounds(r);
        }
        void paint(juce::Graphics& g) override {g.fillAll(ui::colours::raised);}
    private:
        juce::Component& content;juce::TextButton dockButton;
    };
    class Window final : public juce::DocumentWindow {
    public:
        Window(const juce::String& title,std::function<void()> close,std::function<void()> blur)
            :DocumentWindow(title,ui::colours::surface,DocumentWindow::allButtons),onClose(std::move(close)),onBlur(std::move(blur)) {
            setUsingNativeTitleBar(true);
        }
        void closeButtonPressed() override {if(onClose)onClose();}
        void activeWindowStatusChanged() override {if(!isActiveWindow()&&onBlur)onBlur();}
    private:
        std::function<void()> onClose,onBlur;
    };
    void setPlaceholder(bool visible) {message.setVisible(visible);show.setVisible(visible);returnHere.setVisible(visible);}
    void closeWindow() {
        if(!window)return;
        if(onWindowClosed)onWindowClosed(*window);
        window->clearContentComponent(); // the editor is non-owned by FloatingContent
        window->setLookAndFeel(nullptr);window.reset();
    }
    juce::String name;juce::Component& view;
    juce::Label message;juce::TextButton show,returnHere;
    std::unique_ptr<Window> window;
};
}
