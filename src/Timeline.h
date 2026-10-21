#pragma once
// The arrangement timeline: a song laid out along one time axis. No JUCE and no
// audio code, so every editing rule is unit-testable in milliseconds.
//
// Time is measured in ticks, 960 to a quarter note (a sixteenth is 240 ticks,
// the same unit the piano roll uses, a 4/4 bar is 3840). A region places one of
// two kinds of source on the timeline by reference:
//   * a Session clip (a track and a scene), which is MIDI or audio depending on
//     what that slot holds, lives on its track's row;
//   * a Channel Rack pattern (a pattern index) lives on one of the rack lanes.
// Several placements of one source all follow its edits. Regions on a row never
// overlap: placing, pasting or moving a region over others overwrites them.
//
// Message-thread value type. The audio thread only ever reads an immutable copy.
#include "PianoRollSequence.h"
#include "RackModel.h"
#include "SessionCapacity.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace alien::timeline {

inline constexpr std::int64_t ticksPerStep = 240, ticksPerBeat = 960, ticksPerBar = 3840;
static_assert(ticksPerStep == PianoRollPattern::ticksPerStep, "the timeline and the piano roll share one tick");
inline constexpr int maxBars = 16384, defaultBars = 16;   // 18 h 12 min at 60 BPM, the longest song the engine and export support
inline constexpr std::int64_t maxLength = maxBars * ticksPerBar;
inline constexpr int rackLanes = 4;
inline constexpr int rackRowBase = maxSessionTracks;      // rows 0..15 are Session tracks, then the rack lanes
inline constexpr int rowCount = rackRowBase + rackLanes;
inline constexpr std::size_t maxRegions = 1024, maxMarkers = 128;
inline constexpr int maxMarkerNameBytes = 32;

inline constexpr bool isTrackRow(int row) noexcept { return row >= 0 && row < rackRowBase; }
inline constexpr bool isPatternRow(int row) noexcept { return row >= rackRowBase && row < rowCount; }

// Process-unique identity for regions, groups and markers, so a selection can
// tell "the same region" from "a different one that reused a slot" across edits
// and undo. Message thread only.
inline std::uint32_t newId() noexcept {
    static std::uint32_t next = 0;
    if (++next == 0) ++next;
    return next;
}

inline double secondsForTicks(double ticks, double bpm) noexcept {
    const double tempo = std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0;
    return ticks * 60.0 / (tempo * static_cast<double>(ticksPerBeat));
}

struct Region {
    std::uint32_t id{};
    std::uint32_t group{};            // 0: not grouped. Regions sharing a group move, copy and select together.
    int row{};                        // Session track, or rackRowBase + lane
    int source{};                     // Session scene, or Channel Rack pattern index
    std::int64_t start{}, length{ticksPerBar};
    // Where in the source the region begins (nonzero after trimming its left edge).
    // MIDI and rack patterns count ticks, audio counts seconds; both are kept
    // so a region still plays the right part if its clip is replaced by the other kind.
    std::int64_t offset{};
    double audioOffset{};
    bool muted{};
    // Non-destructive audio edits. The source file is immutable; playback and export
    // interpret these values when reading it.
    double gain{1.0};
    std::int64_t fadeIn{}, fadeOut{};
    bool loopSource{}, reversed{};
    std::int64_t end() const noexcept { return start + length; }
    bool isPattern() const noexcept { return isPatternRow(row); }
    bool operator==(const Region&) const = default;
};

struct Marker {
    std::uint32_t id{};
    std::int64_t tick{};
    std::string name;
    std::uint8_t colour{};
    bool operator==(const Marker&) const = default;
};

using Ids = std::vector<std::uint32_t>;

