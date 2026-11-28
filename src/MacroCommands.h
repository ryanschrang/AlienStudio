#pragma once
#include "SidechainCommands.h"

// Pure document edits for a plugin rack's eight macros (RACK-01, T2-02). The controller and command Preview share
// them, so what Preview predicts is what the live edit does. They change only the document: the host then sets the
// mapped parameters of the loaded plugins from the macros (PluginHost::applyMacros).
namespace alien::macro_edit {
enum class Kind { value, rename, map, unmap };
struct Edit {
    Kind kind{Kind::value};
    RackStage stage{RackStage::track};
    int rack{-1};          // a track, return slot or bus slot; 0 for the master
    int macro{-1};         // 0 to 7
    double value{};        // value: 0 to 1
    juce::String name;     // rename: empty gives the macro its default name again
    MacroTarget target;    // map: the parameter and its range; unmap: the parameter (its range is ignored)
};

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto* rack = sidechain_edit::rackOf(d, e.stage, e.rack);
    if (rack == nullptr) return "There is no plugin rack " + juce::String(e.rack) + " of that kind.";
    if (e.macro < 0 || e.macro >= rackMacroCount) return "Macros are numbered 0 to " + juce::String(rackMacroCount - 1) + ".";
    if (e.stage == RackStage::track && d.tracks[static_cast<std::size_t>(e.rack)].settings.frozen()) return "Unfreeze the track before changing its macros.";
    const auto& macro = rack->macros[static_cast<std::size_t>(e.macro)];
    switch (e.kind) {
    case Kind::value:
        if (!std::isfinite(e.value) || e.value < 0 || e.value > 1) return "A macro's value must be a finite number from 0 to 1.";
        break;
    case Kind::rename:
        if (e.name.trim().length() > maxMacroNameChars) return "A macro name holds at most " + juce::String(maxMacroNameChars) + " characters.";
        break;
    case Kind::map: {
        if (!rack->hasDevice(e.target.instance)) return "That device is not in this rack.";
        if (!e.target.valid()) return "A macro target needs a parameter ID and a range from 0 to 1.";
        const auto same = [&](const MacroTarget& t) { return t.instance == e.target.instance && t.parameterId == e.target.parameterId; };
        for (int m = 0; m < rackMacroCount; ++m)
            if (m != e.macro && std::any_of(rack->macros[static_cast<std::size_t>(m)].targets.begin(), rack->macros[static_cast<std::size_t>(m)].targets.end(), same))
                return "That parameter already follows " + macroDisplayName(rack->macros[static_cast<std::size_t>(m)], m) + ".";
        if (std::none_of(macro.targets.begin(), macro.targets.end(), same) && macro.targets.size() >= static_cast<std::size_t>(maxMacroTargets))
            return "A macro moves at most " + juce::String(maxMacroTargets) + " parameters.";
        break;
    }
    case Kind::unmap:
        if (std::none_of(macro.targets.begin(), macro.targets.end(), [&](const MacroTarget& t) { return t.instance == e.target.instance && t.parameterId == e.target.parameterId; }))
            return macroDisplayName(macro, e.macro) + " does not move that parameter.";
        break;
    }
    return {};
}

// Changes `d` (an edit that changes nothing is accepted), or says why it cannot.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    auto& macro = sidechain_edit::rackOf(d, e.stage, e.rack)->macros[static_cast<std::size_t>(e.macro)];
    const auto same = [&](const MacroTarget& t) { return t.instance == e.target.instance && t.parameterId == e.target.parameterId; };
    switch (e.kind) {
    case Kind::value: macro.value = static_cast<float>(e.value); break;
    case Kind::rename: macro.name = e.name.trim(); break;
    case Kind::map:
        if (const auto found = std::find_if(macro.targets.begin(), macro.targets.end(), same); found != macro.targets.end()) *found = e.target;
        else macro.targets.push_back(e.target);
        break;
    case Kind::unmap: std::erase_if(macro.targets, same); break;
    }
    return {};
}
} // namespace alien::macro_edit
