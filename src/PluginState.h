#pragma once
#include "SessionCapacity.h"
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace alien {
// Per-plugin limit on saved state, in base64 characters (about 18 MiB of binary
// data). Sampler-style plugins can embed megabytes of data; the whole project
// file must still stay under the 128 MiB limit enforced when saving.
inline constexpr int maxPluginStateChars = 24 * 1024 * 1024;
struct PluginState {
    juce::String descriptionXml, stateBase64;
    bool bypass{};
    bool operator==(const PluginState&) const = default;
    bool valid() const {
        return descriptionXml.length() <= 65536 && stateBase64.length() <= maxPluginStateChars
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

inline juce::var pluginToVar(const PluginState& p) {
    auto value = juce::var(new juce::DynamicObject()); auto* o = value.getDynamicObject();
    o->setProperty("description", p.descriptionXml); o->setProperty("state", p.stateBase64); o->setProperty("bypass", p.bypass);
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
    p = {d.toString(), s.toString(), static_cast<bool>(b)}; return p.valid();
}
inline bool rackFromVar(const juce::var& value, PluginRackState& rack) {
    const auto* o = value.getDynamicObject(); if (!o || !pluginFromVar(o->getProperty("instrument"), rack.instrument)) return false;
    const auto effects = o->getProperty("effects"); const auto* list = effects.getArray();
    if (!list || list->size() > 8) return false;
    for (const auto& v : *list) { PluginState p; if (!pluginFromVar(v,p)) return false; rack.effects.push_back(std::move(p)); }
    return rack.valid();
}
}
