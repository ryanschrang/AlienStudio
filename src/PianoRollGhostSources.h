#pragma once
#include "PianoRollGhostNotes.h"
#include "PianoRollMultiClip.h"
#include "ProjectDocument.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace alien {

// The clips whose notes the piano roll shows as ghosts while it edits (track, scene).
// Reads the document only: each entry shares the clip's immutable pattern. Skips the
// edited cell, empty cells, audio clips and cells linked to the edited clip (their
// notes are the edited notes).
inline std::vector<PianoRollGhostClip> ghostClipsFor(const ProjectDocument& doc, int track, int scene,
                                                     GhostNoteSource source) {
    std::vector<PianoRollGhostClip> clips;
    if (source == GhostNoteSource::off || !doc.isValidCell(track, scene) || doc.clip(track, scene).rackClip != 0) return clips;
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

// The clips multi-clip editing merges for (track, scene): the opened clip first, then the clips ghostClipsFor
// would show that have its loop length, that `editable` allows (a frozen track's clip, or one being recorded, is
// not), and whose content no earlier clip shares (linked cells are one content, edited once).
inline std::vector<MultiClipSource> multiClipSourcesFor(const ProjectDocument& doc, int track, int scene, GhostNoteSource source,
                                                        const std::function<bool(int, int)>& editable) {
    std::vector<MultiClipSource> sources;
    if (!doc.isValidCell(track, scene)) return sources;
    const auto opened = doc.clip(track, scene);
    if (opened.isAudioClip() || opened.rackClip != 0) return sources;
    sources.push_back({track, scene, opened.pattern});
    std::vector<std::uint64_t> links{doc.linkOf(track, scene)};
    for (const auto& ghost : ghostClipsFor(doc, track, scene, source)) {
        if (ghost.pattern == nullptr || ghost.pattern->loopSteps != opened.notes().loopSteps) continue;
        if (editable && !editable(ghost.track, ghost.scene)) continue;
        const auto link = doc.linkOf(ghost.track, ghost.scene);
        if (link != 0 && std::find(links.begin(), links.end(), link) != links.end()) continue;
        links.push_back(link);
        sources.push_back({ghost.track, ghost.scene, ghost.pattern});
    }
    return sources;
}

} // namespace alien
