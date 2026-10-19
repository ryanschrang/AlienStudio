#pragma once
#include "StudioWidgets.h"
#include "Branding.h"
#include <array>
#include <functional>

namespace alien {

// Application top bar: brand, view switch, transport (play + LCD with tempo)
// and project file actions. It only reports intent through callbacks.
class TopBar final : public juce::Component {
public:
    static constexpr int height = 58;

    TopBar() {
        namespace col = ui::colours;
        addAndMakeVisible(brandMark);
        addAndMakeVisible(title);
        title.setText("Alien Studio", juce::dontSendNotification);
        title.setFont(ui::font(15.0f, true));
        title.setColour(juce::Label::textColourId, col::text);
        title.setJustificationType(juce::Justification::bottomLeft);
        title.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(brandSubtitle);
        brandSubtitle.setText("PATTERN STUDIO", juce::dontSendNotification);
        brandSubtitle.setFont(ui::font(9.5f, true));
        brandSubtitle.setColour(juce::Label::textColourId, col::accent);
        brandSubtitle.setJustificationType(juce::Justification::topLeft);
        brandSubtitle.setInterceptsMouseClicks(false, false);

        const std::array<const char*, 4> viewNames{"Steps", "Piano Roll", "Session", "Mixer"};
        for (int i = 0; i < 4; ++i) {
            auto& b = viewButtons[static_cast<std::size_t>(i)];
            const juce::String name(viewNames[static_cast<std::size_t>(i)]);
            addAndMakeVisible(b);
            b.setButtonText(name);
            b.setName("Show " + name + " view");
            b.getProperties().set("style", "tab");
            b.setRadioGroupId(1001);
            b.setTooltip("Show the " + name + " view");
            b.onClick = [this, i] { if (onViewSelected) onViewSelected(i); };
        }

        addAndMakeVisible(play);
        play.setButtonText("Play");
        play.setIconOnly(true);
        play.setGlyph(ui::Glyph::play);
        play.setName("Start or stop playback");
        play.setTooltip("Start or stop the current pattern");
        play.setColour(juce::TextButton::buttonColourId, col::control);
        play.setColour(juce::TextButton::buttonOnColourId, col::accent);
        play.setGlyphColour(col::accent);
        play.onClick = [this] { if (onPlayToggled) onPlayToggled(); };

        addAndMakeVisible(transportDisplay);
        tempo.setSliderStyle(juce::Slider::LinearBarVertical);
        tempo.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120, juce::dontSendNotification);
        tempo.setName("Tempo");
        tempo.setTooltip("Tempo in beats per minute. Drag up/down or use the mouse wheel; double-click resets to 120.");
        tempo.setDoubleClickReturnValue(true, 120.0);
        tempo.setMouseDragSensitivity(220);
        tempo.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        tempo.onValueChange = [this] { if (onTempoChanged) onTempoChanged(static_cast<int>(tempo.getValue())); };
        tempo.onDragEnd = [this] { if (onTempoCommitted) onTempoCommitted(); };

        for (auto* b : {&openButton, &saveButton, &portableButton}) {
            addAndMakeVisible(b);
            b->getProperties().set("style", "ghost");
        }
        saveButton.setButtonText("Save");
        saveButton.setGlyph(ui::Glyph::save);
        saveButton.setName("Save Alien Studio session project");
        saveButton.setTooltip("Save the session project");
        saveButton.onClick = [this] { if (onSave) onSave(); };
        portableButton.setButtonText("Save portable");
        portableButton.setGlyph(ui::Glyph::collect);
        portableButton.setName("Save project copy with collected audio");
        portableButton.setTooltip("Save a project file and adjacent media folder; move both together");
        portableButton.onClick = [this] { if (onSavePortable) onSavePortable(); };
        openButton.setButtonText("Open");
        openButton.setGlyph(ui::Glyph::open);
        openButton.setName("Open Alien Studio session project");
        openButton.setTooltip("Open a session project");
        openButton.onClick = [this] { if (onOpen) onOpen(); };
    }

