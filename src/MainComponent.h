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
#include "DrumSamplerPanel.h"
#include "ArrangementPanel.h"
#include "AudioToolsPanel.h"
#include "AudioFileWrite.h"
#include "TempoWarp.h"
#include "WavExport.h"
#include "PluginHost.h"
#include "PluginPanel.h"

// The application window's content. It owns the audio engine and device, the
// project controller and every view, and wires them together. Views never talk
// to each other directly; they report intent and this class routes it.
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::MidiKeyboardState::Listener,
                            private juce::ChangeListener,
                            private juce::KeyListener {
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
        tabs.addTab("Drums", alien::ui::colours::surface, &drumPanel, false);
        tabs.addTab("Arrangement", alien::ui::colours::surface, &arrangementPanel, false);
        tabs.addTab("Audio lab", alien::ui::colours::surface, &audioTools, false);
        drumPanel.onLoad = [this](int t, int p, const juce::File& f) { controller.loadDrum(f, t, p); };
        drumPanel.onClear = [this](int t, int p) { controller.clearDrum(t, p); refreshExtraViews(); };
        drumPanel.onHit = [this](int t, int p) { engine.drumAuditions[t].push(alien::drumBaseNote + p, 0.8f); };
        drumPanel.onRoute = [this](int t) { engine.liveDrumTrack.store(t); };
        arrangementPanel.onChange = [this] { controller.setArrangement(arrangementPanel.getArrangement()); };
        arrangementPanel.onExport = [this] { chooseWavExport(); };
        audioTools.onRecord = [this] { chooseRecording(); };
        audioTools.onSettings = [this] { showAudioSettings(); };
        audioTools.onWarp = [this] { chooseTempoWarp(); };
        tabs.addTab("Plugins", alien::ui::colours::surface, &pluginPanel, false);
        pluginPanel.onScan = [this] { choosePluginScan(); };
        pluginPanel.onTarget = [this](int) { refreshPlugins(); };
        pluginPanel.onAdd = [this](bool instrument, int index) { addPlugin(instrument,index); };
        pluginPanel.onAction = [this](int op,int slot) { pluginAction(op,slot); };
        if (auto xml = juce::parseXML(pluginListFile())) knownPlugins.recreateFromXml(*xml);
        controller.onBeforeSnapshot = [this] { snapshotPlugins(); };
        controller.beforeStructureEdit = [this] { snapshotPlugins(); };
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
        controller.markAsNewBaseline();
        refreshAll();
        updateUndoState();

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
        if (alien::ProjectController::hasRecoverableSession())
            offerRecoveryWhenVisible(juce::Component::SafePointer<MainComponent>(this));
    }

    ~MainComponent() override {
        stopTimer();
        if (keySource != nullptr) keySource->removeKeyListener(this);
        tabs.getTabbedButtonBar().removeChangeListener(this);
        releaseNotes();
        keyboardState.removeListener(this);
        if (audioWorkCancellation) audioWorkCancellation->store(true);
        if (exportCancellation) exportCancellation->store(true);
        audioWorkPool.removeAllJobs(true, -1);
        audio.removeAudioCallback(&engine);
        engine.capture.store(nullptr);
        audioSettingsWindow = nullptr;
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

    // Ask to save unsaved changes, then run "then" unless the user cancels.
    void confirmDiscardChanges(const juce::String& action, std::function<void()> then) {
        if (controller.isBusy() || projectBusy) return;
        if (!controller.isDirty()) { then(); return; }
        const auto name = projectName();
        auto* window = new juce::AlertWindow("Save changes to \"" + name + "\"?",
            "Your changes will be lost if you " + action + " without saving.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Don't Save", 2);
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MainComponent> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([safe, then](int result) {
            if (safe == nullptr) return;
            if (result == 2) then();
            else if (result == 1) safe->controller.save([then](bool saved) { if (saved) then(); });
        }), true);
    }

    // Close the app, prompting for unsaved changes first.
    void requestQuit(std::function<void()> quit) {
        if (activeCapture) { closeAfterTake = true; finishRecording(); return; }
        if (unsavedTake || projectBusy) { audioTools.setStatus("Finish saving the take or audio operation before closing."); showView(6); return; }

        confirmDiscardChanges("close Alien Studio", [this, quit] {
            controller.discardRecovery();
            quit();
        });
    }

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
        topBar.onTempoChanged = [this](int bpm) {
            controller.setBpm(bpm);
            if (!topBar.isTempoDragging()) controller.commit("Change tempo", -1, -1, "tempo");
        };
        topBar.onTempoCommitted = [this] { controller.commit("Change tempo", -1, -1, "tempo"); };
        topBar.onNew = [this] { newProject(); };
        topBar.onOpen = [this] { openProject(); };
        topBar.onSave = [this] { controller.save(); };
        topBar.onSavePortable = [this] { controller.chooseSavePath(true); };
        topBar.onUndo = [this] { undo(); };
        topBar.onRedo = [this] { redo(); };
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
            sessionView.setBusy(busy || projectBusy);
            drumPanel.setBusy(busy || projectBusy);
            updateStatusState();
        };
        controller.beforeDocumentReplaced = [this] { releaseNotes(); };
        controller.onDocumentReplaced = [this] {
            restorePlugins();
            refreshAll();
            tabs.setCurrentTabIndex(controller.document().selectedClip().hasAudio() ? 2 : 1);
        };
        controller.onDocumentRestored = [this] { restorePlugins(); refreshAll(); };
        controller.onHistoryChanged = [this] { updateUndoState(); };
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
        pianoRoll.onEditCommitted = [this] {
            if (suppressPianoRollCommit) return;
            const auto& d = controller.document();
            controller.commit("Edit notes", d.selectedTrack, d.selectedScene);
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
        sessionView.onEdited = [this](const juce::String& label) {
            const auto& d = controller.document();
            const bool clipEdit = label == "Remove audio";
            controller.commit(label, clipEdit ? d.selectedTrack : -1, clipEdit ? d.selectedScene : -1);
        };
        stepsView.onEdited = [this](const juce::String& label, const juce::String& key) {
            controller.commit(label, -1, -1, key);
        };
        sessionView.onStructureChanged = [this] {
            mixerPanel.rebuild();
            restorePlugins();
            refreshExtraViews();
            loadSelectedClip();
        };
        mixerPanel.onTrackEdited = [this](int track, bool committed) {
            controller.trackSettingsEdited(track);
            sessionView.repaint();
            if (committed)
                controller.commit("Change " + controller.document().tracks[static_cast<std::size_t>(track)].name + " mix",
                                  -1, -1, "mixer-" + juce::String(track));
        };
        devicesView.onPresetChanged = [this](int preset) {
            releaseNotes();
            controller.setLivePreset(preset);
            controller.commit("Change live sound");
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
        mixerPanel.rebuild();
        stepsView.refresh();
        sessionView.rebuildGrid();
        sessionView.refreshFromDocument();
        loadSelectedClip();
        updateEditorTitle();
        refreshExtraViews();
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
        // it and republishes it through onChange; one undo step covers both.
        suppressPianoRollCommit = true;
        pianoRoll.clearAllNotes();
        suppressPianoRollCommit = false;
        controller.commit("Clear clip", track, scene);
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
        case 1: workspace.setEditorTitle("Piano Roll", d.tracks[static_cast<std::size_t>(d.selectedTrack)].name + dot
                                         + d.scenes[static_cast<std::size_t>(d.selectedScene)].name); break;
        case 2: workspace.setEditorTitle("Session", sessionView.subtitle()); break;
        case 3: workspace.setEditorTitle("Mixer", juce::String(d.trackCount()) + " channels"); break;
        default: break;
        }
    }

    void updateOctave() { keyboardDock.setOctave(typingInput.typing.octave()); }

    // Undo / redo / file commands --------------------------------------------
    void undo() { if (projectBusy || controller.isBusy()) return; snapshotPlugins(); releaseNotes(); controller.undo(); }
    void redo() { if (projectBusy || controller.isBusy()) return; snapshotPlugins(); releaseNotes(); controller.redo(); }
    void newProject() { confirmDiscardChanges("start a new session", [this] { controller.newProject(); }); }
    void openProject() { confirmDiscardChanges("open another session", [this] { controller.chooseProjectToOpen(); }); }

    juce::String projectName() const {
        const auto& file = controller.currentFile();
        return file != juce::File{} ? file.getFileNameWithoutExtension() : juce::String("Untitled");
    }

    void updateUndoState() {
        topBar.setUndoState(controller.canUndo() ? controller.undoLabel() : juce::String{},
                            controller.canRedo() ? controller.redoLabel() : juce::String{});
        const bool dirty = controller.isDirty();
        if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
            window->setName((dirty ? "*" : "") + projectName() + juce::String(" - Alien Studio"));
        if (dirty && !wasDirty)
            statusBar.setProjectStatus(projectName() + juce::String(juce::CharPointer_UTF8("  \xc2\xb7  unsaved changes")),
                                       "Autosaved every minute; use Save to keep your work.");
        wasDirty = dirty;
        updateStatusState();
    }

    void parentHierarchyChanged() override {
        auto* window = getTopLevelComponent();
        if (window == this || window == keySource.getComponent()) return;
        if (keySource != nullptr) keySource->removeKeyListener(this);
        keySource = window;
        if (window != nullptr) window->addKeyListener(this);
        updateUndoState();
    }

    // Window-level shortcuts. Text fields and the piano roll handle their own
    // Ctrl+Z first; this only sees keys nothing else used.
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        const auto mods = key.getModifiers();
        if (!mods.isCommandDown() || mods.isAltDown()) return false;
        const auto code = juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (code == 'Z' && !mods.isShiftDown()) { undo(); return true; }
        if ((code == 'Z' && mods.isShiftDown()) || code == 'Y') { redo(); return true; }
        if (code == 'S' && mods.isShiftDown()) { controller.chooseSavePath(false); return true; }
        if (code == 'S') { controller.save(); return true; }
        if (code == 'O') { openProject(); return true; }
        if (code == 'N') { newProject(); return true; }
        return false;
    }

    void updateStatusState() {
        using State = alien::StatusBar::State;
        statusBar.setState(controller.isBusy() ? State::busy
                           : !controller.currentFile().existsAsFile() ? State::unsaved
                           : controller.isDirty() ? State::edited : State::saved);
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

    // After a crash the previous session's autosave is offered once the main
    // window is on screen.
    static void offerRecoveryWhenVisible(juce::Component::SafePointer<MainComponent> safeThis) {
        juce::Timer::callAfterDelay(300, [safeThis] {
            if (safeThis == nullptr) return;
            if (!safeThis->isShowing()) { offerRecoveryWhenVisible(safeThis); return; }
            const auto when = alien::ProjectController::recoveryFile().getLastModificationTime();
            const auto original = alien::ProjectController::recoveredOriginalFile();
            auto* window = new juce::AlertWindow("Recover unsaved work?",
                "Alien Studio did not close normally. An autosave of "
                    + (original != juce::File{} ? "\"" + original.getFileNameWithoutExtension() + "\"" : juce::String("your session"))
                    + " from " + when.toString(true, true, false) + " is available.",
                juce::MessageBoxIconType::NoIcon, safeThis.getComponent());
            window->addButton("Recover", 1, juce::KeyPress(juce::KeyPress::returnKey));
            window->addButton("Discard", 0);
            window->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result) {
                if (safeThis == nullptr) return;
                if (result == 1) safeThis->controller.recoverSession();
                else safeThis->controller.discardRecovery();
            }), true);
        });
    }

    // Callbacks ---------------------------------------------------------------
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const int newMode = tabs.getCurrentTabIndex() == 0 ? 0 : tabs.getCurrentTabIndex() == 5 ? 2 : 1;
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
        arrangementPanel.setPlayhead(engine.arrangementPosition.load());
        audioTools.setDestination(d.selectedTrack, d.selectedScene);
        if (activeCapture) {
            audioTools.setMeter(activeCapture->level.load());
            audioTools.setStatus("Recording " + juce::String(activeCapture->frames.load() / activeCapture->sampleRate, 1) + " s");
            if (activeCapture->done.load()) finishRecording();
        }
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        pianoRoll.setPlayheadStep(engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load() / 1000.0);
        if (pluginHost.changed.exchange(false)) controller.runtimePluginEdited();
        if (++statusTicks % 10 == 0) updateEngineStatus();
        if (statusTicks % (20 * 60) == 0) controller.autosaveIfNeeded(); // Once a minute.
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

    void showAudioSettings() {
        if(projectBusy || activeCapture) return;
        if(audioSettingsWindow){audioSettingsWindow->toFront(true);return;}
        auto* selector=new juce::AudioDeviceSelectorComponent(audio,0,2,0,2,false,false,true,false);
        selector->setSize(560,420);
        juce::DialogWindow::LaunchOptions options;options.content.setOwned(selector);
        options.dialogTitle="Audio input & output";options.dialogBackgroundColour=alien::ui::colours::surface;
        options.useNativeTitleBar=true;options.resizable=false;audioSettingsWindow=options.launchAsync();
    }
    void chooseRecording() {
        if(activeCapture){finishRecording();return;}
        if((projectBusy && !unsavedTake) || audioWorkCancellation)return;
        const int track=controller.document().selectedTrack,scene=controller.document().selectedScene;
        takeFileChooser=std::make_unique<juce::FileChooser>("Save new recording",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getNonexistentChildFile("AlienTake",".wav"),"*.wav");
        juce::Component::SafePointer<MainComponent> safe(this);
        takeFileChooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,
            [safe,track,scene](const juce::FileChooser& chooser){if(safe && chooser.getResult()!=juce::File{})safe->beginRecording(chooser.getResult().withFileExtension("wav"),track,scene);});
    }
    void beginRecording(const juce::File& file,int track,int scene) {
        if((projectBusy && !unsavedTake) || activeCapture)return;
        if(file.exists()){audioTools.setStatus("Choose a new filename to preserve existing audio.");return;}
        if(unsavedTake){takeTarget=file;setProjectBusy(true);queueTakeWrite({});return;}
        auto* device=audio.getCurrentAudioDevice();
        if(!device || device->getActiveInputChannels().countNumberOfSetBits()<audioTools.inputChannels()) {
            audioTools.setStatus("Enable the required input channels in Audio settings, then Record take.");return;
        }
        const double rate=device->getCurrentSampleRate();
        if(rate<8000 || rate>192000 || !std::isfinite(rate)){audioTools.setStatus("Unsupported input sample rate.");return;}
        const auto bytes=static_cast<std::size_t>(rate)*60*static_cast<std::size_t>(audioTools.inputChannels())*sizeof(float);
        if(bytes>controller.availableAudioBytes()){audioTools.setStatus("Insufficient session audio capacity for a 60-second take.");return;}
        try { activeCapture=std::make_shared<alien::AudioCapture>(rate,audioTools.inputChannels()); }
        catch(const std::bad_alloc&){audioTools.setStatus("Not enough memory to record.");return;}
        takeTarget=file;takeTrack=track;takeScene=scene;
        setProjectBusy(true);audioTools.setBusy(true,true);audioTools.setDestination(track,scene);
        engine.capture.store(activeCapture.get(),std::memory_order_release);
        audioTools.setStatus("Recording… Input is not monitored. Stop & save take when finished.");
    }
    void finishRecording() {
        if(!activeCapture)return;
        // Message-thread-only: use the manager's existing dispatch lock for one
        // pointer exchange. No allocation/I/O under the lock and no transport reset.
        { const juce::ScopedLock lock(audio.getAudioCallbackLock());engine.capture.store(nullptr); }
        queueTakeWrite(std::move(activeCapture));
    }
    void queueTakeWrite(std::shared_ptr<alien::AudioCapture> take) {
        const auto file=takeTarget;const int track=takeTrack,scene=takeScene;
        const auto retained=unsavedTake;
        audioTools.setBusy(true);audioTools.setMeter(0);audioTools.setStatus("Saving take…");
        audioWorkCancellation=std::make_shared<std::atomic<bool>>(false);auto cancel=audioWorkCancellation;
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,take,retained,file,track,scene,cancel]{
            auto result=std::make_shared<alien::AudioClipImportResult>();result->clip=take?take->finish():retained;
            if(!result->clip)result->error="No input samples captured.";
            else alien::writeClipWav(file,*result->clip,*cancel,result->error);
            juce::MessageManager::callAsync([safe,file,track,scene,result]{if(!safe)return;
                safe->audioWorkCancellation.reset();safe->setProjectBusy(false);safe->audioTools.setBusy(false);
                if(result->clip && result->error.isEmpty()){
                    safe->unsavedTake.reset();safe->finishAudioImport(file,track,scene,*result);
                    safe->audioTools.setStatus("Saved take: "+file.getFullPathName());
                    if(safe->closeAfterTake)juce::JUCEApplication::getInstance()->systemRequestedQuit();
                } else {
                    safe->unsavedTake=result->clip;safe->closeAfterTake=false;
                    if(safe->unsavedTake)safe->setProjectBusy(true);
                    safe->audioTools.setStatus(result->error+" Take retained in memory. Record take retries Save to a new file.");
                }
            });return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }
    void chooseTempoWarp() {
        if(audioWorkCancellation){audioWorkCancellation->store(true);audioTools.setStatus("Cancelling tempo render…");return;}
        if(projectBusy)return;
        const int track=controller.document().selectedTrack,scene=controller.document().selectedScene;
        const auto& clip=controller.document().clip(track, scene);
        if(!clip.audio){audioTools.setStatus("Select an audio clip in Session before rendering a tempo match.");return;}
        const double ratio=audioTools.sourceBpm()/engine.bpm.load();
        if(ratio<0.5 || ratio>2 || clip.audio->frames/clip.audio->sampleRate>60){audioTools.setStatus("Use a clip up to 60 seconds and a duration ratio between 0.5 and 2.");return;}
        warpFileChooser=std::make_unique<juce::FileChooser>("Save tempo-matched audio as a new WAV",
            juce::File(clip.audioFilePath).getSiblingFile(juce::File(clip.audioFilePath).getFileNameWithoutExtension()+"-warped.wav"),"*.wav");
        const auto* source=clip.audio.get();
        juce::Component::SafePointer<MainComponent> safe(this);
        warpFileChooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,
            [safe,source,track,scene,ratio](const juce::FileChooser& chooser){if(safe && chooser.getResult()!=juce::File{})safe->startTempoWarp(chooser.getResult().withFileExtension("wav"),source,track,scene,ratio);});
    }
    void startTempoWarp(const juce::File& file,const alien::SessionAudioClip* source,int track,int scene,double ratio) {
        if(projectBusy)return;
        if(file.exists()){audioTools.setStatus("Choose a new filename to preserve existing audio.");return;}
        const auto bytes=static_cast<std::size_t>(std::ceil(source->frames*ratio))*static_cast<std::size_t>(source->channels)*sizeof(float);
        if(bytes>controller.availableAudioBytes()){audioTools.setStatus("Session audio capacity exceeded.");return;}
        setProjectBusy(true);audioTools.setBusy(true,false,true);audioTools.setStatus("Rendering pitch-preserving tempo match…");
        audioWorkCancellation=std::make_shared<std::atomic<bool>>(false);auto cancel=audioWorkCancellation;
        const auto assets=controller.document();
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,file,source,track,scene,ratio,cancel,assets]{
            auto result=std::make_shared<alien::AudioClipImportResult>();result->clip=alien::warpTempo(*source,ratio,*cancel);
            if(!result->clip)result->error="Tempo render cancelled or exceeds supported limits.";
            else if(!alien::writeClipWav(file,*result->clip,*cancel,result->error))result->clip.reset();
            juce::MessageManager::callAsync([safe,file,track,scene,result]{if(!safe)return;
                safe->audioWorkCancellation.reset();safe->setProjectBusy(false);safe->audioTools.setBusy(false);
                if(result->clip){safe->finishAudioImport(file,track,scene,*result);safe->audioTools.setStatus("Tempo match saved. Original file preserved; reimport it to restore the source.");}
                else safe->audioTools.setStatus(result->error);
            });return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }

    void setProjectBusy(bool busy) {
        projectBusy = busy;
        controller.setExternalBusy(busy);
        topBar.setBusy(busy); sessionView.setBusy(busy); drumPanel.setBusy(busy);
        mixerPanel.setEnabled(!busy); arrangementPanel.setEnabled(!busy);
    }
    void finishAudioImport(const juce::File& file, int track, int scene, const alien::AudioClipImportResult& result) {
        if (!controller.installAudio(file, track, scene, result.clip)) audioTools.setStatus("Audio could not be installed: session memory limit reached.");
    }
    void refreshExtraViews() {
        const auto& d = controller.document();
        juce::StringArray names, scenes;
        std::array<std::array<juce::String, alien::drumPadCount>, alien::maxSessionTracks> paths{};
        for (int t = 0; t < d.trackCount(); ++t) { names.add(d.tracks[t].name); paths[t] = d.tracks[t].drumPaths; }
        for (const auto& scene : d.scenes) scenes.add(scene.name);
        drumPanel.setTrackNames(names); drumPanel.setPaths(paths);
        refreshPlugins();
        arrangementPanel.setGridNames(names, scenes); arrangementPanel.setArrangement(d.arrangement);
    }
    void chooseWavExport() {
        if (exportCancellation) { exportCancellation->store(true); return; }
        if (projectBusy || controller.isBusy()) return;
        exportFileChooser = std::make_unique<juce::FileChooser>("Export arrangement WAV", juce::File{}, "*.wav");
        juce::Component::SafePointer<MainComponent> safe(this);
        exportFileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe](const juce::FileChooser& chooser) { if (safe && chooser.getResult() != juce::File{}) safe->exportWav(chooser.getResult().withFileExtension("wav")); });
    }
    void exportWav(juce::File file) {
        if (projectBusy || controller.isBusy()) return;
        snapshotPlugins();
        auto song = controller.captureArrangement();
        auto project = std::make_shared<alien::SessionProjectData>(alien::toProjectData(controller.document()));
        auto renderEngine = std::make_shared<ToneEngine>();
        renderEngine->prepare(48000);
        { alien::PluginHost renderHost(*renderEngine->plugins);
          const auto errors=renderHost.restore(pluginStates(),controller.document().masterPlugins,48000);
          if(errors.isNotEmpty()){ arrangementPanel.setStatus("Export stopped: " + errors);return; } }
        auto assets = controller.document();
        alien::DrumSamplePointers drums{};
        for (int t = 0; t < assets.trackCount(); ++t) for (int p = 0; p < alien::drumPadCount; ++p) drums[t][p] = assets.tracks[t].drumAssets[p].get();
        exportCancellation = std::make_shared<std::atomic<bool>>(false); auto cancel = exportCancellation;
        setProjectBusy(true); arrangementPanel.setEnabled(true); arrangementPanel.setStatus("Exporting WAV. Click Export WAV again to cancel.");
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job = [safe, file, song, project, assets, drums, cancel, renderEngine] {
            juce::String error; const bool ok = alien::exportArrangementWav(file, *project, *song, drums, *cancel, error, renderEngine.get());
            juce::MessageManager::callAsync([safe, file, ok, error] { if (!safe) return; safe->exportCancellation.reset(); safe->setProjectBusy(false); safe->arrangementPanel.setStatus(ok ? "Exported " + file.getFullPathName() : error); });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioWorkPool.addJob(std::move(job));
    }

    static juce::File pluginListFile() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio/Plugins.xml");
    }
    int pluginTarget() const {
        const int t=pluginPanel.destination();
        return t==alien::maxSessionTracks ? t : juce::jlimit(0,controller.document().trackCount()-1,t);
    }
    std::vector<alien::PluginRackState> pluginStates() const {
        std::vector<alien::PluginRackState> states;
        for (const auto& t:controller.document().tracks) states.push_back(t.plugins);
        return states;
    }
    void refreshPlugins() {
        juce::StringArray names; for(const auto& t:controller.document().tracks) names.add(t.name);
        const int t=pluginTarget();
        pluginPanel.refresh(names,t==alien::maxSessionTracks?controller.document().masterPlugins:controller.document().tracks[t].plugins,knownPlugins);
    }
    double deviceRate() const { auto* device=audio.getCurrentAudioDevice(); return device?device->getCurrentSampleRate():44100.0; }
    void snapshotPlugins() {
        // Native parameters may have changed since the most recent timer tick.
        // State capture and topology changes run only with audio detached.
        pluginHost.changed.store(false);
        auto tracks=pluginStates(); auto master=controller.document().masterPlugins;
        audio.removeAudioCallback(&engine);
        pluginHost.snapshot(tracks,master);
        audio.addAudioCallback(&engine);
        controller.setPluginRacks(tracks,master);
    }
    void restorePlugins() {
        audio.removeAudioCallback(&engine);
        auto errors=pluginHost.restore(pluginStates(),controller.document().masterPlugins,deviceRate());
        audio.addAudioCallback(&engine);
        pluginPanel.setStatus(errors.isEmpty()?"Plugin racks restored. Select a slot to open its editor.":"Missing or unsupported plugins (saved state preserved):\n"+errors);
        refreshPlugins();
    }
    void addPlugin(bool instrument,int index) {
        if(projectBusy || controller.isBusy()) return;
        auto types=knownPlugins.getTypes(); if(index<0 || index>=types.size())return;
        if(types[index].isInstrument!=instrument) { pluginPanel.setStatus("Select an instrument for the instrument slot, or an FX plugin for Add FX.");return; }
        const int t=pluginTarget(); if(instrument && t==alien::maxSessionTracks)return;
        snapshotPlugins(); auto states=pluginStates();auto master=controller.document().masterPlugins;
        auto& rack=t==alien::maxSessionTracks?master:states[t];
        alien::PluginState next{types[index].createXml()->toString(),{},false};
        if(instrument)rack.instrument=next;
        else { if(rack.effects.size()>=8){pluginPanel.setStatus("Each rack supports eight FX slots.");return;}rack.effects.push_back(next); }
        controller.setPluginRacks(states,master); restorePlugins();
    }
    void pluginAction(int op,int slot) {
        if(projectBusy || controller.isBusy())return;
        const int t=pluginTarget();
        if(op==0){pluginHost.openEditor(t,slot);return;}
        snapshotPlugins();auto states=pluginStates();auto master=controller.document().masterPlugins;
        auto& rack=t==alien::maxSessionTracks?master:states[t];
        if(slot<0){if(t==alien::maxSessionTracks)return;if(op==1)rack.instrument.bypass=!rack.instrument.bypass;else if(op==2)rack.instrument={};}
        else if(slot<static_cast<int>(rack.effects.size())){
            if(op==1)rack.effects[slot].bypass=!rack.effects[slot].bypass;
            else if(op==2)rack.effects.erase(rack.effects.begin()+slot);
            else { const int other=slot+(op==3?-1:1);if(other>=0 && other<static_cast<int>(rack.effects.size()))std::swap(rack.effects[slot],rack.effects[other]); }
        }
        controller.setPluginRacks(states,master);restorePlugins();
    }
    void choosePluginScan() {
        if(projectBusy || controller.isBusy())return;
        pluginChooser=std::make_unique<juce::FileChooser>("Select a 64-bit VST3 bundle",juce::File{},"*.vst3");
        juce::Component::SafePointer<MainComponent> safe(this);
        pluginChooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& chooser){if(safe && chooser.getResult()!=juce::File{})safe->scanPlugin(chooser.getResult());});
    }
    void scanPlugin(juce::File bundle) {
        if(projectBusy || controller.isBusy())return;
        if(!bundle.hasFileExtension("vst3")){pluginPanel.setStatus("Choose a .vst3 bundle, not its parent folder.");return;}
        setProjectBusy(true);pluginPanel.setStatus("Scanning in a separate process (60-second timeout)...");
        juce::Component::SafePointer<MainComponent> safe(this);
        const auto executable=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,bundle,executable]{
            auto output=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienPluginScan",".xml");
            juce::ChildProcess child;juce::String error;
            if(!child.start(juce::StringArray{executable,"--scan-vst3",bundle.getFullPathName(),output.getFullPathName()},0))error="Could not start scanner";
            else if(!child.waitForProcessToFinish(60000)){child.kill();child.waitForProcessToFinish(1000);error="Scanner timed out";}
            else if(child.getExitCode()!=0)error="Plugin scan failed or crashed";
            auto xml=std::shared_ptr<juce::XmlElement>(juce::parseXML(output).release());output.deleteFile();
            juce::MessageManager::callAsync([safe,bundle,xml,error]{if(!safe)return;safe->setProjectBusy(false);
                if(error.isNotEmpty()||!xml){
                    auto failures=pluginListFile().getSiblingFile("PluginScanFailures.txt");failures.getParentDirectory().createDirectory();failures.appendText(bundle.getFullPathName()+"\n");
                    safe->pluginPanel.setStatus(error+". Failure recorded; select Scan VST3 to retry explicitly.");return;
                }
                juce::KnownPluginList found;found.recreateFromXml(*xml);for(const auto& type:found.getTypes())safe->knownPlugins.addType(type);
                pluginListFile().getParentDirectory().createDirectory();safe->knownPlugins.createXml()->writeTo(pluginListFile());safe->refreshPlugins();safe->pluginPanel.setStatus("Scan complete. Choose an instrument or FX from the list.");
            });return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }

    // Members (order matters: construction top to bottom) -------------------
    alien::ui::StudioLookAndFeel studioLookAndFeel;
    juce::TooltipWindow tooltipWindow{this, 650};
    ToneEngine engine;
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    alien::ProjectController controller{engine};
    alien::PluginHost pluginHost{*engine.plugins};
    alien::PluginPanel pluginPanel;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::FileChooser> pluginChooser;
    alien::TopBar topBar;
    alien::StatusBar statusBar;
    alien::KeyboardDock keyboardDock{keyboardState};
    alien::StepSequencerView stepsView{controller};
    alien::PianoRollEditor pianoRoll;
    alien::SessionView sessionView{controller, engine};
    alien::TrackMixerPanel mixerPanel{controller, engine.tracks};
    alien::DevicesView devicesView{engine.midi};
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    alien::CompactWorkspace workspace{tabs, devicesView};
    TypingPianoRouter typingInput{*this, keyboardState};
    juce::Component::SafePointer<juce::Component> keySource;
    bool loadingClip{}, suppressPianoRollCommit{}, wasDirty{};
    int statusTicks{};
    alien::DrumSamplerPanel drumPanel;
    alien::ArrangementPanel arrangementPanel;
    alien::AudioToolsPanel audioTools;
    bool projectBusy{}, closeAfterTake{};
    std::shared_ptr<alien::AudioCapture> activeCapture;
    std::shared_ptr<alien::SessionAudioClip> unsavedTake;
    juce::File takeTarget;
    int takeTrack{}, takeScene{};
    std::unique_ptr<juce::FileChooser> takeFileChooser, warpFileChooser, exportFileChooser;
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow;
    std::shared_ptr<std::atomic<bool>> audioWorkCancellation, exportCancellation;
    juce::ThreadPool audioWorkPool{1};
};
