#pragma once
// The project's clip library (T1-03, increment A; see docs/SHARED_CLIP_MODEL.md).
//
// Musical content lives here once, as immutable pattern revisions, and every use of it
// (a launch slot, an arrangement placement) refers to a clip by id. A clip points at a
// pattern; several clips may point at one pattern (a linked duplicate) and then share every
// edit, while an ordinary duplicate gets a pattern of its own. Identity is never inferred
// from equal content or from a shared pointer: two patterns holding the same notes, or the
// same immutable empty payload, are still two patterns.
//
// This increment is additive: nothing in the app reads or writes the library yet, and the
// Session, Song and Channel Rack models stay authoritative. It has no JUCE and no audio code.
// Message-thread value type: copying it is cheap (payloads are shared and immutable), so a
// copy serves as an undo snapshot and every operation either succeeds completely or leaves
// the library exactly as it was.
#include "PianoRollSequence.h"
#include "RackModel.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace alien::library {

// ---- ids ------------------------------------------------------------------------------------------------
// A nonzero, project-scoped 64-bit id of one kind. Kinds do not mix: a PatternId is never a ClipId.
template <class Kind>
struct Id {
    std::uint64_t value{};
    explicit operator bool() const noexcept { return value != 0; }
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};
struct PatternKind {}; struct ClipKind {}; struct LaneKind {}; struct MediaKind {};
using PatternId = Id<PatternKind>;
using ClipId = Id<ClipKind>;
using LaneId = Id<LaneKind>;      // unique within its pattern
using MediaId = Id<MediaKind>;

// Saved as exactly 16 lower-case hex digits.
inline std::string toHex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4) text[static_cast<std::size_t>(i)] = digits[value & 0xF];
    return text;
}
template <class Kind> std::string toHex(Id<Kind> id) { return toHex(id.value); }
// Accepts exactly 16 hex digits (either case) for a nonzero value.
template <class Kind>
std::optional<Id<Kind>> parseId(std::string_view text) noexcept {
    if (text.size() != 16) return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text) {
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        value = value << 4 | static_cast<std::uint64_t>(digit);
    }
    if (value == 0) return std::nullopt;
    return Id<Kind>{value};
}

// ---- content --------------------------------------------------------------------------------------------
// A note sequence: a Session MIDI clip's notes. Pitches are absolute.
struct NoteLane {
    std::shared_ptr<const PianoRollPattern> notes;
};
// One Channel Rack channel's row, kept losslessly: every step with all its values (including
// steps that are off or past the pattern's length) and, when the row plays notes, its note
// sequence, which is then authoritative while the steps stay dormant (as in rack::Pattern).
struct StepLane {
    std::array<rack::Step, rack::maxSteps> steps{};
    std::shared_ptr<const PianoRollPattern> notes;
};
// Recorded or imported audio. Where in the file a use starts is a placement property, not content.
struct AudioLane {
    MediaId media;
};
struct PatternLane {
    LaneId id;
    std::variant<NoteLane, StepLane, AudioLane> content;
};
// One revision of a pattern's content. Immutable once shared.
struct PatternData {
    std::int64_t lengthTicks{16 * PianoRollPattern::ticksPerStep};
    std::vector<PatternLane> lanes;

