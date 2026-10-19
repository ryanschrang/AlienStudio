#pragma once

#include "PianoRollSequence.h"
#include "TrackMixer.h"
#include "SynthEngine.h"
#include "DrumSampler.h"
#include "Arrangement.h"
#include "SessionCapacity.h"
#include "PluginState.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

namespace alien {

// Capacity of a session. The audio engine pre-allocates this many tracks so it
// never allocates while playing; raising either value is a one-line change.
// Version 1-3 projects always contained exactly this grid.
inline constexpr int sessionProjectTrackCount = 4;
inline constexpr int sessionProjectSceneCount = 4;
inline constexpr int sessionProjectFormatVersion = 6;

struct SessionProjectClip {
    PianoRollPattern pattern;
    juce::String audioFilePath;
};

// On-disk representation of a session. clips is [scene][track].
struct SessionProjectData {
    std::vector<std::vector<SessionProjectClip>> clips;
    std::vector<TrackSettings> trackSettings;
    std::vector<juce::String> trackNames;  // Empty entry: use the default name.
    std::vector<int> trackColours;
    std::vector<juce::String> sceneNames;
    std::vector<std::array<juce::String, drumPadCount>> drumPaths;
    Arrangement arrangement;
    std::vector<PluginRackState> plugins;
    PluginRackState masterPlugins;
    int launchQuantizationSteps{16};
    int selectedTrack{};
    int selectedScene{};
    int bpm{120};
    int synthPreset{};
    std::array<bool, 16> stepPattern{};
    std::array<int, 16> stepPitches = [] {
        std::array<int, 16> values{};
        values.fill(57);
        return values;
    }();
    std::array<int, 16> stepVelocities = [] {
        std::array<int, 16> values{};
        values.fill(102);
        return values;
    }();

    explicit SessionProjectData(int tracks = sessionProjectTrackCount, int scenes = sessionProjectSceneCount) {
        resize(tracks, scenes);
    }

    void resize(int tracks, int scenes) {
        clips.assign(static_cast<std::size_t>(juce::jmax(0, scenes)),
                     std::vector<SessionProjectClip>(static_cast<std::size_t>(juce::jmax(0, tracks))));
        plugins.assign(static_cast<std::size_t>(juce::jmax(0, tracks)), {});
        drumPaths.assign(static_cast<std::size_t>(juce::jmax(0, tracks)), {});
        trackSettings.assign(static_cast<std::size_t>(juce::jmax(0, tracks)), TrackSettings{});
        trackNames.assign(static_cast<std::size_t>(juce::jmax(0, tracks)), juce::String{});
        trackColours.clear();
        for (int t = 0; t < tracks; ++t) trackColours.push_back(t);
        sceneNames.assign(static_cast<std::size_t>(juce::jmax(0, scenes)), juce::String{});
    }

    int trackCount() const noexcept { return static_cast<int>(trackSettings.size()); }
    int sceneCount() const noexcept { return static_cast<int>(clips.size()); }

