#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace alien {

// MIDI learn state is published by the UI and consumed by the audio thread.
// Scene commands travel back to the UI through a bounded single-producer,
// single-consumer queue, keeping component access off the real-time thread.
class MidiControllerMappings {
public:
    static_assert(std::atomic<int>::is_always_lock_free,
        "MIDI learn state must remain lock-free on the audio thread");
    static constexpr int sceneCount = 4;
    static constexpr int stopAllAction = sceneCount;
    static constexpr int actionCount = sceneCount + 1;

    void beginLearn(int action) noexcept {
        if (action >= 0 && action < actionCount) learnTarget.store(action, std::memory_order_release);
    }
    void cancelLearn() noexcept { learnTarget.store(-1, std::memory_order_release); }
    int takeLearnedAction() noexcept { return learnedAction.exchange(-1, std::memory_order_acq_rel); }
    void resetEdges() noexcept { isDown.fill(false); }

    int mappingFor(int action) const noexcept {
        return action >= 0 && action < actionCount
            ? mappings[static_cast<std::size_t>(action)].load(std::memory_order_acquire) : -1;
    }
    void clearMapping(int action) noexcept {
        if (action >= 0 && action < actionCount)
            mappings[static_cast<std::size_t>(action)].store(-1, std::memory_order_release);
    }

    // Returns a scene/stop action only on a low-to-high edge (MIDI value 64).
    // The first message received while learning binds the CC without firing it.
    int processCC(int channel, int controller, int value) noexcept {
        if (channel < 0 || channel > 15 || controller < 0 || controller > 127
            || value < 0 || value > 127) return -1;
        const int code = channel * 128 + controller;
        int target = learnTarget.load(std::memory_order_acquire);
        if (target >= 0 && learnTarget.compare_exchange_strong(target, -1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            for (int i = 0; i < actionCount; ++i)
                if (i != target && mappings[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) == code)
                    mappings[static_cast<std::size_t>(i)].store(-1, std::memory_order_release);
            mappings[static_cast<std::size_t>(target)].store(code, std::memory_order_release);
            observedMappings[static_cast<std::size_t>(target)] = code;
            isDown[static_cast<std::size_t>(target)] = value >= 64;
            learnedAction.store(target, std::memory_order_release);
            return -1;
        }

        int triggered = -1;
        for (int i = 0; i < actionCount; ++i) {
            const auto index = static_cast<std::size_t>(i);
            const int mapped = mappings[index].load(std::memory_order_acquire);
            if (observedMappings[index] != mapped) {
                observedMappings[index] = mapped;
                isDown[index] = false;
            }
            if (mapped != code) continue;
            const bool downNow = value >= 64;
            if (downNow && !isDown[index]) triggered = i;
            isDown[index] = downNow;
        }
        return triggered;
    }

private:
    std::array<std::atomic<int>, actionCount> mappings{{-1, -1, -1, -1, -1}};
    std::atomic<int> learnTarget{-1};
    std::atomic<int> learnedAction{-1};
    // Audio-thread-only edge and configuration tracking.
    std::array<int, actionCount> observedMappings{{-1, -1, -1, -1, -1}};
    std::array<bool, actionCount> isDown{};
};

class MidiControllerActionQueue {
public:
    static constexpr std::size_t capacity = 16;
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
        "MIDI action queue indices must remain lock-free on the audio thread");

    bool push(int action) noexcept {
        if (action < 0 || action >= MidiControllerMappings::actionCount) return false;
        const auto w = write.load(std::memory_order_relaxed);
        const auto next = (w + 1) % entries.size();
        if (next == read.load(std::memory_order_acquire)) return false;
        entries[w] = action;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(int& action) noexcept {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        action = entries[r];
        read.store((r + 1) % entries.size(), std::memory_order_release);
        return true;
    }

private:
    // One slot stays empty to distinguish full from empty.
    std::array<int, capacity> entries{};
    std::atomic<std::size_t> read{}, write{};
};

} // namespace alien
