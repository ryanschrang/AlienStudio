#pragma once
#include "ClipLibrary.h"
#include "PianoRollSequence.h"
#include "SessionAudioClip.h"
#include "SessionProject.h"
#include "TrackMixer.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <map>
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

// What one Session cell plays: a read-only view of its clip in the document's library
// (ProjectDocument::clip). To change a cell, change a copy and give it to setClip.
struct SessionClip {
    std::shared_ptr<const PianoRollPattern> pattern = emptyPattern();
    // Decoded audio is immutable and retained by the ProjectController for the
    // lifetime of the engine, so the engine may hold raw pointers to it.
    std::shared_ptr<const SessionAudioClip> audio;
    juce::String audioFileName, audioFilePath;

    SessionClip() = default;
    SessionClip(const SessionClip&) = default;
    SessionClip(SessionClip&&) = default;
    // Only a named copy can be assigned: `doc.clip(t, s) = ...` would change a temporary and nothing else.
    SessionClip& operator=(const SessionClip&) & = default;
    SessionClip& operator=(SessionClip&&) & = default;

    const PianoRollPattern& notes() const noexcept { return *pattern; }
    bool hasAudio() const noexcept { return audio != nullptr; }
    // The project names an audio file that could not be loaded (moved, deleted, or
    // unreadable). The reference is kept so saving does not lose it and the file
    // can be relinked; the clip is silent until then.
    bool audioOffline() const noexcept { return audio == nullptr && audioFilePath.isNotEmpty(); }
    // An audio clip, loaded or offline, as opposed to a MIDI clip.
    bool isAudioClip() const noexcept { return audio != nullptr || audioFilePath.isNotEmpty(); }
    bool isEmpty() const noexcept { return audio == nullptr && audioFilePath.isEmpty() && pattern->count == 0; }
};

// An audio file a clip plays: the decoded audio (null when the file is offline) and where it is.
struct SessionMedia {
    std::shared_ptr<const SessionAudioClip> audio;
    juce::String fileName, filePath;
    bool operator==(const SessionMedia&) const = default;
};

struct SessionTrack {
    juce::String name;
    TrackSettings settings;
    int colour{}; // Index into the track colour palette.
    PluginRackState plugins;
    std::array<juce::String, drumPadCount> drumPaths{};
    std::array<std::shared_ptr<const SessionAudioClip>, drumPadCount> drumAssets{};
    std::uint32_t id = newObjectId();
};

// A return track slot (see SessionReturn): whether it exists, its name, strip and FX rack.
struct ReturnTrack {
    bool used{};
    juce::String name;
    ReturnSettings settings;
    PluginRackState plugins;
    bool operator==(const ReturnTrack&) const = default;
};
// Returns A-D, as in a new project.
inline std::array<ReturnTrack, maxReturns> defaultReturnTracks() {
    std::array<ReturnTrack, maxReturns> returns{};
    for (int r = 0; r < defaultReturns; ++r) returns[static_cast<std::size_t>(r)] = {true, defaultReturnName(r), {}, {}};
    return returns;
}

// A group bus slot (see SessionBus): whether it exists, its name, strip and FX rack.
struct GroupBus {
    bool used{};
    juce::String name;
    BusSettings settings;
    PluginRackState plugins;
    bool operator==(const GroupBus&) const = default;
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
    // The Session grid, [scene][track]: each cell names the clip it launches, or none. The clip's
    // musical content lives once, in `library` (T1-03; see docs/SHARED_CLIP_MODEL.md), and the
    // audio it plays in `media`. Change cells only with setClip and clearClip.
    std::vector<std::vector<library::ClipId>> cells;
    library::Library library;
    std::map<std::uint64_t, SessionMedia> media;   // by MediaId
    std::uint64_t nextMedia{1};
    int bpm{120};
    int launchQuantizationSteps{16};
    float masterGain{1.0f}; // whole-mix level, 0..maxMasterGain
    RecordSettings recording; // metronome, count-in and recording options
    int livePreset{};
    int selectedTrack{};
    int selectedScene{};
    StepSequence steps;
    alien::timeline::Timeline timeline;   // the Arrangement view: where clips and rack patterns sit on the song
    PluginRackState masterPlugins;
    std::array<ReturnTrack, maxReturns> returns = defaultReturnTracks();   // return tracks, by slot
    std::array<GroupBus, maxBuses> buses{};          // group buses, by slot

