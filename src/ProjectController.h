#pragma once
#include "ProjectDocument.h"
#include "ToneEngine.h"
#include "AudioClipImport.h"
#include "PortableProject.h"
#include <functional>
#include <memory>
#include <vector>

// Owns the ProjectDocument and is the only code that changes it. Every edit
// goes through here so the audio engine is kept in sync, and so later layers
// (undo, autosave, dirty tracking) have one place to hook into. File work and
// audio decoding run on a worker thread; results come back on the message
// thread. Message thread only; no GUI types.
namespace alien {

class ProjectController {
public:
    static constexpr std::size_t maxSessionAudioBytes = 512u * 1024u * 1024u;

    explicit ProjectController(ToneEngine& toneEngine)
        : engine(toneEngine), doc(ProjectDocument::createDefault()) {}

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
        for (int t = 0; t < doc.trackCount() && t < static_cast<int>(engine.tracks.size()); ++t)
            engine.tracks[static_cast<std::size_t>(t)].set(doc.tracks[static_cast<std::size_t>(t)].settings);
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
    void save() {
        if (projectFile.existsAsFile()) writeProject(projectFile, false);
        else chooseSavePath(false);
    }

    void chooseSavePath(bool portable) {
        projectFileChooser = std::make_unique<juce::FileChooser>("Save Alien Studio session",
            projectFile.getParentDirectory().exists()
                ? projectFile : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("AlienStudioSession.alienstudio"), "*.alienstudio");
        std::weak_ptr<int> token = alive;
        projectFileChooser->launchAsync(juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, token, portable](const juce::FileChooser& chooser) {
                if (token.lock() != nullptr && chooser.getResult() != juce::File{})
                    writeProject(chooser.getResult(), portable);
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

    void writeProject(juce::File file, bool portable) {
        if (busy) return;
        for (int t = 0; t < doc.trackCount(); ++t) trackSettingsEdited(t);
        const auto project = toProjectData(doc);
        if (alien::serializeSessionProject(project).isEmpty()) {
            status("Could not save: session data is invalid", {});
            return;
        }
        const auto target = file.hasFileExtension("alienstudio") ? file : file.withFileExtension("alienstudio");
        setBusy(true);
        status(portable ? ui8("Collecting audio\xe2\x80\xa6") : ui8("Saving project\xe2\x80\xa6"), {});
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, target, project, portable] {
            auto saved = std::make_shared<SessionProjectData>();
            juce::String error;
            const bool success = writeSessionProjectFile(target, project, portable, *saved, error);
            juce::MessageManager::callAsync([this, token, target, success, saved, project, error] {
                if (token.lock() == nullptr) return;
                setBusy(false);
                if (!success) { status("Save failed: " + error, error); return; }
                // Update only audio references from this snapshot; keep edits made while saving.
                for (int s = 0; s < sessionProjectSceneCount && s < doc.sceneCount(); ++s)
                    for (int t = 0; t < sessionProjectTrackCount && t < doc.trackCount(); ++t) {
                        auto& current = doc.clip(t, s);
                        const auto& old = project.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                        const auto& written = saved->clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                        if (current.audioFilePath == old.audioFilePath && written.audioFilePath.isNotEmpty())
                            current.audioFilePath = resolveProjectAudio(target, written.audioFilePath).getFullPathName();
                    }
                projectFile = target;
                status("Saved " + target.getFileName(), target.getFullPathName());
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        pool.addJob(std::move(job));
    }

    void openProject(const juce::File& file) {
        if (busy) return;
        setBusy(true);
        status(ui8("Opening project\xe2\x80\xa6"), {});
        std::weak_ptr<int> token = alive;
        std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, file] {
            auto result = std::make_shared<LoadResult>(loadProjectFile(file));
            juce::MessageManager::callAsync([this, token, file, result] {
                if (token.lock() != nullptr) finishLoad(file, *result);
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
        result.audio.resize(static_cast<std::size_t>(sessionProjectTrackCount * sessionProjectSceneCount));
        for (int s = 0; s < sessionProjectSceneCount; ++s)
            for (int t = 0; t < sessionProjectTrackCount; ++t) {
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
                result.audio[static_cast<std::size_t>(s * sessionProjectTrackCount + t)] = decoded.clip;
            }
        return result;
    }

    void finishLoad(const juce::File& file, const LoadResult& result) {
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
                const auto& asset = result.audio[static_cast<std::size_t>(s * sessionProjectTrackCount + t)];
                clip.audio = asset;
                if (asset != nullptr) retained.push_back(asset);
            }
        retainedBytes += result.decodedBytes;
        doc = std::move(loaded);
        projectFile = file;
        pushDocumentToEngine();
        if (onDocumentReplaced) onDocumentReplaced();
        status("Opened " + file.getFileName(), file.getFullPathName());
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
        importStatus("Imported " + file.getFileName(), file.getFullPathName());
        if (onClipContentChanged) onClipContentChanged(track, scene);
    }

    void publishStop(std::size_t track) {
        engine.requestedPianoRollClip[track].store(-1);
        SessionClipLaunch stopped;
        stopped.pattern.clear();
        stopped.clipIndex = -1;
        engine.pianoRollLaunches[track].publish(stopped);
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
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace alien