    std::function<void(int)> onViewSelected;
    std::function<void()> onPlayToggled, onTempoCommitted, onOpen, onSave, onSavePortable;
    std::function<void(int)> onTempoChanged;

    void setActiveView(int index) {
        for (std::size_t i = 0; i < viewButtons.size(); ++i)
            viewButtons[i].setToggleState(static_cast<int>(i) == index, juce::dontSendNotification);
    }

    void setPlaying(bool playing) {
        if (play.getToggleState() == playing) return;
        play.setToggleState(playing, juce::dontSendNotification);
        play.setGlyph(playing ? ui::Glyph::stop : ui::Glyph::play);
        play.setButtonText(playing ? "Stop" : "Play");
    }

    void setPosition(int step, bool running, const juce::String& source) { transportDisplay.setPosition(step, running, source); }
    void setTempo(int bpm) { tempo.setValue(bpm, juce::dontSendNotification); }

    void setBusy(bool busy) {
        saveButton.setEnabled(!busy); portableButton.setEnabled(!busy); openButton.setEnabled(!busy);
    }

    void resized() override {
        auto top = getLocalBounds().reduced(12, 0);
        const bool compact = getWidth() < 1180;
        brandMark.setBounds(top.removeFromLeft(36).withSizeKeepingCentre(36, 36));
        top.removeFromLeft(8);
        if (!compact) {
            auto brand = top.removeFromLeft(118);
            title.setBounds(brand.removeFromTop(brand.getHeight() / 2 + 2));
            brandSubtitle.setBounds(brand.translated(1, 0));
        }
        title.setVisible(!compact);
        brandSubtitle.setVisible(!compact);
        top.removeFromLeft(compact ? 4 : 10);

        const std::array<int, 4> widths{62, 88, 74, 62};
        const int total = widths[0] + widths[1] + widths[2] + widths[3] + 12;
        auto views = top.removeFromLeft(total).withSizeKeepingCentre(total, 32);
        viewSwitchBounds = views.expanded(4, 4);
        for (std::size_t i = 0; i < viewButtons.size(); ++i) {
            viewButtons[i].setBounds(views.removeFromLeft(widths[i]));
            views.removeFromLeft(4);
        }

        auto files = top.removeFromRight(compact ? 112 : 300);
        for (auto* b : {&portableButton, &saveButton, &openButton}) {
            b->setIconOnly(compact);
            const int w = compact ? 34 : (b == &portableButton ? 128 : 80);
            b->setBounds(files.removeFromRight(w).withSizeKeepingCentre(w, 32));
            files.removeFromRight(4);
        }

        const int transportWidth = 44 + 8 + 300;
        auto transport = top.withSizeKeepingCentre(juce::jmin(top.getWidth(), transportWidth), 44);
        play.setBounds(transport.removeFromLeft(44).withSizeKeepingCentre(44, 40));
        transport.removeFromLeft(8);
        transportDisplay.setBounds(transport);
    }

    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1e2025), 0.0f, 0.0f,
                                               juce::Colour(0xff17181c), 0.0f, static_cast<float>(r.getBottom()), false));
        g.fillRect(r);
        g.setColour(juce::Colours::black);
        g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);
        g.setColour(ui::colours::inset.withAlpha(0.8f));
        g.fillRoundedRectangle(viewSwitchBounds.toFloat(), 7.0f);
    }

private:
    branding::LogoMark brandMark;
    juce::Label title, brandSubtitle;
    std::array<ui::GlyphButton, 4> viewButtons;
    ui::GlyphButton play, openButton, saveButton, portableButton;
    juce::Slider tempo{juce::Slider::LinearBarVertical, juce::Slider::NoTextBox};
    ui::TransportDisplay transportDisplay{tempo};
    juce::Rectangle<int> viewSwitchBounds;
};

} // namespace alien
