#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace alien {

struct MidiEvent { std::uint8_t status{0}, data1{0}, data2{0}; };

// One producer (message thread), one consumer (audio thread). A full queue
// triggers a panic on the consumer, so a lost note-off cannot leave a stuck key.
class ControlQueue {
public:
    static constexpr std::size_t capacity = 128;
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    bool push(MidiEvent event) noexcept {
        const auto index = write.load(std::memory_order_relaxed);
        const auto next = (index + 1) % capacity;
        if (next == read.load(std::memory_order_acquire)) {
            overflow.store(true, std::memory_order_release);
            return false;
        }
        events[index] = event;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(MidiEvent& event) noexcept {
        const auto index = read.load(std::memory_order_relaxed);
        if (index == write.load(std::memory_order_acquire)) return false;
        event = events[index];
        read.store((index + 1) % capacity, std::memory_order_release);
        return true;
    }
    bool takeOverflow() noexcept { return overflow.exchange(false, std::memory_order_acq_rel); }
    void discardPending() noexcept { read.store(write.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::array<MidiEvent, capacity> events{};
    std::atomic<std::size_t> write{0}, read{0};
    std::atomic<bool> overflow{false};
};

} // namespace alien
