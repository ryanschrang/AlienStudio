#pragma once
// Callback timing on a real audio device (T1-04 D31, measurement gate 2).
//
// The audio thread records how long each callback took and how long since the previous callback
// started, into storage reserved before recording starts: no allocation, lock or I/O on the audio
// thread. The control thread arms the recorder, waits, disarms it and then summarises what was
// recorded. A callback that arrives while the storage is full is counted, not recorded.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace alien {

struct CallbackTimingSummary {
    std::size_t callbacks{}, dropped{};   // recorded, and arrived with the storage full
    double p50{}, p95{}, p99{}, max{};    // callback duration, microseconds
    double periodUs{};                    // what one block of the device's buffer lasts
    std::size_t overBudget{};             // callbacks that took longer than one period
    std::size_t late{};                   // callbacks that started more than 1.5 periods after the previous one
    double maxIntervalUs{};               // longest time between two callback starts
    std::size_t longest{};                // which callback took longest (0 is the first recorded)
    std::vector<std::size_t> overBudgetAt;   // which callbacks went over budget, in order
    double p95BudgetPercent() const noexcept { return periodUs > 0 ? p95 / periodUs * 100.0 : 0.0; }
};

class CallbackTimingRecorder {
public:
    // Control thread, while nothing records.
    explicit CallbackTimingRecorder(std::size_t capacity) : durations(capacity), intervals(capacity) {}
    // Control thread: forget what was recorded and record the callbacks that follow.
    void arm() noexcept {
        recorded.store(0, std::memory_order_relaxed);
        overflow.store(0, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_relaxed);   // the audio thread forgets its previous start
        armed.store(true, std::memory_order_release);
    }
    // Control thread: stop recording. A callback already inside record() still completes its entry, so wait
    // longer than one callback before summarising or arming again.
    void disarm() noexcept { armed.store(false, std::memory_order_release); }
    bool isArmed() const noexcept { return armed.load(std::memory_order_acquire); }

    // Audio thread. `startUs` is when this callback started on a steady clock, `durationUs` how long it took.
    void record(double startUs, double durationUs) noexcept {
        if (!armed.load(std::memory_order_acquire)) { hasPrevious = false; return; }
        if (const auto g = generation.load(std::memory_order_relaxed); g != seenGeneration) { seenGeneration = g; hasPrevious = false; }
        const double interval = hasPrevious ? startUs - previousStartUs : -1.0;
        previousStartUs = startUs; hasPrevious = true;
        const auto n = recorded.load(std::memory_order_relaxed);
        if (n >= durations.size()) { overflow.fetch_add(1, std::memory_order_relaxed); return; }
        durations[n] = durationUs; intervals[n] = interval;
        recorded.store(n + 1, std::memory_order_release);   // the entry is complete before it is counted
    }

    // Control thread, after disarm(). `periodUs` is one block's length at the device's rate and buffer size.
    CallbackTimingSummary summarise(double periodUs) const {
        CallbackTimingSummary s;
        const auto n = std::min(recorded.load(std::memory_order_acquire), durations.size());
        s.callbacks = n; s.dropped = overflow.load(std::memory_order_relaxed); s.periodUs = periodUs;
        if (n == 0) return s;
        std::vector<double> sorted(durations.begin(), durations.begin() + static_cast<std::ptrdiff_t>(n));
        std::sort(sorted.begin(), sorted.end());
        s.p50 = rank(sorted, 0.50); s.p95 = rank(sorted, 0.95); s.p99 = rank(sorted, 0.99); s.max = sorted.back();
        for (std::size_t i = 0; i < n; ++i) {
            if (durations[i] > durations[s.longest]) s.longest = i;
            if (durations[i] > periodUs) { ++s.overBudget; s.overBudgetAt.push_back(i); }
            if (intervals[i] < 0) continue;   // the first callback after arming has no previous start
            s.maxIntervalUs = std::max(s.maxIntervalUs, intervals[i]);
            if (intervals[i] > 1.5 * periodUs) ++s.late;
        }
        return s;
    }
    // Nearest rank: the smallest value with at least `p` of the values at or below it.
    static double rank(const std::vector<double>& sorted, double p) noexcept {
        if (sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
        return sorted[std::clamp<std::size_t>(index, 1, sorted.size()) - 1];
    }
private:
    std::vector<double> durations, intervals;
    std::atomic<std::size_t> recorded{0}, overflow{0};
    std::atomic<unsigned> generation{0};
    std::atomic<bool> armed{false};
    double previousStartUs{};   // audio thread only
    unsigned seenGeneration{};  // audio thread only
    bool hasPrevious{};         // audio thread only
};

} // namespace alien
