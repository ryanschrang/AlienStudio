#pragma once
#include "PluginRuntime.h"
namespace alien {
// Offline-only capture. Allocate and prepare before rendering, never on callback.
struct StemCapture {
    static constexpr int returnBase = maxSessionTracks, directBus = maxSessionTracks + maxReturns;
    static constexpr int busBase = directBus + 1;   // group buses, after their FX and strip
    int freezeTrack{-1}; // >=0: capture this track before fader/pan/automation.
    std::array<juce::AudioBuffer<float>, busBase + maxBuses> buses;
    // T1-04 D20: each line holds only the latency this render needs (rounded up like the live alignment
    // stages), not maxDelay: a render with no latent bus or return plugin holds no capture delay audio.
    struct Delay {
        std::vector<float> left, right;
        std::size_t position{};
        int capacity{};
        void prepare(int samples) {
            capacity = std::clamp(samples, 0, PluginRuntime::maxDelay);
            left.assign(static_cast<std::size_t>(capacity + 1), 0); right = left; position = 0;
        }
        // Delays by `samples`, or by as much as the line holds; returns false when it held too little.
        bool process(juce::AudioBuffer<float>& buffer, int frames, int samples) noexcept {
            const int wanted = std::clamp(samples, 0, PluginRuntime::maxDelay);
            const auto delay = static_cast<std::size_t>(std::min(wanted, capacity));
            for (int i=0; i<frames; ++i) {
                left[position] = buffer.getSample(0,i); right[position] = buffer.getSample(1,i);
                const auto read = (position + left.size() - delay) % left.size();
                buffer.setSample(0,i,left[read]); buffer.setSample(1,i,right[read]);
                position = (position + 1) % left.size();
            }
            return wanted <= capacity;
        }
    };
    std::array<Delay, maxSessionTracks + 1> dryDelays;
    std::array<Delay, maxBuses> busDelays;
    // Blocks in which a line delayed less than it should, because a plugin reported more latency after the
    // capture was prepared. The render fails rather than publish files that are out of step.
    std::uint32_t shortfalls{};
    // Owner of the idle offline engine, after the plugins are prepared (ToneEngine::prepare).
    void prepare(const PluginRuntime& plugins) { prepare(plugins.busLatency(), plugins.returnLatency()); }
    void prepare(int busLatency, int returnLatency) {
        for (auto& bus : buses) bus.setSize(2, PluginRuntime::blockSize);
        const int dry = PluginRuntime::roundedAlignment(std::min(busLatency + returnLatency, PluginRuntime::maxDelay));
        const int wet = PluginRuntime::roundedAlignment(returnLatency);
        for (auto& delay : dryDelays) delay.prepare(dry);
        for (auto& delay : busDelays) delay.prepare(wet);
        shortfalls = 0;
    }
    std::size_t delayBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& d : dryDelays) bytes += (d.left.size() + d.right.size()) * sizeof(float);
        for (const auto& d : busDelays) bytes += (d.left.size() + d.right.size()) * sizeof(float);
        return bytes;
    }
    void clear() noexcept { for (auto& bus : buses) bus.clear(); }
    // Tracks and the rack's direct bus are captured before the bus stage and the returns, the group
    // buses before the returns; each waits for the stages it skipped, so every file lines up.
    void alignDry(int frames, int busLatency, int returnLatency) noexcept {
        const int dry = std::min(busLatency + returnLatency, PluginRuntime::maxDelay);
        bool held = true;
        for (int t=0; t<maxSessionTracks; ++t) held &= dryDelays[static_cast<std::size_t>(t)].process(buses[static_cast<std::size_t>(t)],frames,dry);
        held &= dryDelays.back().process(buses[directBus],frames,dry);
        for (int b=0; b<maxBuses; ++b) held &= busDelays[static_cast<std::size_t>(b)].process(buses[static_cast<std::size_t>(busBase+b)],frames,returnLatency);
        if (!held) ++shortfalls;
    }
};
} // namespace alien
