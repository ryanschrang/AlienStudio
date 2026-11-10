#pragma once
#include "ProjectDocument.h"
#include <cmath>
#include <cstdint>
#include <optional>

// Pure document edits for the clips placed in the Song: placing a Session cell or rack pattern, moving, trimming, slipping, deleting, muting,
// gain, fades, crossfades, groups, audio source looping and reversing, warp markers, and splitting, duplicating and
// copying them, and comping or forgetting recorded takes. The controller and command Preview share them, so
// what Preview predicts is what the live edit does. New clips take the next Song ids; a Preview hands out the
// same ones (timeline::id_stream).
namespace alien::song_clip {
enum class Kind { move, trimStart, trimEnd, slip, remove, mute, gain, fades, fadeIn, fadeOut, fadeShape, clearFades,
                  crossfade, group, ungroup, loopSource, reverse, split, duplicate, copy, comp, forget,
                  setWarp, addWarp, moveWarp, removeWarp, place };
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
    std::uint32_t take{};           // comp, forget: the recorded take's id (`clips` is empty)
    std::int64_t end{};             // comp: plays the take from `ticks` to `end`
    std::vector<timeline::WarpMarker> warp;   // setWarp: the clip's whole marker list (empty: no warp)
    int index{};                    // moveWarp, removeWarp: which marker; addWarp and moveWarp use `ticks` as its source tick
    int row{}, source{};            // place: a track and scene, or a rack lane row and pattern; `ticks` is the start
    std::int64_t length{};          // place: in ticks; 0 is the clip's natural length
};

inline bool audioOnly(Kind k) noexcept {
    return k == Kind::gain || k == Kind::fades || k == Kind::fadeIn || k == Kind::fadeOut || k == Kind::fadeShape
        || k == Kind::clearFades || k == Kind::crossfade || k == Kind::loopSource || k == Kind::reverse;
}
inline bool warpEdit(Kind k) noexcept { return k == Kind::setWarp || k == Kind::addWarp || k == Kind::moveWarp || k == Kind::removeWarp; }
inline bool oneClip(Kind k) noexcept { return k == Kind::trimStart || k == Kind::trimEnd || k == Kind::fadeIn || k == Kind::fadeOut || warpEdit(k); }
inline bool creates(Kind k) noexcept { return k == Kind::split || k == Kind::duplicate || k == Kind::copy || k == Kind::comp || k == Kind::place; }
inline bool takeEdit(Kind k) noexcept { return k == Kind::comp || k == Kind::forget; }

// What a placement of `source` on `row` really places: a Session cell that holds a rack clip places that clip's rack
// pattern on the first rack lane, as the Session menu's "Place in Song" always has.
struct Placement { int row{}, source{}; };
inline Placement placement(const ProjectDocument& d, int row, int source) {
    if (!timeline::isPatternRow(row) && d.isValidCell(row, source) && d.clip(row, source).rackClip != 0)
        return {timeline::rackRowBase, d.rackPatternForClip(d.clip(row, source).rackClip)};
    return {row, source};
}
inline bool placesPattern(const ProjectDocument& d, int row, int source) { return timeline::isPatternRow(placement(d, row, source).row); }

// How long a clip or pattern naturally lasts in the Song, in ticks: its loop for MIDI and rack patterns, its running
// time at the Song's tempo from `start` for audio (rounded up to a sixteenth so it ends on the grid).
inline std::int64_t naturalLength(const ProjectDocument& d, int row, int source, std::int64_t start = 0) {
    using namespace timeline;
    if (isPatternRow(row)) {
        if (!d.channelRack.validPattern(source)) return ticksPerBar;
        return static_cast<std::int64_t>(d.channelRack.pattern(source).length) * ticksPerStep;
    }
    if (!d.isValidCell(row, source)) return ticksPerBar;
    const auto& clip = d.clip(row, source);
    if (clip.audio != nullptr && clip.audio->sampleRate > 0.0) {
        const TempoMap tempo{d.timeline.tempoChanges, static_cast<double>(d.bpm)};
        const double seconds = static_cast<double>(clip.audio->frames) / clip.audio->sampleRate;
        const double ticks = tempo.tickAfter(static_cast<double>(start), seconds) - static_cast<double>(start);
        return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(ticks / ticksPerStep)) * ticksPerStep, ticksPerStep, maxLength);
    }
    return static_cast<std::int64_t>(clip.notes().loopSteps) * ticksPerStep;
}

// The id of the clip a placement of `source` on `row` put in the Song (`before` became `after`), or 0. Placing clears
// its range on its row, so the placed clip is the only one there that starts at `start` and was not there before.
inline std::uint32_t placedClip(const ProjectDocument& d, const timeline::Timeline& before, const timeline::Timeline& after,
                                int row, int source, std::int64_t start) {
    const auto where = placement(d, row, source);
    for (const auto& r : after.regions)
        if (r.row == where.row && r.start == start && before.find(r.id) == nullptr) return r.id;
    return 0;
}

// Either why the edit is refused, or the Song it leaves (none when it changes nothing).
struct Outcome { juce::String why; std::optional<timeline::Timeline> next; };

