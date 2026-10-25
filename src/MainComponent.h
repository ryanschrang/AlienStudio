#pragma once
#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "CompactWorkspace.h"
#include "TrackMixerPanel.h"
#include "ProjectController.h"
#include "StepSequencerView.h"
#include "ChannelRackView.h"
#include "TimelineView.h"
#include "SessionView.h"
#include "TopBar.h"
#include "ApplicationToolbar.h"
#include "DetachableView.h"
#include "ViewModes.h"
#include "ShellPanels.h"
#include "StudioWidgets.h"
#include "DrumSamplerPanel.h"
#include "AudioToolsPanel.h"
#include "AudioFileWrite.h"
#include "DiskRecording.h"
#include "TempoWarp.h"
#include "StreamingTempoWarp.h"
#include "WavExport.h"
#include "PluginHost.h"
#include "PluginPanel.h"
#include "PluginPreflight.h"

// The application window's content. It owns the audio engine and device, the
// project controller and every view, and wires them together. Views never talk
// to each other directly; they report intent and this class routes it.
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,   // the Song's clip list is dragged onto its lanes
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
        addAndMakeVisible(applicationToolbar);
        addAndMakeVisible(statusBar);
        addChildComponent(keyboardDock);
        wireTopBar();
        wireApplicationToolbar();
        wireController();
        wireEditors();

        addWorkspace("16-step sequencer", stepsView);
        addWorkspace("Piano roll", pianoRoll);
        addWorkspace("Session", sessionView);
        addWorkspace("Mixer", mixerPanel);
        addWorkspace("Drums", drumPanel);
        addWorkspace("Arrangement", timelineView);
        addWorkspace("Audio lab", audioTools);
        drumPanel.onLoad = [this](int t, int p, const juce::File& f) { controller.loadDrum(f, t, p); };
        drumPanel.onClear = [this](int t, int p) { controller.clearDrum(t, p); refreshExtraViews(); };
        drumPanel.onHit = [this](int t, int p) { engine.drumAuditions[t].push(alien::drumBaseNote + p, 0.8f); };
        drumPanel.onRoute = [this](int t) { engine.liveDrumTrack.store(t); };
        wireTimeline();
        audioTools.onRecord = [this] { chooseRecording(); };
        audioTools.onSettings = [this] { showAudioSettings(); };
        audioTools.onWarp = [this] { chooseTempoWarp(); };
        audioTools.onRecover = [this] { chooseTakeRecovery(); };
        addWorkspace("Plugins", pluginPanel);
        addWorkspace("Channel rack", rackView); // index 8: earlier tab numbers never change
        rackView.onTitleChanged = [this] { updateEditorTitle(); };
        rackView.rackGrid().onOpenPianoRoll = [this](int channel) { openRackChannel(channel); };
        pluginPanel.onScanCommand = [this](alien::ScanCommand command) { handleScanCommand(command); };
        pluginPanel.onCancelScan = [this] { if (scanCancel) scanCancel->store(true); };
        pluginPanel.health = [this](int destination, int slot) { return pluginHost.health(destination, slot); };
        pluginHost.onProblem = [this](const juce::String& problem) {
            pluginPanel.setStatus(problem);
            statusBar.setProjectStatus("Plugin problem: " + problem.upToFirstOccurrenceOf("\n", false, false), problem);
            refreshPlugins();
        };
        // The first time a plugin file is seen it is tried out in a separate process, so a plugin that
        // crashes or hangs while loading cannot take the session (and unsaved work) with it.
        preflight = std::make_unique<alien::Preflight>(alien::scan::ScanDatabase::defaultFile().getSiblingFile("PluginPreflight.xml"),
            alien::isolatedPreflightChecker(juce::File::getSpecialLocation(juce::File::currentExecutableFile), 45000), alien::DiagnosticLog(hostLogFile()));
        pluginPanel.setPreflightEnabled(preflight->enabled());
        pluginHost.preflight = [this](const juce::PluginDescription& description) { return preflight->verify(description); };
        scanDatabase.load(alien::scan::ScanDatabase::defaultFile());
        pluginPanel.onTarget = [this](int) { refreshPlugins(); };
        pluginPanel.onAdd = [this](bool instrument, int choice) { addPlugin(instrument,choice); };
        pluginPanel.onAction = [this](int op,int slot) { pluginAction(op,slot); };
        if (auto xml = juce::parseXML(pluginListFile())) knownPlugins.recreateFromXml(*xml);
        controller.onBeforeSnapshot = [this] { snapshotPlugins(SnapshotReason::save); };
        controller.onBeforeAutosaveSnapshot = [this] { snapshotPlugins(SnapshotReason::autosave); };
        controller.beforeStructureEdit = [this] { snapshotPlugins(SnapshotReason::whenEdited); };
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
        if(!alien::RecordingFile::unfinished().isEmpty()) audioTools.setStatus("Unfinished recording takes are available. Use Recover takes in Audio lab.");
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
        if (scanCancel) scanCancel->store(true);
        scanPool.removeAllJobs(true, 20000);
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
        applicationToolbar.setBounds(area.removeFromTop(alien::ApplicationToolbar::height));
        topBar.setBounds(area.removeFromTop(alien::TopBar::height));
        statusBar.setBounds(area.removeFromBottom(alien::StatusBar::height));
        keyboardDock.setVisible(keyboardVisible);
        keyboardDock.setBounds(keyboardVisible ? area.removeFromBottom(getHeight() < 760 ? alien::KeyboardDock::compactHeight
                                                                                       : alien::KeyboardDock::fullHeight)
                                               : juce::Rectangle<int>{});
        workspace.setBounds(area);
    }

    void paint(juce::Graphics& g) override { g.fillAll(alien::ui::colours::window); }

    // Ask to save unsaved changes, then run "then" unless the user cancels.
    void confirmDiscardChanges(const juce::String& action, std::function<void()> then) {
        if (controller.isBusy() || projectBusy) return;
        if (pluginHost.changed.exchange(false)) controller.runtimePluginEdited();
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
        if (controller.isRecording()) {
            // Let a MIDI take finish first so what was played is kept (and offered for saving).
            controller.stopRecording();
            juce::Component::SafePointer<MainComponent> safe(this);
            juce::Timer::callAfterDelay(250, [safe, quit] {
                if (safe == nullptr) return;
                safe->controller.pollRecording();
                if (safe->controller.isRecording()) safe->controller.abandonRecording(); // the audio engine is not answering
                safe->requestQuit(quit);
            });
            return;
        }
        if (projectBusy) { audioTools.setStatus("Finish saving the take or audio operation before closing."); showView(6); return; }

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

    // Switching between our own main/detached windows must not reset live MIDI.
    // Defer until the new foreground window is established, then panic on app blur.
    void applicationWindowDeactivated() {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe]{if(safe&&!juce::Process::isForegroundProcess())safe->releaseNotes();});
    }

