#pragma once
#include "RetiringMailbox.h"
#include "SessionAudioClip.h"
#include "SessionCapacity.h"
#include "Timeline.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace alien {
// One Session clip as the song plays it: its notes (shared with the document, never
// copied) and its decoded audio, or nothing.
struct ArrangementClip {
    SharedPianoRoll pattern;
    const SessionAudioClip* audio{};
};

// Silence, for a region whose placement clip is missing (never in a validated snapshot).
inline const ArrangementClip silentArrangementClip{};

// A placement clip as the song plays it (Region::clip): MIDI only.
struct ArrangementPlacement {
    std::uint64_t clip{};
    ArrangementClip content;
};

// What the audio thread plays in the Arrangement view: the timeline (where
// everything sits) plus every Session clip and placement clip it can refer to.
// Clips point at shared, immutable notes, so a snapshot is a few KiB however
// many notes the song holds.
struct ArrangementPlayback {
    alien::timeline::Timeline timeline;
    std::array<std::array<ArrangementClip, maxSessionTracks>, maxSessionScenes> clips{};
    std::vector<ArrangementPlacement> placements;   // ascending by clip id
    struct WarpedRegion {
        std::uint32_t id{};
        std::shared_ptr<const SessionAudioClip> owner;
        ArrangementClip content;
        WarpedRegion(std::uint32_t region, std::shared_ptr<const SessionAudioClip> clip) : id(region), owner(std::move(clip)), content{{},owner.get()} {}
    };
    std::vector<WarpedRegion> warped; // playback-only cache ownership
    // What a track's region plays. Audio thread safe: a binary search, no allocation.
    const ArrangementClip& clipFor(const alien::timeline::Region& region, std::size_t track) const noexcept {
        const auto warp = std::lower_bound(warped.begin(), warped.end(), region.id, [](const WarpedRegion& w, std::uint32_t id) { return w.id < id; });
        if (warp != warped.end() && warp->id == region.id) return warp->content;
        if (region.clip == 0) return clips[static_cast<std::size_t>(region.source)][track];
        const auto found = std::lower_bound(placements.begin(), placements.end(), region.clip,
                                            [](const ArrangementPlacement& p, std::uint64_t id) { return p.clip < id; });
        return found != placements.end() && found->clip == region.clip ? found->content : silentArrangementClip;
    }
};

// Hands complete, immutable song snapshots from the message thread to the audio
// thread (a RetiringMailbox): only a pointer ever crosses threads, and an old
// snapshot (and with it any clip notes nothing else holds) is released on the
// message thread once the audio thread has begun a block after leaving it.
class ArrangementMailbox {
public:
    ArrangementMailbox() : box(std::make_shared<const ArrangementPlayback>()) {}
    static bool validSnapshot(const ArrangementPlayback& value) {
        if (!value.timeline.valid()) return false;
        for (const auto& r : value.timeline.regions) if (!r.warp.empty()) return false; // prepare worker caches before publication
        for (std::size_t i=0;i<value.warped.size();++i) {
            const auto& w=value.warped[i];
            if (!w.owner || w.content.audio!=w.owner.get() || !w.owner->valid() || w.id==0
                || (i && value.warped[i-1].id>=w.id)) return false;
        }
        for (const auto& scene : value.clips) for (const auto& clip : scene)
            if (!clip.pattern->valid()) return false;
        for (std::size_t i = 0; i < value.placements.size(); ++i) {
            const auto& p = value.placements[i];
            if (p.clip == 0 || p.content.audio != nullptr || !p.content.pattern->valid()
                || (i > 0 && value.placements[i - 1].clip >= p.clip)) return false;
        }
        for (const auto& r : value.timeline.regions)
            if (r.clip != 0 && &value.clipFor(r, 0) == &silentArrangementClip) return false;   // every placement clip is there
        return true;
    }
    // Message thread. Copies the value into a fresh snapshot.
    bool publish(const ArrangementPlayback& value) {
        if (!validSnapshot(value)) return false;
        return publishValidated(std::make_shared<const ArrangementPlayback>(value));
    }
    // Message thread. Takes ownership without copying the snapshot.
    bool publish(std::shared_ptr<const ArrangementPlayback> value) {
        if (value == nullptr || !validSnapshot(*value)) return false;
        return publishValidated(std::move(value));
    }
    // Audio thread. Returns the newest snapshot if one arrived since the last
    // call, otherwise nullptr. The pointer stays valid until a later consume()
    // returns non-null and one more has begun. Wait-free, no allocation, no copy.
    const ArrangementPlayback* consume() noexcept { return box.consume(); }
    // Audio thread (or any thread while no consume() runs concurrently): the
    // snapshot most recently returned by consume(), or the initial empty one.
    const ArrangementPlayback* current() const noexcept { return box.current(); }
    // Message thread: releases snapshots the audio thread has finished with (T1-04 D6).
    // Returns how many old snapshots are still held.
    std::size_t collect() { return box.collect(); }
    // Number of snapshots accepted so far (diagnostics and tests).
    unsigned publishedCount() const noexcept { return published.load(std::memory_order_relaxed); }
private:
    bool publishValidated(std::shared_ptr<const ArrangementPlayback> value) {
        if (!box.publish(std::move(value))) return false;
        published.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    RetiringMailbox<ArrangementPlayback> box;
    std::atomic<unsigned> published{0};
};
} // namespace alien
