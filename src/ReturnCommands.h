#pragma once
#include "ProjectDocument.h"
#include <algorithm>

// Pure document edits for return tracks. The controller and command Preview share
// them, so what Preview predicts is what the live edit does.
namespace alien::return_edit {
enum class Kind { create, rename, remove };
struct Edit { Kind kind{}; int slot{-1}; juce::String name; };

inline bool slotInUse(const ProjectDocument& d, int slot) {
    return slot >= 0 && slot < maxReturns && d.returns[static_cast<std::size_t>(slot)].used;
}
inline int firstFreeSlot(const ProjectDocument& d) {
    for (int r = 0; r < maxReturns; ++r) if (!d.returns[static_cast<std::size_t>(r)].used) return r;
    return -1;
}
inline juce::String storedName(const juce::String& name) { return name.trim().substring(0, 64); }

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.kind == Kind::create) {
        if (!d.trackRegistry.canAllocate()) return "The project cannot allocate another track identity.";
        return firstFreeSlot(d) >= 0 ? juce::String{} : "The project already has the most returns allowed (" + juce::String(maxReturns) + ").";
    }
    if (!slotInUse(d, e.slot)) return "Return slot " + juce::String(e.slot) + " is not in use.";
    if (e.kind == Kind::rename && storedName(e.name).isEmpty()) return "A return name cannot be blank.";
    return {};
}

// Removing a return turns every track's send to it off and drops its send automation,
// its FX and any sidechain keyed by its output. Other returns keep their slots.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (e.kind == Kind::create) {
        const int r = firstFreeSlot(d);
        const auto name = storedName(e.name);
        d.returns[static_cast<std::size_t>(r)] = {true, name.isNotEmpty() ? name : defaultReturnName(r), {}, {}};
    } else if (e.kind == Kind::rename) {
        d.returns[static_cast<std::size_t>(e.slot)].name = storedName(e.name);
    } else {
        const auto slot = static_cast<std::size_t>(e.slot);
        for (auto& track : d.tracks) { track.settings.send[slot] = 0.0f; track.settings.sendPre[slot] = false; }
        auto& lanes = d.timeline.automationLanes;
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&e](const auto& lane) { return automation::sendIndex(lane.parameter) == e.slot; }), lanes.end());
        d.returns[slot] = {};
        d.sidechainReturnDeleted(e.slot);
    }
    return {};
}
} // namespace alien::return_edit
