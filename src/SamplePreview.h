#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <vector>

namespace alien {
// One GUI producer, one audio consumer. A slot's samples grow on the GUI thread, while it owns the
// slot; only the owner of a slot touches its samples. Reclamation never deletes in the callback.
class SamplePreview {
public:
    static_assert(std::atomic<int>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);
    static constexpr int capacity = 5 * 192000;
    SamplePreview() : slots(std::make_unique<std::array<Slot, 3>>()) {}
    // GUI thread. A slot's storage grows here, to the longest preview it has held (T1-04 D): the audio
    // thread never reads a slot it has handed back as free, so the GUI owns it while writing. Nothing is
    // reserved until something is previewed. Returns false if the preview cannot be queued.
    bool request(const float* left, const float* right, int frames, double rate, bool alignToBeat = false) {
        if (!left || frames <= 0 || frames > capacity || !std::isfinite(rate) || rate < 8000 || rate > 192000) return false;
        const auto requestedEpoch = alignedEpoch.load(std::memory_order_acquire);
        auto requestedGeneration = generation.load(std::memory_order_acquire);
        for (auto& slot : *slots) {
            int expected = free;
            if (!slot.state.compare_exchange_strong(expected, writing, std::memory_order_acquire)) continue;
            try {
                // One resize can succeed before the other throws. Recover each
                // channel independently when this free slot is requested again.
                if (slot.left.size() < static_cast<std::size_t>(frames)) slot.left.resize(static_cast<std::size_t>(frames));
                if (slot.right.size() < static_cast<std::size_t>(frames)) slot.right.resize(static_cast<std::size_t>(frames));
            } catch (const std::bad_alloc&) { slot.state.store(free, std::memory_order_release); return false; }
            std::copy_n(left, frames, slot.left.data());
            std::copy_n(right ? right : left, frames, slot.right.data());
            slot.frames = frames; slot.rate = rate; slot.alignToBeat = alignToBeat;
            slot.alignedEpoch = requestedEpoch;
            // Stop/panic may arrive while the producer copies audio. Publish only
            // if that cancellation token still matches; never supersede it with
            // an older in-flight request. Failed requests reclaim storage here.
            const auto nextGeneration = requestedGeneration + 1;
            if (!generation.compare_exchange_strong(requestedGeneration,nextGeneration,std::memory_order_acq_rel)) {
                slot.state.store(free,std::memory_order_release); return false;
            }
            slot.generation = nextGeneration;
            slot.state.store(ready, std::memory_order_release); return true;
        }
        return false;
    }
    void stop() noexcept { generation.fetch_add(1, std::memory_order_acq_rel); }
    // Audio thread: an explicit transport jump invalidates beat-aligned work,
    // including a GUI producer still copying into a writing slot. Independent
    // immediate auditions keep playing; slot storage is reclaimed off callback.
    void cancelAligned() noexcept {
        alignedEpoch.fetch_add(1, std::memory_order_acq_rel);
        if (active >= 0 && (*slots)[active].alignToBeat) release();
    }
    void prepare(double rate) noexcept { outputRate = rate > 0 ? rate : 44100; stop(); }
    void beginBlock(bool panic = false) noexcept {
        if (panic) stop();
        const auto current = generation.load(std::memory_order_acquire);
        if (active >= 0 && (*slots)[active].generation != current) release();
        for (int i = 0; i < 3; ++i) {
            auto& slot = (*slots)[i];
            if (slot.state.load(std::memory_order_acquire) != ready) continue;
            // A GUI request can be published after this block began. Compare
            // against the latest token so that request survives until playback.
            if (slot.generation != generation.load(std::memory_order_acquire)
                || (slot.alignToBeat && slot.alignedEpoch != alignedEpoch.load(std::memory_order_acquire))) {
                slot.state.store(free, std::memory_order_release); continue;
            }
            release(); active = i; position = 0; waiting = slot.alignToBeat; slot.state.store(playing, std::memory_order_relaxed);
        }
    }
    bool waitingForBeat() const noexcept { return active >= 0 && waiting; } // audio thread only
    std::array<float, 2> nextSample(bool transportPlaying = false, double beat = 0, double beatStep = 0) noexcept {
        if (active < 0) return {};
        if (waiting) {
            if (!transportPlaying) { release(); return {}; }
            // Start at the first sample on or after a quarter-note boundary. Test the
            // current phase, not an elapsed countdown: seeks and loop wraps stay aligned.
            if (!std::isfinite(beat) || !std::isfinite(beatStep) || beatStep <= 0
                || std::floor(beat + 1.0e-9) == std::floor(beat - beatStep + 1.0e-9)) return {};
            waiting = false;
        }
        const auto& slot = (*slots)[active];
        const int frame = static_cast<int>(position);
        if (frame >= slot.frames) { release(); return {}; }
        const int next = std::min(frame + 1, slot.frames - 1);
        const float fraction = static_cast<float>(position - frame);
        const float fade = static_cast<float>(std::min({1.0, position / (slot.rate * .005), (slot.frames - position) / (slot.rate * .005)})) * .5f;
        const auto interpolate = [&](const auto& data) { const float value = data[frame] + fraction * (data[next] - data[frame]); return std::isfinite(value) ? value * fade : 0.0f; };
        std::array<float, 2> value{interpolate(slot.left), interpolate(slot.right)};
        position += slot.rate / outputRate; return value;
    }
private:
    void release() noexcept { if (active >= 0) (*slots)[active].state.store(free, std::memory_order_release); active = -1; }
    enum { free, writing, ready, playing };
    struct Slot { std::atomic<int> state{free}; std::vector<float> left, right; int frames{}; double rate{}; unsigned generation{}, alignedEpoch{}; bool alignToBeat{}; };
    std::unique_ptr<std::array<Slot, 3>> slots;
    std::atomic<unsigned> generation{0};
    std::atomic<unsigned> alignedEpoch{0};
    int active{-1}; bool waiting{}; double position{}, outputRate{44100};
public:
    // GUI thread: bytes of preview audio the slots hold now.
    std::size_t reservedBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& slot : *slots) bytes += (slot.left.capacity() + slot.right.capacity()) * sizeof(float);
        return bytes;
    }
};
} // namespace alien
