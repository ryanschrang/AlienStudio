#pragma once
#include "ProjectDocument.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// Pure document edits for the Song's automation lanes: a track's volume, pan or send lane, or a plugin parameter's
// lane. The controller and command Preview share them, so what Preview predicts is what the live edit does.
// Adding the first point of a lane makes the lane; removing its last point keeps the lane, empty.
namespace alien::automation_edit {
// Which lane: a plugin parameter's when `plugin` names one (its instance is not 0), otherwise track `track`'s
// `parameter` lane.
struct LaneRef {
    int track{-1};
    automation::Parameter parameter{automation::Parameter::volume};
    automation::ParameterTarget plugin;
    bool isPlugin() const noexcept { return plugin.instance != 0; }
    bool operator==(const LaneRef&) const = default;
};

enum class Kind { setRead, addPoint, movePoint, removePoint };
struct Edit {
    Kind kind{Kind::setRead};
    LaneRef lane;
    bool enabled{};            // setRead
    std::int64_t from{};       // movePoint, removePoint: the tick of the point it acts on
    automation::Point point;   // addPoint, movePoint: the point as it ends up
};

inline constexpr std::array<const char*, automation::parametersPerTrack> parameterNames{
    "volume", "pan", "sendA", "sendB", "sendC", "sendD", "sendE", "sendF", "sendG", "sendH"};
inline const char* parameterName(automation::Parameter p) noexcept {
    return automation::validParameter(p) ? parameterNames[static_cast<std::size_t>(p)] : "";
}
inline std::optional<automation::Parameter> parameterNamed(const juce::String& name) {
    for (std::size_t i = 0; i < parameterNames.size(); ++i)
        if (name == parameterNames[i]) return static_cast<automation::Parameter>(i);
    return std::nullopt;
}

inline const automation::Lane* trackLane(const timeline::Timeline& song, const LaneRef& r) {
    for (const auto& lane : song.automationLanes) if (lane.track == r.track && lane.parameter == r.parameter) return &lane;
    return nullptr;
}
inline const automation::ParameterLane* pluginLane(const timeline::Timeline& song, const LaneRef& r) {
    for (const auto& lane : song.parameterLanes) if (lane.target == r.plugin) return &lane;
    return nullptr;
}
inline bool laneExists(const timeline::Timeline& song, const LaneRef& r) {
    return r.isPlugin() ? pluginLane(song, r) != nullptr : trackLane(song, r) != nullptr;
}
// The points of the lane, or none when it does not exist yet.
inline const std::vector<automation::Point>* pointsOf(const timeline::Timeline& song, const LaneRef& r) {
    if (r.isPlugin()) { const auto* lane = pluginLane(song, r); return lane != nullptr ? &lane->points : nullptr; }
    const auto* lane = trackLane(song, r); return lane != nullptr ? &lane->points : nullptr;
}
inline const automation::Point* pointAt(const timeline::Timeline& song, const LaneRef& r, std::int64_t tick) {
    if (const auto* points = pointsOf(song, r))
        for (const auto& p : *points) if (p.tick == tick) return &p;
    return nullptr;
}

// True when the edit would leave the document as it is. Only meaningful for an edit with valid arguments.
inline bool changesNothing(const ProjectDocument& d, const Edit& e) {
    const auto& song = d.timeline;
    switch (e.kind) {
    case Kind::setRead:
        if (e.lane.isPlugin()) { const auto* lane = pluginLane(song, e.lane); return lane != nullptr && lane->enabled == e.enabled; }
        else { const auto* lane = trackLane(song, e.lane); return lane != nullptr && lane->enabled == e.enabled; }
    case Kind::addPoint: { const auto* p = pointAt(song, e.lane, e.point.tick); return p != nullptr && *p == e.point; }
    case Kind::movePoint: { const auto* p = pointAt(song, e.lane, e.from); return p != nullptr && e.from == e.point.tick && *p == e.point; }
    case Kind::removePoint: break;
    }
    return false;
}

inline juce::String laneRefusal(const ProjectDocument& d, const LaneRef& r) {
    if (r.isPlugin()) return r.plugin.valid() ? juce::String{} : juce::String("A plugin lane needs a plugin instance and a parameter id.");
    if (r.track < 0 || r.track >= d.trackCount()) return "There is no track " + juce::String(r.track) + ".";
    if (!automation::validParameter(r.parameter)) return "That is not a track automation parameter.";
    if (const int send = automation::sendIndex(r.parameter); send >= 0 && !d.returns[static_cast<std::size_t>(send)].used)
        return "Send " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + send)) + " automation needs return "
            + juce::String::charToString(static_cast<juce::juce_wchar>('A' + send)) + ".";
    return {};
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (const auto why = laneRefusal(d, e.lane); why.isNotEmpty()) return why;
    const auto& song = d.timeline;
    const bool exists = laneExists(song, e.lane);
    if (e.kind == Kind::setRead) return exists ? juce::String{} : juce::String("That lane has no automation yet.");
    if (e.kind == Kind::movePoint || e.kind == Kind::removePoint) {
        if (pointAt(song, e.lane, e.from) == nullptr) return "There is no automation point at tick " + juce::String(e.from) + " on that lane.";
        if (e.kind == Kind::removePoint) return {};
    }
    const auto& p = e.point;
    if (p.tick < 0 || p.tick > song.length()) return "An automation point must be inside the song.";
    if (p.curve != automation::Curve::linear && p.curve != automation::Curve::hold) return "An automation point's curve is linear (0) or hold (1).";
    if (e.lane.isPlugin() ? !(std::isfinite(p.value) && p.value >= 0.0f && p.value <= 1.0f) : !automation::validValue(e.lane.parameter, p.value)) {
        if (e.lane.isPlugin()) return "A plugin automation value must be from 0 to 1.";
        return "A " + juce::String(parameterName(e.lane.parameter)) + " automation value must be from "
            + juce::String(automation::minimum(e.lane.parameter), 1) + " to " + juce::String(automation::maximum(e.lane.parameter), 1) + ".";
    }
    // A point at a tick that already has one replaces it, so only a point at a new tick needs room.
    const auto* points = pointsOf(song, e.lane);
    const bool replaces = pointAt(song, e.lane, p.tick) != nullptr && !(e.kind == Kind::movePoint && p.tick == e.from);
    const std::size_t count = points != nullptr ? points->size() : 0;
    const std::size_t after = e.kind == Kind::movePoint ? count - (replaces ? 1 : 0) : count + (replaces ? 0 : 1);
    if (after > automation::maxPoints) return "A lane holds at most " + juce::String(static_cast<int>(automation::maxPoints)) + " automation points.";
    if (!exists && e.lane.isPlugin() && song.parameterLanes.size() >= timeline::Timeline::maxParameterLanes)
        return "The song already has " + juce::String(static_cast<int>(timeline::Timeline::maxParameterLanes)) + " plugin automation lanes, the most it can hold.";
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (changesNothing(d, e)) return {};
    auto& song = d.timeline;
    const auto edit = [&](auto& lane) {
        switch (e.kind) {
        case Kind::setRead: lane.enabled = e.enabled; break;
        case Kind::addPoint: lane.put(e.point); break;
        case Kind::movePoint:
            std::erase_if(lane.points, [&](const automation::Point& p) { return p.tick == e.from; });
            lane.put(e.point);
            break;
        case Kind::removePoint: std::erase_if(lane.points, [&](const automation::Point& p) { return p.tick == e.from; }); break;
        }
    };
    if (e.lane.isPlugin()) {
        for (auto& lane : song.parameterLanes) if (lane.target == e.lane.plugin) { edit(lane); return {}; }
        song.parameterLanes.push_back({e.lane.plugin, true, {}});
        edit(song.parameterLanes.back());
        return {};
    }
    for (auto& lane : song.automationLanes) if (lane.track == e.lane.track && lane.parameter == e.lane.parameter) { edit(lane); return {}; }
    song.automationLanes.push_back({e.lane.track, e.lane.parameter, true, {}});
    edit(song.automationLanes.back());
    return {};
}
} // namespace alien::automation_edit
