// Arrangement timeline data model. No JUCE: runs in a moment.
#include "Timeline.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>

using namespace alien::timeline;

static int failures = 0;
static void check(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message.c_str()); }
}

static constexpr std::int64_t bar = ticksPerBar;

static std::uint32_t put(Timeline& t, int row, int source, std::int64_t start, std::int64_t length) {
    Region r; r.row = row; r.source = source; r.start = start; r.length = length;
    std::uint32_t id = 0;
    const bool ok = t.place(r, &id);
    check(ok && id != 0, "placing a region at " + std::to_string(start));
    return id;
}
static const Region& get(const Timeline& t, std::uint32_t id) {
    static const Region none{};
    const auto* r = t.find(id);
    check(r != nullptr, "region " + std::to_string(id) + " exists");
    return r != nullptr ? *r : none;
}
static std::size_t countOnRow(const Timeline& t, int row) {
    std::size_t n = 0; for (const auto& r : t.regions) n += r.row == row ? 1 : 0; return n;
}

static void constantsAndDefaults() {
    check(ticksPerBar == 4 * ticksPerBeat && ticksPerBeat == 4 * ticksPerStep && ticksPerStep == 240, "960 ticks to a quarter note, 3840 to a bar");
    check(rackRowBase == alien::maxSessionTracks && rowCount == rackRowBase + rackLanes, "rack lanes follow the Session tracks");
    check(isTrackRow(0) && isTrackRow(rackRowBase - 1) && !isTrackRow(rackRowBase) && !isTrackRow(-1), "track rows");
    check(isPatternRow(rackRowBase) && isPatternRow(rowCount - 1) && !isPatternRow(rowCount) && !isPatternRow(rackRowBase - 1), "pattern rows");
    check(maxLength == 16384 * bar, "the timeline is at most 16384 bars (18 hours at 60 BPM)");
    Timeline t;
    check(t.valid() && t.regions.empty() && t.markers.empty() && t.lengthBars == 16 && t.length() == 16 * bar, "an empty timeline is 16 bars");
    check(!t.loopEnabled && t.loopStart == 0 && t.loopEnd == 4 * bar, "the loop starts as the first four bars, switched off");
    check(t.contentEnd() == 0 && t.minLengthBars() == 1 && !t.hasRegions(), "an empty timeline has no content");
    std::set<std::uint32_t> ids;
    for (int i = 0; i < 100; ++i) ids.insert(newId());
    check(ids.size() == 100 && ids.count(0) == 0, "ids are unique and never zero");
    check(std::abs(secondsForTicks(960, 120.0) - 0.5) < 1e-12 && std::abs(secondsForTicks(3840, 60.0) - 4.0) < 1e-12,
          "a quarter note at 120 BPM is half a second");
    check(std::abs(secondsForTicks(960, std::numeric_limits<double>::quiet_NaN()) - 0.5) < 1e-12 && std::abs(secondsForTicks(960, 0.0) - 0.5) < 1e-12,
          "a tempo that is not usable counts as 120");
}

static void placementTests() {
    Timeline t;
    const auto a = put(t, 2, 1, bar, 2 * bar);
    check(t.valid() && t.regions.size() == 1 && get(t, a).row == 2 && get(t, a).source == 1 && get(t, a).start == bar && get(t, a).end() == 3 * bar,
          "a region lands where it was placed");
    check(get(t, a).offset == 0 && get(t, a).audioOffset == 0.0 && !get(t, a).muted && get(t, a).group == 0, "a new region plays from the start of its source");
    check(t.regionAt(2, bar) == &get(t, a) && t.regionAt(2, 3 * bar - 1) == &get(t, a) && t.regionAt(2, 3 * bar) == nullptr
          && t.regionAt(2, bar - 1) == nullptr && t.regionAt(3, bar) == nullptr, "regionAt covers [start, end) on its own row only");

    // Placing beyond the song end lengthens the song to a whole bar.
    const auto far = put(t, 0, 0, 20 * bar + 17, 100);
    check(t.lengthBars == 21 && get(t, far).end() == 20 * bar + 117, "the song grows to hold a region, in whole bars");

    // Placing over the middle of a region cuts it in two; the right piece keeps its place in the source.
    Timeline cut;
    const auto base = put(cut, 1, 4, 0, 4 * bar);
    cut.tempo = 120.0;
    const auto top = put(cut, 1, 5, bar, bar);
    check(cut.valid() && countOnRow(cut, 1) == 3, "overwriting the middle leaves a piece either side");
    check(get(cut, base).start == 0 && get(cut, base).length == bar, "the left piece ends where the new region starts");
    const Region* right = cut.regionAt(1, 3 * bar);
    check(right != nullptr && right->id != base && right->id != top && right->source == 4 && right->start == 2 * bar && right->end() == 4 * bar,
          "the right piece starts where the new region ends");
    check(right != nullptr && right->offset == 2 * bar && std::abs(right->audioOffset - secondsForTicks(2 * bar, 120.0)) < 1e-9,
          "the right piece plays the part of the source that was left of it");
    check(get(cut, top).source == 5 && get(cut, top).start == bar && get(cut, top).offset == 0, "the new region is untouched");

    // Overhanging either edge trims; covering entirely removes.
    Timeline edge;
    const auto left = put(edge, 0, 0, 0, 2 * bar);
    const auto mid = put(edge, 0, 1, 3 * bar, 2 * bar);
    put(edge, 0, 2, bar, 3 * bar);   // covers the second half of `left` and the first half of `mid`
    check(edge.valid() && get(edge, left).length == bar, "a region overlapped at its end is shortened");
    check(get(edge, mid).start == 4 * bar && get(edge, mid).length == bar && get(edge, mid).offset == bar, "a region overlapped at its start is trimmed from the left");
    const auto small = put(edge, 0, 3, 4 * bar + 100, 100);
    check(edge.valid() && edge.find(small) != nullptr, "a small region fits inside another");
    put(edge, 0, 4, 0, 6 * bar);
    check(edge.valid() && countOnRow(edge, 0) == 1 && edge.find(left) == nullptr && edge.find(mid) == nullptr && edge.find(small) == nullptr,
          "a region covering others replaces them");

    // Invalid placements change nothing.
    Timeline t2;
    const auto keep = put(t2, 0, 0, 0, bar);
    const Timeline before = t2;
    Region bad; bad.row = 0; bad.source = 0;
    bad.length = 0;                           check(!t2.place(bad) && t2 == before, "zero length is refused");
    bad.length = bar; bad.start = -1;         check(!t2.place(bad) && t2 == before, "a negative start is refused");
    bad.start = 0; bad.row = -1;              check(!t2.place(bad) && t2 == before, "a negative row is refused");
    bad.row = rowCount;                       check(!t2.place(bad) && t2 == before, "a row past the last lane is refused");
    bad.row = alien::maxSessionTracks - 1; bad.source = alien::maxSessionScenes; check(!t2.place(bad) && t2 == before, "a scene past the last is refused");
    bad.source = -1;                          check(!t2.place(bad) && t2 == before, "a negative scene is refused");
    bad.row = rackRowBase; bad.source = alien::rack::maxPatterns; check(!t2.place(bad) && t2 == before, "a pattern past the last is refused");
    bad.row = 0; bad.source = 0; bad.start = maxLength - 10; bad.length = 20; check(!t2.place(bad) && t2 == before, "a region past the last bar is refused");
    check(t2.find(keep) != nullptr, "the earlier region survives every refusal");
    check(t2.valid(1, 1, 1) && !t2.valid(0, 1, 1), "validity depends on the real track count");

    // Placing the same bars twice leaves one region.
    Timeline twice;
    twice.placeBars(0, 0, 2, 2); twice.placeBars(0, 1, 2, 2);
    check(twice.regions.size() == 1 && twice.regions[0].source == 1 && twice.regions[0].start == 2 * bar && twice.regions[0].length == 2 * bar,
          "placing over an identical span replaces it");
    check(!twice.placeBars(0, 0, 0, 0) && !twice.placeBars(0, 0, -1, 1), "whole-bar placement refuses bad bars");

    // The region limit.
    Timeline many;
    bool all = true;
    for (std::size_t i = 0; i < maxRegions; ++i) all = all && many.placeBars(static_cast<int>(i % 16), 0, static_cast<int>(i / 16) * 2, 1);
    check(all && many.regions.size() == maxRegions && many.valid(), "1024 regions fit");
    check(!many.placeBars(0, 0, 500, 1) && many.regions.size() == maxRegions, "the 1025th region is refused");
}

