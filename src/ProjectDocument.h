#pragma once
#include "PianoRollSequence.h"
#include "SessionAudioClip.h"
#include "SessionProject.h"
#include "TrackMixer.h"
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include <vector>

// The in-memory song: everything the user edits, independent of the GUI and of
// the audio engine. It is a value type whose clip contents are shared,
// immutable objects, so copying a whole document is cheap (pointer copies).
// Message thread only.
namespace alien {

inline const std::shared_ptr<const PianoRollPattern>& emptyPattern() {
    static const auto empty = std::make_shared<const PianoRollPattern>();
    return empty;
}

struct SessionClip {
    std::shared_ptr<const PianoRollPattern> pattern = emptyPattern();
    // Decoded audio is immutable and retained by the ProjectController for the
    // lifetime of the engine, so the engine may hold raw pointers to it.
    std::shared_ptr<const SessionAudioClip> audio;
    juce::String audioFileName, audioFilePath;

    const PianoRollPattern& notes() const noexcept { return *pattern; }
    bool hasAudio() const noexcept { return audio != nullptr; }
    bool isEmpty() const noexcept { return audio == nullptr && pattern->count == 0; }
};

struct SessionTrack {
    juce::String name;
    TrackSettings settings;
};

struct SessionScene {
    juce::String name;
};

struct StepSequence {
    std::array<bool, 16> on{};
    std::array<int, 16> pitch{};
    std::array<int, 16> velocity{};
    StepSequence() { pitch.fill(57); velocity.fill(102); }
};

struct ProjectDocument {
    std::vector<SessionTrack> tracks;
    std::vector<SessionScene> scenes;
    std::vector<std::vector<SessionClip>> clips; // [scene][track]
    int bpm{120};
    int launchQuantizationSteps{16};
    int livePreset{};
    int selectedTrack{};
    int selectedScene{};
    StepSequence steps;

    static ProjectDocument createDefault(int trackCount = sessionProjectTrackCount,
                                         int sceneCount = sessionProjectSceneCount) {
        ProjectDocument d;
        for (int t = 0; t < trackCount; ++t) d.tracks.push_back({defaultTrackName(t), {}});
        for (int s = 0; s < sceneCount; ++s) d.scenes.push_back({defaultSceneName(s)});
        d.clips.assign(static_cast<std::size_t>(sceneCount),
                       std::vector<SessionClip>(static_cast<std::size_t>(trackCount)));
        for (const int step : {0, 4, 8, 12}) d.steps.on[static_cast<std::size_t>(step)] = true;
        return d;
    }

    static juce::String defaultTrackName(int index) { return "Track " + juce::String(index + 1); }
    static juce::String defaultSceneName(int index) { return "Scene " + juce::String(index + 1); }

    int trackCount() const noexcept { return static_cast<int>(tracks.size()); }
    int sceneCount() const noexcept { return static_cast<int>(scenes.size()); }
    bool isValidCell(int track, int scene) const noexcept {
        return track >= 0 && track < trackCount() && scene >= 0 && scene < sceneCount();
    }
    SessionClip& clip(int track, int scene) {
        return clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
    }
    const SessionClip& clip(int track, int scene) const {
        return clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
    }
    SessionClip& selectedClip() { return clip(selectedTrack, selectedScene); }
    const SessionClip& selectedClip() const { return clip(selectedTrack, selectedScene); }

    void clampSelection() noexcept {
        selectedTrack = juce::jlimit(0, juce::jmax(0, trackCount() - 1), selectedTrack);
        selectedScene = juce::jlimit(0, juce::jmax(0, sceneCount() - 1), selectedScene);
    }
};

// Conversion to and from the on-disk session format. Audio paths are copied as
// strings; decoding is done separately by the loader.
inline SessionProjectData toProjectData(const ProjectDocument& doc) {
    SessionProjectData data;
    data.launchQuantizationSteps = doc.launchQuantizationSteps;
    data.selectedTrack = doc.selectedTrack;
    data.selectedScene = doc.selectedScene;
    data.bpm = doc.bpm;
    data.synthPreset = doc.livePreset;
    for (int t = 0; t < sessionProjectTrackCount && t < doc.trackCount(); ++t)
        data.trackSettings[static_cast<std::size_t>(t)] = doc.tracks[static_cast<std::size_t>(t)].settings;
    data.stepPattern = doc.steps.on;
    data.stepPitches = doc.steps.pitch;
    data.stepVelocities = doc.steps.velocity;
    for (int s = 0; s < sessionProjectSceneCount && s < doc.sceneCount(); ++s)
        for (int t = 0; t < sessionProjectTrackCount && t < doc.trackCount(); ++t) {
            auto& destination = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            destination.pattern = doc.clip(t, s).notes();
            destination.audioFilePath = doc.clip(t, s).audioFilePath;
        }
    return data;
}

inline ProjectDocument fromProjectData(const SessionProjectData& data) {
    auto doc = ProjectDocument::createDefault(sessionProjectTrackCount, sessionProjectSceneCount);
    doc.launchQuantizationSteps = data.launchQuantizationSteps;
    doc.selectedTrack = data.selectedTrack;
    doc.selectedScene = data.selectedScene;
    doc.bpm = data.bpm;
    doc.livePreset = data.synthPreset;
    for (int t = 0; t < sessionProjectTrackCount; ++t)
        doc.tracks[static_cast<std::size_t>(t)].settings = data.trackSettings[static_cast<std::size_t>(t)];
    doc.steps.on = data.stepPattern;
    doc.steps.pitch = data.stepPitches;
    doc.steps.velocity = data.stepVelocities;
    for (int s = 0; s < sessionProjectSceneCount; ++s)
        for (int t = 0; t < sessionProjectTrackCount; ++t) {
            const auto& source = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            auto& clip = doc.clip(t, s);
            clip.pattern = std::make_shared<const PianoRollPattern>(source.pattern);
            clip.audioFilePath = source.audioFilePath;
        }
    doc.clampSelection();
    return doc;
}

} // namespace alien
