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

// Fader text helpers shared by the track strips and the master strip. Faders store a
// percentage of unity (100 = 0 dB); the display and text entry use dB.
namespace mixer_text {
inline juce::String gainToText(double percent) {
    if (percent <= 0.0) return "-inf dB";
    const double db = 20.0 * std::log10(percent / 100.0);
    return (db > 0.05 ? "+" : "") + juce::String(db, 1) + " dB";
}
inline double textToGain(const juce::String& text, double maxPercent) {
    const auto t = text.trim().toLowerCase();
    if (t.startsWith("-inf")) return 0.0;
    if (t.endsWith("%")) return juce::jlimit(0.0, maxPercent, t.dropLastCharacters(1).getDoubleValue());
    const double db = t.upToFirstOccurrenceOf("db", false, true).trim().getDoubleValue();
    return juce::jlimit(0.0, maxPercent, 100.0 * std::pow(10.0, db / 20.0));
}
inline juce::String returnLetter(int r) { return juce::String::charToString(static_cast<juce::juce_wchar>('A' + r)); }
} // namespace mixer_text

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
        gain.textFromValueFunction = [](double v) { return mixer_text::gainToText(v); };
        gain.valueFromTextFunction = [](const juce::String& t) { return mixer_text::textToGain(t, 200.0); };
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

        // One knob per return track (off to 0 dB) and, under it, its pre/post switch.
        sendsLabel.setText("SENDS", juce::dontSendNotification);
        sendsLabel.setFont(ui::font(10.0f, true));
        sendsLabel.setColour(juce::Label::textColourId, ui::colours::textFaint);
        sendsLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(sendsLabel);
        for (std::size_t r = 0; r < sends.size(); ++r) {
            auto& knob = sends[r];
            addAndMakeVisible(knob);
            knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            knob.setRange(0, 100, 1);
            knob.setSkewFactorFromMidPoint(25.0);
            knob.setDoubleClickReturnValue(true, 0.0);
            knob.textFromValueFunction = [](double v) { return mixer_text::gainToText(v); };
            knob.onValueChange = [this, r] {
                controls.send[r].store(static_cast<float>(sends[r].getValue() / 100));
                updateSendTooltip(static_cast<int>(r));
                edited(!sends[r].isMouseButtonDown());
            };
            knob.onDragEnd = [this] { edited(true); };
            auto& mode = sendModes[r];
            addAndMakeVisible(mode);
            mode.setButtonText(mixer_text::returnLetter(static_cast<int>(r)));
            mode.setClickingTogglesState(true);
            mode.setColour(juce::TextButton::buttonOnColourId, ui::colours::accent);
            mode.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
            mode.onClick = [this, r] {
                controls.sendPre[r].store(sendModes[r].getToggleState());
                updateSendTooltip(static_cast<int>(r));
                edited(true);
            };
            updateSendTooltip(static_cast<int>(r));
        }
    }

    void setTrack(const juce::String& name, juce::Colour newColour) {
        colour = newColour;
        title.setText(name, juce::dontSendNotification);
        preset.setName(name + " instrument");
        gain.setName(name + " volume");
        pan.setName(name + " balance");
        mute.setName(name + " mute");
        solo.setName(name + " solo");
        for (std::size_t r = 0; r < sends.size(); ++r) {
            sends[r].setName(name + " send " + mixer_text::returnLetter(static_cast<int>(r)));
            sendModes[r].setName(name + " send " + mixer_text::returnLetter(static_cast<int>(r)) + " pre-fader");
            sends[r].setColour(juce::Slider::rotarySliderFillColourId, colour);
        }
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
        for (std::size_t r = 0; r < sends.size(); ++r) {
            sends[r].setValue(s.send[r] * 100, juce::dontSendNotification);
            sendModes[r].setToggleState(s.sendPre[r], juce::dontSendNotification);
            updateSendTooltip(static_cast<int>(r));
        }
        gain.updateText();
        pan.updateText();
    }

    void tick() { meter.setPeak(controls.peak.load()); }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 8);
        title.setBounds(r.removeFromBottom(26));
        r.removeFromBottom(8);
        preset.setBounds(r.removeFromTop(26));
        r.removeFromTop(6);
        sendsLabel.setBounds(r.removeFromTop(12));
        auto knobs = r.removeFromTop(28), modes = r.removeFromTop(16);
        const int cell = knobs.getWidth() / static_cast<int>(sends.size());
        for (std::size_t i = 0; i < sends.size(); ++i) {
            sends[i].setBounds(knobs.removeFromLeft(cell).withSizeKeepingCentre(juce::jmin(cell, 28), 28));
            sendModes[i].setBounds(modes.removeFromLeft(cell).withSizeKeepingCentre(juce::jmin(cell - 2, 24), 16));
        }
        r.removeFromTop(6);
        panLabel.setBounds(r.removeFromTop(12));
        pan.setBounds(r.removeFromTop(64).withSizeKeepingCentre(60, 64));
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

    // Geometry and controls shared with the tests.
    juce::Slider& sendKnob(int r) { return sends[static_cast<std::size_t>(r)]; }
    juce::TextButton& sendMode(int r) { return sendModes[static_cast<std::size_t>(r)]; }

