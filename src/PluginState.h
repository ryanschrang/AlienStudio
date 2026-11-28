#pragma once
#include "SessionCapacity.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace alien {
// Per-plugin limit on saved state, in base64 characters (about 18 MiB of binary
// data). Sampler-style plugins can embed megabytes of data; the whole project
// file must still stay under the 128 MiB limit enforced when saving.
inline constexpr int maxPluginStateChars = 24 * 1024 * 1024;

// What keys an effect's sidechain input (MIX-04): a Session track before its own FX, a track after its
// fader and pan (what it sends on to its bus or the master), a group bus's output or a return's output.
enum class KeySource : std::uint8_t { track, trackPostFader, bus, returnTrack };
// Where in the mix a rack runs. Each block runs the tracks' FX, then the faders, then the buses' FX,
// then the returns' FX, then the master's, so a key is ready only for the stages after it is made.
enum class RackStage { track, bus, returnTrack, master };
inline bool keyReady(RackStage stage, KeySource from) noexcept {
    switch (stage) {
    case RackStage::track: return from == KeySource::track;
    case RackStage::bus: return from == KeySource::track || from == KeySource::trackPostFader;
    case RackStage::returnTrack: return from != KeySource::returnTrack;
    case RackStage::master: return true;
    }
    return false;
}
// How many keys of a kind there can be (the index is a track, a bus slot or a return slot).
inline int keySlots(KeySource from) noexcept {
    return from == KeySource::bus ? maxBuses : from == KeySource::returnTrack ? maxReturns : maxSessionTracks;
}
inline const char* keySourceName(KeySource from) noexcept {
    switch (from) {
    case KeySource::track: return "track";
    case KeySource::trackPostFader: return "post";
    case KeySource::bus: return "bus";
    case KeySource::returnTrack: return "return";
    }
    return "track";
}

struct PluginState {
    juce::String descriptionXml, stateBase64;
    bool bypass{};
    std::uint64_t instanceId{};
    // An effect with a sidechain input: the index of what keys it (see sidechainFrom), or -1 for none
    // (the effect hears only its own input).
    int sidechain{-1};
    KeySource sidechainFrom{KeySource::track};   // a track before its FX unless saved otherwise (format 25)
    PluginState() = default;
    PluginState(juce::String description, juce::String state, bool bypassed = false)
        : descriptionXml(std::move(description)), stateBase64(std::move(state)), bypass(bypassed) {
        if (descriptionXml.isNotEmpty())
            do { instanceId = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64()); } while (instanceId == 0);
    }
    bool operator==(const PluginState&) const = default;
    bool valid() const {
        return (descriptionXml.isEmpty() ? instanceId == 0 && sidechain == -1 : instanceId != 0) && sidechain >= -1 && sidechain < keySlots(sidechainFrom)
            && (sidechain >= 0 || sidechainFrom == KeySource::track)
            && descriptionXml.length() <= 65536 && stateBase64.length() <= maxPluginStateChars
            && (descriptionXml.isEmpty() ? stateBase64.isEmpty() : juce::parseXML(descriptionXml) != nullptr);
    }
};
inline juce::String pluginDisplayName(const PluginState& p) {
    if (auto xml = juce::parseXML(p.descriptionXml)) {
        const auto name = xml->getStringAttribute("name");
        if (name.isNotEmpty()) return name;
    }
    return "an unnamed plugin";
}
// A rack macro (RACK-01, T2-02, format 39): one knob that moves parameters of the rack's devices together. Each
// target is a device in the same rack, by identity, and one of its parameters by persistent ID; the macro's value 0
// to 1 sets that parameter to `from` + value x (`to` - `from`), in the parameter's own 0 to 1 range, so a target
// can also run backwards. A target whose device has left the rack does nothing.
inline constexpr int rackMacroCount = 8, maxMacroTargets = 16, maxMacroNameChars = 32, maxParameterIdChars = 256;
struct MacroTarget {
    std::uint64_t instance{};
    juce::String parameterId;
    float from{0}, to{1};
    bool operator==(const MacroTarget&) const = default;
    bool valid() const {
        return instance != 0 && parameterId.isNotEmpty() && parameterId.length() <= maxParameterIdChars
            && std::isfinite(from) && std::isfinite(to) && from >= 0 && from <= 1 && to >= 0 && to <= 1;
    }
    float valueAt(float macro) const noexcept { return from + std::clamp(macro, 0.0f, 1.0f) * (to - from); }
};
struct RackMacro {
    juce::String name;   // empty: "Macro N"
    float value{};
    std::vector<MacroTarget> targets;
    bool operator==(const RackMacro&) const = default;
    bool isDefault() const { return name.isEmpty() && value == 0 && targets.empty(); }
    bool valid() const {
        if (name.length() > maxMacroNameChars || !std::isfinite(value) || value < 0 || value > 1
            || targets.size() > static_cast<std::size_t>(maxMacroTargets)) return false;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (!targets[i].valid()) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (targets[j].instance == targets[i].instance && targets[j].parameterId == targets[i].parameterId) return false;
        }
        return true;
    }
};
inline juce::String macroDisplayName(const RackMacro& macro, int index) { return macro.name.isNotEmpty() ? macro.name : "Macro " + juce::String(index + 1); }
using RackMacros = std::array<RackMacro, rackMacroCount>;

