#pragma once
#include "ProjectDocument.h"
#include "ToneEngine.h"
#include "AudioClipImport.h"
#include "PortableProject.h"
#include "DocumentHistory.h"
#include "PianoRollProcessing.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>

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
    std::function<void()> onHistoryChanged;       // undo/redo availability or unsaved state changed

    // Undo, redo and unsaved changes -------------------------------------
    // Call after every finished edit. focusTrack/focusScene name the clip the
    // edit changed (shown again when it is undone). Edits with the same
    // coalesceKey made in quick succession become one undo step.
    void commit(const juce::String& label, int focusTrack = -1, int focusScene = -1,
                const juce::String& coalesceKey = {}) {
        history.record(doc, label, focusTrack, focusScene, coalesceKey);
        historyChanged();
    }

    // Start a fresh history from the current document (e.g. at startup).
    void markAsNewBaseline() {
        history.reset(doc);
        savedId = history.currentId();
        historyChanged();
    }

    bool canUndo() const noexcept { return history.canUndo(); }
    bool canRedo() const noexcept { return history.canRedo(); }
    juce::String undoLabel() const { return history.undoLabel(); }
    juce::String redoLabel() const { return history.redoLabel(); }
    bool isDirty() const noexcept { return history.currentId() != savedId; }

    bool undo() {
        DocumentHistory::Step step;
        if (const auto* state = history.undo(step)) { restore(*state, step); return true; }
        return false;
    }

    bool redo() {
        DocumentHistory::Step step;
        if (const auto* state = history.redo(step)) { restore(*state, step); return true; }
        return false;
    }

    // Replace the song with an empty default project.
    void newProject() {
        engine.playing.store(false);
        if (beforeDocumentReplaced) beforeDocumentReplaced();
        stopEverySlot();
        doc = ProjectDocument::createDefault();
        projectFile = juce::File{};
        pushDocumentToEngine();
        history.reset(doc);
        savedId = history.currentId();
        deleteRecoveryFiles();
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

    // Write an autosave copy on the worker thread if there are unsaved edits
    // that have not been autosaved yet. Never touches the user's project file.
    void autosaveIfNeeded() {
        if (busy || autosaving || !isDirty() || history.currentId() == autosavedId) return;
        for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
        auto project = toProjectData(doc);
        if (!project.valid()) return;
        autosaving = true;
        const auto id = history.currentId();
        const auto original = projectFile.getFullPathName();
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, project, id, original] {
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
            juce::MessageManager::callAsync([this, token, id, ok] {
                if (token.lock() == nullptr) return;
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
    }

    // Track and scene structure --------------------------------------------
    // Appending never disturbs playback. Any other structural edit renumbers
    // tracks or scenes, so Session playback is stopped first; this keeps the
    // engine's per-track slots and clip indices consistent with the document.
    bool addTrack() {
        if (!doc.canAddTrack()) return false;
        doc.tracks.push_back({doc.nextTrackName(), {}, doc.nextTrackColour()});
        for (auto& scene : doc.clips) scene.emplace_back();
        publishStop(static_cast<std::size_t>(doc.trackCount() - 1));
        pushDocumentToEngine();
        return true;
    }

    bool deleteTrack(int track) {
        if (doc.trackCount() <= 1 || track < 0 || track >= doc.trackCount()) return false;
        stopEverySlot();
        doc.tracks.erase(doc.tracks.begin() + track);
        for (auto& scene : doc.clips) scene.erase(scene.begin() + track);
        if (doc.selectedTrack > track) --doc.selectedTrack;
        doc.clampSelection();
        pushDocumentToEngine();
        return true;
    }

    bool moveTrack(int track, int delta) {
        const int target = track + delta;
        if (track < 0 || track >= doc.trackCount() || target < 0 || target >= doc.trackCount()) return false;
        stopEverySlot();
        std::swap(doc.tracks[static_cast<std::size_t>(track)], doc.tracks[static_cast<std::size_t>(target)]);
        for (auto& scene : doc.clips)
            std::swap(scene[static_cast<std::size_t>(track)], scene[static_cast<std::size_t>(target)]);
        if (doc.selectedTrack == track) doc.selectedTrack = target;
        else if (doc.selectedTrack == target) doc.selectedTrack = track;
        pushDocumentToEngine();
        return true;
    }

    bool renameTrack(int track, const juce::String& name) {
        const auto trimmed = name.trim().substring(0, 64);
        if (track < 0 || track >= doc.trackCount() || trimmed.isEmpty()) return false;
        doc.tracks[static_cast<std::size_t>(track)].name = trimmed;
        return true;
    }

    bool addScene() { return insertScene(doc.sceneCount(), nullptr); }

    bool duplicateScene(int scene) {
        if (scene < 0 || scene >= doc.sceneCount()) return false;
        return insertScene(scene + 1, &doc.clips[static_cast<std::size_t>(scene)]);
    }

    bool deleteScene(int scene) {
        if (doc.sceneCount() <= 1 || scene < 0 || scene >= doc.sceneCount()) return false;
        stopEverySlot();
        doc.scenes.erase(doc.scenes.begin() + scene);
        doc.clips.erase(doc.clips.begin() + scene);
        if (doc.selectedScene > scene) --doc.selectedScene;
        doc.clampSelection();
        return true;
    }

    bool moveScene(int scene, int delta) {
        const int target = scene + delta;
        if (scene < 0 || scene >= doc.sceneCount() || target < 0 || target >= doc.sceneCount()) return false;
        stopEverySlot();
        std::swap(doc.scenes[static_cast<std::size_t>(scene)], doc.scenes[static_cast<std::size_t>(target)]);
        std::swap(doc.clips[static_cast<std::size_t>(scene)], doc.clips[static_cast<std::size_t>(target)]);
        if (doc.selectedScene == scene) doc.selectedScene = target;
        else if (doc.selectedScene == target) doc.selectedScene = scene;
        return true;
    }

    bool renameScene(int scene, const juce::String& name) {
        const auto trimmed = name.trim().substring(0, 64);
        if (scene < 0 || scene >= doc.sceneCount() || trimmed.isEmpty()) return false;
        doc.scenes[static_cast<std::size_t>(scene)].name = trimmed;
        return true;
    }

    // The mixer writes the engine directly while a control moves; this copies
    // the result back into the document.
    void trackSettingsEdited(int track) {
        if (track < 0 || track >= doc.trackCount() || track >= static_cast<int>(engine.tracks.size())) return;
        doc.tracks[static_cast<std::size_t>(track)].settings = engine.tracks[static_cast<std::size_t>(track)].snapshot();
    }

    // Playback --------------------------------------------------------------
    void selectClip(int track, int scene) {
        if (!doc.isValidCell(track, scene)) return;
        doc.selectedTrack = track;
        doc.selectedScene = scene;
    }

    void launchClip(int track, int scene) {
        selectClip(track, scene);
        publishClip(doc.selectedTrack, doc.selectedScene);
    }

    void launchScene(int scene) {
        scene = juce::jlimit(0, doc.sceneCount() - 1, scene);
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < doc.trackCount(); ++track) publishClip(track, scene);
    }

    void stopAllTracks() {
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < doc.trackCount(); ++track) publishStop(static_cast<std::size_t>(track));
    }

    void publishClip(int track, int scene) {
        if (!doc.isValidCell(track, scene)) return;
        const auto& clip = doc.clip(track, scene);
        const auto index = static_cast<std::size_t>(track);
        engine.requestedPianoRollClip[index].store(scene);
        SessionClipLaunch launch;
        launch.pattern = clip.hasAudio() ? PianoRollPattern{} : clip.notes();
        launch.audio = clip.audio.get();
        launch.clipIndex = scene;
        engine.pianoRollLaunches[index].publish(launch);
    }

    // Republish only when the clip is playing or queued, so content changes
    // are heard without launching anything new.
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
        doc.bpm = juce::jlimit(60, 200, bpm);
        engine.bpm.store(doc.bpm);
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
        if (!doc.isValidCell(track, scene)) return;
        auto& clip = doc.clip(track, scene);
        // Re-loading a clip into the editor reports identical notes; keep the
        // existing object so it does not look like an edit.
        if (samePianoRollPattern(*clip.pattern, pattern)) return;
        clip.pattern = std::make_shared<const PianoRollPattern>(pattern);
        if (!publishToEngine || clip.hasAudio()) return;
        const auto index = static_cast<std::size_t>(track);
        engine.requestedPianoRollClip[index].store(scene);
        if (engine.activePianoRollClip[index].load() == scene) engine.pianoRollNotes[index].publish(pattern);
        else publishClip(track, scene);
    }

    bool removeAudio(int track, int scene) {
        if (!doc.isValidCell(track, scene)) return false;
        auto& clip = doc.clip(track, scene);
        if (!clip.hasAudio()) return false;
        clip.audio.reset();
        clip.audioFileName.clear();
        clip.audioFilePath.clear();
        republishIfLive(track, scene);
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
        for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
        const auto project = toProjectData(doc);
        if (alien::serializeSessionProject(project).isEmpty()) {
            status("Could not save: session data is invalid", {});
            if (done) done(false);
            return;
        }
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
                // Edits made while the file was being written stay unsaved.
                // Point audio references at what was written (collected media), unless
                // the grid was restructured while saving. Keep edits made meanwhile.
                if (project.sceneCount() == doc.sceneCount() && project.trackCount() == doc.trackCount())
                    for (int s = 0; s < doc.sceneCount(); ++s)
                        for (int t = 0; t < doc.trackCount(); ++t) {
                            auto& current = doc.clip(t, s);
                            const auto& old = project.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                            const auto& written = saved->clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                            if (current.audioFilePath == old.audioFilePath && written.audioFilePath.isNotEmpty())
                                current.audioFilePath = resolveProjectAudio(target, written.audioFilePath).getFullPathName();
                        }
                projectFile = target;
                savedId = savingId;
                if (!isDirty()) deleteRecoveryFiles();
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
        importStatus(ui8("Loading audio\xe2\x80\xa6"), file.getFullPathName());
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file, track, scene] {
            auto result = std::make_shared<AudioClipImportResult>(decodeSessionAudioClip(file));
            juce::MessageManager::callAsync([this, token, file, track, scene, result] {
                if (token.lock() != nullptr) finishImport(file, track, scene, *result);
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }

private:
    struct LoadResult {
        SessionProjectData project;
        std::vector<std::shared_ptr<SessionAudioClip>> audio; // [scene * tracks + track]
        std::size_t decodedBytes{};
        juce::String error;
    };

    static juce::String ui8(const char* bytes) { return juce::String(juce::CharPointer_UTF8(bytes)); }
    static bool validStep(int step) noexcept { return step >= 0 && step < 16; }

    // Worker thread.
    static LoadResult loadProjectFile(const juce::File& file) {
        LoadResult result;
        if (file.getSize() <= 0 || file.getSize() > 4 * 1024 * 1024) {
            result.error = "Project file is empty or exceeds the 4 MiB limit.";
            return result;
        }
        if (!parseSessionProject(file.loadFileAsString(), result.project, result.error)) return result;
        const int tracks = result.project.trackCount(), scenes = result.project.sceneCount();
        result.audio.resize(static_cast<std::size_t>(tracks * scenes));
        for (int s = 0; s < scenes; ++s)
            for (int t = 0; t < tracks; ++t) {
                const auto& clip = result.project.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                if (clip.audioFilePath.isEmpty()) continue;
                const auto decoded = decodeSessionAudioClip(resolveProjectAudio(file, clip.audioFilePath));
                if (!decoded.clip) {
                    result.error = "Track " + juce::String(t + 1) + ", Scene " + juce::String(s + 1) + ": " + decoded.error;
                    return result;
                }
                const auto bytes = decoded.clip->frames * static_cast<std::size_t>(decoded.clip->channels) * sizeof(float);
                if (bytes > maxSessionAudioBytes - result.decodedBytes) {
                    result.error = "Project audio exceeds the 512 MiB session limit.";
                    return result;
                }
                result.decodedBytes += bytes;
                result.audio[static_cast<std::size_t>(s * tracks + t)] = decoded.clip;
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
        engine.playing.store(false);
        engine.synchronizedSceneLaunch.store(false);
        if (beforeDocumentReplaced) beforeDocumentReplaced();
        for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) publishStop(track);

        auto loaded = fromProjectData(result.project);
        for (int s = 0; s < loaded.sceneCount(); ++s)
            for (int t = 0; t < loaded.trackCount(); ++t) {
                auto& clip = loaded.clip(t, s);
                if (clip.audioFilePath.isEmpty()) continue;
                const auto resolved = resolveProjectAudio(file, clip.audioFilePath);
                clip.audioFilePath = resolved.getFullPathName();
                clip.audioFileName = resolved.getFileName();
                const auto& asset = result.audio[static_cast<std::size_t>(s * loaded.trackCount() + t)];
                clip.audio = asset;
                if (asset != nullptr) retained.push_back(asset);
            }
        retainedBytes += result.decodedBytes;
        doc = std::move(loaded);
        projectFile = recovered ? recoveringFrom : file;
        pushDocumentToEngine();
        history.reset(doc);
        // A recovered session has never been saved by the user.
        savedId = recovered ? 0 : history.currentId();
        autosavedId = 0;
        if (!recovered) deleteRecoveryFiles();
        if (onDocumentReplaced) onDocumentReplaced();
        historyChanged();
        if (recovered) status("Recovered unsaved work" + (projectFile != juce::File{} ? " for " + projectFile.getFileName() : juce::String{})
                              + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 save to keep it")), {});
        else status("Opened " + file.getFileName(), file.getFullPathName());
    }

    void finishImport(const juce::File& file, int track, int scene, const AudioClipImportResult& result) {
        if (!result.clip) { importStatus(result.error, result.error); return; }
        const auto bytes = result.clip->frames * static_cast<std::size_t>(result.clip->channels) * sizeof(float);
        if (bytes > maxSessionAudioBytes - retainedBytes) {
            importStatus("Session audio storage is full (512 MiB limit)", "Session audio storage is full (512 MiB limit)");
            return;
        }
        if (!doc.isValidCell(track, scene)) return;
        retainedBytes += bytes;
        retained.push_back(result.clip);
        auto& clip = doc.clip(track, scene);
        clip.audio = result.clip;
        clip.audioFileName = file.getFileName();
        clip.audioFilePath = file.getFullPathName();
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
        // Keep the user's current selection unless the step names a clip.
        doc.selectedTrack = step.focusTrack >= 0 ? step.focusTrack : previous.selectedTrack;
        doc.selectedScene = step.focusScene >= 0 ? step.focusScene : previous.selectedScene;
        doc.clampSelection();
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
                else if (!after.hasAudio() && before.pattern != after.pattern) engine.pianoRollNotes[index].publish(after.notes());
            }
        }
        if (onDocumentRestored) onDocumentRestored();
        historyChanged();
    }

    void historyChanged() { if (onHistoryChanged) onHistoryChanged(); }

    void publishStop(std::size_t track) {
        engine.requestedPianoRollClip[track].store(-1);
        SessionClipLaunch stopped;
        stopped.pattern.clear();
        stopped.clipIndex = -1;
        engine.pianoRollLaunches[track].publish(stopped);
    }

    bool insertScene(int index, const std::vector<SessionClip>* copyFrom) {
        if (!doc.canAddScene() || index < 0 || index > doc.sceneCount()) return false;
        // Inserting before existing scenes renumbers them.
        if (index < doc.sceneCount()) stopEverySlot();
        const auto clips = copyFrom != nullptr ? *copyFrom
                                               : std::vector<SessionClip>(static_cast<std::size_t>(doc.trackCount()));
        const auto name = copyFrom != nullptr ? doc.scenes[static_cast<std::size_t>(index - 1)].name + " copy"
                                              : doc.nextSceneName();
        doc.scenes.insert(doc.scenes.begin() + index, SessionScene{name});
        doc.clips.insert(doc.clips.begin() + index, clips);
        if (doc.selectedScene >= index && index < doc.sceneCount() - 1) ++doc.selectedScene;
        return true;
    }

    void stopEverySlot() {
        engine.synchronizedSceneLaunch.store(false);
        for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) publishStop(track);
    }

    void setBusy(bool b) { busy = b; if (onBusyChanged) onBusyChanged(b); }
    void status(const juce::String& text, const juce::String& tip) { if (onProjectStatus) onProjectStatus(text, tip); }
    void importStatus(const juce::String& text, const juce::String& tip) { if (onImportStatus) onImportStatus(text, tip); }

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
    bool autosaving{};
    juce::File recoveringFrom;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace alien