    static constexpr std::int64_t maxLengthTicks = static_cast<std::int64_t>(PianoRollPattern::maxLoopSteps) * PianoRollPattern::ticksPerStep;
    static constexpr std::size_t maxLanes = static_cast<std::size_t>(rack::maxChannels);
    bool valid() const noexcept {
        if (lengthTicks < PianoRollPattern::ticksPerStep || lengthTicks > maxLengthTicks || lanes.size() > maxLanes) return false;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto& lane = lanes[i];
            if (!lane.id) return false;
            for (std::size_t j = 0; j < i; ++j) if (lanes[j].id == lane.id) return false;
            if (const auto* n = std::get_if<NoteLane>(&lane.content)) {
                if (!n->notes || !n->notes->valid() || loopTicks(*n->notes) != lengthTicks) return false;
            } else if (const auto* s = std::get_if<StepLane>(&lane.content)) {
                const auto steps = lengthTicks / PianoRollPattern::ticksPerStep;
                if (lengthTicks % PianoRollPattern::ticksPerStep != 0 || !rack::validLength(static_cast<int>(steps))) return false;
                for (const auto& step : s->steps) if (!step.valid()) return false;
                if (s->notes && (!s->notes->valid() || s->notes->loopSteps != steps)) return false;
            } else if (!std::get<AudioLane>(lane.content).media) return false;
        }
        return true;
    }
    static std::int64_t loopTicks(const PianoRollPattern& p) noexcept {
        return static_cast<std::int64_t>(p.loopSteps) * PianoRollPattern::ticksPerStep;
    }
};
// One shared, immutable empty payload. Patterns created from it are still separate patterns.
inline std::shared_ptr<const PatternData> emptyPayload() {
    static const auto empty = std::make_shared<const PatternData>();
    return empty;
}

// ---- the library ----------------------------------------------------------------------------------------
struct PatternRecord {
    PatternId id;
    std::uint64_t revision{1};                    // grows with every content edit; it is not a new identity
    std::shared_ptr<const PatternData> data;
    bool operator==(const PatternRecord& o) const noexcept { return id == o.id && revision == o.revision && data == o.data; }
};
struct ClipInfo {
    std::string name;
    std::uint8_t colour{};
    bool operator==(const ClipInfo&) const = default;
};
struct ClipDefinition {
    ClipId id;
    PatternId pattern;
    ClipInfo info;
    bool operator==(const ClipDefinition&) const = default;
};

enum class EditResult { done, unchanged, stale, invalid, missing };

class Library {
public:
    // ---- queries ----
    const PatternRecord* pattern(PatternId id) const noexcept { return find(patterns, id.value); }
    const ClipDefinition* clip(ClipId id) const noexcept { return find(clips, id.value); }
    // The content a clip plays now, or null.
    std::shared_ptr<const PatternData> content(ClipId id) const noexcept {
        const auto* c = clip(id);
        const auto* p = c != nullptr ? pattern(c->pattern) : nullptr;
        return p != nullptr ? p->data : nullptr;
    }
    std::size_t patternCount() const noexcept { return patterns.size(); }
    std::size_t clipCount() const noexcept { return clips.size(); }
    // How many clips use a pattern: more than one means they are linked.
    std::size_t usesOf(PatternId id) const noexcept {
        return static_cast<std::size_t>(std::count_if(clips.begin(), clips.end(), [&](const auto& c) { return c.second.pattern == id; }));
    }
    bool linked(ClipId a, ClipId b) const noexcept {
        const auto* x = clip(a); const auto* y = clip(b);
        return x != nullptr && y != nullptr && a != b && x->pattern == y->pattern;
    }

    // ---- creating ----
    // A new pattern holding `data` (which may be shared with other patterns: it is immutable).
    PatternId addPattern(std::shared_ptr<const PatternData> data) {
        if (!data || !data->valid()) return {};
        const PatternId id{next()};
        patterns.emplace(id.value, PatternRecord{id, 1, std::move(data)});
        return id;
    }
    // A new clip using an existing pattern.
    ClipId addClip(PatternId pattern, ClipInfo info = {}) {
        if (this->pattern(pattern) == nullptr) return {};
        const ClipId id{next()};
        clips.emplace(id.value, ClipDefinition{id, pattern, std::move(info)});
        return id;
    }
    // A new pattern and a clip that uses it.
    ClipId addClip(std::shared_ptr<const PatternData> data, ClipInfo info = {}) {
        if (!data || !data->valid()) return {};
        return addClip(addPattern(std::move(data)), std::move(info));
    }

