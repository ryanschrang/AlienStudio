#pragma once
#include "CommandEngine.h"
#include "StockEffects.h"
#include <initializer_list>
#include <vector>

// T2-03: a gentle mastering chain for Master FX. One action appends Alien EQ, Alien Compressor and
// Alien Limiter, with fixed conservative settings, after whatever the master rack already holds. It is a
// preset, not an assistant: it does not listen to the mix, and every device is an ordinary stock effect
// that can be edited, bypassed, moved or removed afterwards. The chain is three InsertDevice commands in
// one batch, so it is one Undo step and adds no command of its own and nothing to the project format.
namespace alien::mastering {
inline constexpr int devices = 3;
inline constexpr const char* undoLabel = "Add mastering chain";

struct Setting { const char* id; float value; };

// EQ: trim a little low-mid build-up and add a little air. Everything else stays flat.
inline constexpr std::array<Setting, 6> eqSettings{{
    {stock::eq::m1Freq, 300.0f}, {stock::eq::m1Gain, -1.0f}, {stock::eq::m1Q, 0.7f},
    {stock::eq::highFreq, 10000.0f}, {stock::eq::highGain, 1.0f}, {stock::eq::output, 0.0f}}};
// Compressor: slow, low-ratio glue with no make-up gain, so it never makes the mix louder by itself.
inline constexpr std::array<Setting, 7> compressorSettings{{
    {stock::comp::threshold, -12.0f}, {stock::comp::ratio, 1.5f}, {stock::comp::attack, 30.0f}, {stock::comp::release, 200.0f},
    {stock::comp::knee, 6.0f}, {stock::comp::makeup, 0.0f}, {stock::comp::mix, 1.0f}}};
// Limiter: a safety ceiling only. No input gain, so it works only on peaks above -1 dBFS.
inline constexpr std::array<Setting, 3> limiterSettings{{
    {stock::limiter::gain, 0.0f}, {stock::limiter::ceiling, -1.0f}, {stock::limiter::release, 150.0f}}};

// A stock effect as a rack records it, with its saved state holding `settings` (other parameters keep
// their defaults). Message thread only: it builds the effect once to write its state.
template <std::size_t Count>
inline PluginState device(const stock::Info& info, const std::array<Setting, Count>& settings) {
    juce::String state;
    if (auto effect = stock::create(info.id)) {
        for (const auto& setting : settings)
            if (auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(effect->parameters.getParameter(setting.id))) *parameter = setting.value;
        juce::MemoryBlock bytes;
        effect->getStateInformation(bytes);
        state = bytes.toBase64Encoding();
    }
    return PluginState(stock::describe(info).createXml()->toString(), state);
}

// EQ, then compressor, then limiter last. Each call makes devices with new identities.
inline std::vector<PluginState> chain() {
    return {device(stock::catalog[0], eqSettings), device(stock::catalog[1], compressorSettings), device(stock::catalog[4], limiterSettings)};
}

// Why the chain cannot go on the master rack as it stands, or nothing.
inline juce::String refusal(const ProjectDocument& d) {
    const int free = device_edit::maxEffects - static_cast<int>(d.masterPlugins.effects.size());
    if (free >= devices) return {};
    return "The mastering chain needs three free Master FX slots, and " + juce::String(free == 0 ? "none is" : free == 1 ? "only one is" : "only two are")
        + " free. Remove an effect from Master FX first.";
}

// The chain as commands: appended after the master's existing effects, which keep their slots.
inline std::vector<command::Command> commands(const ProjectDocument& d) {
    std::vector<command::Command> list;
    int slot = static_cast<int>(d.masterPlugins.effects.size());
    for (auto& effect : chain()) list.push_back(command::InsertDevice{RackStage::master, 0, slot++, std::move(effect)});
    return list;
}

// Adds the chain as one Undo step. Like CommandEngine::executeWithPlugins, live plugin state is
// captured only once the edit is known to be accepted, and the racks are rebuilt only if it was made.
template <class Capture, class Rebuild>
inline command::Result add(ProjectController& controller, Capture&& capture, Rebuild&& rebuild) {
    const auto refused = [](const juce::String& why) { command::Result r; r.error = why; return r; };
    if (controller.isBusy()) return refused("The project is busy; try again when it has finished.");
    if (!controller.canEditMidiClips()) return refused("Stop recording before changing plugins.");
    if (const auto why = refusal(controller.document()); why.isNotEmpty()) return refused(why);
    command::CommandEngine engine(controller);
    const auto list = commands(controller.document());
    const auto predicted = engine.preview(list);
    if (!predicted.ok || !predicted.changed) return predicted;
    capture();
    auto result = engine.batch(list, undoLabel);
    if (result.ok && result.changed) rebuild();
    return result;
}
inline command::Result add(ProjectController& controller) { return add(controller, [] {}, [] {}); }
} // namespace alien::mastering
