#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace alien {
struct PluginState {
    juce::String descriptionXml, stateBase64;
    bool bypass{};
    bool operator==(const PluginState&) const = default;
    bool valid() const {
        return descriptionXml.length() <= 65536 && stateBase64.length() <= 4 * 1024 * 1024
            && (descriptionXml.isEmpty() ? stateBase64.isEmpty() : juce::parseXML(descriptionXml) != nullptr);
    }
};
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
