#pragma once
#include "ClipLibrary.h"
#include "PianoRollSequence.h"
#include "RackLibrary.h"
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
    std::uint64_t rackClip{};
    juce::String rackName;

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
    bool isEmpty() const noexcept { return rackClip == 0 && audio == nullptr && audioFilePath.isEmpty() && pattern->count == 0; }
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
    bool legacyRackSceneLaunch{};
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

    // The Channel Rack: its channels, and its pattern bank as the library holds it. Each pattern slot names a clip
    // (channelRack.clips) whose lanes are the channels' rows (T1-03 increment C). The patterns here are that
    // content as the rack views and the engine read it; change them only with setRack. Decoded samples are held
    // by the ProjectController.
    rack::Project channelRack = rack::Project::createDefault();

    static ProjectDocument createDefault(int trackCount = sessionProjectTrackCount,
                                         int sceneCount = sessionProjectSceneCount) {
        ProjectDocument d;
        for (int t = 0; t < trackCount; ++t) d.tracks.push_back({defaultTrackName(t), {}, t});
        for (int s = 0; s < sceneCount; ++s) d.scenes.push_back({defaultSceneName(s)});
        d.cells.assign(static_cast<std::size_t>(sceneCount),
                       std::vector<library::ClipId>(static_cast<std::size_t>(trackCount)));
        for (const int step : {0, 4, 8, 12}) d.steps.on[static_cast<std::size_t>(step)] = true;
        d.setRack(rack::Project::createDefault());
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
    int rackPatternForClip(std::uint64_t id) const noexcept {
        if (id != 0) for (int p = 0; p < channelRack.patternCount; ++p)
            if (channelRack.clips[static_cast<std::size_t>(p)] == id) return p;
        return -1;
    }
    bool bindRackClip(int track, int scene, std::uint64_t id) {
        if (!isValidCell(track, scene) || rackPatternForClip(id) < 0) return false;
        for (const auto* list : {&timeline.regions, &timeline.frozenOriginals, &timeline.takes})
            for (const auto& r : *list)
                if (r.playsCell() && r.row == track && r.source == scene) return false;
        if (cellClip(track, scene).value == id) return false;
        cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)] = library::ClipId{id};
        prune();
        return true;
    }
    // What a library clip plays (an empty clip for an unknown id).
    SessionClip contentOf(library::ClipId id) const {
        SessionClip view;
        if (const int p = rackPatternForClip(id.value); p >= 0) {
            view.rackClip = id.value; view.rackName = juce::String::fromUTF8(channelRack.patternNames[static_cast<std::size_t>(p)].c_str());
            return view;
        }
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
        if (content.rackClip != 0) { bindRackClip(track, scene, content.rackClip); return; }
        // Replacing a rack assignment creates ordinary content; it never rewrites the shared rack source.
        if (clip(track, scene).rackClip != 0) cells[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)] = {};
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
        for (int p = 0; p < channelRack.patternCount; ++p)
            if (const auto id = channelRack.clips[static_cast<std::size_t>(p)]) roots.push_back(library::ClipId{id});
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

    // The only way to change the Channel Rack's patterns: every slot whose content changed becomes a new revision of
    // its clip (a slot without one, such as a new or copied pattern, gets a clip of its own), pattern names become
    // clip names, and clips of deleted slots are dropped. Each slot must come back exactly from what the library now
    // holds; if one does not, or `next` is invalid, nothing changes and this returns false. Channels, routing and
    // view settings are taken as they are.
    bool setRack(rack::Project next) {
        if (!next.valid()) return false;
        auto lib = library;
        std::vector<std::uint64_t> used;
        for (int p = 0; p < next.patternCount; ++p) {
            const auto i = static_cast<std::size_t>(p);
            auto& key = next.clips[i];
            // One clip per slot, and only a clip this rack already owns: an id from another document's library
            // (a rack read from a file, say) could name some other clip here.
            const auto owned = channelRack.clips.begin() + channelRack.patternCount;
            if (std::find(used.begin(), used.end(), key) != used.end() || std::find(channelRack.clips.begin(), owned, key) == owned) key = 0;
            const library::ClipInfo info{next.patternNames[i], 0};
            const auto* clip = key != 0 ? lib.clip(library::ClipId{key}) : nullptr;
            // The same pattern object, in the same slot of the same clip, over the same lanes: nothing to do.
            if (clip != nullptr && next.lanes == channelRack.lanes && next.channelCount == channelRack.channelCount) {
                bool same = false;
                for (int q = 0; q < channelRack.patternCount && !same; ++q)
                    same = channelRack.clips[static_cast<std::size_t>(q)] == key && channelRack.patterns[static_cast<std::size_t>(q)] == next.patterns[i];
                if (same) { lib.setClipInfo(library::ClipId{key}, info); used.push_back(key); continue; }
            }
            const auto data = rack::toPatternData(next, p);
            if (data == nullptr || !data->valid()) return false;
            if (clip != nullptr) {
                const auto* pattern = lib.pattern(clip->pattern);
                if (pattern == nullptr) return false;
                if (!rack::samePatternData(*pattern->data, *data)
                    && lib.editPattern(clip->pattern, pattern->revision, data) != library::EditResult::done) return false;
                lib.setClipInfo(library::ClipId{key}, info);
            } else if ((key = lib.addClip(data, info).value) == 0) return false;
            used.push_back(key);
            const auto rebuilt = rack::fromPatternData(*lib.content(library::ClipId{key}), next, next.patterns[i].get());
            if (rebuilt == nullptr || !(*rebuilt == next.pattern(p))) return false;   // never lose a step or a note
        }
        // Sources with saved uses must remain in the bank. Refuse deletion atomically until their uses are cleared.
        const auto kept = [&](std::uint64_t id) {
            return std::find(next.clips.begin(), next.clips.begin() + next.patternCount, id) != next.clips.begin() + next.patternCount;
        };
        for (const auto& scene : cells) for (auto id : scene)
            if (rackPatternForClip(id.value) >= 0 && !kept(id.value)) return false;
        auto rebound = timeline;
        for (auto& r : rebound.regions) if (r.isPattern()) {
            const auto id = r.rackClip; // Document ownership is always by clip ID; indices are generated caches.
            if (id == 0 || !kept(id)) return false;
            r.rackClip = id;
            r.source = static_cast<int>(std::find(next.clips.begin(), next.clips.begin() + next.patternCount, id) - next.clips.begin());
        }
        library = std::move(lib);
        channelRack = std::move(next);
        timeline = std::move(rebound);
        prune();
        return true;
    }
    // The Channel Rack rebuilt from the library alone (what a save writes). Equal to channelRack.
    rack::Project rackFromLibrary() const {
        auto rebuilt = channelRack;
        for (int p = 0; p < channelRack.patternCount; ++p) {
            const auto i = static_cast<std::size_t>(p);
            const auto content = library.content(library::ClipId{channelRack.clips[i]});
            if (content == nullptr) continue;   // not in the library (a document made without createDefault)
            if (auto pattern = rack::fromPatternData(*content, channelRack, channelRack.patterns[i].get())) rebuilt.patterns[i] = std::move(pattern);
        }
        return rebuilt;
    }
    // Whether every rack pattern is in the library and is exactly what it holds.
    bool rackMatchesLibrary() const {
        for (int p = 0; p < channelRack.patternCount; ++p) {
            const auto i = static_cast<std::size_t>(p);
            const auto content = library.content(library::ClipId{channelRack.clips[i]});
            const auto rebuilt = content != nullptr ? rack::fromPatternData(*content, channelRack, nullptr) : nullptr;
            if (rebuilt == nullptr || !(*rebuilt == channelRack.pattern(p))) return false;
        }
        return true;
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

    // Rewrites every sidechain key of kind `from` with `map` (old index -> new index, or -1 for none), after
    // tracks moved or a track, bus or return went away, so a key never lands on whatever takes its place.
    template <class Map>
    void remapSidechains(Map&& map, KeySource from = KeySource::track) {
        const auto rack = [&](PluginRackState& r) {
            for (auto& effect : r.effects) {
                const bool trackKey = effect.sidechainFrom == KeySource::track || effect.sidechainFrom == KeySource::trackPostFader;
                const bool matches = from == KeySource::track ? trackKey : effect.sidechainFrom == from;
                if (effect.sidechain < 0 || !matches) continue;
                effect.sidechain = map(effect.sidechain);
                if (effect.sidechain < 0) effect.sidechainFrom = KeySource::track;
            }
        };
        for (auto& track : tracks) rack(track.plugins);
        rack(masterPlugins);
        for (auto& r : returns) rack(r.plugins);
        for (auto& bus : buses) rack(bus.plugins);
    }
    void sidechainTracksSwapped(int a, int b) { remapSidechains([=](int t) { return t == a ? b : t == b ? a : t; }); }
    void sidechainTrackDeleted(int track) { remapSidechains([=](int t) { return t == track ? -1 : t > track ? t - 1 : t; }); }
    // Buses and returns are slots: removing one leaves the others where they are, and drops keys from it.
    void sidechainBusDeleted(int bus) { remapSidechains([=](int b) { return b == bus ? -1 : b; }, KeySource::bus); }
    void sidechainReturnDeleted(int r) { remapSidechains([=](int k) { return k == r ? -1 : k; }, KeySource::returnTrack); }

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
    if (a.legacyRackSceneLaunch != b.legacyRackSceneLaunch || a.bpm != b.bpm || a.launchQuantizationSteps != b.launchQuantizationSteps || a.livePreset != b.livePreset
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

    data.channelRack = doc.rackFromLibrary();
    data.legacyRackSceneLaunch = doc.legacyRackSceneLaunch;
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
            destination.rackClip = doc.clip(t, s).rackClip;
            if (const auto link = doc.linkOf(t, s); destination.rackClip == 0 && link != 0 && uses[link] > 1) destination.link = link;
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
    // Rack ids belong to the saved document's namespace. Rebind all uses together after importing its bank.
    doc.timeline = {};
    doc.timeline.tempo = data.bpm;
    doc.masterPlugins = data.masterPlugins;
    for (std::size_t r = 0; r < doc.returns.size(); ++r)
        doc.returns[r] = {data.returns[r].used, data.returns[r].name, data.returns[r].settings, data.returnPlugins[r]};
    for (std::size_t b = 0; b < doc.buses.size(); ++b)
        doc.buses[b] = {data.buses[b].used, data.buses[b].name, data.buses[b].settings, data.busPlugins[b]};

    // Each saved pattern becomes a clip of its own (copies made with addPattern stay independent).
    if (!doc.setRack(data.channelRack)) doc.channelRack = data.channelRack;   // only for data that failed validation
    std::map<std::uint64_t, std::uint64_t> rackIds;
    for (int p = 0; p < data.channelRack.patternCount; ++p)
        rackIds[data.channelRack.clips[static_cast<std::size_t>(p)]] = doc.channelRack.clips[static_cast<std::size_t>(p)];
    doc.timeline = data.timeline;
    doc.timeline.tempo = data.bpm;
    for (auto& r : doc.timeline.regions) if (r.isPattern())
        r.rackClip = r.rackClip != 0 ? rackIds.at(r.rackClip) : doc.channelRack.clips[static_cast<std::size_t>(r.source)];
    doc.legacyRackSceneLaunch = data.legacyRackSceneLaunch;
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
            if (source.rackClip != 0) {
                doc.bindRackClip(t, s, rackIds.at(source.rackClip));
                continue;
            }
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