static void validityTests() {
    Timeline t;
    Region a; a.id = 1; a.row = 0; a.source = 0; a.start = 0; a.length = bar;
    Region b = a; b.id = 2; b.start = bar;
    t.regions = {a, b};
    check(t.valid(), "touching regions are valid");
    t.regions[1].start = bar - 1;
    check(!t.valid(), "overlapping regions on a row are invalid");
    t.regions[1].start = bar; t.regions[1].row = 1;
    check(t.valid(), "the same span on different rows is valid");
    t.regions[1].row = 0; t.regions[1].id = 1;
    check(!t.valid(), "duplicate ids are invalid");
    t.regions[1].id = 0;
    check(!t.valid(), "an id of zero is invalid");
    t.regions[1].id = 2;
    t.regions[1].row = alien::maxSessionTracks;   // first rack lane
    check(t.valid(), "a rack lane region is valid");
    t.regions[1].row = rackRowBase + rackLanes;
    check(!t.valid(), "a row past the last lane is invalid");
    t.regions[1].row = 0;
    t.regions[1].audioOffset = std::numeric_limits<double>::quiet_NaN();
    check(!t.valid(), "a non-finite audio offset is invalid");
    t.regions[1].audioOffset = -0.5;
    check(!t.valid(), "a negative audio offset is invalid");
    t.regions[1].audioOffset = 0; t.regions[1].offset = -1;
    check(!t.valid(), "a negative offset is invalid");
    t.regions[1].offset = 0; t.regions[1].length = t.length();   // runs past the end from bar 1
    check(!t.valid(), "a region past the song end is invalid");
    t.regions[1].length = bar;
    t.regions[0].row = 3;
    check(!t.valid(3, 4, 4) && t.valid(4, 4, 4), "rows must be existing tracks");
    t.regions[0].row = 0; t.regions[0].source = 3;
    check(t.valid(4, 4, 4) && !t.valid(4, 3, 4), "scenes must exist");
    t.regions[0].source = 0; t.regions[1].row = rackRowBase; t.regions[1].source = 3;
    check(t.valid(1, 1, 4) && !t.valid(1, 1, 3), "rack patterns must exist, whatever the track count");

    Timeline u;
    u.loopStart = 0; u.loopEnd = ticksPerStep - 1;
    check(!u.valid(), "a loop shorter than a sixteenth is invalid");
    u.loopEnd = u.length() + 1;
    check(!u.valid(), "a loop past the song end is invalid");
    u.loopStart = -1; u.loopEnd = bar;
    check(!u.valid(), "a negative loop start is invalid");
    u.loopStart = 0; u.loopEnd = bar; u.lengthBars = 0;
    check(!u.valid(), "a song of no bars is invalid");
    u.lengthBars = maxBars + 1;
    check(!u.valid(), "a song of more than the longest allowed is invalid");
    u.lengthBars = 16;
    u.markers = {{1, 100, "a", 0}, {2, 50, "b", 0}};
    check(!u.valid(), "markers out of order are invalid");
    u.markers = {{1, 50, "a", 0}, {1, 100, "b", 0}};
    check(!u.valid(), "duplicate marker ids are invalid");
    u.markers = {{1, 50, "a", 0}, {2, u.length() + 1, "b", 0}};
    check(!u.valid(), "a marker past the end is invalid");
    u.markers = {{1, 50, std::string(maxMarkerNameBytes + 1, 'x'), 0}};
    check(!u.valid(), "an over-long marker name is invalid");
    u.markers = {{1, 50, "ok", 0}, {2, 50, "ok", 0}};
    check(u.valid(), "two markers may share a tick");
}

static void moveTests() {
    Timeline t;
    const auto a = put(t, 1, 0, bar, bar);
    const auto b = put(t, 1, 1, 3 * bar, bar);
    check(t.move({a}, bar) && get(t, a).start == 2 * bar && get(t, b).start == 3 * bar, "moving shifts a region");
    check(!t.move({a}, 0, 0), "moving nowhere is not an edit");
    check(!t.move({999}, bar), "moving a region that does not exist is refused");
    check(t.move({a, b}, -10 * bar) && get(t, a).start == 0 && get(t, b).start == bar, "a move cannot go before the start; the selection keeps its shape");
    Ids moved;
    check(t.move({a, b}, 2 * bar, 0, &moved) && moved.size() == 2 && get(t, a).start == 2 * bar && get(t, b).start == 3 * bar, "moving reports what moved");

    // Landing on another region overwrites it.
    Timeline o;
    const auto x = put(o, 0, 0, 0, bar);
    const auto y = put(o, 0, 1, 2 * bar, 2 * bar);
    check(o.move({x}, 3 * bar) && o.valid(), "dropping a region on another is allowed");
    check(get(o, y).start == 2 * bar && get(o, y).length == bar, "...and the one underneath is trimmed");
    check(get(o, x).start == 3 * bar, "the moved region keeps its place");

    // The end of the timeline: a move stops at the longest song and the song grows to fit.
    Timeline e;
    const auto m = put(e, 0, 0, 0, 2 * bar);
    check(e.move({m}, 100 * bar) && e.lengthBars == 102, "moving far right lengthens the song");
    check(e.move({m}, maxLength) && get(e, m).end() == maxLength && e.lengthBars == maxBars, "a move stops at the end of the timeline");

    // Session clips stay on their track; rack patterns change lanes.
    Timeline r;
    const auto clip = put(r, 2, 0, 0, bar);
    const auto p0 = put(r, rackRowBase, 0, bar, bar);
    const auto p2 = put(r, rackRowBase + 2, 1, 0, bar);
    check(r.move({clip}, bar, 3) && get(r, clip).row == 2, "a Session clip cannot leave its track");
    check(r.move({p0}, 0, 1) && get(r, p0).row == rackRowBase + 1, "a pattern moves to the next lane");
    check(r.move({p0, p2}, 0, 5) && get(r, p0).row == rackRowBase + 2 && get(r, p2).row == rackRowBase + 3, "lane moves stop at the last lane, keeping the spacing");
    check(r.move({p0, p2}, 0, -9) && get(r, p0).row == rackRowBase && get(r, p2).row == rackRowBase + 1, "...and at the first lane");
    check(!r.move({p0, p2}, 0, -1), "no further");
    check(r.move({clip, p0}, 0, 1) && get(r, clip).row == 2 && get(r, p0).row == rackRowBase + 1, "a mixed selection moves its patterns only");
    check(r.valid(), "the timeline stays valid");

    // Several regions on one row moving together do not overwrite each other.
    Timeline g;
    const auto g1 = put(g, 0, 0, 0, bar), g2 = put(g, 0, 1, bar, bar), g3 = put(g, 0, 2, 2 * bar, bar);
    check(g.move({g1, g2, g3}, bar) && g.regions.size() == 3 && get(g, g1).start == bar && get(g, g3).start == 3 * bar, "a run of regions moves as a block");
}

