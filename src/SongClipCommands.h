#pragma once
#include "ProjectDocument.h"
#include <cmath>
#include <cstdint>
#include <optional>

// Pure document edits for the clips placed in the Song: moving, trimming, slipping, deleting, muting,
// gain, fades, crossfades, groups, audio source looping and reversing, and splitting, duplicating and
// copying them. The controller and command Preview share them, so what Preview predicts is what the live
// edit does. New clips take the next Song ids; a Preview hands out the same ones (timeline::id_stream).
namespace alien::song_clip {
enum class Kind { move, trimStart, trimEnd, slip, remove, mute, gain, fades, fadeIn, fadeOut, fadeShape, clearFades,
                  crossfade, group, ungroup, loopSource, reverse, split, duplicate, copy };
struct Edit {
    Kind kind{Kind::move};
    timeline::Ids clips;            // the clips, by region id; trims and fade handles use one, a crossfade left then right
    std::int64_t ticks{};           // a move, slip or copy distance, a trim's new edge or split tick, a fade or crossfade length (fades: fade in)
    std::int64_t fadeOut{};         // fades
    int lanes{};                    // move, copy: rack lanes
    double gain{1.0};               // gain
    bool on{};                      // mute, loopSource, reverse
    timeline::FadeEdge edge{};      // fadeShape
    timeline::FadeCurve curve{};    // fadeShape
    std::uint32_t group{};          // group: the new group's id
};

inline bool audioOnly(Kind k) noexcept {
    return k == Kind::gain || k == Kind::fades || k == Kind::fadeIn || k == Kind::fadeOut || k == Kind::fadeShape
        || k == Kind::clearFades || k == Kind::crossfade || k == Kind::loopSource || k == Kind::reverse;
}
inline bool oneClip(Kind k) noexcept { return k == Kind::trimStart || k == Kind::trimEnd || k == Kind::fadeIn || k == Kind::fadeOut; }
inline bool creates(Kind k) noexcept { return k == Kind::split || k == Kind::duplicate || k == Kind::copy; }

// Either why the edit is refused, or the Song it leaves (none when it changes nothing).
struct Outcome { juce::String why; std::optional<timeline::Timeline> next; };

inline Outcome outcome(const ProjectDocument& d, const Edit& e) {
    using namespace timeline;
    const auto& song = d.timeline;
    const auto refuse = [](juce::String why) { Outcome o; o.why = std::move(why); return o; };
    if (e.clips.empty()) return refuse("Choose at least one Song clip.");
    if (oneClip(e.kind) && e.clips.size() != 1) return refuse("Choose one Song clip.");
    if (e.kind == Kind::crossfade && e.clips.size() != 2) return refuse("A crossfade needs a left and a right clip.");
    bool anyAudio = false;
    for (const auto id : e.clips) {
        const auto* r = song.find(id);
        if (r == nullptr) return refuse("There is no clip " + juce::String(static_cast<juce::int64>(id)) + " in the song.");
        anyAudio = anyAudio || !r->isPattern();
        if (!r->isPattern() && r->row >= 0 && r->row < d.trackCount() && d.tracks[static_cast<std::size_t>(r->row)].settings.frozen())
            return refuse("Unfreeze track " + juce::String(r->row + 1) + " before editing its clips.");
    }
    if (audioOnly(e.kind) && !anyAudio) return refuse("Select audio clips for that.");
    switch (e.kind) {
    case Kind::gain:
        if (!std::isfinite(e.gain) || e.gain < 0.0 || e.gain > 64.0) return refuse("Clip gain must be from 0 to 64.");
        break;
    case Kind::fades: case Kind::fadeIn: case Kind::fadeOut: case Kind::crossfade:
        if (e.ticks < 0 || e.fadeOut < 0) return refuse("A fade cannot be negative.");
        break;
    case Kind::fadeShape:
        if (!validFadeCurve(e.curve) || static_cast<int>(e.edge) < 0 || static_cast<int>(e.edge) > static_cast<int>(FadeEdge::crossfade))
            return refuse("That is not a fade shape.");
        break;
    case Kind::group:
        if (e.group == 0 || std::any_of(song.regions.begin(), song.regions.end(), [&](const Region& r) { return r.group == e.group; }))
            return refuse("That group is already in the song.");
        if (song.expandGroups(e.clips).size() < 2) return refuse("Select two or more clips to group them.");
        break;
    default: break;
    }
    if (e.kind == Kind::crossfade) {
        const auto* left = song.find(e.clips[0]); const auto* right = song.find(e.clips[1]);
        if (left->isPattern() || right->isPattern() || left->row != right->row || left->end() != right->start)
            return refuse("A crossfade needs two touching audio clips on the same track.");
    }
    if (e.kind == Kind::split && std::none_of(e.clips.begin(), e.clips.end(), [&](std::uint32_t id) { const auto* r = song.find(id); return e.ticks > r->start && e.ticks < r->end(); }))
        return refuse("Put the play position inside a clip to split it.");
    std::int64_t first = maxLength;
    for (const auto id : e.clips) first = std::min(first, song.find(id)->start);
    if (e.kind == Kind::copy && first + e.ticks < 0) return refuse("That does not fit there.");

    Timeline next = song;
    next.tempo = d.bpm;
    bool done = false;
    const auto& ids = e.clips;
    switch (e.kind) {
    case Kind::move: done = next.move(ids, e.ticks, e.lanes); break;
    case Kind::trimStart: done = next.trimStart(ids[0], e.ticks); break;
    case Kind::trimEnd: done = next.trimEnd(ids[0], e.ticks); break;
    case Kind::slip: done = next.slip(ids, e.ticks); break;
    case Kind::remove: done = next.remove(ids); break;
    case Kind::mute: done = next.setMuted(ids, e.on); break;
    case Kind::gain: done = next.setGain(ids, e.gain); break;
    case Kind::fades: done = next.setFades(ids, e.ticks, e.fadeOut); break;
    case Kind::fadeIn: done = next.setFadeIn(ids[0], e.ticks); break;
    case Kind::fadeOut: done = next.setFadeOut(ids[0], e.ticks); break;
    case Kind::fadeShape: done = next.setFadeCurve(ids, e.edge, e.curve); break;
    case Kind::clearFades: done = next.clearFades(ids); break;
    case Kind::crossfade: done = next.crossfade(ids[0], ids[1], e.ticks); break;
    case Kind::group: done = next.groupAs(ids, e.group); break;
    case Kind::ungroup: done = next.ungroup(ids); break;
    case Kind::loopSource: done = next.setLoopSource(ids, e.on); break;
    case Kind::reverse: done = next.setReversed(ids, e.on); break;
    case Kind::split: done = next.split(ids, e.ticks); break;
    case Kind::duplicate: done = next.duplicate(ids); break;
    case Kind::copy: done = next.paste(song.copy(ids), first + e.ticks, e.lanes); break;
    }
    if (!done && creates(e.kind)) return refuse("That does not fit there.");
    if (!done) return {};
    if (!next.valid(d.trackCount(), d.sceneCount(), d.channelRack.patternCount)) return refuse("That does not fit there.");
    if (next == song) return {};
    // A frozen track plays its render, so none of its clips may change (a move can land on one).
    bool frozen = false;
    for (int track = 0; track < d.trackCount(); ++track) {
        if (!d.tracks[static_cast<std::size_t>(track)].settings.frozen()) continue;
        frozen = true;
        std::vector<Region> before, after;
        for (const auto& r : song.regions) if (r.row == track) before.push_back(r);
        for (const auto& r : next.regions) if (r.row == track) after.push_back(r);
        if (before != after) return refuse("Unfreeze track " + juce::String(track + 1) + " before editing its clips.");
    }
    if (frozen && next.lengthBars != song.lengthBars) next.freezeBaseLengthBars = next.lengthBars;
    Outcome o;
    o.next = std::move(next);
    return o;
}

// These two only ask, so the ids a new clip would take are handed back (the live edit takes them).
inline juce::String refusal(const ProjectDocument& d, const Edit& e) { timeline::id_stream::Scope asking; return outcome(d, e).why; }
// True when the edit would leave the document as it is. Only meaningful for an edit that is not refused.
inline bool changesNothing(const ProjectDocument& d, const Edit& e) { timeline::id_stream::Scope asking; return !outcome(d, e).next.has_value(); }

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    auto o = outcome(d, e);
    if (o.why.isNotEmpty()) return o.why;
    if (!o.next) return {};
    d.timeline = std::move(*o.next);
    d.prune();   // a placement clip no region plays any more is gone (Undo keeps it)
    return {};
}
} // namespace alien::song_clip
