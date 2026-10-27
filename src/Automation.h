#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "SessionCapacity.h"

namespace alien::automation {
// A send's lane follows its return's slot (sendA is always return A), so removing a return never moves another's curve.
enum class Parameter : int { volume = 0, pan = 1, sendA = 2, sendB = 3, sendC = 4, sendD = 5, sendE = 6, sendF = 7, sendG = 8, sendH = 9 };
inline constexpr int parametersPerTrack = 2 + maxReturns;
static_assert(static_cast<int>(Parameter::sendH) == parametersPerTrack - 1, "one send lane per return slot");
inline bool validParameter(Parameter p) noexcept { return static_cast<int>(p) >= 0 && static_cast<int>(p) < parametersPerTrack; }
inline int sendIndex(Parameter p) noexcept { return static_cast<int>(p) >= 2 && validParameter(p) ? static_cast<int>(p) - 2 : -1; }
inline float minimum(Parameter p) noexcept { return p == Parameter::pan ? -1.0f : 0.0f; }
inline float maximum(Parameter p) noexcept { return p == Parameter::volume ? 2.0f : 1.0f; }
enum class Curve : int { linear = 0, hold = 1 };
inline constexpr std::size_t maxPoints = 4096;
struct Point {
    std::int64_t tick{};
    float value{};
    Curve curve{Curve::linear}; // interpolation from this point to the next
    bool operator==(const Point&) const = default;
};
inline bool validValue(Parameter parameter, float value) noexcept {
    return validParameter(parameter) && std::isfinite(value) && value >= minimum(parameter) && value <= maximum(parameter);
}
struct Lane {
    int track{};
    Parameter parameter{Parameter::volume};
    bool enabled{true};
    std::vector<Point> points;
    bool operator==(const Lane&) const = default;
    bool valid(int tracks, std::int64_t length) const noexcept {
        if (track < 0 || track >= tracks || !validParameter(parameter)
            || points.size() > maxPoints) return false;
        std::int64_t last = -1;
        for (const auto& p : points) {
            if (p.tick < 0 || p.tick > length || p.tick <= last || !validValue(parameter, p.value)
                || (p.curve != Curve::linear && p.curve != Curve::hold)) return false;
            last = p.tick;
        }
        return true;
    }
    // Read-only, allocation-free and independent of the audio block size. Before
    // the first point and after the last point, hold the nearest endpoint.
    float valueAt(double tick, float fallback) const noexcept {
        if (!enabled || points.empty() || !std::isfinite(tick)) return fallback;
        const auto right = std::upper_bound(points.begin(), points.end(), tick,
            [](double at, const Point& p) { return at < static_cast<double>(p.tick); });
        if (right == points.begin()) return right->value;
        const auto& left = *(right - 1);
        if (right == points.end() || left.curve == Curve::hold) return left.value;
        const double amount = (tick - static_cast<double>(left.tick)) / static_cast<double>(right->tick - left.tick);
        return static_cast<float>(left.value + (right->value - left.value) * amount);
    }
    // Message-thread editing only. A point at an existing tick replaces it.
    bool put(Point point) {
        if (point.tick < 0 || !validValue(parameter, point.value)
            || (point.curve != Curve::linear && point.curve != Curve::hold)) return false;
        auto at = std::lower_bound(points.begin(), points.end(), point.tick,
            [](const Point& p, std::int64_t tick) { return p.tick < tick; });
        if (at != points.end() && at->tick == point.tick) {
            if (*at == point) return false;
            *at = point; return true;
        }
        if (points.size() >= maxPoints) return false;
        points.insert(at, point); return true;
    }
};
} // namespace alien::automation
