#pragma once
#include "SessionCapacity.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace alien {
// Per-plugin limit on saved state, in base64 characters (about 18 MiB of binary
// data). Sampler-style plugins can embed megabytes of data; the whole project
// file must still stay under the 128 MiB limit enforced when saving.
inline constexpr int maxPluginStateChars = 24 * 1024 * 1024;
struct PluginState {
    juce::String descriptionXml, stateBase64;
    bool bypass{};
    std::uint64_t instanceId{};
    PluginState() = default;
    PluginState(juce::String description, juce::String state, bool bypassed = false)
        : descriptionXml(std::move(description)), stateBase64(std::move(state)), bypass(bypassed) {
        if (descriptionXml.isNotEmpty())
            do { instanceId = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64()); } while (instanceId == 0);
    }
    bool operator==(const PluginState&) const = default;
    bool valid() const {
        return (descriptionXml.isEmpty() ? instanceId == 0 : instanceId != 0) && descriptionXml.length() <= 65536 && stateBase64.length() <= maxPluginStateChars
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
struct PluginRackState {
    PluginState instrument;
    std::vector<PluginState> effects;
    bool operator==(const PluginRackState&) const = default;
    bool valid() const {
        if (!instrument.valid() || effects.size() > 8) return false;
        for (const auto& effect : effects) if (!effect.valid() || effect.descriptionXml.isEmpty()) return false;
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
    return value;
}
inline juce::var rackToVar(const PluginRackState& rack) {
    auto value = juce::var(new juce::DynamicObject()); auto* o = value.getDynamicObject();
    o->setProperty("instrument", pluginToVar(rack.instrument));
    juce::Array<juce::var> fx; for (const auto& p : rack.effects) fx.add(pluginToVar(p));
    o->setProperty("effects", juce::var(fx)); return value;
}
inline bool pluginFromVar(const juce::var& value, PluginState& p) {
    const auto* o = value.getDynamicObject(); if (!o) return false;
    auto d = o->getProperty("description"), s = o->getProperty("state"), b = o->getProperty("bypass");
    if (!d.isString() || !s.isString() || !b.isBool()) return false;
    PluginState parsed{d.toString(), s.toString(), static_cast<bool>(b)};
    // Legacy states acquire an identity once, on load. Copies and undo preserve it.
    if (o->hasProperty("instance") && !parseInstanceId(o->getProperty("instance"), parsed.instanceId)) return false;
    if (!parsed.valid()) return false;
    p = std::move(parsed); return true;
}
inline bool rackFromVar(const juce::var& value, PluginRackState& rack) {
    const auto* o = value.getDynamicObject(); if (!o || !pluginFromVar(o->getProperty("instrument"), rack.instrument)) return false;
    const auto effects = o->getProperty("effects"); const auto* list = effects.getArray();
    if (!list || list->size() > 8) return false;
    for (const auto& v : *list) { PluginState p; if (!pluginFromVar(v,p)) return false; rack.effects.push_back(std::move(p)); }
    return rack.valid();
}
}
