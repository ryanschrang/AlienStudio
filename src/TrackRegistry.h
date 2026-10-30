#pragma once
// Message-thread track identities. Slots are adapters, never identities. The
// registry owns identity only; devices, media and controls stay in their existing
// document stages until the playback-adapter migration (T1-04 C).
#include <algorithm>
#include <cstdint>
#include <compare>
#include <utility>
#include <limits>
#include <optional>
#include <vector>

namespace alien {
struct TrackKind {};
struct TrackId {
    std::uint64_t value{};
    explicit operator bool() const noexcept { return value != 0; }
    bool operator==(const TrackId&) const = default;
    auto operator<=>(const TrackId&) const = default;
};
enum class TrackRole { session, rack, master, returnTrack, bus };
struct TrackBinding {
    TrackRole role{};
    std::uint64_t key{}; // document-local owner token; file adapters use slot + 1
    bool operator==(const TrackBinding&) const = default;
};
struct TrackEntry {
    TrackId id;
    TrackBinding binding;
    bool operator==(const TrackEntry&) const = default;
};
class TrackRegistry {
public:
    const std::vector<TrackEntry>& entries() const noexcept { return tracks; }
    std::uint64_t nextId() const noexcept { return next; }
    bool canAllocate() const noexcept { return next != std::numeric_limits<std::uint64_t>::max(); }
    TrackId idFor(TrackBinding binding) const noexcept {
        for (const auto& entry : tracks) if (entry.binding == binding) return entry.id;
        return {};
    }
    std::optional<TrackBinding> resolve(TrackId id) const noexcept {
        for (const auto& entry : tracks) if (entry.id == id) return entry.binding;
        return std::nullopt;
    }
    bool valid() const noexcept {
        if (next == 0) return false;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto& e = tracks[i];
            if (!e.id || e.id.value >= next || e.binding.key == 0
                || e.binding.role < TrackRole::session || e.binding.role > TrackRole::bus) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (tracks[j].id == e.id || tracks[j].binding == e.binding) return false;
        }
        return true;
    }
    // A single transaction: preserve surviving owners, allocate new identities,
    // and retire deleted owners. Exhaustion/duplicate bindings leave us intact.
    bool synchronize(const std::vector<TrackBinding>& owners) {
        if (!valid()) return false;
        auto candidate = *this;
        candidate.tracks.clear();
        for (auto owner : owners) {
            auto id = idFor(owner);
            if (!id) {
                if (candidate.next == std::numeric_limits<std::uint64_t>::max()) return false;
                id = TrackId{candidate.next++};
            }
            candidate.tracks.push_back({id, owner});
        }
        if (!candidate.valid()) return false;
        *this = std::move(candidate);
        return true;
    }
    // Import identities into an explicitly supplied set of owner adapters.
    bool restore(std::vector<TrackEntry> entries, std::uint64_t nextId) {
        TrackRegistry candidate;
        candidate.tracks = std::move(entries); candidate.next = nextId;
        if (!candidate.valid()) return false;
        *this = std::move(candidate); return true;
    }
    void retainIssuedIdsFrom(const TrackRegistry& other) noexcept { next = std::max(next, other.next); }
    bool sameOwners(const TrackRegistry& other) const noexcept { return tracks == other.tracks; }
private:
    std::vector<TrackEntry> tracks;
    std::uint64_t next{1};
};
} // namespace alien
