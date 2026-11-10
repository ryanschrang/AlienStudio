#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace alien {
// Offline true-peak estimator for one 48 kHz channel. ITU-R BS.1770-5,
// Annex 2, four interpolation phases, with exact coefficients scaled by 8192.
class TruePeak48k {
public:
    void push(double sample) noexcept {
        newest = newest == 0 ? taps - 1 : newest - 1;
        history[newest] = history[newest + taps] = sample;
        maximum = std::max(maximum, std::abs(sample));
        for (const auto& phase : coefficients) {
            double interpolated = 0;
            for (int tap = 0; tap < taps; ++tap)
                interpolated += phase[tap] * history[newest + tap];
            maximum = std::max(maximum, std::abs(interpolated) / 8192.0);
        }
    }

    double peak() const noexcept {
        auto finished = *this;
        for (int i = 1; i < taps; ++i) finished.push(0);
        return finished.maximum;
    }

private:
    static constexpr int taps = 12;
    static constexpr int coefficients[4][taps] = {
        { 14, 90, -161, 272, -487, 1125, 7964, -838, 390, -218, 122, -68 },
        { -239, 240, -424, 730, -1364, 3810, 6388, -1641, 832, -477, 271, -155 },
        { -155, 271, -477, 832, -1641, 6388, 3810, -1364, 730, -424, 240, -239 },
        { -68, 122, -218, 390, -838, 7964, 1125, -487, 272, -161, 90, 14 },
    };
    std::array<double, taps * 2> history{};
    int newest{};
    double maximum{};
};
} // namespace alien
