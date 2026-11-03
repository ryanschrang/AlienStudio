#pragma once
#include "ProjectDocument.h"

namespace alien::rack_command {
enum class Kind { create, assign };
struct Edit { Kind kind{}; int track{-1}, scene{-1}; std::uint64_t clip{}; };
inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.kind == Kind::create) {
        if (d.channelRack.patternCount >= rack::maxPatterns) return "The Channel Rack has reached its pattern limit.";
        for (int c = 0; c < d.channelRack.channelCount; ++c) {
            const int output = d.channelRack.channels[static_cast<std::size_t>(c)].output;
            if (output >= 0 && output < d.trackCount() && d.tracks[static_cast<std::size_t>(output)].settings.frozen())
                return "Unfreeze tracks receiving rack channels before creating a pattern.";
        }
        return {};
    }
    if (!d.isValidCell(e.track, e.scene)) return "The Session destination does not exist.";
    if (d.tracks[static_cast<std::size_t>(e.track)].settings.frozen()) return "Unfreeze the destination track before assigning a rack clip.";
    if (!e.clip || d.rackPatternForClip(e.clip) < 0) return "The rack source no longer exists.";
    if (d.cellClip(e.track, e.scene).value == e.clip) return {};
    for (const auto* list : {&d.timeline.regions, &d.timeline.takes, &d.timeline.frozenOriginals})
        for (const auto& r : *list) if (r.playsCell() && r.row == e.track && r.source == e.scene)
            return "Choose a rack destination without cell-based Song placements.";
    return {};
}
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (e.kind == Kind::assign) {
        if (d.cellClip(e.track, e.scene).value == e.clip) return {};
        return d.bindRackClip(e.track, e.scene, e.clip) ? juce::String{} : "The rack source could not be assigned.";
    }
    const auto before = d; auto next = d.channelRack;
    if (next.addPattern() && d.setRack(std::move(next))) return {};
    d = before; return "The empty rack pattern could not be created.";
}
} // namespace alien::rack_command
