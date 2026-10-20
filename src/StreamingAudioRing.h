#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace alien {
// One decoder producer and one playback consumer. Construct before playback;
// never reset while either side is running. Seek/relaunch coordination belongs
// to the stream owner, which must also keep each consumer's ring independent.
template<std::size_t Capacity = 16384>
class StreamingAudioRing {
public:
    struct Frame { float left{}, right{}; };
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static constexpr std::size_t capacity = Capacity;
    static constexpr std::size_t sampleStorageBytes = Capacity * sizeof(Frame);

    // Producer only. Partial writes are permitted; no unread frame is replaced.
    std::size_t write(std::span<const Frame> input) noexcept {
        const auto position = written.load(std::memory_order_relaxed);
        const auto consumed = read.load(std::memory_order_acquire);
        const auto available = Capacity - static_cast<std::size_t>(position - consumed);
        const auto count = input.size() < available ? input.size() : available;
        for (std::size_t i = 0; i < count; ++i)
            frames[static_cast<std::size_t>(position + i) & (Capacity - 1)] = input[i];
        written.store(position + count, std::memory_order_release);
        return count;
    }

    // Consumer only. Two peeks plus consume support sample interpolation.
    // A failed peek leaves output unchanged. Underrun handling is the caller's.
    bool peek(std::size_t offset, Frame& output) const noexcept {
        const auto position = read.load(std::memory_order_relaxed);
        const auto end = written.load(std::memory_order_acquire);
        if (offset >= end - position) return false;
        output = frames[static_cast<std::size_t>(position + offset) & (Capacity - 1)];
        return true;
    }
    bool consume(std::size_t count) noexcept {
        const auto position = read.load(std::memory_order_relaxed);
        const auto end = written.load(std::memory_order_acquire);
        if (count > end - position) return false;
        read.store(position + count, std::memory_order_release);
        return true;
    }
private:
    std::array<Frame, Capacity> frames{};
    alignas(64) std::atomic<std::uint64_t> written{};
    alignas(64) std::atomic<std::uint64_t> read{};
};
} // namespace alien
