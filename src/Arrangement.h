#pragma once
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
    // What a track's region plays. Audio thread safe: a binary search, no allocation.
    const ArrangementClip& clipFor(const alien::timeline::Region& region, std::size_t track) const noexcept {
        if (region.clip == 0) return clips[static_cast<std::size_t>(region.source)][track];
        const auto found = std::lower_bound(placements.begin(), placements.end(), region.clip,
                                            [](const ArrangementPlacement& p, std::uint64_t id) { return p.clip < id; });
        return found != placements.end() && found->clip == region.clip ? found->content : silentArrangementClip;
    }
};

// Hands complete, immutable song snapshots from the message thread to the audio
// thread. Only a pointer ever crosses threads:
// the audio callback swaps a slot index and reads through a raw pointer, and
// never copies, allocates, or frees a snapshot. All three slots hold
// shared_ptrs that only the publishing thread assigns, so an old snapshot (and
// with it any clip notes nothing else holds) is released on the message thread,
// and only after the audio thread has moved on.
class ArrangementMailbox {
public:
    ArrangementMailbox() {
        const auto empty = std::make_shared<const ArrangementPlayback>();
        for (auto& slot : slots) slot = empty;
    }
    static bool validSnapshot(const ArrangementPlayback& value) {
        if (!value.timeline.valid()) return false;
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
    // call, otherwise nullptr. The pointer stays valid until the next consume()
    // that returns non-null. Wait-free, no allocation, no copy.
    const ArrangementPlayback* consume() noexcept {
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return nullptr;
        front = middle.exchange(front, std::memory_order_acq_rel) & 3;
        return slots[front].get();
    }
    // Audio thread (or any thread while no consume() runs concurrently): the
    // snapshot most recently returned by consume(), or the initial empty one.
    const ArrangementPlayback* current() const noexcept { return slots[front].get(); }
    // Number of snapshots accepted so far (diagnostics and tests).
    unsigned publishedCount() const noexcept { return published.load(std::memory_order_relaxed); }
private:
    bool publishValidated(std::shared_ptr<const ArrangementPlayback> value) {
        std::lock_guard<std::mutex> guard(producerMutex); // producers only; the audio thread never takes it
        slots[back] = std::move(value);
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & 3;
        published.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    static constexpr unsigned dirty = 4;
    std::array<std::shared_ptr<const ArrangementPlayback>, 3> slots;
    std::atomic<unsigned> middle{1};
    std::atomic<unsigned> published{0};
    std::mutex producerMutex;
    unsigned front{}, back{2};
};
} // namespace alien
