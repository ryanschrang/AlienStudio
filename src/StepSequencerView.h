#pragma once
#include "ProjectController.h"
#include "StudioWidgets.h"
#include "SynthEngine.h"
#include <cmath>
#include <functional>

namespace alien {

// One pad in the step sequencer row. Groups of four alternate shade so beats
// read at a glance; the pad shows its note and velocity and lights an LED
// while the playhead is on it.
class SequencerStepButton final : public juce::ToggleButton {
public:
    SequencerStepButton(const juce::String& label, int index) : juce::ToggleButton(label), stepIndex(index) {}

    void setPlaybackPosition(bool isCurrent) {
        if (playbackPosition == isCurrent) return;
        playbackPosition = isCurrent;
        repaint();
    }

    void setStepInfo(const juce::String& note, float velocity01, bool isSelected) {
        if (note == noteName && std::abs(velocity01 - velocity) < 0.001f && isSelected == selected) return;
        noteName = note; velocity = velocity01; selected = isSelected;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override {
        namespace col = ui::colours;
        const auto bounds = getLocalBounds().toFloat().reduced(1.5f);
        const bool on = getToggleState();
        const bool altGroup = (stepIndex / 4) % 2 == 1;
        auto base = on ? col::accent : juce::Colour(altGroup ? 0xff24262c : 0xff2c2f36);
        if (playbackPosition) base = base.brighter(on ? 0.18f : 0.22f);
        if (isButtonDown) base = base.brighter(0.12f);
        else if (isMouseOver) base = base.brighter(0.06f);
        g.setGradientFill(juce::ColourGradient(base.brighter(0.06f), 0.0f, bounds.getY(),
                                               base.darker(on ? 0.18f : 0.1f), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);

        const auto led = juce::Rectangle<float>(bounds.getWidth() * 0.36f, 3.0f)
                             .withCentre({bounds.getCentreX(), bounds.getY() + 6.0f});
        g.setColour(playbackPosition ? col::warning : (on ? col::accentInk.withAlpha(0.28f) : col::inset));
        g.fillRoundedRectangle(led, 1.5f);

        const auto ink = on ? col::accentInk : col::textDim;
        auto inner = bounds.reduced(8.0f, 6.0f).withTrimmedTop(8.0f);
        g.setColour(ink);
        g.setFont(ui::font(13.0f, true));
        g.drawText(getButtonText(), inner.removeFromTop(18.0f), juce::Justification::centredLeft, false);
        if (inner.getHeight() > 26.0f) {
            g.setColour(on ? ink.withAlpha(0.8f) : col::textFaint);
            g.setFont(ui::monoFont(11.0f));
            g.drawText(noteName, inner.removeFromTop(16.0f), juce::Justification::centredLeft, false);
        }
        if (on) {
            const auto bar = juce::Rectangle<float>(bounds.getX() + 8.0f, bounds.getBottom() - 9.0f,
                                                    bounds.getWidth() - 16.0f, 3.0f);
            g.setColour(col::accentInk.withAlpha(0.2f));
            g.fillRoundedRectangle(bar, 1.5f);
            g.setColour(col::accentInk.withAlpha(0.7f));
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, velocity)), 1.5f);
        }
        if (selected) {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.6f);
        }
        if (hasKeyboardFocus(true)) {
            g.setColour(col::accent);
            g.drawRoundedRectangle(bounds.reduced(3.0f), 3.5f, 1.0f);
        }
    }

private:
    int stepIndex{};
    bool playbackPosition{}, selected{};
    juce::String noteName;
    float velocity{0.8f};
};

