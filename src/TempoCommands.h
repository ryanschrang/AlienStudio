#pragma once
#include "ProjectDocument.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure document edits for the project tempo, the Song's tempo changes and its time
// signature changes. The controller and command Preview share them, so what Preview
// predicts is what the live edit does, including frozen tracks being unfrozen first.
namespace alien::tempo_edit {
inline constexpr int minProjectBpm = 60, maxProjectBpm = 200;

enum class Kind { projectTempo, setChange, moveChange, removeChange, clearChanges, setMeter, removeMeter, clearMeters };
struct Edit {
    Kind kind{Kind::projectTempo};
    int bpm{};                 // projectTempo
    std::int64_t tick{}, to{}; // tempo changes; `to` is where a move ends up
    double changeBpm{};        // setChange
    int bar{}, numerator{}, denominator{};   // time signature changes
};

// Frozen audio was rendered at the old tempo, so a tempo edit unfreezes it. A time signature does not.
inline bool changesTempo(const Edit& e) noexcept { return e.kind != Kind::setMeter && e.kind != Kind::removeMeter && e.kind != Kind::clearMeters; }
inline bool anyFrozen(const ProjectDocument& d) noexcept {
    return std::any_of(d.tracks.begin(), d.tracks.end(), [](const auto& track) { return track.settings.frozen(); });
}
inline void unfreezeAll(ProjectDocument& d) {
    for (int t = 0; t < d.trackCount(); ++t) {
        auto& settings = d.tracks[static_cast<std::size_t>(t)].settings;
        if (!settings.frozen()) continue;
        const auto scene = settings.freezeScene;
        d.timeline.unfreezeTrack(t);
        settings.freezeScene = -1;
        if (d.isValidCell(t, scene)) d.clearClip(t, scene);
    }
    d.timeline.lengthBars = std::max(d.timeline.freezeBaseLengthBars, d.timeline.minLengthBars());
    d.timeline.freezeBaseLengthBars = 0;
}

inline bool hasTempoChange(const ProjectDocument& d, std::int64_t tick) {
    const auto& changes = d.timeline.tempoChanges;
    return std::any_of(changes.begin(), changes.end(), [tick](const auto& c) { return c.tick == tick; });
}

// True when the edit would leave the document as it is. Only meaningful for an edit with valid arguments.
inline bool changesNothing(const ProjectDocument& d, const Edit& e) {
    const auto& song = d.timeline;
    switch (e.kind) {
    case Kind::projectTempo: return d.bpm == e.bpm;
    case Kind::setChange: return std::any_of(song.tempoChanges.begin(), song.tempoChanges.end(), [&](const auto& c) { return c.tick == e.tick && c.bpm == e.changeBpm; });
    case Kind::moveChange: return e.tick == e.to;
    case Kind::removeChange: return !hasTempoChange(d, e.tick);
    case Kind::clearChanges: return song.tempoChanges.empty();
    case Kind::setMeter: return song.meterMap().meterAtBar(e.bar) == timeline::Meter{e.numerator, e.denominator};
    case Kind::removeMeter: return std::none_of(song.meterChanges.begin(), song.meterChanges.end(), [&](const auto& c) { return c.bar == e.bar; });
    case Kind::clearMeters: return song.meterChanges.empty();
    }
    return true;
}

inline bool editSong(timeline::Timeline& song, const Edit& e) {
    switch (e.kind) {
    case Kind::setChange: return song.setTempoChange(e.tick, e.changeBpm);
    case Kind::moveChange: return song.moveTempoChange(e.tick, e.to);
    case Kind::removeChange: return song.removeTempoChange(e.tick);
    case Kind::clearChanges: return song.clearTempoChanges();
    case Kind::setMeter: return song.setMeterChange(e.bar, e.numerator, e.denominator);
    case Kind::removeMeter: return song.removeMeterChange(e.bar);
    case Kind::clearMeters: return song.clearMeterChanges();
    case Kind::projectTempo: break;
    }
    return false;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    switch (e.kind) {
    case Kind::projectTempo:
        return e.bpm >= minProjectBpm && e.bpm <= maxProjectBpm ? juce::String{} : "The project tempo must be a whole number from 60 to 200 BPM.";
    case Kind::setChange:
        if (!timeline::validTempo(e.changeBpm)) return "A tempo change must be from 60 to 200 BPM.";
        if (e.tick <= 0) return "A tempo change must come after the start of the song; the project tempo sets the start.";
        break;
    case Kind::moveChange:
        if (!hasTempoChange(d, e.tick)) return "There is no tempo change at tick " + juce::String(e.tick) + ".";
        if (e.to <= 0) return "A tempo change must come after the start of the song; the project tempo sets the start.";
        break;
    case Kind::setMeter:
        if (!timeline::validMeter(e.numerator, e.denominator)) return "A time signature needs 1 to " + juce::String(timeline::maxMeterNumerator) + " beats of a 2, 4, 8 or 16.";
        if (e.bar < 0 || e.bar >= d.timeline.lengthBars) return "A time signature change must be inside the song.";
        break;
    case Kind::removeChange: case Kind::clearChanges: case Kind::removeMeter: case Kind::clearMeters: break;
    }
    if (changesNothing(d, e)) return {};
    // Unfreezing can shorten the song, so a tempo edit is judged against the unfrozen song.
    auto song = d.timeline;
    if (changesTempo(e) && anyFrozen(d)) { auto unfrozen = d; unfreezeAll(unfrozen); song = unfrozen.timeline; }
    song.tempo = d.bpm;
    if (editSong(song, e) && song.valid(d.trackCount(), d.sceneCount(), d.channelRack.patternCount)) return {};
    return changesTempo(e) ? "That tempo change must be inside the song, within the most changes a song can hold."
                           : "That time signature change does not fit in the song.";
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (changesNothing(d, e)) return {};
    if (changesTempo(e) && anyFrozen(d)) unfreezeAll(d);
    if (e.kind == Kind::projectTempo) { d.bpm = e.bpm; return {}; }
    const auto bars = d.timeline.lengthBars;
    d.timeline.tempo = d.bpm;
    if (!editSong(d.timeline, e)) return "The song could not take that edit.";
    // A frozen track gets its length back when it unfreezes; a time signature can lengthen the song meanwhile.
    if (anyFrozen(d) && d.timeline.lengthBars != bars) d.timeline.freezeBaseLengthBars = d.timeline.lengthBars;
    return {};
}
} // namespace alien::tempo_edit