private:
    void updateSendTooltip(int r) {
        const auto i = static_cast<std::size_t>(r);
        const auto letter = mixer_text::returnLetter(r);
        const bool pre = sendModes[i].getToggleState();
        sends[i].setTooltip("Send to return " + letter + ": " + (sends[i].getValue() <= 0 ? juce::String("off") : mixer_text::gainToText(sends[i].getValue()))
                            + (pre ? " (pre-fader)" : " (post-fader)") + ". Double-click to turn it off.");
        sendModes[i].setTooltip(pre ? "Send " + letter + " is pre-fader: it ignores this track's fader and pan. Click for post-fader."
                                    : "Send " + letter + " is post-fader: it follows this track's fader and pan. Click for pre-fader.");
    }

    TrackControls& controls;
    std::function<void(bool)> edited;
    juce::Label title, gainLabel, panLabel, sendsLabel;
    juce::ComboBox preset;
    juce::Slider gain, pan;
    std::array<juce::Slider, maxReturns> sends;
    std::array<juce::TextButton, maxReturns> sendModes;
    juce::TextButton mute, solo;
    ui::LevelMeter meter;
    juce::Rectangle<int> scaleBounds;
    juce::Colour colour{ui::colours::accent};
};

// A return track's strip: its level (the same range as a track fader), pan, mute and meter.
// What the tracks send arrives here, through the return's FX (chosen in Plugins as
// "Return A FX" and so on), and goes on to the master.
class ReturnChannelStrip final : public juce::Component {
public:
    ReturnChannelStrip(int returnIndex, ReturnControls& returnControls, std::function<void(bool)> editedCallback)
        : index(returnIndex), controls(returnControls), edited(std::move(editedCallback)) {
        for (auto* c : std::array<juce::Component*, 7>{&title, &panLabel, &pan, &mute, &gainLabel, &gain, &meter}) addAndMakeVisible(c);
        const auto letter = mixer_text::returnLetter(index);
        title.setText("RETURN " + letter, juce::dontSendNotification);
        title.setFont(ui::font(12.0f, true));
        title.setJustificationType(juce::Justification::centred);
        title.setColour(juce::Label::textColourId, ui::colours::accentInk);
        title.setInterceptsMouseClicks(false, false);
        for (auto* label : {&gainLabel, &panLabel}) {
            label->setFont(ui::font(10.0f, true));
            label->setColour(juce::Label::textColourId, ui::colours::textFaint);
            label->setJustificationType(juce::Justification::centred);
        }
        panLabel.setText("PAN", juce::dontSendNotification);
        gainLabel.setText("VOLUME", juce::dontSendNotification);
        pan.setName("Return " + letter + " balance");
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setRange(-100, 100, 1);
        pan.setDoubleClickReturnValue(true, 0.0);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        pan.setTooltip("Return " + letter + " stereo balance. Double-click to centre.");
        pan.setColour(juce::Slider::rotarySliderFillColourId, ui::colours::accent);
        pan.onValueChange = [this] { controls.pan.store(static_cast<float>(pan.getValue() / 100)); edited(!pan.isMouseButtonDown()); };
        pan.onDragEnd = [this] { edited(true); };
        mute.setName("Return " + letter + " mute");
        mute.setButtonText("M"); mute.setTooltip("Mute return " + letter);
        mute.setClickingTogglesState(true);
        mute.setColour(juce::TextButton::buttonOnColourId, ui::colours::mute);
        mute.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
        mute.onClick = [this] { controls.mute.store(mute.getToggleState()); edited(true); };
        gain.setName("Return " + letter + " volume");
        gain.setSliderStyle(juce::Slider::LinearVertical);
        gain.setRange(0, 200, 1);
        gain.setSkewFactorFromMidPoint(50.0);
        gain.setTooltip("Return " + letter + " volume, after its FX. Double-click to reset to 0 dB.");
        gain.setDoubleClickReturnValue(true, 100.0);
        gain.textFromValueFunction = [](double v) { return mixer_text::gainToText(v); };
        gain.valueFromTextFunction = [](const juce::String& t) { return mixer_text::textToGain(t, 200.0); };
        gain.onValueChange = [this] { controls.gain.store(static_cast<float>(gain.getValue() / 100)); edited(!gain.isMouseButtonDown()); };
        gain.onDragEnd = [this] { edited(true); };
        gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
        gain.setColour(juce::Slider::trackColourId, ui::colours::accent);
    }

