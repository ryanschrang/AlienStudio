#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>

namespace alien {
// One GUI producer, one audio consumer. Slots are allocated before playback;
// only the owner of a slot touches its samples. Reclamation never deletes in the callback.
class SamplePreview {
public:
    static_assert(std::atomic<int>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);
    static constexpr int capacity = 5 * 192000;
    SamplePreview() : slots(std::make_unique<std::array<Slot, 3>>()) {}
    bool request(const float* left, const float* right, int frames, double rate) noexcept {
        if (!left || frames <= 0 || frames > capacity || !std::isfinite(rate) || rate < 8000 || rate > 192000) return false;
        for (auto& slot : *slots) {
            int expected = free;
            if (!slot.state.compare_exchange_strong(expected, writing, std::memory_order_acquire)) continue;
            std::copy_n(left, frames, slot.left.data());
            std::copy_n(right ? right : left, frames, slot.right.data());
            slot.frames = frames; slot.rate = rate;
            slot.generation = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            slot.state.store(ready, std::memory_order_release); return true;
        }
        return false;
    }
    void stop() noexcept { generation.fetch_add(1, std::memory_order_acq_rel); }
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
            if (slot.generation != generation.load(std::memory_order_acquire)) { slot.state.store(free, std::memory_order_release); continue; }
            release(); active = i; position = 0; slot.state.store(playing, std::memory_order_relaxed);
        }
    }
    std::array<float, 2> nextSample() noexcept {
        if (active < 0) return {};
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
    struct Slot { std::atomic<int> state{free}; std::array<float, capacity> left{}, right{}; int frames{}; double rate{}; unsigned generation{}; };
    std::unique_ptr<std::array<Slot, 3>> slots;
    std::atomic<unsigned> generation{0};
    int active{-1}; double position{}, outputRate{44100};
};
} // namespace alien
