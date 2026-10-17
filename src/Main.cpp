#include <JuceHeader.h>
#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include "TypingPiano.h"
#include <array>
#include <atomic>
#include <cmath>

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    std::array<std::atomic<bool>, 16> steps{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, preset{0};
    NoteQueue notes;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sampleRate = device->getCurrentSampleRate();
        synth.prepare(sampleRate);
        discardPendingNotes();
        resetTransport();
    }
    void audioDeviceStopped() override { synth.allSoundOff(); discardPendingNotes(); resetTransport(); }
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        const bool playNow = playing.load();
        if (wasPlaying && !playNow) { synth.allSoundOff(); resetTransport(); }
        wasPlaying = playNow;
        synth.setPatch(alien::presets[static_cast<std::size_t>(juce::jlimit(0, 2, preset.load()))]);
        NoteQueue::Event event{};
        const bool panic = notes.panic.exchange(false);
        // Bound work per callback even if the producer keeps publishing.
        for (int n = 0; n < 256 && notes.pop(event); ++n) {
            if (!panic) {
                if (event.velocity > 0) synth.noteOn(event.note, event.velocity);
                else synth.noteOff(event.note);
            }
        }
        if (panic) synth.allSoundOff();
        for (int i = 0; i < samples; ++i) {
            if (playNow) {
                const auto tick = clock.next(sampleRate, bpm.load());
                if (tick.noteOff) synth.noteOff(57, 1);
                if (tick.step >= 0) {
                    currentStep.store(tick.step);
                    if (steps[static_cast<std::size_t>(tick.step)].load()) synth.noteOn(57, 0.8f, 1);
                }
            }
            const float value = synth.nextSample();
            for (int c = 0; c < channels; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = value;
        }
    }
private:
    void discardPendingNotes() noexcept {
        NoteQueue::Event event{};
        for (int n = 0; n < 256 && notes.pop(event); ++n) {}
        notes.panic.store(false);
    }
    void resetTransport() noexcept {
        clock.reset(); wasPlaying = false; currentStep.store(-1);
    }
    alien::SynthEngine synth;
    alien::StepClock clock;
    double sampleRate{44100.0};
    bool wasPlaying{};
};

// Typing is deliberately scoped to piano focus so editors and shortcuts keep their keys.
class PianoComponent : public juce::MidiKeyboardComponent {
public:
    explicit PianoComponent(juce::MidiKeyboardState& s)
        : MidiKeyboardComponent(s, horizontalKeyboard), state(s) { clearKeyMappings(); }
    TypingPiano typing;
    std::function<void()> octaveChanged;
    void releaseTyping() { typing.releaseAll([this](int n, bool on) { send(n, on); }); }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()
            || key.getModifiers().isAltDown() || key.getModifiers().isShiftDown()) return false;
        if (key.getKeyCode() == 'Z' || key.getKeyCode() == 'X') return true;
        return typing.press(key.getKeyCode(), [this](int n, bool on) { send(n, on); });
    }
    bool keyStateChanged(bool down) override {
        typing.releaseMissing([](int k) { return juce::KeyPress::isKeyCurrentlyDown(k); },
                              [this](int n, bool on) { send(n, on); });
        const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
        const bool allowed = !mods.isAnyModifierKeyDown();
        for (int i = 0; i < 2; ++i) {
            const bool now = juce::KeyPress::isKeyCurrentlyDown(i == 0 ? 'Z' : 'X');
            if (now && !octaveHeld[i] && allowed) {
                typing.shift(i == 0 ? -1 : 1);
                if (octaveChanged) octaveChanged();
            }
            octaveHeld[i] = now;
        }
        bool used = false;
        if (down && allowed)
            for (int k : TypingPiano::keys)
                if (juce::KeyPress::isKeyCurrentlyDown(k))
                    used |= typing.press(k, [this](int n, bool on) { send(n, on); });
        return used;
    }
    void focusLost(FocusChangeType cause) override {
        releaseTyping();
        octaveHeld.fill(false);
        MidiKeyboardComponent::focusLost(cause);
    }