// The 16-step sequencer page: channel card, pad row and step inspector.
class StepSequencerView final : public juce::Component {
public:
    explicit StepSequencerView(ProjectController& projectController) : controller(projectController) {
        namespace col = ui::colours;
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new SequencerStepButton(juce::String(i + 1), i));
            button->setName("Sequencer step " + juce::String(i + 1));
            button->setTooltip("Toggle sixteenth-note step " + juce::String(i + 1));
            button->setTitle("Step " + juce::String(i + 1));
            addAndMakeVisible(button);
            button->onClick = [this, button, i] {
                controller.setStepOn(i, button->getToggleState());
                selectStep(i);
                if (onEdited) onEdited();
            };
        }
        for (auto* label : {&selectedStepLabel, &stepPitchLabel, &stepVelocityLabel}) {
            addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true));
            label->setColour(juce::Label::textColourId, col::textFaint);
        }
        selectedStepLabel.setText("STEP", juce::dontSendNotification);
        addAndMakeVisible(stepSelector);
        stepSelector.setName("Selected sequencer step");
        for (int i = 0; i < 16; ++i) stepSelector.addItem("Step " + juce::String(i + 1), i + 1);
        stepSelector.onChange = [this] { selectStep(stepSelector.getSelectedId() - 1); };
        stepPitchLabel.setText("PITCH", juce::dontSendNotification);
        addAndMakeVisible(stepPitch);
        stepPitch.setName("Selected step MIDI pitch");
        stepPitch.setTooltip("Set the selected step pitch from MIDI note 0 to 127");
        stepPitch.setRange(0, 127, 1);
        stepPitch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 24);
        stepPitch.onValueChange = [this] {
            controller.setStepPitch(selectedStep, static_cast<int>(stepPitch.getValue()));
            updateInspectorText();
            refreshPadInfo();
        };
        stepPitch.onDragEnd = [this] { if (onEdited) onEdited(); };
        addAndMakeVisible(stepPitchName);
        stepPitchName.setName("Selected step note name");
        stepPitchName.setFont(ui::monoFont(14.0f, true));
        stepPitchName.setColour(juce::Label::textColourId, col::accent);
        stepPitchName.setJustificationType(juce::Justification::centred);
        stepVelocityLabel.setText("VELOCITY", juce::dontSendNotification);
        addAndMakeVisible(stepVelocity);
        stepVelocity.setName("Selected step velocity");
        stepVelocity.setTooltip("Set the selected step velocity from 1 to 127");
        stepVelocity.setRange(1, 127, 1);
        stepVelocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 24);
        stepVelocity.onValueChange = [this] {
            controller.setStepVelocity(selectedStep, static_cast<int>(stepVelocity.getValue()));
            refreshPadInfo();
        };
        stepVelocity.onDragEnd = [this] { if (onEdited) onEdited(); };
        addAndMakeVisible(stepHelp);
        stepHelp.setText("Click a pad to toggle it and select it for editing. The step row plays the live synth.",
                         juce::dontSendNotification);
        stepHelp.setFont(ui::font(12.5f));
        stepHelp.setColour(juce::Label::textColourId, col::textFaint);
        stepHelp.setJustificationType(juce::Justification::centredLeft);
        refresh();
    }

    // Called after a committed edit (for undo/dirty tracking).
    std::function<void()> onEdited;

    // Re-read everything from the document.
    void refresh() {
        const auto& steps = controller.document().steps;
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setToggleState(steps.on[static_cast<std::size_t>(i)], juce::dontSendNotification);
        selectStep(selectedStep);
        repaint();
    }

    void setPlayhead(int step) {
        for (int i = 0; i < pads.size(); ++i) pads[i]->setPlaybackPosition(i == step);
    }

    void setPlaying(bool isPlaying) {
        if (playing == isPlaying) return;
        playing = isPlaying;
        repaint(channelCardBounds);
    }

    juce::String subtitle() const {
        return juce::String(juce::CharPointer_UTF8("16 steps  \xc2\xb7  1/16 notes  \xc2\xb7  ")) + presetName();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(20, 16);
        stepHelp.setBounds(r.removeFromTop(22));
        r.removeFromTop(14);
        const bool singleRow = r.getWidth() >= 860;
        const int channelWidth = r.getWidth() >= 1000 ? 150 : 0;
        const int rowHeight = juce::jlimit(96, 150, (r.getHeight() - 140) / (singleRow ? 2 : 3));
        auto rack = r.removeFromTop(singleRow ? rowHeight : rowHeight * 2 + 8);
        channelCardBounds = rack.removeFromLeft(channelWidth);
        if (channelWidth > 0) rack.removeFromLeft(12);
        const int perRow = singleRow ? 16 : 8;
        const int groupGap = 10, padGap = 4;
        const int groups = perRow / 4;
        const int padW = (rack.getWidth() - (groups - 1) * groupGap - (perRow - groups) * padGap) / perRow;
        const int padH = singleRow ? rack.getHeight() : (rack.getHeight() - 8) / 2;
        for (int i = 0; i < pads.size(); ++i) {
            const int column = i % perRow, row = i / perRow;
            const int x = rack.getX() + column * (padW + padGap) + (column / 4) * (groupGap - padGap);
            pads[i]->setBounds(x, rack.getY() + row * (padH + 8), padW, padH);
        }
        r.removeFromTop(18);
        auto inspector = r.removeFromTop(104);
        inspectorBounds = inspector;
        auto row = inspector.reduced(16, 0).withTrimmedTop(56).withHeight(30);
        auto stepColumn = row.removeFromLeft(110);
        selectedStepLabel.setBounds(stepColumn.getX(), stepColumn.getY() - 18, 100, 16);
        stepSelector.setBounds(stepColumn.withWidth(100));
        row.removeFromLeft(14);
        const int sliderWidth = (row.getWidth() - 70 - 24) / 2;
        auto pitchColumn = row.removeFromLeft(sliderWidth);
        stepPitchLabel.setBounds(pitchColumn.getX(), pitchColumn.getY() - 18, 100, 16);
        stepPitch.setBounds(pitchColumn);
        stepPitchName.setBounds(row.removeFromLeft(64));
        row.removeFromLeft(24);
        stepVelocityLabel.setBounds(row.getX(), row.getY() - 18, 100, 16);
        stepVelocity.setBounds(row);
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::surface);
        if (!channelCardBounds.isEmpty()) {
            auto card = channelCardBounds.toFloat();
            ui::drawCard(g, card, col::raised);
            g.setColour(col::accent);
            g.fillRoundedRectangle(card.removeFromLeft(4.0f).reduced(0.0f, 1.0f), 2.0f);
            auto text = channelCardBounds.reduced(14, 12);
            ui::drawCaption(g, "Channel", text.removeFromTop(14));
            g.setColour(col::text);
            g.setFont(ui::font(14.0f, true));
            g.drawText("Step Synth", text.removeFromTop(22), juce::Justification::centredLeft, true);
            g.setColour(col::textDim);
            g.setFont(ui::font(12.5f));
            g.drawText(presetName(), text.removeFromTop(18), juce::Justification::centredLeft, true);
            g.setColour(col::textFaint);
            g.drawText(playing ? juce::String("Playing")
                               : juce::String(juce::CharPointer_UTF8("1 bar  \xc2\xb7  16 steps")),
                       text.removeFromTop(18), juce::Justification::centredLeft, true);
        }
        ui::drawCard(g, inspectorBounds.toFloat(), col::raised);
        ui::drawCaption(g, "Step " + juce::String(selectedStep + 1) + " inspector",
                        inspectorBounds.reduced(16, 0).removeFromTop(30), col::textDim);
    }