    bool valid() const noexcept {
        const int tracks = trackCount(), scenes = sceneCount();
        if (tracks < 1 || tracks > maxSessionTracks || scenes < 1 || scenes > maxSessionScenes
            || static_cast<int>(trackNames.size()) != tracks || static_cast<int>(trackColours.size()) != tracks
            || static_cast<int>(sceneNames.size()) != scenes)
            return false;
        if (!arrangement.valid(tracks, scenes) || drumPaths.size() != trackSettings.size()) return false;
        if ((launchQuantizationSteps != 4 && launchQuantizationSteps != 16)
            || selectedTrack < 0 || selectedTrack >= tracks
            || selectedScene < 0 || selectedScene >= scenes
            || bpm < 60 || bpm > 200 || synthPreset < 0 || synthPreset >= presetCount)
            return false;
        if (plugins.size() != trackSettings.size() || !masterPlugins.valid() || !masterPlugins.instrument.descriptionXml.isEmpty()) return false;
        for (const auto& rack : plugins) if (!rack.valid()) return false;
        for (const auto& track : trackSettings) if (!track.valid()) return false;
        for (const int colour : trackColours) if (colour < 0 || colour > 255) return false;
        for (const auto& scene : clips) {
            if (static_cast<int>(scene.size()) != tracks) return false;
            for (const auto& clip : scene)
                if (!clip.pattern.valid()) return false;
        }
        for (std::size_t i = 0; i < stepPitches.size(); ++i)
            if (stepPitches[i] < 0 || stepPitches[i] > 127
                || stepVelocities[i] < 1 || stepVelocities[i] > 127) return false;
        return true;
    }
};

inline juce::String serializeSessionProject(const SessionProjectData& project) {
    if (!project.valid()) return {};

    auto root = juce::var(new juce::DynamicObject());
    auto* rootObject = root.getDynamicObject();
    rootObject->setProperty("format", "AlienStudioSession");
    rootObject->setProperty("version", sessionProjectFormatVersion);
    rootObject->setProperty("launchQuantizationSteps", project.launchQuantizationSteps);
    rootObject->setProperty("selectedTrack", project.selectedTrack);
    rootObject->setProperty("selectedScene", project.selectedScene);
    rootObject->setProperty("bpm", project.bpm);
    rootObject->setProperty("synthPreset", project.synthPreset);
    juce::Array<juce::var> pluginRacks;
    for (const auto& rack : project.plugins) pluginRacks.add(rackToVar(rack));
    rootObject->setProperty("plugins", juce::var(pluginRacks));
    rootObject->setProperty("masterPlugins", rackToVar(project.masterPlugins));
    juce::Array<juce::var> settings;
    for (std::size_t t = 0; t < project.trackSettings.size(); ++t) {
        const auto& track = project.trackSettings[t];
        auto value = juce::var(new juce::DynamicObject());
        auto* object = value.getDynamicObject();
        object->setProperty("preset", track.preset);
        object->setProperty("instrument", track.instrument);
        object->setProperty("gain", track.gain);
        object->setProperty("pan", track.pan);
        object->setProperty("mute", track.mute);
        object->setProperty("solo", track.solo);
        object->setProperty("name", project.trackNames[t]);
        object->setProperty("colour", project.trackColours[t]);
        settings.add(value);
    }
    rootObject->setProperty("trackSettings", juce::var(settings));
    juce::Array<juce::var> sceneNames;
    for (const auto& name : project.sceneNames) sceneNames.add(name);
    rootObject->setProperty("sceneNames", juce::var(sceneNames));
    juce::Array<juce::var> kits;
    for (const auto& kit : project.drumPaths) {
        juce::Array<juce::var> pads;
        for (const auto& path : kit) pads.add(path);
        kits.add(juce::var(pads));
    }
    rootObject->setProperty("drumPaths", juce::var(kits));
    juce::Array<juce::var> steps;
    for (const auto enabled : project.stepPattern) steps.add(enabled);
    rootObject->setProperty("stepPattern", juce::var(steps));
    juce::Array<juce::var> pitches;
    juce::Array<juce::var> velocities;
    for (std::size_t i = 0; i < project.stepPitches.size(); ++i) {
        pitches.add(project.stepPitches[i]);
        velocities.add(project.stepVelocities[i]);
    }
    rootObject->setProperty("stepPitches", juce::var(pitches));
    rootObject->setProperty("stepVelocities", juce::var(velocities));

    auto songValue = juce::var(new juce::DynamicObject());
    songValue.getDynamicObject()->setProperty("lengthBars", project.arrangement.lengthBars);
    juce::Array<juce::var> regions;
    for (std::size_t i = 0; i < project.arrangement.count; ++i) {
        const auto& r = project.arrangement.regions[i];
        auto value = juce::var(new juce::DynamicObject());
        auto* object = value.getDynamicObject();
        object->setProperty("track",r.track); object->setProperty("scene",r.scene);
        object->setProperty("startBar",r.startBar); object->setProperty("lengthBars",r.lengthBars);
        regions.add(value);
    }
    songValue.getDynamicObject()->setProperty("regions",juce::var(regions));
    rootObject->setProperty("arrangement",songValue);
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
    if (!version.isInt() || (static_cast<int>(version) < 1 || static_cast<int>(version) > sessionProjectFormatVersion)) {
        error = "This Alien Studio project version is not supported."; return false;
    }
    const int formatVersion = static_cast<int>(version);
    const bool fixedGrid = formatVersion < 4;

    const auto scenesValue = rootObject->getProperty("scenes");
    const auto* scenes = scenesValue.getArray();
    if (scenes == nullptr || scenes->isEmpty()) { error = "Project contains no scenes."; return false; }
    const auto* firstScene = (*scenes)[0].getArray();
    const int sceneCount = scenes->size();
    const int trackCount = firstScene != nullptr ? firstScene->size() : 0;
    if (fixedGrid && (sceneCount != sessionProjectSceneCount || trackCount != sessionProjectTrackCount)) {
        error = "Project must contain exactly four scenes of four tracks."; return false;
    }
    if (trackCount < 1 || trackCount > maxSessionTracks || sceneCount > maxSessionScenes) {
        error = "Project has more tracks or scenes than this version of Alien Studio supports."; return false;
    }

    SessionProjectData parsed(trackCount, sceneCount);
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
    if (formatVersion < 3) {
        for (auto& track : parsed.trackSettings) track.preset = parsed.synthPreset;
    } else {
        const auto settingsValue = rootObject->getProperty("trackSettings");
        const auto* settings = settingsValue.getArray();
        if (settings == nullptr || settings->size() != trackCount) {
            error = fixedGrid ? "Project must contain four track settings."
                              : "Project track settings do not match its clips."; return false;
        }
        for (int i = 0; i < trackCount; ++i) {
            const auto* object = (*settings)[i].getDynamicObject();
            if (object == nullptr) { error = "Invalid track settings."; return false; }
            const auto p = object->getProperty("preset"), g = object->getProperty("gain"),
                pan = object->getProperty("pan"), mute = object->getProperty("mute"),
                solo = object->getProperty("solo");
            if (!p.isInt() || !(g.isDouble() || g.isInt()) || !(pan.isDouble() || pan.isInt())
                || !mute.isBool() || !solo.isBool()) {
                error = "Invalid track settings."; return false;
            }
            auto& track = parsed.trackSettings[static_cast<std::size_t>(i)];
            track = {static_cast<int>(p), static_cast<float>(g), static_cast<float>(pan),
                static_cast<bool>(mute), static_cast<bool>(solo)};
            if (object->hasProperty("instrument")) {
                const auto instrument = object->getProperty("instrument");
                if (!instrument.isInt()) { error = "Invalid instrument type."; return false; }
                track.instrument = static_cast<int>(instrument);
            }
            if (!track.valid()) { error = "Track settings outside supported range."; return false; }
            const auto name = object->getProperty("name"), colour = object->getProperty("colour");
            if (name.isString()) parsed.trackNames[static_cast<std::size_t>(i)] = name.toString().substring(0, 64);
            if (colour.isInt()) parsed.trackColours[static_cast<std::size_t>(i)] = juce::jlimit(0, 255, static_cast<int>(colour));
        }
    }
    if (const auto* names = rootObject->getProperty("sceneNames").getArray())
        for (int i = 0; i < juce::jmin(sceneCount, names->size()); ++i)
            if ((*names)[i].isString()) parsed.sceneNames[static_cast<std::size_t>(i)] = (*names)[i].toString().substring(0, 64);
    for (int i = 0; i < 16; ++i) {
        if (!(*steps)[i].isBool()) { error = "Project step pattern is invalid."; return false; }
        parsed.stepPattern[static_cast<std::size_t>(i)] = static_cast<bool>((*steps)[i]);
    }
    if (formatVersion >= 2) {
        const auto pitchesValue = rootObject->getProperty("stepPitches");
        const auto velocitiesValue = rootObject->getProperty("stepVelocities");
        const auto* pitches = pitchesValue.getArray();
        const auto* velocities = velocitiesValue.getArray();
        if (pitches == nullptr || velocities == nullptr
            || pitches->size() != static_cast<int>(parsed.stepPitches.size())
            || velocities->size() != static_cast<int>(parsed.stepVelocities.size())) {
            error = "Project step note data is missing or invalid."; return false;
        }
        for (int i = 0; i < 16; ++i) {
            if (!(*pitches)[i].isInt() || !(*velocities)[i].isInt()) {
                error = "Project step note data is invalid."; return false;
            }
            parsed.stepPitches[static_cast<std::size_t>(i)] = static_cast<int>((*pitches)[i]);
            parsed.stepVelocities[static_cast<std::size_t>(i)] = static_cast<int>((*velocities)[i]);
        }
    }

    if (rootObject->hasProperty("drumPaths")) {
        const auto kitsValue = rootObject->getProperty("drumPaths");
        const auto* kits = kitsValue.getArray();
        if (kits == nullptr || kits->size() != trackCount) { error = "Invalid drum kits."; return false; }
        for (int t = 0; t < trackCount; ++t) {
            const auto* pads = (*kits)[t].getArray();
            if (pads == nullptr || pads->size() != drumPadCount) { error = "Invalid drum pads."; return false; }
            for (int p = 0; p < drumPadCount; ++p) {
                if (!(*pads)[p].isString()) { error = "Invalid drum sample path."; return false; }
                parsed.drumPaths[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)] = (*pads)[p].toString();
            }
        }
    }
    for (int sceneIndex = 0; sceneIndex < sceneCount; ++sceneIndex) {
        const auto* tracks = (*scenes)[sceneIndex].getArray();
        if (tracks == nullptr || tracks->size() != trackCount) {
            error = fixedGrid ? "Each project scene must contain exactly four tracks."
                              : "Every project scene must contain one clip per track."; return false;
        }
        for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            auto& clip = parsed.clips[static_cast<std::size_t>(sceneIndex)][static_cast<std::size_t>(trackIndex)];
            const auto* clipObject = (*tracks)[trackIndex].getDynamicObject();
            if (clipObject == nullptr) { error = "A project clip is invalid."; return false; }
            const auto loopSteps = clipObject->getProperty("loopSteps");
            const auto path = clipObject->getProperty("audioFilePath");
            const auto notesValue = clipObject->getProperty("notes");
            const auto* notes = notesValue.getArray();
            if (!loopSteps.isInt() || notes == nullptr || notes->size() > static_cast<int>(PianoRollPattern::maxNotes)
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
                const bool velocityIsNumber = velocity.isDouble() || velocity.isInt();
                const bool probabilityIsNumber = probability.isDouble() || probability.isInt();
                if (!pitch.isInt() || !startTicks.isInt() || !lengthTicks.isInt() || !velocityIsNumber || !probabilityIsNumber) {
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

    if (rootObject->hasProperty("arrangement")) {
        const auto songValue = rootObject->getProperty("arrangement");
        const auto* object = songValue.getDynamicObject();
        if (object == nullptr || !object->getProperty("lengthBars").isInt()) { error="Invalid arrangement."; return false; }
        parsed.arrangement.lengthBars=static_cast<int>(object->getProperty("lengthBars"));
        const auto regionsValue=object->getProperty("regions"); const auto* regions=regionsValue.getArray();
        if (regions==nullptr || regions->size()>Arrangement::capacity) { error="Invalid arrangement regions.";return false; }
        for (const auto& value : *regions) {
            const auto* r=value.getDynamicObject();
            if(r==nullptr || !r->getProperty("track").isInt() || !r->getProperty("scene").isInt()
                || !r->getProperty("startBar").isInt() || !r->getProperty("lengthBars").isInt()) { error="Invalid region.";return false; }
            if(!parsed.arrangement.add({static_cast<int>(r->getProperty("track")),static_cast<int>(r->getProperty("scene")),
                static_cast<int>(r->getProperty("startBar")),static_cast<int>(r->getProperty("lengthBars"))})) {
                error="Overlapping or out-of-range arrangement region.";return false;
            }
        }
    }
    if (formatVersion >= 6) {
        const auto rackValue = rootObject->getProperty("plugins"); const auto* racks = rackValue.getArray();
        if (!racks || racks->size() != trackCount || !rackFromVar(rootObject->getProperty("masterPlugins"), parsed.masterPlugins)) { error="Invalid plugin racks"; return false; }
        for (int t=0;t<trackCount;++t) if (!rackFromVar((*racks)[t],parsed.plugins[t])) { error="Invalid plugin state"; return false; }
    }
    if (!parsed.valid()) { error = "Project settings are outside the supported range."; return false; }
    destination = std::move(parsed);
    error.clear();
    return true;
}

} // namespace alien
