#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace alien {
// Hands complete, immutable snapshots from control threads to the audio thread. Only a pointer
// ever crosses threads: the audio callback swaps a slot index and reads through a raw pointer, and
// never copies, allocates or frees a snapshot. Three slots hold shared_ptrs that only producers
// assign, so every snapshot is released on a control thread.
//
// T1-04 D6: a snapshot the audio thread has stopped reading is released once the audio thread has
// started a later block, instead of staying in a slot until two more publishes overwrite it. Each
// consume() counts a visit and, when it moves to a newer snapshot, stamps the one it leaves with
// that visit. A later visit means the block that left it has finished, because one audio thread
// runs blocks in order. Until then the snapshot waits, even if a publish needs its slot, so it is
// never freed while the block that left it may still read it.
template <class T>
class RetiringMailbox {
public:
    explicit RetiringMailbox(std::shared_ptr<const T> initial) {
        for (auto& slot : slots) slot = initial;
    }
    // Control thread (producers are serialised by a mutex the audio thread never takes).
    bool publish(std::shared_ptr<const T> value) {
        if (value == nullptr) return false;
        std::lock_guard<std::mutex> guard(producerMutex);
        retire(back);
        slots[back] = std::move(value);
        leftAt[back].store(0, std::memory_order_relaxed);   // not read yet
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & 3;
        collectLocked();
        return true;
    }
    // Control thread: releases every snapshot the audio thread has finished with. Returns how many
    // left-behind snapshots are still held (waiting for the audio thread's next block).
    std::size_t collect() {
        std::lock_guard<std::mutex> guard(producerMutex);
        collectLocked();
        return heldLocked();
    }
    // Audio thread, once per block. The newest snapshot if one arrived since the last call, else
    // nullptr. The pointer stays valid until a later consume() returns non-null and one more
    // consume() has begun. Wait-free, no allocation, no copy.
    const T* consume() noexcept {
        const auto visit = visits.load(std::memory_order_relaxed) + 1;
        visits.store(visit, std::memory_order_release);   // everything before this block is finished
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return nullptr;
        leftAt[front].store(visit, std::memory_order_relaxed);   // published by the exchange below
        front = middle.exchange(front, std::memory_order_acq_rel) & 3;
        return slots[front].get();
    }
    // Audio thread (or any thread while no consume() runs concurrently): the snapshot most
    // recently returned by consume(), or the initial one.
    const T* current() const noexcept { return slots[front].get(); }

private:
    // A slot's content may go once the audio thread never read it, or has begun a block after the
    // one that left it.
    bool finished(std::size_t slot) const noexcept {
        const auto left = leftAt[slot].load(std::memory_order_relaxed);
        return left == 0 || visits.load(std::memory_order_acquire) > left;
    }
    void retire(unsigned slot) {
        if (slots[slot] != nullptr && !finished(slot))
            waiting.push_back({std::move(slots[slot]), leftAt[slot].load(std::memory_order_relaxed)});
        slots[slot].reset();
    }
    void collectLocked() {
        const auto now = visits.load(std::memory_order_acquire);
        std::erase_if(waiting, [now](const Waiting& w) { return now > w.leftAt; });
        // The audio thread holds `front`. It reads `middle` only once a producer marks it dirty, and
        // only producers (holding this mutex) do, so a clean middle and the back slot are ours.
        if (finished(back)) slots[back].reset();
        const auto m = middle.load(std::memory_order_acquire);
        if ((m & dirty) == 0 && finished(m & 3)) slots[m & 3].reset();
    }
    std::size_t heldLocked() const noexcept {
        std::size_t held = waiting.size();
        const auto m = middle.load(std::memory_order_acquire);
        if (slots[back] != nullptr) ++held;
        if ((m & dirty) == 0 && slots[m & 3] != nullptr) ++held;
        return held;
    }
    struct Waiting { std::shared_ptr<const T> snapshot; std::uint64_t leftAt{}; };
    static constexpr unsigned dirty = 4;
    std::array<std::shared_ptr<const T>, 3> slots;
    std::array<std::atomic<std::uint64_t>, 3> leftAt{};   // the visit that left a slot's content; 0: not read since written
    std::atomic<std::uint64_t> visits{0};
    std::atomic<unsigned> middle{1};
    std::mutex producerMutex;
    std::vector<Waiting> waiting;   // producers only
    unsigned front{}, back{2};
};
} // namespace alien