    void refreshSettings() {
        const auto s = controls.snapshot();
        gain.setValue(s.gain * 100, juce::dontSendNotification);
        pan.setValue(s.pan * 100, juce::dontSendNotification);
        mute.setToggleState(s.mute, juce::dontSendNotification);
        gain.updateText();
    }
    void tick() { meter.setPeak(controls.peak.load()); }

    void resized() override {
        auto r = getLocalBounds().reduced(6, 8);
        title.setBounds(r.removeFromBottom(26));
        r.removeFromBottom(8);
        panLabel.setBounds(r.removeFromTop(12));
        pan.setBounds(r.removeFromTop(44).withSizeKeepingCentre(44, 44));
        r.removeFromTop(6);
        mute.setBounds(r.removeFromTop(24).withSizeKeepingCentre(juce::jmin(r.getWidth(), 48), 24));
        r.removeFromTop(10);
        gainLabel.setBounds(r.removeFromTop(12));
        r.removeFromTop(4);
        const int faderW = 34, meterW = 6;
        auto area = r.withSizeKeepingCentre(faderW + 8 + meterW * 2 + 2, r.getHeight());
        gain.setBounds(area.removeFromLeft(faderW));
        area.removeFromLeft(8);
        meter.setBounds(area.withTrimmedBottom(22).withSizeKeepingCentre(meterW * 2 + 2, area.getHeight() - 22));
    }

    void paint(juce::Graphics& g) override {
        ui::drawCard(g, getLocalBounds().toFloat(), ui::colours::raised, 6.0f);
        g.setColour(ui::colours::accent.withAlpha(0.75f));
        g.fillRoundedRectangle(title.getBounds().toFloat(), 4.0f);
    }

private:
    int index;
    ReturnControls& controls;
    std::function<void(bool)> edited;
    juce::Label title, panLabel, gainLabel;
    juce::Slider pan, gain;
    juce::TextButton mute;
    ui::LevelMeter meter;
};

// The master channel: one fader for the whole mix (-inf to +12 dB) and an output
// meter. The meter reads before the output soft clipper, so its red clip light
// shows when the mix is hot enough to be bent by it.
class MasterChannelStrip final : public juce::Component {
public:
    MasterChannelStrip(MasterControls& masterControls, std::function<void(bool)> editedCallback)
        : controls(masterControls), edited(std::move(editedCallback)) {
        for (auto* c : std::array<juce::Component*, 4>{&title, &gainLabel, &gain, &meter}) addAndMakeVisible(c);
        title.setText("MASTER", juce::dontSendNotification);
        title.setFont(ui::font(13.0f, true));
        title.setJustificationType(juce::Justification::centred);
        title.setColour(juce::Label::textColourId, ui::colours::accentInk);
        title.setInterceptsMouseClicks(false, false);
        gainLabel.setText("VOLUME", juce::dontSendNotification);
        gainLabel.setFont(ui::font(10.0f, true));
        gainLabel.setColour(juce::Label::textColourId, ui::colours::textFaint);
        gainLabel.setJustificationType(juce::Justification::centred);
        gain.setName("Master volume");
        gain.setSliderStyle(juce::Slider::LinearVertical);
        gain.setRange(0, maxMasterGain * 100.0, 1);
        gain.setSkewFactorFromMidPoint(50.0);
        gain.setTooltip("Master volume for the whole mix, after master FX. Double-click to reset to 0 dB.");
        gain.setDoubleClickReturnValue(true, 100.0);
        gain.textFromValueFunction = [](double v) { return mixer_text::gainToText(v); };
        gain.valueFromTextFunction = [](const juce::String& t) { return mixer_text::textToGain(t, maxMasterGain * 100.0); };
        gain.onValueChange = [this] {
            controls.gain.store(static_cast<float>(gain.getValue() / 100));
            edited(!gain.isMouseButtonDown());
        };
        gain.onDragEnd = [this] { edited(true); };
        gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 20);
        gain.setColour(juce::Slider::trackColourId, ui::colours::accent);
    }

    void refreshSettings() {
        gain.setValue(controls.gain.load() * 100, juce::dontSendNotification);
        gain.updateText();
    }
    void tick() { meter.setPeak(controls.peak.load()); }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 8);
        title.setBounds(r.removeFromBottom(26));
        r.removeFromBottom(8);
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
        g.setColour(ui::colours::accent);
        g.fillRoundedRectangle(title.getBounds().toFloat(), 4.0f);
        g.setFont(ui::font(9.5f));
        for (const auto db : {12.0, 6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
            const double pct = 100.0 * std::pow(10.0, db / 20.0);
            if (pct > gain.getMaximum()) continue;
            const int y = gain.getY() + juce::roundToInt(gain.getPositionOfValue(pct));
            g.setColour(db == 0.0 ? ui::colours::textDim : ui::colours::textFaint);
            g.drawText(db > 0 ? "+" + juce::String(static_cast<int>(db)) : juce::String(static_cast<int>(db)),
                       scaleBounds.getX(), y - 6, scaleBounds.getWidth(), 12, juce::Justification::centred);
        }
    }