static void trimTests() {
    Timeline t;
    t.tempo = 120.0;
    const auto a = put(t, 0, 0, 4 * bar, 4 * bar);
    check(t.trimStart(a, 5 * bar) && get(t, a).start == 5 * bar && get(t, a).length == 3 * bar && get(t, a).end() == 8 * bar, "trimming the left edge keeps the end");
    check(get(t, a).offset == bar && std::abs(get(t, a).audioOffset - secondsForTicks(bar, 120.0)) < 1e-9, "...and moves the start of the source with it");
    check(t.trimStart(a, 4 * bar) && get(t, a).start == 4 * bar && get(t, a).offset == 0 && std::abs(get(t, a).audioOffset) < 1e-9,
          "the left edge can be pulled back out to the start of the source");
    check(!t.trimStart(a, 3 * bar) && get(t, a).start == 4 * bar, "...but no further: there is nothing before the source");
    check(!t.trimStart(a, 4 * bar), "trimming to where it already is changes nothing");
    check(t.trimStart(a, 100 * bar) && get(t, a).length == 1 && get(t, a).start == 8 * bar - 1, "a region cannot be trimmed away entirely");

    Timeline u;
    const auto b = put(u, 0, 0, 0, 4 * bar);
    check(u.trimEnd(b, 2 * bar) && get(u, b).length == 2 * bar && get(u, b).offset == 0, "trimming the right edge shortens the region");
    check(u.trimEnd(b, 6 * bar) && get(u, b).length == 6 * bar, "the right edge can be dragged out past the source (it loops)");
    check(u.trimEnd(b, 0) && get(u, b).length == 1, "a region cannot be trimmed away from the right either");
    check(!u.trimEnd(b, 1) && !u.trimEnd(999, bar), "no change, or no such region, is refused");
    check(u.trimEnd(b, 40 * bar) && u.lengthBars == 40, "extending past the song end lengthens the song");

    // Neighbours stop the edges.
    Timeline n;
    const auto left = put(n, 0, 0, 0, bar);
    const auto mid = put(n, 0, 1, 2 * bar, bar);
    const auto right = put(n, 0, 2, 4 * bar, bar);
    put(n, 1, 0, 2 * bar, 3 * bar);   // another row never blocks
    check(n.trimEnd(mid, 10 * bar) && get(n, mid).end() == 4 * bar, "the right edge stops at the next region");
    check(n.trimEnd(mid, 3 * bar) && n.trimEnd(mid, 10 * bar) && get(n, mid).end() == 4 * bar, "...also after shortening and growing again");
    // Trim the middle one's left so it carries an offset, then try to extend left past the neighbour.
    check(n.trimStart(mid, 2 * bar + 480) && get(n, mid).offset == 480, "trimmed left by 480");
    check(n.trimStart(mid, 0) && get(n, mid).start == 2 * bar, "the left edge stops at the start of the source (480 ticks back), and the neighbour is further away");
    check(get(n, left).end() == bar && get(n, right).start == 4 * bar, "neighbours are untouched");
    // The previous region's end can be further right than the start of the source.
    Timeline w;
    w.tempo = 120.0;
    const auto wl = put(w, 0, 0, 0, 3 * bar);
    const auto wr = put(w, 0, 1, 3 * bar, 3 * bar);
    check(w.trimStart(wr, 4 * bar) && w.trimEnd(wl, 3 * bar + 1920), "a region trimmed from the left, its neighbour grown towards it");
    check(w.trimStart(wr, 0) && get(w, wr).start == 3 * bar + 1920 && get(w, wr).offset == bar - 1920, "the left edge cannot pass the previous region's end");
    check(w.valid(), "regions never overlap after trimming");

    // Audio offsets follow the tempo the timeline was told about.
    Timeline s;
    s.tempo = 60.0;
    const auto c = put(s, 0, 0, 0, 4 * bar);
    s.trimStart(c, bar);
    check(std::abs(get(s, c).audioOffset - secondsForTicks(bar, 60.0)) < 1e-9 && std::abs(get(s, c).audioOffset - 4.0) < 1e-9, "a bar at 60 BPM is four seconds of audio");
    s.tempo = 30.0;   // at 30 BPM the four seconds of audio before the region are only 1920 ticks
    check(s.trimStart(c, 0) && get(s, c).start == bar - 1920, "pulling an edge back is limited by the audio that is there at the current tempo");
}

static void splitTests() {
    Timeline t;
    t.tempo = 120.0;
    const auto a = put(t, 0, 3, bar, 4 * bar);
    const auto b = put(t, 1, 4, 0, 4 * bar);
    Ids created;
    check(t.split({a}, 3 * bar, &created) && created.size() == 1 && t.regions.size() == 3, "splitting makes one more region");
    check(get(t, a).start == bar && get(t, a).end() == 3 * bar && get(t, a).source == 3, "the left half keeps its start");
    const auto& right = get(t, created[0]);
    check(right.start == 3 * bar && right.end() == 5 * bar && right.source == 3 && right.row == 0 && right.id != a, "the right half takes the rest");
    check(right.offset == 2 * bar && std::abs(right.audioOffset - secondsForTicks(2 * bar, 120.0)) < 1e-9, "the right half plays on from where the left half stopped");
    check(t.valid() && get(t, b).length == 4 * bar, "other rows are untouched");

    check(!t.split({a}, bar) && !t.split({a}, 3 * bar) && !t.split({a}, 100 * bar), "a cut at or beyond an edge is not a cut");
    const auto before = t.regions.size();
    check(!t.split({999}, 2 * bar) && t.regions.size() == before, "splitting nothing is refused");

    // Splitting twice then rejoining the offsets: the middle piece starts where the first ended.
    Ids second;
    check(t.split({a}, 2 * bar, &second) && get(t, second[0]).offset == bar && get(t, second[0]).start == 2 * bar, "a region can be split again");

    // Several at once, including grouped ones.
    Timeline g;
    const auto x = put(g, 0, 0, 0, 4 * bar), y = put(g, 1, 0, 0, 4 * bar), z = put(g, 2, 0, 0, 4 * bar);
    g.group({x, y});
    Ids made;
    check(g.split({x, y}, 2 * bar, &made) && made.size() == 2 && g.regions.size() == 5, "every listed region is split");
    check(get(g, made[0]).group == get(g, x).group && get(g, made[0]).group != 0, "both halves of a split stay in the group");
    check(g.find(z) != nullptr && g.regions.size() == 5 && get(g, z).length == 4 * bar, "an unlisted region is left whole");

    // A cut at one region's edge must not spoil the cut through another: only the regions it really crosses are split.
    Timeline mixed;
    const auto inside = put(mixed, 0, 0, 0, 2 * bar), edge = put(mixed, 1, 0, bar, 2 * bar);
    Ids pieces;
    check(mixed.split({inside, edge}, bar, &pieces) && pieces.size() == 1 && mixed.regions.size() == 3,
          "a cut through one region and along the edge of another splits only the first");
    check(get(mixed, inside).length == bar && get(mixed, edge).start == bar && get(mixed, edge).length == 2 * bar && mixed.valid(),
          "and leaves the other whole");

    // Splitting near the region limit is refused cleanly.
    Timeline full;
    for (std::size_t i = 0; i < maxRegions; ++i) full.placeBars(static_cast<int>(i % 16), 0, static_cast<int>(i / 16) * 2, 2);
    const Timeline copy = full;
    check(!full.split({full.regions[0].id}, bar) && full == copy, "a split that would exceed 1024 regions changes nothing");
}