inline Outcome outcome(const ProjectDocument& d, const Edit& e) {
    using namespace timeline;
    const auto& song = d.timeline;
    const auto refuse = [](juce::String why) { Outcome o; o.why = std::move(why); return o; };
    if (takeEdit(e.kind)) {
        // A take is a kept recording pass, not a clip in the Song. Forgetting one leaves the comp and its audio file;
        // a comp plays part of it on its track over what is there, and the take stays.
        const auto found = std::find_if(song.takes.begin(), song.takes.end(), [&](const Region& r) { return r.id == e.take; });
        if (found == song.takes.end()) return refuse("There is no take " + juce::String(static_cast<juce::int64>(e.take)) + " in the song.");
        Timeline next = song;
        next.tempo = d.bpm;
        if (e.kind == Kind::forget) {
            std::erase_if(next.takes, [&](const Region& r) { return r.id == e.take; });
        } else {
            const auto& take = *found;
            if (!d.isValidCell(take.row, take.source) || !d.clip(take.row, take.source).isAudioClip()
                || (take.ownsClip() && d.library.clip(library::ClipId{take.clip}) == nullptr))
                return refuse("That take's audio is no longer in the project.");
            if (take.reversed || take.loopSource) return refuse("A reversed or looped take cannot be comped.");
            if (e.ticks < take.start || e.end > take.end() || e.end <= e.ticks) return refuse("Choose a range inside the take.");
            if (take.row >= 0 && take.row < d.trackCount() && d.tracks[static_cast<std::size_t>(take.row)].settings.frozen())
                return refuse("Unfreeze track " + juce::String(take.row + 1) + " before comping its takes.");
            if (!next.compTake(e.take, e.ticks, e.end) || !next.valid(d.trackCount(), d.sceneCount(), d.channelRack.patternCount))
                return refuse("That take cannot be comped there.");
        }
        Outcome o;
        o.next = std::move(next);
        return o;
    }
    // What every edit that made a new Song checks last: that the Song is still valid and leaves frozen tracks alone.
    const auto settle = [&](Timeline next, bool done) -> Outcome {
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
    };
    if (e.kind == Kind::place) {
        // A Session cell (empty or not) on its track, or a rack pattern on a rack lane. It overwrites what it lands on.
        const auto where = placement(d, e.row, e.source);
        if (isPatternRow(where.row)) {
            if (!d.channelRack.validPattern(where.source)) return refuse("There is no rack pattern " + juce::String(where.source + 1) + ".");
        } else {
            if (e.row < 0 || e.row >= d.trackCount()) return refuse("There is no track " + juce::String(e.row + 1) + ".");
            if (e.source < 0 || e.source >= d.sceneCount()) return refuse("There is no scene " + juce::String(e.source + 1) + ".");
        }
        if (e.ticks < 0 || e.ticks >= maxLength) return refuse("A Song clip must start inside the song.");
        if (e.length < 0 || e.length > maxLength) return refuse("A Song clip's length must be from 1 to " + juce::String(maxLength) + " ticks (0 for its own length).");
        Region r;
        r.row = where.row; r.source = where.source; r.start = e.ticks;
        r.length = e.length > 0 ? e.length : naturalLength(d, where.row, where.source, e.ticks);
        if (isPatternRow(where.row)) r.rackClip = d.channelRack.clips[static_cast<std::size_t>(where.source)];
        Timeline next = song;
        next.tempo = d.bpm;
        const bool done = next.place(r);
        return settle(std::move(next), done);
    }
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
    if (warpEdit(e.kind)) {
        // Warp markers pin a clip's source seconds to ticks of its content (Region::offset counts in them).
        const auto& r = *song.find(e.clips[0]);
        if (r.isPattern()) return refuse("Warp markers are for audio clips.");
        const auto count = static_cast<int>(r.warp.size());
        if (e.kind == Kind::setWarp) {
            if (!e.warp.empty() && (!d.isValidCell(r.row, r.source) || !d.clip(r.row, r.source).isAudioClip())) return refuse("Warp markers are for audio clips.");
            if (!validWarp(e.warp)) return refuse("Warp needs 2 to 256 markers, each later in the clip and in the source than the one before.");
        } else if (r.warp.empty()) {
            return refuse("Turn on warp for that clip first.");
        } else if (e.kind == Kind::addWarp) {
            if (r.warp.size() >= maxWarpMarkers) return refuse("A clip has at most 256 warp markers.");
            if (e.ticks <= r.warp.front().tick || e.ticks >= r.warp.back().tick) return refuse("Put the warp marker between the clip's first and last markers.");
        } else {
            if (e.index < 0 || e.index >= count) return refuse("There is no warp marker " + juce::String(e.index) + " on that clip.");
            if (e.index == 0 || e.index == count - 1) return refuse("The first and last warp markers stay where they are.");
            const auto i = static_cast<std::size_t>(e.index);
            if (e.kind == Kind::moveWarp && (e.ticks <= r.warp[i - 1].tick || e.ticks >= r.warp[i + 1].tick))
                return refuse("A warp marker stays between the markers on either side of it.");
        }
    }
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
    case Kind::comp: case Kind::forget: break;   // handled above
    case Kind::setWarp: done = next.setWarp(ids[0], e.warp); break;
    case Kind::addWarp: done = next.addWarpMarker(ids[0], e.ticks); break;
    case Kind::moveWarp: done = next.moveWarpMarker(ids[0], static_cast<std::size_t>(e.index), e.ticks); break;
    case Kind::removeWarp: done = next.removeWarpMarker(ids[0], static_cast<std::size_t>(e.index)); break;
    case Kind::place: break;   // handled above
    }
    return settle(std::move(next), done);
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
