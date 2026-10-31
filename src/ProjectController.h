#pragma once
#include "ProjectDocument.h"
#include "ViewModes.h"
#include "ToneEngine.h"
#include "AudioClipImport.h"
#include "AudioRelink.h"
#include "PortableProject.h"
#include "DocumentHistory.h"
#include "PianoRollProcessing.h"
#include "RackSounds.h"
#include "RackChannelNotes.h"
#include "DiskRecording.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <set>

// Owns the ProjectDocument and is the only code that changes it. Every edit
// goes through here so the audio engine is kept in sync. Callers report each
// finished edit with commit(), which feeds undo/redo, the unsaved-changes
// state and autosave. File work and audio decoding run on a worker thread;
// results come back on the message thread. Message thread only.
namespace alien {

class ProjectController {
public:
    static constexpr std::size_t maxSessionAudioBytes = 512u * 1024u * 1024u;

    explicit ProjectController(ToneEngine& toneEngine)
        : engine(toneEngine), doc(ProjectDocument::createDefault()) {
        runtimeDirty = false;
        history.reset(doc);
        savedId = history.currentId();
    }

    ~ProjectController() {
        alive.reset();
        audioFileChooser.reset();
        projectFileChooser.reset();
        pool.removeAllJobs(true, -1);
    }

    const ProjectDocument& document() const noexcept { return doc; }
    void setExternalBusy(bool value) { setBusy(value); }
    bool isBusy() const noexcept { return busy; }
    const juce::File& currentFile() const noexcept { return projectFile; }

    // Message-thread notifications.
    std::function<void(const juce::String&, const juce::String&)> onProjectStatus; // text, tooltip
    std::function<void(const juce::String&, const juce::String&)> onImportStatus;
    std::function<void(bool)> onBusyChanged;
    std::function<void()> beforeDocumentReplaced; // e.g. release held notes
    std::function<void()> onDocumentReplaced;     // whole document was opened/replaced
    std::function<void(int, int)> onClipContentChanged;
    std::function<void()> onDocumentRestored;     // undo/redo replaced the document
    std::function<void()> onBeforeSnapshot;
    // Autosave runs unattended while the user may be playing, so it gets its own
    // hook: the owner can decline to interrupt audio to capture plugin state.
    std::function<void()> onBeforeAutosaveSnapshot;
    std::function<void()> beforeStructureEdit;
    std::function<void()> onHistoryChanged;       // undo/redo availability or unsaved state changed

    // Undo, redo and unsaved changes -------------------------------------
    // Call after every finished edit. focusTrack/focusScene name the clip the
    // edit changed (shown again when it is undone). Edits with the same
    // coalesceKey made in quick succession become one undo step.
    void commit(const juce::String& label, int focusTrack = -1, int focusScene = -1,
                const juce::String& coalesceKey = {}, bool continuous = false) {
        for(int t=0;t<doc.trackCount();++t)engine.tracks[static_cast<std::size_t>(t)].freezeScene.store(doc.tracks[static_cast<std::size_t>(t)].settings.freezeScene);
        doc.synchronizeTrackRegistry();
        const auto previous = history.currentId();
        history.record(doc, label, focusTrack, focusScene, coalesceKey, continuous);
        if (history.currentId() != previous) publishArrangement();
        historyChanged();
    }

    // Start a fresh history from the current document (e.g. at startup).
    void markAsNewBaseline() {
        runtimeDirty = false;
        history.reset(doc);
        savedId = history.currentId();
        historyChanged();
    }

