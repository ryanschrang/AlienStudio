#include "Automation.h"
#include "TrackMixer.h"
#include "Timeline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
static void check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
int main() {
    using namespace alien::automation;
    Lane lane{0, Parameter::volume, true, {}};
    check(lane.put({0, 0.0f}) && lane.put({100, 2.0f}), "create linear lane");
    check(lane.valueAt(25.0, 1.0f) == 0.5f && lane.valueAt(100.0, 1.0f) == 2.0f, "linear endpoints and interpolation");
    check(lane.valueAt(-1.0, 1.0f) == 0.0f && lane.valueAt(1000.0, 1.0f) == 2.0f, "endpoint hold");
    check(lane.put({0, 0.0f, Curve::hold}) && lane.valueAt(99.9, 1.0f) == 0.0f, "hold curve");
    check(!lane.put({50, std::numeric_limits<float>::quiet_NaN()}) && lane.points.size() == 2, "NaN rejected");
    check(!lane.put({-1, 1.0f}) && !lane.put({20, 3.0f}), "invalid time/value rejected");
    lane.enabled = false; check(lane.valueAt(50, 0.75f) == 0.75f, "disabled lane restores manual value");
    lane.enabled = true;
    alien::timeline::Timeline t; t.automationLanes = {lane};
    check(t.valid(), "timeline accepts valid automation");
    auto b = t; b.automationLanes.push_back(lane); check(!b.valid(), "duplicate targets rejected");
    b = t; b.automationLanes[0].points.push_back({50, 0.5f}); check(!b.valid(), "unsorted points rejected");
    b = t; b.automationLanes[0].points[1].tick = t.length() + 1; check(!b.valid(), "off-song points rejected");
    b = t; b.automationLanes[0].enabled = false; check(!t.playbackEquals(b), "automation republishes playback");
    b = t; b.tracksSwapped(0, 1); check(b.automationLanes[0].track == 1, "lanes follow track reorder");
    b.trackDeleted(0); check(b.automationLanes[0].track == 0, "lanes follow renumbered tracks");
    b.trackDeleted(0); check(b.automationLanes.empty(), "deleting track deletes its automation");
    alien::TrackMixer mixer; mixer.prepare(48000);
    alien::TrackSettings settings; settings.gain = 0.25f; settings.pan = 0.5f;
    float l = 1.0f, r = 1.0f; mixer.processAutomated(l, r, settings, false);
    check(l == 0.125f && r == 0.25f, "automation applies the exact sample without extra smoothing");
    settings.mute = true; l = r = 1; mixer.processAutomated(l, r, settings, false);
    check(l == 0 && r == 0, "mute still overrides automation");
    for (int k = 0; k < alien::maxReturns; ++k) {   // one send lane per return slot, A-H
        const auto parameter = static_cast<Parameter>(2 + k);
        Lane send{0, parameter, true, {}};
        check(sendIndex(parameter) == k && send.put({0, 0.0f}) && send.put({100, 1.0f}), "send targets and endpoints");
        check(!send.put({50, -0.1f}) && !send.put({50, 1.1f}), "send bounds rejected");
        check(send.valueAt(25, 0) == 0.25f, "send interpolation");
        t.automationLanes.push_back(send);
    }
    t.automationLanes.push_back({0, Parameter::pan, true, {{0, 0}}});
    check(t.valid(), "every parameter fits on a track");
    auto invalid = t; invalid.automationLanes.back().parameter = static_cast<Parameter>(alien::automation::parametersPerTrack);
    check(!invalid.valid() && !validValue(static_cast<Parameter>(alien::automation::parametersPerTrack), 0), "unknown target rejected even with a valid value");
    alien::SendRamp ramp; ramp.prepare(48000); ramp.setTarget(0.25f);
    check(ramp.automated(1) == 1, "automation applies without smoothing");
    const float firstManual = ramp.next();
    check(firstManual < 1 && firstManual > 0.25f, "read bypass glides from automation toward manual");
    for (int i = 0; i < 10000; ++i) ramp.next();
    check(std::abs(ramp.next() - 0.25f) < 0.00001f, "manual target remains intact");
    std::puts("Automation tests passed");
}
