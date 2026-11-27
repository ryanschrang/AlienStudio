#include "ProjectController.h"

// Out-of-line members of ProjectController (moved from ProjectController.h so they compile once instead of in
// every file that includes the header).

namespace alien {

ProjectController::ProjectController(ToneEngine& toneEngine) : engine(toneEngine), doc(ProjectDocument::createDefault()) {
    runtimeDirty = false;
    history.reset(doc);
    savedId = history.currentId();
    markStreamProblemsReported();   // only what happens from here on is reported (T1-04 D12)
}

ProjectController::~ProjectController() {
    warpCancelled.store(true);
    alive.reset();
    audioFileChooser.reset();
    projectFileChooser.reset();
    pool.removeAllJobs(true, -1);
    engine.arrangementPreparing.store(false);
}

bool ProjectController::applyProjectScale(int root, ScaleType type) {
    const ProjectMusicalScale next{true, root, type};
    if (!projectScaleOpen() || !next.valid()) return false;
    doc.musicalScale = next;
    if (onMusicalScaleChanged) onMusicalScaleChanged();
    return true;
}

void ProjectController::commit(const juce::String& label, int focusTrack, int focusScene, const juce::String& coalesceKey, bool continuous, int undoTrack, int undoScene) {
    for(int t=0;t<doc.trackCount();++t)engine.tracks[static_cast<std::size_t>(t)].freezeScene.store(doc.tracks[static_cast<std::size_t>(t)].settings.freezeScene);
    doc.synchronizeTrackRegistry();
    const auto previous = history.currentId();
    history.record(doc, label, focusTrack, focusScene, coalesceKey, continuous, undoTrack, undoScene);
    if (history.currentId() != previous || (engine.arrangementPreparing.load() && !warpWorkActive)) publishArrangement();
    historyChanged();
}

void ProjectController::markAsNewBaseline() {
    runtimeDirty = false;
    history.reset(doc);
    savedId = history.currentId();
    historyChanged();
}

void ProjectController::setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master) {
    setPluginRacks(racks, master, returnRacks());
}

void ProjectController::setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns) {
    setPluginRacks(racks, master, returns, busRacks());
}

void ProjectController::setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns, const BusRacks& buses) {
    if (racks.size() != doc.tracks.size()) return;
    for (std::size_t t=0;t<racks.size();++t) doc.tracks[t].plugins = racks[t];
    doc.masterPlugins = master; runtimeDirty = false;
    for (std::size_t r=0;r<returns.size();++r) if (doc.returns[r].used) doc.returns[r].plugins = returns[r];
    for (std::size_t b=0;b<buses.size();++b) if (doc.buses[b].used) doc.buses[b].plugins = buses[b];
    commit("Change plugins");
}

BusRacks ProjectController::busRacks() const {
    BusRacks racks;
    for (std::size_t b=0;b<racks.size();++b) if (doc.buses[b].used) racks[b] = doc.buses[b].plugins;
    return racks;
}

ReturnRacks ProjectController::returnRacks() const {
    ReturnRacks racks;
    for (std::size_t r=0;r<racks.size();++r) if (doc.returns[r].used) racks[r] = doc.returns[r].plugins;
    return racks;
}

bool ProjectController::undo() {
    if (take.active || songTake.active) return false; // the take would be written over the restored clip
    DocumentHistory::Step step;
    if (const auto* state = history.undo(step)) { restore(*state, step); return true; }
    return false;
}

bool ProjectController::redo() {
    if (take.active || songTake.active) return false;
    DocumentHistory::Step step;
    if (const auto* state = history.redo(step)) { restore(*state, step); return true; }
    return false;
}

void ProjectController::newProject() {
    abandonRecording();
    engine.playing.store(false);
    engine.requestedRackClip.store(rack::RackPlayer::noLaunchRequest);
    engine.sceneRackClip.store(rack::RackPlayer::noLaunchRequest);
    for (auto& request : engine.requestedTrackRackClip) request.store(rack::RackPlayer::noLaunchRequest);
    for (auto& request : engine.sceneTrackRackClip) request.store(rack::RackPlayer::noLaunchRequest);
    engine.rackPerformMode.store(false);
    engine.resetRackPlayback.store(true);
    copiedCell = {};
    if (copiedContent && copiedContent->rackClip != 0) copiedContent.reset();
    if (beforeDocumentReplaced) beforeDocumentReplaced();
    releaseReplacedAudio();
    stopEverySlot();
    doc = ProjectDocument::createDefault();
    projectFile = juce::File{};
    pushDocumentToEngine();
    runtimeDirty = false;
    history.reset(doc);
    // The replaced project's audio, and the rack samples only it used (D14), freed once the engine lets go of them.
    awaitAudioRelease(setAsideRetainedAudio(forgetUnusedRackAudio()));
    savedId = history.currentId();
    discardRecovery();
    if (onDocumentReplaced) onDocumentReplaced();
    historyChanged();
    status("New session", {});
}

juce::File ProjectController::recoveryDirectory() {
    if (recoveryDirectoryOverride() != juce::File{}) return recoveryDirectoryOverride();
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Alien Studio").getChildFile("Recovery");
}

juce::File ProjectController::recoveredOriginalFile() {
    const auto info = juce::JSON::parse(recoveryInfoFile().loadFileAsString());
    if (auto* object = info.getDynamicObject()) {
        const auto path = object->getProperty("projectFile").toString();
        if (path.isNotEmpty() && juce::File::isAbsolutePath(path)) return juce::File(path);
    }
    return {};
}

void ProjectController::deleteRecoveryFiles() {
    recoveryFile().deleteFile();
    recoveryInfoFile().deleteFile();
}

void ProjectController::discardRecovery() {
    recoveryGeneration.fetch_add(1);
    std::lock_guard<std::mutex> lock(recoveryMutex);
    deleteRecoveryFiles();
    autosaving = false;
    autosavedId = 0;
}

void ProjectController::autosaveIfNeeded() {
    if (busy || autosaving || !isDirty()) return;
    if (onBeforeAutosaveSnapshot) onBeforeAutosaveSnapshot();
    else if (onBeforeSnapshot) onBeforeSnapshot();
    if (history.currentId() == autosavedId) return;
    for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
    for (int r = 0; r < maxReturns; ++r) returnSettingsEdited(r);
    for (int b = 0; b < maxBuses; ++b) busSettingsEdited(b);
    masterGainEdited();
    auto project = toProjectData(doc);

    if (!project.valid()) {
        // Say so once; a silent failure would leave the user believing they are protected.
        if (!autosaveWarned) {
            autosaveWarned = true;
            const auto reason = describeInvalidSession(doc);
            status("Autosave skipped: " + reason, reason);
        }
        return;
    }
    autosaveWarned = false;
    autosaving = true;
    const auto id = history.currentId();
    const auto original = projectFile.getFullPathName();
    const auto generation = recoveryGeneration.load();
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, project, id, original, generation] {
        std::lock_guard<std::mutex> lock(recoveryMutex);
        if (generation != recoveryGeneration.load()) return juce::ThreadPoolJob::jobHasFinished;
        bool ok = recoveryDirectory().createDirectory().wasOk();
        SessionProjectData written;
        juce::String error;
        ok = ok && writeSessionProjectFile(recoveryFile(), project, false, written, error);
        if (ok) {
            auto info = juce::var(new juce::DynamicObject());
            info.getDynamicObject()->setProperty("projectFile", original);
            info.getDynamicObject()->setProperty("savedAt", juce::Time::getCurrentTime().toISO8601(true));
            ok = recoveryInfoFile().replaceWithText(juce::JSON::toString(info));
        }
        juce::MessageManager::callAsync([this, token, id, ok, generation] {
            if (token.lock() == nullptr || generation != recoveryGeneration.load()) return;
            autosaving = false;
            if (ok) autosavedId = id;
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::recoverSession() {
    recoveringFrom = recoveredOriginalFile();
    openProject(recoveryFile(), true);
}

void ProjectController::pushDocumentToEngine() {
    doc.synchronizeTrackRegistry();
    engine.bpm.store(doc.bpm);
    engine.launchQuantizationSteps.store(doc.launchQuantizationSteps);
    engine.preset.store(doc.livePreset);
    for (std::size_t i = 0; i < doc.steps.on.size(); ++i) {
        engine.steps[i].store(doc.steps.on[i]);
        engine.stepPitches[i].store(doc.steps.pitch[i]);
        engine.stepVelocities[i].store(doc.steps.velocity[i]);
    }
    for (std::size_t t = 0; t < engine.tracks.size(); ++t)
        engine.tracks[t].set(t < doc.tracks.size() ? doc.tracks[t].settings : TrackSettings{});
    engine.setActiveTracks(doc.trackCount());
    engine.masterControls.gain.store(doc.masterGain);
    for (std::size_t r = 0; r < engine.returns.size(); ++r) {
        engine.returns[r].set(doc.returns[r].settings);
        engine.returns[r].active.store(doc.returns[r].used);
    }
    for (std::size_t b = 0; b < engine.buses.size(); ++b) {
        engine.buses[b].set(doc.buses[b].settings);
        engine.buses[b].active.store(doc.buses[b].used);
    }
    engine.recording.apply(doc.recording);

    for (std::size_t t = 0; t < engine.tracks.size(); ++t)
        for (int pad = 0; pad < drumPadCount; ++pad)
            engine.drumKits[t].samples[pad].store(t < doc.tracks.size() ? doc.tracks[t].drumAssets[pad].get() : nullptr);
    pushRackToEngine();
    publishArrangement();
}

bool ProjectController::addTrack() {
    if (busy) return false;
    if (beforeStructureEdit) beforeStructureEdit();
    if (!doc.canAddTrack() || !doc.trackRegistry.canAllocate()) return false;
    if (!prepareEngineTracks(doc.trackCount() + 1, "Could not add a track")) return false;
    doc.tracks.push_back({doc.nextTrackName(), {}, doc.nextTrackColour()});
    for (auto& scene : doc.cells) scene.emplace_back();
    publishStop(static_cast<std::size_t>(doc.trackCount() - 1));
    pushDocumentToEngine();
    return true;
}

bool ProjectController::deleteTrack(int track) {
    if (busy) return false;
    if (beforeStructureEdit) beforeStructureEdit();
    if (doc.trackCount() <= 1 || track < 0 || track >= doc.trackCount()) return false;
    stopEverySlot();
    const int route = engine.liveDrumTrack.load();
    if (route == track) engine.liveDrumTrack.store(-1);
    else if (route > track) engine.liveDrumTrack.store(route - 1);
    doc.timeline.trackDeleted(track);
    doc.channelRack.trackDeleted(track); // channels routed there fall back to the master bus
    doc.sidechainTrackDeleted(track);    // effects keyed by it lose their key
    doc.tracks.erase(doc.tracks.begin() + track);
    for (auto& scene : doc.cells) scene.erase(scene.begin() + track);
    doc.prune();
    if (doc.selectedTrack > track) --doc.selectedTrack;
    doc.clampSelection();
    pushDocumentToEngine();
    return true;
}

bool ProjectController::moveTrack(int track, int delta) {
    if (busy) return false;
    if (beforeStructureEdit) beforeStructureEdit();
    const int target = track + delta;
    if (track < 0 || track >= doc.trackCount() || target < 0 || target >= doc.trackCount()) return false;
    stopEverySlot();
    const int route = engine.liveDrumTrack.load();
    if (route == track) engine.liveDrumTrack.store(target);
    else if (route == target) engine.liveDrumTrack.store(track);
    doc.timeline.tracksSwapped(track, target);
    doc.channelRack.tracksSwapped(track, target); // channel routes follow their tracks
    doc.sidechainTracksSwapped(track, target);
    std::swap(doc.tracks[static_cast<std::size_t>(track)], doc.tracks[static_cast<std::size_t>(target)]);
    for (auto& scene : doc.cells)
        std::swap(scene[static_cast<std::size_t>(track)], scene[static_cast<std::size_t>(target)]);
    if (doc.selectedTrack == track) doc.selectedTrack = target;
    else if (doc.selectedTrack == target) doc.selectedTrack = track;
    pushDocumentToEngine();
    return true;
}

bool ProjectController::renameTrack(int track, const juce::String& name) {
    if (busy) return false;
    const auto trimmed = name.trim().substring(0, 64);
    if (track < 0 || track >= doc.trackCount() || trimmed.isEmpty()) return false;
    doc.tracks[static_cast<std::size_t>(track)].name = trimmed;
    return true;
}

bool ProjectController::setTrackColour(int track, int colour) {
    if (busy || track < 0 || track >= doc.trackCount() || colour < 0 || colour >= trackColourCount) return false;
    doc.tracks[static_cast<std::size_t>(track)].colour = colour;
    return true;
}

bool ProjectController::duplicateScene(int scene) {
    if (scene < 0 || scene >= doc.sceneCount()) return false;
    return insertScene(scene + 1, &doc.cells[static_cast<std::size_t>(scene)]);
}

bool ProjectController::deleteScene(int scene) {
    if (busy) return false;
    for(const auto& track:doc.tracks)if(track.settings.freezeScene==scene)return false;
    for(const auto& region:doc.timeline.frozenOriginals)if(region.source==scene)return false;
    if (beforeStructureEdit) beforeStructureEdit();
    if (doc.sceneCount() <= 1 || scene < 0 || scene >= doc.sceneCount()) return false;
    stopEverySlot();
    doc.timeline.sceneDeleted(scene);
    for(auto& track:doc.tracks)if(track.settings.freezeScene>scene)--track.settings.freezeScene;
    doc.scenes.erase(doc.scenes.begin() + scene);
    doc.cells.erase(doc.cells.begin() + scene);
    doc.prune();
    if (doc.selectedScene > scene) --doc.selectedScene;
    doc.clampSelection();
    return true;
}

bool ProjectController::moveScene(int scene, int delta) {
    if (busy) return false;
    if (beforeStructureEdit) beforeStructureEdit();
    const int target = scene + delta;
    if (scene < 0 || scene >= doc.sceneCount() || target < 0 || target >= doc.sceneCount()) return false;
    stopEverySlot();
    doc.timeline.scenesSwapped(scene, target);
    for(auto& track:doc.tracks){auto& frozen=track.settings.freezeScene;if(frozen==scene)frozen=target;else if(frozen==target)frozen=scene;}
    std::swap(doc.scenes[static_cast<std::size_t>(scene)], doc.scenes[static_cast<std::size_t>(target)]);
    std::swap(doc.cells[static_cast<std::size_t>(scene)], doc.cells[static_cast<std::size_t>(target)]);
    if (doc.selectedScene == scene) doc.selectedScene = target;
    else if (doc.selectedScene == target) doc.selectedScene = scene;
    return true;
}

bool ProjectController::renameScene(int scene, const juce::String& name) {
    if (busy) return false;
    const auto trimmed = name.trim().substring(0, 64);
    if (scene < 0 || scene >= doc.sceneCount() || trimmed.isEmpty()) return false;
    doc.scenes[static_cast<std::size_t>(scene)].name = trimmed;
    return true;
}

bool ProjectController::setMasterVolume(double gain) {
    if(busy || !std::isfinite(gain) || gain<0 || gain>maxMasterGain)return false;
    doc.masterGain=static_cast<float>(gain);
    engine.masterControls.gain.store(doc.masterGain);
    return true; // CommandEngine owns the history entry
}

void ProjectController::returnSettingsEdited(int r) {
    if (r < 0 || r >= maxReturns || !doc.returns[static_cast<std::size_t>(r)].used) return;
    const auto next = engine.returns[static_cast<std::size_t>(r)].snapshot();
    if (next.valid()) doc.returns[static_cast<std::size_t>(r)].settings = next;
}

void ProjectController::busSettingsEdited(int b) {
    if (b < 0 || b >= maxBuses || !doc.buses[static_cast<std::size_t>(b)].used) return;
    const auto next = engine.buses[static_cast<std::size_t>(b)].snapshot();
    if (next.valid()) doc.buses[static_cast<std::size_t>(b)].settings = next;
}

int ProjectController::addBus(const juce::String& name) {
    const int b = bus_edit::firstFreeSlot(doc);
    if (busy || bus_edit::apply(doc, {bus_edit::Kind::create, -1, name}).isNotEmpty()) return -1;
    pushDocumentToEngine();
    return b;
}

bool ProjectController::renameBus(int b, const juce::String& name) {
    return !busy && bus_edit::apply(doc, {bus_edit::Kind::rename, b, name}).isEmpty();
}

bool ProjectController::deleteBus(int b) {
    if (busy || !bus_edit::slotInUse(doc, b)) return false;
    for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
    if (bus_edit::apply(doc, {bus_edit::Kind::remove, b}).isNotEmpty()) return false;
    pushDocumentToEngine();
    return true;
}

bool ProjectController::setBusVolume(int b, double gain) {
    if (busy || !bus_edit::slotInUse(doc, b) || !std::isfinite(gain) || gain < 0 || gain > 2) return false;
    const auto value = static_cast<float>(gain); const auto slot = static_cast<std::size_t>(b);
    doc.buses[slot].settings.gain = value;
    engine.buses[slot].gain.store(value);
    return true;
}

bool ProjectController::setBusPan(int b, double pan) {
    if (busy || !bus_edit::slotInUse(doc, b) || !std::isfinite(pan) || pan < -1 || pan > 1) return false;
    const auto value = static_cast<float>(pan); const auto slot = static_cast<std::size_t>(b);
    doc.buses[slot].settings.pan = value;
    engine.buses[slot].pan.store(value);
    return true;
}

bool ProjectController::setBusMute(int b, bool muted) {
    if (busy || !bus_edit::slotInUse(doc, b)) return false;
    doc.buses[static_cast<std::size_t>(b)].settings.mute = muted;
    engine.buses[static_cast<std::size_t>(b)].mute.store(muted);
    return true;
}

bool ProjectController::setBusSolo(int b, bool soloed) {
    if (busy || !bus_edit::slotInUse(doc, b)) return false;
    doc.buses[static_cast<std::size_t>(b)].settings.solo = soloed;
    engine.buses[static_cast<std::size_t>(b)].solo.store(soloed);
    return true;
}

bool ProjectController::setReturnVolume(int r, double gain) {
    if (busy || !return_edit::slotInUse(doc, r) || !std::isfinite(gain) || gain < 0 || gain > 2) return false;
    const auto value = static_cast<float>(gain); const auto slot = static_cast<std::size_t>(r);
    doc.returns[slot].settings.gain = value;
    engine.returns[slot].gain.store(value);
    return true;
}

bool ProjectController::setReturnPan(int r, double pan) {
    if (busy || !return_edit::slotInUse(doc, r) || !std::isfinite(pan) || pan < -1 || pan > 1) return false;
    const auto value = static_cast<float>(pan); const auto slot = static_cast<std::size_t>(r);
    doc.returns[slot].settings.pan = value;
    engine.returns[slot].pan.store(value);
    return true;
}

bool ProjectController::setReturnMute(int r, bool muted) {
    if (busy || !return_edit::slotInUse(doc, r)) return false;
    doc.returns[static_cast<std::size_t>(r)].settings.mute = muted;
    engine.returns[static_cast<std::size_t>(r)].mute.store(muted);
    return true;
}

int ProjectController::addReturn(const juce::String& name) {
    const int r = return_edit::firstFreeSlot(doc);
    if (busy || return_edit::apply(doc, {return_edit::Kind::create, -1, name}).isNotEmpty()) return -1;
    pushDocumentToEngine();
    return r;
}

bool ProjectController::renameReturn(int r, const juce::String& name) {
    if (busy || !return_edit::slotInUse(doc, r) || doc.returns[static_cast<std::size_t>(r)].name == return_edit::storedName(name)) return false;
    return return_edit::apply(doc, {return_edit::Kind::rename, r, name}).isEmpty();
}

bool ProjectController::deleteReturn(int r) {
    if (busy || !return_edit::slotInUse(doc, r)) return false;
    for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
    if (return_edit::apply(doc, {return_edit::Kind::remove, r}).isNotEmpty()) return false;
    pushDocumentToEngine();
    return true;
}

bool ProjectController::setTrackOutput(int track, int output) {
    if (busy || track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size())
        || output < -1 || output >= maxBuses || (output >= 0 && !doc.buses[static_cast<std::size_t>(output)].used)) return false;
    auto& settings = doc.tracks[static_cast<std::size_t>(track)].settings;
    if (settings.output == output) return false;
    settings.output = output;
    engine.tracks[static_cast<std::size_t>(track)].output.store(output);
    return true;
}

bool ProjectController::applyConvolutionResponseCommand(const device_edit::ConvolutionResponseEdit& edit) {
    return canEditMidiClips() && device_edit::applyConvolutionResponse(doc, edit).isEmpty();
}

bool ProjectController::setTrackVolume(int track, double gain) {
    if (busy || track < 0 || track >= doc.trackCount() || !std::isfinite(gain) || gain < 0 || gain > 2) return false;
    const auto value = static_cast<float>(gain);
    doc.tracks[static_cast<std::size_t>(track)].settings.gain = value;
    engine.tracks[static_cast<std::size_t>(track)].gain.store(value);
    return true;
}

bool ProjectController::setTrackSend(int track, int r, double level) {
    if (busy || track < 0 || track >= doc.trackCount() || !return_edit::slotInUse(doc, r) || !std::isfinite(level) || level < 0 || level > 1) return false;
    const auto value = static_cast<float>(level); const auto slot = static_cast<std::size_t>(r);
    doc.tracks[static_cast<std::size_t>(track)].settings.send[slot] = value;
    engine.tracks[static_cast<std::size_t>(track)].send[slot].store(value);
    return true;
}

bool ProjectController::setTrackSendMode(int track, int r, bool preFader) {
    if (busy || track < 0 || track >= doc.trackCount() || !return_edit::slotInUse(doc, r)) return false;
    const auto slot = static_cast<std::size_t>(r);
    doc.tracks[static_cast<std::size_t>(track)].settings.sendPre[slot] = preFader;
    engine.tracks[static_cast<std::size_t>(track)].sendPre[slot].store(preFader);
    return true;
}

bool ProjectController::setTrackPan(int track, double pan) {
    if (busy || track < 0 || track >= doc.trackCount() || !std::isfinite(pan) || pan < -1 || pan > 1) return false;
    const auto value = static_cast<float>(pan);
    doc.tracks[static_cast<std::size_t>(track)].settings.pan = value;
    engine.tracks[static_cast<std::size_t>(track)].pan.store(value);
    return true;
}

bool ProjectController::setTrackMute(int track, bool muted) {
    if (busy || track < 0 || track >= doc.trackCount()) return false;
    doc.tracks[static_cast<std::size_t>(track)].settings.mute = muted;
    engine.tracks[static_cast<std::size_t>(track)].mute.store(muted);
    return true;
}

bool ProjectController::setTrackSound(int track, int preset, bool drumKit) {
    if (busy || track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size()) || isTrackFrozen(track)
        || (!drumKit && (preset < 0 || preset >= presetCount))) return false;
    auto& settings = doc.tracks[static_cast<std::size_t>(track)].settings;
    settings.instrument = drumKit ? 1 : 0;
    engine.tracks[static_cast<std::size_t>(track)].instrument.store(settings.instrument);
    if (!drumKit) { settings.preset = preset; engine.tracks[static_cast<std::size_t>(track)].preset.store(preset); }
    return true;
}

bool ProjectController::setTrackSolo(int track, bool soloed) {
    if (busy || track < 0 || track >= doc.trackCount()) return false;
    doc.tracks[static_cast<std::size_t>(track)].settings.solo = soloed;
    engine.tracks[static_cast<std::size_t>(track)].solo.store(soloed);
    return true;
}

void ProjectController::trackSettingsEdited(int track) {
    if (track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size())) return;
    auto next=engine.tracks[static_cast<std::size_t>(track)].snapshot();
    const auto& previous=doc.tracks[static_cast<std::size_t>(track)].settings;
    if(previous.frozen()) {
        next.preset=previous.preset;next.instrument=previous.instrument;next.freezeScene=previous.freezeScene;
        engine.tracks[static_cast<std::size_t>(track)].preset.store(previous.preset);
        engine.tracks[static_cast<std::size_t>(track)].instrument.store(previous.instrument);
    }
    doc.tracks[static_cast<std::size_t>(track)].settings=next;
}

