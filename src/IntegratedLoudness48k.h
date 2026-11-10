#pragma once

#include "StockDsp.h"
#include <optional>
#include <vector>

namespace alien {
// Offline stereo measurement. ITU-R BS.1770-5 Annex 1, tables 1 and 2.
class IntegratedLoudness48k {
public:
    void push(double left, double right) {
        const double l = filter(0, left);
        const double r = filter(1, right);
        chunkEnergy += l * l + r * r;
        if (++framesInChunk < 4800) return;

        // Four adjacent 100 ms chunks give a 400 ms block. Advancing one
        // chunk per measurement gives the required 75% overlap.
        chunks[nextChunk] = chunkEnergy;
        nextChunk = (nextChunk + 1) % chunks.size();
        chunkEnergy = 0;
        framesInChunk = 0;
        if (completeChunks < chunks.size()) ++completeChunks;
        if (completeChunks < chunks.size()) return;
        const double blockPower = (chunks[0] + chunks[1] + chunks[2] + chunks[3]) / 19200.0;
        if (blockPower > absoluteGatePower) {
            aboveAbsoluteGate.push_back(blockPower);
            absolutePowerSum += blockPower;
        }
    }

    std::optional<double> lufs() const noexcept {
        if (aboveAbsoluteGate.empty()) return {};
        const double relativeGate = 0.1 * absolutePowerSum / static_cast<double>(aboveAbsoluteGate.size());
        double sum = 0;
        std::size_t count = 0;
        for (double power : aboveAbsoluteGate) {
            if (power <= relativeGate) continue;
            sum += power;
            ++count;
        }
        return -0.691 + 10.0 * std::log10(sum / static_cast<double>(count));
    }

private:
    static constexpr dsp::BiquadCoefficients shelf{
        1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
    static constexpr dsp::BiquadCoefficients highPass{1, -2, 1, -1.99004745483398, 0.99007225036621};
    const double absoluteGatePower = std::pow(10.0, (-70.0 + 0.691) / 10.0);

    double filter(std::size_t channel, double input) noexcept {
        return highPassState[channel].process(highPass, shelfState[channel].process(shelf, input));
    }

    std::array<dsp::BiquadState, 2> shelfState{}, highPassState{};
    std::array<double, 4> chunks{};
    std::vector<double> aboveAbsoluteGate;
    double chunkEnergy{}, absolutePowerSum{};
    int framesInChunk{};
    std::size_t nextChunk{}, completeChunks{};
};
} // namespace alien
