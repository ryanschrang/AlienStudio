#pragma once
#include "ProjectDocument.h"
#include <algorithm>

// Pure document edit for unfreezing one Song track (the UnfreezeTrack command). The controller and
// command Preview share it, so what Preview predicts is what the live edit does.
namespace alien::freeze_edit {
inline juce::String refusal(const ProjectDocument& d, int track) {
    if (track < 0 || track >= d.trackCount())
        return "There is no track " + juce::String(track + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    return d.tracks[static_cast<std::size_t>(track)].settings.frozen() ? juce::String{}
        : "Track " + juce::String(track + 1) + " is not frozen.";
}

// Puts the track's original Song clips back in place of its frozen audio, forgets the frozen
// render's Session cell and, once no track is frozen, gives the song back the length it had.
inline juce::String unfreeze(ProjectDocument& d, int track) {
    if (const auto why = refusal(d, track); why.isNotEmpty()) return why;
    if (!d.timeline.unfreezeTrack(track)) return "The track's original Song clips could not be restored.";
    auto& settings = d.tracks[static_cast<std::size_t>(track)].settings;
    const int scene = settings.freezeScene;
    settings.freezeScene = -1;
    if (d.isValidCell(track, scene)) d.clearClip(track, scene);
    if (std::none_of(d.tracks.begin(), d.tracks.end(), [](const auto& t) { return t.settings.frozen(); })) {
        d.timeline.lengthBars = std::max(d.timeline.freezeBaseLengthBars, d.timeline.minLengthBars());
        d.timeline.freezeBaseLengthBars = 0;
    }
    return {};
}
} // namespace alien::freeze_edit
