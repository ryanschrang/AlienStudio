#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    std::array<std::atomic<bool>, 16> steps{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120};
    std::atomic<int> currentStep{-1};

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sampleRate = device->getCurrentSampleRate();
        phase = 0.0;
        samplesToStep = 0.0;
        step = 0;
    }
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        for (int i = 0; i < samples; ++i) {
            float value = 0.0f;
            if (playing.load()) {
                if (samplesToStep <= 0.0) {
                    currentStep.store(step);
                    envelope = steps[step].load() ? 0.28 : 0.0;
                    step = (step + 1) % 16;
                    samplesToStep += sampleRate * 60.0 / (juce::jmax(1, bpm.load()) * 4.0);
                }
                samplesToStep -= 1.0;
                value = static_cast<float>(std::sin(phase) * envelope);
                phase += juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
                if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                envelope *= 0.9995;
            } else {
                currentStep.store(-1);
            }
            for (int c = 0; c < channels; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = value;
        }
    }
private:
    double sampleRate{44100.0}, phase{0.0}, samplesToStep{0.0}, envelope{0.0};
    int step{0};
};

class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent() {
        setSize(960, 420);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO  |  Step Sequencer Prototype", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(play);
        play.setButtonText("Play / Stop");
        play.onClick = [this] { engine.playing.store(!engine.playing.load()); };
        addAndMakeVisible(tempo);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120);
        tempo.onValueChange = [this] { engine.bpm.store(static_cast<int>(tempo.getValue())); };
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new juce::ToggleButton(juce::String(i + 1)));
            addAndMakeVisible(button);
            button->onClick = [this, button, i] { engine.steps[i].store(button->getToggleState()); };
        }
        engine.steps[0].store(true); pads[0]->setToggleState(true, juce::dontSendNotification);
        engine.steps[4].store(true); pads[4]->setToggleState(true, juce::dontSendNotification);
        engine.steps[8].store(true); pads[8]->setToggleState(true, juce::dontSendNotification);
        engine.steps[12].store(true); pads[12]->setToggleState(true, juce::dontSendNotification);
        audio.setAudioChannels(0, 2);
        audio.deviceManager.addAudioCallback(&engine);
        startTimerHz(20);
    }
    ~MainComponent() override {
        stopTimer();
        audio.deviceManager.removeAudioCallback(&engine);
        audio.shutdownAudio();
    }
    void resized() override {
        title.setBounds(20, 20, getWidth() - 40, 50);
        play.setBounds(30, 95, 130, 38);
        tempo.setBounds(180, 95, 260, 38);
        const int width = (getWidth() - 60) / 8;
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setBounds(30 + (i % 8) * width, 175 + (i / 8) * 65, width - 6, 50);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151722));
        g.setColour(juce::Colours::lightgreen);
        g.drawText("BPM: " + juce::String(engine.bpm.load()) + "   |   Active step: " +
            juce::String(engine.currentStep.load() + 1), 470, 95, 440, 40, juce::Justification::centredLeft);
    }
private:
    void timerCallback() override { repaint(); }
    ToneEngine engine;
    juce::AudioDeviceManager audio;
    juce::AudioAppComponent* unused{nullptr};
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