inline bool contains(const Ids& ids, std::uint32_t id) noexcept {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Regions copied from a timeline. Starts are relative to the earliest one.
struct Clipboard {
    std::vector<Region> regions;
    bool empty() const noexcept { return regions.empty(); }
    std::int64_t span() const noexcept {
        std::int64_t end = 0;
        for (const auto& r : regions) end = std::max(end, r.end());
        return end;
    }
    // Drops regions whose track, scene or pattern no longer exists.
    void prune(int tracks, int scenes, int patterns) {
        std::erase_if(regions, [&](const Region& r) {
            return r.isPattern() ? r.source < 0 || r.source >= patterns
                                 : r.row < 0 || r.row >= tracks || r.source < 0 || r.source >= scenes;
        });
    }
};

struct Timeline {
    int lengthBars{defaultBars};
    bool loopEnabled{false};
    std::int64_t loopStart{0}, loopEnd{4 * ticksPerBar};
    std::vector<Region> regions;
    std::vector<Marker> markers;   // ascending by tick
    // Tempo used to turn a tick distance into audio seconds when an edit trims or
    // splits an audio region. The owner keeps it equal to the song tempo; it is
    // not part of the timeline's content.
    double tempo{120.0};

    std::int64_t length() const noexcept { return static_cast<std::int64_t>(lengthBars) * ticksPerBar; }

    // ---- queries ----------------------------------------------------------------
    const Region* find(std::uint32_t id) const noexcept {
        for (const auto& r : regions) if (r.id == id) return &r;
        return nullptr;
    }
    Region* find(std::uint32_t id) noexcept {
        return const_cast<Region*>(static_cast<const Timeline&>(*this).find(id));
    }
    // The region that covers `tick` on `row`, or nullptr.
    const Region* regionAt(int row, std::int64_t tick) const noexcept {
        for (const auto& r : regions) if (r.row == row && tick >= r.start && tick < r.end()) return &r;
        return nullptr;
    }
    // Regions on rows rowLo..rowHi that overlap [from, to): what a marquee box selects.
    Ids idsIn(int rowLo, int rowHi, std::int64_t from, std::int64_t to) const {
        if (rowLo > rowHi) std::swap(rowLo, rowHi);
        if (from > to) std::swap(from, to);
        Ids ids;
        for (const auto& r : regions)
            if (r.row >= rowLo && r.row <= rowHi && r.start < to && r.end() > from) ids.push_back(r.id);
        return ids;
    }
    // `ids` plus every region grouped with any of them.
    Ids expandGroups(const Ids& ids) const {
        Ids out;
        std::vector<std::uint32_t> groups;
        for (const auto& r : regions)
            if (contains(ids, r.id) && r.group != 0) groups.push_back(r.group);
        for (const auto& r : regions)
            if (contains(ids, r.id) || (r.group != 0 && std::find(groups.begin(), groups.end(), r.group) != groups.end()))
                out.push_back(r.id);
        return out;
    }
    // End of the last region or marker: the song cannot be shortened below it.
    std::int64_t contentEnd() const noexcept {
        std::int64_t end = 0;
        for (const auto& r : regions) end = std::max(end, r.end());
        for (const auto& m : markers) end = std::max(end, m.tick);
        return end;
    }
    int minLengthBars() const noexcept {
        return static_cast<int>(std::max<std::int64_t>(1, (contentEnd() + ticksPerBar - 1) / ticksPerBar));
    }
    bool hasRegions() const noexcept { return !regions.empty(); }

    // ---- whole-timeline rules -----------------------------------------------------
    bool valid(int tracks = maxSessionTracks, int scenes = maxSessionScenes, int patterns = rack::maxPatterns) const {
        if (lengthBars < 1 || lengthBars > maxBars || regions.size() > maxRegions || markers.size() > maxMarkers) return false;
        if (loopStart < 0 || loopEnd > length() || loopEnd - loopStart < ticksPerStep) return false;
        std::vector<const Region*> order;
        order.reserve(regions.size());
        for (const auto& r : regions) {
            if (r.id == 0 || r.start < 0 || r.length < 1 || r.end() > length() || r.offset < 0
                || !std::isfinite(r.audioOffset) || r.audioOffset < 0.0 || !std::isfinite(r.gain) || r.gain < 0.0 || r.gain > 64.0
                || r.fadeIn < 0 || r.fadeOut < 0 || r.fadeIn > r.length || r.fadeOut > r.length || r.source < 0) return false;
            if (r.isPattern()) { if (r.source >= patterns) return false; }
            else if (r.row < 0 || r.row >= tracks || r.source >= scenes) return false;
            order.push_back(&r);
        }
        std::sort(order.begin(), order.end(), [](const Region* a, const Region* b) {
            return a->row != b->row ? a->row < b->row : a->start < b->start;
        });
        for (std::size_t i = 1; i < order.size(); ++i)
            if (order[i]->row == order[i - 1]->row && order[i]->start < order[i - 1]->end()) return false;
        std::vector<std::uint32_t> ids;
        for (const auto& r : regions) ids.push_back(r.id);
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
        ids.clear();
        std::int64_t previous = 0;
        for (const auto& m : markers) {
            if (m.id == 0 || m.tick < 0 || m.tick > length() || m.tick < previous
                || m.name.size() > static_cast<std::size_t>(maxMarkerNameBytes)) return false;
            previous = m.tick;
            ids.push_back(m.id);
        }
        std::sort(ids.begin(), ids.end());
        return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    }

    // Same song, whatever the tempo bookkeeping.
    bool operator==(const Timeline& o) const noexcept {
        return lengthBars == o.lengthBars && loopEnabled == o.loopEnabled && loopStart == o.loopStart
            && loopEnd == o.loopEnd && regions == o.regions && markers == o.markers;
    }
    // Would the audio engine play these two identically? Names, colours, ids and
    // groups do not change the sound, so editing them must not republish the song.
    bool playbackEquals(const Timeline& o) const noexcept {
        if (lengthBars != o.lengthBars || loopEnabled != o.loopEnabled || loopStart != o.loopStart
            || loopEnd != o.loopEnd || regions.size() != o.regions.size()) return false;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const auto& a = regions[i]; const auto& b = o.regions[i];
            if (a.row != b.row || a.source != b.source || a.start != b.start || a.length != b.length
                || a.offset != b.offset || a.audioOffset != b.audioOffset || a.gain != b.gain
                || a.fadeIn != b.fadeIn || a.fadeOut != b.fadeOut || a.loopSource != b.loopSource
                || a.reversed != b.reversed || a.muted != b.muted) return false;
        }
        return true;
    }

    // ---- placing and editing regions ------------------------------------------------
    // Places a new region, overwriting whatever it lands on. Returns false (and
    // changes nothing) when the placement is not valid.
    bool place(Region r, std::uint32_t* idOut = nullptr) {
        Timeline next = *this;
        r.id = newId();
        if (r.length < 1 || r.start < 0 || !next.ensureLength(r.end())) return false;
        next.carve(r.row, r.start, r.end());
        next.regions.push_back(r);
        if (!adopt(std::move(next))) return false;
        if (idOut != nullptr) *idOut = r.id;
        return true;
    }
    // A whole-bar placement of a Session clip, as the old bar-based arrangement made them.
    bool placeBars(int track, int scene, int startBar, int lengthBarCount, std::uint32_t* idOut = nullptr) {
        Region r;
        r.row = track; r.source = scene;
        r.start = static_cast<std::int64_t>(startBar) * ticksPerBar;
        r.length = static_cast<std::int64_t>(lengthBarCount) * ticksPerBar;
        return place(r, idOut);
    }

    // Moves regions by a number of ticks and, for rack patterns, a number of lanes.
    // Session clips stay on their own track's row: a clip belongs to its track.
    // Nothing moves before the start. What the regions land on is overwritten.
    bool move(const Ids& ids, std::int64_t deltaTicks, int deltaRows = 0, Ids* moved = nullptr) {
        Timeline next = *this;
        std::vector<Region> carried;
        for (const auto& r : next.regions) if (contains(ids, r.id)) carried.push_back(r);
        if (carried.empty()) return false;
        std::int64_t minStart = maxLength, maxEnd = 0;
        int minLane = rowCount, maxLane = -1;
        for (const auto& r : carried) {
            minStart = std::min(minStart, r.start); maxEnd = std::max(maxEnd, r.end());
            if (r.isPattern()) { minLane = std::min(minLane, r.row); maxLane = std::max(maxLane, r.row); }
        }
        deltaTicks = std::clamp(deltaTicks, -minStart, maxLength - maxEnd);
        deltaRows = maxLane < 0 ? 0 : std::clamp(deltaRows, rackRowBase - minLane, rowCount - 1 - maxLane);
        if (deltaTicks == 0 && deltaRows == 0) return false;
        std::erase_if(next.regions, [&](const Region& r) { return contains(ids, r.id); });
        next.ensureLength(maxEnd + deltaTicks);
        for (auto& r : carried) {
            r.start += deltaTicks;
            if (r.isPattern()) r.row += deltaRows;
            next.carve(r.row, r.start, r.end());
        }
        for (const auto& r : carried) next.regions.push_back(r);
        if (!adopt(std::move(next))) return false;
        if (moved != nullptr) for (const auto& r : carried) moved->push_back(r.id);
        return true;
    }

    // Moves the left edge of one region, revealing or hiding the start of its
    // source. It cannot pass the region before it, its own end, or the start of
    // the source (there is nothing before that to reveal).
    bool trimStart(std::uint32_t id, std::int64_t newStart) {
        Timeline next = *this;
        Region* r = next.find(id);
        if (r == nullptr) return false;
        // Source that lies before the region's start, in ticks, for MIDI and for audio alike.
        const auto audioRoom = static_cast<std::int64_t>(std::floor(r->audioOffset / secondsForTicks(1, tempo) + 1e-9));
        std::int64_t lowest = std::max<std::int64_t>(0, r->start - std::min(r->offset, audioRoom));
        for (const auto& o : next.regions)
            if (o.id != id && o.row == r->row && o.end() <= r->start) lowest = std::max(lowest, o.end());
        newStart = std::clamp(newStart, lowest, r->end() - 1);
        if (newStart == r->start) return false;
        next.trimLeftBy(*r, newStart - r->start);
        return adopt(std::move(next));
    }
    // Moves the right edge of one region. It cannot pass the next region on its row.
    bool trimEnd(std::uint32_t id, std::int64_t newEnd) {
        Timeline next = *this;
        Region* r = next.find(id);
        if (r == nullptr) return false;
        std::int64_t highest = maxLength;
        for (const auto& o : next.regions)
            if (o.id != id && o.row == r->row && o.start >= r->end()) highest = std::min(highest, o.start);
        newEnd = std::clamp(newEnd, r->start + 1, highest);
        if (newEnd == r->end()) return false;
        r->length = newEnd - r->start;
        next.ensureLength(newEnd);
        return adopt(std::move(next));
    }

    // Cuts every listed region that spans `tick` in two. The new right halves are reported.
    bool split(const Ids& ids, std::int64_t tick, Ids* created = nullptr) {
        Timeline next = *this;
        std::vector<Region> rights;
        for (auto& r : next.regions) {
            if (!contains(ids, r.id) || tick <= r.start || tick >= r.end()) continue;
            Region right = r;
            right.id = newId();
            next.trimLeftBy(right, tick - r.start);
            r.length = tick - r.start;
            rights.push_back(right);
        }
        if (rights.empty()) return false;
        for (const auto& r : rights) next.regions.push_back(r);
        if (!adopt(std::move(next))) return false;
        if (created != nullptr) for (const auto& r : rights) created->push_back(r.id);
        return true;
    }

    // Copies the listed regions to just after the end of the selection, as one unit.
    bool duplicate(const Ids& ids, Ids* created = nullptr) {
        const auto clip = copy(ids);
        if (clip.empty()) return false;
        std::int64_t first = maxLength;
        for (const auto& r : regions) if (contains(ids, r.id)) first = std::min(first, r.start);
        return paste(clip, first + clip.span(), 0, created);
    }

    Clipboard copy(const Ids& ids) const {
        Clipboard clip;
        std::int64_t first = maxLength;
        for (const auto& r : regions) if (contains(ids, r.id)) first = std::min(first, r.start);
        for (const auto& r : regions) {
            if (!contains(ids, r.id)) continue;
            Region c = r;
            c.id = 0; c.start -= first;
            clip.regions.push_back(c);
        }
        return clip;
    }
    // Pastes at `at`, moving rack patterns by `laneDelta` lanes. Copies get new
    // groups of their own. What they land on is overwritten.
    bool paste(const Clipboard& clip, std::int64_t at, int laneDelta = 0, Ids* created = nullptr) {
        if (clip.empty() || at < 0) return false;
        Timeline next = *this;
        std::vector<Region> fresh;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> groupMap;
        for (auto r : clip.regions) {
            r.id = newId();
            r.start += at;
            if (r.isPattern()) {
                r.row += laneDelta;
                if (!isPatternRow(r.row)) return false;
            }
            if (r.group != 0) {
                auto it = std::find_if(groupMap.begin(), groupMap.end(), [&](const auto& p) { return p.first == r.group; });
                if (it == groupMap.end()) { groupMap.emplace_back(r.group, newId()); it = groupMap.end() - 1; }
                r.group = it->second;
            }
            if (!next.ensureLength(r.end())) return false;
            fresh.push_back(r);
        }
        for (const auto& r : fresh) next.carve(r.row, r.start, r.end());
        for (const auto& r : fresh) next.regions.push_back(r);
        if (!adopt(std::move(next))) return false;
        if (created != nullptr) for (const auto& r : fresh) created->push_back(r.id);
        return true;
    }

    bool remove(const Ids& ids) {
        Timeline next = *this;
        const auto before = next.regions.size();
        std::erase_if(next.regions, [&](const Region& r) { return contains(ids, r.id); });
        if (next.regions.size() == before) return false;
        return adopt(std::move(next));
    }
    // Moves the source underneath the region without moving either edit boundary.
    // Positive values move later into the source. Audio offsets follow the project tempo.
    bool slip(const Ids& ids, std::int64_t deltaTicks) {
        if (deltaTicks == 0) return false;
        Timeline next = *this;
        bool changed = false;
        for (auto& r : next.regions) if (contains(ids, r.id)) {
            const auto applied = std::max(deltaTicks, -r.offset);
            if (applied == 0) continue;
            r.offset += applied;
            r.audioOffset = std::max(0.0, r.audioOffset + secondsForTicks(applied, tempo));
            changed = true;
        }
        return changed && adopt(std::move(next));
    }
    bool setLoopSource(const Ids& ids, bool looped) { return editAudio(ids, [=](Region& r) { if (r.loopSource == looped) return false; r.loopSource = looped; return true; }); }
    bool setReversed(const Ids& ids, bool reversed) { return editAudio(ids, [=](Region& r) { if (r.reversed == reversed) return false; r.reversed = reversed; return true; }); }
    bool setGain(const Ids& ids, double gain) {
        if (!std::isfinite(gain) || gain < 0.0 || gain > 64.0) return false;
        return editAudio(ids, [=](Region& r) { if (std::abs(r.gain - gain) < 1.0e-12) return false; r.gain = gain; return true; });
    }
    bool setFades(const Ids& ids, std::int64_t fadeIn, std::int64_t fadeOut) {
        if (fadeIn < 0 || fadeOut < 0) return false;
        return editAudio(ids, [=](Region& r) { const auto in = std::min(fadeIn, r.length), out = std::min(fadeOut, r.length); if (r.fadeIn == in && r.fadeOut == out) return false; r.fadeIn = in; r.fadeOut = out; return true; });
    }
    // Applies complementary fades to two touching clips. Their boundaries stay fixed,
    // which keeps the timeline deterministic and the operation fully reversible.
    bool crossfade(std::uint32_t leftId, std::uint32_t rightId, std::int64_t ticks) {
        if (ticks <= 0) return false;
        Timeline next = *this;
        auto* left = next.find(leftId); auto* right = next.find(rightId);
        if (left == nullptr || right == nullptr || left->row != right->row || left->end() != right->start) return false;
        ticks = std::min({ticks, left->length, right->length});
        if (left->fadeOut == ticks && right->fadeIn == ticks) return false;
        left->fadeOut = ticks; right->fadeIn = ticks;
        return adopt(std::move(next));
    }

    bool setMuted(const Ids& ids, bool muted) {
        Timeline next = *this;
        bool changed = false;
        for (auto& r : next.regions) if (contains(ids, r.id) && r.muted != muted) { r.muted = muted; changed = true; }
        return changed && adopt(std::move(next));
    }
    // Ties the listed regions (and everything already grouped with them) together.
    bool group(const Ids& ids) {
        Timeline next = *this;
        const Ids all = next.expandGroups(ids);
        if (all.size() < 2) return false;
        const auto g = newId();
        for (auto& r : next.regions) if (contains(all, r.id)) r.group = g;
        return adopt(std::move(next));
    }
    bool ungroup(const Ids& ids) {
        Timeline next = *this;
        bool changed = false;
        for (const auto id : next.expandGroups(ids)) {
            auto* r = next.find(id);
            if (r != nullptr && r->group != 0) { r->group = 0; changed = true; }
        }
        return changed && adopt(std::move(next));
    }

    // ---- song length, loop and markers --------------------------------------------------
    bool setLengthBars(int bars) {
        bars = std::clamp(bars, minLengthBars(), maxBars);
        if (bars == lengthBars) return false;
        Timeline next = *this;
        next.lengthBars = bars;
        if (next.loopEnd > next.length()) {
            next.loopEnd = next.length();
            if (next.loopEnd - next.loopStart < ticksPerStep) next.loopStart = 0;
        }
        return adopt(std::move(next));
    }
    bool setLoop(std::int64_t start, std::int64_t end) {
        if (end < start) std::swap(start, end);
        start = std::clamp<std::int64_t>(start, 0, length());
        end = std::clamp<std::int64_t>(end, 0, length());
        if (end - start < ticksPerStep || (start == loopStart && end == loopEnd)) return false;
        loopStart = start; loopEnd = end;
        return true;
    }
    bool setLoopEnabled(bool on) {
        if (loopEnabled == on) return false;
        loopEnabled = on; return true;
    }

    // Returns the new marker's id, or 0 when there is no room or the tick is off the timeline.
    std::uint32_t addMarker(std::int64_t tick, std::string name, std::uint8_t colour = 0) {
        if (markers.size() >= maxMarkers || tick < 0 || tick > length()) return 0;
        Marker m{newId(), tick, cleanName(std::move(name)), colour};
        markers.insert(std::upper_bound(markers.begin(), markers.end(), tick,
            [](std::int64_t t, const Marker& other) { return t < other.tick; }), m);
        return m.id;
    }
    bool moveMarker(std::uint32_t id, std::int64_t tick) {
        auto it = std::find_if(markers.begin(), markers.end(), [&](const Marker& m) { return m.id == id; });
        if (it == markers.end()) return false;
        tick = std::clamp<std::int64_t>(tick, 0, length());
        if (tick == it->tick) return false;
        Marker m = *it;
        markers.erase(it);
        m.tick = tick;
        markers.insert(std::upper_bound(markers.begin(), markers.end(), tick,
            [](std::int64_t t, const Marker& other) { return t < other.tick; }), m);
        return true;
    }
    bool renameMarker(std::uint32_t id, std::string name) {
        for (auto& m : markers) if (m.id == id) {
            name = cleanName(std::move(name));
            if (name == m.name) return false;
            m.name = std::move(name); return true;
        }
        return false;
    }
    bool colourMarker(std::uint32_t id, std::uint8_t colour) {
        for (auto& m : markers) if (m.id == id && m.colour != colour) { m.colour = colour; return true; }
        return false;
    }
    bool removeMarker(std::uint32_t id) {
        const auto before = markers.size();
        std::erase_if(markers, [&](const Marker& m) { return m.id == id; });
        return markers.size() != before;
    }
    const Marker* markerAfter(std::int64_t tick) const noexcept {
        for (const auto& m : markers) if (m.tick > tick) return &m;
        return nullptr;
    }
    const Marker* markerBefore(std::int64_t tick) const noexcept {
        const Marker* found = nullptr;
        for (const auto& m : markers) if (m.tick < tick) found = &m;
        return found;
    }

    // ---- keeping references right when the project changes shape ----------------------------
    void trackDeleted(int track) {
        std::erase_if(regions, [&](const Region& r) { return !r.isPattern() && r.row == track; });
        for (auto& r : regions) if (!r.isPattern() && r.row > track) --r.row;
        tidyGroups();
    }
    void tracksSwapped(int a, int b) {
        for (auto& r : regions) if (!r.isPattern()) { if (r.row == a) r.row = b; else if (r.row == b) r.row = a; }
    }
    void sceneDeleted(int scene) {
        std::erase_if(regions, [&](const Region& r) { return !r.isPattern() && r.source == scene; });
        for (auto& r : regions) if (!r.isPattern() && r.source > scene) --r.source;
        tidyGroups();
    }
    void scenesSwapped(int a, int b) {
        for (auto& r : regions) if (!r.isPattern()) { if (r.source == a) r.source = b; else if (r.source == b) r.source = a; }
    }
    void sceneInserted(int index) {
        for (auto& r : regions) if (!r.isPattern() && r.source >= index) ++r.source;
    }
    void patternDeleted(int index) {
        std::erase_if(regions, [&](const Region& r) { return r.isPattern() && r.source == index; });
        for (auto& r : regions) if (r.isPattern() && r.source > index) --r.source;
        tidyGroups();
    }
    // Drops regions that point at a track, scene or pattern that does not exist.
    void prune(int tracks, int scenes, int patterns) {
        std::erase_if(regions, [&](const Region& r) {
            return r.isPattern() ? r.source < 0 || r.source >= patterns
                                 : r.row < 0 || r.row >= tracks || r.source < 0 || r.source >= scenes;
        });
        tidyGroups();
    }

    // A group of one is not a group.
    void tidyGroups() {
        for (auto& r : regions) {
            if (r.group == 0) continue;
            int members = 0;
            for (const auto& o : regions) members += o.group == r.group ? 1 : 0;
            if (members < 2) r.group = 0;
        }
    }

private:
    template <typename Edit>
    bool editAudio(const Ids& ids, Edit&& edit) {
        Timeline next = *this; bool changed = false;
        for (auto& r : next.regions) if (!r.isPattern() && contains(ids, r.id)) changed = edit(r) || changed;
        return changed && adopt(std::move(next));
    }
    static std::string cleanName(std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        if (text.size() > static_cast<std::size_t>(maxMarkerNameBytes)) {
            std::size_t cut = static_cast<std::size_t>(maxMarkerNameBytes);
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;   // never split a UTF-8 character
            text.resize(cut);
        }
        return text;
    }
    bool ensureLength(std::int64_t endTick) noexcept {
        if (endTick > maxLength) return false;
        lengthBars = std::max(lengthBars, static_cast<int>((endTick + ticksPerBar - 1) / ticksPerBar));
        return true;
    }
    // Takes the left `delta` ticks off a region (a negative delta gives them back).
    void trimLeftBy(Region& r, std::int64_t delta) const noexcept {
        r.start += delta; r.length -= delta; r.offset = std::max<std::int64_t>(0, r.offset + delta);
        r.audioOffset = std::max(0.0, r.audioOffset + secondsForTicks(delta, tempo));
    }
    // Clears [from, to) on a row: regions inside it go, regions that overhang are
    // trimmed, and a region that spans it is cut in two around it.
    void carve(int row, std::int64_t from, std::int64_t to) {
        std::vector<Region> halves;
        for (std::size_t i = 0; i < regions.size();) {
            Region& r = regions[i];
            if (r.row != row || r.end() <= from || r.start >= to) { ++i; continue; }
            if (r.start >= from && r.end() <= to) { regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
            if (r.start < from && r.end() > to) {
                Region right = r;
                right.id = newId();
                trimLeftBy(right, to - r.start);
                r.length = from - r.start;
                halves.push_back(right);
            } else if (r.start < from) {
                r.length = from - r.start;
            } else {
                trimLeftBy(r, to - r.start);
            }
            ++i;
        }
        for (const auto& h : halves) regions.push_back(h);
    }
    // Commits `next` if it is a valid timeline.
    bool adopt(Timeline&& next) {
        next.tidyGroups();
        if (!next.valid()) return false;
        *this = std::move(next);
        return true;
    }
};

} // namespace alien::timeline