private:
    MasterControls& controls;
    std::function<void(bool)> edited;
    juce::Label title, gainLabel;
    juce::Slider gain;
    ui::LevelMeter meter;
    juce::Rectangle<int> scaleBounds;
};

// Console-style mixer: one strip per Session track, scrolling horizontally
// when there are more tracks than fit. Stored values are unchanged (gain
// 0..200 %, pan -100..100); only the presentation is in dB / L-C-R.
class TrackMixerPanel final : public juce::Component {
public:
    TrackMixerPanel(ProjectController& projectController,
                    std::array<TrackControls, ToneEngine::sessionTracks>& engineControls,
                    std::array<ReturnControls, maxReturns>& returnControls,
                    MasterControls& masterControls)
        : controller(projectController), controls(engineControls),
          masterStrip(masterControls, [this](bool committed) { if (onMasterEdited) onMasterEdited(committed); }) {
        addAndMakeVisible(masterStrip);
        for (int r = 0; r < maxReturns; ++r) {
            returnStrips[static_cast<std::size_t>(r)] = std::make_unique<ReturnChannelStrip>(r, returnControls[static_cast<std::size_t>(r)],
                [this, r](bool committed) { if (onReturnEdited) onReturnEdited(r, committed); });
            addAndMakeVisible(*returnStrips[static_cast<std::size_t>(r)]);
        }
        addAndMakeVisible(help);
        help.setText("Independent instrument, level, balance and sends for each Session track. Sends A-D feed the return tracks "
                     "(their FX are set in Plugins); the master fader on the right sets the overall level.",
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
    // Same contract for the master fader, and for a return strip (its index).
    std::function<void(bool)> onMasterEdited;
    std::function<void(int, bool)> onReturnEdited;

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
        masterStrip.refreshSettings();
        for (auto& strip : returnStrips) strip->refreshSettings();
        const auto& d = controller.document();
        for (std::size_t t = 0; t < strips.size() && t < d.tracks.size(); ++t) {
            strips[t]->setTrack(d.tracks[t].name, ui::trackColour(d.tracks[t].colour));
            strips[t]->refreshSettings();
        }
    }

    void tick() {
        if (!isShowing()) return;
        for (auto& strip : strips) strip->tick();
        for (auto& strip : returnStrips) strip->tick();
        masterStrip.tick();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16, 12);
        help.setBounds(area.removeFromTop(22));
        area.removeFromTop(10);
        masterStrip.setBounds(area.removeFromRight(minStripWidth));
        area.removeFromRight(gap);
        for (int r = maxReturns - 1; r >= 0; --r) {
            returnStrips[static_cast<std::size_t>(r)]->setBounds(area.removeFromRight(returnStripWidth));
            area.removeFromRight(r == 0 ? gap * 2 : 4);
        }
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

    // For the tests.
    MixerChannelStrip* trackStrip(int t) { return t >= 0 && t < static_cast<int>(strips.size()) ? strips[static_cast<std::size_t>(t)].get() : nullptr; }
    ReturnChannelStrip& returnStrip(int r) { return *returnStrips[static_cast<std::size_t>(r)]; }

private:
    static constexpr int gap = 10, minStripWidth = 118, returnStripWidth = 84;

    ProjectController& controller;
    std::array<TrackControls, ToneEngine::sessionTracks>& controls;
    MasterChannelStrip masterStrip;
    std::array<std::unique_ptr<ReturnChannelStrip>, maxReturns> returnStrips;
    juce::Label help;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<MixerChannelStrip>> strips;
};

} // namespace alien
