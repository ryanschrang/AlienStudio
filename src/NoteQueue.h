#pragma once
#include <array>
#include <atomic>

// Single GUI producer / audio consumer. Overflow requests a panic instead of losing note-offs.
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
class NoteQueue {
public:
    struct Event { int note; float velocity; };
    void push(int note, float velocity) noexcept {
        const auto w = write.load(std::memory_order_relaxed);
        const auto next = (w + 1) % events.size();
        if (next == read.load(std::memory_order_acquire)) { panic.store(true); return; }
        events[w] = {note, velocity}; write.store(next, std::memory_order_release);
    }
    bool pop(Event& event) noexcept {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        event = events[r]; read.store((r + 1) % events.size(), std::memory_order_release); return true;
    }
    std::atomic<bool> panic{false};
private:
    std::array<Event, 256> events{};
    std::atomic<std::size_t> read{0}, write{0};
};

