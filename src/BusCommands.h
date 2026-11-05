#pragma once
#include "ProjectDocument.h"

// Pure document edits for group buses. The controller and command Preview share them,
// so what Preview predicts is what the live edit does.
namespace alien::bus_edit {
enum class Kind { create, rename, remove };
struct Edit { Kind kind{}; int slot{-1}; juce::String name; };

inline bool slotInUse(const ProjectDocument& d, int slot) {
    return slot >= 0 && slot < maxBuses && d.buses[static_cast<std::size_t>(slot)].used;
}
inline int firstFreeSlot(const ProjectDocument& d) {
    for (int b = 0; b < maxBuses; ++b) if (!d.buses[static_cast<std::size_t>(b)].used) return b;
    return -1;
}
inline juce::String storedName(const juce::String& name) { return name.trim().substring(0, 64); }
// The lowest "Bus n" no bus is using.
inline juce::String nextName(const ProjectDocument& d) {
    for (int n = 1;; ++n) {
        bool taken = false;
        for (const auto& bus : d.buses) taken = taken || (bus.used && bus.name == "Bus " + juce::String(n));
        if (!taken) return "Bus " + juce::String(n);
    }
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.kind == Kind::create) {
        if (!d.trackRegistry.canAllocate()) return "The project cannot allocate another track identity.";
        return firstFreeSlot(d) >= 0 ? juce::String{} : "The project already has the most buses allowed (" + juce::String(maxBuses) + ").";
    }
    if (!slotInUse(d, e.slot)) return "Bus slot " + juce::String(e.slot) + " is not in use.";
    if (e.kind == Kind::rename && storedName(e.name).isEmpty()) return "A bus name cannot be blank.";
    return {};
}

// Deleting a bus sends its tracks back to the master and drops its FX and any sidechain
// keyed by its output. Other buses keep their slots.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (e.kind == Kind::create) {
        const auto name = storedName(e.name);
        const auto label = name.isNotEmpty() ? name : nextName(d);
        d.buses[static_cast<std::size_t>(firstFreeSlot(d))] = {true, label, {}, {}};
    } else if (e.kind == Kind::rename) {
        d.buses[static_cast<std::size_t>(e.slot)].name = storedName(e.name);
    } else {
        for (auto& track : d.tracks) if (track.settings.output == e.slot) track.settings.output = -1;
        d.buses[static_cast<std::size_t>(e.slot)] = {};
        d.sidechainBusDeleted(e.slot);
    }
    return {};
}
} // namespace alien::bus_edit
