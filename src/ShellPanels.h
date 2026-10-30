#pragma once
#include "StudioWidgets.h"
#include "MidiInputPanel.h"
#include "SynthEngine.h"
#include "PresetBrowser.h"
#include <functional>

// Smaller shell panels around the editors: the keyboard dock, the status bar
// and the Devices page.
namespace alien {

// On-screen keyboard with octave controls and the computer-keyboard hint.
class KeyboardDock final : public juce::Component {
public:
    static constexpr int fullHeight = 132, compactHeight = 110;

    explicit KeyboardDock(juce::MidiKeyboardState& state)
        : keyboard(state, juce::MidiKeyboardComponent::horizontalKeyboard) {
        namespace col = ui::colours;
        addAndMakeVisible(keyboard);
        keyboard.clearKeyMappings();
        keyboard.setAvailableRange(0, 127);
        keyboard.setOctaveForMiddleC(4);
        keyboard.setLowestVisibleKey(48);
        keyboard.setName("On-screen keyboard");
        keyboard.setVelocity(0.8f, true);
        for (auto* b : {&octaveDown, &octaveUp}) {
            addAndMakeVisible(b);
            b->setIconOnly(true);
        }
        octaveDown.setButtonText("Octave -"); octaveUp.setButtonText("Octave +");
        octaveDown.setGlyph(ui::Glyph::minus); octaveUp.setGlyph(ui::Glyph::plus);
        octaveDown.setTooltip("Octave down (Z)"); octaveUp.setTooltip("Octave up (X)");
        octaveDown.onClick = [this] { if (onOctaveShift) onOctaveShift(-1); };
        octaveUp.onClick = [this] { if (onOctaveShift) onOctaveShift(1); };
        addAndMakeVisible(octaveReadout);
        octaveReadout.setFont(ui::monoFont(13.0f, true));
        octaveReadout.setColour(juce::Label::textColourId, col::accent);
        octaveReadout.setJustificationType(juce::Justification::centred);
        octaveReadout.setTooltip("Lowest computer-keyboard note");
        addAndMakeVisible(help);
        help.setFont(ui::font(12.0f));
        help.setColour(juce::Label::textColourId, col::textFaint);
        help.setJustificationType(juce::Justification::centredRight);
        help.setText(juce::String(juce::CharPointer_UTF8(
            "Computer keys  A W S E D F T G Y H U J  \xc2\xb7  Z / X octave  \xc2\xb7  works outside text fields")),
            juce::dontSendNotification);
    }

    std::function<void(int)> onOctaveShift;

    void setOctave(int octave) {
        octave_ = octave;
        octaveReadout.setText("C" + juce::String(octave), juce::dontSendNotification);
        keyboard.setLowestVisibleKey(12 * (octave + 1));
    }

    void resized() override {
        auto dock = getLocalBounds();
        auto header = dock.removeFromTop(34).reduced(12, 5);
        header.removeFromLeft(78); // "Keyboard" caption.
        octaveDown.setBounds(header.removeFromLeft(28));
        octaveReadout.setBounds(header.removeFromLeft(44));
        octaveUp.setBounds(header.removeFromLeft(28));
        header.removeFromLeft(12);
        help.setBounds(header);
        keyboard.setBounds(dock.reduced(12, 0).withTrimmedBottom(10));
        keyboard.setKeyWidth(juce::jlimit(16.0f, 34.0f, static_cast<float>(keyboard.getWidth()) / 36.0f));
        setOctave(octave_);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(ui::colours::raised);
        g.fillAll();
        g.setColour(juce::Colours::black);
        g.fillRect(0, 0, getWidth(), 1);
        ui::drawCaption(g, "Keyboard", {14, 5, 72, 24}, ui::colours::textFaint);
    }

private:
    juce::MidiKeyboardComponent keyboard;
    ui::GlyphButton octaveDown, octaveUp;
    juce::Label help, octaveReadout;
    int octave_{4};
};

// Bottom status line: project state on the left, audio engine on the right.
class StatusBar final : public juce::Component {
public:
    static constexpr int height = 26;

    StatusBar() {
        namespace col = ui::colours;
        addAndMakeVisible(projectStatus);
        projectStatus.setFont(ui::font(12.0f));
        projectStatus.setColour(juce::Label::textColourId, col::textDim);
        projectStatus.setJustificationType(juce::Justification::centredLeft);
        projectStatus.setText("Unsaved session", juce::dontSendNotification);
        addAndMakeVisible(engineStatus);
        engineStatus.setFont(ui::monoFont(11.5f));
        engineStatus.setColour(juce::Label::textColourId, col::textFaint);
        engineStatus.setJustificationType(juce::Justification::centredRight);
    }