struct PluginRackState {
    PluginState instrument;
    std::vector<PluginState> effects;
    RackMacros macros{};
    bool operator==(const PluginRackState&) const = default;
    bool hasDevice(std::uint64_t instance) const {
        if (instance == 0) return false;
        if (instrument.instanceId == instance) return true;
        return std::any_of(effects.begin(), effects.end(), [instance](const PluginState& e) { return e.instanceId == instance; });
    }
    // The macros' targets on devices no longer in the rack go (after a device is removed or replaced).
    bool dropStaleMacroTargets() {
        bool changed = false;
        for (auto& macro : macros)
            changed = std::erase_if(macro.targets, [this](const MacroTarget& t) { return !hasDevice(t.instance); }) > 0 || changed;
        return changed;
    }
    // The same rack without its macros: what the plugin host loads (macros are applied to the loaded plugins after).
    PluginRackState devicesOnly() const { auto copy = *this; copy.macros = {}; return copy; }
    bool valid() const {
        if (!instrument.valid() || effects.size() > 8) return false;
        for (const auto& effect : effects) if (!effect.valid() || effect.descriptionXml.isEmpty()) return false;
        for (int m = 0; m < rackMacroCount; ++m) {
            const auto& macro = macros[static_cast<std::size_t>(m)];
            if (!macro.valid()) return false;
            // One parameter follows one macro at most, so two knobs never fight over it.
            for (int n = 0; n < m; ++n)
                for (const auto& a : macros[static_cast<std::size_t>(n)].targets)
                    for (const auto& b : macro.targets) if (a.instance == b.instance && a.parameterId == b.parameterId) return false;
        }
        return true;
    }
};
// Name of the first plugin in the rack whose saved state exceeds the limit, or
// an empty string when every state fits. Lets save errors point at the culprit.
inline juce::String oversizedPluginName(const PluginRackState& rack) {
    if (rack.instrument.stateBase64.length() > maxPluginStateChars) return pluginDisplayName(rack.instrument);
    for (const auto& effect : rack.effects)
        if (effect.stateBase64.length() > maxPluginStateChars) return pluginDisplayName(effect);
    return {};
}
// The four return tracks' FX racks (effects only, like the master's).
using ReturnRacks = std::array<PluginRackState, maxReturns>;
// The group buses' FX racks (effects only), one per bus slot whether or not the bus exists.
using BusRacks = std::array<PluginRackState, maxBuses>;

