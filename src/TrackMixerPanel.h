#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "TrackMixer.h"
#include "ProjectController.h"
#include "StudioWidgets.h"
#include "SynthEngine.h"
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace alien {

// One console channel strip: instrument, pan knob, mute/solo, fader with dB
// scale, peak meter and coloured name plate. Writes the engine's lock-free
// TrackControls directly while a control moves.
class MixerChannelStrip final : public juce::Component {
public:
    MixerChannelStrip(TrackControls& trackControls, std::function<void(bool)> editedCallback)
        : controls(trackControls), edited(std::move(editedCallback)) {
        for (auto* c : std::array<juce::Component*, 9>{&title, &preset, &gainLabel, &gain, &panLabel, &pan,
                                                       &mute, &solo, &meter})
            addAndMakeVisible(c);
        title.setFont(ui::font(13.0f, true));
        title.setJustificationType(juce::Justification::centred);
        title.setColour(juce::Label::textColourId, ui::colours::accentInk);
        title.setInterceptsMouseClicks(false, false);
        for (std::size_t p = 0; p < presetNames.size(); ++p) preset.addItem(presetNames[p], static_cast<int>(p) + 1);
        preset.setTooltip("Changes new notes on this track; held notes finish with their original sound");
        preset.addItem("Drum kit", 100);
        preset.onChange = [this] {
            controls.instrument.store(preset.getSelectedId() == 100 ? 1 : 0);
            if (preset.getSelectedId() != 100) controls.preset.store(preset.getSelectedId() - 1);
            edited(true);
        };

        gainLabel.setText("VOLUME", juce::dontSendNotification);
        gain.setSliderStyle(juce::Slider::LinearVertical);
        gain.setRange(0, 200, 1);
        gain.setSkewFactorFromMidPoint(50.0);
        gain.setTooltip("Track volume. Double-click to reset to 0 dB.");
        gain.setDoubleClickReturnValue(true, 100.0);
        gain.textFromValueFunction = [](double v) { return gainToText(v); };
        gain.valueFromTextFunction = [](const juce::String& t) { return textToGain(t); };
        gain.onValueChange = [this] { controls.gain.store(static_cast<float>(gain.getValue() / 100)); edited(!gain.isMouseButtonDown()); };
        gain.onDragEnd = [this] { edited(true); };
        gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 20);

        panLabel.setText("PAN", juce::dontSendNotification);
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setRange(-100, 100, 1);
        pan.setTooltip("Stereo balance. Double-click to centre.");
        pan.setDoubleClickReturnValue(true, 0.0);
        pan.textFromValueFunction = [](double v) {
            const int p = juce::roundToInt(v);
            return p == 0 ? juce::String("C") : (p < 0 ? "L" : "R") + juce::String(std::abs(p));
        };
        pan.valueFromTextFunction = [](const juce::String& t) {
            const auto s = t.trim().toUpperCase();
            if (s.startsWith("L")) return -std::abs(s.substring(1).getDoubleValue());
            if (s.startsWith("R")) return std::abs(s.substring(1).getDoubleValue());
            return s == "C" ? 0.0 : s.getDoubleValue();
        };
        pan.onValueChange = [this] { controls.pan.store(static_cast<float>(pan.getValue() / 100)); edited(!pan.isMouseButtonDown()); };
        pan.onDragEnd = [this] { edited(true); };
        pan.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 18);

        for (auto* label : {&gainLabel, &panLabel}) {
            label->setFont(ui::font(10.0f, true));
            label->setColour(juce::Label::textColourId, ui::colours::textFaint);
            label->setJustificationType(juce::Justification::centred);
        }
        mute.setButtonText("M"); solo.setButtonText("S");
        mute.setTooltip("Mute"); solo.setTooltip("Solo");
        mute.setColour(juce::TextButton::buttonOnColourId, ui::colours::mute);
        solo.setColour(juce::TextButton::buttonOnColourId, ui::colours::solo);
        mute.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
        solo.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff06162a));
        mute.setClickingTogglesState(true); solo.setClickingTogglesState(true);
        mute.onClick = [this] { controls.mute.store(mute.getToggleState()); edited(true); };
        solo.onClick = [this] { controls.solo.store(solo.getToggleState()); edited(true); };
    }

    void setTrack(const juce::String& name, juce::Colour newColour) {
        colour = newColour;
        title.setText(name, juce::dontSendNotification);
        preset.setName(name + " instrument");
        gain.setName(name + " volume");
        pan.setName(name + " balance");
        mute.setName(name + " mute");
        solo.setName(name + " solo");
        gain.setColour(juce::Slider::trackColourId, colour);
        pan.setColour(juce::Slider::rotarySliderFillColourId, colour);
        repaint();
    }

    void refreshSettings() {
        const auto s = controls.snapshot();
        preset.setSelectedId(s.instrument == 1 ? 100 : s.preset + 1, juce::dontSendNotification);
        gain.setValue(s.gain * 100, juce::dontSendNotification);
        pan.setValue(s.pan * 100, juce::dontSendNotification);
        mute.setToggleState(s.mute, juce::dontSendNotification);
        solo.setToggleState(s.solo, juce::dontSendNotification);
        gain.updateText();
        pan.updateText();
    }

    void tick() { meter.setPeak(controls.peak.load()); }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 8);
        title.setBounds(r.removeFromBottom(26));
        r.removeFromBottom(8);
        preset.setBounds(r.removeFromTop(26));
        r.removeFromTop(8);
        panLabel.setBounds(r.removeFromTop(12));
        pan.setBounds(r.removeFromTop(70).withSizeKeepingCentre(64, 70));
        r.removeFromTop(8);
        auto ms = r.removeFromTop(24);
        mute.setBounds(ms.removeFromLeft(ms.getWidth() / 2 - 3));
        ms.removeFromLeft(6);
        solo.setBounds(ms);
        r.removeFromTop(10);
        gainLabel.setBounds(r.removeFromTop(12));
        r.removeFromTop(4);
        const int faderW = 40, meterW = 6;
        auto area = r.withSizeKeepingCentre(faderW + 26 + meterW * 2 + 2, r.getHeight());
        gain.setBounds(area.removeFromLeft(faderW));
        scaleBounds = area.removeFromLeft(26);
        meter.setBounds(area.withTrimmedBottom(22).withSizeKeepingCentre(meterW * 2 + 2, area.getHeight() - 22));
    }

    void paint(juce::Graphics& g) override {
        ui::drawCard(g, getLocalBounds().toFloat(), ui::colours::raised, 6.0f);
        g.setColour(colour);
        g.fillRoundedRectangle(title.getBounds().toFloat(), 4.0f);
        g.setFont(ui::font(9.5f));
        for (const auto db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
            const double pct = 100.0 * std::pow(10.0, db / 20.0);
            if (pct > gain.getMaximum()) continue;
            const int y = gain.getY() + juce::roundToInt(gain.getPositionOfValue(pct));
            g.setColour(db == 0.0 ? ui::colours::textDim : ui::colours::textFaint);
            g.drawText(db > 0 ? "+" + juce::String(static_cast<int>(db)) : juce::String(static_cast<int>(db)),
                       scaleBounds.getX(), y - 6, scaleBounds.getWidth(), 12, juce::Justification::centred);
        }
    }

