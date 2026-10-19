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

// Process-unique identity for tracks and scenes, so restoring an undo state
// can tell a reorder (engine slots must be reset) from a content change.
inline std::uint32_t newObjectId() {
    static std::uint32_t next = 0;
    return ++next;
}

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
    int colour{}; // Index into the track colour palette.
    std::uint32_t id = newObjectId();
};

struct SessionScene {
    juce::String name;
    std::uint32_t id = newObjectId();
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
        for (int t = 0; t < trackCount; ++t) d.tracks.push_back({defaultTrackName(t), {}, t});
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

    bool canAddTrack() const noexcept { return trackCount() < maxSessionTracks; }
    bool canAddScene() const noexcept { return sceneCount() < maxSessionScenes; }

    // Next "Track N" / "Scene N" whose number is not already used.
    juce::String nextTrackName() const {
        for (int n = 1;; ++n) {
            bool used = false;
            for (const auto& t : tracks) used = used || t.name == defaultTrackName(n - 1);
            if (!used) return defaultTrackName(n - 1);
        }
    }
    juce::String nextSceneName() const {
        for (int n = 1;; ++n) {
            bool used = false;
            for (const auto& s : scenes) used = used || s.name == defaultSceneName(n - 1);
            if (!used) return defaultSceneName(n - 1);
        }
    }
    int nextTrackColour() const {
        for (int c = 0; c < 64; ++c) {
            bool used = false;
            for (const auto& t : tracks) used = used || t.colour % 8 == c % 8;
            if (!used) return c % 8;
        }
        return trackCount() % 8;
    }

    void clampSelection() noexcept {
        selectedTrack = juce::jlimit(0, juce::jmax(0, trackCount() - 1), selectedTrack);
        selectedScene = juce::jlimit(0, juce::jmax(0, sceneCount() - 1), selectedScene);
    }
};

// True when two documents have the same content (selection is ignored).
// Clip contents are compared by identity, which is exact because they are
// immutable and replaced on every edit.
inline bool sameContent(const ProjectDocument& a, const ProjectDocument& b) {
    auto sameSettings = [](const TrackSettings& x, const TrackSettings& y) {
        return x.preset == y.preset && x.gain == y.gain && x.pan == y.pan && x.mute == y.mute && x.solo == y.solo;
    };
    if (a.bpm != b.bpm || a.launchQuantizationSteps != b.launchQuantizationSteps || a.livePreset != b.livePreset
        || a.steps.on != b.steps.on || a.steps.pitch != b.steps.pitch || a.steps.velocity != b.steps.velocity
        || a.tracks.size() != b.tracks.size() || a.scenes.size() != b.scenes.size())
        return false;
    for (std::size_t t = 0; t < a.tracks.size(); ++t)
        if (a.tracks[t].id != b.tracks[t].id || a.tracks[t].name != b.tracks[t].name
            || a.tracks[t].colour != b.tracks[t].colour || !sameSettings(a.tracks[t].settings, b.tracks[t].settings))
            return false;
    for (std::size_t s = 0; s < a.scenes.size(); ++s)
        if (a.scenes[s].id != b.scenes[s].id || a.scenes[s].name != b.scenes[s].name) return false;
    for (std::size_t s = 0; s < a.clips.size(); ++s)
        for (std::size_t t = 0; t < a.clips[s].size(); ++t) {
            const auto& x = a.clips[s][t];
            const auto& y = b.clips[s][t];
            // Audio paths are excluded: they only change with the audio itself, or
            // when a portable save re-points them at collected copies.
            if (x.pattern != y.pattern || x.audio != y.audio) return false;
        }
    return true;
}

// Conversion to and from the on-disk session format. Audio paths are copied as
// strings; decoding is done separately by the loader.
inline SessionProjectData toProjectData(const ProjectDocument& doc) {
    SessionProjectData data(doc.trackCount(), doc.sceneCount());
    data.launchQuantizationSteps = doc.launchQuantizationSteps;
    data.selectedTrack = doc.selectedTrack;
    data.selectedScene = doc.selectedScene;
    data.bpm = doc.bpm;
    data.synthPreset = doc.livePreset;
    for (int t = 0; t < doc.trackCount(); ++t) {
        const auto& track = doc.tracks[static_cast<std::size_t>(t)];
        data.trackSettings[static_cast<std::size_t>(t)] = track.settings;
        data.trackNames[static_cast<std::size_t>(t)] = track.name;
        data.trackColours[static_cast<std::size_t>(t)] = track.colour;
    }
    for (int s = 0; s < doc.sceneCount(); ++s)
        data.sceneNames[static_cast<std::size_t>(s)] = doc.scenes[static_cast<std::size_t>(s)].name;
    data.stepPattern = doc.steps.on;
    data.stepPitches = doc.steps.pitch;
    data.stepVelocities = doc.steps.velocity;
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t) {
            auto& destination = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            destination.pattern = doc.clip(t, s).notes();
            destination.audioFilePath = doc.clip(t, s).audioFilePath;
        }
    return data;
}

inline ProjectDocument fromProjectData(const SessionProjectData& data) {
    auto doc = ProjectDocument::createDefault(data.trackCount(), data.sceneCount());
    doc.launchQuantizationSteps = data.launchQuantizationSteps;
    doc.selectedTrack = data.selectedTrack;
    doc.selectedScene = data.selectedScene;
    doc.bpm = data.bpm;
    doc.livePreset = data.synthPreset;
    for (int t = 0; t < data.trackCount(); ++t) {
        auto& track = doc.tracks[static_cast<std::size_t>(t)];
        track.settings = data.trackSettings[static_cast<std::size_t>(t)];
        if (data.trackNames[static_cast<std::size_t>(t)].isNotEmpty()) track.name = data.trackNames[static_cast<std::size_t>(t)];
        track.colour = data.trackColours[static_cast<std::size_t>(t)];
    }
    for (int s = 0; s < data.sceneCount(); ++s)
        if (data.sceneNames[static_cast<std::size_t>(s)].isNotEmpty())
            doc.scenes[static_cast<std::size_t>(s)].name = data.sceneNames[static_cast<std::size_t>(s)];
    doc.steps.on = data.stepPattern;
    doc.steps.pitch = data.stepPitches;
    doc.steps.velocity = data.stepVelocities;
    for (int s = 0; s < data.sceneCount(); ++s)
        for (int t = 0; t < data.trackCount(); ++t) {
            const auto& source = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            auto& clip = doc.clip(t, s);
            clip.pattern = std::make_shared<const PianoRollPattern>(source.pattern);
            clip.audioFilePath = source.audioFilePath;
        }
    doc.clampSelection();
    return doc;
}

} // namespace alien
