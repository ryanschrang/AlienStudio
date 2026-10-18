#pragma once

#include "PianoRollSequence.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

namespace alien {

inline constexpr int sessionProjectTrackCount = 4;
inline constexpr int sessionProjectSceneCount = 4;

struct SessionProjectClip {
    PianoRollPattern pattern;
    juce::String audioFilePath;
};

struct SessionProjectData {
    std::array<std::array<SessionProjectClip, sessionProjectTrackCount>, sessionProjectSceneCount> clips{};
    int launchQuantizationSteps{16};
    int selectedTrack{};
    int selectedScene{};
    int bpm{120};
    int synthPreset{};
    std::array<bool, 16> stepPattern{};

    bool valid() const noexcept {
        if ((launchQuantizationSteps != 4 && launchQuantizationSteps != 16)
            || selectedTrack < 0 || selectedTrack >= sessionProjectTrackCount
            || selectedScene < 0 || selectedScene >= sessionProjectSceneCount
            || bpm < 60 || bpm > 200 || synthPreset < 0 || synthPreset > 2)
            return false;
        for (const auto& scene : clips)
            for (const auto& clip : scene)
                if (!clip.pattern.valid()) return false;
        return true;
    }
};

inline juce::String serializeSessionProject(const SessionProjectData& project) {
    if (!project.valid()) return {};

    auto root = juce::var(new juce::DynamicObject());
    auto* rootObject = root.getDynamicObject();
    rootObject->setProperty("format", "AlienStudioSession");
    rootObject->setProperty("version", 1);
    rootObject->setProperty("launchQuantizationSteps", project.launchQuantizationSteps);
    rootObject->setProperty("selectedTrack", project.selectedTrack);
    rootObject->setProperty("selectedScene", project.selectedScene);
    rootObject->setProperty("bpm", project.bpm);
    rootObject->setProperty("synthPreset", project.synthPreset);
    juce::Array<juce::var> steps;
    for (const auto enabled : project.stepPattern) steps.add(enabled);
    rootObject->setProperty("stepPattern", juce::var(steps));

    juce::Array<juce::var> scenes;
    for (const auto& scene : project.clips) {
        juce::Array<juce::var> tracks;
        for (const auto& clip : scene) {
            auto clipValue = juce::var(new juce::DynamicObject());
            auto* clipObject = clipValue.getDynamicObject();
            clipObject->setProperty("loopSteps", clip.pattern.loopSteps);
            clipObject->setProperty("audioFilePath", clip.audioFilePath);

            juce::Array<juce::var> notes;
            for (std::size_t i = 0; i < clip.pattern.count; ++i) {
                const auto& note = clip.pattern.notes[i];
                auto noteValue = juce::var(new juce::DynamicObject());
                auto* noteObject = noteValue.getDynamicObject();
                noteObject->setProperty("pitch", note.pitch);
                noteObject->setProperty("startTicks", PianoRollPattern::toTicks(note.startStep));
                noteObject->setProperty("lengthTicks", PianoRollPattern::toTicks(note.lengthSteps));
                noteObject->setProperty("velocity", static_cast<double>(note.velocity));
                noteObject->setProperty("probability", static_cast<double>(note.probability));
                notes.add(noteValue);
            }
            clipObject->setProperty("notes", juce::var(notes));
            tracks.add(clipValue);
        }
        scenes.add(juce::var(tracks));
    }
    rootObject->setProperty("scenes", juce::var(scenes));
    return juce::JSON::toString(root, juce::JSON::FormatOptions().withIndentLevel(2));
}

inline bool parseSessionProject(const juce::String& json, SessionProjectData& destination,
                                juce::String& error) {
    const auto root = juce::JSON::parse(json);
    const auto* rootObject = root.getDynamicObject();
    if (rootObject == nullptr) { error = "Project file is not valid JSON."; return false; }
    if (rootObject->getProperty("format").toString() != "AlienStudioSession") {
        error = "This file is not an Alien Studio session project."; return false;
    }
    const auto version = rootObject->getProperty("version");
    if (!version.isInt() || static_cast<int>(version) != 1) {
        error = "This Alien Studio project version is not supported."; return false;
    }

    SessionProjectData parsed;
    const auto quantization = rootObject->getProperty("launchQuantizationSteps");
    const auto selectedTrack = rootObject->getProperty("selectedTrack");
    const auto selectedScene = rootObject->getProperty("selectedScene");
    const auto bpm = rootObject->getProperty("bpm");
    const auto synthPreset = rootObject->getProperty("synthPreset");
    const auto stepsValue = rootObject->getProperty("stepPattern");
    const auto* steps = stepsValue.getArray();
    if (!quantization.isInt() || !selectedTrack.isInt() || !selectedScene.isInt()
        || !bpm.isInt() || !synthPreset.isInt() || steps == nullptr || steps->size() != 16) {
        error = "Project launch settings are missing or invalid."; return false;
    }
    parsed.launchQuantizationSteps = static_cast<int>(quantization);
    parsed.selectedTrack = static_cast<int>(selectedTrack);
    parsed.selectedScene = static_cast<int>(selectedScene);
    parsed.bpm = static_cast<int>(bpm);
    parsed.synthPreset = static_cast<int>(synthPreset);
    for (int i = 0; i < 16; ++i) {
        if (!(*steps)[i].isBool()) { error = "Project step pattern is invalid."; return false; }
        parsed.stepPattern[static_cast<std::size_t>(i)] = static_cast<bool>((*steps)[i]);
    }

    const auto scenesValue = rootObject->getProperty("scenes");
    const auto* scenes = scenesValue.getArray();
    if (scenes == nullptr || scenes->size() != sessionProjectSceneCount) {
        error = "Project must contain exactly four scenes."; return false;
    }
    for (int sceneIndex = 0; sceneIndex < sessionProjectSceneCount; ++sceneIndex) {
        const auto* tracks = (*scenes)[sceneIndex].getArray();
        if (tracks == nullptr || tracks->size() != sessionProjectTrackCount) {
            error = "Each project scene must contain exactly four tracks."; return false;
        }
        for (int trackIndex = 0; trackIndex < sessionProjectTrackCount; ++trackIndex) {
            auto& clip = parsed.clips[static_cast<std::size_t>(sceneIndex)][static_cast<std::size_t>(trackIndex)];
            const auto* clipObject = (*tracks)[trackIndex].getDynamicObject();
            if (clipObject == nullptr) { error = "A project clip is invalid."; return false; }
            const auto loopSteps = clipObject->getProperty("loopSteps");
            const auto path = clipObject->getProperty("audioFilePath");
            const auto notesValue = clipObject->getProperty("notes");
            const auto* notes = notesValue.getArray();
            if (!loopSteps.isInt() || notes == nullptr || notes->size() > PianoRollPattern::maxNotes
                || (!path.isString() && !path.isVoid())) {
                error = "A project clip is missing valid pattern or audio data."; return false;
            }
            clip.pattern.loopSteps = static_cast<int>(loopSteps);
            if (clip.pattern.loopSteps < 1 || clip.pattern.loopSteps > PianoRollPattern::maxLoopSteps) {
                error = "A project clip has an unsupported loop length."; return false;
            }
            if (path.isString()) clip.audioFilePath = path.toString();
            for (const auto& noteValue : *notes) {
                const auto* note = noteValue.getDynamicObject();
                if (note == nullptr) { error = "A piano-roll note is invalid."; return false; }
                const auto pitch = note->getProperty("pitch");
                const auto startTicks = note->getProperty("startTicks");
                const auto lengthTicks = note->getProperty("lengthTicks");
                const auto velocity = note->getProperty("velocity");
                const auto probability = note->getProperty("probability");
                const bool pitchIsInt = pitch.isInt();
                const bool startIsInt = startTicks.isInt();
                const bool lengthIsInt = lengthTicks.isInt();
                const bool velocityIsNumber = velocity.isDouble() || velocity.isInt();
                const bool probabilityIsNumber = probability.isDouble() || probability.isInt();
                if (!pitchIsInt || !startIsInt || !lengthIsInt || !velocityIsNumber || !probabilityIsNumber) {
                    error = "A piano-roll note contains missing or invalid values."; return false;
                }
                const int notePitch = static_cast<int>(pitch);
                const int start = static_cast<int>(startTicks);
                const int length = static_cast<int>(lengthTicks);
                const double noteVelocity = static_cast<double>(velocity);
                const double noteProbability = static_cast<double>(probability);
                if (notePitch < 0 || notePitch > 127 || start < 0
                    || start >= clip.pattern.loopSteps * PianoRollPattern::ticksPerStep || length < 1
                    || length > clip.pattern.loopSteps * PianoRollPattern::ticksPerStep - start
                    || !std::isfinite(noteVelocity) || noteVelocity <= 0.0 || noteVelocity > 1.0
                    || !std::isfinite(noteProbability) || noteProbability < 0.0 || noteProbability > 1.0) {
                    error = "A piano-roll note is outside the supported range."; return false;
                }
                const auto id = clip.pattern.add(notePitch, PianoRollPattern::fromTicks(start),
                    PianoRollPattern::fromTicks(length), static_cast<float>(noteVelocity),
                    static_cast<float>(noteProbability));
                if (id == 0) { error = "A piano-roll note is outside the supported range."; return false; }
            }
            if (!clip.pattern.valid()) { error = "A project clip contains an invalid piano-roll pattern."; return false; }
        }
    }

    if (!parsed.valid()) { error = "Project settings are outside the supported range."; return false; }
    destination = std::move(parsed);
    error.clear();
    return true;
}

} // namespace alien