private:
    static juce::String gainToText(double percent) {
        if (percent <= 0.0) return "-inf dB";
        const double db = 20.0 * std::log10(percent / 100.0);
        return (db > 0.05 ? "+" : "") + juce::String(db, 1) + " dB";
    }
    static double textToGain(const juce::String& text) {
        const auto t = text.trim().toLowerCase();
        if (t.startsWith("-inf")) return 0.0;
        if (t.endsWith("%")) return juce::jlimit(0.0, 200.0, t.dropLastCharacters(1).getDoubleValue());
        const double db = t.upToFirstOccurrenceOf("db", false, true).trim().getDoubleValue();
        return juce::jlimit(0.0, 200.0, 100.0 * std::pow(10.0, db / 20.0));
    }

    TrackControls& controls;
    std::function<void(bool)> edited;
    juce::Label title, gainLabel, panLabel;
    juce::ComboBox preset;
    juce::Slider gain, pan;
    juce::TextButton mute, solo;
    ui::LevelMeter meter;
    juce::Rectangle<int> scaleBounds;
    juce::Colour colour{ui::colours::accent};
};

// Console-style mixer: one strip per Session track, scrolling horizontally
// when there are more tracks than fit. Stored values are unchanged (gain
// 0..200 %, pan -100..100); only the presentation is in dB / L-C-R.
class TrackMixerPanel final : public juce::Component {
public:
    TrackMixerPanel(ProjectController& projectController,
                    std::array<TrackControls, ToneEngine::sessionTracks>& engineControls)
        : controller(projectController), controls(engineControls) {
        addAndMakeVisible(help);
        help.setText("Independent instrument, level and balance for each Session track. "
                     "The live keyboard and step synth are set in Devices.",
            juce::dontSendNotification);
        help.setColour(juce::Label::textColourId, ui::colours::textFaint);
        help.setFont(ui::font(12.5f));
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(false, true);
        viewport.setScrollBarThickness(10);
        rebuild();
    }

    // Called whenever a strip control changes the engine. "committed" is true
    // for discrete changes and at the end of a fader/knob drag.
    std::function<void(int, bool)> onTrackEdited;

    // Recreate strips after tracks are added, removed or reordered.
    void rebuild() {
        strips.clear();
        const auto& d = controller.document();
        for (int t = 0; t < d.trackCount() && t < static_cast<int>(controls.size()); ++t) {
            auto strip = std::make_unique<MixerChannelStrip>(controls[static_cast<std::size_t>(t)],
                [this, t](bool committed) { if (onTrackEdited) onTrackEdited(t, committed); });
            content.addAndMakeVisible(*strip);
            strips.push_back(std::move(strip));
        }
        refreshSettings();
        resized();
    }

    void refreshSettings() {
        const auto& d = controller.document();
        for (std::size_t t = 0; t < strips.size() && t < d.tracks.size(); ++t) {
            strips[t]->setTrack(d.tracks[t].name, ui::trackColour(d.tracks[t].colour));
            strips[t]->refreshSettings();
        }
    }

    void tick() {
        if (!isShowing()) return;
        for (auto& strip : strips) strip->tick();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16, 12);
        help.setBounds(area.removeFromTop(22));
        area.removeFromTop(10);
        viewport.setBounds(area);
        const int count = juce::jmax(1, static_cast<int>(strips.size()));
        const int stripWidth = juce::jlimit(minStripWidth, 168, (area.getWidth() - (count - 1) * gap) / count);
        const int contentWidth = count * stripWidth + (count - 1) * gap;
        const int height = area.getHeight() - (contentWidth > area.getWidth() ? 12 : 0);
        content.setSize(contentWidth, height);
        for (std::size_t i = 0; i < strips.size(); ++i)
            strips[i]->setBounds(static_cast<int>(i) * (stripWidth + gap), 0, stripWidth, height);
    }

    void paint(juce::Graphics& g) override { g.fillAll(ui::colours::surface); }

private:
    static constexpr int gap = 10, minStripWidth = 118;

    ProjectController& controller;
    std::array<TrackControls, ToneEngine::sessionTracks>& controls;
    juce::Label help;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<MixerChannelStrip>> strips;
};

} // namespace alien
