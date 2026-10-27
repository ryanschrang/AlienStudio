#pragma once
#include "PianoRollSequence.h"
#include <cstddef>
#include <memory>
#include <span>

// Ghost notes (PROD-05): notes of other clips drawn dimmed behind the clip being
// edited. They are a view only: the editor keeps shared, immutable pointers to
// the other clips' patterns, never selects, hit-tests or edits them, and showing
// or hiding them records nothing. Message thread only; nothing here reaches audio.
namespace alien {

enum class GhostNoteSource {
    off,        // only the edited clip is shown
    sameScene,  // the other tracks' clips in the edited clip's scene (what plays alongside it)
    sameTrack,  // the edited track's clips in the other scenes
};

struct PianoRollGhostClip {
    std::shared_ptr<const PianoRollPattern> pattern;
    int track{}, scene{};
};

// One ghost note placed in the edited clip's loop.
struct PianoRollGhostNote {
    int pitch{};
    double startStep{}, lengthSteps{};
    float velocity{};
    int track{}, scene{};
};

// Calls visit(PianoRollGhostNote) for every ghost note inside a loop of loopSteps.
// A shorter clip repeats, as it does when launched beside the edited one; notes are
// cut at the end of the loop. At most `limit` notes are visited; returns how many.
template <typename Visit>
std::size_t forEachGhostNote(std::span<const PianoRollGhostClip> clips, int loopSteps, Visit&& visit,
                             std::size_t limit = 8192) {
    std::size_t visited = 0;
    if (loopSteps < 1) return 0;
    for (const auto& clip : clips) {
        if (clip.pattern == nullptr || !clip.pattern->valid()) continue;
        const auto& source = *clip.pattern;
        for (int offset = 0; offset < loopSteps; offset += source.loopSteps)
            for (std::size_t i = 0; i < source.count; ++i) {
                const auto& note = source.notes[i];
                const double start = note.startStep + offset;
                if (start >= loopSteps) continue;
                if (visited == limit) return visited;
                visit(PianoRollGhostNote{note.pitch, start, std::min(note.lengthSteps, loopSteps - start),
                                         note.velocity, clip.track, clip.scene});
                ++visited;
            }
    }
    return visited;
}

} // namespace alien
