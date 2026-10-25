#include "ParameterAutomation.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using namespace alien::automation;
    ParameterLane lane{{42, "cutoff"}};
    check(lane.put({100, 1}) && lane.put({0, 0}), "ordered insertion");
    check(lane.valid(100) && lane.valueAt(25, 0.9f) == 0.25f, "normalized interpolation");
    check(lane.valueAt(0, 0.9f) == 0 && lane.valueAt(500, 0.9f) == 1, "endpoint hold");
    check(lane.valueAt(-1, 0.9f) == 0.9f, "inactive transport preserves manual value");
    check(!lane.put({50, 1.01f}) && !lane.put({50, -0.01f})
        && !lane.put({50, std::numeric_limits<float>::quiet_NaN()})
        && !lane.put({50, 0.5f, static_cast<Curve>(99)}), "invalid points rejected");
    check(lane.put({0, 0.2f, Curve::hold}) && lane.valueAt(99, 0) == 0.2f, "hold interpolation");
    lane.enabled = false;
    check(lane.valueAt(50, 0.9f) == 0.9f, "read bypass");
    check(!lane.valid(99), "off-song points rejected");
    auto damaged = lane; damaged.points.push_back({50, 0.5f});
    check(!damaged.valid(100), "unsorted data rejected");
    damaged = lane; damaged.target.instance = 0;
    check(!damaged.valid(100), "unassigned instance rejected");
    damaged = lane; damaged.target.parameterId = std::string("cut\0off", 7);
    check(!damaged.valid(100), "embedded NUL rejected");
    std::vector<ParameterBinding> bindings{{42, "cutoff", 0, 0, 2}, {43, "cutoff", 0, 1, 2}};
    check(resolve(lane.target, bindings) == &bindings[0], "duplicate plugin types stay distinct");
    bindings[0].slot = 3; bindings[0].parameter = 7;
    check(resolve(lane.target, bindings)->slot == 3 && resolve(lane.target, bindings)->parameter == 7,
        "effect and parameter reorder retains identity");
    bindings.erase(bindings.begin());
    check(resolve(lane.target, bindings) == nullptr, "replacement never inherits automation");
    bindings.push_back({42, "resonance", 0, 0, 2});
    check(resolve(lane.target, bindings) == nullptr, "missing parameter remains unresolved");
    bindings.push_back({42, "cutoff", 0, 0, 2});
    bindings.push_back({42, "cutoff", 0, 1, 2});
    check(resolve(lane.target, bindings) == nullptr, "ambiguous binding fails closed");
    ParameterLane full{{7, "gain"}};
    for (std::size_t i = 0; i < maxPoints; ++i) check(full.put({static_cast<std::int64_t>(i), 0.5f}), "fill lane");
    check(!full.put({static_cast<std::int64_t>(maxPoints), 0.5f}) && full.put({0, 0.25f}), "point budget");
    std::puts("Parameter automation tests passed");
}