static void clipboardTests() {
    Timeline t;
    t.tempo = 120.0;
    const auto a = put(t, 0, 0, bar, bar);
    const auto b = put(t, 1, 1, 2 * bar, 2 * bar);
    const auto c = put(t, rackRowBase + 1, 2, bar, 2 * bar);
    const auto clip = t.copy({a, b, c});
    check(clip.regions.size() == 3 && !clip.empty(), "copying takes every listed region");
    std::int64_t earliest = maxLength;
    for (const auto& r : clip.regions) { earliest = std::min(earliest, r.start); check(r.id == 0, "copied regions have no id"); }
    check(earliest == 0 && clip.span() == 3 * bar, "copied starts are relative to the earliest, which is bar 0 of the clip");
    check(t.copy({}).empty() && t.copy({12345}).empty(), "copying nothing gives an empty clipboard");

    Ids created;
    check(t.paste(clip, 10 * bar, 0, &created) && created.size() == 3 && t.regions.size() == 6, "pasting adds the copies");
    check(get(t, created[0]).start == 10 * bar && get(t, created[1]).start == 11 * bar && get(t, created[2]).start == 10 * bar, "...in the same arrangement, from the paste position");
    check(get(t, created[0]).row == 0 && get(t, created[1]).row == 1 && get(t, created[2]).row == rackRowBase + 1, "...on the same rows");
    check(get(t, created[0]).source == 0 && get(t, created[1]).source == 1 && get(t, created[2]).source == 2, "...of the same sources");
    for (const auto id : created) check(id != a && id != b && id != c, "copies have fresh ids");

    // Pasting patterns into other lanes.
    Ids lanes;
    check(t.paste(clip, 30 * bar, 2, &lanes) && get(t, lanes[2]).row == rackRowBase + 3, "patterns can be pasted into another lane");
    check(get(t, lanes[0]).row == 0 && get(t, lanes[1]).row == 1, "Session clips keep their tracks");
    const Timeline before = t;
    check(!t.paste(clip, 40 * bar, 3) && t == before, "a lane past the last is refused and nothing is pasted");
    check(!t.paste(Clipboard{}, bar) && !t.paste(clip, -1) && t == before, "an empty clipboard or a negative position is refused");
    check(!t.paste(clip, maxLength - bar) && t == before, "a paste past the last bar is refused");

    // Pasting over existing regions overwrites them.
    Timeline o;
    const auto base = put(o, 0, 0, 0, 8 * bar);
    Timeline src;
    const auto s1 = put(src, 0, 5, 0, bar);
    check(o.paste(src.copy({s1}), 2 * bar) && o.valid() && o.regions.size() == 3, "pasting into the middle of a region splits it");
    check(get(o, base).length == 2 * bar, "the left part is untouched");

    // Copies keep trimming, mute and (as new groups) grouping.
    Timeline g;
    const auto g1 = put(g, 0, 0, 0, 2 * bar), g2 = put(g, 1, 1, 0, 2 * bar), g3 = put(g, 2, 2, 0, 2 * bar);
    g.trimStart(g1, bar); g.setMuted({g1}, true);
    g.group({g1, g2});
    Ids copies;
    check(g.paste(g.copy({g1, g2, g3}), 10 * bar, 0, &copies) && copies.size() == 3, "pasting a group");
    check(get(g, copies[0]).offset == bar && get(g, copies[0]).muted && get(g, copies[0]).start == 11 * bar, "trim and mute are copied");
    check(get(g, copies[0]).group != 0 && get(g, copies[0]).group == get(g, copies[1]).group, "grouped copies are grouped together");
    check(get(g, copies[0]).group != get(g, g1).group, "...but not with the originals");
    check(get(g, copies[2]).group == 0, "an ungrouped region stays ungrouped");

    // Copying a single member of a group leaves a group of one, which is no group.
    Ids one;
    check(g.paste(g.copy({g1}), 20 * bar, 0, &one) && get(g, one[0]).group == 0, "a lone copy of a group member is not grouped");

    // Pruning a clipboard for a smaller project.
    auto pruned = clip;
    pruned.prune(1, 1, 2);
    check(pruned.regions.size() == 1 && pruned.regions[0].row == 0, "pruning drops clips for tracks that are gone and patterns that no longer exist");
    pruned = clip; pruned.prune(2, 2, 3);
    check(pruned.regions.size() == 3, "...and keeps what still exists");
}

static void duplicateTests() {
    Timeline t;
    const auto a = put(t, 0, 0, 2 * bar, bar);
    const auto b = put(t, 1, 1, 3 * bar, bar);
    Ids copies;
    check(t.duplicate({a, b}, &copies) && copies.size() == 2, "duplicating a selection copies it");
    check(get(t, copies[0]).start == 4 * bar && get(t, copies[1]).start == 5 * bar, "the copies start where the selection ended, keeping their spacing");
    check(get(t, a).start == 2 * bar && get(t, b).start == 3 * bar, "the originals stay");
    copies.clear();
    check(t.duplicate({a}, &copies) && get(t, copies[0]).start == 3 * bar && get(t, copies[0]).row == 0, "duplicating a single region puts the copy right after it");
    check(get(t, b).row == 1 && get(t, b).start == 3 * bar && countOnRow(t, 0) == 3, "...on its own row only");
    Timeline over;
    const auto ox = put(over, 0, 0, 0, bar), oy = put(over, 0, 1, bar, bar);
    copies.clear();
    check(over.duplicate({ox}, &copies) && over.find(oy) == nullptr && over.regions.size() == 2 && get(over, copies[0]).start == bar,
          "a copy that lands on another region overwrites it");
    check(!t.duplicate({}) && !t.duplicate({4242}), "duplicating nothing is refused");
    Timeline edge;
    const auto e = put(edge, 0, 0, maxLength - bar, bar);
    check(!edge.duplicate({e}), "duplicating past the end of the timeline is refused");
}

