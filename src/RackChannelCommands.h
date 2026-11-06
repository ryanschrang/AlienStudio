#pragma once
#include "ProjectDocument.h"
#include "RackSounds.h"
#include <cmath>
#include <cstdint>
#include <string>

// Pure document edits for the Channel Rack's channels: adding, removing, moving, naming and
// mixing them. The controller and command Preview share them, so what Preview predicts is
// what the live edit does. They never touch a plugin; the host rebuilds after the commit.
namespace alien::rack_channel {
enum class Kind { add, addSynth, remove, move, duplicate, rename, volume, pan, mute, solo, output, choke };
struct Edit {
    Kind kind{Kind::remove};
    int channel{-1};
    int value{};                 // move: where it ends up; output, choke group, or synth preset
    double level{};              // volume or pan
    bool on{};                   // mute or solo
    std::uint64_t instance{};    // duplicate: the copy's plugin identity, so Preview and execute agree
    juce::String text;           // rename: the name; add: a built-in sound id, or empty for an empty sampler
};

inline std::uint64_t freshInstance() {
    std::uint64_t id = 0;
    while (id == 0) id = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64());
    return id;
}

// Changes `p` and returns true, or returns false when there is nothing to change.
inline bool editRack(rack::Project& p, const Edit& e) {
    const auto with = [&](auto change) {
        auto channel = p.channels[static_cast<std::size_t>(e.channel)];
        change(channel);
        return p.setChannel(e.channel, channel);
    };
    switch (e.kind) {
    case Kind::add: {
        if (e.text.isEmpty()) return p.addChannel("Sampler", "");
        const auto id = e.text.toStdString();
        return p.addChannel(rack::builtinDisplayName(id), "builtin:" + id);
    }
    case Kind::addSynth: {
        if (!p.addChannel(alien::presetNames[static_cast<std::size_t>(e.value)], "")) return false;
        const int channel = p.channelCount - 1;
        p.setInstrumentPreset(channel, e.value);
        PianoRollPattern notes;
        notes.loopSteps = p.pattern(p.selectedPattern).length;
        return p.setChannelNotes(p.selectedPattern, channel, notes);
    }
    case Kind::remove: return p.removeChannel(e.channel);
    case Kind::move: {
        const int step = e.value > e.channel ? 1 : -1;
        for (int c = e.channel; c != e.value; c += step) if (!p.moveChannel(c, step)) return false;
        return e.value != e.channel;
    }
    case Kind::duplicate:
        if (!p.duplicateChannel(e.channel)) return false;
        if (p.info[static_cast<std::size_t>(e.channel + 1)].hasPlugin()) p.renewPluginInstance(e.channel + 1, e.instance);
        return true;
    case Kind::rename: return p.renameChannel(e.channel, e.text.toStdString());
    case Kind::volume: return with([&](rack::Channel& c) { c.volume = static_cast<float>(e.level); });
    case Kind::pan: return with([&](rack::Channel& c) { c.pan = static_cast<float>(e.level); });
    case Kind::mute: return with([&](rack::Channel& c) { c.mute = e.on; });
    case Kind::solo: return with([&](rack::Channel& c) { c.solo = e.on; });
    case Kind::output: return with([&](rack::Channel& c) { c.output = static_cast<std::int8_t>(e.value); });
    case Kind::choke: return with([&](rack::Channel& c) { c.chokeGroup = static_cast<std::uint8_t>(e.value); });
    }
    return false;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto& p = d.channelRack;
    const bool adds = e.kind == Kind::add || e.kind == Kind::addSynth || e.kind == Kind::duplicate;
    if (e.kind != Kind::add && e.kind != Kind::addSynth && !p.validChannel(e.channel)) return "There is no rack channel " + juce::String(e.channel + 1) + ".";
    if (adds && p.channelCount >= rack::maxChannels) return "The Channel Rack holds " + juce::String(rack::maxChannels) + " channels.";
    switch (e.kind) {
    case Kind::add:
        if (e.text.isNotEmpty() && rack::builtinDisplayName(e.text.toStdString()) == nullptr) return "There is no built-in sound \"" + e.text + "\".";
        break;
    case Kind::addSynth:
        if (e.value < 0 || e.value >= alien::presetCount) return "Synth presets are numbered 0 to " + juce::String(alien::presetCount - 1) + ".";
        break;
    case Kind::move:
        if (!p.validChannel(e.value)) return "There is no rack channel " + juce::String(e.value + 1) + " to move to.";
        break;
    case Kind::duplicate:
        if (p.info[static_cast<std::size_t>(e.channel)].hasPlugin() && e.instance == 0) return "A copied plugin channel needs an identity of its own.";
        break;
    case Kind::rename:
        if (e.text.trim().isEmpty()) return "A channel name cannot be blank.";
        break;
    case Kind::volume:
        if (!std::isfinite(e.level) || e.level < 0 || e.level > 1) return "A channel's volume must be a finite number from 0 to 1.";
        break;
    case Kind::pan:
        if (!std::isfinite(e.level) || e.level < -1 || e.level > 1) return "A channel's pan must be a finite number from -1 (left) to 1 (right).";
        break;
    case Kind::output:
        if (e.value != rack::masterOutput && (e.value < 0 || e.value >= d.trackCount() || e.value > rack::maxOutputTrack)) return "There is no track " + juce::String(e.value + 1) + " to route to.";
        break;
    case Kind::choke:
        if (e.value < 0 || e.value > rack::maxChokeGroup) return "Choke groups are numbered 0 (none) to " + juce::String(rack::maxChokeGroup) + ".";
        break;
    case Kind::remove: case Kind::mute: case Kind::solo: break;
    }
    auto next = p;
    if (!editRack(next, e)) return {};   // an edit that changes nothing is accepted
    if (!next.valid()) return "The Channel Rack cannot take that edit.";
    for (int c = 0; c < next.channelCount; ++c) {
        const int output = next.channels[static_cast<std::size_t>(c)].output;
        if (output >= 0 && output < d.trackCount() && d.tracks[static_cast<std::size_t>(output)].settings.frozen())
            return "Unfreeze the tracks receiving rack channels before editing their channels.";
    }
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    auto next = d.channelRack;
    if (!editRack(next, e)) return {};
    return d.setRack(std::move(next)) ? juce::String{} : juce::String("The Channel Rack could not take that edit.");
}
} // namespace alien::rack_channel
