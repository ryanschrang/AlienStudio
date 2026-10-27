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
            knob.setVisible(returnNames[r].isNotEmpty());
            mode.setVisible(returnNames[r].isNotEmpty());
        }

        // Where the fader goes: the master, or one of the group buses.
        outputLabel.setText("OUTPUT", juce::dontSendNotification);
        outputLabel.setFont(ui::font(10.0f, true));
        outputLabel.setColour(juce::Label::textColourId, ui::colours::textFaint);
        outputLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(outputLabel);
        addAndMakeVisible(output);
        output.setTooltip("Where this track goes after its fader: the master, or a group bus");
        output.onChange = [this] { if (onOutputChosen && output.getSelectedId() > 0) onOutputChosen(output.getSelectedId() - 2); };
    }

    // Called with -1 (the master) or a bus slot when the user picks an output.
    std::function<void(int)> onOutputChosen;
    // busNames: one per bus slot, empty for a slot not in use.
    void setOutputs(const std::array<juce::String, maxBuses>& busNames, int current) {
        output.clear(juce::dontSendNotification);
        output.addItem("Master", 1);
        for (int b = 0; b < maxBuses; ++b)
            if (busNames[static_cast<std::size_t>(b)].isNotEmpty()) output.addItem(busNames[static_cast<std::size_t>(b)], b + 2);
        output.setSelectedId(output.indexOfItemId(current + 2) >= 0 ? current + 2 : 1, juce::dontSendNotification);
    }

    // returnNames: one per return slot, empty for a slot not in use. Only the existing returns get a knob.
    void setReturns(const std::array<juce::String, maxReturns>& names) {
        if (names == returnNames) return;
        returnNames = names;
        for (std::size_t r = 0; r < sends.size(); ++r) {
            sends[r].setVisible(names[r].isNotEmpty());
            sendModes[r].setVisible(names[r].isNotEmpty());
            updateSendTooltip(static_cast<int>(r));
        }
        resized();
    }

    void setTrack(const juce::String& name, juce::Colour newColour) {

        colour = newColour;
        title.setText(name, juce::dontSendNotification);
        preset.setName(name + " instrument");
        output.setName(name + " output");

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
        preset.setEnabled(!s.frozen());
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
        r.removeFromTop(4);
        outputLabel.setBounds(r.removeFromTop(12));
        output.setBounds(r.removeFromTop(24));
        r.removeFromTop(6);
        sendsLabel.setBounds(r.removeFromTop(12));

        // Up to four sends to a row, in slot order; more returns add a row.
        int shown = 0;
        for (const auto& n : returnNames) shown += n.isNotEmpty() ? 1 : 0;
        const int perRow = juce::jmin(sendsPerRow, juce::jmax(1, shown));
        const int cell = r.getWidth() / perRow;
        juce::Rectangle<int> knobs, modes;
        int column = perRow;
        for (std::size_t i = 0; i < sends.size(); ++i) {
            if (returnNames[i].isEmpty()) continue;
            if (column == perRow) { knobs = r.removeFromTop(28); modes = r.removeFromTop(16); column = 0; }
            sends[i].setBounds(knobs.removeFromLeft(cell).withSizeKeepingCentre(juce::jmin(cell, 28), 28));
            sendModes[i].setBounds(modes.removeFromLeft(cell).withSizeKeepingCentre(juce::jmin(cell - 2, 24), 16));
            ++column;
        }
        if (shown == 0) r.removeFromTop(44);
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
    juce::ComboBox& outputBox() { return output; }


private:
    void updateSendTooltip(int r) {
        const auto i = static_cast<std::size_t>(r);
        const auto letter = mixer_text::returnLetter(r);
        const auto destination = returnNames[i].isNotEmpty() && returnNames[i] != defaultReturnName(r)
            ? returnNames[i] + " (return " + letter + ")" : "return " + letter;
        const bool pre = sendModes[i].getToggleState();
        sends[i].setTooltip("Send to " + destination + ": " + (sends[i].getValue() <= 0 ? juce::String("off") : mixer_text::gainToText(sends[i].getValue()))
                            + (pre ? " (pre-fader)" : " (post-fader)") + ". Double-click to turn it off.");
        sendModes[i].setTooltip(pre ? "Send " + letter + " is pre-fader: it ignores this track's fader and pan. Click for post-fader."
                                    : "Send " + letter + " is post-fader: it follows this track's fader and pan. Click for pre-fader.");
    }

    TrackControls& controls;
    std::function<void(bool)> edited;
    juce::Label title, gainLabel, panLabel, sendsLabel, outputLabel;
    juce::ComboBox preset, output;

    juce::Slider gain, pan;
    static constexpr int sendsPerRow = 4;
    std::array<juce::Slider, maxReturns> sends;
    std::array<juce::TextButton, maxReturns> sendModes;
    std::array<juce::String, maxReturns> returnNames = [] {
        std::array<juce::String, maxReturns> names;
        for (int r = 0; r < defaultReturns; ++r) names[static_cast<std::size_t>(r)] = defaultReturnName(r);
        return names;
    }();
    juce::TextButton mute, solo;
    ui::LevelMeter meter;
    juce::Rectangle<int> scaleBounds;
    juce::Colour colour{ui::colours::accent};
};

