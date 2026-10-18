#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "MidiInputPanel.h"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener, private juce::ChangeListener {
public:
    MainComponent() {
        setSize(1120, 720);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO  |  Pattern Studio", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(midiInput);
        addAndMakeVisible(play);
        play.setButtonText("Play / Stop");
        play.onClick = [this] { releaseNotes(); engine.playing.store(!engine.playing.load()); };
        addAndMakeVisible(tempo);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120);
        tempo.onValueChange = [this] { engine.bpm.store(static_cast<int>(tempo.getValue())); };
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new juce::ToggleButton(juce::String(i + 1)));
            stepsPanel.addAndMakeVisible(button);
            button->onClick = [this, button, i] { engine.steps[i].store(button->getToggleState()); };
        }
        engine.steps[0].store(true); pads[0]->setToggleState(true, juce::dontSendNotification);
        engine.steps[4].store(true); pads[4]->setToggleState(true, juce::dontSendNotification);
        engine.steps[8].store(true); pads[8]->setToggleState(true, juce::dontSendNotification);
        engine.steps[12].store(true); pads[12]->setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(tabs);
        tabs.addTab("16-step sequencer", juce::Colour(0xff202736), &stepsPanel, false);
        tabs.addTab("Piano roll", juce::Colour(0xff202736), &pianoRoll, false);
        tabs.getTabbedButtonBar().addChangeListener(this);
        stepsPanel.addAndMakeVisible(stepHelp);
        stepHelp.setText("16 steps  |  Sixteenth notes  |  A3 synth pattern", juce::dontSendNotification);
        stepHelp.setJustificationType(juce::Justification::centred);
        pianoRoll.onChange = [this] { engine.pianoRollNotes.publish(pianoRoll.getPattern()); };
        pianoRoll.onAudition = [this](int note, float velocity) {
            if (velocity > 0) keyboardState.noteOn(3, note, velocity);
            else keyboardState.noteOff(3, note, 0);
        };
        engine.pianoRollNotes.publish(pianoRoll.getPattern());
        addAndMakeVisible(preset);
        preset.addItem("Lead (saw)", 1);
        preset.addItem("Bass (square)", 2);
        preset.addItem("Pluck (sine)", 3);
        preset.setSelectedId(1);
        preset.onChange = [this] { releaseNotes(); engine.preset.store(preset.getSelectedId() - 1); };
        addAndMakeVisible(keyboard);
        keyboard.clearKeyMappings();
        keyboard.setAvailableRange(0, 127);
        keyboard.setOctaveForMiddleC(4);
        keyboard.setLowestVisibleKey(48);
        typingInput.octaveChanged = [this] { updateOctave(); };
        for (auto* b : {&octaveDown, &octaveUp}) {
            addAndMakeVisible(b);
        }
        octaveDown.setButtonText("Octave -"); octaveUp.setButtonText("Octave +");
        octaveDown.onClick = [this] { typingInput.typing.shift(-1); updateOctave(); };
        octaveUp.onClick = [this] { typingInput.typing.shift(1); updateOctave(); };
        addAndMakeVisible(help);
        updateOctave();
        keyboard.setVelocity(0.8f, true);
        keyboardState.addListener(this);
        audio.initialiseWithDefaultDevices(0, 2);
        audio.addAudioCallback(&engine);
        startTimerHz(20);
        resized();
    }
    ~MainComponent() override {
        stopTimer();
        tabs.getTabbedButtonBar().removeChangeListener(this);
        releaseNotes();
        keyboardState.removeListener(this);
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
    }
    void resized() override {
        title.setBounds(20, 10, 250, 30);
        midiInput.setBounds(280, 10, getWidth() - 300, 32);
        play.setBounds(20, 52, 130, 34);
        tempo.setBounds(165, 52, 255, 34);
        preset.setBounds(440, 52, 190, 34);
        tabs.setBounds(20, 106, getWidth() - 40, getHeight() - 280);
        octaveDown.setBounds(20, getHeight() - 154, 100, 30);
        octaveUp.setBounds(130, getHeight() - 154, 100, 30);
        help.setBounds(245, getHeight() - 154, getWidth() - 265, 30);
        keyboard.setBounds(20, getHeight() - 110, getWidth() - 40, 90);
        const int width = (getWidth() - 100) / 8;
        stepHelp.setBounds(20, 40, getWidth() - 80, 35);
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setBounds(30 + (i % 8) * width, 110 + (i / 8) * 75, width - 8, 60);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151722));
        g.setColour(juce::Colours::lightgreen);
        const auto status = engine.patternMode.load() == 1 ? juce::String("Piano roll | 4 bars")
            : "Step " + juce::String(engine.currentStep.load() + 1) + " / 16";
        g.drawText("BPM: " + juce::String(engine.bpm.load()) + "   |   " + status,
            650, 52, getWidth() - 670, 34, juce::Justification::centredLeft);
    }
    void releaseNotes() {
        pianoRoll.releaseAudition();
        typingInput.release();
        keyboardState.allNotesOff(0);
        midiInput.resetTestButtons();
        engine.midi.requestReset();
        engine.notes.panic.store(true);
    }
private:
    void updateOctave() {
        help.setText("Play notes (outside text fields): A W S E D F T G Y H U J | Z/X octave | C"
                     + juce::String(typingInput.typing.octave()), juce::dontSendNotification);
        keyboard.setLowestVisibleKey(12 * (typingInput.typing.octave() + 1));
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        engine.patternMode.store(tabs.getCurrentTabIndex() == 1 ? 1 : 0);
        releaseNotes();
    }
    void timerCallback() override {
        midiInput.tick();
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        pianoRoll.setPlayheadStep(engine.pianoRollPosition.load() / 1000.0);
        repaint();
    }
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override {
        if (channel == 4) return; // MIDI highlight only; sound uses its own audio pipe.
        if (!keyboardState.isNoteOnForChannels(0x7 & ~(1 << (channel - 1)), note))
            engine.notes.push(note, velocity);
    }
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float) override {
        if (channel == 4) return;
        if (!keyboardState.isNoteOnForChannels(0x7, note)) engine.notes.push(note, 0);
    }
    ToneEngine engine;
    alien::MidiInputPanel midiInput{engine.midi};
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard{keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard};
    juce::TextButton octaveDown, octaveUp;
    juce::Label help;
    juce::ComboBox preset;
    juce::Label title;
    juce::TextButton play;
    juce::Slider tempo{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::OwnedArray<juce::ToggleButton> pads;
    juce::Component stepsPanel;
    juce::Label stepHelp;
    alien::PianoRollEditor pianoRoll;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    TypingPianoRouter typingInput{*this, keyboardState};
};

class AlienStudioApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Alien Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override {
        window = std::make_unique<Window>(getApplicationName());
    }
    void shutdown() override { window.reset(); }
private:
    class Window : public juce::DocumentWindow {
    public:
        explicit Window(juce::String name) : DocumentWindow(name, juce::Colours::black,
            juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(960, 650, 1920, 1440);
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                centreWithSize(juce::jmin(getWidth(), display->userArea.getWidth() - 40),
                               juce::jmin(getHeight(), display->userArea.getHeight() - 40));
            else centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void activeWindowStatusChanged() override {
            if (!isActiveWindow())
                if (auto* content = dynamic_cast<MainComponent*>(getContentComponent())) content->releaseNotes();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(AlienStudioApp)
