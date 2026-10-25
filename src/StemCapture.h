#pragma once
#include "PluginRuntime.h"
namespace alien {
// Offline-only capture. Allocate and prepare before rendering, never on callback.
struct StemCapture {
    static constexpr int returnBase = maxSessionTracks, directBus = maxSessionTracks + maxReturns;
    std::array<juce::AudioBuffer<float>, directBus + 1> buses;
    struct Delay {
        std::vector<float> left, right;
        std::size_t position{};
        void prepare(int samples) { left.assign(static_cast<std::size_t>(samples + 1), 0); right = left; position = 0; }
        void process(juce::AudioBuffer<float>& buffer, int frames, int samples) noexcept {
            const auto delay = static_cast<std::size_t>(std::clamp(samples,0,PluginRuntime::maxDelay));
            for (int i=0; i<frames; ++i) {
                left[position] = buffer.getSample(0,i); right[position] = buffer.getSample(1,i);
                const auto read = (position + left.size() - delay) % left.size();
                buffer.setSample(0,i,left[read]); buffer.setSample(1,i,right[read]);
                position = (position + 1) % left.size();
            }
        }
    };
    std::array<Delay, maxSessionTracks + 1> dryDelays;
    void prepare() {
        for (auto& bus : buses) bus.setSize(2, PluginRuntime::blockSize);
        for (auto& delay : dryDelays) delay.prepare(PluginRuntime::maxDelay);
    }
    void clear() noexcept { for (auto& bus : buses) bus.clear(); }
    void alignDry(int frames, int returnLatency) noexcept {
        for (int t=0; t<maxSessionTracks; ++t) dryDelays[static_cast<std::size_t>(t)].process(buses[static_cast<std::size_t>(t)],frames,returnLatency);
        dryDelays.back().process(buses[directBus],frames,returnLatency);
    }
};
} // namespace alien