bool ProjectController::setTrackArmed(int track, bool armed) {
    if (!recordingSetupOpen(track)) return false;
    doc.tracks[static_cast<std::size_t>(track)].settings.armed = armed;
    engine.tracks[static_cast<std::size_t>(track)].armed.store(armed);
    return true;
}

bool ProjectController::setTrackInput(int track, int input, bool stereo) {
    if (!recordingSetupOpen(track) || input < 0 || input + (stereo ? 1 : 0) >= maxTrackInputs) return false;
    auto& settings = doc.tracks[static_cast<std::size_t>(track)].settings;
    settings.input = input; settings.stereoInput = stereo;
    auto& controls = engine.tracks[static_cast<std::size_t>(track)];
    controls.input.store(input); controls.stereoInput.store(stereo);
    return true;
}

bool ProjectController::setTrackMonitor(int track, int mode) {
    if (!recordingSetupOpen(track) || mode < 0 || mode > 2) return false;
    doc.tracks[static_cast<std::size_t>(track)].settings.monitor = mode;
    engine.tracks[static_cast<std::size_t>(track)].monitor.store(mode);
    return true;
}

bool ProjectController::recordingSetupOpen(int track) const {
    return !busy && track >= 0 && track < doc.trackCount() && track < static_cast<int>(engine.tracks.size()) && !isTrackFrozen(track);
}

float ProjectController::takeInputLevel(int track) noexcept {
    if (track < 0 || track >= static_cast<int>(engine.tracks.size())) return 0.0f;
    return engine.tracks[static_cast<std::size_t>(track)].inputPeak.exchange(0.0f, std::memory_order_relaxed);
}

void ProjectController::selectClip(int track, int scene) {
    if (!doc.isValidCell(track, scene)) return;
    doc.selectedTrack = track;
    doc.selectedScene = scene;
}

void ProjectController::launchClip(int track, int scene) {
    selectClip(track, scene);
    if (doc.isValidCell(track, scene) && doc.clip(track, scene).rackClip != 0) {
        if (launchRackClip(doc.clip(track, scene).rackClip, track)) publishStop(static_cast<std::size_t>(track));
        return;
    }
    if (doc.isValidCell(track, scene)) stopRackOnTrack(track);
    publishClip(doc.selectedTrack, doc.selectedScene);
}

bool ProjectController::startTransportForLaunch() {
    if (engine.patternMode.load() != sessionMode || engine.playing.load()
        || engine.recording.phase.load() == RecordControls::countIn) return false;
    engine.playing.store(true);
    return true;
}

void ProjectController::launchScene(int scene) {
    scene = juce::jlimit(0, doc.sceneCount() - 1, scene);
    bool hasRack = false;
    for (int track = 0; track < doc.trackCount(); ++track)
        if (const auto source = doc.clip(track, scene).rackClip) {
            if (!canLaunchRackClip(source)) return;
            hasRack = true;
        }
    if (hasRack) {
        pushRackToEngine(); engine.rackPerformMode.store(true); engine.patternMode.store(1); engine.playing.store(true);
    }
    engine.requestedRackClip.store(rack::RackPlayer::noLaunchRequest);
    engine.sceneRackClip.store(0); // A scene owns every Session slot and stops the menu's standalone launch.
    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
        engine.requestedTrackRackClip[track].store(rack::RackPlayer::noLaunchRequest);
        auto source = static_cast<int>(track) < doc.trackCount() ? doc.clip(static_cast<int>(track), scene).rackClip : 0;
        if (doc.legacyRackSceneLaunch && source != 0)
            for (int earlier = 0; earlier < static_cast<int>(track); ++earlier)
                if (doc.clip(earlier, scene).rackClip == source) { source = 0; break; }
        engine.sceneTrackRackClip[track].store(source);
    }
    engine.synchronizedSceneLaunch.store(true);
    for (int track = 0; track < doc.trackCount(); ++track) {
        const auto rack = doc.clip(track, scene).rackClip;
        if (rack != 0) publishStop(static_cast<std::size_t>(track));
        else publishClip(track, scene);
    }
}

void ProjectController::stopAllTracks() {
    engine.sceneRackClip.store(0);
    for (auto& source : engine.sceneTrackRackClip) source.store(0);
    engine.synchronizedSceneLaunch.store(true);
    for (int track = 0; track < doc.trackCount(); ++track) publishStop(static_cast<std::size_t>(track));
    stopRackClip();
}

int ProjectController::rackPatternForClip(std::uint64_t clip) const noexcept {
    return doc.rackPatternForClip(clip);
}

bool ProjectController::rackPatternInUse(int pattern) const {
    if (!doc.channelRack.validPattern(pattern)) return false;
    const auto id = doc.channelRack.clips[static_cast<std::size_t>(pattern)];
    for (const auto& scene : doc.cells) for (auto cell : scene) if (cell.value == id) return true;
    for (const auto& r : doc.timeline.regions) if (r.isPattern() && r.rackClip == id) return true;
    return false;
}

bool ProjectController::canLaunchRackClip(std::uint64_t clip) const {
    if (busy || isRecording() || engine.patternMode.load() == 2 || rackPatternForClip(clip) < 0) return false;
    for (int c = 0; c < doc.channelRack.channelCount; ++c)
        if (isTrackFrozen(doc.channelRack.channels[static_cast<std::size_t>(c)].output)) return false;
    return true;
}

bool ProjectController::launchRackClip(std::uint64_t clip, int track) {
    if (track < -1 || track >= doc.trackCount()) return false;
    if (!canLaunchRackClip(clip)) return false;
    pushRackToEngine();
    engine.rackPerformMode.store(true);
    engine.patternMode.store(1);
    if (track >= 0) engine.requestedTrackRackClip[static_cast<std::size_t>(track)].store(clip);
    else engine.requestedRackClip.store(clip);
    engine.playing.store(true);
    return true;
}

bool ProjectController::stopRackClip() {
    if (busy || isRecording()) return false;
    engine.requestedRackClip.store(0);
    return true;
}

void ProjectController::stopTrack(int track) {
    if (track < 0 || track >= doc.trackCount()) return;
    publishStop(static_cast<std::size_t>(track));
    stopRackOnTrack(track);
}

void ProjectController::stopRackOnTrack(int track) {
    if (track < 0 || track >= doc.trackCount()) return;
    const auto index = static_cast<std::size_t>(track);
    const auto hasSource = [](std::uint64_t id) { return id != 0 && id != rack::RackPlayer::noLaunchRequest; };
    if (hasSource(engine.activeTrackRackClip[index].load()) || hasSource(engine.queuedTrackRackClip[index].load())
        || hasSource(engine.requestedTrackRackClip[index].load()) || hasSource(engine.sceneTrackRackClip[index].load()))
        engine.requestedTrackRackClip[index].store(0);
    else if (doc.legacyRackSceneLaunch) {
        // Before D3, duplicate assignments in a scene controlled one occurrence.
        for (int scene = 0; scene < doc.sceneCount(); ++scene) if (const auto source = doc.clip(track, scene).rackClip; source != 0)
            for (std::size_t other = 0; other < ToneEngine::sessionTracks; ++other)
                if (engine.activeTrackRackClip[other].load() == source || engine.queuedTrackRackClip[other].load() == source
                    || engine.requestedTrackRackClip[other].load() == source || engine.sceneTrackRackClip[other].load() == source)
                    engine.requestedTrackRackClip[other].store(0);
    }
}

void ProjectController::publishClip(int track, int scene) {
    if (!doc.isValidCell(track, scene)) return;
    if (doc.clip(track, scene).rackClip != 0) return;
    if(isTrackFrozen(track) && scene!=doc.tracks[static_cast<std::size_t>(track)].settings.freezeScene){publishStop(static_cast<std::size_t>(track));return;}
    const auto& clip = doc.clip(track, scene);
    const auto index = static_cast<std::size_t>(track);
    engine.requestedPianoRollClip[index].store(scene);
    SessionClipLaunch launch;
    launch.pattern = clip.isAudioClip() ? PianoRollPattern{} : clip.notes();
    launch.audio = clip.audio.get();
    launch.clipIndex = scene;
    engine.pianoRollLaunches[index].publish(launch);
}

ProjectController::RecordStart ProjectController::startRecording() {
    if (take.active || songTake.active) return {false, "Already recording."};
    if (busy) return {false, "Wait for the current operation to finish."};
    if (!recordMidiNotes) return {false, "MIDI recording commands are not connected."};
    if (engine.recording.active()) return {false, "The previous take is still finishing. Try again in a moment."};
    if (engine.patternMode.load() != 1)
        return {false, "Open the Piano Roll or Session view to record."};
    const int track = doc.selectedTrack, scene = doc.selectedScene;
    if(isTrackFrozen(track))return {false,"Unfreeze the track before recording."};
    if (!doc.isValidCell(track, scene)) return {false, "Select a clip to record into."};
    const auto& clip = doc.clip(track, scene);
    if (clip.isAudioClip()) return {false, "This clip holds audio. Choose an empty or MIDI clip to record into."};
    if (clip.rackClip != 0) return {false, "This cell launches a Channel Rack clip. Choose an empty or MIDI clip to record into."};

    take = {};
    take.active = true;
    take.track = track; take.scene = scene;
    take.replace = doc.recording.replace;
    take.id = ++lastTakeId;
    take.assembler.reset(clip.notes().loopSteps);
    take.droppedAtStart = engine.recording.events.droppedCount();
    for (RecordEvent stale; engine.recording.events.pop(stale);) {}

    engine.recording.apply(doc.recording);
    engine.recording.targetTrack.store(track);
    engine.recording.targetScene.store(scene);
    engine.recording.take.store(take.id);
    prepareClipForTake(track, scene, take.replace);
    engine.recording.stopRequested.store(false);
    engine.recording.startRequested.store(true);
    return {true, {}};
}

void ProjectController::stopRecording() {
    if (songTake.active) { stopSongTake(); return; }
    if (!take.active) return;
    // Cancelled before the engine saw the request: nothing was recorded.
    if (engine.recording.startRequested.exchange(false)) {
        RecordEvent refusal;
        refusal.kind = RecordEventKind::refused;
        refusal.take = take.id;
        take.assembler.consume(refusal);
        take.cancelled = true;
        finishTake();
        return;
    }
    engine.recording.stopRequested.store(true);
}

void ProjectController::abandonRecording() {
    if (songTake.active) { songTake.abandoned = true; stopSongTake(); }
    if (!take.active) return;
    take.active = false;
    engine.recording.startRequested.store(false);
    engine.recording.stopRequested.store(true);
}

void ProjectController::pollRecording() {
    if (songTake.active) { pollSongTake(); return; }
    if (!take.active) {
        for (RecordEvent stale; engine.recording.events.pop(stale);) {}
        return;
    }
    bool changed = false, ended = false;
    for (RecordEvent event; engine.recording.events.pop(event);) {
        if (event.take != take.id) continue;
        take.assembler.consume(event);
        changed = true;
        if (event.kind == RecordEventKind::refused) take.refused = true;
        if (event.kind == RecordEventKind::end || event.kind == RecordEventKind::refused) { ended = true; break; }
    }
    if (ended) finishTake();
    else if (changed) previewTake();
}

