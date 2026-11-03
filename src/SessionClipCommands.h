#pragma once
#include "ProjectDocument.h"
#include "ClipboardClipCommands.h"

namespace alien::session_clip {
inline juce::String removeAudioRefusal(const ProjectDocument& d, int track, int scene) {
    if (!d.isValidCell(track, scene)) return "The Session cell does not exist.";
    if (d.tracks[static_cast<std::size_t>(track)].settings.frozen()) return "Unfreeze the track before removing clip audio.";
    const auto clip = d.clip(track, scene);
    if (!clip.isAudioClip()) return {};
    // setClip clears only this cell when the remaining notes/loop are default
    // empty; otherwise it edits the shared pattern, including linked cells.
    const auto& blank = *emptyPattern();
    const bool clears = !clip.pattern || (clip.pattern->count == 0 && clip.pattern->loopSteps == blank.loopSteps);
    if (!clears) for (int s = 0; s < d.sceneCount(); ++s) for (int t = 0; t < d.trackCount(); ++t)
        if (d.linkOf(t,s) == d.linkOf(track,scene) && d.tracks[static_cast<std::size_t>(t)].settings.frozen())
            return "Unfreeze linked tracks before changing their shared clip audio.";
    return {};
}
inline bool removeAudio(ProjectDocument& d, int track, int scene) {
    if (removeAudioRefusal(d, track, scene).isNotEmpty()) return false;
    auto clip = d.clip(track, scene);
    if (!clip.isAudioClip()) return true;
    clip.audio.reset(); clip.audioFileName.clear(); clip.audioFilePath.clear();
    d.setClip(track, scene, clip); return true;
}
struct Duplicate { int track{-1}, scene{-1}, toTrack{-1}, toScene{-1}; };
inline juce::String duplicateRefusal(const ProjectDocument& d, const Duplicate& e) {
    if (!d.isValidCell(e.track, e.scene) || !d.isValidCell(e.toTrack, e.toScene)) return "The source or destination Session cell does not exist.";
    if (!d.cellClip(e.track, e.scene) || !d.library.clip(d.cellClip(e.track, e.scene))) return "Choose an existing Session clip to duplicate.";
    if (d.tracks[static_cast<std::size_t>(e.toTrack)].settings.frozen()) return "Unfreeze the destination track before duplicating clips.";
    if (d.cellClip(e.toTrack, e.toScene)) return "The destination cell already contains a clip.";
    if (d.clip(e.track, e.scene).rackClip) {
        if (d.channelRack.patternCount >= rack::maxPatterns) return "The Channel Rack has no room for another independent pattern.";
        for (const auto* list : {&d.timeline.regions, &d.timeline.takes, &d.timeline.frozenOriginals})
            for (const auto& r : *list) if (r.playsCell() && r.row == e.toTrack && r.source == e.toScene)
                return "Choose a rack destination without cell-based Song placements.";
    }
    return {};
}
inline juce::String duplicate(ProjectDocument& d, const Duplicate& e) {
    if (const auto why = duplicateRefusal(d, e); why.isNotEmpty()) return why;
    if (d.clip(e.track, e.scene).rackClip) {
        const clipboard_clip::Clipboard source{d.cellClip(e.track, e.scene), d.clip(e.track, e.scene)};
        if (const auto why = clipboard_clip::apply(d, {clipboard_clip::Kind::paste, e.toTrack, e.toScene}, &source); why.isNotEmpty()) return why;
    } else {
        const auto id = d.library.duplicateClip(d.cellClip(e.track, e.scene));
        if (!id) return "The independent Session clip could not be allocated.";
        d.cells[static_cast<std::size_t>(e.toScene)][static_cast<std::size_t>(e.toTrack)] = id;
    }
    d.selectedTrack = e.toTrack; d.selectedScene = e.toScene;
    return {};
}
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
