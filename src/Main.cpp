#include <JuceHeader.h>
#include "audio/AudioEngine.h"
#include <array>

class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent() {
        setSize(960, 500);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO  |  Polyphonic Synth + Step Sequencer", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(play);
        play.setButtonText("Play / Stop");
        play.onClick = [this] {
            const bool running = !engine.sequencer.playing.load();
            engine.sequencer.playing.store(running);
            if (!running) engine.sequencer.requestPanic();
        };
        addAndMakeVisible(preset);
        preset.addItemList(juce::StringArray{"Lead / Saw", "Bass / Square", "Pluck / Sine"}, 1);
        preset.setSelectedId(1);
        preset.onChange = [this] { engine.sequencer.selectedPreset.store(preset.getSelectedId() - 1); };
        addAndMakeVisible(velocity);
        velocity.setRange(1, 127, 1);
        velocity.setValue(100);
        velocity.setTextValueSuffix(" velocity");
        for (std::size_t i = 0; i < keys.size(); ++i) {
            auto& key = keys[i];
            addAndMakeVisible(key);
            key.setButtonText(juce::StringArray{"C3", "D3", "E3", "F3", "G3", "A3", "B3", "C4"}[static_cast<int>(i)]);
            key.onStateChange = [this, i] {
                const bool down = keys[i].isDown();
                if (held[i] != down) { held[i] = down; sendNote(down, notes[i], 2); }
            };
        }
        addAndMakeVisible(chord);
        chord.setButtonText("Hold C major");
        chord.onStateChange = [this] {
            const bool down = chord.isDown();
            if (chordHeld != down) {
                chordHeld = down;
                for (const int note : {60, 64, 67}) sendNote(down, note, 3);
            }
        };
        addAndMakeVisible(panicButton);
        panicButton.setButtonText("All notes off");
        panicButton.onClick = [this] { engine.sequencer.requestPanic(); };
        addAndMakeVisible(status);
        status.setText("Hold a key or chord to audition. Presets apply to new notes.", juce::dontSendNotification);
        addAndMakeVisible(tempo);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120);
        tempo.onValueChange = [this] { engine.sequencer.bpm.store(static_cast<int>(tempo.getValue())); };
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new juce::ToggleButton(juce::String(i + 1)));
            addAndMakeVisible(button);
            button->onClick = [this, button, i] { engine.sequencer.steps[i].store(button->getToggleState()); };
        }
        engine.sequencer.steps[0].store(true); pads[0]->setToggleState(true, juce::dontSendNotification);
        engine.sequencer.steps[4].store(true); pads[4]->setToggleState(true, juce::dontSendNotification);
        engine.sequencer.steps[8].store(true); pads[8]->setToggleState(true, juce::dontSendNotification);
        engine.sequencer.steps[12].store(true); pads[12]->setToggleState(true, juce::dontSendNotification);
        const auto error = audio.initialiseWithDefaultDevices(0, 2);
        if (error.isNotEmpty()) status.setText("Audio device error: " + error, juce::dontSendNotification);
        audio.addAudioCallback(&engine);
        resized();
        startTimerHz(20);
    }
    ~MainComponent() override {
        stopTimer();
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
    }
    void resized() override {
        title.setBounds(20, 20, getWidth() - 40, 50);
        play.setBounds(30, 95, 130, 38);
        tempo.setBounds(180, 95, 260, 38);
        preset.setBounds(30, 140, 185, 30);
        velocity.setBounds(230, 140, 260, 30);
        panicButton.setBounds(700, 140, 220, 30);
        const int width = (getWidth() - 60) / 8;
        for (std::size_t i = 0; i < keys.size(); ++i)
            keys[i].setBounds(30 + static_cast<int>(i) * width, 320, width - 6, 48);
        chord.setBounds(30, 385, 180, 38);
        status.setBounds(225, 382, getWidth() - 250, 48);
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setBounds(30 + (i % 8) * width, 175 + (i / 8) * 65, width - 6, 50);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151722));
        g.setColour(juce::Colours::lightgreen);
        g.drawText("BPM: " + juce::String(engine.sequencer.bpm.load()) + "   |   Active step: " +
            juce::String(engine.sequencer.currentStep.load() + 1), 470, 95, 440, 40, juce::Justification::centredLeft);
    }
private:
    void timerCallback() override { repaint(); }
    void sendNote(bool down, int note, int channel) {
        engine.sequencer.controls.push({
            static_cast<std::uint8_t>((down ? 0x90 : 0x80) | (channel - 1)),
            static_cast<std::uint8_t>(note),
            static_cast<std::uint8_t>(down ? velocity.getValue() : 0)});
    }
    AudioEngine engine;
    juce::AudioDeviceManager audio;
    juce::ComboBox preset;
    juce::Slider velocity{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label status;
    std::array<juce::TextButton, 8> keys;
    std::array<bool, 8> held{};
    static constexpr std::array<int, 8> notes{48, 50, 52, 53, 55, 57, 59, 60};
    juce::TextButton chord, panicButton;
    bool chordHeld{false};
    juce::Label title;
    juce::TextButton play;
    juce::Slider tempo{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::OwnedArray<juce::ToggleButton> pads;
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
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(AlienStudioApp)