ProjectController::RecordStart ProjectController::startSongTake(const juce::File& folder, double sampleRate, int deviceInputs) {
    if (take.active || songTake.active) return {false, "Already recording."};
    if (busy) return {false, "Wait for the current operation to finish."};
    if (engine.recording.active()) return {false, "The previous take is still finishing. Try again in a moment."};
    if (engine.patternMode.load() != 2) return {false, "Open the Song view (or turn on Song mode) to record onto the timeline."};
    std::vector<int> armed;
    bool anyArmed = false, frozenArmed=false;
    for (int t = 0; t < doc.trackCount() && t < maxSessionTracks; ++t) {
        const auto& s = doc.tracks[static_cast<std::size_t>(t)].settings;
        anyArmed = anyArmed || s.armed;
        frozenArmed=frozenArmed || (s.armed && s.frozen());
        if (s.armed && !s.frozen() && s.input + (s.stereoInput ? 1 : 0) < deviceInputs) armed.push_back(t);
    }
    if(armed.empty() && frozenArmed)return {false,"Unfreeze an armed track before recording."};
    if (armed.empty())
        return {false, anyArmed ? "The armed tracks' inputs are not enabled. Turn them on in Audio settings."
                                : "Arm a track in the Song view (the button on its header) to record onto it."};
    if (doc.timeline.takes.size() + armed.size() > timeline::maxTakes)
        return {false, "The recording take list is full. Forget unused takes in the Takes panel before recording."};
    if (!folder.createDirectory().wasOk()) return {false, "Cannot create the folder for takes: " + folder.getFullPathName()};

    SongTake next;
    for (const int t : armed) {
        const auto& track = doc.tracks[static_cast<std::size_t>(t)];
        const auto file = folder.getNonexistentChildFile(juce::File::createLegalFileName(track.name + " take"), ".wav", false);
        juce::String error;
        // Tests that move the autosave folder keep take journals next to it too.
        const auto journals = recoveryDirectoryOverride() != juce::File{} ? recoveryDirectoryOverride().getChildFile("Recording Recovery")
                                                                           : RecordingFile::recoveryDirectory();
        auto recording = DiskRecording::start(file, sampleRate, track.settings.stereoInput ? 2 : 1, error, journals);
        if (!recording) {
            for (auto& started : next.recordings) started.second->finish(); // nothing captured: removes its files
            return {false, "Cannot record " + track.name + ": " + error};
        }
        next.recordings.emplace_back(t, std::move(recording));
    }
    next.active = true;
    next.id = ++lastTakeId;
    next.sampleRate = sampleRate;
    next.bpm = doc.bpm;
    next.tempoChanges = doc.timeline.tempoChanges;
    next.offsetMs = doc.recording.audioLatencyOffsetMs;
    next.punch = doc.recording.punch;
    next.punchIn = doc.timeline.loopStart;
    next.punchOut = doc.timeline.loopEnd;
    next.loopStart = doc.timeline.loopStart;
    next.loopEnd = doc.timeline.loopEnd;
    songTake = std::move(next);
    for (RecordEvent stale; engine.recording.events.pop(stale);) {}

    auto& capture = *engine.songTake;
    for (auto& sink : capture.sinks) sink.store(nullptr);
    for (const auto& [t, recording] : songTake.recordings) capture.sinks[static_cast<std::size_t>(t)].store(recording.get());
    capture.punch.store(songTake.punch);
    capture.punchIn.store(songTake.punchIn);
    capture.punchOut.store(songTake.punchOut);
    engine.recording.apply(doc.recording);
    engine.recording.targetTrack.store(-1);
    engine.recording.targetScene.store(-1);
    engine.recording.take.store(songTake.id);
    engine.recording.audioTake.store(true);
    engine.recording.stopRequested.store(false);
    engine.recording.startRequested.store(true);
    return {true, {}};
}

std::vector<int> ProjectController::songTakeTracks() const {
    std::vector<int> result;
    for (const auto& recording : songTake.recordings) result.push_back(recording.first);
    return result;
}

bool ProjectController::applyRecordSettings(const RecordSettings& settings) {
    if (busy || !settings.valid()) return false;
    doc.recording = settings;
    engine.recording.apply(settings);
    return true;
}

void ProjectController::republishLinked(int track, int scene) {
    const auto link = doc.isValidCell(track, scene) ? doc.linkOf(track, scene) : 0;
    if (link == 0) return;
    for (int s = 0; s < doc.sceneCount(); ++s) for (int t = 0; t < doc.trackCount(); ++t)
        if ((t != track || s != scene) && doc.linkOf(t, s) == link) republishIfLive(t, s);
}

bool ProjectController::applyRackCommand(const rack_command::Edit& edit) {
    if (!canEditMidiClips()) return false;
    const auto before = doc;
    if (rack_command::apply(doc, edit).isNotEmpty()) return false;
    if (sameContent(before, doc)) return true;
    if (edit.kind == rack_command::Kind::create) pushRackToEngine();
    else publishStop(static_cast<std::size_t>(edit.track)); // preserve existing assignment playback semantics
    return true;
}

bool ProjectController::copyCell(int track, int scene) {
    if (!doc.isValidCell(track, scene) || !doc.cellClip(track, scene)) return false;
    copiedCell = doc.cellClip(track, scene);
    copiedContent = doc.clip(track, scene);
    return true;
}

bool ProjectController::applyClipboardClipCommand(const clipboard_clip::Edit& edit, const clipboard_clip::Clipboard& clipboard) {
    if (!canEditMidiClips()) return false;
    const auto before = doc;
    if (clipboard_clip::apply(doc, edit, &clipboard).isNotEmpty()) return false;
    if (sameContent(before, doc)) return true;
    if (doc.channelRack != before.channelRack) pushRackToEngine();
    if (doc.clip(edit.track, edit.scene).rackClip) publishStop(static_cast<std::size_t>(edit.track));
    else if (edit.kind != clipboard_clip::Kind::unique) republishIfLive(edit.track, edit.scene);
    return true;
}

bool ProjectController::applyMidiClipCommand(const midi_clip::Edit& edit) {
    if (!canEditMidiClips() || midi_clip::refusal(doc,edit).isNotEmpty()) return false;
    const auto id=doc.cellClip(edit.track,edit.scene);const auto link=doc.linkOf(edit.track,edit.scene);
    if (!midi_clip::apply(doc,edit)) return false;
    if (edit.kind!=midi_clip::Kind::makeUnique && (id!=doc.cellClip(edit.track,edit.scene) || link!=doc.linkOf(edit.track,edit.scene))) republishIfLive(edit.track,edit.scene);
    return true;
}

bool ProjectController::applyDuplicateClipCommand(const session_clip::Duplicate& edit) {
    if (!canEditMidiClips()) return false;
    const auto rackBefore = doc.channelRack;
    if (session_clip::duplicate(doc, edit).isNotEmpty()) return false;
    if (doc.channelRack != rackBefore) pushRackToEngine();
    const auto index = static_cast<std::size_t>(edit.toTrack);
    if (engine.requestedPianoRollClip[index].load() == edit.toScene) {
        if (doc.clip(edit.toTrack, edit.toScene).rackClip) publishStop(index);
        else publishClip(edit.toTrack, edit.toScene);
    }
    return true;
}

bool ProjectController::applyMoveClipCommand(const session_clip::Move& edit) {
    if (!canEditMidiClips() || session_clip::moveRefusal(doc, edit).isNotEmpty()) return false;
    if (edit.track == edit.toTrack && edit.scene == edit.toScene) return true;
    const auto sourceRequest = engine.requestedPianoRollClip[static_cast<std::size_t>(edit.track)].load();
    const auto targetRequest = engine.requestedPianoRollClip[static_cast<std::size_t>(edit.toTrack)].load();
    if (!session_clip::move(doc, edit)) return false;
    // Do not replace another scene's queued request, including a pending stop.
    if (sourceRequest == edit.scene) publishClip(edit.track, edit.scene);
    if (targetRequest == edit.toScene) {
        if (doc.clip(edit.toTrack, edit.toScene).rackClip) publishStop(static_cast<std::size_t>(edit.toTrack));
        else publishClip(edit.toTrack, edit.toScene);
    }
    return true;
}

bool ProjectController::applyDeleteClipCommand(int track, int scene) {
    if (!canEditMidiClips() || session_clip::deleteRefusal(doc, track, scene).isNotEmpty()) return false;
    const bool present = static_cast<bool>(doc.cellClip(track, scene));
    if (!session_clip::remove(doc, track, scene)) return false;
    if (present) republishIfLive(track, scene);
    return true;
}

bool ProjectController::applyMakePlacementUnique(std::uint32_t regionId) {
    if (busy || isRecording()) return false;
    auto candidate = doc;
    if (song_placement::makeUnique(candidate, regionId).isNotEmpty()) return false;
    if (candidate.sceneCount() != doc.sceneCount() && beforeStructureEdit) beforeStructureEdit();
    const bool rackChanged = !(candidate.channelRack == doc.channelRack);
    doc = std::move(candidate);
    if (rackChanged) pushRackToEngine();
    return true;
}

void ProjectController::republishIfLive(int track, int scene) {
    if (!doc.isValidCell(track, scene)) return;
    const auto index = static_cast<std::size_t>(track);
    if (engine.activePianoRollClip[index].load() == scene || engine.requestedPianoRollClip[index].load() == scene)
        publishClip(track, scene);
}

bool ProjectController::setStepOn(int step, bool on) {
    if (busy || !validStep(step)) return false;
    doc.steps.on[static_cast<std::size_t>(step)] = on;
    engine.steps[static_cast<std::size_t>(step)].store(on);
    return true;
}

bool ProjectController::setStepPitch(int step, int pitch) {
    if (busy || !validStep(step) || pitch < 0 || pitch > 127) return false;
    doc.steps.pitch[static_cast<std::size_t>(step)] = pitch;
    engine.stepPitches[static_cast<std::size_t>(step)].store(pitch);
    return true;
}

bool ProjectController::setStepVelocity(int step, int velocity) {
    if (busy || !validStep(step) || velocity < 1 || velocity > 127) return false;
    doc.steps.velocity[static_cast<std::size_t>(step)] = velocity;
    engine.stepVelocities[static_cast<std::size_t>(step)].store(velocity);
    return true;
}

bool ProjectController::hasWarpedAudio() const {
    return std::any_of(doc.timeline.regions.begin(),doc.timeline.regions.end(),[](const auto& r){return !r.warp.empty();});
}

bool ProjectController::tempoLocked() const {
    return hasWarpedAudio() && (isRecording() || engine.recording.phase.load()!=RecordControls::idle);
}

juce::String ProjectController::tempoEditRefusal(const tempo_edit::Edit& e) const {
    const bool meter=!tempo_edit::changesTempo(e);
    const bool map=!meter && e.kind!=tempo_edit::Kind::projectTempo;
    if(tempoLocked() || (map && isRecording()))return meter?"Stop recording before changing the time signature.":"Stop recording before changing the tempo.";
    return {};
}

juce::String ProjectController::songEditRefusal(const song_edit::Edit& e) const {
    if(song_edit::changesLoop(e) && tempoLocked() && !song_edit::changesNothing(doc,e))return "Stop recording before changing the loop while the song has warped audio.";
    if(song_edit::changesLength(e) && tempoLocked() && !song_edit::changesNothing(doc,e))return "Stop recording before changing the song length while the song has warped audio.";
    return {};
}

juce::String ProjectController::songClipRefusal(const song_clip::Edit& e) const {
    if(!isRecording() && engine.recording.phase.load()==RecordControls::idle)return {};
    if(song_clip::takeEdit(e.kind))return e.kind==song_clip::Kind::comp?"Stop recording before comping a take.":"Stop recording before forgetting a take.";
    if(e.kind==song_clip::Kind::moveSection)return "Stop recording before moving a section and its clips.";
    timeline::id_stream::Scope asking;   // a split or copy's new ids are the live edit's to take
    const auto o=song_clip::outcome(doc,e);
    if(!o.next)return {};
    const bool warp=hasWarpedAudio() || std::any_of(o.next->regions.begin(),o.next->regions.end(),[](const auto& r){return !r.warp.empty();});
    if(warp && !o.next->playbackEquals(doc.timeline))return "Stop recording before changing Song clips while the song has warped audio.";
    return {};
}

juce::String ProjectController::automationRefusal(const automation_edit::Edit& e) const {
    if(tempoLocked() && !automation_edit::changesNothing(doc,e))return "Stop recording before changing automation while the song has warped audio.";
    return {};
}

bool ProjectController::applyAutomationCommand(const automation_edit::Edit& e) {
    if(busy || automationRefusal(e).isNotEmpty())return false;
    return automation_edit::apply(doc,e).isEmpty();
}

bool ProjectController::applySongClipCommand(const song_clip::Edit& e) {
    if(busy || songClipRefusal(e).isNotEmpty())return false;
    return song_clip::apply(doc,e).isEmpty();
}

bool ProjectController::applySongCommand(const song_edit::Edit& e) {
    if(busy || songEditRefusal(e).isNotEmpty())return false;
    return song_edit::apply(doc,e).isEmpty();
}

bool ProjectController::applyTempoCommand(const tempo_edit::Edit& e) {
    if(busy || tempoEditRefusal(e).isNotEmpty() || tempo_edit::refusal(doc,e).isNotEmpty())return false;
    if(tempo_edit::changesNothing(doc,e))return true;
    const bool unfreezing=tempo_edit::changesTempo(e) && anyFrozenTrack();
    if(e.kind==tempo_edit::Kind::projectTempo && hasWarpedAudio()){engine.arrangementPreparing.store(true);engine.playing.store(false);}
    if(tempo_edit::apply(doc,e).isNotEmpty())return false;
    engine.bpm.store(doc.bpm);
    if(unfreezing){
        pushDocumentToEngine();
        status("Tracks unfrozen for tempo change.",{});
        if(onDocumentRestored)onDocumentRestored();
    }
    return true;
}

void ProjectController::auditionTempo(int bpm) {
    if(busy || tempoLocked())return;
    const int value=juce::jlimit(tempo_edit::minProjectBpm,tempo_edit::maxProjectBpm,bpm);
    if(value!=doc.bpm && hasWarpedAudio()){engine.arrangementPreparing.store(true);engine.playing.store(false);}
    engine.bpm.store(value);
}

void ProjectController::endTempoAudition() {
    engine.bpm.store(doc.bpm);
    if(engine.arrangementPreparing.load() && !warpWorkActive)publishArrangement();
}

bool ProjectController::setLivePreset(int preset) {
    if (busy || preset < 0 || preset >= presetCount) return false;
    doc.livePreset = preset;
    engine.preset.store(preset);
    return true;
}

bool ProjectController::setLaunchQuantization(int steps) {
    if (busy || (steps != 4 && steps != 16)) return false;
    doc.launchQuantizationSteps = steps;
    engine.launchQuantizationSteps.store(steps);
    return true;
}

void ProjectController::setClipPattern(int track, int scene, const PianoRollPattern& pattern, bool publishToEngine) {
    if (doc.isValidCell(track, scene) && doc.clip(track, scene).rackClip != 0) return;
    if (!doc.isValidCell(track, scene) || isTrackFrozen(track)) return;
    if (take.active && track == take.track && scene == take.scene) return; // the take owns this clip until it ends
    auto clip = doc.clip(track, scene);
    // Re-loading a clip into the editor reports identical notes; keep the
    // existing object so it does not look like an edit.
    if (samePianoRollPattern(*clip.pattern, pattern)) return;
    clip.pattern = std::make_shared<const PianoRollPattern>(pattern);
    doc.setClip(track, scene, clip);
    republishLinked(track, scene);   // cells linked to this one now play the new notes too
    if (!publishToEngine || clip.isAudioClip()) return;
    const auto index = static_cast<std::size_t>(track);
    engine.requestedPianoRollClip[index].store(scene);
    if (engine.activePianoRollClip[index].load() == scene) engine.pianoRollNotes[index].publish(pattern);
    else publishClip(track, scene);
}

juce::String ProjectController::clipNotesRefusal(const clip_notes::Edit& e) const {
    if (e.kind == clip_notes::Kind::cell && take.active && e.track == take.track && e.scene == take.scene)
        return "The take recording into this clip owns its notes until it ends.";
    return {};
}

bool ProjectController::applyClipNotesCommand(const clip_notes::Edit& e) {
    if (busy || clipNotesRefusal(e).isNotEmpty() || clip_notes::refusal(doc, e).isNotEmpty()) return false;
    if (clip_notes::changesNothing(doc, e)) return true;
    if (clip_notes::apply(doc, e).isNotEmpty()) return false;
    if (e.kind == clip_notes::Kind::placement) { publishArrangement(); return true; }
    republishLinked(e.track, e.scene);   // cells linked to this one now play the new notes too
    if (doc.clip(e.track, e.scene).isAudioClip()) return true;
    const auto index = static_cast<std::size_t>(e.track);
    engine.requestedPianoRollClip[index].store(e.scene);
    if (engine.activePianoRollClip[index].load() == e.scene) engine.pianoRollNotes[index].publish(*e.notes);
    else publishClip(e.track, e.scene);
    return true;
}

int ProjectController::placementTrack(std::uint64_t clip) const {
    for (const auto& r : doc.timeline.regions) if (clip != 0 && r.clip == clip) return r.row;
    return -1;
}

bool ProjectController::applyRemoveClipAudioCommand(int track, int scene) {
    if (!canEditMidiClips() || session_clip::removeAudioRefusal(doc,track,scene).isNotEmpty()) return false;
    if (!doc.clip(track,scene).isAudioClip()) return true;
    const auto link = doc.linkOf(track,scene);
    if (!session_clip::removeAudio(doc,track,scene)) return false;
    const bool shared = doc.linkOf(track,scene) == link;
    for (int s = 0; s < doc.sceneCount(); ++s) for (int t = 0; t < doc.trackCount(); ++t)
        if (((t == track && s == scene) || (shared && doc.linkOf(t,s) == link))
            && engine.requestedPianoRollClip[static_cast<std::size_t>(t)].load() == s) publishClip(t,s);
    return true;
}

void ProjectController::save(std::function<void(bool)> done) {
    if (projectFile.existsAsFile()) writeProject(projectFile, false, std::move(done));
    else chooseSavePath(false, std::move(done));
}

void ProjectController::chooseSavePath(bool portable, std::function<void(bool)> done) {
    projectFileChooser = std::make_unique<juce::FileChooser>("Save Alien Studio session",
        projectFile.getParentDirectory().exists()
            ? projectFile : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("AlienStudioSession.alienstudio"), "*.alienstudio");
    std::weak_ptr<int> token = alive;
    projectFileChooser->launchAsync(juce::FileBrowserComponent::saveMode
        | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, token, portable, done](const juce::FileChooser& chooser) {
            if (token.lock() == nullptr) return;
            if (chooser.getResult() != juce::File{}) writeProject(chooser.getResult(), portable, done);
            else if (done) done(false);
        });
}

