#pragma once
#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "CompactWorkspace.h"
#include "TrackMixerPanel.h"
#include "ProjectController.h"
#include "StepSequencerView.h"
#include "SessionView.h"
#include "TopBar.h"
#include "ShellPanels.h"
#include "StudioWidgets.h"

// The application window's content. It owns the audio engine and device, the
// project controller and every view, and wires them together. Views never talk
// to each other directly; they report intent and this class routes it.
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::MidiKeyboardState::Listener,
                            private juce::ChangeListener {
public:
    MainComponent() {
        setLookAndFeel(&studioLookAndFeel);
        juce::LookAndFeel::setDefaultLookAndFeel(&studioLookAndFeel);
        setOpaque(true);
        setSize(1280, 800);

        addAndMakeVisible(topBar);
        addAndMakeVisible(statusBar);
        addAndMakeVisible(keyboardDock);
        wireTopBar();
        wireController();
        wireEditors();

        tabs.addTab("16-step sequencer", alien::ui::colours::surface, &stepsView, false);
        tabs.addTab("Piano roll", alien::ui::colours::surface, &pianoRoll, false);
        tabs.addTab("Session", alien::ui::colours::surface, &sessionView, false);
        tabs.addTab("Mixer", alien::ui::colours::surface, &mixerPanel, false);
        tabs.setTabBarDepth(0); // The top-bar view switch drives the tabs.
        tabs.setOutline(0);
        tabs.setIndent(0);
        tabs.getTabbedButtonBar().addChangeListener(this);
        addAndMakeVisible(workspace);
        workspace.onViewChanged = [this](bool) { releaseNotes(); syncViewButtons(); };

        // The first clip starts out holding the piano roll's (empty) pattern and queued.
        controller.setClipPattern(0, 0, pianoRoll.getPattern(), false);
        controller.pushDocumentToEngine();
        controller.publishClip(0, 0);
        refreshAll();

        typingInput.octaveChanged = [this] { updateOctave(); };
        updateOctave();
        keyboardState.addListener(this);
        const auto audioError = audio.initialiseWithDefaultDevices(0, 2);
        audio.addAudioCallback(&engine);
        syncViewButtons();
        startTimerHz(20);
        resized();
        if (audioError.isNotEmpty())
            showAudioErrorWhenVisible(juce::Component::SafePointer<MainComponent>(this), audioError);
    }

    ~MainComponent() override {
        stopTimer();
        tabs.getTabbedButtonBar().removeChangeListener(this);
        releaseNotes();
        keyboardState.removeListener(this);
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void resized() override {
        auto area = getLocalBounds();
        topBar.setBounds(area.removeFromTop(alien::TopBar::height));
        statusBar.setBounds(area.removeFromBottom(alien::StatusBar::height));
        keyboardDock.setBounds(area.removeFromBottom(getHeight() < 760 ? alien::KeyboardDock::compactHeight
                                                                       : alien::KeyboardDock::fullHeight));
        workspace.setBounds(area);
    }

    void paint(juce::Graphics& g) override { g.fillAll(alien::ui::colours::window); }

    void releaseNotes() {
        pianoRoll.releaseAudition();
        typingInput.release();
        keyboardState.allNotesOff(0);
        devicesView.midi().resetTestButtons();
        engine.midi.requestReset();
        engine.notes.panic.store(true);
    }

private:
    // Wiring ------------------------------------------------------------------
    void wireTopBar() {
        topBar.onViewSelected = [this](int index) { showView(index); };
        topBar.onPlayToggled = [this] { releaseNotes(); engine.playing.store(!engine.playing.load()); };
        topBar.onTempoChanged = [this](int bpm) { controller.setBpm(bpm); };
        topBar.onOpen = [this] { controller.chooseProjectToOpen(); };
        topBar.onSave = [this] { controller.save(); };
        topBar.onSavePortable = [this] { controller.chooseSavePath(true); };
    }

    void wireController() {
        controller.onProjectStatus = [this](const juce::String& text, const juce::String& tip) {
            statusBar.setProjectStatus(text, tip);
            updateStatusState();
        };
        controller.onImportStatus = [this](const juce::String& text, const juce::String& tip) {
            sessionView.setImportStatus(text, tip);
        };
        controller.onBusyChanged = [this](bool busy) {
            topBar.setBusy(busy);
            sessionView.setBusy(busy);
            updateStatusState();
        };
        controller.beforeDocumentReplaced = [this] { releaseNotes(); };
        controller.onDocumentReplaced = [this] {
            refreshAll();
            tabs.setCurrentTabIndex(controller.document().selectedClip().hasAudio() ? 2 : 1);
        };
        controller.onClipContentChanged = [this](int track, int scene) {
            sessionView.refreshSelectedClip();
            const auto& d = controller.document();
            if (track == d.selectedTrack && scene == d.selectedScene) tabs.setCurrentTabIndex(2);
        };
    }

    void wireEditors() {
        pianoRoll.onChange = [this] {
            const auto& d = controller.document();
            controller.setClipPattern(d.selectedTrack, d.selectedScene, pianoRoll.getPattern(), !loadingClip);
            if (!loadingClip) sessionView.refreshSelectedClip();
        };
        pianoRoll.onAudition = [this](int note, float velocity) {
            if (velocity > 0) keyboardState.noteOn(3, note, velocity);
            else keyboardState.noteOff(3, note, 0);
        };
        sessionView.onSelectionChanged = [this] { loadSelectedClip(); };
        sessionView.onEditClip = [this](int track, int scene) {
            controller.selectClip(track, scene);
            loadSelectedClip();
            if (controller.document().selectedClip().hasAudio()) tabs.setCurrentTabIndex(2);
            else {
                tabs.setCurrentTabIndex(1);
                pianoRoll.grabKeyboardFocus();
            }
        };
        sessionView.onClearClip = [this] { clearSelectedClip(); };
        sessionView.onTitleChanged = [this] { updateEditorTitle(); };
        mixerPanel.onTrackEdited = [this](int track, bool) {
            controller.trackSettingsEdited(track);
            sessionView.repaint();
        };
        devicesView.onPresetChanged = [this](int preset) {
            releaseNotes();
            controller.setLivePreset(preset);
            stepsView.repaint();
            updateEditorTitle();
        };
        keyboardDock.onOctaveShift = [this](int delta) { typingInput.typing.shift(delta); updateOctave(); };
    }

    // Views -----------------------------------------------------------------
    void refreshAll() {
        const auto& d = controller.document();
        topBar.setTempo(d.bpm);
        devicesView.setPreset(d.livePreset);
        mixerPanel.refreshSettings();
        stepsView.refresh();
        sessionView.rebuildGrid();
        sessionView.refreshFromDocument();
        loadSelectedClip();
        updateEditorTitle();
    }

    void loadSelectedClip() {
        loadingClip = true;
        pianoRoll.setPattern(controller.document().selectedClip().notes());
        loadingClip = false;
        sessionView.refreshSelectedClip();
        updateEditorTitle();
    }

    void clearSelectedClip() {
        const auto& d = controller.document();
        const int track = d.selectedTrack, scene = d.selectedScene;
        controller.removeAudio(track, scene);
        // The selected clip is the one loaded in the piano roll, so this clears
        // it as an undoable edit and republishes it through onChange.
        pianoRoll.clearAllNotes();
        sessionView.setImportStatus("Cleared Track " + juce::String(track + 1) + ", Scene " + juce::String(scene + 1), {});
        sessionView.refreshSelectedClip();
    }

    void showView(int index) {
        if (workspace.isDeviceView()) workspace.showDevices(false);
        tabs.setCurrentTabIndex(index);
        syncViewButtons();
    }

    void syncViewButtons() {
        topBar.setActiveView(workspace.isDeviceView() ? -1 : tabs.getCurrentTabIndex());
        updateEditorTitle();
    }

    void updateEditorTitle() {
        const auto& d = controller.document();
        const auto dot = juce::String(juce::CharPointer_UTF8("  \xc2\xb7  "));
        switch (tabs.getCurrentTabIndex()) {
        case 0: workspace.setEditorTitle("Step Sequencer", stepsView.subtitle()); break;
        case 1: workspace.setEditorTitle("Piano Roll", "Track " + juce::String(d.selectedTrack + 1) + dot
                                         + "Scene " + juce::String(d.selectedScene + 1)); break;
        case 2: workspace.setEditorTitle("Session", sessionView.subtitle()); break;
        case 3: workspace.setEditorTitle("Mixer", juce::String(d.trackCount()) + " channels"); break;
        default: break;
        }
    }

    void updateOctave() { keyboardDock.setOctave(typingInput.typing.octave()); }

    void updateStatusState() {
        using State = alien::StatusBar::State;
        statusBar.setState(controller.isBusy() ? State::busy
                           : controller.currentFile().existsAsFile() ? State::saved : State::unsaved);
    }

    void updateEngineStatus() {
        const auto dot = juce::String(juce::CharPointer_UTF8("  \xc2\xb7  "));
        juce::String text = "No audio device";
        if (auto* device = audio.getCurrentAudioDevice()) {
            auto name = device->getName();
            if (name.length() > 28) name = name.substring(0, 27) + "...";
            text = name + dot + juce::String(device->getCurrentSampleRate() / 1000.0, 1) + " kHz" + dot
                + juce::String(device->getCurrentBufferSizeSamples()) + " smp" + dot + "CPU "
                + juce::String(juce::roundToInt(audio.getCpuUsage() * 100.0)) + "%";
        }
        statusBar.setEngineStatus(text);
    }

    // The main window stays hidden while the startup splash is up, so wait
    // until it is on screen before showing the audio error.
    static void showAudioErrorWhenVisible(juce::Component::SafePointer<MainComponent> safeThis, juce::String error) {
        juce::Timer::callAfterDelay(250, [safeThis, error] {
            if (safeThis == nullptr) return;
            if (!safeThis->isShowing()) { showAudioErrorWhenVisible(safeThis, error); return; }
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                "Audio initialization failed",
                "Alien Studio could not open the default audio output device.\n\n" + error,
                "OK", safeThis.getComponent());
        });
    }

    // Callbacks ---------------------------------------------------------------
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const int newMode = tabs.getCurrentTabIndex() == 0 ? 0 : 1;
        if (engine.patternMode.exchange(newMode) != newMode) releaseNotes();
        else pianoRoll.releaseAudition();
        syncViewButtons();
    }

    void timerCallback() override {
        const bool isPlaying = engine.playing.load();
        topBar.setPlaying(isPlaying);
        const bool stepMode = engine.patternMode.load() == 0;
        const int currentStep = stepMode && isPlaying ? engine.currentStep.load() : -1;
        stepsView.setPlayhead(currentStep);
        stepsView.setPlaying(isPlaying && stepMode);
        const auto& d = controller.document();
        int displayStep = currentStep;
        if (!stepMode) {
            const int position = engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load();
            displayStep = position >= 0 ? position / 1000 : -1;
        }
        topBar.setPosition(displayStep, isPlaying || displayStep >= 0, stepMode ? "Steps" : "Session");
        devicesView.midi().tick();
        mixerPanel.tick();
        int controllerAction = -1;
        while (engine.controllerActions.pop(controllerAction)) sessionView.handleControllerAction(controllerAction);
        sessionView.tick();
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        pianoRoll.setPlayheadStep(engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load() / 1000.0);
        if (++statusTicks % 10 == 0) updateEngineStatus();
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

    // Members (order matters: construction top to bottom) -------------------
    alien::ui::StudioLookAndFeel studioLookAndFeel;
    juce::TooltipWindow tooltipWindow{this, 650};
    ToneEngine engine;
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    alien::ProjectController controller{engine};
    alien::TopBar topBar;
    alien::StatusBar statusBar;
    alien::KeyboardDock keyboardDock{keyboardState};
    alien::StepSequencerView stepsView{controller};
    alien::PianoRollEditor pianoRoll;
    alien::SessionView sessionView{controller, engine};
    alien::TrackMixerPanel mixerPanel{engine.tracks};
    alien::DevicesView devicesView{engine.midi};
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    alien::CompactWorkspace workspace{tabs, devicesView};
    TypingPianoRouter typingInput{*this, keyboardState};
    bool loadingClip{};
    int statusTicks{};
};
