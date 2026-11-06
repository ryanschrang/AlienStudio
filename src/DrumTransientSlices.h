#pragma once
#include "DrumSampler.h"
#include <vector>

namespace alien {
struct DrumTransientSlices {
    std::array<DrumSlice, drumPadCount> ranges{}; // local to the selected view
    int count{};
};

// Worker-only, deterministic energy-onset heuristic, not beat detection. Refuse
// sources over one minute rather than silently analysing only part of a sample.
inline DrumTransientSlices detectDrumTransientSlices(const SessionAudioClip& clip) {
    DrumTransientSlices result;
    if (!clip.valid() || clip.streamed() || clip.sampleRate < 8000 || clip.sampleRate > 192000
        || clip.frames < 2 || clip.frames / clip.sampleRate > 60) return result;
    const auto hop = static_cast<std::size_t>(std::max(1.0, std::round(clip.sampleRate * .005)));
    const auto blocks = (clip.frames + hop - 1) / hop;
    std::vector<double> energy(blocks), novelty(blocks);
    double peak = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const auto end = std::min(clip.frames, (b + 1) * hop);
        double sum = 0;
        for (auto f = b * hop; f < end; ++f) {
            // Max channel energy avoids cancellation in opposite-polarity stereo.
            const double l = clip.sampleAt(0, f), r = clip.sampleAt(clip.channels - 1, f);
            if (!std::isfinite(l) || !std::isfinite(r)) return result;
            sum += std::max(l * l, r * r);
        }
        energy[b] = std::sqrt(sum / static_cast<double>(end - b * hop));
        peak = std::max(peak, energy[b]);
    }
    if (peak < .0001) return result;
    for (std::size_t b = 0; b < blocks; ++b) {
        double baseline = 0;
        const auto begin = b > 4 ? b - 4 : 0;
        for (auto j = begin; j < b; ++j) baseline += energy[j];
        if (b > begin) baseline /= static_cast<double>(b - begin);
        const auto rise = energy[b] - baseline;
        if (rise > std::max(peak * .06, baseline * 1.5)) novelty[b] = rise;
    }
    struct Attack { std::size_t frame; double strength; };
    std::vector<Attack> attacks;
    const auto spacing = static_cast<std::size_t>(std::ceil(clip.sampleRate * .03));
    for (std::size_t b = 0; b < blocks; ++b) {
        if (novelty[b] <= 0 || (b && novelty[b] <= novelty[b - 1])
            || (b + 1 < blocks && novelty[b] < novelty[b + 1])) continue;
        // Locate the rising edge inside the analysis block, before its loud peak.
        auto frame = b * hop;
        const auto end = std::min(clip.frames, frame + hop);
        while (frame + 1 < end && std::max(std::abs(clip.sampleAt(0, frame)),
            std::abs(clip.sampleAt(clip.channels - 1, frame))) < energy[b] * .1) ++frame;
        if (!attacks.empty() && frame - attacks.back().frame < spacing) {
            if (novelty[b] > attacks.back().strength) attacks.back() = {frame, novelty[b]};
        } else attacks.push_back({frame, novelty[b]});
    }
    if (attacks.size() < 2) return result; // one-shot/steady audio: keep the kit
    // Keep the first attack in pad 1 (including any lead-in). Choose the seven
    // strongest remaining boundaries, then restore chronological pad order.
    attacks.erase(attacks.begin());
    std::stable_sort(attacks.begin(), attacks.end(), [](auto a, auto b) { return a.strength > b.strength; });
    if (attacks.size() > drumPadCount - 1) attacks.resize(drumPadCount - 1);
    std::sort(attacks.begin(), attacks.end(), [](auto a, auto b) { return a.frame < b.frame; });
    int start = 0;
    for (const auto attack : attacks) {
        const auto end = static_cast<int>(attack.frame);
        if (end > start) result.ranges[result.count++] = {start, end};
        start = end;
    }
    result.ranges[result.count++] = {start, static_cast<int>(clip.frames)};
    return result;
}
} // namespace alien
