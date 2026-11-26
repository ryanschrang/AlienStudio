// T1-04 D31: the recorder behind AlienDeviceTiming. Records nothing until armed, never allocates while
// recording, counts what does not fit, and summarises durations, budget overruns and late callbacks.
#include "CallbackTiming.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

static std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }

int main() {
    using alien::CallbackTimingRecorder;
    {   // Nearest rank on known values.
        const std::vector<double> one{7.0}, ten{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        require(CallbackTimingRecorder::rank({}, 0.5) == 0.0, "the rank of nothing is 0");
        require(CallbackTimingRecorder::rank(one, 0.5) == 7.0 && CallbackTimingRecorder::rank(one, 0.99) == 7.0, "one value is every rank");
        require(CallbackTimingRecorder::rank(ten, 0.5) == 5.0 && CallbackTimingRecorder::rank(ten, 0.95) == 10.0
                && CallbackTimingRecorder::rank(ten, 0.10) == 1.0 && CallbackTimingRecorder::rank(ten, 0.11) == 2.0, "nearest-rank percentiles");
    }
    CallbackTimingRecorder recorder(100);
    recorder.record(0.0, 50.0);
    require(recorder.summarise(1000.0).callbacks == 0, "nothing is recorded before arming");

    // A 1,000 us period: 98 callbacks on time taking 1..98 us, one that takes 1,500 us, and one that
    // starts 2,000 us after the one before it.
    const auto before = allocations;
    recorder.arm();
    double start = 10'000.0;
    for (int n = 1; n <= 98; ++n) { recorder.record(start, n); start += 1000.0; }
    recorder.record(start, 1500.0); start += 2000.0;
    recorder.record(start, 3.0);
    recorder.record(start + 1000.0, 4.0);   // the storage is full: counted, not recorded
    recorder.disarm();
    require(allocations == before, "recording allocates nothing");
    recorder.record(start + 2000.0, 5.0);
    auto s = recorder.summarise(1000.0);
    require(s.callbacks == 100 && s.dropped == 1, "callbacks that do not fit are counted, and nothing after disarming");
    require(s.p50 == 49.0 && s.p95 == 94.0 && s.p99 == 98.0 && s.max == 1500.0, "duration percentiles and maximum");
    require(s.overBudget == 1, "one callback took longer than its period");
    require(s.late == 1 && s.maxIntervalUs == 2000.0, "one callback started late, and the longest gap is measured");
    require(std::abs(s.p95BudgetPercent() - 9.4) < 1e-9, "p95 as a share of the period");

    // Arming again starts afresh: the first callback has no previous start to be late after.
    recorder.arm();
    recorder.record(1.0e9, 10.0);
    recorder.record(1.0e9 + 1000.0, 20.0);
    recorder.disarm();
    s = recorder.summarise(1000.0);
    require(s.callbacks == 2 && s.dropped == 0 && s.late == 0 && s.maxIntervalUs == 1000.0 && s.max == 20.0,
            "a new arming forgets the previous recording and its last start");
    require(recorder.summarise(0.0).p95BudgetPercent() == 0.0, "no period, no budget share");
    std::cout << "PASS: callback timing recorder (ranks, arming, overflow, budget, late callbacks, no allocation)\n";
}