static void editStateTests() {
    Timeline t;
    const auto a = put(t, 0, 0, 0, bar), b = put(t, 1, 0, 0, bar), c = put(t, 2, 0, 0, bar), d = put(t, 3, 0, 0, bar);
    check(t.setMuted({a, b}, true) && get(t, a).muted && get(t, b).muted && !get(t, c).muted, "muting");
    check(!t.setMuted({a, b}, true), "muting what is muted is not an edit");
    check(t.setMuted({a}, false) && !get(t, a).muted && get(t, b).muted, "unmuting one");

    check(!t.group({a}) && !t.group({}), "a group needs two regions");
    check(t.group({a, b}) && get(t, a).group != 0 && get(t, a).group == get(t, b).group && get(t, c).group == 0, "grouping");
    const auto first = get(t, a).group;
    check(t.group({b, c}) && get(t, a).group == get(t, c).group && get(t, a).group != first, "grouping a member brings its whole group along");
    check(t.expandGroups({c}).size() == 3 && t.expandGroups({d}).size() == 1, "expanding a selection to its groups");
    check(t.ungroup({c}) && get(t, a).group == 0 && get(t, b).group == 0 && get(t, c).group == 0, "ungrouping one member dissolves the group");
    check(!t.ungroup({a, d}), "ungrouping what is not grouped changes nothing");

    t.group({a, b});
    check(t.remove({a}) && t.find(a) == nullptr && get(t, b).group == 0, "removing a member leaves the other ungrouped");
    check(!t.remove({a}) && !t.remove({}), "removing nothing is refused");
    check(t.remove({b, c, d}) && t.regions.empty(), "removing several");

    // Marquee selection by rows and time.
    Timeline m;
    const auto m1 = put(m, 0, 0, 0, bar), m2 = put(m, 1, 0, 2 * bar, bar), m3 = put(m, 2, 0, 4 * bar, bar), m4 = put(m, rackRowBase, 0, bar, 2 * bar);
    auto ids = m.idsIn(0, 1, 0, 3 * bar);
    check(ids.size() == 2 && contains(ids, m1) && contains(ids, m2), "a box over two rows selects what it touches");
    check(m.idsIn(1, 0, 3 * bar, 0).size() == 2, "a box dragged the other way selects the same");
    check(m.idsIn(0, 2, bar, 2 * bar).empty(), "a box between regions selects nothing (touching edges do not count)");
    check(m.idsIn(0, 2, bar - 1, 2 * bar + 1).size() == 2 && contains(m.idsIn(0, 2, 4 * bar + 5, 4 * bar + 6), m3), "one tick of overlap selects");
    check(m.idsIn(rackRowBase, rowCount - 1, 0, 100 * bar) == Ids{m4}, "a box over the rack lanes selects patterns");
}

static void lengthLoopMarkerTests() {
    Timeline t;
    check(t.setLengthBars(32) && t.lengthBars == 32 && !t.setLengthBars(32), "setting the song length");
    put(t, 0, 0, 20 * bar, 2 * bar);
    check(t.setLengthBars(1) && t.lengthBars == 22, "the song cannot be shorter than its last region");
    check(t.setLengthBars(maxBars + 1000) && t.lengthBars == maxBars, "...or longer than 16384 bars");
    Timeline partial;
    put(partial, 0, 0, 5 * bar, 1);
    check(partial.contentEnd() == 5 * bar + 1 && partial.minLengthBars() == 6, "a region that ends part-way into a bar needs that whole bar");
    check(partial.setLengthBars(1) && partial.lengthBars == 6 && partial.valid(), "so shortening stops at the end of that bar");
    t.setLoop(10 * bar, 20 * bar);
    check(t.setLengthBars(22) && t.loopEnd == 20 * bar, "a shorter song keeps a loop that still fits");
    Timeline s;
    s.setLengthBars(8); s.setLoop(6 * bar, 8 * bar);
    check(s.setLengthBars(7) && s.loopEnd == 7 * bar && s.loopStart == 6 * bar, "a loop running past a shortened song is cut to the end");
    Timeline tiny;
    tiny.setLengthBars(8); tiny.setLoop(8 * bar - ticksPerStep, 8 * bar);
    check(tiny.setLengthBars(4) && tiny.valid() && tiny.loopStart == 0 && tiny.loopEnd == 4 * bar, "a loop that no longer fits is reset to the whole song");

    Timeline l;
    check(l.setLoop(bar, 3 * bar) && l.loopStart == bar && l.loopEnd == 3 * bar, "setting the loop");
    check(!l.setLoop(bar, 3 * bar), "...to what it already is does nothing");
    check(l.setLoop(5 * bar, 2 * bar) && l.loopStart == 2 * bar && l.loopEnd == 5 * bar, "a loop dragged backwards is turned round");
    check(!l.setLoop(bar, bar) && !l.setLoop(bar, bar + ticksPerStep - 1) && l.loopStart == 2 * bar, "a loop shorter than a sixteenth is refused");
    check(l.setLoop(-100, 500 * bar) && l.loopStart == 0 && l.loopEnd == l.length(), "a loop is held inside the song");
    check(l.setLoop(bar, bar + ticksPerStep) && l.loopEnd - l.loopStart == ticksPerStep, "a loop can be one sixteenth long");
    check(l.setLoopEnabled(true) && l.loopEnabled && !l.setLoopEnabled(true) && l.setLoopEnabled(false) && !l.loopEnabled, "switching the loop on and off");
    check(l.valid(), "the timeline is valid");

    Timeline m;
    const auto late = m.addMarker(8 * bar, "Drop", 3);
    const auto early = m.addMarker(bar, "  Intro  ");
    const auto same = m.addMarker(8 * bar, "Also at 8");
    check(late != 0 && early != 0 && same != 0 && m.markers.size() == 3 && m.valid(), "adding markers");
    check(m.markers[0].id == early && m.markers[0].name == "Intro" && m.markers[1].id == late && m.markers[2].id == same, "markers stay in time order, in the order they were added at one tick, and names are trimmed");
    check(m.markers[1].colour == 3 && m.markers[0].colour == 0, "markers keep their colour");
    check(m.addMarker(-1, "x") == 0 && m.addMarker(m.length() + 1, "x") == 0 && m.markers.size() == 3, "a marker off the timeline is refused");
    check(m.addMarker(m.length(), "End") != 0, "a marker at the very end is fine");
    check(m.addMarker(0, "") != 0 && m.markers[0].name.empty(), "a marker may have no name");
    const std::string long8 = std::string(30, 'a') + "\xC3\xA9\xC3\xA9";   // two 2-byte characters straddling the 32-byte limit
    const auto cut = m.addMarker(2 * bar, long8);
    const auto* found = [&]() -> const Marker* { for (const auto& k : m.markers) if (k.id == cut) return &k; return nullptr; }();
    check(found != nullptr && found->name.size() == 32 && found->name.substr(30) == "\xC3\xA9", "a long name is cut on a character boundary");
    const std::string nine = std::string(31, 'a') + "\xC3\xA9";   // the second byte of the character is past the limit
    const auto cut2 = m.addMarker(3 * bar, nine);
    for (const auto& k : m.markers) if (k.id == cut2) check(k.name == std::string(31, 'a'), "a character that would be split is dropped whole");

    check(m.moveMarker(early, 9 * bar) && m.valid(), "moving a marker re-sorts the list");
    std::int64_t prev = -1; bool sorted = true;
    for (const auto& k : m.markers) { sorted = sorted && k.tick >= prev; prev = k.tick; }
    check(sorted, "markers are still in time order");
    check(!m.moveMarker(early, 9 * bar) && !m.moveMarker(99999, bar), "moving to the same place, or a missing marker, changes nothing");
    check(m.moveMarker(early, -50) && m.markers[0].tick == 0 && m.moveMarker(early, m.length() * 2) && m.valid(), "a moved marker is held on the timeline");
    check(m.renameMarker(late, "Chorus") && !m.renameMarker(late, "Chorus") && !m.renameMarker(late, "  Chorus ") && !m.renameMarker(99999, "x"), "renaming");
    check(m.colourMarker(late, 5) && !m.colourMarker(late, 5) && !m.colourMarker(99999, 1), "colouring");

    Timeline n;
    n.addMarker(bar, "A"); n.addMarker(3 * bar, "B"); n.addMarker(5 * bar, "C");
    check(n.markerAfter(0)->name == "A" && n.markerAfter(bar)->name == "B" && n.markerAfter(5 * bar) == nullptr, "the next marker is strictly after");
    check(n.markerBefore(6 * bar)->name == "C" && n.markerBefore(5 * bar)->name == "B" && n.markerBefore(bar) == nullptr, "the previous marker is strictly before");
    check(n.minLengthBars() == 5, "a marker counts as content, so the song cannot be shortened past it");
    check(n.removeMarker(n.markers[1].id) && n.markers.size() == 2 && !n.removeMarker(99999), "removing a marker");

    Timeline cap;
    bool allAdded = true;
    for (std::size_t i = 0; i < maxMarkers; ++i) allAdded = allAdded && cap.addMarker(static_cast<std::int64_t>(i) * 100, "m") != 0;
    check(allAdded && cap.markers.size() == maxMarkers && cap.addMarker(1, "one too many") == 0, "128 markers fit; the next is refused");
}

