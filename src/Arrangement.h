#pragma once
#include "SessionAudioClip.h"
#include "SessionCapacity.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace alien {
struct ArrangementRegion {
    int track{}, scene{}, startBar{}, lengthBars{4};
};
struct Arrangement {
    static constexpr std::size_t capacity = 128;
    static constexpr int maxLengthBars = 16384; // 18 h 12 min at the supported 60 BPM minimum.
    std::array<ArrangementRegion, capacity> regions{};
    std::size_t count{};
    int lengthBars{16};
    bool valid(int tracks = maxSessionTracks, int scenes = maxSessionScenes) const noexcept {
        if (lengthBars < 1 || lengthBars > maxLengthBars || count > capacity) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = regions[i];
            if (a.track < 0 || a.track >= tracks || a.scene < 0 || a.scene >= scenes
                || a.startBar < 0 || a.lengthBars < 1 || a.lengthBars > lengthBars
                || a.startBar > lengthBars - a.lengthBars) return false;
            for (std::size_t j = 0; j < i; ++j) {
                const auto& b = regions[j];
                if (a.track == b.track && a.startBar < b.startBar + b.lengthBars
                    && b.startBar < a.startBar + a.lengthBars) return false;
            }
        }
        return true;
    }
    bool add(ArrangementRegion region) noexcept {
        if (count >= capacity) return false;
        regions[count++] = region;
        if (valid()) return true;
        regions[--count] = {}; return false;
    }
    bool replace(std::size_t index, ArrangementRegion region) noexcept {
        if (index >= count) return false;
        const auto old = regions[index]; regions[index] = region;
        if (valid()) return true;
        regions[index] = old; return false;
    }
    bool erase(std::size_t index) noexcept {
        if (index >= count) return false;
        for (auto i = index; i + 1 < count; ++i) regions[i] = regions[i+1];
        regions[--count] = {}; return true;
    }
    // True when both arrangements place the same regions in a song of the same
    // length. Used to tell a re-publish of unchanged timing (clip contents or
    // mixer edits elsewhere in the project) from a real change to the timeline.
    bool sameLayout(const Arrangement& other) const noexcept {
        if (count != other.count || lengthBars != other.lengthBars) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = regions[i]; const auto& b = other.regions[i];
            if (a.track != b.track || a.scene != b.scene || a.startBar != b.startBar || a.lengthBars != b.lengthBars)
                return false;
        }
        return true;
    }
};
struct ArrangementPlayback {
    Arrangement arrangement;
    std::array<std::array<SessionClipLaunch, maxSessionTracks>, maxSessionScenes> clips{};
};

// Hands complete, immutable song snapshots from the message thread to the audio
// thread. A snapshot is about 16 MiB, so only a pointer ever crosses threads:
// the audio callback swaps a slot index and reads through a raw pointer, and
// never copies, allocates, or frees a snapshot. All three slots hold
// shared_ptrs that only the publishing thread assigns, so an old snapshot is
// released on the message thread, and only after the audio thread has moved on.
class ArrangementMailbox {
public:
    ArrangementMailbox() {
        const auto empty = std::make_shared<const ArrangementPlayback>();
        for (auto& slot : slots) slot = empty;
    }
    static bool validSnapshot(const ArrangementPlayback& value) noexcept {
        if (!value.arrangement.valid()) return false;
        for (const auto& scene : value.clips) for (const auto& clip : scene)
            if (!clip.pattern.valid()) return false;
        return true;
    }
    // Message thread. Copies the value into a fresh snapshot.
    bool publish(const ArrangementPlayback& value) {
        if (!validSnapshot(value)) return false;
        return publishValidated(std::make_shared<const ArrangementPlayback>(value));
    }
    // Message thread. Takes ownership without copying the 16 MiB payload.
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
