#pragma once
#include "ProjectDocument.h"

namespace alien::midi_clip {
enum class Kind { create, duplicate, makeUnique };
struct Edit {
    Kind kind{};
    int track{-1}, scene{-1}; // destination, or the cell made unique
    int fromTrack{-1}, fromScene{-1};
};
inline juce::String refusal(const ProjectDocument& d, const Edit& edit) {
    if (!d.isValidCell(edit.track,edit.scene)) return "The destination Session cell does not exist.";
    if (d.tracks[static_cast<std::size_t>(edit.track)].settings.frozen()) return "Unfreeze the destination track before editing its clips.";
    const auto midi=[&](int track,int scene) {
        return d.isValidCell(track,scene) && d.cellClip(track,scene) && d.library.clip(d.cellClip(track,scene))
            && !d.clip(track,scene).isAudioClip() && d.clip(track,scene).rackClip==0;
    };
    if (edit.kind==Kind::makeUnique)
        return midi(edit.track,edit.scene)?juce::String{}:"Choose a Session MIDI clip to make independent.";
    if (d.cellClip(edit.track,edit.scene)) return "The destination cell already contains a clip.";
    if (edit.kind==Kind::duplicate && !midi(edit.fromTrack,edit.fromScene)) return "Choose an existing Session MIDI clip to duplicate.";
    return {};
}
// Pure document operation shared by Preview and live execution. IDs are issued
// by the copied library, so Preview neither consumes live IDs nor edits history.
inline bool apply(ProjectDocument& d,const Edit& edit) {
    if (refusal(d,edit).isNotEmpty()) return false;
    const auto slot=static_cast<std::size_t>(edit.scene), track=static_cast<std::size_t>(edit.track);
    if (edit.kind==Kind::makeUnique) {
        const auto id=d.cellClip(edit.track,edit.scene);const auto* clip=d.library.clip(id);
        return d.library.usesOf(clip->pattern)<2 || d.library.makeClipUnique(id);
    }
    library::ClipId id;
    if (edit.kind==Kind::create) {
        auto data=std::make_shared<library::PatternData>();
        data->lengthTicks=library::PatternData::loopTicks(*emptyPattern());
        data->lanes.push_back({library::LaneId{1},library::NoteLane{emptyPattern()}});
        id=d.library.addClip(std::shared_ptr<const library::PatternData>(std::move(data)));
    } else id=d.library.duplicateClip(d.cellClip(edit.fromTrack,edit.fromScene));
    if (!id) return false;
    d.cells[slot][track]=id;d.selectedTrack=edit.track;d.selectedScene=edit.scene;
    return true;
}
} // namespace alien::midi_clip