// A return track's strip: its level (the same range as a track fader), pan, mute and meter.
// What the tracks send arrives here, through the return's FX (chosen in Plugins as
// "Return A FX" and so on), and goes on to the master. Its name button opens a menu to rename
// or remove it; the slot letter above the pan knob matches the send buttons on the tracks.
class ReturnChannelStrip final : public juce::Component {
public:
    ReturnChannelStrip(int returnIndex, ReturnControls& returnControls, std::function<void(bool)> editedCallback)
        : index(returnIndex), controls(returnControls), edited(std::move(editedCallback)) {
        for (auto* c : std::array<juce::Component*, 8>{&title, &slotLabel, &panLabel, &pan, &mute, &gainLabel, &gain, &meter}) addAndMakeVisible(c);
        const auto letter = mixer_text::returnLetter(index);
        title.setColour(juce::TextButton::buttonColourId, ui::colours::accent.withAlpha(0.75f));
        title.setColour(juce::TextButton::textColourOffId, ui::colours::accentInk);
        title.onClick = [this] { if (onMenu) onMenu(); };
        slotLabel.setText("SEND " + letter, juce::dontSendNotification);
        slotLabel.setFont(ui::font(10.0f, true));
        slotLabel.setColour(juce::Label::textColourId, ui::colours::accent);
        slotLabel.setJustificationType(juce::Justification::centred);
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
        setReturnName(defaultReturnName(index));
    }

    std::function<void()> onMenu;   // the name button: rename or remove