void ProjectController::chooseProjectToOpen() {
    projectFileChooser = std::make_unique<juce::FileChooser>("Open Alien Studio session",
        projectFile.getParentDirectory().exists() ? projectFile : juce::File{}, "*.alienstudio");
    std::weak_ptr<int> token = alive;
    projectFileChooser->launchAsync(juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles,
        [this, token](const juce::FileChooser& chooser) {
            if (token.lock() != nullptr && chooser.getResult().existsAsFile())
                openProject(chooser.getResult());
        });
}

void ProjectController::chooseAudioFile(std::function<void(const juce::File&)> chosen) {
    audioFileChooser = std::make_unique<juce::FileChooser>("Import audio clip", juce::File{}, "*.wav;*.mp3");
    std::weak_ptr<int> token = alive;
    audioFileChooser->launchAsync(juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles,
        [token, chosen = std::move(chosen)](const juce::FileChooser& chooser) {
            if (token.lock() != nullptr && chooser.getResult().existsAsFile())
                chosen(chooser.getResult());
        });
}

void ProjectController::writeProject(juce::File file, bool portable, std::function<void(bool)> done) {
    if (busy) { if (done) done(false); return; }
    if (onBeforeSnapshot) onBeforeSnapshot();
    for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
    for (int r = 0; r < maxReturns; ++r) returnSettingsEdited(r);
    for (int b = 0; b < maxBuses; ++b) busSettingsEdited(b);
    masterGainEdited();
    const auto project = toProjectData(doc);

    if (!project.valid()) {
        const auto reason = describeInvalidSession(doc);
        status("Could not save: " + reason, reason);
        if (done) done(false);
        return;
    }
    history.breakCoalescing();
    const auto savingId = history.currentId();
    const auto target = file.hasFileExtension("alienstudio") ? file : file.withFileExtension("alienstudio");
    setBusy(true);
    status(portable ? ui8("Collecting audio\xe2\x80\xa6") : ui8("Saving project\xe2\x80\xa6"), {});
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, target, project, portable, savingId, done] {
        auto saved = std::make_shared<SessionProjectData>();
        juce::String error;
        const bool success = writeSessionProjectFile(target, project, portable, *saved, error);
        juce::MessageManager::callAsync([this, token, target, success, saved, project, error, savingId, done] {
            if (token.lock() == nullptr) return;
            setBusy(false);
            if (!success) { status("Save failed: " + error, error); if (done) done(false); return; }
            // Relocate every matching reference, including undo/redo and
            // edits made while saving. Track indices can change during IO.
            std::map<juce::String, juce::String> relocated;
            for (int scene = 0; scene < project.sceneCount(); ++scene)
                for (int track = 0; track < project.trackCount(); ++track) {
                    const auto& old = project.clips[scene][track].audioFilePath;
                    const auto& written = saved->clips[scene][track].audioFilePath;
                    if (old.isNotEmpty() && written.isNotEmpty())
                        relocated[old] = resolveProjectAudio(target, written).getFullPathName();
                }
            for (int t = 0; t < project.trackCount(); ++t) for (int pad = 0; pad < drumPadCount; ++pad)
                if (project.drumPaths[t][pad].isNotEmpty())
                    relocated[project.drumPaths[t][pad]] = resolveProjectAudio(target, saved->drumPaths[t][pad]).getFullPathName();
            std::map<std::string, std::string> relocatedRack;
            for (int c = 0; c < project.channelRack.channelCount; ++c) {
                const auto& old = project.channelRack.info[static_cast<std::size_t>(c)];
                if (!old.isFile()) continue;
                const auto& written = saved->channelRack.info[static_cast<std::size_t>(c)].sample;
                relocatedRack[old.sample] = resolveProjectAudio(target, juce::String::fromUTF8(written.c_str(), static_cast<int>(written.size()))).getFullPathName().toStdString();
            }
            for (const auto& [from, to] : relocatedRack)   // the decoded audio follows its new path
                if (auto clip = rackAudio.find(from); clip != rackAudio.end() && rackAudio.find(to) == rackAudio.end()) rackAudio[to] = clip->second;
            auto relocate = [&](ProjectDocument& document) {
                for (int c = 0; c < document.channelRack.channelCount; ++c) {
                    auto& sample = document.channelRack.info[static_cast<std::size_t>(c)].sample;
                    if (auto found = relocatedRack.find(sample); found != relocatedRack.end()) sample = found->second;
                }
                for (auto& track : document.tracks) for (auto& path : track.drumPaths)
                    if (auto found = relocated.find(path); found != relocated.end()) path = found->second;
                for (auto& [id, media] : document.media)   // a file moved, not new content
                    if (auto found = relocated.find(media.filePath); found != relocated.end())
                        media.filePath = found->second;
            };
            relocate(doc);
            history.relocateMedia(relocate);
            projectFile = target;
            savedId = savingId;
            if (!isDirty()) discardRecovery();
            status("Saved " + target.getFileName(), target.getFullPathName());
            historyChanged();
            if (done) done(true);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::openProject(const juce::File& file, bool recovered) {
    if (busy) return;
    setBusy(true);
    status(ui8("Opening project\xe2\x80\xa6"), {});
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, recovered] {
        auto result = std::make_shared<LoadResult>(loadProjectFile(file));
        juce::MessageManager::callAsync([this, token, file, result, recovered] {
            if (token.lock() != nullptr) finishLoad(file, *result, recovered);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

std::optional<session_audio_import::Plan> ProjectController::prepareAudioImport(const juce::File& file,int track,int scene,bool emptyOnly) const {
    if(!canEditMidiClips())return {};
    return session_audio_import::prepare(doc,file,track,scene,emptyOnly);
}

bool ProjectController::applyImportAudioCommand(const session_audio_import::Prepared& prepared) {
    if(!canEditMidiClips())return false;
    auto candidate=doc;
    if(session_audio_import::apply(candidate,prepared).isNotEmpty() || !retain(prepared.decoded.clip))return false;
    const auto [track,scene]=session_audio_import::resolve(doc,prepared.plan);
    doc=std::move(candidate);
    const auto link=doc.linkOf(track,scene);
    for(int s=0;s<doc.sceneCount();++s)for(int t=0;t<doc.trackCount();++t)
        if(((t==track && s==scene) || (link && doc.linkOf(t,s)==link))
            && engine.requestedPianoRollClip[static_cast<std::size_t>(t)].load()==s)publishClip(t,s);
    return true;
}

void ProjectController::decodeAudioImport(session_audio_import::Plan plan,std::function<void(const session_audio_import::Prepared&)> done) {
    setBusy(true);
    importStatus(ui8("Loading audio\xe2\x80\xa6"),plan.file.getFullPathName());
    std::weak_ptr<int> token=alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job=[this,token,plan=std::move(plan),done=std::move(done)] {
        auto prepared=std::make_shared<session_audio_import::Prepared>(session_audio_import::Prepared{plan,importSessionAudioClip(plan.file)});
        juce::MessageManager::callAsync([this,token,prepared,done] {
            if(token.lock()){setBusy(false);done(*prepared);}
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::audioImportFinished(const session_audio_import::Prepared& prepared,const juce::String& refused) {
    const auto path=prepared.plan.file.getFullPathName();
    if(refused.isNotEmpty()){importStatus(refused,path);return;}
    importStatus("Imported "+prepared.plan.file.getFileName(),path);
    const auto [track,scene]=session_audio_import::resolve(doc,prepared.plan);
    if(onClipContentChanged)onClipContentChanged(track,scene);
}

bool ProjectController::canImportBrowserSample(int track, int scene, bool song) const {
    return !busy && !isRecording() && engine.recording.phase.load() == RecordControls::idle
        && doc.isValidCell(track, scene) && !isTrackFrozen(track) && (song || !doc.cellClip(track, scene));
}

void ProjectController::decodeSongSample(const juce::File& file, int track, std::function<void(std::shared_ptr<SessionAudioClip>)> done) {
    if (busy || isRecording() || engine.recording.phase.load() != RecordControls::idle || track < 0 || track >= doc.trackCount() || isTrackFrozen(track)) return;
    setBusy(true); importStatus("Loading sample...", file.getFullPathName());
    std::weak_ptr<int> token = alive;
    pool.addJob([this, token, file, done = std::move(done)] {
        auto result = std::make_shared<AudioClipImportResult>(importSessionAudioClip(file));
        juce::MessageManager::callAsync([this, token, file, done, result] {
            if (!token.lock()) return;
            setBusy(false);
            if (!result->clip) { importStatus(result->error, file.getFullPathName()); return; }
            done(result->clip);
        });
    });
}

bool ProjectController::applySongAudioImport(const song_audio_import::Request& request) {
    if (busy || isRecording() || engine.recording.phase.load() != RecordControls::idle) return false;
    auto candidate = doc;
    if (song_audio_import::apply(candidate, request).isNotEmpty()) return false;
    if (candidate.sceneCount() != doc.sceneCount() && beforeStructureEdit) beforeStructureEdit();
    if (!retain(request.clip)) return false;
    doc = std::move(candidate);
    return true;
}

void ProjectController::songSampleImported(const juce::File& file, const juce::String& error) {
    if (error.isNotEmpty()) { importStatus(error, {}); return; }
    importStatus("Imported " + file.getFileName() + " to Song", file.getFullPathName());
    if (onDocumentRestored) onDocumentRestored();
}

std::shared_ptr<ArrangementPlayback> ProjectController::captureArrangement() const {
    auto song = std::make_shared<ArrangementPlayback>();
    song->timeline = doc.timeline; song->timeline.tempo = doc.bpm;
    for (int s = 0; s < doc.sceneCount(); ++s) for (int t = 0; t < doc.trackCount(); ++t) {
        // An offline audio clip stays silent; it must not fall back to playing stray notes.
        if (!doc.clip(t, s).audioOffline()) song->clips[s][t].pattern = doc.clip(t, s).pattern;   // shared, not copied
        song->clips[s][t].audio = doc.clip(t, s).audio.get();
    }
    for (const auto id : doc.placementClips()) song->placements.push_back({id, {doc.contentOf(alien::library::ClipId{id}).pattern, nullptr}});
    return song;
}

bool ProjectController::songDiffersFromPublished() const {
    if (!published.known || (published.bpm != doc.bpm && std::any_of(doc.timeline.regions.begin(),doc.timeline.regions.end(),[](const auto& r){return !r.warp.empty();})) || published.tracks != doc.trackCount() || published.scenes != doc.sceneCount()
        || !published.timeline.playbackEquals(doc.timeline)) return true;
    std::size_t index = 0;
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t, ++index) {
            const auto& clip = doc.clip(t, s);
            if (published.patterns[index] != clip.pattern || published.audio[index] != clip.audio) return true;
        }
    const auto ids = doc.placementClips();
    if (ids.size() != published.placements.size()) return true;
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (published.placements[i] != doc.contentOf(alien::library::ClipId{ids[i]}).pattern) return true;
    return false;
}

void ProjectController::rememberPublishedSong() {
    published.known = true; published.bpm = doc.bpm;
    published.timeline = doc.timeline;
    published.tracks = doc.trackCount();
    published.scenes = doc.sceneCount();
    published.patterns.clear(); published.audio.clear();
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t) {
            published.patterns.push_back(doc.clip(t, s).pattern);
            published.audio.push_back(doc.clip(t, s).audio);
        }
    published.placements.clear();
    for (const auto id : doc.placementClips()) published.placements.push_back(doc.contentOf(alien::library::ClipId{id}).pattern);
}

void ProjectController::publishArrangement() {
    if (warpWorkActive) return;
    if (!songDiffersFromPublished()) { if(!warpWorkActive)engine.arrangementPreparing.store(false); return; }
    auto song = captureArrangement();
    const bool warped = std::any_of(song->timeline.regions.begin(), song->timeline.regions.end(), [](const auto& r) { return !r.warp.empty(); });
    if (!warped) { if (engine.arrangementMailbox.publish(song)) rememberPublishedSong(); engine.arrangementPreparing.store(false); return; }
    // The document is committed immediately; edits wait while a private cache
    // is rendered. Never publish mismatched tempo/audio or do DSP in the GUI.
    engine.arrangementPreparing.store(true); engine.playing.store(false);
    setBusy(true); warpWorkActive=true; warpCancelled.store(false);
    status("Preparing warped audio...", "Press Play when ready; Escape cancels preparation.");
    const auto revision=history.currentId();
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, song, revision] {
        juce::String error;
        bool ok = false;
        try { ok = prepareArrangementWarps(*song, warpCancelled, error); }
        catch (...) { error = "Not enough resources to render warped audio."; }
        juce::MessageManager::callAsync([this, token, song, ok, error, revision] {
            if (!token.lock()) return;
            if(history.currentId()!=revision && !warpCancelled.load()) {
                warpWorkActive=false;setBusy(false);published.known=false;publishArrangement();return;
            }
            // MIDI edits may update a shared clip during a gesture before
            // its commit. Include those latest notes in the completed song.
            const auto current=captureArrangement();song->clips=current->clips;song->placements=current->placements;
            const bool cancelled=warpCancelled.load();
            const bool ready=ok && !cancelled;
            const juce::String problem=cancelled?"Warp preparation cancelled; markers kept. Use Rebuild warped audio to retry.":error;
            if (ready) {
                for (const auto& w : song->warped) engine.retainDiskAudioSource(w.owner->streamSource);
            } else {
                // A failed warp is silent, never silently played unstretched.
                for (auto& r : song->timeline.regions) if (!r.warp.empty() || std::any_of(song->warped.begin(),song->warped.end(),[&](const auto& w){return w.id==r.id;})) {r.muted = true;r.warp.clear();}
            }
            if (!engine.arrangementMailbox.publish(song)) {
                warpWorkActive=false;setBusy(false);status("Warp playback snapshot was rejected; Undo or rebuild before playing.",{});return;
            }
            rememberPublishedSong();
            warpWorkActive=false;
            engine.arrangementPreparing.store(false, std::memory_order_release);
            setBusy(false);
            status(ready ? "Warped audio ready" : "Warp render failed: " + problem, problem);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::detectRegionTransients(std::uint32_t id,WarpFound found) {
    const auto* r=doc.timeline.find(id);
    if(busy || isRecording() || engine.recording.phase.load()!=RecordControls::idle || !r || r->warp.empty() || !doc.isValidCell(r->row,r->source))return;
    const auto audio=doc.clip(r->row,r->source).audio;if(!audio)return;
    const auto map=r->warp;setBusy(true);warpWorkActive=true;warpCancelled.store(false);status("Detecting transients...","Escape cancels analysis.");
    std::weak_ptr<int> token=alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job=[this,token,id,audio,map,found=std::move(found)] {
        juce::String error;std::vector<timeline::WarpMarker> result;
        try {result=detectWarpTransients(*audio,map,warpCancelled,error);}catch(...){error="Transient analysis failed.";}
        juce::MessageManager::callAsync([this,token,id,result=std::move(result),error,found] {
            if(!token.lock())return;warpWorkActive=false;setBusy(false);
            if(warpCancelled.load()){status("Transient analysis cancelled",{});return;}
            if(result.size()>2 && error.isEmpty()){if(found)found(id,result);}
            else status(error.isEmpty()?"No additional transients found":error,{});
        });return juce::ThreadPoolJob::jobHasFinished;
    };pool.addJob(std::move(job));
}

std::optional<std::vector<timeline::WarpMarker>> ProjectController::fittedWarp(std::uint32_t id) const {
    const auto* r = doc.timeline.find(id);
    if (!r || r->isPattern() || !r->warp.empty() || !doc.isValidCell(r->row,r->source)) return std::nullopt;
    const auto audio=doc.clip(r->row,r->source).audio;
    if (!audio || audio->sampleRate <= 0) return std::nullopt;
    const double end=static_cast<double>(audio->frames)/audio->sampleRate;
    if (r->audioOffset >= end || r->offset > timeline::maxLength-r->length) return std::nullopt;
    return std::vector<timeline::WarpMarker>{{r->offset,r->audioOffset},{r->offset+r->length,end}};
}

bool ProjectController::editTimeline(const juce::String& label, const std::function<bool(alien::timeline::Timeline&)>& edit, const juce::String& coalesceKey) {
    if (busy) return false;
    auto next = doc.timeline;
    next.tempo = doc.bpm;
    if (!edit(next)) return false;
    const bool hasWarp=std::any_of(next.regions.begin(),next.regions.end(),[](const auto& r){return !r.warp.empty();})
        || std::any_of(doc.timeline.regions.begin(),doc.timeline.regions.end(),[](const auto& r){return !r.warp.empty();});
    if(hasWarp && (isRecording() || engine.recording.phase.load()!=RecordControls::idle) && !next.playbackEquals(doc.timeline)) return false;
    for (auto& r : next.regions) {
        if (r.ownsClip() && doc.library.clip(alien::library::ClipId{r.clip}) == nullptr) return false;
        if (r.isPattern()) {
            // Existing references own an ID. Only a newly placed, index-only
            // region crosses the UI compatibility boundary here.
            if (r.rackClip != 0) {
                r.source = doc.rackPatternForClip(r.rackClip);
                if (r.source < 0) return false;
            } else {
                if (const auto* existing = doc.timeline.find(r.id); existing != nullptr && existing->isPattern()) return false;
                if (!doc.channelRack.validPattern(r.source)) return false;
                r.rackClip = doc.channelRack.clips[static_cast<std::size_t>(r.source)];
            }
        }
    }
    if (!next.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount) || next == doc.timeline) return false;
    if(next.frozenOriginals!=doc.timeline.frozenOriginals)return false;
    // Send automation only for a return that exists.
    for(const auto& lane:next.automationLanes)
        if(const int send=automation::sendIndex(lane.parameter);send>=0&&!doc.returns[static_cast<std::size_t>(send)].used)return false;
    for(int track=0;track<doc.trackCount();++track)if(isTrackFrozen(track)) {
        std::vector<timeline::Region> before,after;
        for(const auto& region:doc.timeline.regions)if(region.row==track)before.push_back(region);
        for(const auto& region:next.regions)if(region.row==track)after.push_back(region);
        if(before!=after)return false;
    }
    if(anyFrozenTrack() && next.lengthBars!=doc.timeline.lengthBars)next.freezeBaseLengthBars=next.lengthBars;
    doc.timeline = std::move(next);
    doc.prune();   // a placement clip no region plays any more is gone (undo keeps it)
    commit(label, -1, -1, coalesceKey);
    return true;
}

std::int64_t ProjectController::naturalLength(int row, int source, std::int64_t start) const {
    return song_clip::naturalLength(doc, row, source, start);
}

bool ProjectController::applyNewSongClip(int row, std::int64_t start, std::uint32_t* idOut) {
    if (busy) return false;
    auto candidate = doc;
    if (song_placement::newClip(candidate, row, start, idOut).isNotEmpty()) return false;
    if (candidate.sceneCount() != doc.sceneCount() && beforeStructureEdit) beforeStructureEdit();
    doc = std::move(candidate);
    return true;
}

bool ProjectController::applyHarmonicMidiCommand(const song_placement::HarmonicMidi& edit) {
    return canEditMidiClips() && song_placement::applyHarmonicMidi(doc, edit).isEmpty();
}

int ProjectController::addRecordedTakes(const std::vector<RecordedTake>& takes, const juce::String& label) {
    using namespace alien::timeline;
    if (busy || takes.empty()) return 0;
    const auto saved = doc;
    const auto savedBytes = retainedBytes;
    const auto savedRetained = retained.size();
    const auto fail = [&] {
        doc = saved;
        while (retained.size() > savedRetained) retained.pop_back();
        retainedBytes = savedBytes;
        return 0;
    };
    auto next = doc.timeline;
    next.tempo = doc.bpm;
    std::vector<std::pair<int, int>> cells;
    for (const auto& take : takes) {
        const int row = take.track;
        if (isTrackFrozen(row) || row < 0 || row >= doc.trackCount() || !take.clip || !take.placement.valid) return fail();
        const auto usedByTimeline = [&](int scene) {
            for (const auto& r : next.takes) if (r.row == row && r.source == scene) return true;
            for (const auto& r : next.regions) if (r.row == row && r.source == scene) return true;
            return false;
        };
        int scene = -1;
        for (int s = 0; s < doc.sceneCount() && scene < 0; ++s)
            if (!doc.cellClip(row, s) && !usedByTimeline(s)) scene = s;
        if (scene < 0) {
            if (!doc.canAddScene() || !insertScene(doc.sceneCount(), nullptr)) return fail();
            scene = doc.sceneCount() - 1;
        }
        if (!retain(take.clip)) return fail();
        auto clip = doc.clip(row, scene);
        clip.audio = take.clip;
        clip.audioFileName = take.file.getFileName();
        clip.audioFilePath = take.file.getFullPathName();
        doc.setClip(row, scene, clip);
        std::vector<Region> passes;
        for (const auto& placement : take.passes.empty() ? std::vector<alien::TakePlacement>{take.placement} : take.passes) {
            Region region;
            region.row = row; region.source = scene;
            region.start = placement.start; region.length = placement.length;
            region.audioOffset = placement.audioOffset;
            passes.push_back(region);
        }
        std::size_t active = take.passes.empty() ? 0 : std::min(take.activePass, passes.size() - 1);
        const auto room = timeline::maxTakes - std::min(timeline::maxTakes, next.takes.size());
        if (passes.size() > room && room > 0) {
            const auto drop = passes.size() - room;
            passes.erase(passes.begin(), passes.begin() + static_cast<std::ptrdiff_t>(drop));
            active = active >= drop ? active - drop : passes.size() - 1;
        }
        if (!next.recordTakes(std::move(passes), active)) return fail();
        cells.emplace_back(row, scene);
    }
    if (!next.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) return fail();
    doc.timeline = std::move(next);
    if (!cells.empty()) selectClip(cells.front().first, cells.front().second);
    commit(label, cells.size() == 1 ? cells.front().first : -1, cells.size() == 1 ? cells.front().second : -1);
    return static_cast<int>(cells.size());
}

bool ProjectController::isTrackFrozen(int track) const noexcept {
    return track>=0 && track<doc.trackCount() && doc.tracks[static_cast<std::size_t>(track)].settings.frozen();
}

bool ProjectController::anyFrozenTrack() const noexcept {
    for(const auto& track:doc.tracks) if(track.settings.frozen())return true;
    return false;
}

juce::String ProjectController::freezeRefusal(int track) const {
    if (!canEditMidiClips()) return "Finish the current operation before freezing.";
    return freeze_edit::freezeRefusal(doc, track);
}

bool ProjectController::applyFreezeTrackCommand(const freeze_edit::Install& edit) {
    if (!canEditMidiClips()) return false;
    auto candidate = doc;
    if (freeze_edit::install(candidate, edit, ToneEngine::audioClipLevel).isNotEmpty() || !retain(edit.audio)) return false;
    doc = std::move(candidate);
    pushDocumentToEngine();
    return true;
}

bool ProjectController::applyUnfreezeCommand(int track) {
    if(!canEditMidiClips())return false;
    auto candidate=doc;
    if(freeze_edit::unfreeze(candidate,track).isNotEmpty())return false;
    doc=std::move(candidate);
    pushDocumentToEngine();
    if(onDocumentRestored)onDocumentRestored();
    return true;
}

DrumSliceTarget ProjectController::drumSliceTarget(int track, int pad) const {
    if (track < 0 || track >= doc.trackCount() || pad < 0 || pad >= drumPadCount) return {};
    const auto& owner = doc.tracks[track];
    return {doc.identityScope, owner.id, pad, owner.drumAssets[pad], owner.settings.frozen()};
}

bool ProjectController::applyDrumPadCommand(const drum_pad::Edit& e) {
    if (busy || ((e.kind == drum_pad::Kind::range || e.kind == drum_pad::Kind::slice) && !drumSliceOpen())) return false;
    if (e.kind == drum_pad::Kind::clip && !canEditMidiClips()) return false;
    auto candidate = doc;
    if (drum_pad::apply(candidate, e).isNotEmpty()) return false;
    if (e.kind == drum_pad::Kind::load && !retain(e.clip)) return false;
    // Slice views own no PCM. Keep them alive past any in-flight callback, as with loaded samples;
    // their source was counted and retained when it was loaded.
    const auto& was = doc.tracks[static_cast<std::size_t>(e.track)].drumAssets;
    const auto& now = candidate.tracks[static_cast<std::size_t>(e.track)].drumAssets;
    for (std::size_t p = 0; p < now.size(); ++p) if (now[p] && now[p]->sliceSource && now[p] != was[p]) retained.push_back(now[p]);
    doc = std::move(candidate);
    pushDocumentToEngine();
    return true;
}

juce::String ProjectController::commitDrumPad(const drum_pad::Edit& e, const juce::String& label) {
    if (drumPadCommands) return drumPadCommands(e, label);
    return "Drum pad command route is not configured.";
}

int ProjectController::drumTargetTrack(const DrumSliceTarget& target) const {
    if (target.scope != doc.identityScope || !target.asset || target.frozen || target.pad < 0 || target.pad >= drumPadCount) return -1;
    for (int t = 0; t < doc.trackCount(); ++t)
        if (doc.tracks[t].id == target.trackId) return !isTrackFrozen(t) && doc.tracks[t].drumAssets[target.pad] == target.asset ? t : -1;
    return -1;
}

bool ProjectController::editDrumSlice(const DrumSliceTarget& target, DrumSlice range) {
    if (busy || !drumSliceOpen() || !range.valid()) return false;
    const int track = drumTargetTrack(target);
    if (track < 0) return false;
    const auto source = target.asset->sliceSource ? target.asset->sliceSource : target.asset;
    const bool whole = range.whole() || (range.start == 0 && static_cast<std::size_t>(range.end) == source->frames);
    const auto slices = doc.tracks[track].drumSlices;
    drum_pad::Edit e; e.kind = drum_pad::Kind::range; e.track = track; e.pad = target.pad; e.range = range;
    if (commitDrumPad(e, whole ? "Restore whole drum sample" : "Edit drum slice").isNotEmpty()) return false;
    if (doc.tracks[track].drumSlices != slices && onDocumentRestored) onDocumentRestored();
    return true;
}

bool ProjectController::autoSliceDrum(const DrumSliceTarget& target) {
    if (busy || !drumSliceOpen()) return false;
    const int track = drumTargetTrack(target);
    if (track < 0) return false;
    const auto& owner = doc.tracks[track];
    if (owner.drumPaths[target.pad].isEmpty()) return false;
    if (target.asset->streamed() || target.asset->frames / target.asset->sampleRate > 60) {
        importStatus("Auto slice needs a decoded sample range of at most one minute.", {}); return false;
    }
    const auto assets = owner.drumAssets; const auto ranges = owner.drumSlices;
    const auto paths = owner.drumPaths; const auto instrument = owner.settings.instrument;
    setBusy(true); importStatus("Finding sample attacks...", {});
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, target, assets, ranges, paths, instrument] {
        const auto detected = detectDrumTransientSlices(*target.asset);
        juce::MessageManager::callAsync([this, token, target, assets, ranges, paths, instrument, detected] {
            if (!token.lock()) return;
            setBusy(false);
            if (target.scope != doc.identityScope || isRecording() || engine.recording.active()) {
                importStatus("Auto slice discarded: project or recording state changed.", {}); return;
            }
            for (int t = 0; t < doc.trackCount(); ++t) if (doc.tracks[t].id == target.trackId) {
                const auto& kit = doc.tracks[t];
                if (isTrackFrozen(t) || kit.drumAssets != assets || kit.drumSlices != ranges
                    || kit.drumPaths != paths || kit.settings.instrument != instrument) break;
                if (detected.count < 2) {
                    importStatus("No useful attack boundaries found. Kit unchanged; try equal slicing or edit ranges.", {}); return;
                }
                // The detected slices follow one another, inside the range the pad plays.
                const auto offset = static_cast<std::int64_t>(target.asset->sliceSource ? target.asset->sliceOffset : 0);
                drum_pad::Edit e; e.kind = drum_pad::Kind::slice; e.track = t; e.pad = target.pad;
                e.bounds.push_back(offset + detected.ranges[0].start);
                for (int p = 0; p < detected.count; ++p) e.bounds.push_back(offset + detected.ranges[p].end);
                if (const auto why = commitDrumPad(e, "Auto slice sample to pads"); why.isNotEmpty()) { importStatus(why, {}); return; }
                if (onDocumentRestored) onDocumentRestored();
                importStatus("Found " + juce::String(detected.count) + " slices. Unused pads cleared; Undo restores the kit.", {});
                return;
            }
            importStatus("Auto slice discarded: the target kit changed.", {});
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
    return true;
}

bool ProjectController::sliceDrum(int track, int pad) {
    if (busy || !drumSliceOpen() || isTrackFrozen(track) || track < 0 || track >= doc.trackCount() || pad < 0 || pad >= drumPadCount) return false;
    const auto& owner = doc.tracks[track];
    const auto selected = owner.drumAssets[pad];
    if (!canSliceDrumSource(selected, owner.drumPaths[pad])) {
        importStatus("Load a sample with at least eight frames before slicing.", {}); return false;
    }
    // Eight equal parts of the range the pad plays.
    const auto offset = selected->sliceSource ? selected->sliceOffset : std::size_t{};
    drum_pad::Edit e; e.kind = drum_pad::Kind::slice; e.track = track; e.pad = pad;
    for (int p = 0; p <= drumPadCount; ++p) e.bounds.push_back(static_cast<std::int64_t>(offset + selected->frames * static_cast<std::size_t>(p) / drumPadCount));
    if (const auto why = commitDrumPad(e, "Slice sample to 8 pads"); why.isNotEmpty()) { importStatus(why, {}); return false; }
    if (onDocumentRestored) onDocumentRestored();
    importStatus("Sample split into eight pads. Undo restores the previous kit.", {});
    return true;
}

bool ProjectController::canSliceSessionAudio(int track, int scene) const {
    if (!doc.isValidCell(track, scene) || !canEditMidiClips() || isTrackFrozen(track)) return false;
    const auto clip = doc.clip(track, scene);
    return canSliceDrumSource(clip.audio, clip.audioFilePath);
}

void ProjectController::sessionAudioSliced() {
    if (onDocumentRestored) onDocumentRestored();
    importStatus("Sample split into eight pads. Undo restores the previous kit.", {});
}

bool ProjectController::sliceSessionAudio(int track, int scene) {
    if (!canSliceSessionAudio(track, scene)) return false;
    drum_pad::Edit e; e.kind = drum_pad::Kind::clip; e.track = track; e.scene = scene;
    if (commitDrumPad(e, "Slice sample to 8 pads").isNotEmpty()) return false;
    sessionAudioSliced();
    return true;
}

bool ProjectController::canSliceDrumSource(const std::shared_ptr<const SessionAudioClip>& audio, const juce::String& path) {
    return audio && !audio->streamed() && audio->frames >= drumPadCount && path.isNotEmpty();
}

void ProjectController::loadDrum(const juce::File& file, int track, int pad) {
    if(isTrackFrozen(track)){importStatus("Unfreeze the track before changing its samples.",{});return;}
    if (busy || track < 0 || track >= doc.trackCount() || pad < 0 || pad >= drumPadCount) return;
    const auto trackId = doc.tracks[track].id;
    setBusy(true);
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, trackId, pad] {
        auto result = std::make_shared<AudioClipImportResult>(decodeSessionAudioClip(file));
        juce::MessageManager::callAsync([this, token, file, trackId, pad, result] {
            if (!token.lock()) return;
            setBusy(false);
            for (int t = 0; t < doc.trackCount(); ++t) if (doc.tracks[t].id == trackId) {
                drum_pad::Edit e; e.kind = drum_pad::Kind::load; e.track = t; e.pad = pad; e.path = file.getFullPathName(); e.clip = result->clip;
                if (!result->clip || commitDrumPad(e, "Load drum sample").isNotEmpty()) { importStatus("Could not load drum sample (format or memory limit)", {}); return; }
                if (onDocumentRestored) onDocumentRestored();
                return;
            }
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

std::array<const SessionAudioClip*, rack::maxChannels> ProjectController::rackClipsForExport(std::vector<std::shared_ptr<const SessionAudioClip>>& keepAlive) const {
    std::array<const SessionAudioClip*, rack::maxChannels> clips{};
    for (int c = 0; c < doc.channelRack.channelCount; ++c) {
        const auto& info = doc.channelRack.info[static_cast<std::size_t>(c)];
        if (info.isFile()) {
            auto asset = rackAsset(info);
            if (asset == nullptr) continue;
            clips[static_cast<std::size_t>(c)] = asset.get();
            keepAlive.push_back(std::move(asset));
        } else clips[static_cast<std::size_t>(c)] = rackClip(info);
    }
    return clips;
}

bool ProjectController::editRack(const juce::String& label, const std::function<bool(rack::Project&)>& edit, const juce::String& coalesceKey) {
    auto next = doc.channelRack;
    if (!edit(next)) return false;
    freshPluginInstances(next);
    if (!next.valid() || next == doc.channelRack) return false;
    for(int c=0;c<next.channelCount;++c)if(isTrackFrozen(next.channels[static_cast<std::size_t>(c)].output))return false;
    const bool content = !next.sameContent(doc.channelRack);
    const bool fewerPatterns = next.patternCount < doc.channelRack.patternCount;
    if (!doc.setRack(std::move(next))) return false;   // the library takes the new content
    if (fewerPatterns) doc.timeline.prune(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount);
    pushRackToEngine();
    if (content) commit(label, -1, -1, coalesceKey);
    notifyRackPlugins();
    return true;
}

juce::String ProjectController::rackSliceRefusal(const rack_channel::Edit& e) const {
    const bool keys = e.kind == rack_channel::Kind::keys && !e.bounds.empty();
    if ((e.kind != rack_channel::Kind::slice && !keys) || !doc.channelRack.validChannel(e.channel)) return {};
    const auto& info = doc.channelRack.info[static_cast<std::size_t>(e.channel)];
    if (!info.isFile()) return {};   // the reducer says why
    const auto found = rackAudio.find(info.sample);
    if (found == rackAudio.end()) return "Load the channel's sample before slicing it.";
    if (found->second->streamed() || found->second->sliceSource) return "That sample cannot be sliced.";
    const auto frames = static_cast<std::int64_t>(found->second->frames);
    if (frames > std::numeric_limits<int>::max()) return "That sample is too long to slice.";
    if (!e.bounds.empty() && e.bounds.back() > frames) return "The slices run past the end of the sample (" + juce::String(frames) + " frames).";
    return {};
}

bool ProjectController::findRackAttacks(int channel, std::function<void(const std::vector<std::int64_t>&)> found, bool addsChannels) {
    if (busy || isRecording() || engine.recording.active() || !found || !doc.channelRack.validChannel(channel)) return false;
    const auto info = doc.channelRack.info[static_cast<std::size_t>(channel)];
    if (!info.isFile() || doc.channelRack.channels[static_cast<std::size_t>(channel)].isInstrument()) return false;
    const auto asset = rackRangeAsset(info);
    if (!asset) { importStatus("Load the channel's sample before slicing it.", {}); return false; }
    if (asset->streamed() || asset->frames / asset->sampleRate > 60) {
        importStatus("Attack slicing needs a decoded sample range of at most one minute.", {}); return false;
    }
    const int room = addsChannels ? std::min(rack_channel::maxSlices, rack::maxChannels - doc.channelRack.channelCount) : rack::maxKeyZones;
    if (room < 2) { importStatus("The Channel Rack has no room for two more channels.", {}); return false; }
    const auto scope = doc.identityScope;
    setBusy(true); importStatus("Finding sample attacks...", {});
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, channel, info, asset, room, scope, found, addsChannels] {
        auto bounds = detectAttackBounds(*asset, room);
        const auto offset = asset->sliceSource ? static_cast<std::int64_t>(asset->sliceOffset) : std::int64_t{};
        for (auto& edge : bounds) edge += offset;
        juce::MessageManager::callAsync([this, token, channel, info, asset, scope, found, bounds, addsChannels] {
            if (!token.lock()) return;
            setBusy(false);
            if (scope != doc.identityScope || isRecording() || engine.recording.active()) {
                importStatus("Attack slicing discarded: project or recording state changed.", {}); return;
            }
            const auto& rack = doc.channelRack;
            if (!rack.validChannel(channel) || !(rack.info[static_cast<std::size_t>(channel)] == info) || rackRangeAsset(info) != asset
                || (addsChannels && rack.channelCount + static_cast<int>(bounds.size()) - 1 > rack::maxChannels)) {
                importStatus("Attack slicing discarded: the channel changed.", {}); return;
            }
            if (bounds.size() < 3) {
                importStatus("No useful attack boundaries found. Channels unchanged; try equal slices.", {}); return;
            }
            importStatus("Found " + juce::String(static_cast<int>(bounds.size()) - 1) + " slices at attacks.", {});
            found(bounds);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
    return true;
}

bool ProjectController::applyRackChannelCommand(const rack_channel::Edit& e) {
    if (rackSliceRefusal(e).isNotEmpty()) return false;
    if (busy || rack_channel::apply(doc, e).isNotEmpty()) return false;
    pushRackToEngine();
    return true;
}

bool ProjectController::applyRackPatternCommand(const rack_pattern::Edit& e) {
    if (busy || rack_pattern::apply(doc, e).isNotEmpty()) return false;
    pushRackToEngine();
    return true;
}

bool ProjectController::applyRackSampleCommand(const rack_channel::Edit& e, const std::shared_ptr<SessionAudioClip>& clip) {
    if (busy || clip == nullptr || rack_channel::refusal(doc, e).isNotEmpty() || !retain(clip)) return false;
    if (rack_channel::apply(doc, e).isNotEmpty()) return false;
    rackAudio[e.description.toStdString()] = clip;
    pushRackToEngine();
    return true;
}

bool ProjectController::replaceRackSampleAudio(const std::string& path, const std::shared_ptr<SessionAudioClip>& clip) {
    if (clip == nullptr || !retain(clip)) return false;
    rackAudio[path] = clip;
    pushRackToEngine();
    return true;
}

void ProjectController::setRackPluginStates(const std::vector<std::pair<std::uint64_t, juce::String>>& states) {
    auto next = doc.channelRack;
    bool changed = false;
    for (const auto& [instance, state] : states) changed = next.setPluginState(instance, state.toStdString()) || changed;
    if (!changed || !next.valid() || !doc.setRack(std::move(next))) return;
    runtimeDirty = true;
    pushRackToEngine();
}

std::vector<PluginState> ProjectController::rackPlugins() const {
    std::vector<PluginState> plugins;
    for (int c = 0; c < doc.channelRack.channelCount; ++c) {
        const auto& info = doc.channelRack.info[static_cast<std::size_t>(c)];
        PluginState plugin;
        if (info.hasPlugin()) {
            plugin.descriptionXml = juce::String::fromUTF8(info.pluginDescription().c_str());
            plugin.stateBase64 = juce::String::fromUTF8(info.pluginState().c_str());
            plugin.instanceId = info.pluginInstance();
        }
        plugins.push_back(std::move(plugin));
    }
    return plugins;
}

bool ProjectController::selectRackPattern(int index) {
    if (!doc.channelRack.selectPattern(index)) return false;
    pushRackToEngine();
    return true;
}

void ProjectController::previewRackChannel(int channel, float velocity, int pitch, bool held) {
    if (doc.channelRack.validChannel(channel))
        engine.rackAuditions.push(ToneEngine::rackAuditionEvent(channel, std::clamp(pitch, -rack::channelRootPitch, 127 - rack::channelRootPitch), held), velocity);
}

void ProjectController::decodeRackSample(const juce::File& file, std::function<void(const AudioClipImportResult&)> done) {
    setBusy(true);
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, done = std::move(done)] {
        auto result = std::make_shared<AudioClipImportResult>(decodeSessionAudioClip(file));
        juce::MessageManager::callAsync([this, token, result, done] {
            if (token.lock()) { setBusy(false); done(*result); }
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::rackSampleFinished(const juce::File& file, const juce::String& refused) {
    importStatus(refused.isNotEmpty() ? refused : "Loaded " + file.getFileName(), file.getFullPathName());
    if (refused.isEmpty() && onDocumentRestored) onDocumentRestored();
}

const SessionAudioClip* ProjectController::rackChannelAudio(int channel) const {
    return doc.channelRack.validChannel(channel) ? rackClip(doc.channelRack.info[static_cast<std::size_t>(channel)]) : nullptr;
}

bool ProjectController::rackChannelOffline(int channel) const {
    if (!doc.channelRack.validChannel(channel)) return false;
    const auto& info = doc.channelRack.info[static_cast<std::size_t>(channel)];
    return info.isFile() && rackAudio.find(info.sample) == rackAudio.end();
}


std::vector<MissingMedia> ProjectController::missingMedia() const {
    std::vector<MissingMedia> result;
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t) {
            const auto& clip = doc.clip(t, s);
            if (clip.audioOffline()) result.push_back({false, t, s, clip.audioFilePath, {}});
        }
    for (int t = 0; t < doc.trackCount(); ++t)
        for (int p = 0; p < drumPadCount; ++p) {
            const auto& track = doc.tracks[static_cast<std::size_t>(t)];
            if (track.drumPaths[static_cast<std::size_t>(p)].isNotEmpty() && track.drumAssets[static_cast<std::size_t>(p)] == nullptr)
                result.push_back({true, t, p, track.drumPaths[static_cast<std::size_t>(p)], {}});
        }
    std::set<std::string> listed; // one entry per missing file, however many channels use it
    for (int c = 0; c < doc.channelRack.channelCount; ++c) {
        const auto& info = doc.channelRack.info[static_cast<std::size_t>(c)];
        if (info.isFile() && rackAudio.find(info.sample) == rackAudio.end() && listed.insert(info.sample).second)
            result.push_back({false, c, 0, juce::String::fromUTF8(info.sample.c_str(), static_cast<int>(info.sample.size())), {}, true});
    }
    return result;
}

void ProjectController::relinkMissingAudio(const juce::File& folder, RelinkCompletion complete) {
    if (!canEditMidiClips()) {
        importStatus("Wait until recording or project work finishes.", {});
        return;
    }
    const auto missing = missingMedia();
    if (missing.empty()) { importStatus("No audio is missing.", {}); return; }
    if (!folder.isDirectory()) { importStatus("Choose a folder to search for the missing audio.", {}); return; }
    auto prepared = std::make_shared<PreparedRelink>();
    prepared->scope = doc.identityScope;
    prepared->folder = folder;
    for (const auto& item : missing) {
        RelinkTarget target;
        target.item = item;
        if (!item.rackChannel) {
            target.trackId = doc.tracks[static_cast<std::size_t>(item.track)].id;
            if (!item.drumPad) {
                target.sceneId = doc.scenes[static_cast<std::size_t>(item.slot)].id;
                target.cell = doc.cellClip(item.track, item.slot);
            }
        }
        prepared->targets.push_back(std::move(target));
    }
    setBusy(true);
    importStatus(ui8("Searching for missing audio\xe2\x80\xa6"), folder.getFullPathName());
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, prepared, complete = std::move(complete)] {
        const MediaIndex index(prepared->folder);
        prepared->truncated = index.wasTruncated();
        // A source used by several clips is decoded once and shared; decoded audio is immutable.
        std::map<juce::String, RelinkedAudio> decodedByPath;
        for (auto& target : prepared->targets) {
            auto found = decodedByPath.find(target.item.path);
            if (found == decodedByPath.end())
                found = decodedByPath.emplace(target.item.path, decodeFirstCandidate(index.candidates(target.item.path))).first;
            target.found = found->second;
        }
        juce::MessageManager::callAsync([this, token, prepared, complete] {
            if (token.lock() == nullptr) return;
            setBusy(false);
            complete(prepared);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

std::shared_ptr<const SessionAudioClip> ProjectController::rackAsset(const rack::ChannelInfo& info) const {
    auto base = rackRangeAsset(info);
    if (base == nullptr || !info.hasZones()) return base;
    const auto& file = rackAudio.find(info.sample)->second;
    for (const auto& made : rackKeyMaps)
        if (made.file == file && made.base == base && made.zones == *info.zones) return made.map;
    auto zones = std::make_shared<std::vector<SampleKeyZone>>();
    for (const auto& z : *info.zones) {
        rack::ChannelInfo part;
        part.sample = info.sample; part.sliceStart = z.start; part.sliceEnd = z.end;
        auto view = z.start == 0 && z.end == static_cast<std::int64_t>(file->frames) ? file : rackRangeAsset(part);
        if (view == nullptr) return {};   // a zone past the end of its file plays nothing
        zones->push_back({z.low, z.high, z.root, std::move(view)});
    }
    auto map = std::make_shared<SessionAudioClip>();
    map->sampleRate = base->sampleRate; map->channels = base->channels; map->frames = base->frames;
    map->waveform = base->waveform; map->waveformCount = base->waveformCount;
    map->keyZones = std::move(zones);
    rackKeyMaps.push_back({file, base, *info.zones, map});
    return map;
}

std::shared_ptr<const SessionAudioClip> ProjectController::rackRangeAsset(const rack::ChannelInfo& info) const {
    if (info.isBuiltin()) return rack::builtinClip(info.builtinId());
    if (!info.isFile()) return {};
    const auto found = rackAudio.find(info.sample);
    if (found == rackAudio.end()) return {};
    if (info.wholeSample()) return found->second;
    const auto key = std::make_tuple(info.sample, info.sliceStart, info.sliceEnd);
    if (const auto view = rackSliceViews.find(key); view != rackSliceViews.end() && view->second->sliceSource == found->second) return view->second;
    if (info.sliceEnd > static_cast<std::int64_t>(found->second->frames) || info.sliceEnd > std::numeric_limits<int>::max()) return {};
    auto view = drumSliceView(found->second, {static_cast<int>(info.sliceStart), static_cast<int>(info.sliceEnd)});
    if (view == nullptr || view == found->second) return {};
    rackSliceViews[key] = view;
    retainedSliceViews.push_back(view);   // a replaced view may still be playing
    return view;
}

std::uint64_t ProjectController::freshInstanceId() {
    std::uint64_t id = 0;
    while (id == 0) id = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64());
    return id;
}

void ProjectController::freshPluginInstances(rack::Project& p) {
    for (int c = 0; c < p.channelCount; ++c) {
        const auto& info = p.info[static_cast<std::size_t>(c)];
        if (!info.hasPlugin()) continue;
        for (int d = 0; d < c; ++d)
            if (p.info[static_cast<std::size_t>(d)].pluginInstance() == info.pluginInstance()) { p.renewPluginInstance(c, freshInstanceId()); break; }
    }
}

void ProjectController::notifyRackPlugins() {
    // One number for which channel holds which instrument, rather than a copy of every identity.
    std::uint64_t now = 1469598103934665603ull;
    for (int c = 0; c < doc.channelRack.channelCount; ++c)
        now = (now ^ doc.channelRack.info[static_cast<std::size_t>(c)].pluginInstance()) * 1099511628211ull + static_cast<std::uint64_t>(c);
    if (now == notifiedRackPlugins) return;
    notifiedRackPlugins = now;
    if (onRackPluginsChanged) onRackPluginsChanged();
}

void ProjectController::pushRackToEngine() {
    std::array<const SessionAudioClip*, rack::maxChannels> clips{};
    for (int c = 0; c < doc.channelRack.channelCount; ++c)
        clips[static_cast<std::size_t>(c)] = rackClip(doc.channelRack.info[static_cast<std::size_t>(c)]);
    if (rackPublished.known && rackPublished.clips == clips && rackPublished.project == doc.channelRack) return;
    for (std::size_t c = 0; c < clips.size(); ++c) engine.rackSamples[c].store(clips[c]);
    // The edit has happened; if its channels' hit queue cannot be reserved, say so. The rack still
    // plays, and hits beyond the room it has are counted rather than kept (T1-04 D8).
    if (!engine.prepareRackChannels(doc.channelRack.channelCount))
        status("Not enough memory for " + juce::String(doc.channelRack.channelCount) + " Channel Rack channels",
               "Hits that wait for their time (nudges, ratchets) on channels past " + juce::String(engine.preparedRackChannels())
               + " may not play. Close other programs, save, and reopen the project.");
    engine.rackMailbox.publish(doc.channelRack.snapshot());
    rackPublished.known = true; rackPublished.clips = clips; rackPublished.project = doc.channelRack;
}

bool ProjectController::retain(const std::shared_ptr<SessionAudioClip>& clip) {
    const auto bytes = clip->decodedBytes();
    if (bytes > maxSessionAudioBytes - retainedBytes) return false;
    if (clip->streamed() && !engine.retainDiskAudioSource(clip->streamSource)) return false;
    retained.push_back(clip); retainedBytes += bytes; return true;
}

ProjectController::ReplacedAudio ProjectController::setAsideRetainedAudio(std::vector<std::shared_ptr<const SessionAudioClip>> dropped) {
    using Clip = const SessionAudioClip*;
    std::map<Clip, long> listed;                                       // times in `retained` or `dropped`
    std::map<Clip, const std::shared_ptr<const SessionAudioClip>*> owner;   // one of those, for use_count
    for (const auto* list : {&retained, &dropped})
        for (const auto& clip : *list) { ++listed[clip.get()]; owner.emplace(clip.get(), &clip); }
    // Start from "all of them go" and take back, until nothing changes, any clip with a reference from outside.
    std::set<Clip> going;
    for (const auto& [clip, times] : listed) going.insert(clip);
    for (bool changed = true; changed;) {
        changed = false;
        std::map<Clip, long> inner;   // references held by clips that are going
        for (const auto* clip : going) {
            if (clip->sliceSource) ++inner[clip->sliceSource.get()];
            if (clip->keyZones) for (const auto& zone : *clip->keyZones) if (zone.clip) ++inner[zone.clip.get()];
        }
        for (auto it = going.begin(); it != going.end();) {
            const auto found = inner.find(*it);
            const long internal = listed[*it] + (found == inner.end() ? 0 : found->second);
            if (owner[*it]->use_count() != internal) { it = going.erase(it); changed = true; } else ++it;
        }
    }
    ReplacedAudio out;
    std::vector<std::shared_ptr<const SessionAudioClip>> kept;
    for (auto& clip : retained) {
        if (!going.contains(clip.get())) { kept.push_back(std::move(clip)); continue; }
        out.bytes += clip->decodedBytes();
        out.clips.push_back(std::move(clip));
    }
    for (auto& clip : dropped) out.clips.push_back(std::move(clip));
    out.bytes = std::min(out.bytes, retainedBytes);
    retained = std::move(kept);
    return out;
}

void ProjectController::addRackFiles(const rack::Project& rackProject, std::set<std::string>& files) {
    for (int c = 0; c < rackProject.channelCount; ++c)
        if (const auto& info = rackProject.info[static_cast<std::size_t>(c)]; info.isFile()) files.insert(info.sample);
}

std::vector<std::shared_ptr<const SessionAudioClip>> ProjectController::forgetUnusedRackAudio(std::set<std::string> used) {
    addRackFiles(doc.channelRack, used);
    std::vector<std::shared_ptr<const SessionAudioClip>> dropped;
    for (auto it = rackAudio.begin(); it != rackAudio.end();) {
        if (used.contains(it->first)) { ++it; continue; }
        dropped.push_back(std::move(it->second)); it = rackAudio.erase(it);
    }
    const auto current = [&](const std::string& path, const std::shared_ptr<const SessionAudioClip>& file) {
        const auto found = rackAudio.find(path);
        return found != rackAudio.end() && found->second == file;
    };
    for (auto it = rackSliceViews.begin(); it != rackSliceViews.end();) {
        if (current(std::get<0>(it->first), it->second->sliceSource)) { ++it; continue; }
        dropped.push_back(std::move(it->second)); it = rackSliceViews.erase(it);
    }
    std::set<const SessionAudioClip*> files;
    for (const auto& [path, file] : rackAudio) files.insert(file.get());
    for (auto it = rackKeyMaps.begin(); it != rackKeyMaps.end();) {
        if (files.contains(it->file.get())) { ++it; continue; }
        dropped.push_back(std::move(it->map)); it = rackKeyMaps.erase(it);
    }
    // Views handed to the engine that no kept cache entry uses any more go the same way (including the zones
    // of a key map dropped above, which that map still names).
    std::set<const SessionAudioClip*> cached;
    for (const auto& [key, view] : rackSliceViews) cached.insert(view.get());
    for (const auto& made : rackKeyMaps) {
        cached.insert(made.base.get());
        if (made.map->keyZones) for (const auto& zone : *made.map->keyZones) cached.insert(zone.clip.get());
    }
    for (auto& view : retainedSliceViews) if (!cached.contains(view.get())) dropped.push_back(std::move(view));
    std::erase(retainedSliceViews, nullptr);
    return dropped;
}

void ProjectController::awaitAudioRelease(ReplacedAudio replaced) {
    if (replaced.clips.empty()) return;
    for (auto& clip : replaced.clips) replacedAudio.push_back(std::move(clip));
    replacedBytes += replaced.bytes;
    replacedFence = engine.audioReleaseRequest.load() + 1;   // covers everything set aside so far
    engine.audioReleaseRequest.store(replacedFence, std::memory_order_release);
}

bool ProjectController::reportDroppedNotes(std::uint32_t nowMs) {
    const auto rack = engine.droppedRackHits(), track = engine.droppedTrackMidi();
    if (rack <= reportedRackDrops && track <= reportedTrackDrops) return false;
    if (dropReported && nowMs - dropReportedAt < 1000) return false;
    const auto newRack = rack - reportedRackDrops, newTrack = track - reportedTrackDrops;
    juce::String tip;
    if (newRack > 0) tip << rackDropTip;
    if (newTrack > 0) tip << (tip.isEmpty() ? "" : "\n") << trackMidiDropTip;
    status(droppedNotesText(newRack, newTrack), tip);
    reportedRackDrops = rack; reportedTrackDrops = track; dropReportedAt = nowMs; dropReported = true;
    return true;
}

bool ProjectController::reportStreamProblems(std::uint32_t nowMs) {
    juce::Array<int> droppedOut, unreadable;
    for (std::size_t t = 0; t < ToneEngine::sessionTracks; ++t) {
        if (engine.sessionStreamUnderruns(t) > reportedUnderruns[t]) droppedOut.add(static_cast<int>(t) + 1);
        if (engine.sessionStreamFailures(t) > reportedStreamFailures[t]) unreadable.add(static_cast<int>(t) + 1);
    }
    if (droppedOut.isEmpty() && unreadable.isEmpty()) return false;
    if (streamReported && nowMs - streamReportedAt < 1000) return false;
    juce::String tip;
    if (!droppedOut.isEmpty()) tip << streamDropoutTip;
    if (!unreadable.isEmpty()) tip << (tip.isEmpty() ? "" : "\n") << streamUnreadableTip;
    status(streamProblemText(droppedOut, unreadable), tip);
    markStreamProblemsReported();
    streamReportedAt = nowMs; streamReported = true;
    return true;
}

std::size_t ProjectController::releaseReplacedAudio() {
    // Old Song and Channel Rack snapshots go once the engine has begun a block after leaving them (T1-04 D6).
    engine.arrangementMailbox.collect();
    engine.rackMailbox.collect();
    if (replacedAudio.empty() || warpWorkActive
        || engine.audioReleaseDone.load(std::memory_order_acquire) < replacedFence) return 0;
    const auto freed = replacedBytes;
    replacedAudio.clear();
    retainedBytes -= std::min(retainedBytes, replacedBytes);
    replacedBytes = 0;
    return freed;
}

std::size_t ProjectController::releaseUnreachableAudio(std::uint32_t nowMs) {
    engine.arrangementMailbox.collect();   // old snapshots give up their references first (D6)
    engine.rackMailbox.collect();
    if (!unreachableAudio.empty()) {
        if (engine.editedAudioDone.load(std::memory_order_acquire) < unreachableFence) return 0;
        const auto freed = unreachableBytes;
        unreachableAudio.clear();
        retainedBytes -= std::min(retainedBytes, unreachableBytes);
        unreachableBytes = 0;
        return freed;
    }
    if (busy || warpWorkActive || isRecording() || engine.playing.load()) return 0;
    if (unreachableSwept && nowMs - unreachableSweptAt < 1000) return 0;
    unreachableSwept = true; unreachableSweptAt = nowMs;
    // D17: rack files no channel names, now or in any undo/redo step, leave the caches first.
    std::set<std::string> reachableRackFiles;
    history.forEachDocument([&](const ProjectDocument& state) { addRackFiles(state.channelRack, reachableRackFiles); });
    auto found = setAsideRetainedAudio(forgetUnusedRackAudio(std::move(reachableRackFiles)));
    if (found.clips.empty()) return 0;
    unreachableAudio = std::move(found.clips);
    unreachableBytes = found.bytes;
    unreachableFence = engine.editedAudioRequest.load() + 1;
    engine.editedAudioRequest.store(unreachableFence, std::memory_order_release);
    return 0;
}

void ProjectController::registerStream(const std::shared_ptr<SessionAudioClip>& clip) {
    if (clip && clip->streamed()) engine.retainDiskAudioSource(clip->streamSource);
}

ProjectController::RelinkPlan ProjectController::planRelink(const PreparedRelink& prepared) const {
    RelinkPlan plan(doc);
    if (doc.identityScope != prepared.scope || !canEditMidiClips()) {
        plan.error = "The project changed or is recording. Search for missing audio again.";
        return plan;
    }
    auto remaining = availableAudioBytes();
    std::set<const SessionAudioClip*> counted;
    const auto reserve = [&](const std::shared_ptr<SessionAudioClip>& clip) {
        if (counted.contains(clip.get())) return true;
        const auto bytes = clip->decodedBytes();
        if (bytes > remaining) return false;
        remaining -= bytes;
        counted.insert(clip.get());
        plan.sources.push_back(clip);
        return true;
    };
    const auto trackIndex = [&plan](std::uint32_t id) {
        for (int t = 0; t < plan.after.trackCount(); ++t) if (plan.after.tracks[static_cast<std::size_t>(t)].id == id) return t;
        return -1;
    };
    const auto sceneIndex = [&plan](std::uint32_t id) {
        for (int s = 0; s < plan.after.sceneCount(); ++s) if (plan.after.scenes[static_cast<std::size_t>(s)].id == id) return s;
        return -1;
    };
    for (const auto& want : prepared.targets) {
        const auto& found = want.found;
        if (want.item.rackChannel) {
            if (!found.clip || !found.clip->valid() || found.file == juce::File{}) { plan.stillOffline.addIfNotAlreadyThere(want.item.fileName()); continue; }
            const auto oldPath = want.item.path.toStdString();
            bool stillWanted = false; // skip anything the user changed while the search ran
            for (int c = 0; c < plan.after.channelRack.channelCount; ++c)
                stillWanted = stillWanted || (plan.after.channelRack.info[static_cast<std::size_t>(c)].sample == oldPath && rackAudio.find(oldPath) == rackAudio.end());
            if (!stillWanted) continue;
            const auto newPath = found.file.getFullPathName().toStdString();
            if (newPath.size() > static_cast<std::size_t>(rack::maxSampleBytes)) {
                plan.stillOffline.addIfNotAlreadyThere(want.item.fileName());
                continue;
            }
            if (!rackAudio.contains(newPath) && !plan.rackAssets.contains(newPath)) {
                if (!reserve(found.clip)) {
                    plan.memoryFull = true;
                    plan.stillOffline.addIfNotAlreadyThere(want.item.fileName());
                    continue;
                }
                plan.rackAssets.emplace(newPath, found.clip);
            }
            for (int c = 0; c < plan.after.channelRack.channelCount; ++c)
                if (plan.after.channelRack.info[static_cast<std::size_t>(c)].sample == oldPath) plan.after.channelRack.setSample(c, newPath);
            ++plan.relinked;
            continue;
        }
        const int track = trackIndex(want.trackId);
        const int scene = want.item.drumPad ? 0 : sceneIndex(want.sceneId);
        if (track < 0 || scene < 0 || (want.item.drumPad && (want.item.slot < 0 || want.item.slot >= drumPadCount))) continue; // removed while searching
        auto& owner = plan.after.tracks[static_cast<std::size_t>(track)];
        const auto pad = static_cast<std::size_t>(want.item.slot);
        // Skip anything the user changed while the search ran.
        const bool unchanged = want.item.drumPad
            ? owner.drumPaths[pad] == want.item.path && owner.drumAssets[pad] == nullptr
            : plan.after.cellClip(track, scene) == want.cell && plan.after.clip(track, scene).audioOffline()
                && plan.after.clip(track, scene).audioFilePath == want.item.path;
        if (!unchanged || owner.settings.frozen()) continue;
        if (!want.item.drumPad && session_audio_import::destinationRefusal(plan.after, track, scene).isNotEmpty())
            continue;
        if (!found.clip || !found.clip->valid() || found.file == juce::File{}) { plan.stillOffline.addIfNotAlreadyThere(want.item.fileName()); continue; }
        const auto drumView = want.item.drumPad ? drumSliceView(found.clip, owner.drumSlices[pad]) : std::shared_ptr<const SessionAudioClip>{};
        if (want.item.drumPad && !drumView) { plan.stillOffline.addIfNotAlreadyThere(want.item.fileName()); continue; }
        if (!reserve(found.clip)) {
            plan.memoryFull = true;
            plan.stillOffline.addIfNotAlreadyThere(want.item.fileName());
            continue;
        }
        if (want.item.drumPad) {
            owner.drumAssets[pad] = drumView;
            plan.drumViews.push_back(drumView);
            owner.drumPaths[pad] = found.file.getFullPathName();
        } else {
            auto clip = plan.after.clip(track, scene);
            clip.audio = found.clip;
            clip.audioFilePath = found.file.getFullPathName();
            clip.audioFileName = found.file.getFileName();
            plan.after.setClip(track, scene, clip);
            plan.cells.emplace_back(track, scene);
        }
        ++plan.relinked;
    }
    return plan;
}

void ProjectController::installRelink(RelinkPlan& plan) {
    for (const auto& clip : plan.sources) {
        retainedBytes += clip->decodedBytes();
        retained.push_back(clip);
    }
    retained.insert(retained.end(), plan.drumViews.begin(), plan.drumViews.end());
    for (auto& [path, clip] : plan.rackAssets) rackAudio.try_emplace(path, std::move(clip));
    doc = std::move(plan.after);
    for (const auto& [track, scene] : plan.cells) {
        republishIfLive(track, scene);
        republishLinked(track, scene);
    }
    pushDocumentToEngine();
}

void ProjectController::reportRelink(const PreparedRelink& prepared, const RelinkPlan& plan) {
    const auto& folder = prepared.folder;
    if (plan.error.isNotEmpty()) {
        importStatus(plan.error, folder.getFullPathName());
        return;
    }
    auto text = "Relinked " + juce::String(plan.relinked) + " of " + juce::String(static_cast<int>(prepared.targets.size()))
              + " missing audio file" + (prepared.targets.size() == 1 ? "" : "s");
    if (prepared.truncated) text += " (only the first " + juce::String(static_cast<int>(MediaIndex::maxFilesIndexed)) + " files were searched)";
    if (plan.memoryFull) text += "; audio memory limit reached";
    if (!plan.stillOffline.isEmpty()) {
        text += ". Still offline: ";
        for (int i = 0; i < plan.stillOffline.size() && i < 3; ++i) text += (i ? ", " : "") + plan.stillOffline[i];
        if (plan.stillOffline.size() > 3) text += " and " + juce::String(plan.stillOffline.size() - 3) + " more";
    }
    importStatus(text, folder.getFullPathName());
    status(text, folder.getFullPathName());
}

juce::String ProjectController::describeOffline(const std::vector<MissingMedia>& missing) {
    juce::String text;
    for (std::size_t i = 0; i < missing.size() && i < 10; ++i) {
        const auto& m = missing[i];
        text += (m.rackChannel ? "Channel Rack, channel " + juce::String(m.track + 1)
               : m.drumPad ? "Track " + juce::String(m.track + 1) + ", drum pad " + juce::String(m.slot + 1)
                           : "Track " + juce::String(m.track + 1) + ", Scene " + juce::String(m.slot + 1))
              + ": " + m.fileName() + (m.reason.isNotEmpty() ? " (" + m.reason + ")" : juce::String{}) + "\n";
    }
    if (missing.size() > 10) text += "and " + juce::String(static_cast<int>(missing.size()) - 10) + " more\n";
    return text.trimEnd();
}

ProjectController::LoadResult ProjectController::loadProjectFile(const juce::File& file) {
    LoadResult result;
    if (file.getSize() <= 0 || file.getSize() > maxProjectFileBytes) {
        result.error = "Project file is empty or exceeds the 128 MiB limit.";
        return result;
    }
    if (!parseSessionProject(file.loadFileAsString(), result.project, result.error)) return result;
    const int tracks = result.project.trackCount(), scenes = result.project.sceneCount();
    result.audio.resize(static_cast<std::size_t>(tracks * scenes));
    std::map<juce::String, AudioClipImportResult> drumSources;
    std::set<const SessionAudioClip*> drumCounted;
    for (int s = 0; s < scenes; ++s)
        for (int t = 0; t < tracks; ++t) {
            const auto& clip = result.project.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            if (clip.audioFilePath.isEmpty()) continue;
            const auto location = resolveProjectAudio(file, clip.audioFilePath);
            const auto decoded = location == juce::File{} ? AudioClipImportResult{{}, "The saved audio location is not valid."}
                                                          : importSessionAudioClip(location);
            if (!decoded.clip) {
                // A moved or deleted file must not make the whole project unopenable.
                result.missing.push_back({false, t, s, location == juce::File{} ? clip.audioFilePath : location.getFullPathName(), decoded.error});
                continue;
            }
            const auto bytes = decoded.clip->decodedBytes();
            if (bytes > maxSessionAudioBytes - result.decodedBytes) {
                result.error = "Project audio exceeds the 512 MiB session limit.";
                return result;
            }
            result.decodedBytes += bytes;
            result.audio[static_cast<std::size_t>(s * tracks + t)] = decoded.clip;
            // Kits made from Session clips can reuse their decoded file.
            if (!decoded.clip->streamed()) {
                drumSources.try_emplace(location.getFullPathName(), decoded);
                drumCounted.insert(decoded.clip.get());
            }
        }
    result.drums.resize(tracks);
    const auto drumBudget = maxSessionAudioBytes - result.decodedBytes;
    std::size_t cachedDrumBytes{};
    for (int t = 0; t < tracks; ++t) for (int p = 0; p < drumPadCount; ++p) {
        const auto& path = result.project.drumPaths[t][p];
        if (path.isEmpty()) continue;
        const auto location = resolveProjectAudio(file, path);
        const auto key = location == juce::File{} ? path : location.getFullPathName();
        auto found = drumSources.find(key);
        if (found == drumSources.end()) {
            auto decoded = location == juce::File{} ? AudioClipImportResult{{}, "The saved audio location is not valid."} : decodeSessionAudioClip(location);
            if (decoded.clip) {
                const auto bytes = decoded.clip->decodedBytes();
                if (bytes > drumBudget - cachedDrumBytes) { result.error = "Drum audio exceeds memory limit"; return result; }
                cachedDrumBytes += bytes;
            }
            found = drumSources.emplace(key, std::move(decoded)).first;
        }
        const auto& decoded = found->second;
        auto view = drumSliceView(decoded.clip, result.project.drumSlices[t][p]);
        if (!view) {
            result.missing.push_back({true, t, p, key, decoded.clip ? "Saved slice exceeds the source audio." : decoded.error});
            continue;
        }
        if (drumCounted.insert(decoded.clip.get()).second) {
            const auto bytes = decoded.clip->decodedBytes();
            if (bytes > maxSessionAudioBytes - result.decodedBytes) { result.error = "Drum audio exceeds memory limit"; return result; }
            result.decodedBytes += bytes;
        }
        result.drums[t][p] = std::move(view);
    }
    std::set<std::string> attempted; // a sample used by several channels is decoded once
    for (int c = 0; c < result.project.channelRack.channelCount; ++c) {
        const auto& info = result.project.channelRack.info[static_cast<std::size_t>(c)];
        if (!info.isFile()) continue;
        const auto stored = juce::String::fromUTF8(info.sample.c_str(), static_cast<int>(info.sample.size()));
        const auto location = resolveProjectAudio(file, stored);
        const auto key = (location == juce::File{} ? stored : location.getFullPathName());
        if (!attempted.insert(key.toStdString()).second) continue;
        const auto decoded = location == juce::File{} ? AudioClipImportResult{{}, "The saved audio location is not valid."}
                                                      : decodeSessionAudioClip(location);
        if (!decoded.clip) { result.missing.push_back({false, c, 0, key, decoded.error, true}); continue; }
        const auto bytes = decoded.clip->decodedBytes();
        if (bytes > maxSessionAudioBytes - result.decodedBytes) { result.error = "Channel Rack audio exceeds memory limit"; return result; }
        result.decodedBytes += bytes; result.rackClips.emplace_back(key.toStdString(), decoded.clip);
    }
    return result;
}

void ProjectController::finishLoad(const juce::File& file, const LoadResult& result, bool recovered) {
    setBusy(false);
    if (result.error.isNotEmpty()) { status("Open failed: " + result.error, result.error); return; }
    releaseReplacedAudio();   // what earlier opens replaced, if the engine has let go of it
    if (result.decodedBytes > maxSessionAudioBytes - retainedBytes) {
        status("Open failed: retained audio memory limit reached",
               "Close and restart Alien Studio to release audio from older sessions.");
        return;
    }
    // T1-04 D7: the engine's track slots are prepared before anything is replaced, so an open that cannot
    // have them leaves the open project as it was.
    if (!prepareEngineTracks(result.project.trackCount(), "Open failed")) return;
    // T1-04 D8: and so is the Channel Rack's pending-hit queue for its channels.
    if (!prepareRackChannels(result.project.channelRack.channelCount, "Open failed")) return;
    abandonRecording();
    engine.playing.store(false);
    engine.requestedRackClip.store(rack::RackPlayer::noLaunchRequest);
    engine.sceneRackClip.store(rack::RackPlayer::noLaunchRequest);
    for (auto& request : engine.requestedTrackRackClip) request.store(rack::RackPlayer::noLaunchRequest);
    for (auto& request : engine.sceneTrackRackClip) request.store(rack::RackPlayer::noLaunchRequest);
    engine.rackPerformMode.store(false);
    engine.resetRackPlayback.store(true);
    copiedCell = {};
    if (copiedContent && copiedContent->rackClip != 0) copiedContent.reset();
    engine.synchronizedSceneLaunch.store(false);
    if (beforeDocumentReplaced) beforeDocumentReplaced();
    // A slot without a launch mailbox never played; stopping it would only create one (T1-04 D3).
    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track)
        if (engine.pianoRollLaunches.reserved(track)) publishStop(track);

    auto loaded = fromProjectData(result.project);
    for (int s = 0; s < loaded.sceneCount(); ++s)
        for (int t = 0; t < loaded.trackCount(); ++t) {
            auto clip = loaded.clip(t, s);
            if (clip.audioFilePath.isEmpty()) continue;
            const auto resolved = resolveProjectAudio(file, clip.audioFilePath);
            if (resolved != juce::File{}) {
                clip.audioFilePath = resolved.getFullPathName();
                clip.audioFileName = resolved.getFileName();
            } else {
                clip.audioFileName = MissingMedia::fileNameOf(clip.audioFilePath); // keep the saved reference as written
            }
            const auto& asset = result.audio[static_cast<std::size_t>(s * loaded.trackCount() + t)];
            clip.audio = asset;
            loaded.setClip(t, s, clip);
            if (asset != nullptr) { registerStream(asset); retained.push_back(asset); }
        }
    for (int t = 0; t < loaded.trackCount(); ++t) for (int p = 0; p < drumPadCount; ++p) {
        auto& track = loaded.tracks[t];
        track.drumAssets[p] = result.drums[t][p];
        if (track.drumPaths[p].isNotEmpty()) {
            const auto resolved = resolveProjectAudio(file, track.drumPaths[p]);
            if (resolved != juce::File{}) track.drumPaths[p] = resolved.getFullPathName();
        }
        if (result.drums[t][p]) retained.push_back(result.drums[t][p]);
    }
    for (int c = 0; c < loaded.channelRack.channelCount; ++c) {   // point rack samples at where they were found
        auto& info = loaded.channelRack.info[static_cast<std::size_t>(c)];
        if (!info.isFile()) continue;
        const auto resolved = resolveProjectAudio(file, juce::String::fromUTF8(info.sample.c_str(), static_cast<int>(info.sample.size())));
        const auto absolute = resolved.getFullPathName().toStdString();
        if (resolved != juce::File{} && absolute.size() <= static_cast<std::size_t>(rack::maxSampleBytes)) info.sample = absolute;
    }
    for (const auto& [path, clip] : result.rackClips) { rackAudio[path] = clip; retained.push_back(clip); }
    retainedBytes += result.decodedBytes;
    doc = std::move(loaded);
    projectFile = recovered ? recoveringFrom : file;
    pushDocumentToEngine();
    runtimeDirty = false;
    history.reset(doc);
    // The replaced project's audio, and the rack samples only it used (D14), freed once the engine lets go of them.
    awaitAudioRelease(setAsideRetainedAudio(forgetUnusedRackAudio()));
    // A recovered session has never been saved by the user.
    savedId = recovered ? 0 : history.currentId();
    autosavedId = 0;
    if (!recovered) discardRecovery();
    if (onDocumentReplaced) onDocumentReplaced();
    historyChanged();
    const auto offline = result.missing.empty() ? juce::String{}
        : " - " + juce::String(static_cast<int>(result.missing.size())) + " audio file"
          + (result.missing.size() == 1 ? " is" : "s are") + " offline (use Relink audio on the Session tab)";
    const auto offlineDetail = describeOffline(result.missing);
    if (recovered) status("Recovered unsaved work" + (projectFile != juce::File{} ? " for " + projectFile.getFileName() : juce::String{})
                          + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 save to keep it")) + offline, offlineDetail);
    else status("Opened " + file.getFileName() + offline, result.missing.empty() ? file.getFullPathName() : offlineDetail);
}

void ProjectController::restore(const ProjectDocument& state, const DocumentHistory::Step& step) {
    const auto previous = doc;
    bool reordered = previous.trackCount() != state.trackCount() || previous.sceneCount() != state.sceneCount();
    for (int t = 0; !reordered && t < state.trackCount(); ++t)
        reordered = previous.tracks[static_cast<std::size_t>(t)].id != state.tracks[static_cast<std::size_t>(t)].id;
    for (int s = 0; !reordered && s < state.sceneCount(); ++s)
        reordered = previous.scenes[static_cast<std::size_t>(s)].id != state.scenes[static_cast<std::size_t>(s)].id;
    doc = state;
    doc.library.retainIssuedIdsFrom(previous.library);
    doc.trackRegistry.retainIssuedIdsFrom(previous.trackRegistry);
    // Keep the user's current selection unless the step names a clip.
    doc.selectedTrack = step.focusTrack >= 0 ? step.focusTrack : previous.selectedTrack;
    doc.selectedScene = step.focusScene >= 0 ? step.focusScene : previous.selectedScene;
    doc.clampSelection();
    doc.channelRack.selectedPattern = juce::jlimit(0, doc.channelRack.patternCount - 1, previous.channelRack.selectedPattern);
    doc.channelRack.nameColumnWidth = previous.channelRack.nameColumnWidth;
    if (reordered) stopEverySlot();
    pushDocumentToEngine();
    if (!reordered) {
        // Live clips pick up restored content; audio changes relaunch.
        for (int t = 0; t < doc.trackCount(); ++t) {
            const auto index = static_cast<std::size_t>(t);
            const int active = engine.activePianoRollClip[index].load();
            const int requested = engine.requestedPianoRollClip[index].load();
            if (requested >= 0 && requested < doc.sceneCount() && requested != active) publishClip(t, requested);
            if (active < 0 || active >= doc.sceneCount() || requested != active) continue;
            const auto& before = previous.clip(t, active);
            const auto& after = doc.clip(t, active);
            if (before.audio != after.audio) publishClip(t, active);
            else if (!after.isAudioClip() && before.pattern != after.pattern) engine.pianoRollNotes[index].publish(after.notes());
        }
    }
    if (onDocumentRestored) onDocumentRestored();
    historyChanged();
}

juce::String ProjectController::describeInvalidSession(const ProjectDocument& d) {
    auto name = oversizedPluginName(d.masterPlugins);
    for (std::size_t t = 0; name.isEmpty() && t < d.tracks.size(); ++t) name = oversizedPluginName(d.tracks[t].plugins);
    if (name.isNotEmpty())
        return "the saved state of " + name + " is too large to store (limit about "
            + juce::String(maxPluginStateChars / (1024 * 1024) * 3 / 4) + " MB per plugin)";
    return "session data is invalid";
}

void ProjectController::prepareClipForTake(int track, int scene, bool replace) {
    const auto index = static_cast<std::size_t>(track);
    const bool live = engine.playing.load() && engine.activePianoRollClip[index].load() == scene;
    if (!replace) {
        if (!live) publishClip(track, scene);
        return;
    }
    auto silent = doc.clip(track, scene).notes();
    silent.clear();
    if (live) {
        engine.pianoRollNotes[index].publish(silent);
        return;
    }
    engine.requestedPianoRollClip[index].store(scene);
    SessionClipLaunch launch;
    launch.pattern = silent;
    launch.clipIndex = scene;
    engine.pianoRollLaunches[index].publish(launch);
}

void ProjectController::syncLiveClip(int track, int scene) {
    const auto index = static_cast<std::size_t>(track);
    if (engine.activePianoRollClip[index].load() == scene) engine.pianoRollNotes[index].publish(doc.clip(track, scene).notes());
    else republishIfLive(track, scene);
}

void ProjectController::previewTake() {
    if (!take.active || !doc.isValidCell(take.track, take.scene)) return;
    if (take.track < 0 || take.track >= static_cast<int>(ToneEngine::sessionTracks)) return;
    if (take.assembler.notes().size() == take.previewedNotes) return;
    const auto index = static_cast<std::size_t>(take.track);
    if (engine.activePianoRollClip[index].load() != take.scene) return;
    if (doc.clip(take.track, take.scene).isAudioClip()) return;
    bool truncated = false;
    engine.pianoRollNotes[index].publish(take.assembler.mergeInto(doc.clip(take.track, take.scene).notes(), take.replace, truncated));
    take.previewedNotes = take.assembler.notes().size();
}

void ProjectController::finishTake() {
    RecordingOutcome outcome;
    outcome.track = take.track; outcome.scene = take.scene; outcome.replaced = take.replace;
    outcome.droppedEvents = engine.recording.events.droppedCount() - take.droppedAtStart;
    outcome.noteLimitReached = take.assembler.noteLimitReached();
    outcome.refused = take.refused && !take.cancelled;
    const int track = take.track, scene = take.scene;
    const auto recorded = take.assembler.notes().size();
    const auto assembler = take.assembler;
    take.active = false; // before the clip is written: the take no longer owns it
    auto announce = [&] { if (onRecordingFinished) onRecordingFinished(outcome); };

    if (!doc.isValidCell(track, scene) || doc.clip(track, scene).isAudioClip()) {
        outcome.discarded = true;
        announce();
        return;
    }
    if (recorded == 0) { // keep what was there (replace played the clip silent)
        syncLiveClip(track, scene);
        announce();
        return;
    }
    const auto& base = doc.clip(track, scene).notes();
    bool truncated = false;
    const auto merged = assembler.mergeInto(base, take.replace, truncated);
    outcome.noteLimitReached = outcome.noteLimitReached || truncated;
    outcome.notesAdded = static_cast<int>(merged.count) - (take.replace ? 0 : static_cast<int>(base.count));
    const bool changes = !samePianoRollPattern(base, merged);
    outcome.error = recordMidiNotes ? recordMidiNotes(track, scene, merged)
                                    : juce::String("MIDI recording commands are not connected.");
    if (outcome.error.isNotEmpty()) {
        outcome.discarded = true;
        outcome.notesAdded = 0;
    }
    if (!changes || outcome.discarded) syncLiveClip(track, scene);
    announce();
}

bool ProjectController::prepareEngineTracks(int tracks, const juce::String& action) {
    if (engine.prepareTracks(tracks)) return true;
    status(action + ": not enough memory for " + juce::String(tracks) + " tracks",
           "Alien Studio could prepare " + juce::String(engine.preparedTracks())
           + " tracks. Close other programs or open a project with fewer tracks, then try again.");
    return false;
}

bool ProjectController::prepareRackChannels(int channels, const juce::String& action) {
    if (engine.prepareRackChannels(channels)) return true;
    status(action + ": not enough memory for " + juce::String(channels) + " Channel Rack channels",
           "Alien Studio could prepare " + juce::String(engine.preparedRackChannels())
           + " channels. Close other programs or remove Channel Rack channels, then try again.");
    return false;
}

void ProjectController::publishStop(std::size_t track) {
    engine.requestedPianoRollClip[track].store(-1);
    SessionClipLaunch stopped;
    stopped.pattern.clear();
    stopped.clipIndex = -1;
    engine.pianoRollLaunches[track].publish(stopped);
}

bool ProjectController::insertScene(int index, const std::vector<library::ClipId>* copyFrom) {
    if (busy) return false;
    if (beforeStructureEdit) beforeStructureEdit();
    if (!doc.canAddScene() || index < 0 || index > doc.sceneCount()) return false;
    // Inserting before existing scenes renumbers them.
    if (index < doc.sceneCount()) stopEverySlot();
    std::vector<library::ClipId> row(static_cast<std::size_t>(doc.trackCount()));
    if (copyFrom != nullptr) {
        const auto saved = doc;
        auto next = doc.channelRack;
        std::map<std::uint64_t, int> copies;
        for (std::size_t t = 0; t < row.size() && t < copyFrom->size(); ++t) {
            const auto id = (*copyFrom)[t];
            if (!id) continue;
            const int p = doc.rackPatternForClip(id.value);
            if (p < 0) continue;
            else if (!copies.contains(id.value)) {
                const int slot = next.patternCount;
                if (!next.addPattern(p)) { doc = saved; return false; }
                copies[id.value] = slot;
            }
        }
        if (!copies.empty()) {
            next.selectedPattern = doc.channelRack.selectedPattern;
            if (!doc.setRack(std::move(next))) { doc = saved; return false; }
            for (std::size_t t = 0; t < row.size() && t < copyFrom->size(); ++t)
                if (const auto found = copies.find((*copyFrom)[t].value); found != copies.end())
                    row[t] = library::ClipId{doc.channelRack.clips[static_cast<std::size_t>(found->second)]};
            pushRackToEngine();
        }
        // setRack prunes unused clips: allocate ordinary cell copies only after it finishes.
        for (std::size_t t = 0; t < row.size() && t < copyFrom->size(); ++t)
            if (!row[t] && (*copyFrom)[t]) row[t] = doc.library.duplicateClip((*copyFrom)[t]);
    }
    const auto name = copyFrom != nullptr ? doc.scenes[static_cast<std::size_t>(index - 1)].name + " copy"
                                          : doc.nextSceneName();
    doc.timeline.sceneInserted(index);
    for(auto& track:doc.tracks)if(track.settings.freezeScene>=index)++track.settings.freezeScene;
    doc.scenes.insert(doc.scenes.begin() + index, SessionScene{name});
    doc.cells.insert(doc.cells.begin() + index, row);
    if (doc.selectedScene >= index && index < doc.sceneCount() - 1) ++doc.selectedScene;
    return true;
}

void ProjectController::stopEverySlot() {
    abandonRecording(); // tracks or scenes are being renumbered, so the target is no longer valid
    engine.synchronizedSceneLaunch.store(false);
    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
        publishStop(track);
        engine.requestedTrackRackClip[track].store(0);
        engine.sceneTrackRackClip[track].store(rack::RackPlayer::noLaunchRequest);
    }
}

void ProjectController::stopSongTake() {
    // Cancelled before the engine saw the request: nothing was recorded.
    if (engine.recording.startRequested.exchange(false)) { finishSongTake(true); return; }
    engine.recording.stopRequested.store(true);
}

void ProjectController::pollSongTake() {
    bool ended = false, refused = false;
    for (RecordEvent event; engine.recording.events.pop(event);) {
        if (event.take != songTake.id) continue;
        if (event.kind == RecordEventKind::refused) refused = true;
        if (event.kind == RecordEventKind::end || event.kind == RecordEventKind::refused) { ended = true; break; }
    }
    if (ended) finishSongTake(refused);
}

void ProjectController::finishSongTake(bool refused) {
    auto& capture = *engine.songTake;
    for (auto& sink : capture.sinks) sink.store(nullptr);
    engine.recording.audioTake.store(false);
    const double firstTick = capture.firstTick.load();
    const auto frames = refused ? 0 : capture.frames.load();
    const double latency = capture.latencySamples.load() + songTake.offsetMs * songTake.sampleRate / 1000.0;
    const int ended = capture.ended.load();
    std::vector<alien::TakePass> passes;
    const int passCount = refused ? 0 : std::clamp(capture.passes.load(), 1, alien::SongTakeCapture::maxPasses);
    for (int k = 0; k < passCount; ++k)
        passes.push_back(k == 0 ? alien::TakePass{0, firstTick}
                                : alien::TakePass{capture.passFrame[static_cast<std::size_t>(k)].load(), capture.passTick[static_cast<std::size_t>(k)].load()});
    auto recordings = std::move(songTake.recordings);
    const auto finished = songTake;
    songTake = {};
    struct Loaded { int track{}; RecordingResult result; AudioClipImportResult audio; };
    auto loaded = std::make_shared<std::vector<Loaded>>();
    setBusy(true);
    std::weak_ptr<int> token = alive;
    std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, recordings, loaded, finished, firstTick, frames, latency, ended, refused, passes] {
        for (const auto& [t, recording] : recordings) {
            Loaded item; item.track = t; item.result = recording->finish();
            if (item.result.file.existsAsFile() && !finished.abandoned) item.audio = importSessionAudioClip(item.result.file);
            loaded->push_back(std::move(item));
        }
        juce::MessageManager::callAsync([this, token, loaded, finished, firstTick, frames, latency, ended, refused, passes] {
            if (token.lock() == nullptr) return;
            setBusy(false);
            SongTakeOutcome outcome;
            outcome.refused = refused; outcome.abandoned = finished.abandoned; outcome.ended = ended;
            std::vector<RecordedTake> takes;
            for (const auto& item : *loaded) {
                if (item.result.error.isNotEmpty() && item.result.writtenFrames > 0) outcome.problems << item.result.error << " ";
                if (!item.result.file.existsAsFile()) continue;
                ++outcome.recorded;
                if (finished.abandoned) { outcome.kept.push_back(item.result.file); continue; }
                if (!item.audio.clip) {
                    outcome.problems << "Could not load " << item.result.file.getFileName() << ": " << item.audio.error << " ";
                    outcome.kept.push_back(item.result.file);
                    continue;
                }
                RecordedTake recorded;
                recorded.track = item.track; recorded.file = item.result.file; recorded.clip = item.audio.clip;
                const timeline::TempoMap tempo{finished.tempoChanges, static_cast<double>(finished.bpm)};
                const auto written = std::min<std::uint64_t>(frames, item.result.writtenFrames);
                if (passes.size() > 1) {
                    // A loop recording: every pass with something in it becomes a take; the last full pass plays.
                    for (const auto& pass : placePasses(tempo, passes, written, finished.sampleRate, latency,
                                                        finished.punch ? finished.punchIn : 0, finished.punch ? finished.punchOut : -1,
                                                        finished.loopStart, finished.loopEnd)) {
                        if (!pass.placement.valid) continue;
                        if (pass.complete || recorded.passes.empty()) recorded.activePass = recorded.passes.size();
                        recorded.passes.push_back(pass.placement);
                    }
                    outcome.passes = std::max(outcome.passes, static_cast<int>(recorded.passes.size()));
                    recorded.placement.valid = !recorded.passes.empty();
                } else {
                    recorded.placement = placeTake(tempo, firstTick, written, finished.sampleRate, latency,
                                                   finished.punch ? finished.punchIn : 0, finished.punch ? finished.punchOut : -1);
                }
                if (!recorded.placement.valid) { outcome.kept.push_back(item.result.file); continue; }
                takes.push_back(std::move(recorded));
            }
            if (!takes.empty()) {
                outcome.placed = addRecordedTakes(takes);
                if (outcome.placed == 0) {
                    outcome.problems << "The takes could not be placed on the timeline (no free scene, or the song is full). ";
                    for (const auto& t : takes) outcome.kept.push_back(t.file);
                }
            }
            if (onSongTakeFinished) onSongTakeFinished(outcome);
        });
        return juce::ThreadPoolJob::jobHasFinished;
    };
    pool.addJob(std::move(job));
}

void ProjectController::markStreamProblemsReported() noexcept {
    for (std::size_t t = 0; t < ToneEngine::sessionTracks; ++t) {
        reportedUnderruns[t] = engine.sessionStreamUnderruns(t);
        reportedStreamFailures[t] = engine.sessionStreamFailures(t);
    }
}

} // namespace alien