private:
    void addWorkspace(const juce::String& title,juce::Component& view) {
        const int index=tabs.getNumTabs();
        jassert(index<static_cast<int>(detachableViews.size()));
        auto panel=std::make_unique<alien::DetachableView>(title,view);
        panel->onWindowOpened=[this](juce::Component& window){window.addKeyListener(this);typingInput.addAuxiliaryRoot(window);};
        panel->onWindowClosed=[this](juce::Component& window){window.removeKeyListener(this);typingInput.removeAuxiliaryRoot(window);};
        panel->onWindowDeactivated=[this]{applicationWindowDeactivated();};
        panel->onDocked=[this,index]{
            if(!dockingAll){showView(index);if(auto* window=getTopLevelComponent())window->toFront(true);}
            applicationToolbar.refreshState();
        };
        tabs.addTab(title,alien::ui::colours::surface,panel.get(),false);
        detachableViews[static_cast<size_t>(index)]=std::move(panel);
    }
    alien::DetachableView* currentWorkspace() const {
        const int index=tabs.getCurrentTabIndex();
        return index>=0&&index<static_cast<int>(detachableViews.size())?detachableViews[static_cast<size_t>(index)].get():nullptr;
    }
    void wireApplicationToolbar() {
        using Menu=alien::ApplicationToolbar;
        applicationToolbar.queryState=[this] {
            Menu::State s;
            s.busy=projectBusy||controller.isBusy();
            s.recording=controller.isRecording()||activeCapture!=nullptr;
            s.playing=engine.playing.load();s.view=tabs.getCurrentTabIndex();
            s.deviceView=workspace.isDeviceView();s.keyboardVisible=keyboardVisible;
            s.metronome=controller.document().recording.metronome;s.songMode=songPlayback;
            s.hasSong=controller.timeline().hasRegions();
            s.hasLoop=controller.timeline().loopEnd>controller.timeline().loopStart;
            if(auto* current=currentWorkspace())s.viewDetached=current->isDetached();
            for(const auto& view:detachableViews)if(view&&view->isDetached())s.hasDetachedViews=true;
            if(controller.canUndo())s.undoLabel=controller.undoLabel();
            if(controller.canRedo())s.redoLabel=controller.redoLabel();
            return s;
        };
        applicationToolbar.onCommand=[this](int id) {
            using Menu=alien::ApplicationToolbar;
            if(id>=Menu::viewBase&&id<Menu::viewBase+Menu::viewCount){showView(id-Menu::viewBase);return;}
            switch(id) {
                case Menu::newProject:newProject();break;
                case Menu::openProject:openProject();break;
                case Menu::save:controller.save();break;
                case Menu::saveAs:controller.chooseSavePath(false);break;
                case Menu::savePortable:controller.chooseSavePath(true);break;
                case Menu::exportSong:showView(5);chooseWavExport();break;
                case Menu::exportLoop:showView(5);chooseWavExport(controller.timeline().loopStart,controller.timeline().loopEnd);break;
                case Menu::quit:if(auto* app=juce::JUCEApplication::getInstance())app->systemRequestedQuit();break;
                case Menu::undo:undo();break;
                case Menu::redo:redo();break;
                case Menu::devices:case Menu::midiSettings:workspace.showDevices(true);break;
                case Menu::keyboard:
                    // Release the UI's held notes without interrupting external MIDI or playback.
                    typingInput.release();keyboardState.allNotesOff(1);
                    keyboardVisible=!keyboardVisible;resized();break;
                case Menu::playStop:togglePlay();break;
                case Menu::record:if(activeCapture)finishRecording();else toggleRecording();break;
                case Menu::metronome: {
                    auto settings=controller.document().recording;settings.metronome=!settings.metronome;
                    controller.setRecordSettings(settings);break;
                }
                case Menu::songMode:setSongMode(!songPlayback);break;
                case Menu::audioSettings:showAudioSettings();break;
                case Menu::recordingOptions:topBar.openRecordingOptions();break;
                case Menu::plugins:showView(7);break;
                case Menu::detachView:if(auto* view=currentWorkspace())view->detach();break;
                case Menu::dockView:if(auto* view=currentWorkspace())view->dock();break;
                case Menu::dockAllViews:
                    dockingAll=true;for(auto& view:detachableViews)if(view)view->dock();dockingAll=false;break;
                case Menu::recoverTakes:showView(6);chooseTakeRecovery();break;
                case Menu::shortcuts:
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"Keyboard shortcuts",
                        "F10: open application menus (arrows to navigate, Escape to close)\n"
                        "Ctrl+,: audio device settings\nCtrl+N / O / S: new, open, save\n"
                        "Ctrl+Shift+S: save as\nCtrl+Z: undo\nCtrl+Shift+Z / Ctrl+Y: redo\n"
                        "Shift+Tab: switch editor / devices\nZ / X: typing-piano octave\n\n"
                        "Editors keep their own shortcuts. Text fields keep normal text editing.","OK",this);break;
                case Menu::about:
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"About Alien Studio",
                        "Alien Studio 0.1.0\nAn original, free and open-source digital audio workstation.\n\n"
                        "Create patterns, arrange songs, record audio and MIDI, and host VST3 instruments and effects.","OK",this);break;
                default:break;
            }
            applicationToolbar.refreshState();
        };
    }

    // Wiring ------------------------------------------------------------------
    void wireTopBar() {
        topBar.onViewSelected = [this](int index) { showView(index); };
        topBar.onPlayToggled = [this] { togglePlay(); };
        topBar.onSongModeToggled = [this](bool on) { setSongMode(on); };
        topBar.onRecordToggled = [this] { toggleRecording(); };
        topBar.onMetronomeToggled = [this](bool on) {
            auto settings = controller.document().recording;
            settings.metronome = on;
            controller.setRecordSettings(settings);
        };
        topBar.onRecordSettingsChanged = [this](const alien::RecordSettings& settings) {
            controller.setRecordSettings(settings);
        };
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

    void togglePlay() {
        // During a count-in the transport has not started, so Play/Stop cancels the take instead.
        if (engine.recording.phase.load() == alien::RecordControls::countIn) { controller.stopRecording(); return; }
        releaseNotes();
        const bool starting = !engine.playing.load();
        // Playing the song with nothing on it is silent; say why instead of leaving the transport running for no reason.
        if (starting && engine.patternMode.load() == alien::songMode && !controller.timeline().hasRegions())
            statusBar.setProjectStatus("The Song is empty. Open the Song view and place a pattern or clip, then press Play.",
                                       "Song mode plays the arrangement. Drag a pattern or clip onto a lane in the Song view, or double-click a lane.");
        engine.playing.store(starting);
    }

    // "Song mode": Play plays the whole song from any view. Off, each view plays its own pattern or clips.
    void setSongMode(bool on) {
        songPlayback = on;
        topBar.setSongMode(on);
        const int mode = modeForTab(tabs.getCurrentTabIndex());
        if (engine.patternMode.exchange(mode) != mode) releaseNotes();
        if (on && !controller.timeline().hasRegions())
            statusBar.setProjectStatus("Song mode is on, but the Song is empty. Place patterns or clips in the Song view.",
                                       "Song mode plays the arrangement. Drag a pattern or clip onto a lane in the Song view, or double-click a lane.");
    }

    // The Arrangement screen: a click on the ruler (or a marker) picks where playback starts or jumps to, Space is
    // the transport, and double-clicking a clip opens what it plays.
    void wireTimeline() {
        timelineView.onExport = [this](std::int64_t from, std::int64_t to) { chooseWavExport(from, to); };
        timelineView.onTitleChanged = [this] { if (tabs.getCurrentTabIndex() == 5) updateEditorTitle(); };
        auto& canvas = timelineView.timelineCanvas();
        // The audio callback receives only the enabled inputs, in order; a track's input counts among those.
        canvas.inputNames = [this] {
            juce::StringArray names;
            if (auto* device = audio.getCurrentAudioDevice()) {
                const auto all = device->getInputChannelNames();
                const auto active = device->getActiveInputChannels();
                for (int i = 0; i < all.size(); ++i) if (active[i]) names.add(all[i]);
            }
            return names;
        };
        canvas.onSeek = [this](std::int64_t tick) { engine.timelineSeek.store(tick); };
        canvas.onPlayToggle = [this] { togglePlay(); };
        canvas.onOpenSource = [this](int row, int source) { openTimelineSource(row, source); };
        // A double-click on an empty lane made a new clip (and a Session scene for it when every cell of the track was taken).
        canvas.onClipCreated = [this](int, int, bool sceneAdded) {
            if (sceneAdded) sessionView.rebuildGrid();
            sessionView.refreshFromDocument();
            loadSelectedClip();
        };
    }

    // Double-click on a clip: edit what it plays (the piano roll or Session for a clip, the Channel Rack for a pattern).
    void openTimelineSource(int row, int source) {
        if (alien::timeline::isPatternRow(row)) {
            controller.selectRackPattern(source);
            rackView.refresh();
            showView(alien::rackTabIndex);
            return;
        }
        controller.selectClip(row, source);
        rollTarget = {};
        loadSelectedClip();
        sessionView.refreshFromDocument();
        if (controller.document().selectedClip().isAudioClip()) showView(2);
        else { showView(1); pianoRoll.grabKeyboardFocus(); }
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
        controller.beforeDocumentReplaced = [this] { releaseNotes(); rollTarget = {}; };
        controller.onDocumentReplaced = [this] {
            restorePlugins(true);
            refreshAll();
            tabs.setCurrentTabIndex(controller.document().selectedClip().isAudioClip() ? 2 : 1);
        };
        controller.onDocumentRestored = [this] { restorePlugins(); refreshAll(); };
        controller.onHistoryChanged = [this] { updateUndoState(); };
        controller.onRecordingFinished = [this](const alien::ProjectController::RecordingOutcome& outcome) {
            finishedRecording(outcome);
        };
        controller.onSongTakeFinished = [this](const alien::ProjectController::SongTakeOutcome& outcome) {
            finishedSongTake(outcome);
        };
        controller.onClipContentChanged = [this](int track, int scene) {
            sessionView.refreshSelectedClip();
            const auto& d = controller.document();
            if (track == d.selectedTrack && scene == d.selectedScene) tabs.setCurrentTabIndex(2);
        };
    }

    void wireEditors() {
        pianoRoll.onChange = [this] {
            if (rollTarget.rack) {
                if (loadingClip) return;
                const auto& rack = controller.channelRack();
                controller.setRackChannelNotes(rack.selectedPattern, rollTarget.channel, pianoRoll.getPattern(),
                                               "rack-roll-" + juce::String(rollGesture), &rollFit);
                rackView.refresh();
                return;
            }
            const auto& d = controller.document();
            controller.setClipPattern(d.selectedTrack, d.selectedScene, pianoRoll.getPattern(), !loadingClip);
            if (!loadingClip) sessionView.refreshSelectedClip();
        };
        pianoRoll.onEditCommitted = [this] {
            if (suppressPianoRollCommit) return;
            if (rollTarget.rack) {
                // The stored note sequence is authoritative; do not reload a lossy step conversion.
                ++rollGesture;
                return;
            }
            const auto& d = controller.document();
            controller.commit("Edit notes", d.selectedTrack, d.selectedScene);
        };
        pianoRoll.onAudition = [this](int note, float velocity) {
            if (rollTarget.rack) {
                controller.previewRackChannel(rollTarget.channel, velocity, note - alien::rack::channelRootPitch, true);
                return;
            }
            if (velocity > 0) keyboardState.noteOn(3, note, velocity);
            else keyboardState.noteOff(3, note, 0);
        };
        sessionView.onSelectionChanged = [this] { rollTarget = {}; loadSelectedClip(); };
        sessionView.onEditClip = [this](int track, int scene) {
            rollTarget = {};
            controller.selectClip(track, scene);
            loadSelectedClip();
            if (controller.document().selectedClip().isAudioClip()) tabs.setCurrentTabIndex(2);
            else {
                tabs.setCurrentTabIndex(1);
                pianoRoll.grabKeyboardFocus();
            }
        };
        sessionView.onClearClip = [this] { clearSelectedClip(); };
        sessionView.onRelinkAudio = [this] { chooseRelinkFolder(); };
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
        mixerPanel.onMasterEdited = [this](bool committed) {
            controller.masterGainEdited();
            if (committed) controller.commit("Change master volume", -1, -1, "master-volume");
        };
        mixerPanel.onTrackEdited = [this](int track, bool committed) {
            controller.trackSettingsEdited(track);
            sessionView.repaint();
            if (committed)
                controller.commit("Change " + controller.document().tracks[static_cast<std::size_t>(track)].name + " mix",
                                  -1, -1, "mixer-" + juce::String(track));
        };
        // The Session view's track headers hold the same volume, pan, mute and solo as the Mixer; an edit made there is committed like one made here.
        sessionView.onTrackMixEdited = [this](int track, bool committed) {
            mixerPanel.refreshSettings();
            if (mixerPanel.onTrackEdited) mixerPanel.onTrackEdited(track, committed);
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
        topBar.setRecordSettings(d.recording);
        devicesView.setPreset(d.livePreset);
        mixerPanel.rebuild();
        stepsView.refresh();
        rackView.refresh();
        sessionView.rebuildGrid();
        sessionView.refreshFromDocument();
        loadSelectedClip();
        updateEditorTitle();
        refreshExtraViews();
    }

    void loadSelectedClip() {
        if (rollTarget.rack && !controller.channelRack().validChannel(rollTarget.channel)) rollTarget = {};
        loadingClip = true;
        if (rollTarget.rack) {
            const auto& rack = controller.channelRack();
            pianoRoll.setPattern(alien::rack::channelNotes(rack.pattern(rack.selectedPattern), rollTarget.channel));
        } else pianoRoll.setPattern(controller.document().selectedClip().notes());
        loadingClip = false;
        sessionView.refreshSelectedClip();
        updateEditorTitle();
    }

    // Recording --------------------------------------------------------------
    void toggleRecording() {
        if (controller.isRecording()) { controller.stopRecording(); return; }
        if (projectBusy || controller.isBusy()) return;
        if (rollTarget.rack) { rollTarget = {}; loadSelectedClip(); }   // takes go into Session clips
        // In the Song view (or with Song mode on) Record records audio from the armed tracks onto the timeline.
        if (!workspace.isDeviceView() && engine.patternMode.load() == alien::songMode) { startSongTake(); return; }
        // MIDI recording plays Session clips, so it needs the clip mode that Song mode would replace.
        if (songPlayback) {
            setSongMode(false);
            statusBar.setProjectStatus("Song mode is off while recording MIDI into a clip.", "Turn Song mode on again after the take.");
        }
        // Recording plays Session clips, so make sure the view that does so is showing.
        if (workspace.isDeviceView() || (tabs.getCurrentTabIndex() != 1 && tabs.getCurrentTabIndex() != 2)) {
            showView(1);
            engine.patternMode.store(1); // the tab change reaches the engine only a moment later
            releaseNotes();
        }
        const auto start = controller.startRecording();
        if (!start.started) statusBar.setProjectStatus(start.reason, start.reason);
    }

    // Takes go next to the saved project (in "<project> takes"), or in Documents for an unsaved one.
    juce::File songTakeFolder() const {
        const auto project = controller.currentFile();
        if (project.existsAsFile()) return project.getSiblingFile(project.getFileNameWithoutExtension() + " takes");
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Alien Studio takes");
    }
    void startSongTake() {
        if (activeCapture) { statusBar.setProjectStatus("Finish the Audio lab take first.", {}); return; }
        auto* device = audio.getCurrentAudioDevice();
        if (device == nullptr) { statusBar.setProjectStatus("No audio device is open. Choose one in Audio settings.", {}); return; }
        releaseNotes();
        const auto start = controller.startSongTake(songTakeFolder(), device->getCurrentSampleRate(),
                                                    device->getActiveInputChannels().countNumberOfSetBits());
        if (!start.started) { statusBar.setProjectStatus(start.reason, start.reason); return; }
        const auto& rec = controller.document().recording;
        statusBar.setProjectStatus(juce::String("Recording ") + juce::String(static_cast<int>(controller.songTakeTracks().size()))
                                       + (controller.songTakeTracks().size() == 1 ? " track" : " tracks")
                                       + (rec.punch ? " between the loop markers" : "") + ". Press Record or Stop to finish.",
                                   "Takes are saved in " + songTakeFolder().getFullPathName());
    }
    void finishedSongTake(const alien::ProjectController::SongTakeOutcome& outcome) {
        juce::String message;
        if (outcome.refused) message = "Recording could not start. Open the Song view and try again.";
        else if (outcome.recorded == 0) message = "Nothing was recorded" + juce::String(outcome.ended == 0 || outcome.ended == alien::SongTakeCapture::stopped
                                                     ? "." : " (the playhead never reached the punch-in).");
        else if (outcome.abandoned) message = "The project changed while recording, so the takes were kept on disk only.";
        else {
            message = "Recorded " + juce::String(outcome.placed) + (outcome.placed == 1 ? " take" : " takes") + " onto the Song";
            if (outcome.ended == alien::SongTakeCapture::punchedOut) message += " (stopped at the punch-out)";
            else if (outcome.ended == alien::SongTakeCapture::loopWrapped) message += " (stopped where the loop wrapped)";
            else if (outcome.ended == alien::SongTakeCapture::jumped) message += " (stopped where the playhead jumped)";
            message += ".";
        }
        if (outcome.problems.isNotEmpty()) message += " " + outcome.problems.trim();
        juce::String detail = message;
        for (const auto& file : outcome.kept) detail += "\n" + file.getFullPathName();
        statusBar.setProjectStatus(message, detail);
        sessionView.rebuildGrid();
        sessionView.refreshFromDocument();
        refreshExtraViews();
    }

    void finishedRecording(const alien::ProjectController::RecordingOutcome& outcome) {
        const auto where = "Track " + juce::String(outcome.track + 1) + ", Scene " + juce::String(outcome.scene + 1);
        juce::String message;
        if (outcome.refused) message = "Recording could not start. Open the Piano Roll or Session view and try again.";
        else if (outcome.discarded) message = "The clip changed while recording, so the take was not kept.";
        else if (outcome.notesAdded <= 0) message = outcome.noteLimitReached ? where + " is full (1024 notes), so nothing was added."
                                                                           : juce::String("Nothing was recorded.");
        else {
            message = "Recorded " + juce::String(outcome.notesAdded) + (outcome.notesAdded == 1 ? " note" : " notes") + " on " + where
                + (outcome.replaced ? " (replaced the earlier notes)" : "");
            if (outcome.noteLimitReached) message += ". The clip is full, so later notes were left out";
        }
        if (outcome.droppedEvents > 0) message += ". Some played notes could not be captured";
        statusBar.setProjectStatus(message, message);
        sessionView.setImportStatus(message, {});
        sessionView.refreshFromDocument();
        loadSelectedClip();
    }

    void chooseRelinkFolder() {
        if (projectBusy || controller.isBusy()) return;
        if (controller.missingMedia().empty()) { sessionView.setImportStatus("No audio is missing.", {}); return; }
        const auto start = controller.currentFile().getParentDirectory();
        relinkChooser = std::make_unique<juce::FileChooser>("Choose the folder that contains the missing audio",
                                                           start.isDirectory() ? start : juce::File{});
        juce::Component::SafePointer<MainComponent> safe(this);
        relinkChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& chooser) {
                if (safe && chooser.getResult().isDirectory()) safe->controller.relinkMissingAudio(chooser.getResult());
            });
    }

    void clearSelectedClip() {
        if (rollTarget.rack) { rollTarget = {}; loadSelectedClip(); }   // "Clear clip" is about the Session clip
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

    // The engine plays what the view shows; the piano roll editing a rack channel plays the rack's pattern.
    int modeForTab(int tab) const {
        if (tab == 1 && rollTarget.rack && !songPlayback) return alien::rackMode;
        return alien::patternModeForTab(tab, engine.patternMode.load(), songPlayback);
    }
    // FL Studio's channel piano roll: the piano roll edits a rack channel's steps in the selected pattern as notes.
    void openRackChannel(int channel) {
        if (!controller.channelRack().validChannel(channel) || controller.isRecording()) return;
        rollTarget = {true, channel};
        rollFit = {};
        loadSelectedClip();
        showView(1);
        const int mode = modeForTab(1);
        if (engine.patternMode.exchange(mode) != mode) releaseNotes();
        pianoRoll.grabKeyboardFocus();
        statusBar.setProjectStatus("Editing " + juce::String(controller.channelRack().info[static_cast<std::size_t>(channel)].name)
                                       + " as notes. Each note is one step; pick a Session clip to edit clips again.", {});
    }

    void showView(int index) {
        if (workspace.isDeviceView()) workspace.showDevices(false);
        tabs.setCurrentTabIndex(index);
        syncViewButtons();
        if(auto* view=currentWorkspace();view&&view->isDetached())view->bringToFront();
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
        case 1:
            if (rollTarget.rack) {
                const auto& rack = controller.channelRack();
                const auto& name = rack.info[static_cast<std::size_t>(rollTarget.channel)].name;
                const auto& pattern = rack.patternNames[static_cast<std::size_t>(rack.selectedPattern)];
                workspace.setEditorTitle("Piano Roll", "Channel Rack" + dot + juce::String::fromUTF8(name.c_str(), static_cast<int>(name.size()))
                                         + dot + juce::String::fromUTF8(pattern.c_str(), static_cast<int>(pattern.size())));
            } else workspace.setEditorTitle("Piano Roll", d.tracks[static_cast<std::size_t>(d.selectedTrack)].name + dot
                                            + d.scenes[static_cast<std::size_t>(d.selectedScene)].name);
            break;
        case 2: workspace.setEditorTitle("Session", sessionView.subtitle()); break;
        case 3: workspace.setEditorTitle("Mixer", juce::String(d.trackCount()) + " channels"); break;
        case 4: workspace.setEditorTitle("Drum Sampler", "Eight sample pads per track"); break;
        case 5: workspace.setEditorTitle("Song", timelineView.subtitle()); break;
        case 6: workspace.setEditorTitle("Audio Lab", "Record a take or render a tempo match"); break;
        case 7: workspace.setEditorTitle("Plugin Rack", "VST3 instruments and ordered track / master FX"); break;
        case 8: workspace.setEditorTitle("Channel Rack", rackView.subtitle()); break;
        default: break;
        }
    }

    void updateOctave() { keyboardDock.setOctave(typingInput.typing.octave()); }

    // Undo / redo / file commands --------------------------------------------
    void undo() { if (projectBusy || controller.isBusy() || controller.isRecording()) return; snapshotPlugins(SnapshotReason::whenEdited); releaseNotes(); controller.undo(); }
    void redo() { if (projectBusy || controller.isBusy() || controller.isRecording()) return; snapshotPlugins(SnapshotReason::whenEdited); releaseNotes(); controller.redo(); }
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
        if(key.getKeyCode()==juce::KeyPress::F10Key&&!mods.isAnyModifierKeyDown()) {
            releaseNotes();applicationToolbar.openMenus();return true;
        }
        if (!mods.isCommandDown() || mods.isAltDown()) return false;
        if(key.getKeyCode()==','&&!mods.isShiftDown()) {
            applicationToolbar.menuItemSelected(alien::ApplicationToolbar::audioSettings,4);return true;
        }
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
        if (auto* device = audio.getCurrentAudioDevice(); device != nullptr && device->getCurrentSampleRate() > 0)
            topBar.setOutputLatencyMs(1000.0 * device->getOutputLatencyInSamples() / device->getCurrentSampleRate());
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
        const int tab = tabs.getCurrentTabIndex();
        const int newMode = modeForTab(tab);
        if (tab == 1 && rollTarget.rack) loadSelectedClip();   // the rack may have changed while another view showed
        if (engine.patternMode.exchange(newMode) != newMode) releaseNotes();
        else pianoRoll.releaseAudition();
        syncViewButtons();
    }

    void timerCallback() override {
        applicationToolbar.refreshState();
        controller.pollRecording();
        const int recordPhase = engine.recording.phase.load();
        // A take the engine has not picked up yet already shows as recording.
        topBar.setRecordState(controller.isRecording() && recordPhase == alien::RecordControls::idle
                                  ? static_cast<int>(alien::RecordControls::recording) : recordPhase);
        pianoRoll.setLocked(!rollTarget.rack && controller.isRecording() && controller.recordingTrack() == controller.document().selectedTrack
                            && controller.recordingScene() == controller.document().selectedScene);
        const bool isPlaying = engine.playing.load();
        topBar.setPlaying(isPlaying);
        const bool stepMode = engine.patternMode.load() == 0;
        const int currentStep = stepMode && isPlaying ? engine.currentStep.load() : -1;
        stepsView.setPlayhead(currentStep);
        stepsView.setPlaying(isPlaying && stepMode);
        const auto& d = controller.document();
        const bool rackMode = engine.patternMode.load() == 3;
        const int rackStep = rackMode && isPlaying ? engine.rackStep.load() : -1;
        rackView.setPlayhead(rackStep);
        int displayStep = rackMode ? rackStep : currentStep;
        if (!stepMode && !rackMode) {
            const int position = engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load();
            displayStep = position >= 0 ? position / 1000 : -1;
        }
        const bool songShown = engine.patternMode.load() == alien::songMode;
        if (songShown) {
            // The whole song: show where the playhead is on the song's own bars and beats.
            const auto position = engine.timelinePosition.load();
            displayStep = isPlaying && position >= 0 ? static_cast<int>(position / alien::timeline::ticksPerStep) : -1;
        }
        topBar.setPosition(displayStep, isPlaying || displayStep >= 0, songShown ? "Song" : rackMode ? "Rack" : stepMode ? "Steps" : "Session");
        devicesView.midi().tick();
        mixerPanel.tick();
        int controllerAction = -1;
        while (engine.controllerActions.pop(controllerAction)) sessionView.handleControllerAction(controllerAction);
        sessionView.tick();
        timelineView.setPlayhead(engine.patternMode.load() == 2 && isPlaying ? engine.timelinePosition.load() : -1);
        if (timelineView.isShowing()) timelineView.timelineCanvas().updateInputMeters();
        audioTools.setDestination(d.selectedTrack, d.selectedScene);
        if (activeCapture) {
            audioTools.setMeter(activeCapture->level.load());
            audioTools.setStatus("Recording " + juce::String(activeCapture->frames.load() / activeCapture->sampleRate, 1) + " s / " + juce::String(activeCapture->writtenFrames.load() / activeCapture->sampleRate, 1) + " s written to disk");
            if (activeCapture->done.load()) finishRecording();
        }
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        if (rollTarget.rack) pianoRoll.setPlayheadStep(rackMode && isPlaying ? engine.rackStep.load() : -1.0);
        else pianoRoll.setPlayheadStep(engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load() / 1000.0);
        pluginHost.poll();   // plugins that failed on the audio thread are reported here, never from it
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
        auto* selector=new juce::AudioDeviceSelectorComponent(audio,0,alien::maxTrackInputs,0,2,false,false,true,false);
        selector->setSize(560,420);
        juce::DialogWindow::LaunchOptions options;options.content.setOwned(selector);
        options.dialogTitle="Audio input & output";options.dialogBackgroundColour=alien::ui::colours::surface;
        options.useNativeTitleBar=true;options.resizable=false;audioSettingsWindow=options.launchAsync();
    }
    void chooseRecording() {
        if(activeCapture){finishRecording();return;}
        if(projectBusy || controller.isBusy() || audioWorkCancellation)return;
        const int track=controller.document().selectedTrack,scene=controller.document().selectedScene;
        takeFileChooser=std::make_unique<juce::FileChooser>("Save new recording",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getNonexistentChildFile("AlienTake",".wav"),"*.wav");
        juce::Component::SafePointer<MainComponent> safe(this);
        takeFileChooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,
            [safe,track,scene](const juce::FileChooser& chooser){if(safe && chooser.getResult()!=juce::File{})safe->beginRecording(chooser.getResult().withFileExtension("wav"),track,scene);});
    }
    void beginRecording(const juce::File& file,int track,int scene) {
        if(projectBusy || controller.isBusy() || activeCapture || !controller.document().isValidCell(track,scene))return;
        auto* device=audio.getCurrentAudioDevice();
        if(!device || device->getActiveInputChannels().countNumberOfSetBits()<audioTools.inputChannels()) {
            audioTools.setStatus("Enable the required input channels in Audio settings, then Record take.");return;
        }
        const double rate=device->getCurrentSampleRate();const int channels=audioTools.inputChannels();
        setProjectBusy(true);audioTools.setBusy(true);audioTools.setStatus("Preparing disk recording...");
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,file,track,scene,rate,channels] {
            juce::String error;auto take=alien::DiskRecording::start(file,rate,channels,error);
            juce::MessageManager::callAsync([safe,take,track,scene,rate,channels,error] {
                if(!safe)return;
                if(!take){safe->setProjectBusy(false);safe->audioTools.setBusy(false);safe->audioTools.setStatus(error);return;}
                // Preparation may outlive a device change. Never attach a take
                // at the wrong rate; an empty abandoned take is cleaned up.
                auto* device=safe->audio.getCurrentAudioDevice();
                if(!device || device->getCurrentSampleRate()!=rate || device->getActiveInputChannels().countNumberOfSetBits()<channels){safe->setProjectBusy(false);safe->audioTools.setBusy(false);safe->audioTools.setStatus("Audio device changed while preparing. Start a new take.");return;}
                safe->activeCapture=take;safe->takeTrack=track;safe->takeScene=scene;
                safe->audioTools.setBusy(true,true);safe->audioTools.setDestination(track,scene);
                safe->engine.capture.store(take.get(),std::memory_order_release);
                safe->audioTools.setStatus("Recording to disk. Input monitoring is off. Stop & save take when finished.");
            });return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }
    void finishRecording() {
        if(!activeCapture)return;
        { const juce::ScopedLock lock(audio.getAudioCallbackLock());engine.capture.store(nullptr); }
        const auto take=std::move(activeCapture);const int track=takeTrack,scene=takeScene;
        const auto available=controller.availableAudioBytes();
        audioTools.setBusy(true);audioTools.setMeter(0);audioTools.setStatus("Finalizing recorded WAV...");
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,take,track,scene,available] {
            auto result=std::make_shared<alien::RecordingResult>(take->finish());
            auto decoded=std::make_shared<alien::AudioClipImportResult>();
            const auto frames=result->writtenFrames;
            if(result->file.existsAsFile() && frames<=std::min<std::size_t>(available,alien::SessionAudioClip::maxDecodedBytes)/(take->channels*sizeof(float)))
                *decoded=alien::decodeSessionAudioClip(result->file);
            juce::MessageManager::callAsync([safe,result,decoded,track,scene]{if(safe)safe->recordingFinished(*result,*decoded,track,scene);});
            return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }
    void recordingFinished(const alien::RecordingResult& result,const alien::AudioClipImportResult& decoded,int track,int scene) {
        setProjectBusy(false);audioTools.setBusy(false);
        if(result.file.existsAsFile()) {
            const bool installed=decoded.clip && controller.installAudio(result.file,track,scene,decoded.clip);
            juce::String message="Saved take: "+result.file.getFullPathName();
            if(!installed)message+=". Kept on disk; this take could not be loaded into the RAM-based Session player.";
            if(result.error.isNotEmpty())message+=" "+result.error;
            audioTools.setStatus(message);
            if(closeAfterTake && !result.interrupted && result.error.isEmpty()){closeAfterTake=false;juce::JUCEApplication::getInstance()->systemRequestedQuit();}
            else closeAfterTake=false;
        } else {
            closeAfterTake=false;
            audioTools.setStatus(result.error+" Unfinished file: "+result.partialFile.getFullPathName()+". Use Recover takes after resolving disk problems.");
        }
    }
    void chooseTakeRecovery() {
        if(projectBusy || controller.isBusy())return;
        const auto files=alien::RecordingFile::unfinished();
        if(files.isEmpty()){audioTools.setStatus("No unfinished recording takes were found.");return;}
        juce::PopupMenu menu;
        for(int i=0;i<files.size();++i)menu.addItem(i+1,alien::RecordingFile::recoveryName(files[i]));
        juce::Component::SafePointer<MainComponent> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&audioTools),[safe,files](int selected){
            if(safe && selected>0 && selected<=files.size())safe->recoverTake(files[selected-1]);
        });
    }
    void recoverTake(const juce::File& journal) {
        if(projectBusy || controller.isBusy())return;
        const int track=controller.document().selectedTrack,scene=controller.document().selectedScene;
        const auto available=std::min<std::size_t>(controller.availableAudioBytes(),alien::SessionAudioClip::maxDecodedBytes);
        setProjectBusy(true);audioTools.setBusy(true);audioTools.setStatus("Recovering unfinished recording...");
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,journal,track,scene,available] {
            auto result=std::make_shared<alien::RecordingResult>(alien::RecordingFile::recover(journal));
            auto decoded=std::make_shared<alien::AudioClipImportResult>();
            if(result->file.existsAsFile() && static_cast<std::uint64_t>(result->file.getSize())<=available/4*3+alien::RecordingFile::headerSize)
                *decoded=alien::decodeSessionAudioClip(result->file);
            juce::MessageManager::callAsync([safe,result,decoded,track,scene]{if(safe)safe->recordingFinished(*result,*decoded,track,scene);});
            return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }
    void chooseTempoWarp() {
        if(audioWorkCancellation){audioWorkCancellation->store(true);audioTools.setStatus("Cancelling tempo render…");return;}
        if(projectBusy || controller.isBusy())return;
        const int track=controller.document().selectedTrack,scene=controller.document().selectedScene;
        const auto& clip=controller.document().clip(track,scene);
        const double ratio=audioTools.sourceBpm()/engine.bpm.load();
        if(!std::isfinite(ratio) || ratio<0.5 || ratio>2){audioTools.setStatus("Use a duration ratio between 0.5 and 2.");return;}
        warpSourceChooser=std::make_unique<juce::FileChooser>("Choose source audio (including long recordings)",
            juce::File(clip.audioFilePath),"*.wav;*.mp3");
        juce::Component::SafePointer<MainComponent> safe(this);
        warpSourceChooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,
            [safe,track,scene,ratio](const juce::FileChooser& chooser){
                if(!safe || chooser.getResult()==juce::File{})return;
                const auto source=chooser.getResult();
                safe->warpFileChooser=std::make_unique<juce::FileChooser>("Save tempo-matched audio as a new WAV",
                    source.getSiblingFile(source.getFileNameWithoutExtension()+"-warped.wav"),"*.wav");
                safe->warpFileChooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,
                    [safe,source,track,scene,ratio](const juce::FileChooser& output){if(safe && output.getResult()!=juce::File{})safe->startTempoWarp(output.getResult().withFileExtension("wav"),source,track,scene,ratio);});
            });
    }
    void startTempoWarp(const juce::File& file,const juce::File& source,int track,int scene,double ratio) {
        if(projectBusy || controller.isBusy())return;
        if(file.exists()){audioTools.setStatus("Choose a new filename to preserve existing audio.");return;}
        const auto available=std::min<std::size_t>(controller.availableAudioBytes(),alien::SessionAudioClip::maxDecodedBytes);
        setProjectBusy(true);audioTools.setBusy(true,false,true);audioTools.setStatus("Streaming pitch-preserving tempo match to disk…");
        audioWorkCancellation=std::make_shared<std::atomic<bool>>(false);auto cancel=audioWorkCancellation;
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,file,source,track,scene,ratio,cancel,available]{
            auto result=std::make_shared<alien::RecordingResult>();
            auto decoded=std::make_shared<alien::AudioClipImportResult>();
            try {
                *result=alien::warpTempoFile(source,file,ratio,*cancel);
                if(result->file.existsAsFile() && static_cast<std::uint64_t>(result->file.getSize())<=available/4*3+alien::RecordingFile::headerSize)
                    *decoded=alien::decodeSessionAudioClip(result->file);
            } catch(const std::exception& e){result->error="Tempo render failed: "+juce::String(e.what());}
            juce::MessageManager::callAsync([safe,track,scene,result,decoded]{if(!safe)return;
                safe->audioWorkCancellation.reset();safe->setProjectBusy(false);safe->audioTools.setBusy(false);
                if(result->file.existsAsFile()) {
                    const bool installed=decoded->clip && safe->controller.installAudio(result->file,track,scene,decoded->clip);
                    safe->audioTools.setStatus("Tempo match saved: "+result->file.getFullPathName()+". Source preserved."+
                        (installed?juce::String{}:juce::String(" Kept on disk; could not load into the RAM-based Session player.")));
                } else safe->audioTools.setStatus(result->error);
            });return juce::ThreadPoolJob::jobHasFinished;
        };audioWorkPool.addJob(std::move(job));
    }

    void setProjectBusy(bool busy) {
        projectBusy = busy;
        controller.setExternalBusy(busy);
        topBar.setBusy(busy); sessionView.setBusy(busy); drumPanel.setBusy(busy);
        mixerPanel.setEnabled(!busy);   // the Arrangement stays usable (playing, scrolling); the controller refuses edits while busy
    }
    void finishAudioImport(const juce::File& file, int track, int scene, const alien::AudioClipImportResult& result) {
        if (!controller.installAudio(file, track, scene, result.clip)) audioTools.setStatus("Audio could not be installed: session memory limit reached.");
    }
    void refreshExtraViews() {
        const auto& d = controller.document();
        audioTools.setDestination(d.selectedTrack, d.selectedScene);
        juce::StringArray names;
        std::array<std::array<juce::String, alien::drumPadCount>, alien::maxSessionTracks> paths{};
        for (int t = 0; t < d.trackCount(); ++t) { names.add(d.tracks[t].name); paths[t] = d.tracks[t].drumPaths; }
        drumPanel.setTrackNames(names); drumPanel.setPaths(paths);
        refreshPlugins();
        timelineView.refresh();
        // Where the song will start playing is the view's play position; tell the engine unless it is playing (a jump is not wanted here).
        if (!engine.playing.load()) engine.timelineSeek.store(timelineView.timelineCanvas().cursorTick());
    }
    // `from` and `to` are timeline ticks; to == -1 renders through the end of the song. While a render runs, asking again cancels it.
    void chooseWavExport(std::int64_t from = 0, std::int64_t to = -1) {
        if (exportCancellation) { exportCancellation->store(true); return; }
        if (projectBusy || controller.isBusy()) return;
        exportFileChooser = std::make_unique<juce::FileChooser>("Export arrangement WAV", juce::File{}, "*.wav");
        juce::Component::SafePointer<MainComponent> safe(this);
        exportFileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe, from, to](const juce::FileChooser& chooser) { if (safe && chooser.getResult() != juce::File{}) safe->exportWav(chooser.getResult().withFileExtension("wav"), from, to); });
    }
    void exportWav(juce::File file, std::int64_t from = 0, std::int64_t to = -1) {
        if (projectBusy || controller.isBusy()) return;
        snapshotPlugins();
        auto song = controller.captureArrangement();
        alien::WavExportOptions options;
        options.fromTick = from; options.toTick = to;
        auto keepAlive = std::make_shared<std::vector<std::shared_ptr<const alien::SessionAudioClip>>>();
        options.rackClips = controller.rackClipsForExport(*keepAlive);
        auto project = std::make_shared<alien::SessionProjectData>(alien::toProjectData(controller.document()));
        auto renderEngine = std::make_shared<ToneEngine>();
        renderEngine->prepare(48000);
        // Kept until the export ends: a plugin that fails during the render is reported through it.
        auto renderHost = std::make_shared<alien::PluginHost>(*renderEngine->plugins, hostLogFile());
        { const auto errors=renderHost->restore(pluginStates(),controller.document().masterPlugins,48000);
          if(errors.isNotEmpty()){ timelineView.setStatus("Export stopped: " + errors);return; } }
        auto assets = controller.document();
        alien::DrumSamplePointers drums{};
        for (int t = 0; t < assets.trackCount(); ++t) for (int p = 0; p < alien::drumPadCount; ++p) drums[t][p] = assets.tracks[t].drumAssets[p].get();
        exportCancellation = std::make_shared<std::atomic<bool>>(false); auto cancel = exportCancellation;
        setProjectBusy(true); timelineView.setExporting(true); timelineView.setStatus("Exporting WAV. Click Cancel export to stop.");
        juce::Component::SafePointer<MainComponent> safe(this);
        // The engine and host are handed to the message thread when the render ends, so the plugins are destroyed there (plugins expect that).
        std::function<juce::ThreadPoolJob::JobStatus()> job = [safe, file, song, project, assets, drums, cancel, renderEngine, renderHost, options, keepAlive]() mutable {
            juce::String error; const bool ok = alien::exportArrangementWav(file, *project, *song, drums, *cancel, error, renderEngine.get(), options);
            juce::MessageManager::callAsync([safe, file, ok, error, engine = std::move(renderEngine), host = std::move(renderHost)] {
                // Reports from plugins that failed while rendering are only collected here, on the message thread.
                juce::String pluginFailures; host->onProblem = [&pluginFailures](const juce::String& text) { pluginFailures = text; };
                host->poll();
                if (!safe) return;
                safe->exportCancellation.reset(); safe->setProjectBusy(false); safe->timelineView.setExporting(false);
                safe->timelineView.setStatus(!ok ? error
                    : pluginFailures.isEmpty() ? "Exported " + file.getFullPathName()
                    : "Exported " + file.getFullPathName() + " WITH PLUGIN FAILURES. The file is missing the sound of the plugin(s) that failed, so do not use it as a final mix:\n" + pluginFailures);
            });
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
    // Why plugin state is being copied into the document. Reading it means
    // detaching the audio callback, which stops every Session clip and restarts
    // an Arrangement, so it is only done when it can matter.
    enum class SnapshotReason {
        save,       // Save / export: capture whenever stopped; while playing only if something was edited
        whenEdited, // Undo, redo, structure edits: capture only if a plugin was edited
        autosave,   // Unattended: never interrupt playback; the next quiet minute catches up
        always      // Immediately before rebuilding racks from the document, which detaches audio anyway
    };
    bool pluginsAssigned() const {
        const auto& d = controller.document();
        bool assigned = !d.masterPlugins.effects.empty();
        for (const auto& t : d.tracks) assigned = assigned || !t.plugins.instrument.descriptionXml.isEmpty() || !t.plugins.effects.empty();
        return assigned;
    }
    void snapshotPlugins(SnapshotReason reason = SnapshotReason::save) {
        if (!pluginsAssigned()) { pluginHost.changed.store(false); return; }
        // Native parameters may have changed since the most recent timer tick.
        if (reason != SnapshotReason::always) {
            const bool edited = pluginHost.changed.load() || controller.pluginStateDirty();
            if (!edited && (reason != SnapshotReason::save || engine.playing.load())) return;
            if (reason == SnapshotReason::autosave && engine.playing.load()) return;
        }
        pluginHost.changed.store(false);
        auto tracks=pluginStates(); auto master=controller.document().masterPlugins;
        audio.removeAudioCallback(&engine);
        pluginHost.snapshot(tracks,master);
        audio.addAudioCallback(&engine);
        controller.setPluginRacks(tracks,master);
        loadedRacks = tracks; loadedMaster = master;
    }
    // Racks differ only if a plugin is present on one side and not the other, or
    // its saved state or bypass flag differs. Tracks past the end count as empty.
    static bool sameRacks(const std::vector<alien::PluginRackState>& a, const alien::PluginRackState& masterA,
                          const std::vector<alien::PluginRackState>& b, const alien::PluginRackState& masterB) {
        if (masterA != masterB) return false;
        const alien::PluginRackState empty;
        for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
            if ((i < a.size() ? a[i] : empty) != (i < b.size() ? b[i] : empty)) return false;
        return true;
    }
    // Rebuilds the plugin instances from the document. Restoring detaches the
    // audio callback, so it is skipped when the loaded racks already match, for
    // example after renaming a track or scene or editing notes. Opening or
    // creating a document passes force=true so plugin state always matches the file.
    void restorePlugins(bool force = false) {
        const auto states = pluginStates(); const auto master = controller.document().masterPlugins;
        if (!force && sameRacks(states, master, loadedRacks, loadedMaster)) { refreshPlugins(); return; }
        audio.removeAudioCallback(&engine);
        auto errors=pluginHost.restore(states,master,deviceRate());
        audio.addAudioCallback(&engine);
        loadedRacks = states; loadedMaster = master;
        pluginPanel.setStatus(errors.isEmpty()?"Plugin racks restored. Select a slot to open its editor.":"Missing or unsupported plugins (saved state preserved):\n"+errors);
        refreshPlugins();
    }
    void addPlugin(bool instrument,int choice) {
        if(projectBusy || controller.isBusy()) return;
        juce::PluginDescription chosen;
        if(choice>=alien::PluginPanel::stockBase) {
            const int stockIndex=choice-alien::PluginPanel::stockBase; if(stockIndex<0 || stockIndex>=static_cast<int>(alien::stock::catalog.size()))return;
            chosen=alien::stock::describe(alien::stock::catalog[static_cast<std::size_t>(stockIndex)]);
        } else {
            auto types=knownPlugins.getTypes(); const int index=choice-1; if(index<0 || index>=types.size())return;
            chosen=types[index];
        }
        if(chosen.isInstrument!=instrument) { pluginPanel.setStatus("Select an instrument for the instrument slot, or an FX plugin for Add FX.");return; }
        const int t=pluginTarget(); if(instrument && t==alien::maxSessionTracks)return;
        snapshotPlugins(SnapshotReason::always); auto states=pluginStates();auto master=controller.document().masterPlugins;
        auto& rack=t==alien::maxSessionTracks?master:states[t];
        alien::PluginState next{chosen.createXml()->toString(),{},false};
        if(instrument)rack.instrument=next;
        else { if(rack.effects.size()>=8){pluginPanel.setStatus("Each rack supports eight FX slots.");return;}rack.effects.push_back(next); }
        controller.setPluginRacks(states,master); restorePlugins();
    }
    void pluginAction(int op,int slot) {
        if(projectBusy || controller.isBusy())return;
        const int t=pluginTarget();
        if(op==0){const auto problem=pluginHost.openEditor(t,slot);if(problem.isNotEmpty()){pluginPanel.setStatus(problem);refreshPlugins();}return;}
        snapshotPlugins(SnapshotReason::always);auto states=pluginStates();auto master=controller.document().masterPlugins;
        auto& rack=t==alien::maxSessionTracks?master:states[t];
        if(slot<0){if(t==alien::maxSessionTracks)return;if(op==1)rack.instrument.bypass=!rack.instrument.bypass;else if(op==2)rack.instrument={};}
        else if(slot<static_cast<int>(rack.effects.size())){
            if(op==1)rack.effects[slot].bypass=!rack.effects[slot].bypass;
            else if(op==2)rack.effects.erase(rack.effects.begin()+slot);
            else { const int other=slot+(op==3?-1:1);if(other>=0 && other<static_cast<int>(rack.effects.size()))std::swap(rack.effects[slot],rack.effects[other]); }
        }
        controller.setPluginRacks(states,master);restorePlugins();
    }
    // ---- plugin scanning -------------------------------------------------------------------------------
    static juce::File hostLogFile() { return alien::DiagnosticLog::defaultFile("plugin-host.log"); }
    static juce::File scanLogFile() { return alien::DiagnosticLog::defaultFile("plugin-scan.log"); }
    static juce::File lastScanFile() { return alien::DiagnosticLog::defaultFile("plugin-scan-last.txt"); }

    void handleScanCommand(alien::ScanCommand command) {
        using C = alien::ScanCommand;
        switch (command) {
            case C::standardFolders: { alien::scan::ScanOptions o; o.folders = alien::scan::standardVst3Folders(); startScan(std::move(o)); break; }
            case C::folder: chooseScanFolder(); break;
            case C::singleFile: choosePluginScan(); break;
            case C::retryFailed: {
                alien::scan::ScanOptions o; o.retryFailed = true;
                for (const auto& failed : scanDatabase.failures()) if (juce::File(failed.path).exists()) o.bundles.push_back(juce::File(failed.path));
                if (o.bundles.empty()) { pluginPanel.setStatus("No plugin has failed to scan, so there is nothing to retry."); break; }
                startScan(std::move(o)); break; }
            case C::rescanAll: {
                alien::scan::ScanOptions o; o.rescanUnchanged = true; o.retryFailed = true; o.folders = alien::scan::standardVst3Folders();
                for (const auto& entry : scanDatabase.all()) if (juce::File(entry.path).exists()) o.bundles.push_back(juce::File(entry.path));
                startScan(std::move(o)); break; }
            case C::recheckBlocked: recheckBlockedPlugins(); break;
            case C::togglePreflight:
                if (preflight) {
                    preflight->setEnabled(pluginPanel.preflightIsEnabled());
                    pluginPanel.setStatus(preflight->enabled() ? "Plugins will be tried out in a separate process the first time they are loaded."
                                                               : "Plugins will be loaded without a safety check. A plugin that crashes while loading will end the session.");
                }
                break;
            case C::openScanLog: openDiagnosticFile(lastScanFile(), "No scan has been run yet."); break;
            case C::openProblemLog: openDiagnosticFile(hostLogFile(), "No plugin problems have been logged."); break;
        }
    }
    void openDiagnosticFile(const juce::File& file, const juce::String& missing) {
        if (!file.existsAsFile()) { pluginPanel.setStatus(missing); return; }
        if (!file.startAsProcess()) file.revealToUser();
    }
    void recheckBlockedPlugins() {
        if (projectBusy || controller.isBusy() || !preflight) return;
        const int n = preflight->forgetBlocked();
        if (n == 0) { pluginPanel.setStatus("No plugin is blocked."); return; }
        pluginPanel.setStatus("Checking " + juce::String(n) + " blocked plugin(s) again...");
        snapshotPlugins(SnapshotReason::always);
        restorePlugins(true);
    }
    void chooseScanFolder() {
        if(projectBusy || controller.isBusy() || scanning)return;
        pluginChooser=std::make_unique<juce::FileChooser>("Choose a folder to search for VST3 plugins (subfolders are included)",juce::File{});
        juce::Component::SafePointer<MainComponent> safe(this);
        pluginChooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& chooser){
                if(!safe||chooser.getResult()==juce::File{})return;
                alien::scan::ScanOptions o; o.folders.push_back(chooser.getResult()); safe->startScan(std::move(o));
            });
    }
    void choosePluginScan() {
        if(projectBusy || controller.isBusy() || scanning)return;
        pluginChooser=std::make_unique<juce::FileChooser>("Select a 64-bit VST3 plugin",juce::File{},"*.vst3");
        juce::Component::SafePointer<MainComponent> safe(this);
        pluginChooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& chooser){
                if(!safe||chooser.getResult()==juce::File{})return;
                const auto bundle=chooser.getResult();
                if(!bundle.hasFileExtension("vst3")){safe->pluginPanel.setStatus("Choose a .vst3 plugin, or use Scan a folder to search a folder.");return;}
                // Asked for by name, so it is tried even if it failed before and has not changed.
                alien::scan::ScanOptions o; o.bundles.push_back(bundle); o.retryFailed=true; o.rescanUnchanged=true; safe->startScan(std::move(o));
            });
    }
    // A scan runs on its own thread so the project stays usable. It works on copies of the plugin list and the
    // scan database, saves both after every file (so a crash of the whole application loses nothing), and
    // hands the result back here when it ends.
    struct ScanSession {
        alien::scan::ScanDatabase database;
        juce::KnownPluginList known;
        std::atomic<bool> refreshQueued{false};
    };
    void startScan(alien::scan::ScanOptions options) {
        if (scanning) return;
        options.executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        if (auto* app = juce::JUCEApplication::getInstance()) options.appVersion = app->getApplicationVersion();
        scanning = true; pluginPanel.setScanning(true);
        pluginPanel.setStatus("Scanning for VST3 plugins. Each file is loaded in a separate process, so a plugin that crashes or hangs is recorded and skipped.");
        scanCancel = std::make_shared<std::atomic<bool>>(false);
        auto session = std::make_shared<ScanSession>();
        session->database = scanDatabase;
        if (auto xml = knownPlugins.createXml()) session->known.recreateFromXml(*xml);
        auto cancel = scanCancel;
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job = [safe, session, cancel, options] {
            alien::DiagnosticLog log(scanLogFile(), 4 * 1024 * 1024);
            alien::scan::ScanHooks hooks;
            hooks.progress = [safe](int done, int total, const juce::String& current) {
                if (current.isEmpty()) return;
                const auto text = "Scanning " + juce::String(done + 1) + " of " + juce::String(total) + ": " + current + "\nCancel scan stops after the current file.";
                juce::MessageManager::callAsync([safe, text] { if (safe) safe->pluginPanel.setStatus(text); });
            };
            hooks.checkpoint = [safe, session] {
                session->database.save(alien::scan::ScanDatabase::defaultFile());
                if (auto xml = session->known.createXml()) { pluginListFile().getParentDirectory().createDirectory(); xml->writeTo(pluginListFile()); }
                if (!session->refreshQueued.exchange(true))
                    juce::MessageManager::callAsync([safe, session] {
                        session->refreshQueued = false;
                        if (!safe) return;
                        if (auto xml = session->known.createXml()) safe->knownPlugins.recreateFromXml(*xml);
                        safe->refreshPlugins();
                    });
            };
            auto report = std::make_shared<alien::scan::ScanReport>(alien::scan::runScan(options, session->database, session->known, &alien::scan::runChildProcess, log, *cancel, hooks));
            lastScanFile().getParentDirectory().createDirectory();
            lastScanFile().replaceWithText(report->fullText());
            juce::MessageManager::callAsync([safe, session, report] { if (safe) safe->finishScan(*session, *report); });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        scanPool.addJob(std::move(job));
    }
    void finishScan(ScanSession& session, const alien::scan::ScanReport& report) {
        scanning = false; scanCancel.reset(); pluginPanel.setScanning(false);
        scanDatabase = session.database;
        if (auto xml = session.known.createXml()) knownPlugins.recreateFromXml(*xml);
        // Kept for continuity with earlier versions: a plain list of files that failed to scan.
        auto failures = pluginListFile().getSiblingFile("PluginScanFailures.txt");
        for (const auto& b : report.bundles)
            if (b.action == alien::scan::BundleReport::Action::scanned && b.status != alien::scan::Status::ok && b.status != alien::scan::Status::couldNotStart) {
                failures.getParentDirectory().createDirectory(); failures.appendText(b.bundle.getFullPathName() + "\n");
            }
        refreshPlugins();
        auto text = report.summary();
        if (report.failedCount() > 0) text += "\nScan VST3 > Show the last scan log lists each file that failed and why. Retry plugins that failed tries them again.";
        pluginPanel.setStatus(text);
    }

    // Members (order matters: construction top to bottom) -------------------
    alien::ui::StudioLookAndFeel studioLookAndFeel;
    juce::TooltipWindow tooltipWindow{this, 650};
    ToneEngine engine;
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    alien::ProjectController controller{engine};
    alien::PluginHost pluginHost{*engine.plugins, alien::DiagnosticLog::defaultFile("plugin-host.log")};
    alien::PluginPanel pluginPanel;
    juce::KnownPluginList knownPlugins;
    alien::scan::ScanDatabase scanDatabase;       // what has been scanned, and what happened to each file
    std::unique_ptr<alien::Preflight> preflight;  // the safety check before a plugin is loaded into a project
    bool scanning{};
    std::shared_ptr<std::atomic<bool>> scanCancel;
    std::vector<alien::PluginRackState> loadedRacks; // Rack states the plugin host last loaded or captured.
    alien::PluginRackState loadedMaster;
    std::unique_ptr<juce::FileChooser> pluginChooser, relinkChooser;
    alien::TopBar topBar;
    alien::ApplicationToolbar applicationToolbar;
    alien::StatusBar statusBar;
    alien::KeyboardDock keyboardDock{keyboardState};
    alien::StepSequencerView stepsView{controller};
    alien::PianoRollEditor pianoRoll;
    alien::SessionView sessionView{controller, engine};
    alien::TrackMixerPanel mixerPanel{controller, engine.tracks, engine.masterControls};
    alien::DevicesView devicesView{engine.midi};
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    alien::CompactWorkspace workspace{tabs, devicesView};
    TypingPianoRouter typingInput{*this, keyboardState};
    juce::Component::SafePointer<juce::Component> keySource;
    bool loadingClip{}, suppressPianoRollCommit{}, wasDirty{};
    // What the piano roll edits: the selected Session clip, or a Channel Rack channel in the rack's selected pattern.
    struct RollTarget { bool rack{}; int channel{}; };
    RollTarget rollTarget;
    int rollGesture{};                  // one undo step per piano-roll gesture on a rack channel
    alien::rack::ChannelNotesFit rollFit;
    int statusTicks{};
    alien::DrumSamplerPanel drumPanel;
    alien::TimelineView timelineView{controller};
    alien::AudioToolsPanel audioTools;
    alien::ChannelRackView rackView{controller};
    bool projectBusy{}, closeAfterTake{};
    bool songPlayback{};   // "Song mode" (see setSongMode)
    bool keyboardVisible{}; // starts hidden; the toolbar piano button reveals it in any view
    std::shared_ptr<alien::DiskRecording> activeCapture;
    int takeTrack{}, takeScene{};
    std::unique_ptr<juce::FileChooser> takeFileChooser, warpSourceChooser, warpFileChooser, exportFileChooser;
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow;
    std::shared_ptr<std::atomic<bool>> audioWorkCancellation, exportCancellation;
    juce::ThreadPool audioWorkPool{1};
    juce::ThreadPool scanPool{1};   // plugin scans can take minutes; they must not queue behind audio-tool jobs
    bool dockingAll{};
    // Destroy windows/wrappers before the original editors, typing router and controller.
    std::array<std::unique_ptr<alien::DetachableView>,alien::ApplicationToolbar::viewCount> detachableViews;
};