// Hex strings preserve all 64 bits through JSON implementations that use doubles.
inline juce::String instanceIdString(std::uint64_t id) {
    return juce::String::toHexString(static_cast<juce::int64>(id)).paddedLeft('0', 16);
}
inline bool parseInstanceId(const juce::var& value, std::uint64_t& id) {
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.length() != 16 || !text.containsOnly("0123456789abcdefABCDEF")) return false;
    id = static_cast<std::uint64_t>(text.getHexValue64()); return true;
}
inline juce::var pluginToVar(const PluginState& p) {
    auto value = juce::var(new juce::DynamicObject()); auto* o = value.getDynamicObject();
    o->setProperty("description", p.descriptionXml); o->setProperty("state", p.stateBase64); o->setProperty("bypass", p.bypass);
    o->setProperty("instance", instanceIdString(p.instanceId));
    if (p.sidechain >= 0) o->setProperty("sidechain", p.sidechain);
    if (p.sidechain >= 0 && p.sidechainFrom != KeySource::track) o->setProperty("sidechainFrom", keySourceName(p.sidechainFrom));
    return value;
}
inline juce::var rackToVar(const PluginRackState& rack) {
    auto value = juce::var(new juce::DynamicObject()); auto* o = value.getDynamicObject();
    o->setProperty("instrument", pluginToVar(rack.instrument));
    juce::Array<juce::var> fx; for (const auto& p : rack.effects) fx.add(pluginToVar(p));
    o->setProperty("effects", juce::var(fx));
    // Format 39: written only when a macro is named, moved or mapped.
    if (std::any_of(rack.macros.begin(), rack.macros.end(), [](const RackMacro& m) { return !m.isDefault(); })) {
        juce::Array<juce::var> macros;
        for (const auto& m : rack.macros) {
            auto macro = juce::var(new juce::DynamicObject()); auto* mo = macro.getDynamicObject();
            if (m.name.isNotEmpty()) mo->setProperty("name", m.name);
            mo->setProperty("value", static_cast<double>(m.value));
            juce::Array<juce::var> targets;
            for (const auto& t : m.targets) {
                auto target = juce::var(new juce::DynamicObject()); auto* to = target.getDynamicObject();
                to->setProperty("instance", instanceIdString(t.instance)); to->setProperty("parameter", t.parameterId);
                to->setProperty("from", static_cast<double>(t.from)); to->setProperty("to", static_cast<double>(t.to));
                targets.add(target);
            }
            mo->setProperty("targets", targets);
            macros.add(macro);
        }
        o->setProperty("macros", macros);
    }
    return value;
}
inline bool pluginFromVar(const juce::var& value, PluginState& p) {
    const auto* o = value.getDynamicObject(); if (!o) return false;
    auto d = o->getProperty("description"), s = o->getProperty("state"), b = o->getProperty("bypass");
    if (!d.isString() || !s.isString() || !b.isBool()) return false;
    PluginState parsed{d.toString(), s.toString(), static_cast<bool>(b)};
    // Legacy states acquire an identity once, on load. Copies and undo preserve it.
    if (o->hasProperty("instance") && !parseInstanceId(o->getProperty("instance"), parsed.instanceId)) return false;
    // Absent before project format 21: no sidechain.
    if (o->hasProperty("sidechain")) {
        if (!o->getProperty("sidechain").isInt()) return false;
        parsed.sidechain = static_cast<int>(o->getProperty("sidechain"));
    }
    // Absent before format 25: a track before its FX.
    if (o->hasProperty("sidechainFrom")) {
        const auto from = o->getProperty("sidechainFrom").toString();
        if (!o->getProperty("sidechainFrom").isString() || parsed.sidechain < 0) return false;
        if (from == "post") parsed.sidechainFrom = KeySource::trackPostFader;
        else if (from == "bus") parsed.sidechainFrom = KeySource::bus;
        else if (from == "return") parsed.sidechainFrom = KeySource::returnTrack;
        else if (from != "track") return false;
    }
    if (!parsed.valid()) return false;
    p = std::move(parsed); return true;
}
inline bool rackFromVar(const juce::var& value, PluginRackState& rack) {
    const auto* o = value.getDynamicObject(); if (!o || !pluginFromVar(o->getProperty("instrument"), rack.instrument)) return false;
    const auto effects = o->getProperty("effects"); const auto* list = effects.getArray();
    if (!list || list->size() > 8) return false;
    for (const auto& v : *list) { PluginState p; if (!pluginFromVar(v,p)) return false; rack.effects.push_back(std::move(p)); }
    if (o->hasProperty("macros")) {   // absent before format 39, and when every macro is untouched
        const auto* saved = o->getProperty("macros").getArray();
        if (!saved || saved->size() != rackMacroCount) return false;
        const auto number = [](const juce::var& v, float& out) {
            if (!(v.isDouble() || v.isInt() || v.isInt64())) return false;
            out = static_cast<float>(static_cast<double>(v)); return true;
        };
        for (int m = 0; m < rackMacroCount; ++m) {
            const auto* mo = (*saved)[m].getDynamicObject();
            if (!mo) return false;
            auto& macro = rack.macros[static_cast<std::size_t>(m)];
            if (mo->hasProperty("name")) {
                if (!mo->getProperty("name").isString()) return false;
                macro.name = mo->getProperty("name").toString();
            }
            if (!number(mo->getProperty("value"), macro.value)) return false;
            const auto* targets = mo->getProperty("targets").getArray();
            if (!targets) return false;
            for (const auto& tv : *targets) {
                const auto* to = tv.getDynamicObject();
                MacroTarget t;
                if (!to || !parseInstanceId(to->getProperty("instance"), t.instance) || !to->getProperty("parameter").isString()
                    || !number(to->getProperty("from"), t.from) || !number(to->getProperty("to"), t.to)) return false;
                t.parameterId = to->getProperty("parameter").toString();
                macro.targets.push_back(std::move(t));
            }
        }
    }
    return rack.valid();
}
}