static void remapTests() {
    Timeline t;
    const auto a = put(t, 0, 0, 0, bar);
    const auto b = put(t, 1, 2, 0, bar);
    const auto c = put(t, 2, 1, 0, bar);
    const auto p0 = put(t, rackRowBase, 0, bar, bar);
    const auto p1 = put(t, rackRowBase, 3, 2 * bar, bar);
    const auto p2 = put(t, rackRowBase + 1, 5, 0, bar);

    Timeline d = t;
    d.trackDeleted(1);
    check(d.find(b) == nullptr && d.find(a) != nullptr && get(d, c).row == 1 && get(d, p0).row == rackRowBase, "deleting a track removes its regions and renumbers later tracks; rack lanes are not tracks");
    d = t; d.tracksSwapped(0, 2);
    check(get(d, a).row == 2 && get(d, c).row == 0 && get(d, b).row == 1 && get(d, p0).row == rackRowBase, "swapping tracks swaps their rows");
    d = t; d.sceneDeleted(1);
    check(d.find(c) == nullptr && get(d, b).source == 1 && get(d, a).source == 0 && get(d, p1).source == 3, "deleting a scene removes its regions and renumbers later scenes; patterns are separate");
    d = t; d.scenesSwapped(0, 2);
    check(get(d, a).source == 2 && get(d, b).source == 0 && get(d, c).source == 1 && get(d, p0).source == 0, "swapping scenes swaps the references");
    d = t; d.sceneInserted(1);
    check(get(d, a).source == 0 && get(d, b).source == 3 && get(d, c).source == 2 && get(d, p1).source == 3, "inserting a scene shifts the ones after it");
    d = t; d.patternDeleted(3);
    check(d.find(p1) == nullptr && get(d, p2).source == 4 && get(d, p0).source == 0 && get(d, b).source == 2, "deleting a pattern removes its regions and renumbers later patterns; Session scenes are separate");
    d = t; d.patternDeleted(0);
    check(d.find(p0) == nullptr && get(d, p1).source == 2 && get(d, p2).source == 4, "deleting the first pattern");
    d = t; d.prune(2, 2, 4);
    check(d.find(c) == nullptr && d.find(b) == nullptr && d.find(a) != nullptr && d.find(p2) == nullptr && d.find(p1) != nullptr && d.valid(2, 2, 4),
          "pruning removes every dangling reference");
    d = t; d.group({a, b, c}); d.trackDeleted(1); d.trackDeleted(1);
    check(get(d, a).group == 0, "a group left with one member is dissolved");
}


static void nonDestructiveAudioEditTests() {
    Timeline t; t.tempo = 120.0;
    const auto left = put(t, 0, 0, 0, bar);
    const auto right = put(t, 0, 1, bar, bar);
    check(t.slip({left}, 240), "audio can slip later in its source");
    check(get(t, left).start == 0 && get(t, left).length == bar && get(t, left).offset == 240,
          "slip preserves both timeline edit boundaries");
    check(std::abs(get(t, left).audioOffset - 0.125) < 1.0e-9, "slip stores the matching source offset");
    check(t.slip({left}, -1000) && get(t, left).offset == 0 && get(t, left).audioOffset == 0.0,
          "slip clamps at the beginning of the immutable source");

    check(t.setLoopSource({left}, true) && t.setReversed({left}, true), "loop and reverse are non-destructive region flags");
    check(t.setGain({left}, 2.0), "normalization gain is stored on the region");
    check(t.setFades({left}, 480, 240), "fade boundaries are stored on the region");
    check(get(t, left).loopSource && get(t, left).reversed && get(t, left).gain == 2.0
          && get(t, left).fadeIn == 480 && get(t, left).fadeOut == 240, "audio edit state is retained");

    check(t.crossfade(left, right, 960), "touching clips can be crossfaded");
    check(get(t, right).crossfade == 960 && get(t, left).fadeOut == 0 && get(t, right).fadeIn == 0
          && get(t, left).start == 0 && get(t, left).end() == bar && get(t, right).start == bar,
          "a crossfade is stored on the right clip, replaces the edge fades it covers and moves no boundary");
    check(t.crossfadePartner(right) != nullptr && t.crossfadePartner(right)->id == left && t.crossfadePartner(left) == nullptr,
          "the clip ending where a crossfade starts is its partner");
    const auto before = t;
    check(!t.crossfade(left, right, 960) && t == before, "repeating the same edit is a no-op");
    check(t.setFadeIn(right, 480) && get(t, right).crossfade == 480 && get(t, right).fadeIn == 0,
          "dragging a crossfaded clip's fade-in handle changes the crossfade");
    check(!t.setFadeIn(right, 0) && get(t, right).crossfade == 480, "dragging it to nothing does not silently remove it");
    check(t.crossfade(left, right, 0) && get(t, right).crossfade == 0, "a crossfade of 0 removes it");
    check(t.setFadeIn(right, 240) && get(t, right).fadeIn == 240 && t.setFadeOut(left, 120) && get(t, left).fadeOut == 120,
          "each edge's fade can be set on its own");
    check(t.crossfade(left, right, 5 * bar) && get(t, right).crossfade == bar, "a crossfade is no longer than the clip it starts");
    check(!t.crossfade(right, left, 240), "only a clip ending where the other starts can cross into it");
    check(!t.setGain({left}, -1.0) && !t.setFades({left}, -1, 0), "invalid audio edits are rejected");
    check(t.valid(), "edited timeline remains valid");

    Timeline edges; const auto edge = put(edges, 0, 0, 0, bar);
    check(edges.setFades({edge}, 3000, 3000), "long edge fades can be assigned");
    check(edges.trimEnd(edge, 1200) && get(edges, edge).fadeIn == 1200 && get(edges, edge).fadeOut == 1200,
          "trimming an edited clip clamps fades to its new boundary");
    Ids made;
    check(edges.split({edge}, 600, &made) && made.size() == 1, "an edited clip can still be split");
    check(get(edges, edge).fadeOut == 0 && get(edges, made[0]).fadeIn == 0
          && get(edges, edge).fadeIn == 600 && get(edges, made[0]).fadeOut == 600,
          "split preserves outer fades and creates clean internal edges");

    Timeline crossing; crossing.tempo = 120.0;
    const auto a = put(crossing, 0, 0, 0, bar), b = put(crossing, 0, 1, bar, bar);
    check(crossing.crossfade(a, b, 960), "fixture: a crossfade");
    made.clear();
    check(crossing.split({b}, bar + 1200, &made) && get(crossing, b).crossfade == 960 && get(crossing, made[0]).crossfade == 0,
          "splitting a crossfaded clip keeps the crossfade on its first half only");
    check(crossing.trimEnd(b, bar + 600) && get(crossing, b).crossfade == 600, "trimming clamps a crossfade to the clip");
    Timeline broken = crossing; broken.regions[1].crossfade = 2 * bar;
    check(!broken.valid(), "a crossfade longer than its clip is invalid");
    broken = crossing; broken.regions[1].crossfade = -1;
    check(!broken.valid(), "a negative crossfade is invalid");
}