    rack::Project channelRack = rack::Project::createDefault(); // the Channel Rack; decoded samples are held by the ProjectController

    static ProjectDocument createDefault(int trackCount = sessionProjectTrackCount,
                                         int sceneCount = sessionProjectSceneCount) {
        ProjectDocument d;
        for (int t = 0; t < trackCount; ++t) d.tracks.push_back({defaultTrackName(t), {}, t});
        for (int s = 0; s < sceneCount; ++s) d.scenes.push_back({defaultSceneName(s)});
        d.cells.assign(static_cast<std::size_t>(sceneCount),
                       std::vector<library::ClipId>(static_cast<std::size_t>(trackCount)));
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
    library::ClipId cellClip(int track, int scene) const {
        return cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
    }
    // What a cell plays, resolved from the library: an empty clip for an empty cell.
    SessionClip clip(int track, int scene) const { return contentOf(cellClip(track, scene)); }
    // What a library clip plays (an empty clip for an unknown id).
    SessionClip contentOf(library::ClipId id) const {
        SessionClip view;
        const auto content = library.content(id);
        if (content == nullptr) return view;
        for (const auto& lane : content->lanes) {
            if (const auto* notes = std::get_if<library::NoteLane>(&lane.content)) view.pattern = notes->notes;
            else if (const auto* audio = std::get_if<library::AudioLane>(&lane.content)) {
                const auto found = media.find(audio->media.value);
                if (found != media.end()) { view.audio = found->second.audio; view.audioFileName = found->second.fileName; view.audioFilePath = found->second.filePath; }
            }
        }
        return view;
    }
    SessionClip selectedClip() const { return clip(selectedTrack, selectedScene); }
    // What a Song region on a track plays: its placement clip, or its cell (empty if that is gone).
    SessionClip placementContent(const alien::timeline::Region& r) const {
        if (r.ownsClip()) return contentOf(library::ClipId{r.clip});
        return isValidCell(r.row, r.source) ? clip(r.row, r.source) : SessionClip{};
    }
    // Every placement clip the song uses (its regions and frozen originals), each once, ascending.
    std::vector<std::uint64_t> placementClips() const {
        std::vector<std::uint64_t> ids;
        for (const auto* list : {&timeline.regions, &timeline.frozenOriginals})
            for (const auto& r : *list) if (r.ownsClip()) ids.push_back(r.clip);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }
    // A new placement clip holding `notes` (MIDI only; placement clips have no audio).
    library::ClipId addPlacementClip(std::shared_ptr<const PianoRollPattern> notes, juce::String name = {}) {
        auto data = std::make_shared<library::PatternData>();
        if (notes == nullptr) notes = emptyPattern();
        data->lengthTicks = library::PatternData::loopTicks(*notes);
        data->lanes.push_back({library::LaneId{1}, library::NoteLane{std::move(notes)}});
        return library.addClip(std::shared_ptr<const library::PatternData>(std::move(data)), {name.toStdString(), 0});
    }
    // New notes for a placement clip, as a new revision: every region that names it hears them.
    bool setPlacementNotes(library::ClipId id, std::shared_ptr<const PianoRollPattern> notes) {
        const auto* existing = library.clip(id);
        const auto* pattern = existing != nullptr ? library.pattern(existing->pattern) : nullptr;
        if (pattern == nullptr || notes == nullptr || !notes->valid()) return false;
        auto data = std::make_shared<library::PatternData>(*pattern->data);
        data->lengthTicks = library::PatternData::loopTicks(*notes);
        data->lanes.clear();
        data->lanes.push_back({library::LaneId{1}, library::NoteLane{std::move(notes)}});
        return library.editPattern(existing->pattern, pattern->revision, std::move(data)) == library::EditResult::done;
    }
    // A placement clip's name (the scene it was made from), for the Song view.
    juce::String placementName(std::uint64_t id) const {
        const auto* c = library.clip(library::ClipId{id});
        return c != nullptr ? juce::String::fromUTF8(c->info.name.c_str(), static_cast<int>(c->info.name.size())) : juce::String{};
    }
    // The pattern a cell's clip plays (0 for an empty cell). Cells with the same value are linked.
    std::uint64_t linkOf(int track, int scene) const {
        const auto* c = library.clip(cellClip(track, scene));
        return c != nullptr ? c->pattern.value : 0;
    }
    // How many cells play the same content as this one (1 when it is its own, 0 when empty).
    int linkedCells(int track, int scene) const {
        const auto link = linkOf(track, scene);
        if (link == 0) return 0;
        int n = 0;
        for (int s = 0; s < sceneCount(); ++s) for (int t = 0; t < trackCount(); ++t) n += linkOf(t, s) == link ? 1 : 0;
        return n;
    }
    // Gives a cell new content, as a new revision of the clip it already has (every use of that
    // clip hears it), or as a new clip for an empty cell. Content that is just a new, empty clip (no
    // audio, no notes, the default loop) empties the cell; an empty clip with its own loop is kept.
    void setClip(int track, int scene, const SessionClip& content) {
        const auto& blank = *emptyPattern();
        const bool nothing = !content.isAudioClip() && (content.pattern == nullptr
            || (content.pattern->count == 0 && content.pattern->loopSteps == blank.loopSteps));
        if (nothing) { clearClip(track, scene); return; }
        auto data = std::make_shared<library::PatternData>();
        const auto& notes = content.pattern != nullptr ? content.pattern : emptyPattern();
        data->lengthTicks = library::PatternData::loopTicks(*notes);
        data->lanes.push_back({library::LaneId{1}, library::NoteLane{notes}});
        if (content.isAudioClip()) data->lanes.push_back({library::LaneId{2}, library::AudioLane{mediaFor(track, scene, content)}});
        auto& cell = cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        if (const auto* existing = library.clip(cell)) {
            const auto* pattern = library.pattern(existing->pattern);
            library.editPattern(existing->pattern, pattern->revision, std::move(data));
        } else cell = library.addClip(std::shared_ptr<const library::PatternData>(std::move(data)));
        prune();
    }
    void clearClip(int track, int scene) {
        auto& cell = cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        if (!cell) return;
        cell = {};
        prune();
    }
    // Drops clips no cell or Song placement uses any more, and audio no clip plays. (Undo keeps its own copies.)
    void prune() {
        std::vector<library::ClipId> roots;
        for (const auto& scene : cells) for (const auto id : scene) if (id) roots.push_back(id);
        for (const auto id : placementClips()) roots.push_back(library::ClipId{id});
        library.prune(roots);
        std::erase_if(media, [&](const auto& entry) {
            for (const auto& scene : cells) for (const auto id : scene) {
                const auto content = library.content(id);
                if (content == nullptr) continue;
                for (const auto& lane : content->lanes)
                    if (const auto* audio = std::get_if<library::AudioLane>(&lane.content); audio != nullptr && audio->media.value == entry.first) return false;
            }
            return true;
        });
    }

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

    // Rewrites every effect's sidechain track with `map` (old track -> new track, or -1 for none),
    // after tracks moved or one was deleted, so a key never lands on whatever takes a track's place.
    template <class Map>
    void remapSidechains(Map&& map) {
        const auto rack = [&](PluginRackState& r) { for (auto& effect : r.effects) if (effect.sidechain >= 0) effect.sidechain = map(effect.sidechain); };
        for (auto& track : tracks) rack(track.plugins);
        rack(masterPlugins);
        for (auto& r : returns) rack(r.plugins);
        for (auto& bus : buses) rack(bus.plugins);
    }
    void sidechainTracksSwapped(int a, int b) { remapSidechains([=](int t) { return t == a ? b : t == b ? a : t; }); }
    void sidechainTrackDeleted(int track) { remapSidechains([=](int t) { return t == track ? -1 : t > track ? t - 1 : t; }); }

    void clampSelection() noexcept {
        selectedTrack = juce::jlimit(0, juce::jmax(0, trackCount() - 1), selectedTrack);
        selectedScene = juce::jlimit(0, juce::jmax(0, sceneCount() - 1), selectedScene);
    }
private:
    // The media entry for `content`'s audio: the cell's own entry when it already plays that audio
    // from that file, otherwise a new one.
    library::MediaId mediaFor(int track, int scene, const SessionClip& content) {
        const SessionMedia wanted{content.audio, content.audioFileName, content.audioFilePath};
        if (const auto existing = library.content(cellClip(track, scene)))
            for (const auto& lane : existing->lanes)
                if (const auto* audio = std::get_if<library::AudioLane>(&lane.content)) {
                    const auto found = media.find(audio->media.value);
                    if (found != media.end() && found->second == wanted) return audio->media;
                }
        const library::MediaId id{nextMedia++};
        media.emplace(id.value, wanted);
        return id;
    }
};

// True when two documents have the same content (selection is ignored).
// Clip contents are compared by identity, which is exact because they are
// immutable and replaced on every edit.
inline bool sameContent(const ProjectDocument& a, const ProjectDocument& b) {
    auto sameSettings = [](const TrackSettings& x, const TrackSettings& y) {
        return x.preset == y.preset && x.gain == y.gain && x.pan == y.pan && x.mute == y.mute && x.solo == y.solo && x.instrument == y.instrument
            && x.armed == y.armed && x.input == y.input && x.stereoInput == y.stereoInput && x.monitor == y.monitor
            && x.send == y.send && x.sendPre == y.sendPre && x.freezeScene == y.freezeScene && x.output == y.output;
    };
    if (a.masterPlugins != b.masterPlugins || a.masterGain != b.masterGain || !(a.recording == b.recording) || a.returns != b.returns
        || a.buses != b.buses) return false;

    if (!a.channelRack.sameContent(b.channelRack)) return false; // which pattern is selected is not an edit
    if (!(a.timeline == b.timeline)) return false;
    if (a.bpm != b.bpm || a.launchQuantizationSteps != b.launchQuantizationSteps || a.livePreset != b.livePreset
        || a.steps.on != b.steps.on || a.steps.pitch != b.steps.pitch || a.steps.velocity != b.steps.velocity
        || a.tracks.size() != b.tracks.size() || a.scenes.size() != b.scenes.size())
        return false;
    for (std::size_t t = 0; t < a.tracks.size(); ++t)
        if (a.tracks[t].id != b.tracks[t].id || a.tracks[t].name != b.tracks[t].name
            || a.tracks[t].plugins != b.tracks[t].plugins || a.tracks[t].drumAssets != b.tracks[t].drumAssets || a.tracks[t].colour != b.tracks[t].colour || !sameSettings(a.tracks[t].settings, b.tracks[t].settings))
            return false;
    for (std::size_t s = 0; s < a.scenes.size(); ++s)
        if (a.scenes[s].id != b.scenes[s].id || a.scenes[s].name != b.scenes[s].name) return false;
    for (int s = 0; s < a.sceneCount(); ++s)
        for (int t = 0; t < a.trackCount(); ++t) {
            const auto x = a.clip(t, s);
            const auto y = b.clip(t, s);
            // Audio paths are excluded: they only change with the audio itself, or
            // when a portable save re-points them at collected copies.
            if (x.pattern != y.pattern || x.audio != y.audio) return false;
            // Which cells share content is part of the song: linking two equal cells is an edit.
            if (a.linkOf(t, s) != b.linkOf(t, s)) return false;
        }
    // The timelines are equal, so both name the same placement clips.
    for (const auto id : a.placementClips())
        if (a.contentOf(library::ClipId{id}).pattern != b.contentOf(library::ClipId{id}).pattern) return false;
    return true;
}

// Conversion to and from the on-disk session format. Audio paths are copied as
// strings; decoding is done separately by the loader.
inline SessionProjectData toProjectData(const ProjectDocument& doc) {
    SessionProjectData data(doc.trackCount(), doc.sceneCount());
    data.timeline = doc.timeline;
    data.masterPlugins = doc.masterPlugins;
    for (std::size_t r = 0; r < doc.returns.size(); ++r) {
        const auto& ret = doc.returns[r];
        data.returns[r] = {ret.used, ret.used ? ret.name : juce::String{}, ret.settings};
        data.returnPlugins[r] = ret.used ? ret.plugins : PluginRackState{};
    }
    for (std::size_t b = 0; b < doc.buses.size(); ++b) {
        const auto& bus = doc.buses[b];
        data.buses[b] = {bus.used, bus.used ? bus.name : juce::String{}, bus.settings};
        data.busPlugins[b] = bus.used ? bus.plugins : PluginRackState{};
    }

    data.channelRack = doc.channelRack;
    data.launchQuantizationSteps = doc.launchQuantizationSteps;
    data.masterGain = doc.masterGain;
    data.recording = doc.recording;
    data.selectedTrack = doc.selectedTrack;
    data.selectedScene = doc.selectedScene;
    data.bpm = doc.bpm;
    data.synthPreset = doc.livePreset;
    for (int t = 0; t < doc.trackCount(); ++t) {
        const auto& track = doc.tracks[static_cast<std::size_t>(t)];
        data.plugins[t] = track.plugins;
        data.drumPaths[static_cast<std::size_t>(t)] = track.drumPaths;
        data.trackSettings[static_cast<std::size_t>(t)] = track.settings;
        data.trackNames[static_cast<std::size_t>(t)] = track.name;
        data.trackColours[static_cast<std::size_t>(t)] = track.colour;
    }
    for (int s = 0; s < doc.sceneCount(); ++s)
        data.sceneNames[static_cast<std::size_t>(s)] = doc.scenes[static_cast<std::size_t>(s)].name;
    data.stepPattern = doc.steps.on;
    data.stepPitches = doc.steps.pitch;
    data.stepVelocities = doc.steps.velocity;
    std::map<std::uint64_t, int> uses;   // how many cells play each pattern
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t) if (const auto link = doc.linkOf(t, s)) ++uses[link];
    for (int s = 0; s < doc.sceneCount(); ++s)
        for (int t = 0; t < doc.trackCount(); ++t) {
            auto& destination = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            destination.pattern = doc.clip(t, s).pattern;   // shared, not copied
            destination.audioFilePath = doc.clip(t, s).audioFilePath;
            if (const auto link = doc.linkOf(t, s); link != 0 && uses[link] > 1) destination.link = link;   // saved only where content is shared
        }
    for (const auto id : doc.placementClips()) {
        auto& destination = data.placementClips[id];
        destination.pattern = doc.contentOf(library::ClipId{id}).pattern;
        destination.name = doc.placementName(id);
    }
    return data;
}

