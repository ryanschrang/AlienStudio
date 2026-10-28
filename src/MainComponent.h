#pragma once
#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "PianoRollGhostSources.h"
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
        drumPanel.onRoute = [this](int t) { releaseNotes(); engine.liveDrumTrack.store(t); refreshPlugins(); };
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
        pluginPanel.onLiveInput = [this](int track) { releaseNotes(); engine.liveDrumTrack.store(track); drumPanel.setLiveInput(track); };
        pluginPanel.onTarget = [this](int) { refreshPlugins(); };
        pluginPanel.onAdd = [this](bool instrument, int choice) { addPlugin(instrument,choice); };
        pluginPanel.onAction = [this](int op,int slot) { pluginAction(op,slot); };
        pluginPanel.hasSidechainInput = [this](int rack,int slot) { return pluginHost.hasSidechainInput(rack,slot); };
        pluginPanel.onSidechain = [this](int slot,alien::KeySource from,int source) { setSidechain(slot,from,source); };
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
        timelineView.onExportStems = [this](std::int64_t from, std::int64_t to) { chooseStemExport(from, to); };
        timelineView.onFreezeTrack=[this](int track){startTrackFreeze(track);};
        timelineView.onUnfreezeTrack=[this](int track){if(controller.unfreezeTrack(track)){refreshExtraViews();timelineView.setStatus("Track unfrozen; original clips and saved settings restored.");}};
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
        canvas.onOpenPlacement = [this](std::uint64_t clip) { openPlacementClip(clip); };
        // A double-click on an empty lane made a new clip (and a Session scene for it when every cell of the track was taken).
        canvas.onClipCreated = [this](int, int, bool sceneAdded) {
            if (sceneAdded) sessionView.rebuildGrid();
            sessionView.refreshFromDocument();
            loadSelectedClip();
        };
    }

    // Double-click on a clip: edit what it plays (the piano roll or Session for a clip, the Channel Rack for a pattern).
    void openTimelineSource(int row, int source) {
        if (!alien::timeline::isPatternRow(row) && controller.document().isValidCell(row, source))
            if (const auto id = controller.document().clip(row, source).rackClip; id != 0) {
                controller.selectRackPattern(controller.rackPatternForClip(id)); rackView.refresh();
                showView(alien::rackTabIndex); return;
            }
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

    // Double-click on a placement clip: edit its notes in the piano roll. Every region that plays it follows.
    void openPlacementClip(std::uint64_t clip) {
        const int track = controller.placementTrack(clip);
        if (track < 0) return;
        controller.selectClip(track, controller.document().selectedScene);
        rollTarget = {false, 0, clip};
        loadSelectedClip();
        sessionView.refreshFromDocument();
        showView(1);
        pianoRoll.grabKeyboardFocus();
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
            if (multiClip.active()) loadSelectedClip();   // a clip being edited changed underneath: merge it again
            else refreshGhostNotes();
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
            if (rollTarget.placement != 0) {
                if (!loadingClip) controller.setPlacementPattern(rollTarget.placement, pianoRoll.getPattern());
                return;
            }
            if (multiClip.active()) {
                if (!loadingClip) writeMultiClipEdit();
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
            if (rollTarget.placement != 0) { controller.commit("Edit notes"); timelineView.refresh(); return; }
            const auto& d = controller.document();
            controller.commit("Edit notes", d.selectedTrack, d.selectedScene);
        };
        pianoRoll.onGhostSourceChanged = [this](alien::GhostNoteSource) {
            if (pianoRoll.isMultiClipEditing() || multiClip.active()) loadSelectedClip();   // a different set of clips to edit
            else refreshGhostNotes();
        };
        pianoRoll.onMultiClipChanged = [this](bool) { loadSelectedClip(); };
        pianoRoll.noteTint = [this](std::uint32_t id) -> std::optional<juce::Colour> {
            if (!multiClip.active()) return std::nullopt;
            const int owner = multiClip.ownerOf(id);
            if (owner <= 0) return std::nullopt;
            const auto& source = multiClip.sources()[static_cast<std::size_t>(owner)];
            const auto& tracks = controller.document().tracks;
            // Other tracks' clips in their track's colour; this track's other scenes in a colour per scene.
            const int colour = source.track != controller.document().selectedTrack && source.track < static_cast<int>(tracks.size())
                ? tracks[static_cast<std::size_t>(source.track)].colour : source.scene + 1;
            return alien::ui::trackColour(colour).withSaturation(0.55f).brighter(0.15f);
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
        // Launching a clip plays it. With Song mode on, Play plays the Song instead, so say so rather than stay silent.
        sessionView.onLaunched = [this](bool) {
            if (songPlayback && !engine.playing.load())
                statusBar.setProjectStatus("Song mode is on, so Play plays the Song, not Session clips.",
                                           "Turn Song mode off in the top bar to hear Session clips as you launch them.");
        };
        sessionView.onEditRackClip = [this](std::uint64_t clip) {
            const int pattern = controller.rackPatternForClip(clip);
            if (pattern < 0) return;
            controller.selectRackPattern(pattern);
            rackView.refresh();
            showView(alien::rackTabIndex);
        };
        sessionView.onEditClip = [this](int track, int scene) {
            if (const auto id = controller.document().clip(track, scene).rackClip; id != 0) {
                controller.selectRackPattern(controller.rackPatternForClip(id)); rackView.refresh();
                showView(alien::rackTabIndex); return;
            }
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
        sessionView.onCellContentChanged = [this] {
            if (!rollTarget.rack && rollTarget.placement == 0) loadSelectedClip();
            sessionView.refreshFromDocument();
            refreshExtraViews();
        };
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
        mixerPanel.onReturnEdited = [this](int r, bool committed) {
            controller.returnSettingsEdited(r);
            if (committed) controller.commit("Change " + controller.document().returns[static_cast<std::size_t>(r)].name + " mix", -1, -1, "return-" + juce::String(r));
        };
        mixerPanel.onBusEdited = [this](int b, bool committed) {
            controller.busSettingsEdited(b);
            if (committed) controller.commit("Change " + controller.document().buses[static_cast<std::size_t>(b)].name + " mix", -1, -1, "bus-" + juce::String(b));
        };
        // A bus was added, renamed or deleted, or a track's output changed: one undo step, and the
        // plugin racks follow (a deleted bus takes its FX with it).
        mixerPanel.beforeBusDeleted = [this] { snapshotPlugins(SnapshotReason::whenEdited); };
        mixerPanel.beforeReturnDeleted = [this] { snapshotPlugins(SnapshotReason::whenEdited); };
        mixerPanel.onRoutingChanged = [this](const juce::String& label) {
            controller.commit(label);
            restorePlugins();
            timelineView.refresh();   // the automation editor offers only the sends that exist
        };

        mixerPanel.onTrackEdited = [this](int track, bool committed) {
            controller.trackSettingsEdited(track);
            sessionView.repaint();
            if (committed)
                controller.commit("Change " + controller.document().tracks[static_cast<std::size_t>(track)].name + " mix",
                                  -1, -1, "mixer-" + juce::String(track));
        };
        // Session knob gestures share the Mixer's commit path. Mute/solo commands already own their undo step.
        sessionView.onTrackMixEdited = [this](int track, bool committed) {
            mixerPanel.refreshSettings();
            if (mixerPanel.onTrackEdited) mixerPanel.onTrackEdited(track, committed);
        };
        sessionView.onTrackMixCommandExecuted = [this](int) {
            mixerPanel.refreshSettings();
            sessionView.repaint();
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

    void loadSelectedClip(bool allowMultiClip = true) {
        if (rollTarget.rack && !controller.channelRack().validChannel(rollTarget.channel)) rollTarget = {};
        if (rollTarget.placement != 0 && controller.placementTrack(rollTarget.placement) < 0) rollTarget = {};   // no region plays it any more
        if (rollTarget.rack || rollTarget.placement != 0) multiClip.end();   // only Session clips are edited together
        loadingClip = true;
        if (rollTarget.placement != 0) {
            pianoRoll.setPattern(controller.document().contentOf(alien::library::ClipId{rollTarget.placement}).notes());
        } else if (rollTarget.rack) {
            const auto& rack = controller.channelRack();
            pianoRoll.setPattern(alien::rack::channelNotes(rack.pattern(rack.selectedPattern), rollTarget.channel));
        } else if (!loadMultiClip(allowMultiClip)) pianoRoll.setPattern(controller.document().selectedClip().notes());
        pianoRoll.setEnabled(rollTarget.rack || ((rollTarget.placement != 0 || controller.document().selectedClip().rackClip == 0) && !controller.isTrackFrozen(rollTarget.placement != 0 ? controller.placementTrack(rollTarget.placement)
                                                                                                 : controller.document().selectedTrack)));
        refreshGhostNotes();
        loadingClip = false;
        sessionView.refreshSelectedClip();
        updateEditorTitle();
    }

    // Other clips' notes, shown dimmed behind a Session clip (none for a Channel Rack channel or a Song clip).
    // Clips being edited with it are not ghosts.
    void refreshGhostNotes() {
        const auto& d = controller.document();
        auto ghosts = rollTarget.rack || rollTarget.placement != 0 ? std::vector<alien::PianoRollGhostClip> {}
            : alien::ghostClipsFor(d, d.selectedTrack, d.selectedScene, pianoRoll.getGhostSource());
        if (multiClip.active())
            std::erase_if(ghosts, [&](const alien::PianoRollGhostClip& g) {
                for (const auto& s : multiClip.sources()) if (s.track == g.track && s.scene == g.scene) return true;
                return false;
            });
        pianoRoll.setGhostClips(std::move(ghosts));
        pianoRoll.setEditingClipCount(multiClip.active() ? static_cast<int>(multiClip.sources().size()) : 1);
    }
    // Multi-clip editing (PROD-05): with Edit > Ghost notes > Edit them with this clip on, the piano roll edits the
    // selected Session clip together with the shown clips of its loop length. Returns whether it merged any.
    bool loadMultiClip(bool allowed) {
        multiClip.end();
        if (!allowed || !pianoRoll.isMultiClipEditing() || pianoRoll.getGhostSource() == alien::GhostNoteSource::off) return false;
        const auto& d = controller.document();
        const auto editable = [this](int track, int scene) {
            return !controller.isTrackFrozen(track) && !(controller.isRecording() && controller.recordingTrack() == track
                                                         && controller.recordingScene() == scene);
        };
        if (!multiClip.begin(alien::multiClipSourcesFor(d, d.selectedTrack, d.selectedScene, pianoRoll.getGhostSource(), editable))) return false;
        pianoRoll.setPattern(multiClip.combined());
        return true;
    }
    // Writes one edit of the merged notes back into each clip it changed. The commit after the gesture makes the
    // whole edit, across every clip, one undo step.
    void writeMultiClipEdit() {
        std::vector<std::shared_ptr<const alien::PianoRollPattern>> parts;
        if (!multiClip.split(pianoRoll.getPattern(), parts)) { loadSelectedClip(); return; }   // could not split: show what is there
        const auto& sources = multiClip.sources();
        for (std::size_t i = 0; i < sources.size() && i < parts.size(); ++i)
            controller.setClipPattern(sources[i].track, sources[i].scene, *parts[i], true);
        sessionView.refreshClips();
        sessionView.refreshSelectedClip();
    }

    // Recording --------------------------------------------------------------
    void toggleRecording() {
        if (controller.isRecording()) { controller.stopRecording(); return; }
        if (projectBusy || controller.isBusy()) return;
        if (rollTarget.rack || rollTarget.placement != 0) { rollTarget = {}; loadSelectedClip(); }   // takes go into Session clips
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
            if (outcome.passes > 1) message += ", " + juce::String(outcome.passes) + " loop passes each; the last full pass plays, the others are in Takes";
            if (outcome.ended == alien::SongTakeCapture::punchedOut) message += " (stopped at the punch-out)";
            else if (outcome.ended == alien::SongTakeCapture::loopWrapped) message += " (stopped after the most loop passes a take can hold)";
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
        if (rollTarget.rack || rollTarget.placement != 0) { rollTarget = {}; loadSelectedClip(); }   // "Clear clip" is about the Session clip
        const auto& d = controller.document();
        const int track = d.selectedTrack, scene = d.selectedScene;
        if(controller.isTrackFrozen(track)){timelineView.setStatus("Unfreeze the track before editing its source clips.");return;}
        if (multiClip.active()) loadSelectedClip(false);   // "Clear clip" clears this clip, not every clip being edited with it
        controller.clearCell(track, scene);   // only this cell: content it shares stays with the others
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
        if (tab == 1 && rollTarget.rack && !songPlayback && !engine.rackPerformMode.load()) return alien::rackMode;
        return alien::patternModeForTab(tab, engine.patternMode.load(), songPlayback, engine.rackPerformMode.load());
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
            } else if (rollTarget.placement != 0) {
                const auto name = d.placementName(rollTarget.placement);
                workspace.setEditorTitle("Piano Roll", d.tracks[static_cast<std::size_t>(d.selectedTrack)].name + dot + "Song clip"
                                         + (name.isNotEmpty() ? dot + name : juce::String{}));
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
        pianoRoll.setLocked(!rollTarget.rack && rollTarget.placement == 0 && controller.isRecording() && controller.recordingTrack() == controller.document().selectedTrack
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
        const bool rackEditorPlaying = isPlaying && engine.activeRackClip.load() != 0
            && engine.activeRackClip.load() == d.channelRack.clips[static_cast<std::size_t>(d.channelRack.selectedPattern)];
        rackView.setPlayhead(rackEditorPlaying ? engine.rackStep.load() : -1);
        int displayStep = rackMode ? rackStep : currentStep;
        if (!stepMode && !rackMode) {
            const int position = engine.sessionTrackPositions[static_cast<std::size_t>(d.selectedTrack)].load();
            displayStep = position >= 0 ? position / 1000 : -1;
        }
        const bool songShown = engine.patternMode.load() == alien::songMode;
        juce::String songPosition, songMeter = "4/4";
        if (songShown) {
            // The whole song: show where the playhead is on the song's own bars, beats (of each bar's time
            // signature) and sixteenths.
            const auto position = engine.timelinePosition.load();
            displayStep = isPlaying && position >= 0 ? static_cast<int>(position / alien::timeline::ticksPerStep) : -1;
            const auto bar = controller.timeline().meterMap().barAt(static_cast<double>(std::max<std::int64_t>(0, displayStep >= 0 ? position : 0)));
            const auto into = std::max<std::int64_t>(0, (displayStep >= 0 ? position : 0) - bar.start);
            songMeter = juce::String(bar.meter.numerator) + "/" + juce::String(bar.meter.denominator);
            if (displayStep >= 0)
                songPosition = juce::String(bar.index + 1) + " . " + juce::String(into / bar.meter.beatTicks() + 1) + " . "
                             + juce::String((into % bar.meter.beatTicks()) / alien::timeline::ticksPerStep + 1);
        }
        topBar.setPosition(displayStep, isPlaying || displayStep >= 0, songShown ? "Song" : rackMode ? "Rack" : stepMode ? "Steps" : "Session",
                           songPosition, songMeter);
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
        if (rollTarget.rack) pianoRoll.setPlayheadStep(rackEditorPlaying ? engine.rackStep.load() : -1.0);
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
    void startTrackFreeze(int track) {
        if(exportCancellation || projectBusy || controller.isBusy())return;
        const auto refusal=controller.freezeRefusal(track);
        if(refusal.isNotEmpty()){timelineView.setStatus(refusal);return;}
        snapshotPlugins(SnapshotReason::always,true);
        const auto folder=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio/FreezeAudio");
        if(!folder.createDirectory().wasOk()){timelineView.setStatus("Cannot create the freeze audio folder.");return;}
        const auto file=folder.getChildFile("Freeze-"+juce::Uuid().toString()+".wav");
        auto song=controller.captureArrangement();auto assets=controller.document();
        auto expected=std::make_shared<alien::timeline::Timeline>(controller.timeline());
        const auto trackId=assets.tracks[static_cast<std::size_t>(track)].id;
        auto project=std::make_shared<alien::SessionProjectData>(alien::toProjectData(assets));
        alien::WavExportOptions options;options.freezeTrack=track;options.toTick=controller.freezeEndTick(track);
        auto renderEngine=std::make_shared<ToneEngine>();renderEngine->prepare(48000);
        auto renderHost=std::make_shared<alien::PluginHost>(*renderEngine->plugins,hostLogFile());
        std::vector<alien::PluginRackState> racks(static_cast<std::size_t>(assets.trackCount()));
        racks[static_cast<std::size_t>(track)]=assets.tracks[static_cast<std::size_t>(track)].plugins;
        const auto errors=renderHost->restore(racks,{},alien::ReturnRacks{},48000);
        if(errors.isNotEmpty()){timelineView.setStatus("Freeze stopped: "+errors);return;}
        alien::DrumSamplePointers drums{};
        for(int t=0;t<assets.trackCount();++t)for(int p=0;p<alien::drumPadCount;++p)drums[t][p]=assets.tracks[t].drumAssets[p].get();
        exportCancellation=std::make_shared<std::atomic<bool>>(false);auto cancel=exportCancellation;
        setProjectBusy(true);timelineView.setExporting(true);timelineView.setStatus("Freezing track. Cancel export stops this operation.");
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> job=[safe,track,trackId,file,song,project,assets,expected,options,drums,cancel,renderEngine,renderHost]()mutable{
            juce::String error;
            const bool rendered=alien::exportArrangementWav(file,*project,*song,drums,*cancel,error,renderEngine.get(),options);
            auto result=std::make_shared<alien::AudioClipImportResult>();
            if(rendered && !cancel->load())*result=alien::importSessionAudioClip(file);
            juce::MessageManager::callAsync([safe,track,trackId,file,expected,cancel,result,error,engine=std::move(renderEngine),host=std::move(renderHost)]{
                if(!safe){file.deleteFile();return;}
                safe->exportCancellation.reset();safe->setProjectBusy(false);safe->timelineView.setExporting(false);
                const auto& doc=safe->controller.document();
                const bool current=track>=0 && track<doc.trackCount() && doc.tracks[static_cast<std::size_t>(track)].id==trackId;
                if(cancel->load() || !current || !result->clip || !safe->controller.installFrozenTrack(track,file,result->clip,*expected)){
                    file.deleteFile();
                    safe->timelineView.setStatus(cancel->load()?"Freeze cancelled; original track unchanged."
                        :error.isNotEmpty()?error:result->error.isNotEmpty()?result->error:"Could not install frozen audio (project changed, scene or memory limit). Original track unchanged.");
                }else{safe->refreshExtraViews();safe->timelineView.setStatus("Track frozen. Fader, pan, sends and automation remain live; right-click the track header to unfreeze.");}
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioWorkPool.addJob(std::move(job));
    }

    void chooseStemExport(std::int64_t from, std::int64_t to) {
        if (exportCancellation) { exportCancellation->store(true); return; }
        if (projectBusy || controller.isBusy()) return;
        exportFileChooser = std::make_unique<juce::FileChooser>("Choose parent folder for stems", juce::File{}, "*");
        juce::Component::SafePointer<MainComponent> safe(this);
        exportFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [safe, from, to](const juce::FileChooser& chooser) {
                if (safe && chooser.getResult().isDirectory())
                    safe->exportWav(chooser.getResult().getNonexistentChildFile("Alien Studio stems", "", false), from, to, true);
            });
    }
    void exportWav(juce::File file, std::int64_t from = 0, std::int64_t to = -1, bool stems = false) {
        if (projectBusy || controller.isBusy()) return;
        snapshotPlugins();
        auto song = controller.captureArrangement();
        alien::WavExportOptions options;
        options.fromTick = from; options.toTick = to; options.stems = stems;
        auto keepAlive = std::make_shared<std::vector<std::shared_ptr<const alien::SessionAudioClip>>>();
        options.rackClips = controller.rackClipsForExport(*keepAlive);
        auto project = std::make_shared<alien::SessionProjectData>(alien::toProjectData(controller.document()));
        auto renderEngine = std::make_shared<ToneEngine>();
        renderEngine->prepare(48000);
        // Kept until the export ends: a plugin that fails during the render is reported through it.
        auto renderHost = std::make_shared<alien::PluginHost>(*renderEngine->plugins, hostLogFile());
        auto exportRacks=pluginStates();
        for(std::size_t t=0;t<exportRacks.size();++t)if(controller.isTrackFrozen(static_cast<int>(t)))exportRacks[t]={};
        { const auto errors=renderHost->restore(exportRacks,stems ? alien::PluginRackState{} : controller.document().masterPlugins,controller.returnRacks(),controller.busRacks(),48000);

          if(errors.isNotEmpty()){ timelineView.setStatus("Export stopped: " + errors);return; } }
        auto assets = controller.document();
        alien::DrumSamplePointers drums{};
        for (int t = 0; t < assets.trackCount(); ++t) for (int p = 0; p < alien::drumPadCount; ++p) drums[t][p] = assets.tracks[t].drumAssets[p].get();
        exportCancellation = std::make_shared<std::atomic<bool>>(false); auto cancel = exportCancellation;
        setProjectBusy(true); timelineView.setExporting(true); timelineView.setStatus(stems ? "Exporting stems. Click Cancel export to stop." : "Exporting WAV. Click Cancel export to stop.");
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
    // A Session track's index, maxSessionTracks for the master, or a return's or bus's rack index (PluginRuntime::returnIndex, busIndex).

    int pluginTarget() const {
        const int t=pluginPanel.destination();
        // A deleted bus is no longer a destination: fall back to the master.
        if(alien::PluginRuntime::isBus(t) && !controller.document().buses[static_cast<std::size_t>(t-alien::PluginRuntime::busBase)].used)return alien::maxSessionTracks;
        // So is a removed return.
        if(alien::PluginRuntime::isReturn(t) && !controller.document().returns[static_cast<std::size_t>(t-alien::PluginRuntime::returnBase)].used)return alien::maxSessionTracks;
        return t>=alien::maxSessionTracks && t<alien::PluginRuntime::rackCount ? t : juce::jlimit(0,controller.document().trackCount()-1,t);

    }
    // The rack state `t` (from pluginTarget) names, in copies of the document's racks.
    static alien::PluginRackState& rackFor(int t, std::vector<alien::PluginRackState>& states, alien::PluginRackState& master, alien::ReturnRacks& returns, alien::BusRacks& buses) {
        if (alien::PluginRuntime::isBus(t)) return buses[static_cast<std::size_t>(t-alien::PluginRuntime::busBase)];
        if (alien::PluginRuntime::isReturn(t)) return returns[static_cast<std::size_t>(t-alien::PluginRuntime::returnBase)];

        return t==alien::maxSessionTracks ? master : states[static_cast<std::size_t>(t)];
    }
    std::vector<alien::PluginRackState> pluginStates() const {
        std::vector<alien::PluginRackState> states;
        for (const auto& t:controller.document().tracks) states.push_back(t.plugins);
        return states;
    }
    void refreshPlugins() {
        juce::StringArray names; for(const auto& t:controller.document().tracks) names.add(t.name);
        pluginPanel.refreshLiveInput(names,engine.liveDrumTrack.load());
        drumPanel.setLiveInput(engine.liveDrumTrack.load());
        const auto& d=controller.document();
        std::array<juce::String,alien::maxBuses> busNames;
        for(std::size_t b=0;b<busNames.size();++b)if(d.buses[b].used)busNames[b]=d.buses[b].name;
        std::array<juce::String,alien::maxReturns> returnNames;
        for(std::size_t r=0;r<returnNames.size();++r)if(d.returns[r].used)returnNames[r]=d.returns[r].name;
        const int t=pluginTarget();
        pluginPanel.refresh(names,alien::PluginRuntime::isBus(t)?d.buses[static_cast<std::size_t>(t-alien::PluginRuntime::busBase)].plugins
                                  :alien::PluginRuntime::isReturn(t)?d.returns[static_cast<std::size_t>(t-alien::PluginRuntime::returnBase)].plugins
                                  :t==alien::maxSessionTracks?d.masterPlugins:d.tracks[static_cast<std::size_t>(t)].plugins,knownPlugins,busNames,returnNames);

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
        for (const auto& r : d.returns) assigned = assigned || !r.plugins.effects.empty();
        for (const auto& b : d.buses) assigned = assigned || !b.plugins.effects.empty();

        for (const auto& t : d.tracks) assigned = assigned || !t.plugins.instrument.descriptionXml.isEmpty() || !t.plugins.effects.empty();
        return assigned;
    }
    void snapshotPlugins(SnapshotReason reason = SnapshotReason::save, bool closeEditors=false) {
        if (!pluginsAssigned()) { pluginHost.changed.store(false); return; }
        // Native parameters may have changed since the most recent timer tick.
        if (reason != SnapshotReason::always) {
            const bool edited = pluginHost.changed.load() || controller.pluginStateDirty();
            if (!edited && (reason != SnapshotReason::save || engine.playing.load())) return;
            if (reason == SnapshotReason::autosave && engine.playing.load()) return;
        }
        pluginHost.changed.store(false);
        auto tracks=pluginStates(); auto master=controller.document().masterPlugins; auto returns=controller.returnRacks(); auto buses=controller.busRacks();
        const auto position=engine.timelinePosition.load();const bool playing=engine.playing.load();
        audio.removeAudioCallback(&engine);
        if(closeEditors)pluginHost.closeEditorWindows();
        pluginHost.snapshot(tracks,master,returns,buses);

        for(std::size_t t=0;t<tracks.size();++t)if(controller.isTrackFrozen(static_cast<int>(t)))tracks[t]=controller.document().tracks[t].plugins;
        audio.addAudioCallback(&engine);
        if(closeEditors && position>=0){engine.timelineSeek.store(position);engine.playing.store(playing);}
        controller.setPluginRacks(tracks,master,returns,buses);
        loadedRacks = tracks; loadedMaster = master; loadedReturns = returns; loadedBuses = controller.busRacks();
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
        const auto states = pluginStates(); const auto master = controller.document().masterPlugins; const auto returns = controller.returnRacks();
        const auto buses = controller.busRacks();
        std::vector<int> frozen;
        for(const auto& track:controller.document().tracks)frozen.push_back(track.settings.freezeScene);
        if (!force && frozen==loadedFrozen && returns == loadedReturns && buses == loadedBuses && sameRacks(states, master, loadedRacks, loadedMaster)) { refreshPlugins(); return; }

        const bool freezeChanged=frozen!=loadedFrozen;
        const auto position=engine.timelinePosition.load();const bool playing=engine.playing.load();
        audio.removeAudioCallback(&engine);
        auto runtimeStates=states;
        for(std::size_t t=0;t<runtimeStates.size();++t)if(controller.isTrackFrozen(static_cast<int>(t)))runtimeStates[t]={};
        auto errors=pluginHost.restore(runtimeStates,master,returns,buses,deviceRate());
        loadedFrozen=frozen;
        audio.addAudioCallback(&engine);
        if(freezeChanged && position>=0){engine.timelineSeek.store(position);engine.playing.store(playing);}
        loadedRacks = states; loadedMaster = master; loadedReturns = returns; loadedBuses = buses;

        pluginPanel.setStatus(errors.isEmpty()?"Plugin racks restored. Select a slot to open its editor.":"Missing or unsupported plugins (saved state preserved):\n"+errors);
        refreshPlugins();
    }
    void addPlugin(bool instrument,int choice) {
        if(controller.isTrackFrozen(pluginTarget())){pluginPanel.setStatus("Unfreeze the track before editing its instrument or effects.");return;}
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
        const int t=pluginTarget(); if(instrument && t>=alien::maxSessionTracks)return;
        snapshotPlugins(SnapshotReason::always); auto states=pluginStates();auto master=controller.document().masterPlugins;auto returns=controller.returnRacks();auto buses=controller.busRacks();
        auto& rack=rackFor(t,states,master,returns,buses);

        alien::PluginState next{chosen.createXml()->toString(),{},false};
        if(instrument)rack.instrument=next;
        else { if(rack.effects.size()>=8){pluginPanel.setStatus("Each rack supports eight FX slots.");return;}rack.effects.push_back(next); }
        controller.setPluginRacks(states,master,returns,buses); restorePlugins();
    }

    void pluginAction(int op,int slot) {
        if(controller.isTrackFrozen(pluginTarget())){pluginPanel.setStatus("Unfreeze the track before editing its instrument or effects.");return;}
        if(projectBusy || controller.isBusy())return;
        const int t=pluginTarget();
        if(op==0){const auto problem=pluginHost.openEditor(t,slot);if(problem.isNotEmpty()){pluginPanel.setStatus(problem);refreshPlugins();}return;}
        snapshotPlugins(SnapshotReason::always);auto states=pluginStates();auto master=controller.document().masterPlugins;auto returns=controller.returnRacks();auto buses=controller.busRacks();
        auto& rack=rackFor(t,states,master,returns,buses);

        if(slot<0){if(t>=alien::maxSessionTracks)return;if(op==1)rack.instrument.bypass=!rack.instrument.bypass;else if(op==2)rack.instrument={};}
        else if(slot<static_cast<int>(rack.effects.size())){
            if(op==1)rack.effects[slot].bypass=!rack.effects[slot].bypass;
            else if(op==2)rack.effects.erase(rack.effects.begin()+slot);
            else { const int other=slot+(op==3?-1:1);if(other>=0 && other<static_cast<int>(rack.effects.size()))std::swap(rack.effects[slot],rack.effects[other]); }
        }
        controller.setPluginRacks(states,master,returns,buses);restorePlugins();
    }

    // Keys the effect in `slot` of the shown rack from a track (before its FX or after its fader), a bus or a
    // return (`source` -1: no key). Only a key the mix makes before this rack runs is accepted.
    void setSidechain(int slot,alien::KeySource from,int source) {
        const int t=pluginTarget();
        if(controller.isTrackFrozen(t)){pluginPanel.setStatus("Unfreeze the track before editing its effects.");return;}
        const auto& d=controller.document();
        const bool trackKey=from==alien::KeySource::track || from==alien::KeySource::trackPostFader;
        if(projectBusy || controller.isBusy() || (source>=0 && !alien::keyReady(alien::PluginRuntime::stageOf(t),from))
           || (trackKey && (source>=d.trackCount() || (source==t && from==alien::KeySource::track)))
           || (from==alien::KeySource::bus && (source<0 || source>=alien::maxBuses || !d.buses[static_cast<std::size_t>(source)].used))
           || (from==alien::KeySource::returnTrack && (source<0 || source>=alien::maxReturns || !d.returns[static_cast<std::size_t>(source)].used)))return;
        snapshotPlugins(SnapshotReason::always);auto states=pluginStates();auto master=d.masterPlugins;auto returns=controller.returnRacks();auto buses=controller.busRacks();
        auto& rack=rackFor(t,states,master,returns,buses);
        if(slot<0 || slot>=static_cast<int>(rack.effects.size()))return;
        auto& effect=rack.effects[static_cast<std::size_t>(slot)];
        if(source<0) from=alien::KeySource::track;
        if(effect.sidechain==source && effect.sidechainFrom==from)return;
        effect.sidechain=source; effect.sidechainFrom=from;
        controller.setPluginRacks(states,master,returns,buses);restorePlugins();
        const auto name=from==alien::KeySource::bus?d.buses[static_cast<std::size_t>(std::max(source,0))].name
                       :from==alien::KeySource::returnTrack?d.returns[static_cast<std::size_t>(std::max(source,0))].name
                       :source>=0?d.tracks[static_cast<std::size_t>(source)].name+(from==alien::KeySource::trackPostFader?" after its fader":""):juce::String();
        pluginPanel.setStatus(source<0?"The effect hears only its own input again."
            :"The effect is keyed by "+name+": it reacts to that sound as it plays.");
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
    std::vector<int> loadedFrozen;
    std::vector<alien::PluginRackState> loadedRacks; // Rack states the plugin host last loaded or captured.
    alien::PluginRackState loadedMaster;
    alien::ReturnRacks loadedReturns;
    alien::BusRacks loadedBuses;

    std::unique_ptr<juce::FileChooser> pluginChooser, relinkChooser;
    alien::TopBar topBar;
    alien::ApplicationToolbar applicationToolbar;
    alien::StatusBar statusBar;
    alien::KeyboardDock keyboardDock{keyboardState};
    alien::StepSequencerView stepsView{controller};
    alien::PianoRollEditor pianoRoll;
    alien::SessionView sessionView{controller, engine};
    alien::TrackMixerPanel mixerPanel{controller, engine.tracks, engine.returns, engine.masterControls, engine.buses};

    alien::DevicesView devicesView{engine.midi};
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    alien::CompactWorkspace workspace{tabs, devicesView};
    TypingPianoRouter typingInput{*this, keyboardState};
    juce::Component::SafePointer<juce::Component> keySource;
    bool loadingClip{}, suppressPianoRollCommit{}, wasDirty{};
    // What the piano roll edits: the selected Session clip, or a Channel Rack channel in the rack's selected pattern.
    // What the piano roll edits: the selected Session clip (the default), a Channel Rack channel,
    // or a Song placement clip (placement: its library id).
    struct RollTarget { bool rack{}; int channel{}; std::uint64_t placement{}; };
    alien::MultiClipEdit multiClip;   // the Session clips the piano roll edits together, while that is on
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