    // ---- editing ----
    // Replaces a pattern's content, as one new revision, if nobody has changed it since
    // `expectedRevision` was read. Every clip using the pattern plays the new content.
    EditResult editPattern(PatternId id, std::uint64_t expectedRevision, std::shared_ptr<const PatternData> data) {
        const auto it = patterns.find(id.value);
        if (it == patterns.end()) return EditResult::missing;
        if (it->second.revision != expectedRevision) return EditResult::stale;
        if (!data || !data->valid()) return EditResult::invalid;
        if (data == it->second.data) return EditResult::unchanged;
        it->second.data = std::move(data);
        ++it->second.revision;
        return EditResult::done;
    }
    bool setClipInfo(ClipId id, ClipInfo info) {
        const auto it = clips.find(id.value);
        if (it == clips.end() || it->second.info == info) return false;
        it->second.info = std::move(info);
        return true;
    }

    // ---- copying, linking and forking ----
    // An independent copy: a new clip with a new pattern. The two start with the same immutable
    // payload and part ways at the first edit of either.
    ClipId duplicateClip(ClipId id) {
        const auto* source = clip(id);
        if (source == nullptr) return {};
        const auto* p = pattern(source->pattern);
        if (p == nullptr) return {};
        const auto info = source->info;
        const auto data = p->data;
        const PatternId copy{next()};
        patterns.emplace(copy.value, PatternRecord{copy, 1, data});
        return addClip(copy, info);
    }
    // A linked copy: a new clip using the same pattern, so content edits reach both.
    ClipId linkClip(ClipId id) {
        const auto* source = clip(id);
        if (source == nullptr) return {};
        const auto pattern = source->pattern;
        const auto info = source->info;
        return addClip(pattern, info);
    }
    // Gives a linked clip a pattern of its own holding its current content. Every use of the
    // clip follows; the other clips keep the old pattern. A clip that shares nothing is already
    // unique, and nothing changes.
    bool makeClipUnique(ClipId id) {
        const auto it = clips.find(id.value);
        if (it == clips.end() || usesOf(it->second.pattern) < 2) return false;
        const auto* p = pattern(it->second.pattern);
        if (p == nullptr) return false;
        const PatternId fresh{next()};
        patterns.emplace(fresh.value, PatternRecord{fresh, 1, p->data});
        it->second.pattern = fresh;
        return true;
    }

    // ---- removing ----
    // Removes a clip. Its pattern stays until pruned (undo snapshots and other clips may need it).
    bool removeClip(ClipId id) { return clips.erase(id.value) != 0; }
    // Keeps the clips in `roots` (everything that uses one: launch slots, placements, takes,
    // frozen originals, open editors) and the patterns they use; removes the rest. Unknown ids
    // in `roots` are ignored. Returns how many clips and patterns were removed.
    std::size_t prune(const std::vector<ClipId>& roots) {
        std::size_t removed = 0;
        for (auto it = clips.begin(); it != clips.end();) {
            if (std::find(roots.begin(), roots.end(), it->second.id) == roots.end()) { it = clips.erase(it); ++removed; }
            else ++it;
        }
        for (auto it = patterns.begin(); it != patterns.end();) {
            if (usesOf(it->second.id) == 0) { it = patterns.erase(it); ++removed; }
            else ++it;
        }
        return removed;
    }

    // ---- checking ----
    // Every id nonzero and stored under itself, every clip's pattern present, every payload valid,
    // and the id counter past every id in use (so a new id can never repeat one).
    bool valid() const noexcept {
        for (const auto& [key, p] : patterns)
            if (!p.id || key != p.id.value || p.revision == 0 || !p.data || !p.data->valid() || key >= nextId) return false;
        for (const auto& [key, c] : clips)
            if (!c.id || key != c.id.value || pattern(c.pattern) == nullptr || key >= nextId) return false;
        for (const auto& [key, p] : patterns) if (clips.count(key) != 0) return false;   // one counter: kinds never share a value
        return true;
    }
    bool operator==(const Library&) const = default;

private:
    template <class Map>
    static const typename Map::mapped_type* find(const Map& map, std::uint64_t key) noexcept {
        const auto it = map.find(key);
        return it != map.end() ? &it->second : nullptr;
    }
    std::uint64_t next() noexcept { return nextId++; }

    std::map<std::uint64_t, PatternRecord> patterns;
    std::map<std::uint64_t, ClipDefinition> clips;
    std::uint64_t nextId{1};
};

} // namespace alien::library
