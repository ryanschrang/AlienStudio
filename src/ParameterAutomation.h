#pragma once
#include "Automation.h"
#include <string>
#include <string_view>
namespace alien::automation {
// Persistent instance ID, never an FX slot index. Owners assign a fresh ID on
// insertion/replacement/duplication and preserve it on reorder, save/load and undo.
struct ParameterTarget {
    std::uint64_t instance{};
    std::string parameterId;
    bool operator==(const ParameterTarget&) const = default;
    bool valid() const noexcept {
        return instance != 0 && !parameterId.empty() && parameterId.size() <= 1024
            && parameterId.find('\0') == std::string::npos;
    }
};
struct ParameterLane {
    ParameterTarget target;
    bool enabled{true};
    std::vector<Point> points;
    bool operator==(const ParameterLane&) const = default;
    bool valid(std::int64_t length) const noexcept {
        if (!target.valid() || length < 0 || points.size() > maxPoints) return false;
        std::int64_t previous = -1;
        for (const auto& p : points) {
            if (p.tick < 0 || p.tick > length || p.tick <= previous || !validPoint(p)) return false;
            previous = p.tick;
        }
        return true;
    }
    bool put(Point point) {
        if (point.tick < 0 || !validPoint(point)) return false;
        auto at = std::lower_bound(points.begin(), points.end(), point.tick,
            [](const Point& p, std::int64_t tick) { return p.tick < tick; });
        if (at != points.end() && at->tick == point.tick) {
            if (*at == point) return false;
            *at = point; return true;
        }
        if (points.size() >= maxPoints) return false;
        points.insert(at, point); return true;
    }
    // Normalized 0..1. No allocation, locks, or calls into plugins/listeners.
    float valueAt(double tick, float manual) const noexcept {
        if (!enabled || points.empty() || !std::isfinite(tick) || tick < 0) return manual;
        const auto right = std::upper_bound(points.begin(), points.end(), tick,
            [](double at, const Point& p) { return at < static_cast<double>(p.tick); });
        if (right == points.begin()) return right->value;
        const auto& left = *(right - 1);
        if (right == points.end() || left.curve == Curve::hold) return left.value;
        const auto f = (tick - static_cast<double>(left.tick)) / static_cast<double>(right->tick - left.tick);
        return static_cast<float>(left.value + (right->value - left.value) * f);
    }
private:
    static bool validPoint(const Point& p) noexcept {
        return std::isfinite(p.value) && p.value >= 0 && p.value <= 1
            && (p.curve == Curve::linear || p.curve == Curve::hold);
    }
};
// Rebuilt off the audio thread. Own IDs so snapshots survive metadata reloads.
struct ParameterBinding {
    std::uint64_t instance{};
    std::string parameterId;
    int rack{-1}, slot{-1}, parameter{-1};
};
inline const ParameterBinding* resolve(const ParameterTarget& target,
    const std::vector<ParameterBinding>& bindings) noexcept {
    if (!target.valid()) return nullptr;
    const ParameterBinding* result = nullptr;
    for (const auto& b : bindings) {
        if (b.instance != target.instance || b.parameterId != target.parameterId) continue;
        if (result != nullptr || b.rack < 0 || b.slot < -1 || b.slot >= 8 || b.parameter < 0) return nullptr;
        result = &b;
    }
    return result;
}
} // namespace alien::automation
