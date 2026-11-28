#pragma once
#include "ProjectDocument.h"

namespace alien::track_structure {
inline constexpr int colourCount = 8;
enum class Kind { create, remove, move, rename, colour };
struct Edit { Kind kind{}; int track{-1}, to{-1}; juce::String name; int colour{}; };
inline juce::String name(const juce::String& text) { return text.trim().substring(0,64); }
inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.kind == Kind::create) {
        if (!d.trackRegistry.canAllocate()) return "The project cannot allocate another track identity.";
        return d.canAddTrack() ? juce::String{} : "The project already has the most tracks allowed (" + juce::String(maxSessionTracks) + ").";
    }
    const auto track = [&](int slot) -> juce::String {
        return slot >= 0 && slot < d.trackCount() ? juce::String{}
            : "There is no track " + juce::String(slot + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    };
    if (const auto why = track(e.track); why.isNotEmpty()) return why;
    if (e.kind == Kind::remove && d.trackCount() <= 1) return "The last track cannot be deleted.";
    if (e.kind == Kind::move) return track(e.to);
    if (e.kind == Kind::rename && name(e.name).isEmpty()) return "A track name cannot be blank.";
    if (e.kind == Kind::colour && (e.colour < 0 || e.colour >= colourCount)) return "Track colours are numbered 0 to " + juce::String(colourCount-1) + ".";
    return {};
}
// Shared document semantics for Preview and live structural adapters.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d,e); why.isNotEmpty()) return why;
    switch (e.kind) {
    case Kind::create:
        d.tracks.push_back({d.nextTrackName(), {}, d.nextTrackColour()});
        for (auto& scene : d.cells) scene.emplace_back();
        break;
    case Kind::remove:
        d.timeline.trackDeleted(e.track); d.channelRack.trackDeleted(e.track); d.sidechainTrackDeleted(e.track);
        d.tracks.erase(d.tracks.begin()+e.track);
        for (auto& scene : d.cells) scene.erase(scene.begin()+e.track);
        d.prune();
        if (d.selectedTrack > e.track) --d.selectedTrack;
        d.clampSelection();
        break;
    case Kind::move: {
        const int delta = e.to > e.track ? 1 : -1;
        for (int a=e.track; a!=e.to; a+=delta) {
            const int b=a+delta;
            d.timeline.tracksSwapped(a,b); d.channelRack.tracksSwapped(a,b); d.sidechainTracksSwapped(a,b);
            std::swap(d.tracks[static_cast<std::size_t>(a)],d.tracks[static_cast<std::size_t>(b)]);
            for (auto& scene : d.cells) std::swap(scene[static_cast<std::size_t>(a)],scene[static_cast<std::size_t>(b)]);
            if (d.selectedTrack == a) d.selectedTrack=b; else if (d.selectedTrack == b) d.selectedTrack=a;
        }
        break;
    }
    case Kind::rename: d.tracks[static_cast<std::size_t>(e.track)].name=name(e.name); break;
    case Kind::colour: d.tracks[static_cast<std::size_t>(e.track)].colour=e.colour; break;
    }
    return d.synchronizeTrackRegistry() ? juce::String{} : "The project cannot resolve its track identities.";
}
} // namespace alien::track_structure
