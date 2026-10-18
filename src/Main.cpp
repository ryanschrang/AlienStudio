#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "MidiInputPanel.h"
#include "CompactWorkspace.h"
#include "SessionClipButton.h"
#include "AudioClipImport.h"
#include "AudioClipWaveform.h"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

struct SessionClipContent {
    alien::PianoRollPattern pattern;
    const alien::SessionAudioClip* audio{};
    juce::String audioFileName;
};

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener, private juce::ChangeListener {
public:
    MainComponent() {
        setSize(1120, 720);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO  |  Pattern Studio", juce::dontSendNotification);
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
            stepsPanel.addAndMakeVisible(button);
            button->onClick = [this, button, i] { engine.steps[i].store(button->getToggleState()); };
        }
        engine.steps[0].store(true); pads[0]->setToggleState(true, juce::dontSendNotification);
        engine.steps[4].store(true); pads[4]->setToggleState(true, juce::dontSendNotification);
        engine.steps[8].store(true); pads[8]->setToggleState(true, juce::dontSendNotification);
        engine.steps[12].store(true); pads[12]->setToggleState(true, juce::dontSendNotification);
        tabs.addTab("16-step sequencer", juce::Colour(0xff202736), &stepsPanel, false);
        tabs.addTab("Piano roll", juce::Colour(0xff202736), &pianoRoll, false);
        tabs.addTab("Session", juce::Colour(0xff202736), &sessionPanel, false);
        tabs.getTabbedButtonBar().addChangeListener(this);
        addAndMakeVisible(workspace);
        devicePanel.addAndMakeVisible(deviceTitle);
        deviceTitle.setText("Device rack", juce::dontSendNotification);
        deviceTitle.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        deviceTitle.setColour(juce::Label::textColourId, juce::Colour(0xff8ff0c0));
        devicePanel.addAndMakeVisible(deviceHelp);
        deviceHelp.setText("Choose the synth voice and MIDI input used by the current pattern. Shift+Tab returns to the editor.",
                           juce::dontSendNotification);
        deviceHelp.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        devicePanel.addAndMakeVisible(midiLabel);
        midiLabel.setText("MIDI input", juce::dontSendNotification);
        midiLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        devicePanel.addAndMakeVisible(midiInput);
        stepsPanel.addAndMakeVisible(stepHelp);
        stepHelp.setText("16 steps  |  Sixteenth notes  |  A3 synth pattern", juce::dontSendNotification);
        stepHelp.setJustificationType(juce::Justification::centred);
        sessionPanel.addAndMakeVisible(sessionTitle);
        sessionTitle.setText("Session scenes", juce::dontSendNotification);
        sessionTitle.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        sessionTitle.setColour(juce::Label::textColourId, juce::Colour(0xff8ff0c0));
        sessionPanel.addAndMakeVisible(sessionHelp);
        sessionHelp.setText("Launch a scene to switch all tracks together. Click a clip to launch; right-click to edit or view audio.",
                            juce::dontSendNotification);
        sessionHelp.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        sessionPanel.addAndMakeVisible(launchTimingLabel);
        launchTimingLabel.setText("Launch at", juce::dontSendNotification);
        launchTimingLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        sessionPanel.addAndMakeVisible(launchTiming);
        launchTiming.setName("Session launch quantization");
        launchTiming.setTooltip("Choose whether queued clips and scenes launch on the next beat or bar");
        launchTiming.addItem("Beat", 1);
        launchTiming.addItem("Bar", 2);
        launchTiming.setSelectedId(2, juce::dontSendNotification);
        launchTiming.onChange = [this] {
            engine.launchQuantizationSteps.store(launchTiming.getSelectedId() == 1 ? 4 : 16);
        };
        sessionPanel.addAndMakeVisible(controllerTarget);
        controllerTarget.setName("MIDI controller mapping action");
        controllerTarget.setTooltip("Choose a Session action to assign to a MIDI CC");
        controllerTarget.addItem("Scene 1", 1);
        controllerTarget.addItem("Scene 2", 2);
        controllerTarget.addItem("Scene 3", 3);
        controllerTarget.addItem("Scene 4", 4);
        controllerTarget.addItem("Stop all", 5);
        controllerTarget.setSelectedId(1, juce::dontSendNotification);
        controllerTarget.onChange = [this] { updateControllerMappingLabel(); };
        sessionPanel.addAndMakeVisible(learnController);
        learnController.setButtonText("Learn CC");
        learnController.setName("Learn MIDI controller");
        learnController.setTooltip("Listen for the next incoming MIDI CC and assign it to the selected Session action");
        learnController.onClick = [this] {
            const int action = controllerTarget.getSelectedId() - 1;
            if (learningControllerAction == action) {
                engine.controllerMappings.cancelLearn();
                learningControllerAction = -1;
                learnController.setButtonText("Learn CC");
                updateControllerMappingLabel();
            } else {
                learningControllerAction = action;
                engine.controllerMappings.beginLearn(action);
                learnController.setButtonText("Cancel learn");
                controllerMappingLabel.setText("Move a MIDI CC control…", juce::dontSendNotification);
            }
        };
        sessionPanel.addAndMakeVisible(clearController);
        clearController.setButtonText("Clear");
        clearController.setName("Clear MIDI controller mapping");
        clearController.onClick = [this] {
            engine.controllerMappings.cancelLearn();
            learningControllerAction = -1;
            engine.controllerMappings.clearMapping(controllerTarget.getSelectedId() - 1);
            learnController.setButtonText("Learn CC");
            updateControllerMappingLabel();
        };
        sessionPanel.addAndMakeVisible(controllerMappingLabel);
        controllerMappingLabel.setName("MIDI controller assignment");
        controllerMappingLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        controllerMappingLabel.setJustificationType(juce::Justification::centredLeft);
        controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        for (int scene = 0; scene < sceneCount; ++scene) {
            sessionPanel.addAndMakeVisible(sceneButtons[static_cast<std::size_t>(scene)]);
            auto& sceneButton = sceneButtons[static_cast<std::size_t>(scene)];
            sceneButton.setName("Launch scene " + juce::String(scene + 1));
            sceneButton.setButtonText("Launch scene " + juce::String(scene + 1));
            sceneButton.onClick = [this, scene] { launchScene(scene); };
            for (int track = 0; track < trackCount; ++track) {
                const auto index = cellIndex(track, scene);
                sessionPanel.addAndMakeVisible(clipButtons[index]);
                auto& button = clipButtons[index];
                button.setName("Launch track " + juce::String(track + 1) + " scene " + juce::String(scene + 1));
                button.onClick = [this, track, scene] { selectAndLaunchClip(track, scene); };
                button.onRightClick = [this, track, scene] { selectClipForEditing(track, scene); };
            }
        }
        sessionPanel.addAndMakeVisible(stopClip);
        stopClip.setButtonText("Stop all tracks");
        stopClip.setName("Stop all Session tracks at selected launch boundary");
        stopClip.onClick = [this] { stopAllSessionTracks(); };
        sessionPanel.addAndMakeVisible(editSelectedClip);
        editSelectedClip.setButtonText("Edit selected clip");
        editSelectedClip.setName("Edit selected Session clip");
        editSelectedClip.setTooltip("Open the selected MIDI clip in the piano roll or show its audio waveform");
        editSelectedClip.onClick = [this] {
            selectClipForEditing(selectedTrack, selectedScene);
        };
        addAndMakeVisible(importAudio);
        importAudio.setButtonText("Import WAV/MP3");
        importAudio.setName("Import audio file to selected Session clip");
        importAudio.setTooltip("Decode a WAV or MP3 off the audio thread and add it to the selected clip");
        importAudio.onClick = [this] { chooseAudioFile(); };
        sessionPanel.addAndMakeVisible(removeAudio);
        removeAudio.setButtonText("Remove audio");
        removeAudio.setName("Remove audio from selected Session clip");
        removeAudio.onClick = [this] { removeAudioFromSelectedClip(); };
        sessionPanel.addAndMakeVisible(audioImportStatus);
        audioImportStatus.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        audioImportStatus.setJustificationType(juce::Justification::centredLeft);
        sessionPanel.addAndMakeVisible(audioWaveform);
        pianoRoll.onChange = [this] {
            auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
            clip.pattern = pianoRoll.getPattern();
            if (loadingClip) return;
            if (clip.audio != nullptr) return;
            engine.requestedPianoRollClip[static_cast<std::size_t>(selectedTrack)].store(selectedScene);
            if (engine.activePianoRollClip[static_cast<std::size_t>(selectedTrack)].load() == selectedScene)
                engine.pianoRollNotes[static_cast<std::size_t>(selectedTrack)].publish(
                    clip.pattern);
            else
                publishSessionClip(selectedTrack, selectedScene, false);
        };
        pianoRoll.onAudition = [this](int note, float velocity) {
            if (velocity > 0) keyboardState.noteOn(3, note, velocity);
            else keyboardState.noteOff(3, note, 0);
        };
        for (int scene = 0; scene < sceneCount; ++scene)
            for (int track = 0; track < trackCount; ++track) {
                auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                if (scene == 0 && track == 0) clip.pattern = pianoRoll.getPattern();
            }
        engine.requestedPianoRollClip[0].store(0);
        updateAudioClipEditor();
        publishSessionClip(0, 0, false);
        updateSessionButtons();
        devicePanel.addAndMakeVisible(presetLabel);
        presetLabel.setText("Synth preset", juce::dontSendNotification);
        presetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        devicePanel.addAndMakeVisible(preset);
        preset.addItem("Lead (saw)", 1);
        preset.addItem("Bass (square)", 2);
        preset.addItem("Pluck (sine)", 3);
        preset.setSelectedId(1);
        preset.onChange = [this] { releaseNotes(); engine.preset.store(preset.getSelectedId() - 1); };
        workspace.onViewChanged = [this](bool) { releaseNotes(); };
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
        audioFileChooser.reset();
        audioImportPool.removeAllJobs(true, -1);
        tabs.getTabbedButtonBar().removeChangeListener(this);
        releaseNotes();
        keyboardState.removeListener(this);
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
    }
    void resized() override {
        title.setBounds(20, 10, 250, 30);
        play.setBounds(290, 8, 130, 34);
        tempo.setBounds(435, 8, 245, 34);
        workspace.setBounds(20, 52, getWidth() - 40, getHeight() - 226);
        const auto rackWidth = devicePanel.getWidth();
        deviceTitle.setBounds(24, 18, rackWidth - 48, 34);
        deviceHelp.setBounds(24, 54, rackWidth - 48, 28);
        presetLabel.setBounds(30, 108, 120, 30);
        preset.setBounds(150, 108, juce::jmin(260, rackWidth - 180), 32);
        midiLabel.setBounds(30, 168, 120, 30);
        midiInput.setBounds(150, 168, juce::jmax(300, rackWidth - 180), 32);
        sessionTitle.setBounds(24, 18, sessionPanel.getWidth() - 48, 34);
        sessionHelp.setBounds(24, 54, sessionPanel.getWidth() - 48, 28);
        const int labelWidth = 118;
        const int gridWidth = juce::jmax(280, sessionPanel.getWidth() - labelWidth - 44);
        const int trackWidth = gridWidth / trackCount;
        const int sceneHeight = 48;
        const int sceneGap = 6;
        const int gridTop = 112;
        for (int scene = 0; scene < sceneCount; ++scene) {
            sceneButtons[static_cast<std::size_t>(scene)].setBounds(24, gridTop + scene * (sceneHeight + sceneGap), labelWidth - 8, sceneHeight);
            for (int track = 0; track < trackCount; ++track)
                clipButtons[cellIndex(track, scene)].setBounds(labelWidth + 24 + track * trackWidth,
                    gridTop + scene * (sceneHeight + sceneGap), trackWidth - 8, sceneHeight);
        }
        const int actionY = gridTop + sceneCount * (sceneHeight + sceneGap) + 8;
        editSelectedClip.setBounds(24, actionY, 174, 32);
        stopClip.setBounds(206, actionY, 146, 32);
        launchTimingLabel.setBounds(360, actionY, 76, 32);
        launchTiming.setBounds(438, actionY, 94, 32);
        removeAudio.setBounds(544, actionY, 104, 32);
        audioImportStatus.setBounds(656, actionY, juce::jmax(100, sessionPanel.getWidth() - 680), 32);
        const int mappingY = actionY + 38;
        controllerTarget.setBounds(24, mappingY, 130, 32);
        learnController.setBounds(162, mappingY, 104, 32);
        clearController.setBounds(274, mappingY, 70, 32);
        controllerMappingLabel.setBounds(352, mappingY, juce::jmax(100, sessionPanel.getWidth() - 376), 32);
        audioWaveform.setBounds(24, mappingY + 38, sessionPanel.getWidth() - 48, 72);
        octaveDown.setBounds(20, getHeight() - 154, 100, 30);
        octaveUp.setBounds(130, getHeight() - 154, 100, 30);
        importAudio.setBounds(245, getHeight() - 154, 150, 30);
        help.setBounds(410, getHeight() - 154, getWidth() - 430, 30);
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
            700, 8, getWidth() - 720, 34, juce::Justification::centredLeft);
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
        const int newMode = tabs.getCurrentTabIndex() == 0 ? 0 : 1;
        if (engine.patternMode.exchange(newMode) != newMode) releaseNotes();
        else pianoRoll.releaseAudition();
    }
    void timerCallback() override {
        midiInput.tick();
        int controllerAction = -1;
        while (engine.controllerActions.pop(controllerAction)) {
            if (controllerAction < sceneCount) launchScene(controllerAction);
            else if (controllerAction == alien::MidiControllerMappings::stopAllAction) stopAllSessionTracks();
        }
        const int learnedAction = engine.controllerMappings.takeLearnedAction();
        if (learnedAction >= 0) {
            if (learningControllerAction == learnedAction) {
                learningControllerAction = -1;
                learnController.setButtonText("Learn CC");
            }
            updateControllerMappingLabel();
        }
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        pianoRoll.setPlayheadStep(engine.sessionTrackPositions[static_cast<std::size_t>(selectedTrack)].load() / 1000.0);
        updateSessionButtons();
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
    void selectAndLaunchClip(int clip) {
        selectAndLaunchClip(selectedTrack, clip);
    }
    void selectAndLaunchClip(int track, int scene) {
        loadClip(track, scene);
        publishSessionClip(selectedTrack, selectedScene, false);
        updateSessionButtons();
    }
    void selectClipForEditing(int clip) {
        selectClipForEditing(selectedTrack, clip);
    }
    void selectClipForEditing(int track, int scene) {
        loadClip(track, scene);
        if (clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].audio != nullptr)
            tabs.setCurrentTabIndex(2);
        else {
            tabs.setCurrentTabIndex(1);
            pianoRoll.grabKeyboardFocus();
        }
        updateSessionButtons();
    }
    static constexpr int trackCount = 4, sceneCount = 4;
    static constexpr std::size_t maxSessionAudioBytes = 512u * 1024u * 1024u;
    static constexpr std::size_t cellIndex(int track, int scene) noexcept {
        return static_cast<std::size_t>(scene * trackCount + track);
    }
    void saveEditedClip() {
        clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].pattern = pianoRoll.getPattern();
    }
    void loadClip(int track, int scene) {
        saveEditedClip();
        selectedTrack = juce::jlimit(0, trackCount - 1, track);
        selectedScene = juce::jlimit(0, sceneCount - 1, scene);
        loadingClip = true;
        pianoRoll.setPattern(clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].pattern);
        loadingClip = false;
        updateAudioClipEditor();
    }
    void launchScene(int scene) {
        saveEditedClip();
        scene = juce::jlimit(0, sceneCount - 1, scene);
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < trackCount; ++track) {
            const auto index = static_cast<std::size_t>(track);
            engine.requestedPianoRollClip[index].store(scene);
            publishSessionClip(track, scene, true);
        }
        updateSessionButtons();
    }
    void stopAllSessionTracks() {
        saveEditedClip();
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < trackCount; ++track) {
            engine.requestedPianoRollClip[static_cast<std::size_t>(track)].store(-1);
            alien::SessionClipLaunch stopped;
            stopped.pattern.clear();
            stopped.clipIndex = -1;
            engine.pianoRollLaunches[static_cast<std::size_t>(track)].publish(stopped);
        }
        updateSessionButtons();
    }
    void publishSessionClip(int track, int scene, bool) {
        const auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        engine.requestedPianoRollClip[static_cast<std::size_t>(track)].store(scene);
        alien::SessionClipLaunch launch;
        launch.pattern = clip.audio == nullptr ? clip.pattern : alien::PianoRollPattern{};
        launch.audio = clip.audio;
        launch.clipIndex = scene;
        engine.pianoRollLaunches[static_cast<std::size_t>(track)].publish(launch);
    }
    void chooseAudioFile() {
        audioFileChooser = std::make_unique<juce::FileChooser>("Import audio clip",
            juce::File{}, "*.wav;*.mp3");
        juce::Component::SafePointer<MainComponent> safe(this);
        audioFileChooser->launchAsync(juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
            [safe](const juce::FileChooser& chooser) {
                if (safe && chooser.getResult().existsAsFile())
                    safe->startAudioImport(chooser.getResult(), safe->selectedTrack, safe->selectedScene);
            });
    }
    void startAudioImport(const juce::File& file, int track, int scene) {
        audioImportStatus.setText("Loading audio…", juce::dontSendNotification);
        audioImportStatus.setTooltip(file.getFullPathName());
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> importJob = [safe, file, track, scene] {
            auto result = std::make_shared<alien::AudioClipImportResult>(alien::decodeSessionAudioClip(file));
            juce::MessageManager::callAsync([safe, file, track, scene, result] {
                if (safe != nullptr) safe->finishAudioImport(file, track, scene, *result);
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioImportPool.addJob(std::move(importJob));
    }
    void finishAudioImport(const juce::File& file, int track, int scene,
                           const alien::AudioClipImportResult& result) {
        if (!result.clip) {
            audioImportStatus.setText(result.error, juce::dontSendNotification);
            audioImportStatus.setTooltip(result.error);
            return;
        }
        const auto* data = result.clip.get();
        const auto bytes = data->frames * static_cast<std::size_t>(data->channels) * sizeof(float);
        if (bytes > maxSessionAudioBytes - retainedAudioBytes) {
            audioImportStatus.setText("Session audio storage is full (512 MiB limit)", juce::dontSendNotification);
            audioImportStatus.setTooltip(audioImportStatus.getText());
            return;
        }
        retainedAudioBytes += bytes;
        retainedAudioAssets.push_back(result.clip);
        auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        clip.audio = data;
        clip.audioFileName = file.getFileName();
        if (track == selectedTrack && scene == selectedScene) {
            updateAudioClipEditor();
            tabs.setCurrentTabIndex(2);
        }
        if (engine.activePianoRollClip[static_cast<std::size_t>(track)].load() == scene
            || engine.requestedPianoRollClip[static_cast<std::size_t>(track)].load() == scene)
            publishSessionClip(track, scene, false);
        audioImportStatus.setText("Imported " + file.getFileName(), juce::dontSendNotification);
        audioImportStatus.setTooltip(file.getFullPathName());
        updateSessionButtons();
    }
    void removeAudioFromSelectedClip() {
        auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        if (clip.audio == nullptr) return;
        clip.audio = nullptr;
        clip.audioFileName.clear();
        updateAudioClipEditor();
        const int active = engine.activePianoRollClip[static_cast<std::size_t>(selectedTrack)].load();
        const int requested = engine.requestedPianoRollClip[static_cast<std::size_t>(selectedTrack)].load();
        if (active == selectedScene || requested == selectedScene)
            publishSessionClip(selectedTrack, selectedScene, false);
        audioImportStatus.setText("Audio removed from selected clip", juce::dontSendNotification);
        updateSessionButtons();
    }
    void updateAudioClipEditor() {
        const auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        audioWaveform.setClip(clip.audio, clip.audioFileName);
        removeAudio.setEnabled(clip.audio != nullptr);
        if (clip.audio == nullptr && audioImportStatus.getText().isEmpty())
            audioImportStatus.setText("Selected clip: Track " + juce::String(selectedTrack + 1)
                + ", Scene " + juce::String(selectedScene + 1), juce::dontSendNotification);
    }
    void updateControllerMappingLabel() {
        if (learningControllerAction == controllerTarget.getSelectedId() - 1) {
            controllerMappingLabel.setText("Move a MIDI CC control…", juce::dontSendNotification);
            return;
        }
        const int code = engine.controllerMappings.mappingFor(controllerTarget.getSelectedId() - 1);
        if (code < 0) controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        else controllerMappingLabel.setText("CC " + juce::String(code % 128) + "  |  Ch "
            + juce::String(code / 128 + 1), juce::dontSendNotification);
    }
    void updateSessionButtons() {
        for (int scene = 0; scene < sceneCount; ++scene) {
            sceneButtons[static_cast<std::size_t>(scene)].setButtonText(
                (scene == selectedScene ? "Selected | " : "") + juce::String("Launch scene ") + juce::String(scene + 1));
            for (int track = 0; track < trackCount; ++track) {
                const auto index = static_cast<std::size_t>(track);
                const int active = engine.activePianoRollClip[index].load();
                const int requested = engine.requestedPianoRollClip[index].load();
                const auto& clip = clips[static_cast<std::size_t>(scene)][index];
                auto text = clip.audio != nullptr
                    ? "Track " + juce::String(track + 1) + " | AUDIO\n" + clip.audioFileName.substring(0, 20)
                    : "Track " + juce::String(track + 1) + "\nScene " + juce::String(scene + 1);
                if (scene == active) text += " | Playing";
                else if (scene == requested) text += " | Queued";
                if (track == selectedTrack && scene == selectedScene) text += " | Edit";
                clipButtons[cellIndex(track, scene)].setButtonText(text);
            }
        }
        stopClip.setButtonText("Stop all tracks");
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
    juce::Component sessionPanel;
    juce::Label sessionTitle, sessionHelp, launchTimingLabel, controllerMappingLabel;
    juce::Label audioImportStatus;
    juce::ComboBox launchTiming, controllerTarget;
    juce::TextButton learnController, clearController;
    std::array<juce::TextButton, 4> sceneButtons;
    std::array<alien::SessionClipButton, 16> clipButtons;
    juce::TextButton editSelectedClip, stopClip, importAudio, removeAudio;
    alien::AudioClipWaveform audioWaveform;
    std::array<std::array<SessionClipContent, trackCount>, sceneCount> clips{};
    std::vector<std::shared_ptr<alien::SessionAudioClip>> retainedAudioAssets;
    std::size_t retainedAudioBytes{};
    juce::ThreadPool audioImportPool{1};
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    int selectedTrack{}, selectedScene{};
    int learningControllerAction{-1};
    bool loadingClip{};
    juce::Component devicePanel;
    juce::Label deviceTitle, deviceHelp, presetLabel, midiLabel;
    alien::CompactWorkspace workspace{tabs, devicePanel};
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
