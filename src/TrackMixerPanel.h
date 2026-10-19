#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "TrackMixer.h"
#include "StudioWidgets.h"
#include <array>
#include <cmath>

namespace alien {

// Console-style mixer: one vertical channel strip per Session track with an
// instrument slot, pan knob, mute/solo, fader with dB scale and peak meter.
// Stored values are unchanged (gain 0..200 %, pan -100..100); only the
// presentation is in dB / L-C-R.
class TrackMixerPanel final : public juce::Component {
public:
    explicit TrackMixerPanel(std::array<TrackControls, 4>& controls) : tracks(controls) {
        addAndMakeVisible(help);
        help.setText("Independent instrument, level and balance for each Session track. "
                     "The live keyboard and step synth are set in Devices.",
            juce::dontSendNotification);
        help.setColour(juce::Label::textColourId, ui::colours::textFaint);
        help.setFont(ui::font(12.5f));
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto name = "Track " + juce::String(static_cast<int>(i + 1));
            auto& strip = strips[i];
            for (auto* component : std::array<juce::Component*, 9>{&strip.title, &strip.preset,
                &strip.gainLabel, &strip.gain, &strip.panLabel, &strip.pan, &strip.mute, &strip.solo, &strip.meter})
                addAndMakeVisible(component);
            strip.title.setText(name, juce::dontSendNotification);
            strip.title.setFont(ui::font(13.0f, true));
            strip.title.setJustificationType(juce::Justification::centred);
            strip.title.setColour(juce::Label::textColourId, ui::colours::accentInk);
            strip.title.setInterceptsMouseClicks(false, false);
            strip.preset.addItem("Lead (saw)", 1);
            strip.preset.addItem("Bass (square)", 2);
            strip.preset.addItem("Pluck (sine)", 3);
            strip.preset.setName(name + " instrument");
            strip.preset.setTooltip("Changes new notes on this track; held notes finish with their original sound");
            strip.preset.onChange = [this, i] { tracks[i].preset.store(strips[i].preset.getSelectedId() - 1); };

            strip.gainLabel.setText("VOLUME", juce::dontSendNotification);
            strip.gain.setSliderStyle(juce::Slider::LinearVertical);
            strip.gain.setRange(0, 200, 1);
            strip.gain.setSkewFactorFromMidPoint(50.0);
            strip.gain.setName(name + " volume");
            strip.gain.setTooltip("Track volume. Double-click to reset to 0 dB.");
            strip.gain.setDoubleClickReturnValue(true, 100.0);
            strip.gain.textFromValueFunction = [](double v) { return gainToText(v); };
            strip.gain.valueFromTextFunction = [](const juce::String& t) { return textToGain(t); };
            strip.gain.setColour(juce::Slider::trackColourId, ui::trackColour(static_cast<int>(i)));
            strip.gain.onValueChange = [this, i] { tracks[i].gain.store(static_cast<float>(strips[i].gain.getValue() / 100)); };
            strip.gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 20);

            strip.panLabel.setText("PAN", juce::dontSendNotification);
            strip.pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            strip.pan.setRange(-100, 100, 1);
            strip.pan.setName(name + " balance");
            strip.pan.setTooltip("Stereo balance. Double-click to centre.");
            strip.pan.setDoubleClickReturnValue(true, 0.0);
            strip.pan.textFromValueFunction = [](double v) {
                const int p = juce::roundToInt(v);
                return p == 0 ? juce::String("C") : (p < 0 ? "L" : "R") + juce::String(std::abs(p));
            };
            strip.pan.valueFromTextFunction = [](const juce::String& t) {
                const auto s = t.trim().toUpperCase();
                if (s.startsWith("L")) return -std::abs(s.substring(1).getDoubleValue());
                if (s.startsWith("R")) return std::abs(s.substring(1).getDoubleValue());
                return s == "C" ? 0.0 : s.getDoubleValue();
            };
            strip.pan.setColour(juce::Slider::rotarySliderFillColourId, ui::trackColour(static_cast<int>(i)));
            strip.pan.onValueChange = [this, i] { tracks[i].pan.store(static_cast<float>(strips[i].pan.getValue() / 100)); };
            strip.pan.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 18);

