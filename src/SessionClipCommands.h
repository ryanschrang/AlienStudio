#pragma once
#include "ProjectDocument.h"

namespace alien::session_clip {
struct Move { int track{-1}, scene{-1}, toTrack{-1}, toScene{-1}; };
inline juce::String moveRefusal(const ProjectDocument& d, const Move& e) {
    if (!d.isValidCell(e.track, e.scene) || !d.isValidCell(e.toTrack, e.toScene)) return "The source or destination Session cell does not exist.";
    if (d.tracks[static_cast<std::size_t>(e.track)].settings.frozen()
        || d.tracks[static_cast<std::size_t>(e.toTrack)].settings.frozen()) return "Unfreeze both tracks before moving a Session clip.";
    if (!d.cellClip(e.track, e.scene)) return "Choose an existing Session clip to move.";
    if (e.track == e.toTrack && e.scene == e.toScene) return {};
    if (d.cellClip(e.toTrack, e.toScene)) return "Choose an empty destination cell; moving never overwrites clips.";
    for (const auto* list : {&d.timeline.regions, &d.timeline.takes, &d.timeline.frozenOriginals})
        for (const auto& r : *list) if (r.playsCell()
            && ((r.row == e.track && r.source == e.scene) || (r.row == e.toTrack && r.source == e.toScene)))
            return "Make the Song cell placements independent before moving this Session clip.";
    return {};
}
inline bool move(ProjectDocument& d, const Move& e) {
    if (moveRefusal(d, e).isNotEmpty()) return false;
    if (e.track == e.toTrack && e.scene == e.toScene) return true;
    d.cells[static_cast<std::size_t>(e.toScene)][static_cast<std::size_t>(e.toTrack)] = d.cellClip(e.track, e.scene);
    d.cells[static_cast<std::size_t>(e.scene)][static_cast<std::size_t>(e.track)] = {};
    d.selectedTrack = e.toTrack; d.selectedScene = e.toScene;
    return true; // no allocation, pruning, media changes or new identities
}
// Removing a Session cell never edits the content shared by other cells or
// deletes the rack pattern. An already absent cell is a successful no-op.
inline juce::String deleteRefusal(const ProjectDocument& d, int track, int scene) {
    if (!d.isValidCell(track, scene)) return "The Session cell does not exist.";
    if (d.tracks[static_cast<std::size_t>(track)].settings.frozen())
        return "Unfreeze the track before deleting its Session clips.";
    return {};
}
inline bool remove(ProjectDocument& d, int track, int scene) {
    if (deleteRefusal(d, track, scene).isNotEmpty()) return false;
    d.clearClip(track, scene);
    return true;
}
} // namespace alien::session_clip