private:
    juce::String presetName() const {
        const int index = juce::jlimit(0, static_cast<int>(presetNames.size()) - 1, controller.document().livePreset);
        return presetNames[static_cast<std::size_t>(index)];
    }

    void selectStep(int step) {
        selectedStep = juce::jlimit(0, 15, step);
        const auto& steps = controller.document().steps;
        const auto index = static_cast<std::size_t>(selectedStep);
        stepSelector.setSelectedId(selectedStep + 1, juce::dontSendNotification);
        stepPitch.setValue(steps.pitch[index], juce::dontSendNotification);
        stepVelocity.setValue(steps.velocity[index], juce::dontSendNotification);
        updateInspectorText();
        refreshPadInfo();
        repaint(inspectorBounds);
    }

    void updateInspectorText() {
        const int pitch = controller.document().steps.pitch[static_cast<std::size_t>(selectedStep)];
        stepPitchName.setText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 4), juce::dontSendNotification);
    }

    void refreshPadInfo() {
        const auto& steps = controller.document().steps;
        for (int i = 0; i < pads.size(); ++i) {
            const auto index = static_cast<std::size_t>(i);
            pads[i]->setStepInfo(juce::MidiMessage::getMidiNoteName(steps.pitch[index], true, true, 4),
                                 static_cast<float>(steps.velocity[index]) / 127.0f, i == selectedStep);
        }
    }

    ProjectController& controller;
    juce::OwnedArray<SequencerStepButton> pads;
    juce::Label stepHelp, selectedStepLabel, stepPitchLabel, stepPitchName, stepVelocityLabel;
    juce::ComboBox stepSelector;
    juce::Slider stepPitch{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Slider stepVelocity{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Rectangle<int> channelCardBounds, inspectorBounds;
    int selectedStep{};
    bool playing{};
};

} // namespace alien