inline ProjectDocument fromProjectData(const SessionProjectData& data) {
    auto doc = ProjectDocument::createDefault(data.trackCount(), data.sceneCount());
    doc.timeline = data.timeline;
    doc.timeline.tempo = data.bpm;
    doc.masterPlugins = data.masterPlugins;
    for (std::size_t r = 0; r < doc.returns.size(); ++r)
        doc.returns[r] = {data.returns[r].used, data.returns[r].name, data.returns[r].settings, data.returnPlugins[r]};
    for (std::size_t b = 0; b < doc.buses.size(); ++b)
        doc.buses[b] = {data.buses[b].used, data.buses[b].name, data.buses[b].settings, data.busPlugins[b]};

    doc.channelRack = data.channelRack;
    doc.launchQuantizationSteps = data.launchQuantizationSteps;
    doc.masterGain = data.masterGain;
    doc.recording = data.recording;
    doc.selectedTrack = data.selectedTrack;
    doc.selectedScene = data.selectedScene;
    doc.bpm = data.bpm;
    doc.livePreset = data.synthPreset;
    for (int t = 0; t < data.trackCount(); ++t) {
        auto& track = doc.tracks[static_cast<std::size_t>(t)];
        track.plugins = data.plugins[t];
        track.drumPaths = data.drumPaths[static_cast<std::size_t>(t)];
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
    std::map<std::uint64_t, library::ClipId> links;   // saved link -> the first clip with it
    for (int s = 0; s < data.sceneCount(); ++s)
        for (int t = 0; t < data.trackCount(); ++t) {
            const auto& source = data.clips[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            // A linked cell after the first of its link shares that cell's content; every other cell is its own.
            if (source.link != 0) {
                const auto first = links.find(source.link);
                if (first != links.end() && doc.library.clip(first->second) != nullptr) {
                    doc.cells[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)] = doc.library.linkClip(first->second);
                    continue;
                }
            }
            SessionClip clip;
            clip.pattern = source.pattern.shared() != nullptr ? source.pattern.shared() : emptyPattern();
            clip.audioFilePath = source.audioFilePath;
            doc.setClip(t, s, clip);
            if (source.link != 0 && doc.cellClip(t, s)) links[source.link] = doc.cellClip(t, s);
        }
    // Placement clips get new ids in this library; the regions follow.
    std::map<std::uint64_t, std::uint64_t> placed;
    for (const auto& [saved, source] : data.placementClips)
        placed[saved] = doc.addPlacementClip(source.pattern.shared() != nullptr ? source.pattern.shared() : emptyPattern(), source.name).value;
    for (auto* list : {&doc.timeline.regions, &doc.timeline.frozenOriginals})
        for (auto& r : *list) if (r.ownsClip()) { const auto found = placed.find(r.clip); r.clip = found != placed.end() ? found->second : 0; }
    doc.prune();
    doc.clampSelection();
    return doc;
}

} // namespace alien