    void runtimePluginEdited() { runtimeDirty = true; historyChanged(); }
    // True when a plugin was edited since its state was last copied into the document.
    bool pluginStateDirty() const noexcept { return runtimeDirty; }
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master) {
        setPluginRacks(racks, master, returnRacks());
    }
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns) {
        setPluginRacks(racks, master, returns, busRacks());
    }
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns, const BusRacks& buses) {
        if (racks.size() != doc.tracks.size()) return;
        for (std::size_t t=0;t<racks.size();++t) doc.tracks[t].plugins = racks[t];
        doc.masterPlugins = master; runtimeDirty = false;
        for (std::size_t r=0;r<returns.size();++r) if (doc.returns[r].used) doc.returns[r].plugins = returns[r];
        for (std::size_t b=0;b<buses.size();++b) if (doc.buses[b].used) doc.buses[b].plugins = buses[b];
        commit("Change plugins");
    }
    // The group buses' FX racks (empty for unused slots), in the shape PluginHost takes them.
    BusRacks busRacks() const {
        BusRacks racks;
        for (std::size_t b=0;b<racks.size();++b) if (doc.buses[b].used) racks[b] = doc.buses[b].plugins;
        return racks;
    }

    // The return tracks' FX racks (empty for unused slots), in the shape PluginHost takes them.
    ReturnRacks returnRacks() const {
        ReturnRacks racks;
        for (std::size_t r=0;r<racks.size();++r) if (doc.returns[r].used) racks[r] = doc.returns[r].plugins;
        return racks;
    }
    bool canUndo() const noexcept { return history.canUndo(); }
    bool canRedo() const noexcept { return history.canRedo(); }
    juce::String undoLabel() const { return history.undoLabel(); }
    juce::String redoLabel() const { return history.redoLabel(); }
    bool isDirty() const noexcept { return runtimeDirty || history.currentId() != savedId; }

    bool undo() {
        if (take.active || songTake.active) return false; // the take would be written over the restored clip
        DocumentHistory::Step step;
        if (const auto* state = history.undo(step)) { restore(*state, step); return true; }
        return false;
    }

    bool redo() {
        if (take.active || songTake.active) return false;
        DocumentHistory::Step step;
        if (const auto* state = history.redo(step)) { restore(*state, step); return true; }
        return false;
    }

    // Put back a document captured earlier in the same edit, before it was committed
    // (e.g. a command batch that failed part way). The history is not changed.
    void revertTo(const ProjectDocument& state) { restore(state, {{}, state.selectedTrack, state.selectedScene}); }

    // Replace the song with an empty default project.
    void newProject() {
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
        stopEverySlot();
        doc = ProjectDocument::createDefault();
        projectFile = juce::File{};
        pushDocumentToEngine();
        runtimeDirty = false;
        history.reset(doc);
        savedId = history.currentId();
        discardRecovery();
        if (onDocumentReplaced) onDocumentReplaced();
        historyChanged();
        status("New session", {});
    }

    // Autosave and crash recovery ----------------------------------------
    static juce::File recoveryDirectory() {
        if (recoveryDirectoryOverride() != juce::File{}) return recoveryDirectoryOverride();
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Alien Studio").getChildFile("Recovery");
    }
    // Tests point autosave somewhere temporary so a real autosave is never touched.
    static juce::File& recoveryDirectoryOverride() { static juce::File directory; return directory; }
    static juce::File recoveryFile() { return recoveryDirectory().getChildFile("Autosave.alienstudio"); }
    static juce::File recoveryInfoFile() { return recoveryDirectory().getChildFile("Autosave.json"); }

    // An autosave left behind means the last session did not close normally.
    static bool hasRecoverableSession() { return recoveryFile().existsAsFile(); }

    // The project the autosave belonged to, if it had been saved before.
    static juce::File recoveredOriginalFile() {
        const auto info = juce::JSON::parse(recoveryInfoFile().loadFileAsString());
        if (auto* object = info.getDynamicObject()) {
            const auto path = object->getProperty("projectFile").toString();
            if (path.isNotEmpty() && juce::File::isAbsolutePath(path)) return juce::File(path);
        }
        return {};
    }

    static void deleteRecoveryFiles() {
        recoveryFile().deleteFile();
        recoveryInfoFile().deleteFile();
    }

    // Serialise cleanup with an in-flight writer. A queued writer and its UI
    // completion also carry a generation so neither can revive discarded work.
    void discardRecovery() {
        recoveryGeneration.fetch_add(1);
        std::lock_guard<std::mutex> lock(recoveryMutex);
        deleteRecoveryFiles();
        autosaving = false;
        autosavedId = 0;
    }
    bool isAutosaving() const noexcept { return autosaving; }

    // Write an autosave copy on the worker thread if there are unsaved edits
    // that have not been autosaved yet. Never touches the user's project file.
    void autosaveIfNeeded() {
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

    // Open the autosave. The result keeps the original project path (if any)
    // and counts as unsaved, so the user decides where to keep it.
    void recoverSession() {
        recoveringFrom = recoveredOriginalFile();
        openProject(recoveryFile(), true);
    }

    // Engine synchronisation ---------------------------------------------
    void pushDocumentToEngine() {
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
        engine.activeTracks.store(doc.trackCount());
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

    // Track and scene structure --------------------------------------------
    // Appending never disturbs playback. Any other structural edit renumbers
    // tracks or scenes, so Session playback is stopped first; this keeps the
    // engine's per-track slots and clip indices consistent with the document.
    bool addTrack() {
        if (busy) return false;
        if (beforeStructureEdit) beforeStructureEdit();
        if (!doc.canAddTrack() || !doc.trackRegistry.canAllocate()) return false;
        doc.tracks.push_back({doc.nextTrackName(), {}, doc.nextTrackColour()});
        for (auto& scene : doc.cells) scene.emplace_back();
        publishStop(static_cast<std::size_t>(doc.trackCount() - 1));
        pushDocumentToEngine();
        return true;
    }

    bool deleteTrack(int track) {
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

    bool moveTrack(int track, int delta) {
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

    bool renameTrack(int track, const juce::String& name) {
        if (busy) return false;
        const auto trimmed = name.trim().substring(0, 64);
        if (track < 0 || track >= doc.trackCount() || trimmed.isEmpty()) return false;
        doc.tracks[static_cast<std::size_t>(track)].name = trimmed;
        return true;
    }

    // Number of colours in the track palette (ui::trackColour).
    static constexpr int trackColourCount = 8;
    bool setTrackColour(int track, int colour) {
        if (busy || track < 0 || track >= doc.trackCount() || colour < 0 || colour >= trackColourCount) return false;
        doc.tracks[static_cast<std::size_t>(track)].colour = colour;
        return true;
    }

    bool addScene() { return insertScene(doc.sceneCount(), nullptr); }

    bool duplicateScene(int scene) {
        if (scene < 0 || scene >= doc.sceneCount()) return false;
        return insertScene(scene + 1, &doc.cells[static_cast<std::size_t>(scene)]);
    }

    bool deleteScene(int scene) {
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

    bool moveScene(int scene, int delta) {
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

    bool renameScene(int scene, const juce::String& name) {
        if (busy) return false;
        const auto trimmed = name.trim().substring(0, 64);
        if (scene < 0 || scene >= doc.sceneCount() || trimmed.isEmpty()) return false;
        doc.scenes[static_cast<std::size_t>(scene)].name = trimmed;
        return true;
    }

    // The mixer writes the engine directly while a control moves; this copies
    // the result back into the document.
    void masterGainEdited() { doc.masterGain = sanitizedMasterGain(engine.masterControls.gain.load()); }
    void returnSettingsEdited(int r) {
        if (r < 0 || r >= maxReturns || !doc.returns[static_cast<std::size_t>(r)].used) return;
        const auto next = engine.returns[static_cast<std::size_t>(r)].snapshot();
        if (next.valid()) doc.returns[static_cast<std::size_t>(r)].settings = next;
    }
    void busSettingsEdited(int b) {
        if (b < 0 || b >= maxBuses || !doc.buses[static_cast<std::size_t>(b)].used) return;
        const auto next = engine.buses[static_cast<std::size_t>(b)].snapshot();
        if (next.valid()) doc.buses[static_cast<std::size_t>(b)].settings = next;
    }

    // Group buses. A new bus takes the first free slot (its index, or -1 when all are in use);
    // deleting one sends its tracks back to the master and drops its FX. Buses feed only the
    // master, so no edit can make a loop. The caller commits, and rebuilds the plugin racks
    // after a delete.
    int addBus(const juce::String& name = {}) {
        if (busy || !doc.trackRegistry.canAllocate()) return -1;
        for (std::size_t b = 0; b < doc.buses.size(); ++b) {
            if (doc.buses[b].used) continue;
            const auto trimmed = name.trim().substring(0, 64);
            doc.buses[b] = {true, trimmed.isNotEmpty() ? trimmed : nextBusName(), {}, {}};
            pushDocumentToEngine();
            return static_cast<int>(b);
        }
        return -1;
    }
    juce::String nextBusName() const {
        for (int n = 1;; ++n) {
            bool taken = false;
            for (const auto& bus : doc.buses) taken = taken || (bus.used && bus.name == "Bus " + juce::String(n));
            if (!taken) return "Bus " + juce::String(n);
        }
    }
    bool renameBus(int b, const juce::String& name) {
        const auto trimmed = name.trim().substring(0, 64);
        if (busy || b < 0 || b >= maxBuses || !doc.buses[static_cast<std::size_t>(b)].used || trimmed.isEmpty()) return false;
        doc.buses[static_cast<std::size_t>(b)].name = trimmed;
        return true;
    }
    bool deleteBus(int b) {
        if (busy || b < 0 || b >= maxBuses || !doc.buses[static_cast<std::size_t>(b)].used) return false;
        for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
        for (auto& track : doc.tracks) if (track.settings.output == b) track.settings.output = -1;
        doc.buses[static_cast<std::size_t>(b)] = {};
        doc.sidechainBusDeleted(b);   // effects keyed by its output lose their key
        pushDocumentToEngine();
        return true;
    }
    // Return tracks (MIX-05). A new return takes the first free slot (its index, or -1 when all are
    // in use) and that slot's letter: sends and send automation name the slot, so removing a return
    // never moves another's. Removing one turns every track's send to it off, drops its send
    // automation and its FX. The caller commits, and rebuilds the plugin racks after a removal.
    int addReturn(const juce::String& name = {}) {
        if (busy || !doc.trackRegistry.canAllocate()) return -1;
        for (std::size_t r = 0; r < doc.returns.size(); ++r) {
            if (doc.returns[r].used) continue;
            const auto trimmed = name.trim().substring(0, 64);
            doc.returns[r] = {true, trimmed.isNotEmpty() ? trimmed : defaultReturnName(static_cast<int>(r)), {}, {}};
            pushDocumentToEngine();
            return static_cast<int>(r);
        }
        return -1;
    }
    bool renameReturn(int r, const juce::String& name) {
        const auto trimmed = name.trim().substring(0, 64);
        if (busy || r < 0 || r >= maxReturns || !doc.returns[static_cast<std::size_t>(r)].used || trimmed.isEmpty()
            || doc.returns[static_cast<std::size_t>(r)].name == trimmed) return false;
        doc.returns[static_cast<std::size_t>(r)].name = trimmed;
        return true;
    }
    bool deleteReturn(int r) {
        if (busy || r < 0 || r >= maxReturns || !doc.returns[static_cast<std::size_t>(r)].used) return false;
        const auto slot = static_cast<std::size_t>(r);
        for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
        for (auto& track : doc.tracks) { track.settings.send[slot] = 0.0f; track.settings.sendPre[slot] = false; }
        auto& lanes = doc.timeline.automationLanes;
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [r](const auto& lane) { return automation::sendIndex(lane.parameter) == r; }), lanes.end());
        doc.returns[slot] = {};
        doc.sidechainReturnDeleted(r);   // effects keyed by its output lose their key
        pushDocumentToEngine();
        return true;
    }

    // -1 sends the track to the master. The engine hears the change at once; the caller commits.
    bool setTrackOutput(int track, int output) {
        if (busy || track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size())
            || output < -1 || output >= maxBuses || (output >= 0 && !doc.buses[static_cast<std::size_t>(output)].used)) return false;
        auto& settings = doc.tracks[static_cast<std::size_t>(track)].settings;
        if (settings.output == output) return false;
        settings.output = output;
        engine.tracks[static_cast<std::size_t>(track)].output.store(output);
        return true;
    }

    // Message-thread command setters. Only the addressed control changes; the
    // caller commits after the complete command/batch, just like track renaming.
    bool setTrackVolume(int track, double gain) {
        if (busy || track < 0 || track >= doc.trackCount() || !std::isfinite(gain) || gain < 0 || gain > 2) return false;
        const auto value = static_cast<float>(gain);
        doc.tracks[static_cast<std::size_t>(track)].settings.gain = value;
        engine.tracks[static_cast<std::size_t>(track)].gain.store(value);
        return true;
    }
    bool setTrackPan(int track, double pan) {
        if (busy || track < 0 || track >= doc.trackCount() || !std::isfinite(pan) || pan < -1 || pan > 1) return false;
        const auto value = static_cast<float>(pan);
        doc.tracks[static_cast<std::size_t>(track)].settings.pan = value;
        engine.tracks[static_cast<std::size_t>(track)].pan.store(value);
        return true;
    }

    bool setTrackMute(int track, bool muted) {
        if (busy || track < 0 || track >= doc.trackCount()) return false;
        doc.tracks[static_cast<std::size_t>(track)].settings.mute = muted;
        engine.tracks[static_cast<std::size_t>(track)].mute.store(muted);
        return true;
    }
    bool setTrackSolo(int track, bool soloed) {
        if (busy || track < 0 || track >= doc.trackCount()) return false;
        doc.tracks[static_cast<std::size_t>(track)].settings.solo = soloed;
        engine.tracks[static_cast<std::size_t>(track)].solo.store(soloed);
        return true;
    }

    void trackSettingsEdited(int track) {
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

    // A track's recording settings (arm, input, monitoring). The engine hears the change at once; it is one undo step.
    bool editTrackRecording(int track, const juce::String& label, const std::function<void(TrackSettings&)>& change) {
        if (busy || isTrackFrozen(track) || track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size())) return false;
        auto& settings = doc.tracks[static_cast<std::size_t>(track)].settings;
        auto next = settings;
        change(next);
        if (!next.valid() || (next.armed == settings.armed && next.input == settings.input
            && next.stereoInput == settings.stereoInput && next.monitor == settings.monitor)) return false;
        settings.armed = next.armed; settings.input = next.input; settings.stereoInput = next.stereoInput; settings.monitor = next.monitor;
        auto& controls = engine.tracks[static_cast<std::size_t>(track)];
        controls.input.store(next.input); controls.stereoInput.store(next.stereoInput);
        controls.monitor.store(next.monitor); controls.armed.store(next.armed);
        commit(label);
        return true;
    }
    // The loudest input on `track` since the last call (0 when it is neither armed nor monitoring).
    float takeInputLevel(int track) noexcept {
        if (track < 0 || track >= static_cast<int>(engine.tracks.size())) return 0.0f;
        return engine.tracks[static_cast<std::size_t>(track)].inputPeak.exchange(0.0f, std::memory_order_relaxed);
    }

    // Playback --------------------------------------------------------------
    void selectClip(int track, int scene) {
        if (!doc.isValidCell(track, scene)) return;
        doc.selectedTrack = track;
        doc.selectedScene = scene;
    }

    void launchClip(int track, int scene) {
        selectClip(track, scene);
        if (doc.isValidCell(track, scene) && doc.clip(track, scene).rackClip != 0) {
            if (launchRackClip(doc.clip(track, scene).rackClip, track)) publishStop(static_cast<std::size_t>(track));
            return;
        }
        if (doc.isValidCell(track, scene)) stopRackOnTrack(track);
        publishClip(doc.selectedTrack, doc.selectedScene);
    }

    // A clip or scene launched from the Session view should be heard at once: while Session clips are what the
    // engine plays (not the Song, not the Channel Rack, not a recording's count-in), a stopped transport starts.
    // Returns whether it started; the launch itself waits for the next Beat/Bar boundary as usual.
    bool startTransportForLaunch() {
        if (engine.patternMode.load() != sessionMode || engine.playing.load()
            || engine.recording.phase.load() == RecordControls::countIn) return false;
        engine.playing.store(true);
        return true;
    }

    void launchScene(int scene) {
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

    void stopAllTracks() {
        engine.sceneRackClip.store(0);
        for (auto& source : engine.sceneTrackRackClip) source.store(0);
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < doc.trackCount(); ++track) publishStop(static_cast<std::size_t>(track));
        stopRackClip();
    }

    // A launch is transport state, not an edit or a change of the pattern being edited.
    int rackPatternForClip(std::uint64_t clip) const noexcept {
        return doc.rackPatternForClip(clip);
    }
    bool rackPatternInUse(int pattern) const {
        if (!doc.channelRack.validPattern(pattern)) return false;
        const auto id = doc.channelRack.clips[static_cast<std::size_t>(pattern)];
        for (const auto& scene : doc.cells) for (auto cell : scene) if (cell.value == id) return true;
        for (const auto& r : doc.timeline.regions) if (r.isPattern() && r.rackClip == id) return true;
        return false;
    }
    bool canLaunchRackClip(std::uint64_t clip) const {
        if (busy || isRecording() || engine.patternMode.load() == 2 || rackPatternForClip(clip) < 0) return false;
        for (int c = 0; c < doc.channelRack.channelCount; ++c)
            if (isTrackFrozen(doc.channelRack.channels[static_cast<std::size_t>(c)].output)) return false;
        return true;
    }
    bool launchRackClip(std::uint64_t clip, int track = -1) {
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
    bool stopRackClip() {
        if (busy || isRecording()) return false;
        engine.requestedRackClip.store(0);
        return true;
    }

    // Stops one Session track at the next launch boundary; the other tracks keep playing.
    void stopTrack(int track) {
        if (track < 0 || track >= doc.trackCount()) return;
        publishStop(static_cast<std::size_t>(track));
        stopRackOnTrack(track);
    }
    void stopRackOnTrack(int track) {
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

    void publishClip(int track, int scene) {
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

    // Recording ---------------------------------------------------------------
    // What the user is told when a take ends.
    struct RecordingOutcome {
        int track{-1}, scene{-1};
        int notesAdded{};                 // notes now in the clip because of this take
        bool replaced{};                  // the take replaced the clip's earlier notes
        bool refused{};                   // the engine could not start the take
        bool discarded{};                 // the clip changed underneath the take, so nothing was kept
        bool noteLimitReached{};          // the clip is full (1024 notes)
        std::uint64_t droppedEvents{};    // played notes the engine had no room to report
    };
    struct RecordStart { bool started{}; juce::String reason; };
    std::function<void(const RecordingOutcome&)> onRecordingFinished;

    // Message thread only. Records into the selected clip, which must be empty
    // or hold MIDI. The count-in, metronome and monitoring are done by the
    // engine; the take becomes one undo step when it ends.
    RecordStart startRecording() {
        if (take.active || songTake.active) return {false, "Already recording."};
        if (busy) return {false, "Wait for the current operation to finish."};
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

    // Ask the engine to end the take; its notes arrive through pollRecording().
    void stopRecording() {
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

    bool isRecording() const noexcept { return take.active || songTake.active; }
    int recordingTrack() const noexcept { return take.active ? take.track : -1; }
    int recordingScene() const noexcept { return take.active ? take.scene : -1; }

    // Drops a take without keeping it (the document is being replaced).
    void abandonRecording() {
        if (songTake.active) { songTake.abandoned = true; stopSongTake(); }
        if (!take.active) return;
        take.active = false;
        engine.recording.startRequested.store(false);
        engine.recording.stopRequested.store(true);
    }

    // Message-thread timer: turns what the engine captured into notes, lets the
    // looping clip play back what has been recorded so far, and finishes the take.
    void pollRecording() {
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

    // Song-view audio takes ----------------------------------------------------
    // What the user is told when an audio take ends.
    struct SongTakeOutcome {
        int placed{};                // takes now on the timeline
        int recorded{};              // tracks that captured audio
        bool refused{}, abandoned{};
        int ended{};                 // SongTakeCapture::Ended
        int passes{};                // loop passes recorded (0 or 1: not a loop recording)
        std::vector<juce::File> kept; // files of the takes that could not be placed (abandoned, or failed to load)
        juce::String problems;       // disk or loading problems
    };
    std::function<void(const SongTakeOutcome&)> onSongTakeFinished;

    // Message thread only. Records every armed track whose input the device has (`deviceInputs` enabled
    // channels) into a new WAV in `folder` while the song plays; the engine counts in and starts the song,
    // and with punch on records only the loop range. The take is placed as one undo step when it ends.
    RecordStart startSongTake(const juce::File& folder, double sampleRate, int deviceInputs) {
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
    bool isSongTakeActive() const noexcept { return songTake.active; }
    // The tracks being recorded, for the view.
    std::vector<int> songTakeTracks() const {
        std::vector<int> result;
        for (const auto& recording : songTake.recordings) result.push_back(recording.first);
        return result;
    }

    void setRecordSettings(const RecordSettings& settings) {
        if (!settings.valid() || settings == doc.recording) return;
        doc.recording = settings;
        engine.recording.apply(settings);
        commit("Change recording settings", -1, -1, "recordSettings");
    }

    // Republish only when the clip is playing or queued, so content changes
    // are heard without launching anything new.
    // Republishes every other cell that shares this cell's content, where it is playing or queued.
    void republishLinked(int track, int scene) {
        const auto link = doc.isValidCell(track, scene) ? doc.linkOf(track, scene) : 0;
        if (link == 0) return;
        for (int s = 0; s < doc.sceneCount(); ++s) for (int t = 0; t < doc.trackCount(); ++t)
            if ((t != track || s != scene) && doc.linkOf(t, s) == link) republishIfLive(t, s);
    }

    // ---- linked clips (T1-03): Session cells that share one clip's content ----------------------
    bool assignRackClip(int track, int scene, std::uint64_t clip) {
        if (busy || isRecording() || !doc.isValidCell(track, scene) || isTrackFrozen(track)) return false;
        if (!doc.bindRackClip(track, scene, clip)) return false;
        publishStop(static_cast<std::size_t>(track));
        commit("Assign rack clip", track, scene);
        return true;
    }
    bool copyRackToCell(int track, int scene, std::uint64_t clip) {
        if (busy || isRecording() || !doc.isValidCell(track, scene) || isTrackFrozen(track)) return false;
        const int source = rackPatternForClip(clip);
        if (source < 0) return false;
        const auto saved = doc;
        auto next = doc.channelRack;
        const int slot = next.patternCount;
        if (!next.addPattern(source)) return false;
        next.selectedPattern = doc.channelRack.selectedPattern;
        if (!doc.setRack(std::move(next)) || !doc.bindRackClip(track, scene, doc.channelRack.clips[static_cast<std::size_t>(slot)])) {
            doc = saved; return false;
        }
        pushRackToEngine(); publishStop(static_cast<std::size_t>(track));
        commit("Copy rack clip", track, scene);
        return true;
    }
    // Copy remembers a cell's clip. Paste puts an independent copy of it in a cell; Paste linked puts a
    // clip that shares its content (an edit of either is an edit of both). Make unique gives a linked
    // cell its own copy again. Each is one undo step; a pasted cell's old content is replaced, not edited.
    bool copyCell(int track, int scene) {
        if (!doc.isValidCell(track, scene) || !doc.cellClip(track, scene)) return false;
        copiedCell = doc.cellClip(track, scene);
        copiedContent = doc.clip(track, scene);
        return true;
    }
    bool hasCopiedCell() const noexcept { return copiedContent.has_value(); }
    bool canPasteLinked() const { return copiedCell && doc.library.clip(copiedCell) != nullptr; }
    bool pasteCell(int track, int scene, bool linked) {
        if (busy || isRecording() || !doc.isValidCell(track, scene) || isTrackFrozen(track) || !copiedContent) return false;
        if (copiedContent->rackClip != 0)
            return linked ? assignRackClip(track, scene, copiedContent->rackClip) : copyRackToCell(track, scene, copiedContent->rackClip);
        if (take.active && track == take.track && scene == take.scene) return false;
        if (linked) {
            if (!canPasteLinked()) return false;
            const auto* source = doc.library.clip(copiedCell);
            if (doc.linkOf(track, scene) == source->pattern.value) return false;   // already plays that content
            doc.cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)] = doc.library.linkClip(copiedCell);
            doc.prune();
        } else {
            doc.clearClip(track, scene);
            doc.setClip(track, scene, *copiedContent);
        }
        republishIfLive(track, scene);
        commit(linked ? "Paste linked clip" : "Paste clip", track, scene);
        return true;
    }
    bool makeCellUnique(int track, int scene) {
        if (busy || isRecording() || !doc.isValidCell(track, scene) || isTrackFrozen(track)) return false;
        if (doc.clip(track, scene).rackClip != 0) return copyRackToCell(track, scene, doc.clip(track, scene).rackClip);
        const auto id = doc.cellClip(track, scene);
        if (!id || !doc.library.makeClipUnique(id)) return false;
        commit("Make clip unique", track, scene);
        return true;
    }
    // Empties one cell. Content other cells share stays theirs; Song regions on the cell fall silent.
    bool clearCell(int track, int scene) {
        if (busy || !doc.isValidCell(track, scene) || isTrackFrozen(track)) return false;
        if (take.active && track == take.track && scene == take.scene) return false;
        if (!doc.cellClip(track, scene)) return false;
        doc.clearClip(track, scene);
        republishIfLive(track, scene);
        return true;
    }
    // Whether a Song placement shares what it plays with something else: another placement of the same
    // cell or placement clip, or a linked cell.
    bool placementShared(std::uint32_t regionId) const {
        const auto* r = doc.timeline.find(regionId);
        if (r == nullptr) return false;
        if (r->isPattern()) return doc.rackPatternForClip(r->rackClip) >= 0;
        if (r->ownsClip()) {
            int uses = 0;
            for (const auto& other : doc.timeline.regions) uses += other.clip == r->clip ? 1 : 0;
            return uses > 1;
        }
        if (!doc.isValidCell(r->row, r->source) || !doc.cellClip(r->row, r->source)) return false;
        int uses = 0;
        for (const auto& other : doc.timeline.regions) uses += other.playsCell() && other.row == r->row && other.source == r->source ? 1 : 0;
        return uses > 1 || doc.linkedCells(r->row, r->source) > 1;
    }
    // Gives one Song placement content of its own: an independent copy of what it plays. A MIDI placement
    // gets a placement clip, which no Session cell launches; an audio placement gets a free cell of its track
    // (a scene is added when there is none). The cell and the other placements keep theirs.
    bool makePlacementUnique(std::uint32_t regionId) {
        if (busy || isRecording() || !placementShared(regionId)) return false;
        const auto* r = doc.timeline.find(regionId);
        if (r->isPattern()) {
            const auto saved = doc;
            auto next = doc.channelRack;
            const int slot = next.patternCount;
            const int source = doc.rackPatternForClip(r->rackClip);
            if (source < 0 || !next.addPattern(source)) return false;
            next.selectedPattern = doc.channelRack.selectedPattern;
            if (!doc.setRack(std::move(next))) return false;
            auto* own = doc.timeline.find(regionId);
            own->source = slot; own->rackClip = doc.channelRack.clips[static_cast<std::size_t>(slot)];
            if (!doc.timeline.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) { doc = saved; return false; }
            pushRackToEngine(); commit("Make placement unique"); return true;
        }
        const int row = r->row, from = r->source;
        if (isTrackFrozen(row)) return false;
        const auto content = doc.placementContent(*r);
        if (!content.isAudioClip()) {
            const auto name = r->ownsClip() ? doc.placementName(r->clip) : doc.scenes[static_cast<std::size_t>(from)].name;
            const auto saved = doc;
            const auto id = doc.addPlacementClip(content.pattern, name);
            auto* region = doc.timeline.find(regionId);
            region->clip = id.value; region->source = -1;
            if (!id || !doc.timeline.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) { doc = saved; return false; }
            doc.prune();
            commit("Make placement unique");
            return true;
        }
        const auto saved = doc;
        const auto used = [&](int scene) {
            if (doc.cellClip(row, scene)) return true;
            for (const auto& x : doc.timeline.regions) if (x.row == row && x.source == scene) return true;
            for (const auto& x : doc.timeline.takes) if (x.row == row && x.source == scene) return true;
            for (const auto& x : doc.timeline.frozenOriginals) if (x.row == row && x.source == scene) return true;
            return false;
        };
        int free = -1;
        for (int s = 0; s < doc.sceneCount() && free < 0; ++s) if (!used(s)) free = s;
        if (free < 0) {
            if (!doc.canAddScene() || !insertScene(doc.sceneCount(), nullptr)) { doc = saved; return false; }
            free = doc.sceneCount() - 1;
        }
        doc.cells[static_cast<std::size_t>(free)][static_cast<std::size_t>(row)] = doc.library.duplicateClip(doc.cellClip(row, from));
        doc.timeline.find(regionId)->source = free;
        if (!doc.timeline.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) { doc = saved; return false; }
        commit("Make placement unique", row, free);
        return true;
    }

    void republishIfLive(int track, int scene) {
        if (!doc.isValidCell(track, scene)) return;
        const auto index = static_cast<std::size_t>(track);
        if (engine.activePianoRollClip[index].load() == scene || engine.requestedPianoRollClip[index].load() == scene)
            publishClip(track, scene);
    }

    // Edits -----------------------------------------------------------------
    void setStepOn(int step, bool on) {
        if (!validStep(step)) return;
        doc.steps.on[static_cast<std::size_t>(step)] = on;
        engine.steps[static_cast<std::size_t>(step)].store(on);
    }
    void setStepPitch(int step, int pitch) {
        if (!validStep(step)) return;
        doc.steps.pitch[static_cast<std::size_t>(step)] = juce::jlimit(0, 127, pitch);
        engine.stepPitches[static_cast<std::size_t>(step)].store(doc.steps.pitch[static_cast<std::size_t>(step)]);
    }
    void setStepVelocity(int step, int velocity) {
        if (!validStep(step)) return;
        doc.steps.velocity[static_cast<std::size_t>(step)] = juce::jlimit(1, 127, velocity);
        engine.stepVelocities[static_cast<std::size_t>(step)].store(doc.steps.velocity[static_cast<std::size_t>(step)]);
    }
    void setBpm(int bpm) {
        const bool unfreezing=juce::jlimit(60,200,bpm)!=doc.bpm && unfreezeForTempo();
        doc.bpm = juce::jlimit(60, 200, bpm);
        engine.bpm.store(doc.bpm);
        if(unfreezing && onDocumentRestored)onDocumentRestored();
    }
    // Frozen audio was rendered at the old tempo, so a tempo edit unfreezes every frozen track first.
    bool unfreezeForTempo() {
        if(!anyFrozenTrack())return false;
        for(int t=0;t<doc.trackCount();++t)if(isTrackFrozen(t)) {
            const auto scene=doc.tracks[static_cast<std::size_t>(t)].settings.freezeScene;
            doc.timeline.unfreezeTrack(t);doc.tracks[static_cast<std::size_t>(t)].settings.freezeScene=-1;
            if(doc.isValidCell(t,scene))doc.clearClip(t,scene);
        }
        doc.timeline.lengthBars=std::max(doc.timeline.freezeBaseLengthBars,doc.timeline.minLengthBars());
        doc.timeline.freezeBaseLengthBars=0;
        pushDocumentToEngine();
        status("Tracks unfrozen for tempo change.",{});
        return true;
    }
    // The song's tempo map: its tempo changes after the project tempo.
    timeline::TempoMap songTempo() const noexcept { return {doc.timeline.tempoChanges, static_cast<double>(doc.bpm)}; }
    // Tempo changes on the Song, each one undo step. Frozen tracks are unfrozen first.
    bool setTempoChange(std::int64_t tick, double bpm) {
        return editTempoMap("Tempo change", [=](timeline::Timeline& t) { return t.setTempoChange(tick, bpm); });
    }
    bool moveTempoChange(std::int64_t from, std::int64_t to) {
        return editTempoMap("Move tempo change", [=](timeline::Timeline& t) { return t.moveTempoChange(from, to); });
    }
    bool removeTempoChange(std::int64_t tick) {
        return editTempoMap("Remove tempo change", [=](timeline::Timeline& t) { return t.removeTempoChange(tick); });
    }
    bool clearTempoChanges() {
        return editTempoMap("Remove tempo changes", [](timeline::Timeline& t) { return t.clearTempoChanges(); });
    }
    bool editTempoMap(const juce::String& label, const std::function<bool(timeline::Timeline&)>& edit) {
        if (busy || isRecording()) return false;
        auto probe = doc.timeline;
        if (!edit(probe) || !probe.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) return false;
        const bool unfrozen = unfreezeForTempo();
        const bool changed = editTimeline(label, edit);
        if (unfrozen && !changed) commit("Unfreeze tracks", -1, -1, {});
        if (unfrozen && onDocumentRestored) onDocumentRestored();
        return changed;
    }
    void setLivePreset(int preset) {
        doc.livePreset = preset;
        engine.preset.store(preset);
    }
    void setLaunchQuantization(int steps) {
        doc.launchQuantizationSteps = steps == 4 ? 4 : 16;
        engine.launchQuantizationSteps.store(doc.launchQuantizationSteps);
    }

    // Called by the piano roll as notes change. Editing a clip that is not
    // playing queues it to launch, so changes are heard straight away.
    void setClipPattern(int track, int scene, const PianoRollPattern& pattern, bool publishToEngine) {
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

    // Called by the piano roll as a placement clip's notes change: every region that plays it follows.
    bool setPlacementPattern(std::uint64_t clip, const PianoRollPattern& pattern) {
        const auto id = alien::library::ClipId{clip};
        if (busy || placementTrack(clip) < 0 || doc.library.clip(id) == nullptr) return false;
        for (const auto& r : doc.timeline.regions) if (r.clip == clip && isTrackFrozen(r.row)) return false;
        if (samePianoRollPattern(*doc.contentOf(id).pattern, pattern)) return false;
        if (!doc.setPlacementNotes(id, std::make_shared<const PianoRollPattern>(pattern))) return false;
        publishArrangement();
        return true;
    }
    // A placement clip with `notes`, for a paste that brings back a clip that was cut. Nothing plays it until a
    // region names it (an unused one is dropped at the next edit). Returns its id, or 0.
    std::uint64_t restorePlacementClip(std::shared_ptr<const PianoRollPattern> notes, const juce::String& name) {
        if (busy || notes == nullptr || !notes->valid()) return 0;
        return doc.addPlacementClip(std::move(notes), name).value;
    }
    // The track whose Song regions play placement clip `clip` (-1 when none does).
    int placementTrack(std::uint64_t clip) const {
        for (const auto& r : doc.timeline.regions) if (clip != 0 && r.clip == clip) return r.row;
        return -1;
    }

    bool removeAudio(int track, int scene) {
        if (!doc.isValidCell(track, scene) || isTrackFrozen(track)) return false;
        auto clip = doc.clip(track, scene);
        if (!clip.isAudioClip()) return false; // a loaded clip, or an offline reference
        clip.audio.reset();
        clip.audioFileName.clear();
        clip.audioFilePath.clear();
        doc.setClip(track, scene, clip);
        republishIfLive(track, scene);
        republishLinked(track, scene);
        return true;
    }

    // Files -----------------------------------------------------------------
    // done(true) once the project is safely on disk; done(false) if the user
    // cancelled or the save failed.
    void save(std::function<void(bool)> done = {}) {
        if (projectFile.existsAsFile()) writeProject(projectFile, false, std::move(done));
        else chooseSavePath(false, std::move(done));
    }

    void chooseSavePath(bool portable, std::function<void(bool)> done = {}) {
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

    void chooseProjectToOpen() {
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

    void chooseAudioFile() {
        audioFileChooser = std::make_unique<juce::FileChooser>("Import audio clip", juce::File{}, "*.wav;*.mp3");
        std::weak_ptr<int> token = alive;
        audioFileChooser->launchAsync(juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
            [this, token](const juce::FileChooser& chooser) {
                if (token.lock() != nullptr && chooser.getResult().existsAsFile())
                    importAudio(chooser.getResult(), doc.selectedTrack, doc.selectedScene);
            });
    }

    void writeProject(juce::File file, bool portable, std::function<void(bool)> done = {}) {
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

    void openProject(const juce::File& file, bool recovered = false) {
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

    void importAudio(const juce::File& file, int track, int scene) {
        if(isTrackFrozen(track)){importStatus("Unfreeze the track before replacing its audio.",{});return;}
        if (busy || !doc.isValidCell(track, scene)) return;
        setBusy(true);
        importStatus(ui8("Loading audio\xe2\x80\xa6"), file.getFullPathName());
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, track, scene] {
            auto result = std::make_shared<AudioClipImportResult>(importSessionAudioClip(file));
            juce::MessageManager::callAsync([this, token, file, track, scene, result] {
                if (token.lock() != nullptr) { setBusy(false); finishImport(file, track, scene, *result); }
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }

    std::shared_ptr<ArrangementPlayback> captureArrangement() const {
        auto song = std::make_shared<ArrangementPlayback>();
        song->timeline = doc.timeline;
        for (int s = 0; s < doc.sceneCount(); ++s) for (int t = 0; t < doc.trackCount(); ++t) {
            // An offline audio clip stays silent; it must not fall back to playing stray notes.
            if (!doc.clip(t, s).audioOffline()) song->clips[s][t].pattern = doc.clip(t, s).pattern;   // shared, not copied
            song->clips[s][t].audio = doc.clip(t, s).audio.get();
        }
        for (const auto id : doc.placementClips()) song->placements.push_back({id, {doc.contentOf(alien::library::ClipId{id}).pattern, nullptr}});
        return song;
    }
    // What the engine currently holds. Commits that do not touch the song (mixer,
    // tempo, names, plugin state) must not rebuild, validate and re-send a snapshot.
    // Shared pointers keep every compared clip alive, so an address can never be
    // reused by a different clip while it is being compared.
    struct PublishedSong {
        bool known{};
        alien::timeline::Timeline timeline;
        int tracks{}, scenes{};
        std::vector<std::shared_ptr<const PianoRollPattern>> patterns;
        std::vector<std::shared_ptr<const SessionAudioClip>> audio;
        std::vector<std::shared_ptr<const PianoRollPattern>> placements;   // by doc.placementClips()
    };
    bool songDiffersFromPublished() const {
        if (!published.known || published.tracks != doc.trackCount() || published.scenes != doc.sceneCount()
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
    void rememberPublishedSong() {
        published.known = true;
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
    void publishArrangement() {
        if (!songDiffersFromPublished()) return;
        if (engine.arrangementMailbox.publish(captureArrangement())) rememberPublishedSong();
    }
    // The Arrangement timeline ------------------------------------------------------
    const alien::timeline::Timeline& timeline() const noexcept { return doc.timeline; }

    // Applies `edit` to a copy of the timeline. When it changes something valid for
    // this project the copy becomes the timeline and the change is one undo step
    // (edits with the same coalesceKey made in quick succession merge). Playback
    // follows without restarting. Returns whether anything changed.
    bool editTimeline(const juce::String& label, const std::function<bool(alien::timeline::Timeline&)>& edit,
                      const juce::String& coalesceKey = {}) {
        if (busy) return false;
        auto next = doc.timeline;
        next.tempo = doc.bpm;
        if (!edit(next)) return false;
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
    // How long a clip or pattern naturally lasts on the timeline, in ticks: its loop
    // for MIDI and rack patterns, its running time at the current tempo for audio
    // (rounded up to a sixteenth so it ends on the grid).
    std::int64_t naturalLength(int row, int source, std::int64_t start = 0) const {
        using namespace alien::timeline;
        if (isPatternRow(row)) {
            if (!doc.channelRack.validPattern(source)) return ticksPerBar;
            return static_cast<std::int64_t>(doc.channelRack.pattern(source).length) * ticksPerStep;
        }
        if (!doc.isValidCell(row, source)) return ticksPerBar;
        const auto& clip = doc.clip(row, source);
        if (clip.audio != nullptr && clip.audio->sampleRate > 0.0) {
            const double seconds = static_cast<double>(clip.audio->frames) / clip.audio->sampleRate;
            const double ticks = songTempo().tickAfter(static_cast<double>(start), seconds) - static_cast<double>(start);
            return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(ticks / ticksPerStep)) * ticksPerStep, ticksPerStep, maxLength);
        }
        return static_cast<std::int64_t>(clip.notes().loopSteps) * ticksPerStep;
    }
    // Where the song starts and is shown when it is next played.
    bool placeRegion(int row, int source, std::int64_t start, std::uint32_t* idOut = nullptr) {
        if (!timeline::isPatternRow(row) && doc.isValidCell(row, source) && doc.clip(row, source).rackClip != 0) {
            source = rackPatternForClip(doc.clip(row, source).rackClip);
            row = timeline::rackRowBase;
        }
        const auto length = naturalLength(row, source, start);
        return editTimeline(alien::timeline::isPatternRow(row) ? "Place pattern" : "Place clip", [&](alien::timeline::Timeline& t) {
            alien::timeline::Region r; r.row = row; r.source = source; r.start = start; r.length = length;
            return t.place(r, idOut);
        });
    }
    // A new, empty MIDI clip for a track, put on the timeline in one undo step. It takes the first Session cell of that track
    // that is empty and that the timeline does not already use, so every new clip is a clip of its own; when there is no such
    // cell a scene is added. The clip is selected, ready to be opened in the piano roll. Returns false (and changes nothing)
    // when the track does not exist, the project is busy, or there is no room for another scene.
    bool placeNewClip(int row, std::int64_t start, std::uint32_t* idOut = nullptr, int* sceneOut = nullptr, bool* sceneAddedOut = nullptr) {
        using namespace alien::timeline;
        if (busy || isTrackFrozen(row) || isPatternRow(row) || row < 0 || row >= doc.trackCount() || start < 0) return false;
        const auto usedByTimeline = [&](int scene) {
            for (const auto& r : doc.timeline.takes) if (r.row == row && r.source == scene) return true;
            for (const auto& r : doc.timeline.regions) if (r.row == row && r.source == scene) return true;
            return false;
        };
        const auto isFree = [&](int scene) { return scene >= 0 && scene < doc.sceneCount() && doc.clip(row, scene).isEmpty() && !usedByTimeline(scene); };
        int scene = -1;
        if (isFree(doc.selectedScene)) scene = doc.selectedScene;
        for (int s = 0; s < doc.sceneCount() && scene < 0; ++s) if (isFree(s)) scene = s;
        const bool needsScene = scene < 0;
        if (needsScene && !doc.canAddScene()) return false;
        const auto saved = doc;   // a failed placement must leave no trace, not even the new scene
        if (needsScene) {
            if (!insertScene(doc.sceneCount(), nullptr)) return false;
            scene = doc.sceneCount() - 1;
        }
        std::uint32_t id = 0;
        auto next = doc.timeline;
        next.tempo = doc.bpm;
        Region region;
        region.row = row; region.source = scene; region.start = start; region.length = naturalLength(row, scene, start);
        if (!next.place(region, &id) || !next.valid(doc.trackCount(), doc.sceneCount(), doc.channelRack.patternCount)) {
            doc = saved;
            return false;
        }
        doc.timeline = std::move(next);
        selectClip(row, scene);
        commit("New clip", row, scene);
        if (idOut != nullptr) *idOut = id;
        if (sceneOut != nullptr) *sceneOut = scene;
        if (sceneAddedOut != nullptr) *sceneAddedOut = needsScene;
        return true;
    }
    // A finished Song-view audio take on one track: its file, the loaded (or streamed) audio, and
    // where it goes on the timeline.
    struct RecordedTake {
        int track{-1};
        juce::File file;
        std::shared_ptr<SessionAudioClip> clip;
        alien::TakePlacement placement;
        // A loop recording: one placement per pass of the loop (placement is then unused), and the pass
        // that goes on the song. The others are kept in the take list.
        std::vector<alien::TakePlacement> passes;
        std::size_t activePass{};
    };
    // Puts every take of one recording on the timeline as one undo step ("Record take"). Each take
    // becomes the audio of a Session cell of its track that is empty and unused by the timeline (a scene
    // is added when there is none) and a region at its placement, over whatever was there; each pass of
    // a loop recording becomes a take of that cell, and only the active pass is put on the song. When the
    // take list has no room for every pass, the earliest are left out. Nothing changes when any take
    // cannot be placed. Returns how many tracks' takes were placed.
    int addRecordedTakes(const std::vector<RecordedTake>& takes) {
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
                if (doc.clip(row, s).isEmpty() && !usedByTimeline(s)) scene = s;
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
        commit("Record take", cells.size() == 1 ? cells.front().first : -1, cells.size() == 1 ? cells.front().second : -1);
        return static_cast<int>(cells.size());
    }
    bool compRecordedTake(std::uint32_t id, std::int64_t from, std::int64_t to) {
        if (isRecording() || busy) return false;
        const auto found = std::find_if(doc.timeline.takes.begin(), doc.timeline.takes.end(),
            [=](const timeline::Region& r) { return r.id == id; });
        if (found == doc.timeline.takes.end() || !doc.isValidCell(found->row, found->source)
            || !doc.clip(found->row, found->source).isAudioClip()) return false;
        return editTimeline("Comp recording take", [=](timeline::Timeline& t) { return t.compTake(id, from, to); });
    }
    bool isTrackFrozen(int track) const noexcept {
        return track>=0 && track<doc.trackCount() && doc.tracks[static_cast<std::size_t>(track)].settings.frozen();
    }
    bool anyFrozenTrack() const noexcept {
        for(const auto& track:doc.tracks) if(track.settings.frozen())return true;
        return false;
    }
    std::int64_t freezeEndTick(int track) const noexcept {
        std::int64_t end=0; for(const auto& region:doc.timeline.regions)if(region.row==track)end=std::max(end,region.end());
        return end;
    }
    juce::String freezeRefusal(int track) const {
        for (int s = 0; s < doc.sceneCount(); ++s)
            if (doc.isValidCell(track, s) && doc.clip(track, s).rackClip != 0)
                return "Clear rack launch assignments before freezing this Session track. Rack channels keep their own routing.";
        if(busy || isRecording())return "Finish the current operation before freezing.";
        if(track<0 || track>=doc.trackCount())return "Choose a Session track.";
        if(isTrackFrozen(track))return "Track is already frozen.";
        if(freezeEndTick(track)==0)return "Place this track's clips in Song before freezing.";
        for(int c=0;c<doc.channelRack.channelCount;++c)if(doc.channelRack.channels[static_cast<std::size_t>(c)].output==track)
            return "Freeze currently supports Session tracks without Channel Rack inputs. Reroute those channels first.";
        for(const auto& region:doc.timeline.regions)if(region.row==track) {
            if(doc.placementContent(region).audioOffline())return "Relink missing audio before freezing.";
            if(region.group!=0)for(const auto& other:doc.timeline.regions)if(other.group==region.group && other.row!=track)
                return "Ungroup clips shared with other tracks before freezing.";
        }
        return {};
    }
    // Called only after the worker has rendered/imported a fresh cache. One undo transaction.
    bool installFrozenTrack(int track,const juce::File& file,std::shared_ptr<SessionAudioClip> audio,
                            const timeline::Timeline& expected) {
        if(freezeRefusal(track).isNotEmpty() || doc.timeline!=expected || !audio || !audio->valid())return false;
        const auto saved=doc; const auto retainedBefore=retained.size(); const auto bytesBefore=retainedBytes;
        auto fail=[&] { doc=saved; retained.resize(retainedBefore);retainedBytes=bytesBefore;return false; };
        int scene=-1;
        for(int i=0;i<doc.sceneCount() && scene<0;++i)if(doc.clip(track,i).isEmpty()) {
            bool used=false;
            for(const auto* regions:{&doc.timeline.regions,&doc.timeline.takes,&doc.timeline.frozenOriginals})
                for(const auto& region:*regions)used=used || (region.row==track && region.source==i);
            if(!used)scene=i;
        }
        if(scene<0){if(!insertScene(doc.sceneCount(),nullptr))return fail();scene=doc.sceneCount()-1;}
        if(!retain(audio))return fail();
        auto cache=doc.clip(track,scene);cache.audio=audio;cache.audioFilePath=file.getFullPathName();cache.audioFileName=file.getFileName();
        doc.setClip(track,scene,cache);
        if(!anyFrozenTrack())doc.timeline.freezeBaseLengthBars=doc.timeline.lengthBars;
        const double ticks=songTempo().tickAfter(0.0,static_cast<double>(audio->frames)/audio->sampleRate);
        if(!std::isfinite(ticks) || ticks<1 || ticks>timeline::maxLength || !doc.timeline.freezeTrack(track,scene,static_cast<std::int64_t>(std::ceil(ticks)),1.0/ToneEngine::audioClipLevel))return fail();
        doc.tracks[static_cast<std::size_t>(track)].settings.freezeScene=scene;
        if(!toProjectData(doc).valid())return fail();
        pushDocumentToEngine();commit("Freeze track");if(onDocumentRestored)onDocumentRestored();return true;
    }
    bool unfreezeTrack(int track) {
        if(busy || isRecording() || !isTrackFrozen(track))return false;
        const int scene=doc.tracks[static_cast<std::size_t>(track)].settings.freezeScene;
        if(!doc.timeline.unfreezeTrack(track))return false;
        doc.tracks[static_cast<std::size_t>(track)].settings.freezeScene=-1;
        if(doc.isValidCell(track,scene))doc.clearClip(track,scene);
        if(!anyFrozenTrack()) {
            doc.timeline.lengthBars=std::max(doc.timeline.freezeBaseLengthBars,doc.timeline.minLengthBars());
            doc.timeline.freezeBaseLengthBars=0;
        }
        pushDocumentToEngine();commit("Unfreeze track");if(onDocumentRestored)onDocumentRestored();return true;
    }

    // Deleting a pattern renumbers the ones after it, and the timeline must follow.
    bool deleteRackPattern(int index) {
        auto next = doc.channelRack;
        if (!next.deletePattern(index) || !doc.setRack(std::move(next))) return false;
        // setRack remaps each remaining Song reference by clip identity; in-use sources are refused.
        pushRackToEngine();
        commit("Delete pattern");
        return true;
    }
    void clearDrum(int track, int pad) {
        if(isTrackFrozen(track))return;
        if (track < 0 || track >= doc.trackCount() || pad < 0 || pad >= drumPadCount) return;
        doc.tracks[track].drumPaths[pad].clear();
        doc.tracks[track].drumAssets[pad].reset();
        pushDocumentToEngine(); commit("Clear drum pad");
    }
    void loadDrum(const juce::File& file, int track, int pad) {
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
                for (auto& track : doc.tracks) if (track.id == trackId) {
                    if (!result->clip || !retain(result->clip)) { importStatus("Could not load drum sample (format or memory limit)", {}); return; }
                    track.drumAssets[pad] = result->clip;
                    track.drumPaths[pad] = file.getFullPathName();
                    track.settings.instrument = 1;
                    pushDocumentToEngine(); commit("Load drum sample");
                    if (onDocumentRestored) onDocumentRestored();
                    return;
                }
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }
    // Channel Rack ------------------------------------------------------------
    const rack::Project& channelRack() const noexcept { return doc.channelRack; }
    // Sample assets for a render on another thread (null for synth channels, silent channels or missing files).
    // Synth presets travel in the rack snapshot. Loaded files in `keepAlive` must outlive the render.
    std::array<const SessionAudioClip*, rack::maxChannels> rackClipsForExport(std::vector<std::shared_ptr<const SessionAudioClip>>& keepAlive) const {
        std::array<const SessionAudioClip*, rack::maxChannels> clips{};
        for (int c = 0; c < doc.channelRack.channelCount; ++c) {
            const auto& info = doc.channelRack.info[static_cast<std::size_t>(c)];
            if (info.isFile()) {
                const auto found = rackAudio.find(info.sample);
                if (found == rackAudio.end()) continue;
                keepAlive.push_back(found->second);
                clips[static_cast<std::size_t>(c)] = found->second.get();
            } else clips[static_cast<std::size_t>(c)] = rackClip(info);
        }
        return clips;
    }

    // Applies `edit` to a copy of the rack. When it changes something valid the
    // copy becomes the rack, the engine plays it, and the change is one undo
    // step (edits with the same coalesceKey made in quick succession merge, so a
    // dragged paint stroke or fader is one step). Choosing which pattern is shown
    // is not an edit and is not recorded.
    bool editRack(const juce::String& label, const std::function<bool(rack::Project&)>& edit,
                  const juce::String& coalesceKey = {}) {
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
    // The owner hosts the rack's plugin instruments: told when which channel holds which one has changed
    // (a plugin chosen or replaced, a channel added, removed, moved or duplicated, undo and redo).
    std::function<void()> onRackPluginsChanged;
    // Gives a channel a VST3 instrument (descriptionXml from the plugin list), named after it. One undo step.
    bool setRackPlugin(int channel, const juce::String& descriptionXml, const juce::String& name) {
        if (descriptionXml.isEmpty() || !doc.channelRack.validChannel(channel)) return false;
        const auto description = descriptionXml.toStdString();
        const auto instance = freshInstanceId();
        return editRack("Change channel instrument", [&](rack::Project& p) {
            const bool changed = p.setPlugin(channel, description, instance);
            if (changed && name.isNotEmpty()) p.renameChannel(channel, name.toStdString());
            return changed;
        });
    }
    // Keeps each rack plugin instrument's current state (from the host, by instance). Like capturing a track's
    // plugins before a save, it is not an undo step of its own.
    void setRackPluginStates(const std::vector<std::pair<std::uint64_t, juce::String>>& states) {
        auto next = doc.channelRack;
        bool changed = false;
        for (const auto& [instance, state] : states) changed = next.setPluginState(instance, state.toStdString()) || changed;
        if (!changed || !next.valid() || !doc.setRack(std::move(next))) return;
        runtimeDirty = true;
        pushRackToEngine();
    }
    // The rack's plugin instruments in channel order (an empty state where a channel has none).
    std::vector<PluginState> rackPlugins() const {
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
    bool selectRackPattern(int index) {
        if (!doc.channelRack.selectPattern(index)) return false;
        pushRackToEngine();
        return true;
    }
    // Resizes the Channel Rack's name column (clamped). Like choosing a pattern it is saved with the project but is
    // not an undo step, and the engine does not need to hear about it.
    bool setRackNameColumnWidth(int width) { return doc.channelRack.setNameColumnWidth(width); }
    // Sounds a channel now, whatever the transport is doing (clicking its name).
    void previewRackChannel(int channel, float velocity = 1.0f, int pitch = 0, bool held = false) {
        if (doc.channelRack.validChannel(channel))
            engine.rackAuditions.push(ToneEngine::rackAuditionEvent(channel, std::clamp(pitch, -rack::channelRootPitch, 127 - rack::channelRootPitch), held), velocity);
    }
    // Full piano-roll notes for this channel, without conversion to steps. Edits with the same
    // coalesceKey made in quick succession are one undo step, so a whole drag in the piano roll is one.
    bool setRackChannelNotes(int pattern, int channel, const PianoRollPattern& notes, const juce::String& coalesceKey = {},
                             rack::ChannelNotesFit* fit = nullptr) {
        if (!doc.channelRack.validPattern(pattern) || !doc.channelRack.validChannel(channel)) return false;
        if (fit) *fit = {};
        if (!notes.valid() || notes.loopSteps != doc.channelRack.pattern(pattern).length) return false;
        const auto existing = rack::channelNotes(doc.channelRack.pattern(pattern), channel);
        if (rack::Pattern::sameNotes(existing, notes)) return false;
        return editRack("Edit channel notes", [&](rack::Project& p) { return p.setChannelNotes(pattern, channel, notes); }, coalesceKey);
    }
    bool setRackSynth(int channel, int preset) {
        if (preset < 0 || preset >= alien::presetCount) return false;
        return editRack("Change channel instrument", [&](rack::Project& p) {
            const bool changed = p.setInstrumentPreset(channel, preset);
            if (changed) p.renameChannel(channel, alien::presetNames[static_cast<std::size_t>(preset)]);
            return changed;
        });
    }
    // Gives a channel one of the built-in drum sounds and names it after the sound.
    bool setRackBuiltin(int channel, const juce::String& soundId) {
        const auto* name = rack::builtinDisplayName(soundId.toStdString());
        if (name == nullptr) return false;
        const auto id = soundId.toStdString();
        return editRack("Change channel sound", [&](rack::Project& p) {
            bool changed = p.setSample(channel, "builtin:" + id);
            return p.renameChannel(channel, name) || changed;
        });
    }
    // Decodes an audio file off the message thread and gives it to a channel.
    void loadRackSample(const juce::File& file, int channel) {
        if (busy || !doc.channelRack.validChannel(channel)) return;
        setBusy(true);
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, channel] {
            auto result = std::make_shared<AudioClipImportResult>(decodeSessionAudioClip(file));
            juce::MessageManager::callAsync([this, token, file, channel, result] {
                if (!token.lock()) return;
                setBusy(false);
                const auto path = file.getFullPathName().toStdString();
                if (path.size() > static_cast<std::size_t>(rack::maxSampleBytes)) { importStatus("That file's path is too long to store in a project", file.getFullPathName()); return; }
                if (!result->clip || !retain(result->clip)) {
                    importStatus(result->clip ? "Session audio storage is full (512 MiB limit)" : "Could not load sample: " + result->error, file.getFullPathName());
                    return;
                }
                rackAudio[path] = result->clip;
                editRack("Load channel sample", [&](rack::Project& p) {
                    bool changed = p.setSample(channel, path);
                    return p.renameChannel(channel, file.getFileNameWithoutExtension().toStdString()) || changed;
                });
                pushRackToEngine(); // the same file loaded again is a new decode
                importStatus("Loaded " + file.getFileName(), file.getFullPathName());
                if (onDocumentRestored) onDocumentRestored();
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }
    // True when the channel names a file that is not loaded (moved or deleted).
    bool rackChannelOffline(int channel) const {
        if (!doc.channelRack.validChannel(channel)) return false;
        const auto& info = doc.channelRack.info[static_cast<std::size_t>(channel)];
        return info.isFile() && rackAudio.find(info.sample) == rackAudio.end();
    }

    bool installAudio(const juce::File& file, int track, int scene, std::shared_ptr<SessionAudioClip> clip) {
        if (!doc.isValidCell(track, scene) || !clip || isTrackFrozen(track)) return false;
        const auto bytes = clip->decodedBytes();
        if (bytes > maxSessionAudioBytes - retainedBytes) return false;
        finishImport(file, track, scene, {clip, {}});
        return true;
    }
    std::size_t availableAudioBytes() const noexcept { return maxSessionAudioBytes - retainedBytes; }

    // Audio the project refers to that is not loaded (see MissingMedia), clips
    // first in scene order, then drum pads.
    std::vector<MissingMedia> missingMedia() const {
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

    // Looks for every missing file by name inside `folder` and its subfolders,
    // loads what it finds off the message thread, and installs it all as one
    // undoable step. Files that are not found stay offline and keep their saved
    // reference, so nothing is lost by trying.
    void relinkMissingAudio(const juce::File& folder) {
        if (busy) return;
        const auto missing = missingMedia();
        if (missing.empty()) { importStatus("No audio is missing.", {}); return; }
        if (!folder.isDirectory()) { importStatus("Choose a folder to search for the missing audio.", {}); return; }
        auto wanted = std::make_shared<std::vector<RelinkWant>>();
        for (const auto& item : missing)
            wanted->push_back(item.rackChannel ? RelinkWant{item, 0u, 0u}
                : RelinkWant{item, doc.tracks[static_cast<std::size_t>(item.track)].id,
                             item.drumPad ? 0u : doc.scenes[static_cast<std::size_t>(item.slot)].id});
        setBusy(true);
        importStatus(ui8("Searching for missing audio\xe2\x80\xa6"), folder.getFullPathName());
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, folder, wanted] {
            auto outcome = std::make_shared<RelinkOutcome>();
            const MediaIndex index(folder);
            outcome->truncated = index.wasTruncated();
            // A source used by several clips is decoded once and shared; decoded audio is immutable.
            std::map<juce::String, RelinkedAudio> decodedByPath;
            for (const auto& want : *wanted) {
                auto found = decodedByPath.find(want.item.path);
                if (found == decodedByPath.end())
                    found = decodedByPath.emplace(want.item.path, decodeFirstCandidate(index.candidates(want.item.path))).first;
                outcome->found.push_back(found->second);
            }
            juce::MessageManager::callAsync([this, token, wanted, outcome, folder] {
                if (token.lock() != nullptr) finishRelink(*wanted, *outcome, folder);
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }

private:
    // The sound a channel plays: a built-in drum sound (generated once and kept
    // for the life of the program), a loaded file, or nothing (silent or offline).
    const SessionAudioClip* rackClip(const rack::ChannelInfo& info) const {
        if (info.isBuiltin()) return rack::builtinClip(info.builtinId()).get();
        if (info.isFile()) {
            const auto found = rackAudio.find(info.sample);
            return found == rackAudio.end() ? nullptr : found->second.get();
        }
        return nullptr;
    }
    // Hands the engine the rack and the sound for each channel. Skipped when
    // nothing changed, so unrelated edits do not rebuild the snapshot.
    static std::uint64_t freshInstanceId() {
        std::uint64_t id = 0;
        while (id == 0) id = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64());
        return id;
    }
    // A duplicated plugin channel is a second instance with the same sound: it gets an identity of its own.
    static void freshPluginInstances(rack::Project& p) {
        for (int c = 0; c < p.channelCount; ++c) {
            const auto& info = p.info[static_cast<std::size_t>(c)];
            if (!info.hasPlugin()) continue;
            for (int d = 0; d < c; ++d)
                if (p.info[static_cast<std::size_t>(d)].pluginInstance() == info.pluginInstance()) { p.renewPluginInstance(c, freshInstanceId()); break; }
        }
    }
    void notifyRackPlugins() {
        // One number for which channel holds which instrument, rather than a copy of every identity.
        std::uint64_t now = 1469598103934665603ull;
        for (int c = 0; c < doc.channelRack.channelCount; ++c)
            now = (now ^ doc.channelRack.info[static_cast<std::size_t>(c)].pluginInstance()) * 1099511628211ull + static_cast<std::uint64_t>(c);
        if (now == notifiedRackPlugins) return;
        notifiedRackPlugins = now;
        if (onRackPluginsChanged) onRackPluginsChanged();
    }
    std::uint64_t notifiedRackPlugins{1469598103934665603ull};
    void pushRackToEngine() {
        std::array<const SessionAudioClip*, rack::maxChannels> clips{};
        for (int c = 0; c < doc.channelRack.channelCount; ++c)
            clips[static_cast<std::size_t>(c)] = rackClip(doc.channelRack.info[static_cast<std::size_t>(c)]);
        if (rackPublished.known && rackPublished.clips == clips && rackPublished.project == doc.channelRack) return;
        for (std::size_t c = 0; c < clips.size(); ++c) engine.rackSamples[c].store(clips[c]);
        engine.rackMailbox.publish(doc.channelRack.snapshot());
        rackPublished.known = true; rackPublished.clips = clips; rackPublished.project = doc.channelRack;
    }
    bool retain(const std::shared_ptr<SessionAudioClip>& clip) {
        const auto bytes = clip->decodedBytes();
        if (bytes > maxSessionAudioBytes - retainedBytes) return false;
        if (clip->streamed() && !engine.retainDiskAudioSource(clip->streamSource)) return false;
        retained.push_back(clip); retainedBytes += bytes; return true;
    }
    void registerStream(const std::shared_ptr<SessionAudioClip>& clip) {
        if (clip && clip->streamed()) engine.retainDiskAudioSource(clip->streamSource);
    }
    struct RelinkWant {
        MissingMedia item;
        std::uint32_t trackId{}, sceneId{}; // identify the target even if tracks or scenes move while searching
    };
    struct RelinkOutcome {
        std::vector<RelinkedAudio> found; // parallel to the wanted list
        bool truncated{};
    };
    bool retainOnce(const std::shared_ptr<SessionAudioClip>& clip, std::set<const SessionAudioClip*>& counted) {
        if (counted.count(clip.get()) != 0) return true;
        if (!retain(clip)) return false;
        counted.insert(clip.get());
        return true;
    }
    void finishRelink(const std::vector<RelinkWant>& wanted, const RelinkOutcome& outcome, const juce::File& folder) {
        setBusy(false);
        int relinked = 0;
        bool memoryFull = false;
        std::set<const SessionAudioClip*> counted;
        juce::StringArray stillOffline;
        const auto trackIndex = [this](std::uint32_t id) {
            for (int t = 0; t < doc.trackCount(); ++t) if (doc.tracks[static_cast<std::size_t>(t)].id == id) return t;
            return -1;
        };
        const auto sceneIndex = [this](std::uint32_t id) {
            for (int s = 0; s < doc.sceneCount(); ++s) if (doc.scenes[static_cast<std::size_t>(s)].id == id) return s;
            return -1;
        };
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            const auto& want = wanted[i];
            const auto& found = outcome.found[i];
            if (want.item.rackChannel) {
                if (!found.clip) { stillOffline.addIfNotAlreadyThere(want.item.fileName()); continue; }
                const auto oldPath = want.item.path.toStdString();
                bool stillWanted = false; // skip anything the user changed while the search ran
                for (int c = 0; c < doc.channelRack.channelCount; ++c)
                    stillWanted = stillWanted || (doc.channelRack.info[static_cast<std::size_t>(c)].sample == oldPath && rackAudio.find(oldPath) == rackAudio.end());
                if (!stillWanted) continue;
                const auto newPath = found.file.getFullPathName().toStdString();
                if (newPath.size() > static_cast<std::size_t>(rack::maxSampleBytes) || !retainOnce(found.clip, counted)) {
                    memoryFull = memoryFull || newPath.size() <= static_cast<std::size_t>(rack::maxSampleBytes);
                    stillOffline.addIfNotAlreadyThere(want.item.fileName());
                    continue;
                }
                rackAudio[newPath] = found.clip;
                for (int c = 0; c < doc.channelRack.channelCount; ++c)
                    if (doc.channelRack.info[static_cast<std::size_t>(c)].sample == oldPath) doc.channelRack.setSample(c, newPath);
                ++relinked;
                continue;
            }
            const int track = trackIndex(want.trackId);
            const int scene = want.item.drumPad ? 0 : sceneIndex(want.sceneId);
            if (track < 0 || scene < 0) continue; // removed while searching
            auto& owner = doc.tracks[static_cast<std::size_t>(track)];
            const auto pad = static_cast<std::size_t>(want.item.slot);
            // Skip anything the user changed while the search ran.
            const bool unchanged = want.item.drumPad
                ? owner.drumPaths[pad] == want.item.path && owner.drumAssets[pad] == nullptr
                : doc.clip(track, scene).audioOffline() && doc.clip(track, scene).audioFilePath == want.item.path;
            if (!unchanged) continue;
            if (!found.clip) { stillOffline.addIfNotAlreadyThere(want.item.fileName()); continue; }
            if (!retainOnce(found.clip, counted)) {
                memoryFull = true;
                stillOffline.addIfNotAlreadyThere(want.item.fileName());
                continue;
            }
            if (want.item.drumPad) {
                owner.drumAssets[pad] = found.clip;
                owner.drumPaths[pad] = found.file.getFullPathName();
            } else {
                auto clip = doc.clip(track, scene);
                clip.audio = found.clip;
                clip.audioFilePath = found.file.getFullPathName();
                clip.audioFileName = found.file.getFileName();
                doc.setClip(track, scene, clip);
                republishIfLive(track, scene);
            }
            ++relinked;
        }
        if (relinked > 0) {
            pushDocumentToEngine();
            commit("Relink audio");
            if (onDocumentRestored) onDocumentRestored();
        }
        auto text = "Relinked " + juce::String(relinked) + " of " + juce::String(static_cast<int>(wanted.size()))
                  + " missing audio file" + (wanted.size() == 1 ? "" : "s");
        if (outcome.truncated) text += " (only the first " + juce::String(static_cast<int>(MediaIndex::maxFilesIndexed)) + " files were searched)";
        if (memoryFull) text += "; audio memory limit reached";
        if (!stillOffline.isEmpty()) {
            text += ". Still offline: ";
            for (int i = 0; i < stillOffline.size() && i < 3; ++i) text += (i ? ", " : "") + stillOffline[i];
            if (stillOffline.size() > 3) text += " and " + juce::String(stillOffline.size() - 3) + " more";
        }
        importStatus(text, folder.getFullPathName());
        status(text, folder.getFullPathName());
    }

    static juce::String describeOffline(const std::vector<MissingMedia>& missing) {
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

    struct LoadResult {
        SessionProjectData project;
        std::vector<std::shared_ptr<SessionAudioClip>> audio; // [scene * tracks + track]
        std::vector<std::array<std::shared_ptr<SessionAudioClip>, drumPadCount>> drums;
        std::vector<std::pair<std::string, std::shared_ptr<SessionAudioClip>>> rackClips; // by resolved path
        std::size_t decodedBytes{};
        std::vector<MissingMedia> missing; // referenced audio that could not be loaded; the project still opens
        juce::String error;
    };

    static juce::String ui8(const char* bytes) { return juce::String(juce::CharPointer_UTF8(bytes)); }
    static bool validStep(int step) noexcept { return step >= 0 && step < 16; }

    // Worker thread.
    static LoadResult loadProjectFile(const juce::File& file) {
        LoadResult result;
        if (file.getSize() <= 0 || file.getSize() > maxProjectFileBytes) {
            result.error = "Project file is empty or exceeds the 128 MiB limit.";
            return result;
        }
        if (!parseSessionProject(file.loadFileAsString(), result.project, result.error)) return result;
        const int tracks = result.project.trackCount(), scenes = result.project.sceneCount();
        result.audio.resize(static_cast<std::size_t>(tracks * scenes));
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
            }
        result.drums.resize(tracks);
        for (int t = 0; t < tracks; ++t) for (int p = 0; p < drumPadCount; ++p) {
            const auto& path = result.project.drumPaths[t][p];
            if (path.isEmpty()) continue;
            const auto location = resolveProjectAudio(file, path);
            const auto decoded = location == juce::File{} ? AudioClipImportResult{{}, "The saved audio location is not valid."}
                                                          : decodeSessionAudioClip(location);
            if (!decoded.clip) {
                result.missing.push_back({true, t, p, location == juce::File{} ? path : location.getFullPathName(), decoded.error});
                continue;
            }
            const auto bytes = decoded.clip->decodedBytes();
            if (bytes > maxSessionAudioBytes - result.decodedBytes) { result.error = "Drum audio exceeds memory limit"; return result; }
            result.decodedBytes += bytes; result.drums[t][p] = decoded.clip;
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

    void finishLoad(const juce::File& file, const LoadResult& result, bool recovered) {
        setBusy(false);
        if (result.error.isNotEmpty()) { status("Open failed: " + result.error, result.error); return; }
        if (result.decodedBytes > maxSessionAudioBytes - retainedBytes) {
            status("Open failed: retained audio memory limit reached",
                   "Close and restart Alien Studio to release audio from older sessions.");
            return;
        }
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
        for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) publishStop(track);

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

    void finishImport(const juce::File& file, int track, int scene, const AudioClipImportResult& result) {
        if (!result.clip) { importStatus(result.error, result.error); return; }
        if (!doc.isValidCell(track, scene) || isTrackFrozen(track)) return;
        if (!retain(result.clip)) {
            importStatus("Session audio storage is full (512 MiB limit)", "Session audio storage is full (512 MiB limit)");
            return;
        }
        auto clip = doc.clip(track, scene);
        clip.audio = result.clip;
        clip.audioFileName = file.getFileName();
        clip.audioFilePath = file.getFullPathName();
        doc.setClip(track, scene, clip);
        republishIfLive(track, scene);
        commit("Import audio", track, scene);
        importStatus("Imported " + file.getFileName(), file.getFullPathName());
        if (onClipContentChanged) onClipContentChanged(track, scene);
    }

    // Apply an undo/redo state and bring the engine in line with it.
    void restore(const ProjectDocument& state, const DocumentHistory::Step& step) {
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

    // Human-readable cause when the document cannot be serialised.
    static juce::String describeInvalidSession(const ProjectDocument& d) {
        auto name = oversizedPluginName(d.masterPlugins);
        for (std::size_t t = 0; name.isEmpty() && t < d.tracks.size(); ++t) name = oversizedPluginName(d.tracks[t].plugins);
        if (name.isNotEmpty())
            return "the saved state of " + name + " is too large to store (limit about "
                + juce::String(maxPluginStateChars / (1024 * 1024) * 3 / 4) + " MB per plugin)";
        return "session data is invalid";
    }

    void historyChanged() { if (onHistoryChanged) onHistoryChanged(); }

    // Recording helpers -----------------------------------------------------
    // Makes the target clip the one looping on its track. Overdub leaves a clip
    // that is already playing untouched; replace plays it silent so only the
    // new notes are heard while recording.
    void prepareClipForTake(int track, int scene, bool replace) {
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

    // Make the engine play what the document holds for this clip.
    void syncLiveClip(int track, int scene) {
        const auto index = static_cast<std::size_t>(track);
        if (engine.activePianoRollClip[index].load() == scene) engine.pianoRollNotes[index].publish(doc.clip(track, scene).notes());
        else republishIfLive(track, scene);
    }

    // The clip loops with everything recorded so far, so a take can be heard
    // layering up while it is made.
    void previewTake() {
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

    void finishTake() {
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
        setClipPattern(track, scene, merged, true);
        if (!changes) syncLiveClip(track, scene);
        commit("Record MIDI", track, scene);
        announce();
    }

    void publishStop(std::size_t track) {
        engine.requestedPianoRollClip[track].store(-1);
        SessionClipLaunch stopped;
        stopped.pattern.clear();
        stopped.clipIndex = -1;
        engine.pianoRollLaunches[track].publish(stopped);
    }

    // `copyFrom`: a scene to copy. Its clips are copied as independent clips (an edit of one copy
    // does not reach the other), as Duplicate scene always did.
    bool insertScene(int index, const std::vector<library::ClipId>* copyFrom) {
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

    void stopEverySlot() {
        abandonRecording(); // tracks or scenes are being renumbered, so the target is no longer valid
        engine.synchronizedSceneLaunch.store(false);
        for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
            publishStop(track);
            engine.requestedTrackRackClip[track].store(0);
            engine.sceneTrackRackClip[track].store(rack::RackPlayer::noLaunchRequest);
        }
    }

    void setBusy(bool b) { busy = b; if (onBusyChanged) onBusyChanged(b); }
    void status(const juce::String& text, const juce::String& tip) { if (onProjectStatus) onProjectStatus(text, tip); }
    void importStatus(const juce::String& text, const juce::String& tip) { if (onImportStatus) onImportStatus(text, tip); }

    struct Take {
        bool active{}, replace{}, refused{}, cancelled{};
        int track{-1}, scene{-1};
        std::uint32_t id{};
        std::uint64_t droppedAtStart{};
        std::size_t previewedNotes{};
        RecordingAssembler assembler;
    };
    Take take;
    struct SongTake {
        bool active{}, abandoned{};
        std::uint32_t id{};
        std::vector<std::pair<int, std::shared_ptr<DiskRecording>>> recordings; // track, its take
        double sampleRate{44100.0};
        int bpm{120}, offsetMs{};
        std::vector<timeline::TempoChange> tempoChanges;   // the song's tempo map when the take began
        bool punch{};
        std::int64_t punchIn{}, punchOut{};
        std::int64_t loopStart{}, loopEnd{};   // where each pass of a loop recording begins and ends
    };
    SongTake songTake;

    void stopSongTake() {
        // Cancelled before the engine saw the request: nothing was recorded.
        if (engine.recording.startRequested.exchange(false)) { finishSongTake(true); return; }
        engine.recording.stopRequested.store(true);
    }
    void pollSongTake() {
        bool ended = false, refused = false;
        for (RecordEvent event; engine.recording.events.pop(event);) {
            if (event.take != songTake.id) continue;
            if (event.kind == RecordEventKind::refused) refused = true;
            if (event.kind == RecordEventKind::end || event.kind == RecordEventKind::refused) { ended = true; break; }
        }
        if (ended) finishSongTake(refused);
    }
    // The engine has ended the take (or never started it) and no longer touches the sinks. The files are
    // finished and loaded off the message thread; then every take is placed in one undo step.
    void finishSongTake(bool refused) {
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
    std::uint32_t lastTakeId{};
    library::ClipId copiedCell{};                  // what Copy remembered, for Paste linked
    std::optional<SessionClip> copiedContent;      // and its content, for Paste (kept if the cell changes)

    ToneEngine& engine;
    ProjectDocument doc;
    juce::File projectFile;
    bool busy{};
    // Every decoded clip stays alive while the engine might reference it.
    std::vector<std::shared_ptr<const SessionAudioClip>> retained;
    std::size_t retainedBytes{};
    juce::ThreadPool pool{1};
    std::unique_ptr<juce::FileChooser> audioFileChooser, projectFileChooser;
    DocumentHistory history;
    std::uint64_t savedId{}, autosavedId{};
    bool runtimeDirty{};
    bool autosaveWarned{};
    PublishedSong published;
    // Decoded Channel Rack samples by path. Kept (with `retained`) for the life of
    // the controller so the engine can hold raw pointers to them.
    std::map<std::string, std::shared_ptr<const SessionAudioClip>> rackAudio;
    struct PublishedRack {
        bool known{};
        rack::Project project;
        std::array<const SessionAudioClip*, rack::maxChannels> clips{};
    } rackPublished;
    bool autosaving{};
    std::mutex recoveryMutex;
    std::atomic<std::uint64_t> recoveryGeneration{};
    juce::File recoveringFrom;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace alien