    void setReturnName(const juce::String& name) {
        title.setButtonText(name);
        title.setTooltip(name + " (send " + mixer_text::returnLetter(index) + "): click to rename or remove this return");
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
        slotLabel.setBounds(r.removeFromTop(12));
        r.removeFromTop(2);
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

    void paint(juce::Graphics& g) override { ui::drawCard(g, getLocalBounds().toFloat(), ui::colours::raised, 6.0f); }

    // For the tests.
    juce::TextButton& nameButton() { return title; }

private:
    int index;
    ReturnControls& controls;
    std::function<void(bool)> edited;
    juce::TextButton title;
    juce::Label slotLabel, panLabel, gainLabel;
    juce::Slider pan, gain;
    juce::TextButton mute;
    ui::LevelMeter meter;
};

// A group bus's strip: the tracks whose output is this bus arrive here after their faders, go
// through its FX (chosen in Plugins as "<name> FX (bus)"), this strip, and on to the master.
// Its name button opens a menu to rename or delete it.
class BusChannelStrip final : public juce::Component {
public:
    BusChannelStrip(BusControls& busControls, std::function<void(bool)> editedCallback)
        : controls(busControls), edited(std::move(editedCallback)) {
        for (auto* c : std::array<juce::Component*, 8>{&title, &panLabel, &pan, &mute, &solo, &gainLabel, &gain, &meter}) addAndMakeVisible(c);
        title.setColour(juce::TextButton::buttonColourId, ui::colours::accent.withAlpha(0.45f));
        title.setColour(juce::TextButton::textColourOffId, ui::colours::accentInk);
        title.onClick = [this] { if (onMenu) onMenu(); };
        for (auto* label : {&gainLabel, &panLabel}) {
            label->setFont(ui::font(10.0f, true));
            label->setColour(juce::Label::textColourId, ui::colours::textFaint);
            label->setJustificationType(juce::Justification::centred);
        }
        panLabel.setText("PAN", juce::dontSendNotification);
        gainLabel.setText("VOLUME", juce::dontSendNotification);
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setRange(-100, 100, 1);
        pan.setDoubleClickReturnValue(true, 0.0);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        pan.setColour(juce::Slider::rotarySliderFillColourId, ui::colours::accent);
        pan.onValueChange = [this] { controls.pan.store(static_cast<float>(pan.getValue() / 100)); edited(!pan.isMouseButtonDown()); };
        pan.onDragEnd = [this] { edited(true); };
        mute.setButtonText("M"); solo.setButtonText("S");
        mute.setClickingTogglesState(true); solo.setClickingTogglesState(true);
        mute.setColour(juce::TextButton::buttonOnColourId, ui::colours::mute);
        mute.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
        solo.setColour(juce::TextButton::buttonOnColourId, ui::colours::solo);
        solo.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff06162a));
        mute.onClick = [this] { controls.mute.store(mute.getToggleState()); edited(true); };
        solo.onClick = [this] { controls.solo.store(solo.getToggleState()); edited(true); };
        gain.setSliderStyle(juce::Slider::LinearVertical);
        gain.setRange(0, 200, 1);
        gain.setSkewFactorFromMidPoint(50.0);
        gain.setDoubleClickReturnValue(true, 100.0);
        gain.textFromValueFunction = [](double v) { return mixer_text::gainToText(v); };
        gain.valueFromTextFunction = [](const juce::String& t) { return mixer_text::textToGain(t, 200.0); };
        gain.onValueChange = [this] { controls.gain.store(static_cast<float>(gain.getValue() / 100)); edited(!gain.isMouseButtonDown()); };
        gain.onDragEnd = [this] { edited(true); };
        gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
        gain.setColour(juce::Slider::trackColourId, ui::colours::accent);
    }

    std::function<void()> onMenu;   // the name button: rename or delete

    void setBusName(const juce::String& name) {
        title.setButtonText(name);
        title.setTooltip(name + ": click to rename or delete this bus");
        pan.setName(name + " balance"); pan.setTooltip(name + " stereo balance. Double-click to centre.");
        mute.setName(name + " mute"); mute.setTooltip("Mute " + name);
        solo.setName(name + " solo"); solo.setTooltip("Solo " + name + " (and the tracks that feed it)");
        gain.setName(name + " volume"); gain.setTooltip(name + " volume, after its FX. Double-click to reset to 0 dB.");
    }
    void refreshSettings() {
        const auto s = controls.snapshot();
        gain.setValue(s.gain * 100, juce::dontSendNotification);
        pan.setValue(s.pan * 100, juce::dontSendNotification);
        mute.setToggleState(s.mute, juce::dontSendNotification);
        solo.setToggleState(s.solo, juce::dontSendNotification);
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
        auto ms = r.removeFromTop(24);
        mute.setBounds(ms.removeFromLeft(ms.getWidth() / 2 - 2));
        ms.removeFromLeft(4);
        solo.setBounds(ms);
        r.removeFromTop(10);
        gainLabel.setBounds(r.removeFromTop(12));
        r.removeFromTop(4);
        const int faderW = 34, meterW = 6;
        auto area = r.withSizeKeepingCentre(faderW + 8 + meterW * 2 + 2, r.getHeight());
        gain.setBounds(area.removeFromLeft(faderW));
        area.removeFromLeft(8);
        meter.setBounds(area.withTrimmedBottom(22).withSizeKeepingCentre(meterW * 2 + 2, area.getHeight() - 22));
    }

    void paint(juce::Graphics& g) override { ui::drawCard(g, getLocalBounds().toFloat(), ui::colours::raised, 6.0f); }

    // For the tests.
    juce::Slider& volume() { return gain; }
    juce::TextButton& soloButton() { return solo; }

