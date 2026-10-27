#pragma once
#include "PianoRollGhostNotes.h"
#include "ProjectDocument.h"
#include <vector>

namespace alien {

// The clips whose notes the piano roll shows as ghosts while it edits (track, scene).
// Reads the document only: each entry shares the clip's immutable pattern. Skips the
// edited cell, empty cells, audio clips and cells linked to the edited clip (their
// notes are the edited notes).
inline std::vector<PianoRollGhostClip> ghostClipsFor(const ProjectDocument& doc, int track, int scene,
                                                     GhostNoteSource source) {
    std::vector<PianoRollGhostClip> clips;
    if (source == GhostNoteSource::off || !doc.isValidCell(track, scene)) return clips;
    const auto link = doc.linkOf(track, scene);
    const auto consider = [&](int t, int s) {
        if (t == track && s == scene) return;
        if (link != 0 && doc.linkOf(t, s) == link) return;
        const auto clip = doc.clip(t, s);
        if (clip.isAudioClip() || clip.notes().count == 0) return;
        clips.push_back({clip.pattern, t, s});
    };
    if (source == GhostNoteSource::sameScene)
        for (int t = 0; t < doc.trackCount(); ++t) consider(t, scene);
    else
        for (int s = 0; s < doc.sceneCount(); ++s) consider(track, s);
    return clips;
}

} // namespace alien
