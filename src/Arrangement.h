#pragma once
#include "SessionAudioClip.h"
#include <memory>

namespace alien {
struct ArrangementRegion {
    int track{}, scene{}, startBar{}, lengthBars{4};
};
struct Arrangement {
    static constexpr std::size_t capacity = 128;
    std::array<ArrangementRegion, capacity> regions{};
    std::size_t count{};
    int lengthBars{16};
    bool valid() const noexcept {
        if (lengthBars < 1 || lengthBars > 128 || count > capacity) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = regions[i];
            if (a.track < 0 || a.track >= 4 || a.scene < 0 || a.scene >= 4
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
};
struct ArrangementPlayback {
    Arrangement arrangement;
    std::array<std::array<SessionClipLaunch, 4>, 4> clips{};
};

// Heap storage is allocated before playback. Copy and exchange are bounded;
// the audio callback neither allocates nor frees snapshots/sample data.
class ArrangementMailbox {
public:
    ArrangementMailbox() : buffers(std::make_unique<std::array<ArrangementPlayback,3>>()) {}
    bool publish(const ArrangementPlayback& value) noexcept {
        if (!value.arrangement.valid()) return false;
        for (const auto& scene : value.clips) for (const auto& clip : scene)
            if (!clip.pattern.valid()) return false;
        (*buffers)[back] = value;
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & 3;
        return true;
    }
    bool consume(ArrangementPlayback& value) noexcept {
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return false;
        front = middle.exchange(front, std::memory_order_acq_rel) & 3;
        value = (*buffers)[front]; return true;
    }
private:
    static constexpr unsigned dirty = 4;
    std::unique_ptr<std::array<ArrangementPlayback,3>> buffers;
    std::atomic<unsigned> middle{1};
    unsigned front{}, back{2};
};
} // namespace alien
