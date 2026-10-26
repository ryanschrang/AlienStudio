#pragma once

#include "PianoRollSequence.h"
#include "MidiRecording.h"
#include "TrackMixer.h"
#include "SynthEngine.h"
#include "DrumSampler.h"
#include "Arrangement.h"
#include "SessionCapacity.h"
#include "PluginState.h"
#include "RackProject.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace alien {

// Capacity of a session. The audio engine pre-allocates this many tracks so it
// never allocates while playing; raising either value is a one-line change.
// Version 1-3 projects always contained exactly this grid.
inline constexpr int sessionProjectTrackCount = 4;
inline constexpr int sessionProjectSceneCount = 4;
inline constexpr int sessionProjectFormatVersion = 18;   // 18: song tempo changes

struct SessionProjectClip {
    SharedPianoRoll pattern;   // shared with the document; empty clips share nothing at all
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
    alien::timeline::Timeline timeline;   // Absent before version 8: the old bar-based arrangement, converted on load.
    std::vector<PluginRackState> plugins;
    PluginRackState masterPlugins;
    // Return tracks A-D and their FX. Absent before version 15: unity, centred, no FX (and every send off).
    std::array<ReturnSettings, maxReturns> returns{};
    ReturnRacks returnPlugins{};
    rack::Project channelRack = rack::Project::createDefault(); // Absent before version 7: the starting rack.
    int launchQuantizationSteps{16};
    float masterGain{1.0f}; // Absent in older files: unity.
    RecordSettings recording; // Absent in older files: defaults.
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
        if (!timeline.valid(tracks, scenes, channelRack.patternCount) || drumPaths.size() != trackSettings.size()) return false;
        if ((launchQuantizationSteps != 4 && launchQuantizationSteps != 16)
            || selectedTrack < 0 || selectedTrack >= tracks
            || selectedScene < 0 || selectedScene >= scenes
            || bpm < 60 || bpm > 200 || synthPreset < 0 || synthPreset >= presetCount || !validMasterGain(masterGain)
            || !recording.valid())
            return false;
        if (plugins.size() != trackSettings.size() || !masterPlugins.valid() || !masterPlugins.instrument.descriptionXml.isEmpty()) return false;
        for (const auto& pluginRack : plugins) if (!pluginRack.valid()) return false;
        for (const auto& r : returns) if (!r.valid()) return false;
        for (const auto& pluginRack : returnPlugins) if (!pluginRack.valid() || !pluginRack.instrument.descriptionXml.isEmpty()) return false;
        // Identities are global across tracks, master and returns. Ambiguous IDs
        // must fail validation rather than redirect existing automation.
        std::vector<std::uint64_t> identities;
        auto uniqueRack = [&](const PluginRackState& rack) {
            auto unique = [&](const PluginState& plugin) {
                if (plugin.instanceId == 0) return true;
                if (std::find(identities.begin(), identities.end(), plugin.instanceId) != identities.end()) return false;
                identities.push_back(plugin.instanceId); return true;
            };
            if (!unique(rack.instrument)) return false;
            for (const auto& effect : rack.effects) if (!unique(effect)) return false;
            return true;
        };
        for (const auto& rack : plugins) if (!uniqueRack(rack)) return false;
        if (!uniqueRack(masterPlugins)) return false;
        for (const auto& rack : returnPlugins) if (!uniqueRack(rack)) return false;
        if (!channelRack.valid()) return false;
        for (const auto& scene : clips) if (static_cast<int>(scene.size()) != tracks) return false;
        for (std::size_t t=0;t<trackSettings.size();++t) {
            const auto& track=trackSettings[t]; if (!track.valid()) return false;
            if (track.frozen()) {
                if (track.freezeScene>=scenes || clips[static_cast<std::size_t>(track.freezeScene)][t].audioFilePath.isEmpty()) return false;
                int cache=0, original=0;
                for(const auto& region:timeline.regions) if(region.row==static_cast<int>(t)) { if(region.source!=track.freezeScene)return false; ++cache; }
                for(const auto& region:timeline.frozenOriginals) original+=region.row==static_cast<int>(t);
                if(cache!=1 || original==0 || timeline.freezeBaseLengthBars==0)return false;
            }
        }
        for(const auto& region:timeline.frozenOriginals) if(!trackSettings[static_cast<std::size_t>(region.row)].frozen())return false;
        for (const int colour : trackColours) if (colour < 0 || colour > 255) return false;
        for (const auto& scene : clips) {
            if (static_cast<int>(scene.size()) != tracks) return false;
            for (const auto& clip : scene)
                if (!clip.pattern->valid()) return false;
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
    rootObject->setProperty("masterGain", project.masterGain);
    {
        auto recordingVar = juce::var(new juce::DynamicObject());
        auto* recordingObject = recordingVar.getDynamicObject();
        recordingObject->setProperty("metronome", project.recording.metronome);
        recordingObject->setProperty("metronomeVolume", project.recording.metronomeVolume);
        recordingObject->setProperty("countInBars", project.recording.countInBars);
        recordingObject->setProperty("latencyOffsetMs", project.recording.latencyOffsetMs);
        recordingObject->setProperty("replace", project.recording.replace);
        recordingObject->setProperty("audioLatencyOffsetMs", project.recording.audioLatencyOffsetMs);
        recordingObject->setProperty("punch", project.recording.punch);
        rootObject->setProperty("recording", recordingVar);
    }
    rootObject->setProperty("channelRack", rack::rackToVar(project.channelRack));
    juce::Array<juce::var> pluginRacks;
    for (const auto& pluginRack : project.plugins) pluginRacks.add(rackToVar(pluginRack));
    rootObject->setProperty("plugins", juce::var(pluginRacks));
    rootObject->setProperty("masterPlugins", rackToVar(project.masterPlugins));
    juce::Array<juce::var> returns;
    for (std::size_t r = 0; r < project.returns.size(); ++r) {
        auto value = juce::var(new juce::DynamicObject());
        auto* object = value.getDynamicObject();
        object->setProperty("gain", project.returns[r].gain);
        object->setProperty("pan", project.returns[r].pan);
        object->setProperty("mute", project.returns[r].mute);
        object->setProperty("plugins", rackToVar(project.returnPlugins[r]));
        returns.add(value);
    }
    rootObject->setProperty("returns", juce::var(returns));
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
        object->setProperty("freezeScene",track.freezeScene);
        object->setProperty("armed", track.armed); object->setProperty("input", track.input);
        object->setProperty("stereoInput", track.stereoInput); object->setProperty("monitor", track.monitor);
        juce::Array<juce::var> sends;   // [level, pre-fader] for each return
        for (std::size_t r = 0; r < track.send.size(); ++r) {
            juce::Array<juce::var> send;
            send.add(track.send[r]); send.add(track.sendPre[r]);
            sends.add(juce::var(send));
        }
        object->setProperty("sends", juce::var(sends));
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

    {
        auto timelineValue = juce::var(new juce::DynamicObject());
        auto* timelineObject = timelineValue.getDynamicObject();
        timelineObject->setProperty("freezeBaseLengthBars",project.timeline.freezeBaseLengthBars);
        timelineObject->setProperty("lengthBars", project.timeline.lengthBars);
        timelineObject->setProperty("loopEnabled", project.timeline.loopEnabled);
        timelineObject->setProperty("loopStart", static_cast<int>(project.timeline.loopStart));
        timelineObject->setProperty("loopEnd", static_cast<int>(project.timeline.loopEnd));
        // Groups are saved as small numbers (1, 2, ...), not the in-memory ids, which differ every run.
        std::vector<std::uint32_t> groups;
        juce::Array<juce::var> regions;
        auto savedRegions = project.timeline.regions;
        savedRegions.insert(savedRegions.end(), project.timeline.takes.begin(), project.timeline.takes.end());
        savedRegions.insert(savedRegions.end(),project.timeline.frozenOriginals.begin(),project.timeline.frozenOriginals.end());
        for (std::size_t savedIndex = 0; savedIndex < savedRegions.size(); ++savedIndex) {
            const auto& r = savedRegions[savedIndex];
            auto value = juce::var(new juce::DynamicObject());
            auto* object = value.getDynamicObject();
            int group = 0;
            if (r.group != 0) {
                auto it = std::find(groups.begin(), groups.end(), r.group);
                if (it == groups.end()) { groups.push_back(r.group); it = groups.end() - 1; }
                group = static_cast<int>(it - groups.begin()) + 1;
            }
            object->setProperty("row", r.row); object->setProperty("source", r.source);
            object->setProperty("start", static_cast<int>(r.start)); object->setProperty("length", static_cast<int>(r.length));
            object->setProperty("offset", static_cast<int>(r.offset)); object->setProperty("audioOffset", r.audioOffset);
            object->setProperty("gain", r.gain); object->setProperty("fadeIn", static_cast<int>(r.fadeIn));
            object->setProperty("fadeOut", static_cast<int>(r.fadeOut)); object->setProperty("loopSource", r.loopSource);
            object->setProperty("reversed", r.reversed); object->setProperty("crossfade", static_cast<int>(r.crossfade));
            object->setProperty("muted", r.muted); object->setProperty("group", group);
            if (savedIndex >= project.timeline.regions.size()+project.timeline.takes.size()) object->setProperty("frozenOriginal",true);
            else if (savedIndex >= project.timeline.regions.size()) object->setProperty("take", true);
            regions.add(value);
        }
        timelineObject->setProperty("regions", juce::var(regions));
        juce::Array<juce::var> automationValues;
        for (const auto& lane : project.timeline.automationLanes) {
            auto value = juce::var(new juce::DynamicObject()); auto* object = value.getDynamicObject();
            object->setProperty("track", lane.track); object->setProperty("parameter", static_cast<int>(lane.parameter));
            object->setProperty("enabled", lane.enabled);
            juce::Array<juce::var> points;
            for (const auto& point : lane.points) {
                auto p = juce::var(new juce::DynamicObject()); auto* o = p.getDynamicObject();
                o->setProperty("tick", static_cast<int>(point.tick)); o->setProperty("value", point.value);
                o->setProperty("curve", static_cast<int>(point.curve)); points.add(p);
            }
            object->setProperty("points", juce::var(points)); automationValues.add(value);
        }
        timelineObject->setProperty("automation", juce::var(automationValues));
        juce::Array<juce::var> parameters;
        for (const auto& lane : project.timeline.parameterLanes) {
            auto value = juce::var(new juce::DynamicObject()); auto* o = value.getDynamicObject();
            o->setProperty("instance", instanceIdString(lane.target.instance));
            o->setProperty("parameterId", juce::String::fromUTF8(lane.target.parameterId.c_str()));
            o->setProperty("enabled", lane.enabled);
            juce::Array<juce::var> points;
            for (const auto& point : lane.points) {
                auto p = juce::var(new juce::DynamicObject()); auto* object = p.getDynamicObject();
                object->setProperty("tick", static_cast<int>(point.tick)); object->setProperty("value", point.value);
                object->setProperty("curve", static_cast<int>(point.curve)); points.add(p);
            }
            o->setProperty("points", juce::var(points)); parameters.add(value);
        }
        timelineObject->setProperty("parameterAutomation", juce::var(parameters));

        juce::Array<juce::var> markers;
        for (const auto& m : project.timeline.markers) {
            auto value = juce::var(new juce::DynamicObject());
            auto* object = value.getDynamicObject();
            object->setProperty("tick", static_cast<int>(m.tick));
            object->setProperty("name", juce::String(juce::CharPointer_UTF8(m.name.c_str())));
            object->setProperty("colour", static_cast<int>(m.colour));
            markers.add(value);
        }
        timelineObject->setProperty("markers", juce::var(markers));

        juce::Array<juce::var> tempoChanges;
        for (const auto& c : project.timeline.tempoChanges) {
            auto value = juce::var(new juce::DynamicObject());
            value.getDynamicObject()->setProperty("tick", static_cast<juce::int64>(c.tick));
            value.getDynamicObject()->setProperty("bpm", c.bpm);
            tempoChanges.add(value);
        }
        timelineObject->setProperty("tempoChanges", juce::var(tempoChanges));
        rootObject->setProperty("timeline", timelineValue);
    }
    juce::Array<juce::var> scenes;
    for (const auto& scene : project.clips) {
        juce::Array<juce::var> tracks;
        for (const auto& clip : scene) {
            auto clipValue = juce::var(new juce::DynamicObject());
            auto* clipObject = clipValue.getDynamicObject();
            const PianoRollPattern& pattern = clip.pattern;
            clipObject->setProperty("loopSteps", pattern.loopSteps);
            clipObject->setProperty("audioFilePath", clip.audioFilePath);

            juce::Array<juce::var> notes;
            for (std::size_t i = 0; i < pattern.count; ++i) {
                const auto& note = pattern.notes[i];
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

// Reads the "timeline" object of a version 8 project. Region and marker ids are made up
// fresh (they only identify things while the program runs); saved group numbers become groups.
inline bool parseTimeline(const juce::var& value, timeline::Timeline& out, juce::String& error,
                          bool requireAudioEdits = true, bool requireAutomation = false) {
    const auto* object = value.getDynamicObject();
    if (object == nullptr) { error = "The project's timeline is missing."; return false; }
    const auto length = object->getProperty("lengthBars"), loopStart = object->getProperty("loopStart"),
               loopEnd = object->getProperty("loopEnd"), loopEnabled = object->getProperty("loopEnabled");
    if (!length.isInt() || !loopStart.isInt() || !loopEnd.isInt() || !loopEnabled.isBool()) {
        error = "The project's timeline settings are invalid."; return false;
    }
    timeline::Timeline parsed;
    parsed.lengthBars = static_cast<int>(length);
    const auto base=object->getProperty("freezeBaseLengthBars");
    if(!base.isVoid() && !base.isInt()){error="Invalid freeze length.";return false;}
    if(base.isInt())parsed.freezeBaseLengthBars=static_cast<int>(base);
    parsed.loopStart = static_cast<int>(loopStart); parsed.loopEnd = static_cast<int>(loopEnd);
    parsed.loopEnabled = static_cast<bool>(loopEnabled);
    const auto regionsValue = object->getProperty("regions"), markersValue = object->getProperty("markers");
    const auto* regions = regionsValue.getArray();
    const auto* markers = markersValue.getArray();
    if (regions == nullptr || markers == nullptr || regions->size() > static_cast<int>(2 * timeline::maxRegions + timeline::maxTakes)
        || markers->size() > static_cast<int>(timeline::maxMarkers)) {
        error = "The project's timeline has too many regions or markers."; return false;
    }
    std::vector<std::pair<int, std::uint32_t>> groups;
    for (const auto& item : *regions) {
        const auto* r = item.getDynamicObject();
        if (r == nullptr) { error = "A timeline region is invalid."; return false; }
        const auto row = r->getProperty("row"), source = r->getProperty("source"), start = r->getProperty("start"),
                   regionLength = r->getProperty("length"), offset = r->getProperty("offset"),
                   audioOffset = r->getProperty("audioOffset"), muted = r->getProperty("muted"), group = r->getProperty("group");
        if (!row.isInt() || !source.isInt() || !start.isInt() || !regionLength.isInt() || !offset.isInt()
            || !(audioOffset.isDouble() || audioOffset.isInt()) || !muted.isBool() || !group.isInt()) {
            error = "A timeline region is missing values."; return false;
        }
        timeline::Region region;
        region.id = timeline::newId();
        region.row = static_cast<int>(row); region.source = static_cast<int>(source);
        region.start = static_cast<int>(start); region.length = static_cast<int>(regionLength);
        region.offset = static_cast<int>(offset); region.audioOffset = static_cast<double>(audioOffset);
        const auto gain = r->getProperty("gain"), fadeIn = r->getProperty("fadeIn"), fadeOut = r->getProperty("fadeOut"),
                   loopSource = r->getProperty("loopSource"), reversed = r->getProperty("reversed");
        if (requireAudioEdits || !gain.isVoid() || !fadeIn.isVoid() || !fadeOut.isVoid() || !loopSource.isVoid() || !reversed.isVoid()) {
            if (!(gain.isDouble() || gain.isInt()) || !fadeIn.isInt() || !fadeOut.isInt() || !loopSource.isBool() || !reversed.isBool()) {
                error = "A timeline region has invalid audio edits."; return false;
            }
            region.gain = static_cast<double>(gain); region.fadeIn = static_cast<int>(fadeIn); region.fadeOut = static_cast<int>(fadeOut);
            region.loopSource = static_cast<bool>(loopSource); region.reversed = static_cast<bool>(reversed);
        }
        // Absent before version 11: no crossfade.
        if (const auto crossfade = r->getProperty("crossfade"); !crossfade.isVoid()) {
            if (!crossfade.isInt()) { error = "A timeline region has an invalid crossfade."; return false; }
            region.crossfade = static_cast<int>(crossfade);
        }
        region.muted = static_cast<bool>(muted);
        if (const int g = static_cast<int>(group); g > 0) {
            auto it = std::find_if(groups.begin(), groups.end(), [g](const auto& p) { return p.first == g; });
            if (it == groups.end()) { groups.emplace_back(g, timeline::newId()); it = groups.end() - 1; }
            region.group = it->second;
        }
        const auto take = r->getProperty("take");
        if (!take.isVoid() && !take.isBool()) { error = "Invalid recording take flag."; return false; }
        const auto frozen=r->getProperty("frozenOriginal");
        if((!frozen.isVoid() && !frozen.isBool()) || (static_cast<bool>(take) && static_cast<bool>(frozen))) {error="Invalid frozen clip.";return false;}
        if(static_cast<bool>(frozen))parsed.frozenOriginals.push_back(region);
        else if (static_cast<bool>(take)) parsed.takes.push_back(region);
        else parsed.regions.push_back(region);
    }
    for (const auto& item : *markers) {
        const auto* m = item.getDynamicObject();
        if (m == nullptr || !m->getProperty("tick").isInt() || !m->getProperty("name").isString() || !m->getProperty("colour").isInt()) {
            error = "A timeline marker is invalid."; return false;
        }
        timeline::Marker marker;
        marker.id = timeline::newId();
        marker.tick = static_cast<int>(m->getProperty("tick"));
        marker.name = m->getProperty("name").toString().toStdString();
        marker.colour = static_cast<std::uint8_t>(juce::jlimit(0, 255, static_cast<int>(m->getProperty("colour"))));
        parsed.markers.push_back(marker);
    }
    // Absent before version 18: the project tempo throughout. Validated with the rest of the timeline.
    const auto tempoValue = object->getProperty("tempoChanges");
    if (!tempoValue.isVoid()) {
        const auto* changes = tempoValue.getArray();
        if (changes == nullptr || changes->size() > static_cast<int>(timeline::maxTempoChanges)) { error = "Invalid tempo changes."; return false; }
        for (const auto& item : *changes) {
            const auto* c = item.getDynamicObject();
            if (c == nullptr || !(c->getProperty("tick").isInt() || c->getProperty("tick").isInt64())
                || !(c->getProperty("bpm").isInt() || c->getProperty("bpm").isDouble())) {
                error = "A tempo change is invalid."; return false;
            }
            parsed.tempoChanges.push_back({static_cast<std::int64_t>(static_cast<juce::int64>(c->getProperty("tick"))),
                                           static_cast<double>(c->getProperty("bpm"))});
        }
    }
    const auto automationValue = object->getProperty("automation");
    if (requireAutomation || !automationValue.isVoid()) {
        const auto* lanes = automationValue.getArray();
        if (lanes == nullptr || lanes->size() > maxSessionTracks * automation::parametersPerTrack) { error = "Invalid automation lanes."; return false; }
        for (const auto& value : *lanes) {
            const auto* o = value.getDynamicObject();
            if (o == nullptr || !o->getProperty("track").isInt() || !o->getProperty("parameter").isInt()
                || !o->getProperty("enabled").isBool()) { error = "Invalid automation lane."; return false; }
            automation::Lane lane;
            lane.track = static_cast<int>(o->getProperty("track"));
            lane.parameter = static_cast<automation::Parameter>(static_cast<int>(o->getProperty("parameter")));
            lane.enabled = static_cast<bool>(o->getProperty("enabled"));
            const auto pointsValue = o->getProperty("points"); const auto* points = pointsValue.getArray();
            if (points == nullptr || points->size() > static_cast<int>(automation::maxPoints)) { error = "Invalid automation points."; return false; }
            for (const auto& p : *points) {
                const auto* point = p.getDynamicObject();
                if (point == nullptr || !point->getProperty("tick").isInt() || !point->getProperty("curve").isInt()
                    || !(point->getProperty("value").isInt() || point->getProperty("value").isDouble())) {
                    error = "Invalid automation point."; return false;
                }
                lane.points.push_back({static_cast<int>(point->getProperty("tick")),
                    static_cast<float>(static_cast<double>(point->getProperty("value"))),
                    static_cast<automation::Curve>(static_cast<int>(point->getProperty("curve")))});
            }
            parsed.automationLanes.push_back(std::move(lane));
        }
    }
    const auto parameterValue = object->getProperty("parameterAutomation");
    if (!parameterValue.isVoid()) {
        const auto* lanes = parameterValue.getArray();
        if (!lanes || lanes->size() > static_cast<int>(timeline::Timeline::maxParameterLanes)) { error = "Invalid parameter automation lanes."; return false; }
        for (const auto& value : *lanes) {
            const auto* o = value.getDynamicObject();
            automation::ParameterLane lane;
            if (!o || !parseInstanceId(o->getProperty("instance"), lane.target.instance)
                || !o->getProperty("parameterId").isString() || !o->getProperty("enabled").isBool()) {
                error = "Invalid parameter automation target."; return false;
            }
            lane.target.parameterId = o->getProperty("parameterId").toString().toStdString();
            lane.enabled = static_cast<bool>(o->getProperty("enabled"));
            const auto pointsValue = o->getProperty("points"); const auto* points = pointsValue.getArray();
            if (!points || points->size() > static_cast<int>(automation::maxPoints)) { error = "Invalid parameter automation points."; return false; }
            for (const auto& p : *points) {
                const auto* point = p.getDynamicObject();
                if (!point || !point->getProperty("tick").isInt() || !point->getProperty("curve").isInt()
                    || !(point->getProperty("value").isInt() || point->getProperty("value").isDouble())) {
                    error = "Invalid parameter automation point."; return false;
                }
                lane.points.push_back({static_cast<int>(point->getProperty("tick")),
                    static_cast<float>(static_cast<double>(point->getProperty("value"))),
                    static_cast<automation::Curve>(static_cast<int>(point->getProperty("curve")))});
            }
            parsed.parameterLanes.push_back(std::move(lane));
        }
    }
    // The file's own scene and pattern counts are checked with the rest of the project.
    if (!parsed.valid()) { error = "The project's timeline is outside the supported range."; return false; }
    out = std::move(parsed);
    return true;
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
    const auto masterGain = rootObject->getProperty("masterGain");
    if (masterGain.isDouble() || masterGain.isInt()) {
        parsed.masterGain = static_cast<float>(static_cast<double>(masterGain));
        if (!validMasterGain(parsed.masterGain)) { error = "The project's master volume is out of range."; return false; }
    } else if (!masterGain.isVoid()) {
        error = "The project's master volume is invalid."; return false;
    }
    const auto recordingValue = rootObject->getProperty("recording");
    if (!recordingValue.isVoid()) {
        auto* recordingObject = recordingValue.getDynamicObject();
        const auto metronome = recordingValue.getProperty("metronome", juce::var());
        const auto volume = recordingValue.getProperty("metronomeVolume", juce::var());
        const auto countIn = recordingValue.getProperty("countInBars", juce::var());
        const auto offset = recordingValue.getProperty("latencyOffsetMs", juce::var());
        const auto replace = recordingValue.getProperty("replace", juce::var());
        if (recordingObject == nullptr || !metronome.isBool() || !(volume.isDouble() || volume.isInt()) || !countIn.isInt()
            || !offset.isInt() || !replace.isBool()) {
            error = "The project's recording settings are invalid."; return false;
        }
        parsed.recording.metronome = static_cast<bool>(metronome);
        parsed.recording.metronomeVolume = static_cast<float>(static_cast<double>(volume));
        parsed.recording.countInBars = static_cast<int>(countIn);
        parsed.recording.latencyOffsetMs = static_cast<int>(offset);
        parsed.recording.replace = static_cast<bool>(replace);
        // Added with Song-view audio takes; older projects leave them out.
        const auto audioOffset = recordingValue.getProperty("audioLatencyOffsetMs", juce::var());
        const auto punch = recordingValue.getProperty("punch", juce::var());
        if ((!audioOffset.isVoid() && !audioOffset.isInt()) || (!punch.isVoid() && !punch.isBool())) {
            error = "The project's recording settings are invalid."; return false;
        }
        if (!audioOffset.isVoid()) parsed.recording.audioLatencyOffsetMs = static_cast<int>(audioOffset);
        if (!punch.isVoid()) parsed.recording.punch = static_cast<bool>(punch);
        if (!parsed.recording.valid()) { error = "The project's recording settings are out of range."; return false; }
    }
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
            const auto frozenScene=object->getProperty("freezeScene");
            if(!frozenScene.isVoid() && !frozenScene.isInt()){error="Invalid frozen track.";return false;}
            if(frozenScene.isInt())track.freezeScene=static_cast<int>(frozenScene);
            // Recording settings are optional: projects saved before them have none (not armed, input 1, monitoring automatic).
            const auto armed = object->getProperty("armed"), input = object->getProperty("input"),
                stereoInput = object->getProperty("stereoInput"), monitor = object->getProperty("monitor");
            if ((!armed.isVoid() && !armed.isBool()) || (!input.isVoid() && !input.isInt())
                || (!stereoInput.isVoid() && !stereoInput.isBool()) || (!monitor.isVoid() && !monitor.isInt())) {
                error = "Invalid track recording settings."; return false;
            }
            if (armed.isBool()) track.armed = static_cast<bool>(armed);
            if (input.isInt()) track.input = static_cast<int>(input);
            if (stereoInput.isBool()) track.stereoInput = static_cast<bool>(stereoInput);
            if (monitor.isInt()) track.monitor = static_cast<int>(monitor);
            // Sends are optional (absent before version 15: all off); when present there is one [level, pre] per return.
            if (object->hasProperty("sends")) {
                const auto sendsValue = object->getProperty("sends");
                const auto* sends = sendsValue.getArray();
                if (sends == nullptr || sends->size() != maxReturns) { error = "Invalid track sends."; return false; }
                for (int r = 0; r < maxReturns; ++r) {
                    const auto* send = (*sends)[r].getArray();
                    if (send == nullptr || send->size() != 2 || !((*send)[0].isDouble() || (*send)[0].isInt()) || !(*send)[1].isBool()) {
                        error = "Invalid track sends."; return false;
                    }
                    track.send[static_cast<std::size_t>(r)] = static_cast<float>(static_cast<double>((*send)[0]));
                    track.sendPre[static_cast<std::size_t>(r)] = static_cast<bool>((*send)[1]);
                }
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
            // Built on the heap: a pattern is 32 KiB, and an empty one with the default loop is not kept at all.
            auto built = std::make_shared<PianoRollPattern>();
            built->loopSteps = static_cast<int>(loopSteps);
            if (built->loopSteps < 1 || built->loopSteps > PianoRollPattern::maxLoopSteps) {
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
                    || start >= built->loopSteps * PianoRollPattern::ticksPerStep || length < 1
                    || length > built->loopSteps * PianoRollPattern::ticksPerStep - start
                    || !std::isfinite(noteVelocity) || noteVelocity <= 0.0 || noteVelocity > 1.0
                    || !std::isfinite(noteProbability) || noteProbability < 0.0 || noteProbability > 1.0) {
                    error = "A piano-roll note is outside the supported range."; return false;
                }
                const auto id = built->add(notePitch, PianoRollPattern::fromTicks(start),
                    PianoRollPattern::fromTicks(length), static_cast<float>(noteVelocity),
                    static_cast<float>(noteProbability));
                if (id == 0) { error = "A piano-roll note is outside the supported range."; return false; }
            }
            if (!built->valid()) { error = "A project clip contains an invalid piano-roll pattern."; return false; }
            if (built->count > 0 || built->loopSteps != PianoRollPattern::defaultLoopSteps)
                clip.pattern = std::shared_ptr<const PianoRollPattern>(std::move(built));
        }
    }

    if (formatVersion >= 8) {
        if (!parseTimeline(rootObject->getProperty("timeline"), parsed.timeline, error, formatVersion >= 9, formatVersion >= 10)) return false;
    } else if (rootObject->hasProperty("arrangement")) {
        // Before version 8 the song was whole bars of Session clips. Each becomes a region at the same place.
        const auto songValue = rootObject->getProperty("arrangement");
        const auto* object = songValue.getDynamicObject();
        if (object == nullptr || !object->getProperty("lengthBars").isInt()) { error="Invalid arrangement."; return false; }
        const int bars = static_cast<int>(object->getProperty("lengthBars"));
        if (bars < 1 || bars > 128) { error = "Invalid arrangement length."; return false; }
        parsed.timeline.setLengthBars(bars);
        const auto regionsValue=object->getProperty("regions"); const auto* regions=regionsValue.getArray();
        if (regions==nullptr || regions->size()>128) { error="Invalid arrangement regions.";return false; }
        for (const auto& value : *regions) {
            const auto* r=value.getDynamicObject();
            if(r==nullptr || !r->getProperty("track").isInt() || !r->getProperty("scene").isInt()
                || !r->getProperty("startBar").isInt() || !r->getProperty("lengthBars").isInt()) { error="Invalid region.";return false; }
            const int track = static_cast<int>(r->getProperty("track")), scene = static_cast<int>(r->getProperty("scene"));
            const int startBar = static_cast<int>(r->getProperty("startBar")), lengthBars = static_cast<int>(r->getProperty("lengthBars"));
            // The old format kept every region inside the song and none overlapping; anything else is damage.
            if (track < 0 || track >= trackCount || scene < 0 || scene >= sceneCount || startBar < 0 || lengthBars < 1
                || startBar + lengthBars > bars
                || std::any_of(parsed.timeline.regions.begin(), parsed.timeline.regions.end(), [&](const timeline::Region& other) {
                       return other.row == track && other.start < static_cast<std::int64_t>(startBar + lengthBars) * timeline::ticksPerBar
                           && other.end() > static_cast<std::int64_t>(startBar) * timeline::ticksPerBar; })
                || !parsed.timeline.placeBars(track, scene, startBar, lengthBars)) {
                error="Overlapping or out-of-range arrangement region.";return false;
            }
        }
    }
    if (formatVersion >= 6) {
        const auto rackValue = rootObject->getProperty("plugins"); const auto* racks = rackValue.getArray();
        if (!racks || racks->size() != trackCount || !rackFromVar(rootObject->getProperty("masterPlugins"), parsed.masterPlugins)) { error="Invalid plugin racks"; return false; }
        for (int t=0;t<trackCount;++t) if (!rackFromVar((*racks)[t],parsed.plugins[t])) { error="Invalid plugin state"; return false; }
    }
    if (formatVersion >= 7 && !rack::rackFromVar(rootObject->getProperty("channelRack"), parsed.channelRack, error)) return false;
    if (rootObject->hasProperty("returns")) {
        const auto returnsValue = rootObject->getProperty("returns");
        const auto* returns = returnsValue.getArray();
        if (returns == nullptr || returns->size() != maxReturns) { error = "Invalid return tracks."; return false; }
        for (int r = 0; r < maxReturns; ++r) {
            const auto* object = (*returns)[r].getDynamicObject();
            if (object == nullptr) { error = "Invalid return tracks."; return false; }
            const auto g = object->getProperty("gain"), pan = object->getProperty("pan"), mute = object->getProperty("mute");
            auto& settings = parsed.returns[static_cast<std::size_t>(r)];
            auto& plugins = parsed.returnPlugins[static_cast<std::size_t>(r)];
            if (!(g.isDouble() || g.isInt()) || !(pan.isDouble() || pan.isInt()) || !mute.isBool()
                || !rackFromVar(object->getProperty("plugins"), plugins) || !plugins.instrument.descriptionXml.isEmpty()) {
                error = "Invalid return tracks."; return false;
            }
            settings = {static_cast<float>(static_cast<double>(g)), static_cast<float>(static_cast<double>(pan)), static_cast<bool>(mute)};
            if (!settings.valid()) { error = "A return track's level or pan is out of range."; return false; }
        }
    }
    if (formatVersion >= 17) {
        const auto* timelineObject = rootObject->getProperty("timeline").getDynamicObject();
        if (!timelineObject || !timelineObject->hasProperty("parameterAutomation")) { error = "Missing parameter automation."; return false; }
        auto hasIds = [](const juce::var& rack) {
            const auto* o = rack.getDynamicObject();
            if (!o) return false;
            const auto instrument = o->getProperty("instrument");
            if (!instrument.getDynamicObject() || !instrument.getDynamicObject()->hasProperty("instance")) return false;
            const auto effects = o->getProperty("effects");
            if (!effects.getArray()) return false;
            for (const auto& p : *effects.getArray()) if (!p.getDynamicObject() || !p.getDynamicObject()->hasProperty("instance")) return false;
            return true;
        };
        const auto plugins = rootObject->getProperty("plugins");
        for (const auto& rack : *plugins.getArray()) if (!hasIds(rack)) { error = "Missing plugin identity."; return false; }
        if (!hasIds(rootObject->getProperty("masterPlugins"))) { error = "Missing master plugin identity."; return false; }
        const auto returns = rootObject->getProperty("returns");
        if (!returns.getArray()) { error = "Missing return tracks."; return false; }
        for (const auto& value : *returns.getArray()) if (!hasIds(value.getDynamicObject()->getProperty("plugins"))) { error = "Missing return plugin identity."; return false; }
    }
    if (!parsed.valid()) { error = "Project settings are outside the supported range."; return false; }
    destination = std::move(parsed);
    error.clear();
    return true;
}

} // namespace alien