static bool near(double a, double b, double tolerance = 1.0e-9) { return std::abs(a - b) <= tolerance; }

static void loopPassTakeTests() {
    Timeline t;
    std::vector<Region> passes(3);
    for (std::size_t k = 0; k < passes.size(); ++k) {
        passes[k].row = 2; passes[k].source = 4; passes[k].start = 0; passes[k].length = k == 2 ? bar / 2 : bar;
        passes[k].audioOffset = 2.0 * static_cast<double>(k);
    }
    Ids ids;
    check(t.recordTakes(passes, 1, &ids) && ids.size() == 3 && t.takes.size() == 3, "every pass of a loop recording is kept as a take");
    check(t.regions.size() == 1 && t.regions[0].audioOffset == 2.0 && t.regions[0].length == bar && t.regions[0].id != ids[1],
          "only the active pass is on the arrangement, as a region of its own");
    check(t.takes[0].audioOffset == 0.0 && t.takes[2].length == bar / 2 && t.takes[0].id == ids[0], "the takes keep their own place in the file");
    const auto before = t;
    check(!t.recordTakes(passes, 3) && !t.recordTakes({}, 0) && t == before, "an active pass that is not one of them, or no passes, is refused");
    Timeline full; full.takes.assign(maxTakes - 2, passes[0]);
    for (auto& r : full.takes) r.id = newId();
    check(!full.recordTakes(passes, 0) && full.takes.size() == maxTakes - 2, "more passes than the take list has room for are refused");
    Region pattern = passes[0]; pattern.row = rackRowBase;
    check(!t.recordTakes({passes[0], pattern}, 0) && t == before, "a pattern lane cannot hold a recorded pass");
}

static void tempoMapTests() {
    Timeline t; t.tempo = 120.0;
    check(t.tempoChanges.empty() && t.tempoAt(0) == 120.0 && t.tempoAt(1.0e9) == 120.0, "no changes: the project tempo throughout");
    check(near(t.secondsBetween(0, bar), 2.0) && near(t.tickAfter(0, 2.0), static_cast<double>(bar)),
          "without changes a 4/4 bar at 120 BPM is two seconds, both ways");
    check(std::isinf(t.nextTempoChange(0)), "no next change");

    // 120 BPM for two bars, 60 from bar 2, 180 from bar 4.
    check(t.setTempoChange(2 * bar, 60.0) && t.setTempoChange(4 * bar, 180.0), "tempo changes are added");
    check(t.tempoChanges.size() == 2 && t.tempoChanges[0].tick == 2 * bar && t.tempoChanges[1].tick == 4 * bar, "kept in order");
    check(t.tempoAt(2 * bar - 1) == 120.0 && t.tempoAt(2 * bar) == 60.0 && t.tempoAt(4 * bar) == 180.0,
          "a change applies from its own tick on");
    check(t.nextTempoChange(0) == 2 * bar && t.nextTempoChange(2 * bar) == 4 * bar && std::isinf(t.nextTempoChange(4 * bar)),
          "the next change is strictly after the tick asked about");
    // 2 bars at 120 = 4 s, 2 bars at 60 = 8 s, 1 bar at 180 = 4/3 s.
    check(near(t.secondsBetween(0, 5 * bar), 4.0 + 8.0 + 4.0 / 3.0), "seconds add up across every change");
    check(near(t.secondsBetween(5 * bar, 0), -(4.0 + 8.0 + 4.0 / 3.0)), "backwards is negative");
    check(near(t.secondsBetween(bar, 3 * bar), 2.0 + 4.0), "a span that starts and ends between changes");
    check(near(t.tickAfter(0, 12.0), static_cast<double>(4 * bar)), "seconds land on a change exactly");
    check(near(t.tickAfter(bar, 5.0), static_cast<double>(2 * bar + bar / 4 * 3)), "forward across a change");
    check(near(t.tickAfter(5 * bar, -(4.0 / 3.0 + 8.0)), static_cast<double>(2 * bar)), "backward across changes");
    check(near(t.tickAfter(0, -1.0), -static_cast<double>(bar) / 2.0), "before tick 0 the project tempo continues");
    for (double tick : {0.0, 100.5, 7679.0, 7680.0, 9000.25, 15360.0, 20000.0})
        for (double seconds : {0.001, 1.0, 3.9999, 7.5, 13.0})
            check(near(t.secondsBetween(tick, t.tickAfter(tick, seconds)), seconds, 1.0e-9)
                  && near(t.tickAfter(t.tickAfter(tick, seconds), -seconds), tick, 1.0e-6),
                  "tickAfter and secondsBetween invert each other from " + std::to_string(tick));
    const auto copy = t.tempoChanges;
    const TempoMap live{copy, 90.0};
    check(live.tempoAt(0) == 90.0 && near(live.secondsBetween(0, 2 * bar), 16.0 / 3.0) && live.tempoAt(2 * bar) == 60.0,
          "a map read with another base tempo changes only the part before the first change");

    // Editing.
    check(t.setTempoChange(2 * bar, 70.0) && t.tempoChanges.size() == 2 && t.tempoAt(2 * bar) == 70.0, "setting an existing change replaces its tempo");
    check(!t.setTempoChange(2 * bar, 70.0), "setting the same tempo again changes nothing");
    check(!t.setTempoChange(0, 100.0) && !t.setTempoChange(-1, 100.0), "tick 0 belongs to the project tempo");
    check(!t.setTempoChange(bar, 59.9) && !t.setTempoChange(bar, 200.1) && !t.setTempoChange(bar, std::nan("")),
          "tempos outside 60-200 BPM are refused");
    check(!t.setTempoChange(t.length() + 1, 100.0) && t.setTempoChange(t.length(), 100.0), "a change must be inside the song");
    check(t.removeTempoChange(t.length()) && !t.removeTempoChange(t.length()), "a change can be removed once");
    check(t.moveTempoChange(4 * bar, 3 * bar) && t.tempoAt(3 * bar) == 180.0 && t.tempoChanges.size() == 2, "a change can move");
    check(t.moveTempoChange(3 * bar, 2 * bar) && t.tempoChanges.size() == 1 && t.tempoAt(2 * bar) == 180.0,
          "moving onto another change replaces it");
    check(!t.moveTempoChange(bar, 3 * bar) && !t.moveTempoChange(2 * bar, 0), "moving a missing change, or to tick 0, is refused");
    check(t.minLengthBars() == 2 && t.setLengthBars(1) && t.lengthBars == 2, "the song cannot be shortened below a tempo change");

    // Validation and equality.
    Timeline bad = t; bad.tempoChanges.push_back({2 * bar, 100.0});
    check(!bad.valid(), "two changes at one tick are invalid");
    bad = t; bad.tempoChanges.insert(bad.tempoChanges.begin(), {3 * bar, 100.0});
    check(!bad.valid(), "changes out of order are invalid");
    bad = t; bad.tempoChanges[0].bpm = 10.0;
    check(!bad.valid(), "an out-of-range tempo is invalid");
    bad = t; bad.tempoChanges.assign(maxTempoChanges + 1, {});
    for (std::size_t i = 0; i < bad.tempoChanges.size(); ++i) bad.tempoChanges[i] = {static_cast<std::int64_t>(i + 1), 120.0};
    check(!bad.valid(), "more than the maximum number of changes is invalid");
    Timeline other = t; other.tempoChanges[0].bpm = 100.0;
    check(!(other == t) && !other.playbackEquals(t), "the tempo map is part of the song and of what plays");
    other = t; other.tempo = 90.0;
    check(other == t, "the project tempo copy is bookkeeping, not content");
    check(t.clearTempoChanges() && t.tempoChanges.empty() && !t.clearTempoChanges(), "all changes can be cleared");

    // Audio edits measure source seconds along the tempo map.
    Timeline audio; audio.tempo = 120.0;
    check(audio.setTempoChange(bar, 60.0), "fixture: 60 BPM from bar 1");
    const auto clip = put(audio, 0, 0, 0, 2 * bar);
    check(audio.trimStart(clip, bar + bar / 2) && near(get(audio, clip).audioOffset, 2.0 + 2.0),
          "trimming across a change skips 2 s at 120 BPM and 2 s at 60 BPM of source");
    check(audio.trimStart(clip, bar / 2) && near(get(audio, clip).audioOffset, 1.0),
          "revealing it again gives the seconds back the same way");
    Ids made;
    check(audio.split({clip}, bar + bar / 4, &made) && made.size() == 1 && near(get(audio, made[0]).audioOffset, 1.0 + 1.0 + 1.0),
          "the right half of a split starts at the source second under the cut");
    check(audio.slip({clip}, bar / 2) && near(get(audio, clip).audioOffset, 1.0 + 1.0),
          "slipping moves the source by the seconds that many ticks last where the clip starts");
}