private:
    void send(int n, bool on) { if (on) state.noteOn(2, n, .8f); else state.noteOff(2, n, 0); }
    juce::MidiKeyboardState& state;
    std::array<bool, 2> octaveHeld{};
};

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener {
public:
    MainComponent() {
        setSize(960, 550);
        addMouseListener(this, true);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO  |  Step Sequencer Prototype", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(play);
        play.setButtonText("Play / Stop");
        play.onClick = [this] { releaseNotes(); engine.playing.store(!engine.playing.load()); };
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
        addAndMakeVisible(preset);
        preset.addItem("Lead (saw)", 1);
        preset.addItem("Bass (square)", 2);
        preset.addItem("Pluck (sine)", 3);
        preset.setSelectedId(1);
        preset.onChange = [this] { releaseNotes(); engine.preset.store(preset.getSelectedId() - 1); };
        addAndMakeVisible(keyboard);
        keyboard.setAvailableRange(12, 119);
        keyboard.setLowestVisibleKey(48);
        keyboard.octaveChanged = [this] { updateOctave(); };
        for (auto* b : {&octaveDown, &octaveUp}) {
            addAndMakeVisible(b); b->setWantsKeyboardFocus(false);
        }
        octaveDown.setButtonText("Octave -"); octaveUp.setButtonText("Octave +");
        octaveDown.onClick = [this] { keyboard.typing.shift(-1); updateOctave(); };
        octaveUp.onClick = [this] { keyboard.typing.shift(1); updateOctave(); };
        addAndMakeVisible(help);
        updateOctave();
        keyboard.setVelocity(0.8f, true);
        keyboardState.addListener(this);
        audio.initialiseWithDefaultDevices(0, 2);
        audio.addAudioCallback(&engine);
        startTimerHz(20);
    }
    ~MainComponent() override {
        stopTimer();
        releaseNotes();
        keyboardState.removeListener(this);
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
    }
    void resized() override {
        title.setBounds(20, 20, getWidth() - 40, 50);
        play.setBounds(30, 95, 130, 38);
        tempo.setBounds(180, 95, 260, 38);
        preset.setBounds(30, 325, 240, 32);
        octaveDown.setBounds(290, 325, 100, 32);
        octaveUp.setBounds(400, 325, 100, 32);
        help.setBounds(30, 363, getWidth() - 60, 40);
        keyboard.setBounds(30, 410, getWidth() - 60, 100);
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
public:
    void mouseDown(const juce::MouseEvent& event) override {
        if (event.originalComponent != &keyboard && !keyboard.isParentOf(event.originalComponent)
            && event.originalComponent != &octaveDown && event.originalComponent != &octaveUp) {
            keyboard.releaseTyping();
            if (keyboard.hasKeyboardFocus(false)) keyboard.giveAwayKeyboardFocus();
        }
    }
    void releaseNotes() {
        keyboard.releaseTyping();
        keyboardState.allNotesOff(0);
        engine.notes.panic.store(true);
    }
private:
    void updateOctave() {
        help.setText("Click piano to play: A W S E D F T G Y H U J | Z/X octave | C"
                     + juce::String(keyboard.typing.octave()), juce::dontSendNotification);
        keyboard.setLowestVisibleKey(12 * (keyboard.typing.octave() + 1));
        if (keyboard.isShowing()) keyboard.grabKeyboardFocus();
    }
    void timerCallback() override { repaint(); }
    void handleNoteOn(juce::MidiKeyboardState*, int, int note, float velocity) override {
        if (!(keyboardState.isNoteOn(1, note) && keyboardState.isNoteOn(2, note)))
            engine.notes.push(note, velocity);
    }
    void handleNoteOff(juce::MidiKeyboardState*, int, int note, float) override {
        if (!keyboardState.isNoteOnForChannels(3, note)) engine.notes.push(note, 0);
    }
    ToneEngine engine;
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    PianoComponent keyboard{keyboardState};
    juce::TextButton octaveDown, octaveUp;
    juce::Label help;
    juce::ComboBox preset;
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
        void activeWindowStatusChanged() override {
            if (!isActiveWindow())
                if (auto* content = dynamic_cast<MainComponent*>(getContentComponent())) content->releaseNotes();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(AlienStudioApp)
