#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "TrackMixer.h"
#include <array>

namespace alien {
class TrackMixerPanel final : public juce::Component {
public:
    explicit TrackMixerPanel(std::array<TrackControls, 4>& controls) : tracks(controls) {
        addAndMakeVisible(help);
        help.setText("Session tracks | Independent sounds and levels | Live keyboard and step preset remain in Device rack",
            juce::dontSendNotification);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto name = "Track " + juce::String(static_cast<int>(i + 1));
            auto& strip = strips[i];
            for (auto* component : std::array<juce::Component*, 8>{&strip.title, &strip.preset,
                &strip.gainLabel, &strip.gain, &strip.panLabel, &strip.pan, &strip.mute, &strip.solo})
                addAndMakeVisible(component);
            strip.title.setText(name, juce::dontSendNotification);
            strip.preset.addItem("Lead (saw)", 1);
            strip.preset.addItem("Bass (square)", 2);
            strip.preset.addItem("Pluck (sine)", 3);
            strip.preset.setName(name + " instrument");
            strip.preset.setTooltip("Changes new notes on this track; held notes finish with their original sound");
            strip.preset.onChange = [this, i] { tracks[i].preset.store(strips[i].preset.getSelectedId() - 1); };
            strip.gainLabel.setText("Volume (%)", juce::dontSendNotification);
            strip.gain.setRange(0, 200, 1);
            strip.gain.setName(name + " volume");
            strip.gain.setTextValueSuffix(" %");
            strip.gain.onValueChange = [this, i] { tracks[i].gain.store(static_cast<float>(strips[i].gain.getValue() / 100)); };
            strip.panLabel.setText("Balance (L / R)", juce::dontSendNotification);
            strip.pan.setRange(-100, 100, 1);
            strip.pan.setName(name + " balance");
            strip.pan.onValueChange = [this, i] { tracks[i].pan.store(static_cast<float>(strips[i].pan.getValue() / 100)); };
            for (auto* slider : {&strip.gain, &strip.pan}) {
                slider->setSliderStyle(juce::Slider::LinearHorizontal);
                slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 22);
            }
            strip.mute.setButtonText("Mute"); strip.solo.setButtonText("Solo");
            strip.mute.setName(name + " mute"); strip.solo.setName(name + " solo");
            strip.mute.setClickingTogglesState(true); strip.solo.setClickingTogglesState(true);
            strip.mute.onClick = [this, i] { tracks[i].mute.store(strips[i].mute.getToggleState()); };
            strip.solo.onClick = [this, i] { tracks[i].solo.store(strips[i].solo.getToggleState()); };
        }
        refreshSettings();
    }
    void refreshSettings() {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto s = tracks[i].snapshot();
            strips[i].preset.setSelectedId(s.preset + 1, juce::dontSendNotification);
            strips[i].gain.setValue(s.gain * 100, juce::dontSendNotification);
            strips[i].pan.setValue(s.pan * 100, juce::dontSendNotification);
            strips[i].mute.setToggleState(s.mute, juce::dontSendNotification);
            strips[i].solo.setToggleState(s.solo, juce::dontSendNotification);
        }
    }
    void tick() {
        for (std::size_t i = 0; i < tracks.size(); ++i)
            displayedPeaks[i] = std::max(tracks[i].peak.load(), displayedPeaks[i] * 0.8f);
        repaint();
    }
    void resized() override {
        help.setBounds(16, 8, getWidth() - 32, 28);
        const int width = (getWidth() - 32) / 4;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto& s = strips[i];
            const int x = 24 + static_cast<int>(i) * width, w = width - 24;
            s.title.setBounds(x, 46, w, 24);
            s.preset.setBounds(x, 78, w, 28);
            s.gainLabel.setBounds(x, 116, w, 22); s.gain.setBounds(x, 140, w, 48);
            s.panLabel.setBounds(x, 192, w, 22); s.pan.setBounds(x, 216, w, 48);
            s.mute.setBounds(x, 278, w / 2 - 4, 28);
            s.solo.setBounds(x + w / 2 + 4, 278, w / 2 - 4, 28);
        }
    }
    void paint(juce::Graphics& g) override {
        const int width = (getWidth() - 32) / 4;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const juce::Rectangle<float> bar(24.0f + static_cast<float>(i * width), 322.0f,
                static_cast<float>(width - 24), 8.0f);
            g.setColour(juce::Colour(0xff303b4b)); g.fillRect(bar);
            g.setColour(displayedPeaks[i] >= 1 ? juce::Colours::orange : juce::Colour(0xff63dfc2));
            g.fillRect(bar.withWidth(bar.getWidth() * std::clamp(displayedPeaks[i], 0.0f, 1.0f)));
        }
    }
private:
    struct Strip {
        juce::Label title, gainLabel, panLabel;
        juce::ComboBox preset;
        juce::Slider gain, pan;
        juce::TextButton mute, solo;
    };
    std::array<TrackControls, 4>& tracks;
    std::array<Strip, 4> strips;
    std::array<float, 4> displayedPeaks{};
    juce::Label help;
};
} // namespace alien
