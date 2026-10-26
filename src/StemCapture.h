#pragma once
#include "PluginRuntime.h"
namespace alien {
// Offline-only capture. Allocate and prepare before rendering, never on callback.
struct StemCapture {
    static constexpr int returnBase = maxSessionTracks, directBus = maxSessionTracks + maxReturns;
    static constexpr int busBase = directBus + 1;   // group buses, after their FX and strip
    int freezeTrack{-1}; // >=0: capture this track before fader/pan/automation.
    std::array<juce::AudioBuffer<float>, busBase + maxBuses> buses;
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
    std::array<Delay, maxBuses> busDelays;
    void prepare() {
        for (auto& bus : buses) bus.setSize(2, PluginRuntime::blockSize);
        for (auto& delay : dryDelays) delay.prepare(PluginRuntime::maxDelay);
        for (auto& delay : busDelays) delay.prepare(PluginRuntime::maxDelay);
    }
    void clear() noexcept { for (auto& bus : buses) bus.clear(); }
    // Tracks and the rack's direct bus are captured before the bus stage and the returns, the group
    // buses before the returns; each waits for the stages it skipped, so every file lines up.
    void alignDry(int frames, int busLatency, int returnLatency) noexcept {
        const int dry = std::min(busLatency + returnLatency, PluginRuntime::maxDelay);
        for (int t=0; t<maxSessionTracks; ++t) dryDelays[static_cast<std::size_t>(t)].process(buses[static_cast<std::size_t>(t)],frames,dry);
        dryDelays.back().process(buses[directBus],frames,dry);
        for (int b=0; b<maxBuses; ++b) busDelays[static_cast<std::size_t>(b)].process(buses[static_cast<std::size_t>(busBase+b)],frames,returnLatency);
    }
};
} // namespace alien
