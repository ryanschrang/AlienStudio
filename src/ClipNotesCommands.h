#pragma once
#include "ProjectDocument.h"
#include <memory>

// Pure document edits for the notes of a Session MIDI clip and of a Song placement clip, as the piano roll writes them.
// The controller and command Preview share them, so what Preview predicts is what the live edit does. Each edit carries
// the clip's whole note sequence; a piano-roll gesture runs one per change under one coalescing key, so the gesture is
// one Undo step.
namespace alien::clip_notes {
enum class Kind { cell, placement };
struct Edit {
    Kind kind{Kind::cell};
    int track{-1}, scene{-1};                     // cell
    std::uint64_t placement{};                    // placement: the library clip that Song clips play
    std::shared_ptr<const PianoRollPattern> notes;
};

// The track a Song clip playing placement clip `clip` is on, or -1 when none plays it.
inline int placementTrack(const ProjectDocument& d, std::uint64_t clip) {
    for (const auto& r : d.timeline.regions) if (clip != 0 && r.clip == clip) return r.row;
    return -1;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.notes == nullptr || !e.notes->valid()) return "Those are not notes a clip can hold.";
    if (e.kind == Kind::placement) {
        if (d.library.clip(library::ClipId{e.placement}) == nullptr || placementTrack(d, e.placement) < 0)
            return "No Song clip plays placement clip " + juce::String(static_cast<juce::int64>(e.placement)) + ".";
        for (const auto& r : d.timeline.regions)
            if (r.clip == e.placement && r.row >= 0 && r.row < d.trackCount() && d.tracks[static_cast<std::size_t>(r.row)].settings.frozen())
                return "Unfreeze track " + juce::String(r.row + 1) + " before editing its clips.";
        return {};
    }
    if (!d.isValidCell(e.track, e.scene)) return "There is no Session cell at track " + juce::String(e.track + 1) + ", scene " + juce::String(e.scene + 1) + ".";
    if (d.tracks[static_cast<std::size_t>(e.track)].settings.frozen()) return "Unfreeze track " + juce::String(e.track + 1) + " before editing its clips.";
    if (d.clip(e.track, e.scene).rackClip != 0) return "That cell plays a rack pattern: edit its channel notes in the Channel Rack.";
    return {};
}

// True when the clip already holds these notes. Only meaningful for an edit that is not refused.
inline bool changesNothing(const ProjectDocument& d, const Edit& e) {
    const auto& now = e.kind == Kind::placement ? d.contentOf(library::ClipId{e.placement}).pattern : d.clip(e.track, e.scene).pattern;
    return now != nullptr && samePianoRollPattern(*now, *e.notes);
}

// Empty on success, otherwise why not. A cell's notes are its content: linked cells hear them too, and a cell left
// with no notes at the blank length is cleared, as the piano roll always has done. A placement clip's notes are a new
// revision that every Song clip playing it hears.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (changesNothing(d, e)) return {};
    if (e.kind == Kind::placement)
        return d.setPlacementNotes(library::ClipId{e.placement}, e.notes) ? juce::String{} : juce::String("Those notes could not be stored.");
    auto clip = d.clip(e.track, e.scene);
    clip.pattern = e.notes;
    d.setClip(e.track, e.scene, clip);
    return {};
}
} // namespace alien::clip_notes