static void equalityTests() {
    Timeline a;
    const auto id = put(a, 0, 0, 0, bar);
    Timeline b = a;
    check(a == b && a.playbackEquals(b), "a copy equals its source");
    b.tempo = 90.0;
    check(a == b, "tempo bookkeeping is not content");
    b.addMarker(bar, "x");
    check(!(a == b) && a.playbackEquals(b), "a marker changes the content but not the sound");
    b = a;
    b.regions[0].group = 7;
    check(!(a == b) && a.playbackEquals(b), "a group changes the content but not the sound");
    b = a; b.regions[0].id = 99;
    check(!(a == b) && a.playbackEquals(b), "ids are not part of the sound");
    b = a; b.setMuted({id}, true);
    check(!a.playbackEquals(b), "muting changes the sound");
    b = a; b.trimStart(id, 100); check(!a.playbackEquals(b), "trimming changes the sound");
    b = a; b.setLoopEnabled(true); check(!a.playbackEquals(b), "the loop changes the sound");
    b = a; b.setLoop(bar, 2 * bar); check(!a.playbackEquals(b), "the loop range changes the sound");
    b = a; b.setLengthBars(20); check(!a.playbackEquals(b), "the song length changes the sound");
    b = a; b.regions[0].audioOffset = 0.5; check(!a.playbackEquals(b), "the audio offset changes the sound");
    b = a; b.regions[0].source = 1; check(!a.playbackEquals(b), "the source changes the sound");
    b = a; b.setGain({id}, 1.5); check(!a.playbackEquals(b), "region gain changes the sound");
    b = a; b.setFades({id}, 120, 120); check(!a.playbackEquals(b), "region fades change the sound");
    b = a; b.setReversed({id}, true); check(!a.playbackEquals(b), "reverse changes the sound");
    b = a; b.regions[0].crossfade = 240; check(!a.playbackEquals(b), "a crossfade changes the sound");
    b = a; b.setLoopSource({id}, true); check(!a.playbackEquals(b), "source looping changes the sound");
    b = a; put(b, 1, 0, 0, bar); check(!a.playbackEquals(b), "another region changes the sound");
}

static void freezeModelChecks() {
    alien::timeline::Timeline t;
    alien::timeline::Region a;a.row=0;a.source=0;a.length=3840;
    if(!t.place(a))std::abort();a.row=1;if(!t.place(a))std::abort();
    const auto before=t;const auto original=*t.regionAt(0,0);
    if(!t.freezeTrack(0,2,5000,2.0) || t.frozenOriginals.size()!=1 || t.regionAt(0,0)->gain!=2.0)std::abort();
    auto frozen=t;
    if(t.freezeTrack(0,3,5000) || !(t==frozen))std::abort();
    t.tracksSwapped(0,1);if(t.frozenOriginals[0].row!=1)std::abort();
    t.scenesSwapped(0,1);if(t.frozenOriginals[0].source!=1)std::abort();
    t.scenesSwapped(0,1);t.tracksSwapped(0,1);
    if(!t.unfreezeTrack(0) || *t.find(original.id)!=original || !t.frozenOriginals.empty())std::abort();
    if(t.regions.size()!=before.regions.size())std::abort();
}

int main() {
    freezeModelChecks();
    constantsAndDefaults();
    placementTests();
    validityTests();
    moveTests();
    trimTests();
    splitTests();
    clipboardTests();
    duplicateTests();
    editStateTests();
    lengthLoopMarkerTests();
    remapTests();
    nonDestructiveAudioEditTests();
    equalityTests();
    tempoMapTests();
    loopPassTakeTests();
    if (failures == 0) std::puts("TimelineTests passed");
    return failures == 0 ? 0 : 1;
}
