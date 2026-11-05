#pragma once
#include "ProjectDocument.h"

// Pure document edit for an effect's sidechain key. The controller and command Preview
// share it, so what Preview predicts is what the live edit does. It never touches a
// plugin: the host rebuilds the racks afterwards, and an effect with no sidechain input
// simply stays unkeyed when it does.
namespace alien::sidechain_edit {
struct Edit {
    RackStage stage{RackStage::track};
    int rack{-1};                       // a track, return slot or bus slot; 0 for the master
    int effect{-1};                     // the FX slot in that rack
    KeySource from{KeySource::track};
    int source{-1};                     // a track, bus slot or return slot; -1 for no key
};

template <class Document>
auto* rackOf(Document& d, RackStage stage, int rack) {
    decltype(&d.masterPlugins) found = nullptr;
    if (stage == RackStage::master) { if (rack == 0) found = &d.masterPlugins; }
    else if (stage == RackStage::track) { if (rack >= 0 && rack < d.trackCount()) found = &d.tracks[static_cast<std::size_t>(rack)].plugins; }
    else if (stage == RackStage::returnTrack) { if (rack >= 0 && rack < maxReturns && d.returns[static_cast<std::size_t>(rack)].used) found = &d.returns[static_cast<std::size_t>(rack)].plugins; }
    else if (rack >= 0 && rack < maxBuses && d.buses[static_cast<std::size_t>(rack)].used) found = &d.buses[static_cast<std::size_t>(rack)].plugins;
    return found;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto* rack = rackOf(d, e.stage, e.rack);
    if (rack == nullptr) return "There is no plugin rack " + juce::String(e.rack) + " of that kind.";
    if (e.stage == RackStage::track && d.tracks[static_cast<std::size_t>(e.rack)].settings.frozen()) return "Unfreeze the track before editing its effects.";
    if (e.effect < 0 || e.effect >= static_cast<int>(rack->effects.size()) || rack->effects[static_cast<std::size_t>(e.effect)].descriptionXml.isEmpty())
        return "There is no effect in FX slot " + juce::String(e.effect + 1) + ".";
    if (e.source == -1) return {};
    if (e.source < -1) return "A sidechain source is a slot number, or -1 for no key.";
    if (!keyReady(e.stage, e.from)) return "That key is not ready when this rack runs: the mix makes it later.";
    if (e.from == KeySource::bus) return e.source < maxBuses && d.buses[static_cast<std::size_t>(e.source)].used ? juce::String{} : "Bus slot " + juce::String(e.source) + " is not in use.";
    if (e.from == KeySource::returnTrack) return e.source < maxReturns && d.returns[static_cast<std::size_t>(e.source)].used ? juce::String{} : "Return slot " + juce::String(e.source) + " is not in use.";
    if (e.source >= d.trackCount()) return "There is no track " + juce::String(e.source + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    if (e.stage == RackStage::track && e.from == KeySource::track && e.source == e.rack) return "An effect cannot be keyed by its own track.";
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    auto& effect = rackOf(d, e.stage, e.rack)->effects[static_cast<std::size_t>(e.effect)];
    effect.sidechain = e.source;
    effect.sidechainFrom = e.source < 0 ? KeySource::track : e.from;
    return {};
}
} // namespace alien::sidechain_edit
