#pragma once
// What the host records when a plugin misbehaves, and the lock-free queue that carries the record
// from the audio thread (which must not allocate, lock or log) to the message thread (which tells
// the user and writes the log). Standard library only, so the queue is tested without JUCE.
#include "FaultGuard.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace alien {

// True if every sample is an ordinary number. Tests the bit pattern, so it stays correct under
// fast-math (where isfinite may be optimised away). Allocation-free, safe on the audio thread.
inline bool allFinite(const float* data, int count) noexcept {
    std::uint32_t bad = 0;
    for (int i = 0; i < count; ++i) { std::uint32_t bits; std::memcpy(&bits, data + i, sizeof bits); bad |= static_cast<std::uint32_t>((bits & 0x7f800000u) == 0x7f800000u); }
    return bad == 0;
}

// Which call into the plugin went wrong.
enum class PluginPhase : std::uint8_t {
    load,           // creating the instance, its bus layout, restoring saved state into it
    prepare,        // prepareToPlay
    process,        // processBlock, on the audio thread
    release,        // releaseResources
    saveState,      // getStateInformation
    editor,         // creating or showing the editor window
    query           // tail length and other getters
};

inline const char* phaseName(PluginPhase phase) noexcept {
    switch (phase) {
        case PluginPhase::load: return "loading";
        case PluginPhase::prepare: return "preparing to play";
        case PluginPhase::process: return "processing audio";
        case PluginPhase::release: return "releasing resources";
        case PluginPhase::saveState: return "saving its state";
        case PluginPhase::editor: return "opening its editor";
        case PluginPhase::query: return "answering a query";
    }
    return "running";
}

struct PluginFault {
    int rack{-1};                       // track index, or PluginRuntime::masterIndex
    int slot{-2};                       // -1 instrument, 0.. effect slot
    PluginPhase phase{PluginPhase::process};
    bool nonFiniteOutput{};             // no crash: the plugin returned NaN or infinity
    fault::Fault fault;
    std::int64_t samplePosition{};      // engine position when it happened (process phase)
};

inline std::string describe(const PluginFault& f) {
    if (f.nonFiniteOutput) return "produced invalid audio (NaN or infinity) while " + std::string(phaseName(f.phase));
    return "failed while " + std::string(phaseName(f.phase)) + ": " + fault::describe(f.fault);
}

// Many producers (the audio thread, the message thread preparing a plugin), one consumer (the
// message thread). Fixed storage, no allocation, no locks. A full queue drops the newest report
// and counts it: the slot is already disabled, so a flood of reports adds nothing.
class FaultQueue {
public:
    static constexpr std::uint32_t capacity = 64;
    bool push(const PluginFault& report) noexcept {
        std::uint32_t index = head.load(std::memory_order_relaxed);
        for (;;) {   // claim a position only while the ring has room
            if (index - tail.load(std::memory_order_acquire) >= capacity) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
            if (head.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) break;
        }
        auto& entry = entries[index % capacity];
        entry.report = report;
        entry.published.store(true, std::memory_order_release);
        return true;
    }
    bool pop(PluginFault& report) noexcept {
        const std::uint32_t index = tail.load(std::memory_order_relaxed);
        auto& entry = entries[index % capacity];
        if (!entry.published.load(std::memory_order_acquire)) return false;
        report = entry.report;
        entry.published.store(false, std::memory_order_release);
        tail.store(index + 1, std::memory_order_release);
        return true;
    }
    std::uint32_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }
private:
    struct Entry { PluginFault report; std::atomic<bool> published{false}; };
    std::array<Entry, capacity> entries;
    std::atomic<std::uint32_t> head{0}, tail{0};
    std::atomic<std::uint32_t> dropped{0};
};

} // namespace alien