    void setProjectStatus(const juce::String& text, const juce::String& tooltip) {
        projectStatus.setText(text, juce::dontSendNotification);
        projectStatus.setTooltip(tooltip);
    }
    void setEngineStatus(const juce::String& text) { engineStatus.setText(text, juce::dontSendNotification); }

    enum class State { unsaved, busy, saved, edited };
    void setState(State s) { if (state != s) { state = s; repaint(); } }

    void resized() override {
        auto r = getLocalBounds().reduced(12, 0);
        engineStatus.setBounds(r.removeFromRight(360));
        projectStatus.setBounds(r.withTrimmedLeft(14));
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::window);
        g.setColour(juce::Colours::black);
        g.fillRect(0, 0, getWidth(), 1);
        const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({18.0f, getHeight() * 0.5f});
        if (state == State::edited) {
            // Hollow dot: saved before, but has unsaved changes.
            g.setColour(col::warning);
            g.drawEllipse(dot, 1.4f);
            return;
        }
        g.setColour(state == State::busy ? col::warning : state == State::saved ? col::accent : col::textFaint);
        g.fillEllipse(dot);
    }

private:
    juce::Label projectStatus, engineStatus;
    State state{State::unsaved};
};

// Devices page: the live/step synth preset and the MIDI input selector.
class DevicesView final : public juce::Component {
public:
    explicit DevicesView(MidiInputPipe& pipe) : midiInput(pipe) {
        namespace col = ui::colours;
        addAndMakeVisible(deviceHelp);
        deviceHelp.setText("Choose the live keyboard / step synth and MIDI input. Session instruments are in Mixer. Shift+Tab returns.",
                           juce::dontSendNotification);
        deviceHelp.setFont(ui::font(12.5f));
        deviceHelp.setColour(juce::Label::textColourId, col::textFaint);
        for (auto* label : {&midiLabel, &presetLabel}) {
            addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true));
            label->setColour(juce::Label::textColourId, col::textFaint);
        }
        midiLabel.setText("MIDI INPUT", juce::dontSendNotification);
        presetLabel.setText("LIVE / STEP SYNTH", juce::dontSendNotification);
        addAndMakeVisible(midiInput);
        addAndMakeVisible(browser);
        browser.onApply = [this](int index) { if (onPresetChanged) onPresetChanged(index); };
    }

    std::function<void(int)> onPresetChanged;

    void setPreset(int index) { browser.setCurrentPreset(index); }
    PresetBrowser& presets() noexcept { return browser; }
    MidiInputPanel& midi() noexcept { return midiInput; }

    void resized() override {
        auto r = getLocalBounds().reduced(20, 16);
        deviceHelp.setBounds(r.removeFromTop(22));
        r.removeFromTop(14);
        const int cardWidth = juce::jmin(560, r.getWidth());
        synthCard = r.removeFromTop(juce::jlimit(200, 320, r.getHeight() - 182)).withWidth(cardWidth);
        r.removeFromTop(14);
        midiCard = r.removeFromTop(juce::jmin(168, r.getHeight())).withWidth(cardWidth);
        auto synth = synthCard.reduced(18, 14).withTrimmedLeft(4);
        presetLabel.setBounds(synth.removeFromTop(16));
        synth.removeFromTop(10);
        browser.setBounds(synth);
        auto midi = midiCard.reduced(18, 14).withTrimmedLeft(4);
        midiLabel.setBounds(midi.removeFromTop(16));
        midi.removeFromTop(10);
        midiInput.setBounds(midi.removeFromTop(72));
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::surface);
        for (auto bounds : {synthCard, midiCard}) {
            auto card = bounds.toFloat();
            ui::drawCard(g, card, col::raised);
            g.setColour(bounds == synthCard ? col::accent : col::solo);
            g.fillRoundedRectangle(card.removeFromLeft(4.0f).reduced(0.0f, 1.0f), 2.0f);
        }
        g.setColour(col::textFaint);
        g.setFont(ui::font(12.0f));
        g.drawText("MPK mini profiles map pads and knobs; pitch, modulation and sustain route to the active instrument.",
                   midiCard.reduced(22, 14).removeFromBottom(18), juce::Justification::centredLeft, true);
    }

private:
    MidiInputPanel midiInput;
    juce::Label deviceHelp, presetLabel, midiLabel;
    PresetBrowser browser;
    juce::Rectangle<int> synthCard, midiCard;
};

} // namespace alien
