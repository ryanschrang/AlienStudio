#pragma once
#include "ProjectDocument.h"

namespace alien::session_clip {
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
