#pragma once
#include "ProjectDocument.h"
#include "SceneCommands.h"
#include "SongClipCommands.h"
#include <utility>

// Two Song edits that also touch the Session or the Channel Rack (the NewSongClip and MakeSongClipUnique commands).
// Pure: the controller and Preview both run them, and a refusal leaves the document as it was.
namespace alien::song_placement {

// A new, empty MIDI clip for a track at `start`. It takes the first Session cell of that track that is empty and that
// the Song does not already use (the selected scene first), so every new clip is a clip of its own; when there is no
// such cell a scene is added. The cell is selected, ready to be opened in the piano roll.
inline juce::String newClip(ProjectDocument& d, int row, std::int64_t start, std::uint32_t* idOut = nullptr) {
    if (timeline::isPatternRow(row) || row < 0 || row >= d.trackCount()) return "Choose a track for the new clip.";
    if (d.tracks[static_cast<std::size_t>(row)].settings.frozen()) return "Unfreeze the track before adding a clip to it.";
    if (start < 0) return "A new clip does not fit there.";
    auto next = d;
    const auto usedBySong = [&](int scene) {
        for (const auto& r : next.timeline.takes) if (r.row == row && r.source == scene) return true;
        for (const auto& r : next.timeline.regions) if (r.row == row && r.source == scene) return true;
        return false;
    };
    const auto isFree = [&](int scene) { return scene >= 0 && scene < next.sceneCount() && next.clip(row, scene).isEmpty() && !usedBySong(scene); };
    int scene = -1;
    if (isFree(next.selectedScene)) scene = next.selectedScene;
    for (int s = 0; s < next.sceneCount() && scene < 0; ++s) if (isFree(s)) scene = s;
    if (scene < 0) {
        if (!next.canAddScene() || scene_edit::apply(next, {scene_edit::Kind::create}).isNotEmpty())
            return "A new clip does not fit there: the Session has no room for another scene.";
        scene = next.sceneCount() - 1;
    }
    std::uint32_t id = 0;
    auto song = next.timeline;
    song.tempo = next.bpm;
    timeline::Region region;
    region.row = row; region.source = scene; region.start = start; region.length = song_clip::naturalLength(next, row, scene, start);
    if (!song.place(region, &id) || !song.valid(next.trackCount(), next.sceneCount(), next.channelRack.patternCount)) return "A new clip does not fit there.";
    next.timeline = std::move(song);
    next.selectedTrack = row; next.selectedScene = scene;
    d = std::move(next);
    if (idOut != nullptr) *idOut = id;
    return {};
}

// Whether a Song placement shares what it plays with something else: another placement of the same cell or
// placement clip, or a linked cell. A pattern placement shares its rack pattern.
inline bool shared(const ProjectDocument& d, std::uint32_t regionId) {
    const auto* r = d.timeline.find(regionId);
    if (r == nullptr) return false;
    if (r->isPattern()) return d.rackPatternForClip(r->rackClip) >= 0;
    if (r->ownsClip()) {
        int uses = 0;
        for (const auto& other : d.timeline.regions) uses += other.clip == r->clip ? 1 : 0;
        return uses > 1;
    }
    if (!d.isValidCell(r->row, r->source) || !d.cellClip(r->row, r->source)) return false;
    int uses = 0;
    for (const auto& other : d.timeline.regions) uses += other.playsCell() && other.row == r->row && other.source == r->source ? 1 : 0;
    return uses > 1 || d.linkedCells(r->row, r->source) > 1;
}

// Gives one Song placement content of its own: an independent copy of what it plays. A pattern placement gets a new
// rack pattern; a MIDI placement gets a placement clip, which no Session cell launches; an audio placement gets a
// free cell of its track (a scene is added when there is none). The cell and the other placements keep theirs.
inline juce::String makeUnique(ProjectDocument& d, std::uint32_t regionId) {
    if (d.timeline.find(regionId) == nullptr) return "That Song clip is no longer there.";
    if (!shared(d, regionId)) return "That placement already plays content of its own.";
    const juce::String noRoom = "The placement could not get its own copy.";
    auto next = d;
    auto* own = next.timeline.find(regionId);
    const auto fits = [&] { return next.timeline.valid(next.trackCount(), next.sceneCount(), next.channelRack.patternCount); };
    if (own->isPattern()) {
        auto rack = next.channelRack;
        const int slot = rack.patternCount;
        const int source = next.rackPatternForClip(own->rackClip);
        if (source < 0 || !rack.addPattern(source)) return "The Channel Rack has no room for another pattern.";
        rack.selectedPattern = next.channelRack.selectedPattern;
        if (!next.setRack(std::move(rack))) return noRoom;
        own = next.timeline.find(regionId);
        own->source = slot; own->rackClip = next.channelRack.clips[static_cast<std::size_t>(slot)];
        if (!fits()) return noRoom;
        d = std::move(next);
        return {};
    }
    const int row = own->row, from = own->source;
    if (row < 0 || row >= next.trackCount()) return noRoom;
    if (next.tracks[static_cast<std::size_t>(row)].settings.frozen()) return "Unfreeze the track before changing its Song clips.";
    const auto content = next.placementContent(*own);
    if (!content.isAudioClip()) {
        const auto name = own->ownsClip() ? next.placementName(own->clip) : next.scenes[static_cast<std::size_t>(from)].name;
        const auto id = next.addPlacementClip(content.pattern, name);
        own = next.timeline.find(regionId);
        own->clip = id.value; own->source = -1;
        if (!id || !fits()) return noRoom;
        next.prune();
        d = std::move(next);
        return {};
    }
    const auto used = [&](int scene) {
        if (next.cellClip(row, scene)) return true;
        for (const auto& x : next.timeline.regions) if (x.row == row && x.source == scene) return true;
        for (const auto& x : next.timeline.takes) if (x.row == row && x.source == scene) return true;
        for (const auto& x : next.timeline.frozenOriginals) if (x.row == row && x.source == scene) return true;
        return false;
    };
    int free = -1;
    for (int s = 0; s < next.sceneCount() && free < 0; ++s) if (!used(s)) free = s;
    if (free < 0) {
        if (!next.canAddScene() || scene_edit::apply(next, {scene_edit::Kind::create}).isNotEmpty()) return "The Session has no room for another scene.";
        free = next.sceneCount() - 1;
    }
    next.cells[static_cast<std::size_t>(free)][static_cast<std::size_t>(row)] = next.library.duplicateClip(next.cellClip(row, from));
    next.timeline.find(regionId)->source = free;
    if (!fits()) return noRoom;
    d = std::move(next);
    return {};
}

// The Session cell an audio placement plays, which Undo shows after it got a cell of its own; {-1, -1} otherwise.
inline std::pair<int, int> audioCell(const ProjectDocument& d, std::uint32_t regionId) {
    const auto* r = d.timeline.find(regionId);
    if (r == nullptr || !r->playsCell() || !d.isValidCell(r->row, r->source) || !d.placementContent(*r).isAudioClip()) return {-1, -1};
    return {r->row, r->source};
}
} // namespace alien::song_placement
