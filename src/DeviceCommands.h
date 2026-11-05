#pragma once
#include "SidechainCommands.h"

// Pure document edits for the devices in a plugin rack. The controller and command
// Preview share them, so what Preview predicts is what the live edit does. They never
// touch a plugin: the host captures live state first and rebuilds the racks afterwards.
namespace alien::device_edit {
inline constexpr int instrumentSlot = -1;
inline constexpr int maxEffects = 8;

enum class Kind { insert, remove, move, bypass };
struct Edit {
    Kind kind{Kind::remove};
    RackStage stage{RackStage::track};
    int rack{-1};                 // a track, return slot or bus slot; 0 for the master
    int slot{instrumentSlot};     // an FX slot, or the instrument slot on a track
    int to{-1};                   // move: the FX slot it ends up in
    bool bypassed{};              // bypass: the new state
    PluginState device;           // insert: carries its identity, so Preview and execute agree
};

inline bool identityUsed(const ProjectDocument& d, std::uint64_t id) {
    const auto inRack = [id](const PluginRackState& rack) {
        if (rack.instrument.instanceId == id) return true;
        return std::any_of(rack.effects.begin(), rack.effects.end(), [id](const PluginState& effect) { return effect.instanceId == id; });
    };
    if (inRack(d.masterPlugins)) return true;
    for (const auto& track : d.tracks) if (inRack(track.plugins)) return true;
    for (const auto& slot : d.returns) if (slot.used && inRack(slot.plugins)) return true;
    for (const auto& slot : d.buses) if (slot.used && inRack(slot.plugins)) return true;
    return false;
}

inline juce::String missingDevice(const PluginRackState& rack, RackStage stage, int slot) {
    if (slot == instrumentSlot) {
        if (stage != RackStage::track) return "Only a track has an instrument slot.";
        return rack.instrument.descriptionXml.isEmpty() ? "This track has no instrument." : juce::String{};
    }
    return slot >= 0 && slot < static_cast<int>(rack.effects.size()) ? juce::String{} : "There is no effect in FX slot " + juce::String(slot + 1) + ".";
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto* rack = sidechain_edit::rackOf(d, e.stage, e.rack);
    if (rack == nullptr) return "There is no plugin rack " + juce::String(e.rack) + " of that kind.";
    if (e.stage == RackStage::track && d.tracks[static_cast<std::size_t>(e.rack)].settings.frozen()) return "Unfreeze the track before editing its instrument or effects.";
    const int count = static_cast<int>(rack->effects.size());
    if (e.kind == Kind::insert) {
        const auto xml = e.device.descriptionXml.isEmpty() ? nullptr : juce::parseXML(e.device.descriptionXml);
        if (xml == nullptr || !xml->hasTagName("PLUGIN") || !e.device.valid()) return "That is not a usable plugin description.";
        const bool instrument = xml->getBoolAttribute("isInstrument");
        if (e.slot == instrumentSlot) {
            if (e.stage != RackStage::track) return "Only a track has an instrument slot.";
            if (!instrument) return "Select an instrument for the instrument slot, or an FX plugin for Add FX.";
        } else {
            if (instrument) return "Select an instrument for the instrument slot, or an FX plugin for Add FX.";
            if (count >= maxEffects) return "Each rack supports eight FX slots.";
            if (e.slot < 0 || e.slot > count) return "An effect goes in FX slot 1 to " + juce::String(count + 1) + " of this rack.";
        }
        return identityUsed(d, e.device.instanceId) ? "That device is already in the project." : juce::String{};
    }
    if (const auto why = missingDevice(*rack, e.stage, e.slot); why.isNotEmpty()) return why;
    if (e.kind == Kind::move) {
        if (e.slot == instrumentSlot) return "An instrument stays in its own slot.";
        if (e.to < 0 || e.to >= count) return "There is no FX slot " + juce::String(e.to + 1) + " to move to.";
    }
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    auto& rack = *sidechain_edit::rackOf(d, e.stage, e.rack);
    const auto at = [&rack](int slot) { return rack.effects.begin() + slot; };
    switch (e.kind) {
    case Kind::insert:
        if (e.slot == instrumentSlot) rack.instrument = e.device;
        else rack.effects.insert(at(e.slot), e.device);
        break;
    case Kind::remove:
        if (e.slot == instrumentSlot) rack.instrument = {};
        else rack.effects.erase(at(e.slot));
        break;
    case Kind::move: {
        auto moving = rack.effects[static_cast<std::size_t>(e.slot)];
        rack.effects.erase(at(e.slot));
        rack.effects.insert(at(e.to), std::move(moving));
        break;
    }
    case Kind::bypass:
        (e.slot == instrumentSlot ? rack.instrument : rack.effects[static_cast<std::size_t>(e.slot)]).bypass = e.bypassed;
        break;
    }
    return {};
}
} // namespace alien::device_edit