private:
    BusControls& controls;
    std::function<void(bool)> edited;
    juce::TextButton title, mute, solo;
    juce::Label panLabel, gainLabel;
    juce::Slider pan, gain;
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
                    std::array<ReturnControls, maxReturns>& engineReturnControls,
                    MasterControls& masterControls,
                    std::array<BusControls, maxBuses>& engineBusControls)
        : controller(projectController), controls(engineControls), returnControls(engineReturnControls), busControls(engineBusControls),

          masterStrip(masterControls, [this](bool committed) { if (onMasterEdited) onMasterEdited(committed); }) {
        addAndMakeVisible(masterStrip);
        addAndMakeVisible(help);
        help.setText("Independent instrument, level, balance and sends for each Session track. Output sends a track to the master "
                     "or a group bus; each send feeds the return with its letter (bus and return FX are set in Plugins).",
            juce::dontSendNotification);
        help.setColour(juce::Label::textColourId, ui::colours::textFaint);
        help.setFont(ui::font(12.5f));
        addAndMakeVisible(addBusButton);
        addBusButton.setButtonText("Add bus");
        addBusButton.setTooltip("Add a group bus: route tracks to it with their Output menu, then mix them together");
        addBusButton.onClick = [this] { addBus(); };
        addAndMakeVisible(addReturnButton);
        addReturnButton.setButtonText("Add return");
        addReturnButton.setTooltip("Add a return track: every track gets a send knob for it, and it has its own FX in Plugins");
        addReturnButton.onClick = [this] { addReturn(); };

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
    std::function<void(int, bool)> onBusEdited;   // a bus strip (its slot)
    // A bus was added, renamed or deleted, or a track's output changed; the label names the edit.
    // The panel has already updated itself; the owner commits it.
    std::function<void(const juce::String&)> onRoutingChanged;
    std::function<void()> beforeBusDeleted;      // the owner can capture the bus's plugin state first
    std::function<void()> beforeReturnDeleted;   // the same, before a return is removed

    // Recreate strips after tracks are added, removed or reordered.
    void rebuild() {
        strips.clear();
        const auto& d = controller.document();
        for (int t = 0; t < d.trackCount() && t < static_cast<int>(controls.size()); ++t) {
            auto strip = std::make_unique<MixerChannelStrip>(controls[static_cast<std::size_t>(t)],
                [this, t](bool committed) { if (onTrackEdited) onTrackEdited(t, committed); });
            strip->onOutputChosen = [this, t](int output) { chooseOutput(t, output); };
            content.addAndMakeVisible(*strip);
            strips.push_back(std::move(strip));
        }
        rebuildBuses();
        rebuildReturns();
        refreshSettings();
        resized();
    }

    void refreshSettings() {
        masterStrip.refreshSettings();
        const auto& d = controller.document();
        // A strip that came or went (an undo, a loaded project) is rebuilt first.
        std::vector<int> used;
        for (int r = 0; r < maxReturns; ++r) if (d.returns[static_cast<std::size_t>(r)].used) used.push_back(r);
        std::vector<int> shown;
        for (const auto& [slot, strip] : returnStrips) shown.push_back(slot);
        if (used != shown) { rebuildReturns(); resized(); }
        for (auto& [slot, strip] : returnStrips) {
            strip->setReturnName(d.returns[static_cast<std::size_t>(slot)].name);
            strip->refreshSettings();
        }
        std::array<juce::String, maxBuses> busNames;
        for (std::size_t b = 0; b < busNames.size(); ++b) if (d.buses[b].used) busNames[b] = d.buses[b].name;
        std::array<juce::String, maxReturns> returnNames;
        for (std::size_t r = 0; r < returnNames.size(); ++r) if (d.returns[r].used) returnNames[r] = d.returns[r].name;
        for (std::size_t t = 0; t < strips.size() && t < d.tracks.size(); ++t) {
            strips[t]->setTrack(d.tracks[t].name, ui::trackColour(d.tracks[t].colour));
            strips[t]->setOutputs(busNames, d.tracks[t].settings.output);
            strips[t]->setReturns(returnNames);
            strips[t]->refreshSettings();
        }
        for (auto& [slot, strip] : busStrips) {
            strip->setBusName(d.buses[static_cast<std::size_t>(slot)].name);
            strip->refreshSettings();
        }
        addBusButton.setEnabled(busStrips.size() < static_cast<std::size_t>(maxBuses));
        addReturnButton.setEnabled(returnStrips.size() < static_cast<std::size_t>(maxReturns));
    }

    void tick() {
        if (!isShowing()) return;
        for (auto& strip : strips) strip->tick();
        for (auto& [slot, strip] : busStrips) strip->tick();
        for (auto& [slot, strip] : returnStrips) strip->tick();
        masterStrip.tick();
    }

    // Routing edits, from the strips' menus (and the tests). Each returns false when nothing changed.
    int addBus() {
        const int b = controller.addBus();
        if (b >= 0) routingChanged("Add bus");
        return b;
    }
    bool renameBus(int b, const juce::String& name) {
        if (!controller.renameBus(b, name)) return false;
        routingChanged("Rename bus");
        return true;
    }
    bool deleteBus(int b) {
        if (b < 0 || b >= maxBuses || !controller.document().buses[static_cast<std::size_t>(b)].used) return false;
        if (beforeBusDeleted) beforeBusDeleted();
        if (!controller.deleteBus(b)) return false;
        routingChanged("Delete bus");
        return true;
    }
    int addReturn() {
        const int r = controller.addReturn();
        if (r >= 0) routingChanged("Add return");
        return r;
    }
    bool renameReturn(int r, const juce::String& name) {
        if (!controller.renameReturn(r, name)) return false;
        routingChanged("Rename return");
        return true;
    }
    bool deleteReturn(int r) {
        if (r < 0 || r >= maxReturns || !controller.document().returns[static_cast<std::size_t>(r)].used) return false;
        if (beforeReturnDeleted) beforeReturnDeleted();
        if (!controller.deleteReturn(r)) return false;
        routingChanged("Remove return");
        return true;
    }
    bool chooseOutput(int track, int output) {
        if (!controller.setTrackOutput(track, output)) { refreshSettings(); return false; }
        routingChanged("Change track output");
        return true;
    }


    void resized() override {
        auto area = getLocalBounds().reduced(16, 12);
        auto header = area.removeFromTop(22);
        addBusButton.setBounds(header.removeFromRight(90));
        header.removeFromRight(gap);
        addReturnButton.setBounds(header.removeFromRight(100));
        header.removeFromRight(gap);
        help.setBounds(header);
        area.removeFromTop(10);

        masterStrip.setBounds(area.removeFromRight(minStripWidth));
        area.removeFromRight(gap);
        // The returns stay in view beside the master, narrower when there are many of them.
        if (!returnStrips.empty()) {
            const int count = static_cast<int>(returnStrips.size());
            const int width = juce::jlimit(minReturnStripWidth, returnStripWidth, (area.getWidth() * 2 / 5 - (count - 1) * 4) / count);
            for (auto it = returnStrips.rbegin(); it != returnStrips.rend(); ++it) {
                it->second->setBounds(area.removeFromRight(width));
                area.removeFromRight(std::next(it) == returnStrips.rend() ? gap * 2 : 4);
            }
        }
        viewport.setBounds(area);
        // The group buses scroll with the tracks, after them.
        const int busWidth = busStrips.empty() ? 0 : gap * 2 + static_cast<int>(busStrips.size()) * (returnStripWidth + 4) - 4;
        const int count = juce::jmax(1, static_cast<int>(strips.size()));
        const int stripWidth = juce::jlimit(minStripWidth, 168, (area.getWidth() - busWidth - (count - 1) * gap) / count);
        const int tracksWidth = count * stripWidth + (count - 1) * gap;
        const int contentWidth = tracksWidth + busWidth;
        const int height = area.getHeight() - (contentWidth > area.getWidth() ? 12 : 0);
        content.setSize(contentWidth, height);
        for (std::size_t i = 0; i < strips.size(); ++i)
            strips[i]->setBounds(static_cast<int>(i) * (stripWidth + gap), 0, stripWidth, height);
        int x = tracksWidth + gap * 2;
        for (auto& [slot, strip] : busStrips) { strip->setBounds(x, 0, returnStripWidth, height); x += returnStripWidth + 4; }
    }


    void paint(juce::Graphics& g) override { g.fillAll(ui::colours::surface); }

    // For the tests.
    MixerChannelStrip* trackStrip(int t) { return t >= 0 && t < static_cast<int>(strips.size()) ? strips[static_cast<std::size_t>(t)].get() : nullptr; }
    ReturnChannelStrip* returnStrip(int r) {
        for (auto& [slot, strip] : returnStrips) if (slot == r) return strip.get();
        return nullptr;
    }
    int returnStripCount() const noexcept { return static_cast<int>(returnStrips.size()); }
    juce::TextButton& addReturnControl() { return addReturnButton; }
    BusChannelStrip* busStrip(int b) {
        for (auto& [slot, strip] : busStrips) if (slot == b) return strip.get();
        return nullptr;
    }
    int busStripCount() const noexcept { return static_cast<int>(busStrips.size()); }
    juce::TextButton& addBusControl() { return addBusButton; }

private:
    static constexpr int gap = 10, minStripWidth = 118, returnStripWidth = 84, minReturnStripWidth = 64;

    void rebuildReturns() {
        returnStrips.clear();
        const auto& d = controller.document();
        for (int r = 0; r < maxReturns; ++r) {
            if (!d.returns[static_cast<std::size_t>(r)].used) continue;
            auto strip = std::make_unique<ReturnChannelStrip>(r, returnControls[static_cast<std::size_t>(r)],
                [this, r](bool committed) { if (onReturnEdited) onReturnEdited(r, committed); });
            strip->onMenu = [this, r] { showReturnMenu(r); };
            addAndMakeVisible(*strip);
            returnStrips.emplace_back(r, std::move(strip));
        }
    }
    void showReturnMenu(int r) {
        auto* strip = returnStrip(r);
        if (strip == nullptr) return;
        juce::PopupMenu menu;
        menu.addItem(1, "Rename...");
        menu.addItem(2, "Remove return (its sends and FX go with it)");
        juce::Component::SafePointer<TrackMixerPanel> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(strip), [safe, r](int result) {
            if (safe == nullptr) return;
            if (result == 2) safe->deleteReturn(r);
            else if (result == 1) safe->askReturnName(r);
        });
    }
    void askReturnName(int r) {
        if (r < 0 || r >= maxReturns || !controller.document().returns[static_cast<std::size_t>(r)].used) return;
        auto* window = new juce::AlertWindow("Rename return", {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", controller.document().returns[static_cast<std::size_t>(r)].name);
        window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TrackMixerPanel> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create(
            [safe, window, r](int result) {
                if (safe != nullptr && result == 1) safe->renameReturn(r, window->getTextEditorContents("name"));
            }), true);
    }

    void rebuildBuses() {
        busStrips.clear();
        const auto& d = controller.document();
        for (int b = 0; b < maxBuses; ++b) {
            if (!d.buses[static_cast<std::size_t>(b)].used) continue;
            auto strip = std::make_unique<BusChannelStrip>(busControls[static_cast<std::size_t>(b)],
                [this, b](bool committed) { if (onBusEdited) onBusEdited(b, committed); });
            strip->onMenu = [this, b] { showBusMenu(b); };
            content.addAndMakeVisible(*strip);
            busStrips.emplace_back(b, std::move(strip));
        }
    }
    void routingChanged(const juce::String& label) {
        rebuildBuses();
        rebuildReturns();
        refreshSettings();
        resized();
        if (onRoutingChanged) onRoutingChanged(label);
    }
    void showBusMenu(int b) {
        auto* strip = busStrip(b);
        if (strip == nullptr) return;
        juce::PopupMenu menu;
        menu.addItem(1, "Rename...");
        menu.addItem(2, "Delete bus (its tracks go to the master)");
        juce::Component::SafePointer<TrackMixerPanel> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(strip), [safe, b](int result) {
            if (safe == nullptr) return;
            if (result == 2) safe->deleteBus(b);
            else if (result == 1) safe->askBusName(b);
        });
    }
    void askBusName(int b) {
        if (b < 0 || b >= maxBuses || !controller.document().buses[static_cast<std::size_t>(b)].used) return;
        auto* window = new juce::AlertWindow("Rename bus", {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", controller.document().buses[static_cast<std::size_t>(b)].name);
        window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TrackMixerPanel> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create(
            [safe, window, b](int result) {
                if (safe != nullptr && result == 1) safe->renameBus(b, window->getTextEditorContents("name"));
            }), true);
    }

    ProjectController& controller;
    std::array<TrackControls, ToneEngine::sessionTracks>& controls;
    std::array<ReturnControls, maxReturns>& returnControls;
    std::array<BusControls, maxBuses>& busControls;

    MasterChannelStrip masterStrip;
    std::vector<std::pair<int, std::unique_ptr<ReturnChannelStrip>>> returnStrips;   // (slot, strip), in slot order
    juce::Label help;
    juce::TextButton addBusButton, addReturnButton;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<MixerChannelStrip>> strips;
    std::vector<std::pair<int, std::unique_ptr<BusChannelStrip>>> busStrips;   // (slot, strip), in slot order
};


} // namespace alien