            for (auto* label : {&strip.gainLabel, &strip.panLabel}) {
                label->setFont(ui::font(10.0f, true));
                label->setColour(juce::Label::textColourId, ui::colours::textFaint);
                label->setJustificationType(juce::Justification::centred);
            }
            strip.mute.setButtonText("M"); strip.solo.setButtonText("S");
            strip.mute.setTooltip("Mute"); strip.solo.setTooltip("Solo");
            strip.mute.setName(name + " mute"); strip.solo.setName(name + " solo");
            strip.mute.setColour(juce::TextButton::buttonOnColourId, ui::colours::mute);
            strip.solo.setColour(juce::TextButton::buttonOnColourId, ui::colours::solo);
            strip.mute.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
            strip.solo.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff06162a));
            strip.mute.setClickingTogglesState(true); strip.solo.setClickingTogglesState(true);
            strip.mute.onClick = [this, i] { tracks[i].mute.store(strips[i].mute.getToggleState()); };
            strip.solo.onClick = [this, i] { tracks[i].solo.store(strips[i].solo.getToggleState()); };
        }
        refreshSettings();
        for (auto& strip : strips) { strip.gain.updateText(); strip.pan.updateText(); }
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
        if (!isShowing()) return;
        for (std::size_t i = 0; i < tracks.size(); ++i)
            strips[i].meter.setPeak(tracks[i].peak.load());
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16, 12);
        help.setBounds(area.removeFromTop(22));
        area.removeFromTop(10);
        const int stripWidth = juce::jlimit(112, 168, (area.getWidth() - 3 * gap) / 4);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto& s = strips[i];
            auto r = area.removeFromLeft(stripWidth).reduced(8, 8);
            area.removeFromLeft(gap);
            stripBounds[i] = r.expanded(8, 8);
            s.title.setBounds(r.removeFromBottom(26));
            r.removeFromBottom(8);
            s.preset.setBounds(r.removeFromTop(26));
            r.removeFromTop(8);
            s.panLabel.setBounds(r.removeFromTop(12));
            s.pan.setBounds(r.removeFromTop(70).withSizeKeepingCentre(64, 70));
            r.removeFromTop(8);
            auto ms = r.removeFromTop(24);
            s.mute.setBounds(ms.removeFromLeft(ms.getWidth() / 2 - 3));
            ms.removeFromLeft(6);
            s.solo.setBounds(ms);
            r.removeFromTop(10);
            s.gainLabel.setBounds(r.removeFromTop(12));
            r.removeFromTop(4);
            // Fader on the left, scale in the middle, meter on the right.
            auto faderArea = r;
            const int faderW = 40;
            const int meterW = 6;
            const int total = faderW + 26 + meterW * 2 + 2;
            faderArea = faderArea.withSizeKeepingCentre(total, faderArea.getHeight());
            s.gain.setBounds(faderArea.removeFromLeft(faderW));
            scaleBounds[i] = faderArea.removeFromLeft(26);
            s.meter.setBounds(faderArea.withTrimmedBottom(22).withSizeKeepingCentre(meterW * 2 + 2, faderArea.getHeight() - 22));
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::surface);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto r = stripBounds[i].toFloat();
            ui::drawCard(g, r, ui::colours::raised, 6.0f);
            // Coloured name plate behind the title label.
            const auto plate = strips[i].title.getBounds().toFloat();
            g.setColour(ui::trackColour(static_cast<int>(i)));
            g.fillRoundedRectangle(plate, 4.0f);
            // dB scale beside the fader, aligned with the fader's own value mapping.
            auto& fader = strips[i].gain;
            const auto sb = scaleBounds[i];
            g.setFont(ui::font(9.5f));
            for (const auto db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
                const double pct = 100.0 * std::pow(10.0, db / 20.0);
                if (pct > fader.getMaximum()) continue;
                const int y = fader.getY() + juce::roundToInt(fader.getPositionOfValue(pct));
                g.setColour(db == 0.0 ? ui::colours::textDim : ui::colours::textFaint);
                g.drawText(db > 0 ? "+" + juce::String(static_cast<int>(db)) : juce::String(static_cast<int>(db)),
                           sb.getX(), y - 6, sb.getWidth(), 12, juce::Justification::centred);
            }
        }
    }

private:
    static constexpr int gap = 10;

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

    struct Strip {
        juce::Label title, gainLabel, panLabel;
        juce::ComboBox preset;
        juce::Slider gain, pan;
        juce::TextButton mute, solo;
        ui::LevelMeter meter;
    };
    std::array<TrackControls, 4>& tracks;
    std::array<Strip, 4> strips;
    std::array<juce::Rectangle<int>, 4> stripBounds{}, scaleBounds{};
    juce::Label help;
};

} // namespace alien
