#pragma once

#include "CommandEngine.h"
#include "StockEffects.h"
#include <initializer_list>

namespace alien::mastering {

// Fixed starting point. The three stock processors retain their own controls
// and state, so the user can audition, adjust or bypass any of them.
template <class Capture, class Rebuild>
command::Result addGentleChain(ProjectController& controller, Capture&& capture, Rebuild&& rebuild) {
    if (!controller.canEditMidiClips()) return {.error = "Stop recording or finish project work before changing Master FX."};
    int slot = static_cast<int>(controller.document().masterPlugins.effects.size());
    if (slot + 3 > device_edit::maxEffects) return {.error = "The mastering chain needs three free Master FX slots."};

    std::vector<command::Command> edits;
    const auto append = [&](const stock::Info& info, std::initializer_list<std::pair<const char*, float>> values) {
        auto effect = stock::create(info.id);
        for (const auto& [id, value] : values) {
            auto* parameter = effect->parameters.getParameter(id);
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        }
        juce::MemoryBlock bytes;
        effect->getStateInformation(bytes);
        edits.emplace_back(command::InsertDevice{RackStage::master, 0, slot++,
            PluginState{stock::describe(info).createXml()->toString(), bytes.toBase64Encoding()}});
    };
    append(stock::catalog[0], {{stock::eq::lowGain, -1.f}});
    append(stock::catalog[1], {{stock::comp::ratio, 1.5f}, {stock::comp::attack, 30.f},
        {stock::comp::release, 150.f}, {stock::comp::mix, .5f}});
    append(stock::catalog[4], {{stock::limiter::ceiling, -1.f}});

    command::CommandEngine commands(controller);
    const auto predicted = commands.preview(edits);
    if (!predicted.ok || !predicted.changed) return predicted;
    capture();
    auto result = commands.batch(edits, "Add gentle mastering chain");
    if (result.ok && result.changed) rebuild();
    return result;
}

} // namespace alien::mastering
