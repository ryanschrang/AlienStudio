#pragma once
// The Channel Rack inside a project file (format version 7, steps with gate, nudge
// and ratchet from version 12): channels with their sound and mixer settings, and
// the pattern bank. Built-in sounds are saved by
// name and regenerated on load; other samples are saved as file references and
// handled like drum-pad samples (relative paths, collected on a portable save,
// relinkable when missing).
#include "RackModel.h"
#include "ClipLibrary.h"
#include "PluginState.h"
#include <limits>
#include <juce_core/juce_core.h>
#include <cmath>

namespace alien::rack {

namespace detail {
inline juce::String text(const std::string& value) { return juce::String::fromUTF8(value.c_str(), static_cast<int>(value.size())); }
inline bool isNumber(const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }
inline bool integerIn(const juce::var& v, int low, int high, int& out) {
    if (!(v.isInt() || v.isInt64())) return false;
    const auto value = static_cast<juce::int64>(v);
    if (value < low || value > high) return false;
    out = static_cast<int>(value); return true;
}
inline bool floatIn(const juce::var& v, float low, float high, float& out) {
    if (!isNumber(v)) return false;
    const double value = static_cast<double>(v);
    if (!std::isfinite(value) || value < low || value > high) return false;
    out = static_cast<float>(value); return true;
}
} // namespace detail

inline std::uint64_t savedClipId(const Project& project, int slot) {
    auto id = project.clips[static_cast<std::size_t>(slot)];
    if (id == 0) {
        id = static_cast<std::uint64_t>(slot + 1);
        while (std::find(project.clips.begin(), project.clips.end(), id) != project.clips.end()) id += maxPatterns;
    }
    return id;
}
inline juce::var rackToVar(const Project& project, bool identities = false) {
    auto root = juce::var(new juce::DynamicObject());
    auto* object = root.getDynamicObject();
    object->setProperty("swing", static_cast<double>(project.swing));
    object->setProperty("selectedPattern", project.selectedPattern);
    object->setProperty("nameColumnWidth", project.nameColumnWidth);
    juce::Array<juce::var> channels;
    for (int c = 0; c < project.channelCount; ++c) {
        const auto i = static_cast<std::size_t>(c);
        auto value = juce::var(new juce::DynamicObject());
        auto* channel = value.getDynamicObject();
        channel->setProperty("name", detail::text(project.info[i].name));
        if (identities) channel->setProperty("lane", detail::text(library::toHex(project.lanes[i])));
        channel->setProperty("sample", detail::text(project.info[i].sample));
        channel->setProperty("volume", static_cast<double>(project.channels[i].volume));
        channel->setProperty("pan", static_cast<double>(project.channels[i].pan));
        channel->setProperty("mute", project.channels[i].mute);
        channel->setProperty("solo", project.channels[i].solo);
        channel->setProperty("output", static_cast<int>(project.channels[i].output));
        channel->setProperty("chokeGroup", static_cast<int>(project.channels[i].chokeGroup));
        channel->setProperty("instrumentPreset", project.channels[i].instrumentPreset);
        // A plugin instrument (format 31), saved like a track's instrument.
        if (project.info[i].hasPlugin()) {
            PluginState plugin;
            plugin.descriptionXml = juce::String::fromUTF8(project.info[i].pluginDescription().c_str());
            plugin.stateBase64 = juce::String::fromUTF8(project.info[i].pluginState().c_str());
            plugin.instanceId = project.info[i].pluginInstance();
            channel->setProperty("plugin", pluginToVar(plugin));
        }
        channels.add(value);
    }
    object->setProperty("channels", juce::var(channels));
    juce::Array<juce::var> patterns;
    for (int p = 0; p < project.patternCount; ++p) {
        const auto& source = project.pattern(p);
        auto value = juce::var(new juce::DynamicObject());
        auto* pattern = value.getDynamicObject();
        if (identities) pattern->setProperty("clip", detail::text(library::toHex(savedClipId(project, p))));
        pattern->setProperty("name", detail::text(project.patternNames[static_cast<std::size_t>(p)]));
        pattern->setProperty("length", source.length);
        // Only steps that are on, or that keep non-default values drawn earlier,
        // are written: [channel, step, on, velocity, pan, pitch, probability, gate, nudge, ratchet].
        juce::Array<juce::var> steps;
        for (int c = 0; c < project.channelCount; ++c)
            for (int s = 0; s < maxSteps; ++s) {
                const auto& step = source.steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
                if (step == Step{}) continue;
                juce::Array<juce::var> cell;
                cell.add(c); cell.add(s); cell.add(step.on ? 1 : 0); cell.add(static_cast<int>(step.velocity));
                cell.add(static_cast<int>(step.pan)); cell.add(static_cast<int>(step.pitch)); cell.add(static_cast<int>(step.probability));
                cell.add(static_cast<int>(step.gate)); cell.add(static_cast<int>(step.nudge)); cell.add(static_cast<int>(step.ratchet));
                steps.add(juce::var(cell));
            }
        pattern->setProperty("steps", juce::var(steps));
        juce::Array<juce::var> noteRows;
        for (int c = 0; c < project.channelCount; ++c) {
            const auto& sequence = source.notes[static_cast<std::size_t>(c)];
            if (!sequence) continue;
            auto rowValue = juce::var(new juce::DynamicObject());
            rowValue.getDynamicObject()->setProperty("channel", c);
            juce::Array<juce::var> events;
            for (std::size_t n = 0; n < sequence->count; ++n) {
                const auto& note = sequence->notes[n];
                juce::Array<juce::var> event;
                event.add(static_cast<juce::int64>(note.id)); event.add(note.pitch);
                event.add(PianoRollPattern::toTicks(note.startStep)); event.add(PianoRollPattern::toTicks(note.lengthSteps));
                event.add(static_cast<double>(note.velocity)); event.add(static_cast<double>(note.probability));
                events.add(juce::var(event));
            }
            rowValue.getDynamicObject()->setProperty("notes", juce::var(events));
            noteRows.add(rowValue);
        }
        pattern->setProperty("noteRows", juce::var(noteRows));
        patterns.add(value);
    }
    object->setProperty("patterns", juce::var(patterns));
    return root;
}

// Strict: anything unexpected rejects the whole rack, so a damaged file is
// reported instead of being opened as something different.
inline bool rackFromVar(const juce::var& value, Project& out, juce::String& error, bool identities = false) {
    auto fail = [&](const char* reason) { error = reason; return false; };
    const auto* object = value.getDynamicObject();
    if (object == nullptr) return fail("The project's Channel Rack is missing or invalid.");
    Project parsed;
    float swing = 0.0f;
    int selected = 0;
    if (!detail::floatIn(object->getProperty("swing"), 0.0f, 1.0f, swing)
        || !detail::integerIn(object->getProperty("selectedPattern"), 0, maxPatterns - 1, selected))
        return fail("The Channel Rack's swing or selected pattern is invalid.");
    parsed.swing = swing;
    // Optional (projects saved before it existed use the default width); present but invalid rejects the rack.
    if (object->hasProperty("nameColumnWidth")
        && !detail::integerIn(object->getProperty("nameColumnWidth"), minNameColumn, maxNameColumn, parsed.nameColumnWidth))
        return fail("The Channel Rack's name column width is invalid.");

    const auto channelsValue = object->getProperty("channels");
    const auto* channels = channelsValue.getArray();
    if (channels == nullptr || channels->size() > maxChannels) return fail("The Channel Rack has an invalid channel list.");
    for (const auto& item : *channels) {
        const auto* channel = item.getDynamicObject();
        if (channel == nullptr) return fail("A Channel Rack channel is invalid.");
        const auto name = channel->getProperty("name"), sample = channel->getProperty("sample");
        const auto mute = channel->getProperty("mute"), solo = channel->getProperty("solo");
        Channel settings;
        int output = 0, choke = 0;
        if (!name.isString() || !sample.isString() || !mute.isBool() || !solo.isBool()
            || !detail::floatIn(channel->getProperty("volume"), 0.0f, 1.0f, settings.volume)
            || !detail::floatIn(channel->getProperty("pan"), -1.0f, 1.0f, settings.pan)
            || !detail::integerIn(channel->getProperty("output"), masterOutput, maxOutputTrack, output)
            || !detail::integerIn(channel->getProperty("chokeGroup"), 0, maxChokeGroup, choke))
            return fail("A Channel Rack channel has missing or out-of-range values.");
        if (channel->hasProperty("instrumentPreset")
            && !detail::integerIn(channel->getProperty("instrumentPreset"), -1, alien::presetCount - 1, settings.instrumentPreset))
            return fail("Invalid Channel Rack instrument preset.");
        if (settings.isSynth() && !sample.toString().isEmpty()) return fail("A synth channel cannot also name a sample.");
        PluginState plugin;
        if (channel->hasProperty("plugin")) {
            if (!pluginFromVar(channel->getProperty("plugin"), plugin) || plugin.descriptionXml.isEmpty() || plugin.instanceId == 0
                || settings.isSynth() || !sample.toString().isEmpty() || plugin.sidechain >= 0)
                return fail("A Channel Rack plugin instrument is invalid.");
        }
        settings.mute = static_cast<bool>(mute); settings.solo = static_cast<bool>(solo);
        settings.output = static_cast<std::int8_t>(output); settings.chokeGroup = static_cast<std::uint8_t>(choke);
        const auto sampleText = sample.toString().toStdString();
        if (sampleText.size() > maxSampleBytes) return fail("A Channel Rack sample path is too long.");
        if (!parsed.addChannel(Project::clean(name.toString().toStdString()), sampleText)) return fail("The Channel Rack has too many channels.");
        parsed.channels[static_cast<std::size_t>(parsed.channelCount - 1)] = settings;
        if (plugin.descriptionXml.isNotEmpty()
            && !parsed.setPlugin(parsed.channelCount - 1, plugin.descriptionXml.toStdString(), plugin.instanceId))
            return fail("A Channel Rack plugin instrument is invalid.");
        if (plugin.descriptionXml.isNotEmpty())
            parsed.setPluginState(plugin.instanceId, plugin.stateBase64.toStdString());
        if (identities) {
            if (!channel->getProperty("lane").isString()) return fail("Invalid rack lane identity.");
            const auto id = library::parseId<library::LaneKind>(channel->getProperty("lane").toString().toStdString());
            if (!id || id->value == std::numeric_limits<std::uint64_t>::max()) return fail("Invalid rack lane identity.");
            parsed.lanes[static_cast<std::size_t>(parsed.channelCount - 1)] = id->value;
            parsed.nextLane = std::max(parsed.nextLane, id->value + 1);
        }
    }

    const auto patternsValue = object->getProperty("patterns");
    const auto* patterns = patternsValue.getArray();
    if (patterns == nullptr || patterns->isEmpty() || patterns->size() > maxPatterns) return fail("The Channel Rack has an invalid pattern list.");
    parsed.patternCount = patterns->size();
    if (selected >= parsed.patternCount) return fail("The Channel Rack's selected pattern does not exist.");
    parsed.selectedPattern = selected;
    for (int p = 0; p < patterns->size(); ++p) {
        const auto* pattern = (*patterns)[p].getDynamicObject();
        if (pattern == nullptr) return fail("A Channel Rack pattern is invalid.");
        const auto name = pattern->getProperty("name");
        if (identities) {
            if (!pattern->getProperty("clip").isString()) return fail("Invalid rack clip identity.");
            const auto id = library::parseId<library::ClipKind>(pattern->getProperty("clip").toString().toStdString());
            if (!id) return fail("Invalid rack clip identity.");
            parsed.clips[static_cast<std::size_t>(p)] = id->value;
        }
        int length = 0;
        const auto stepsValue = pattern->getProperty("steps");
        const auto* steps = stepsValue.getArray();
        if (!name.isString() || !detail::integerIn(pattern->getProperty("length"), 16, maxSteps, length) || !validLength(length) || steps == nullptr
            || steps->size() > maxChannels * maxSteps)
            return fail("A Channel Rack pattern has missing or invalid values.");
        auto built = std::make_shared<Pattern>();
        built->length = length;
        for (const auto& item : *steps) {
            const auto* cell = item.getArray();
            int c = 0, s = 0, on = 0, velocity = 0, pan = 0, pitch = 0, probability = 0;
            int gate = fullGate, nudge = 0, ratchet = 1;
            // Seven values before version 12 (gate, nudge and ratchet take their defaults), ten from then on.
            if (cell == nullptr || (cell->size() != 7 && cell->size() != 10)
                || !detail::integerIn((*cell)[0], 0, parsed.channelCount - 1, c) || !detail::integerIn((*cell)[1], 0, maxSteps - 1, s)
                || !detail::integerIn((*cell)[2], 0, 1, on) || !detail::integerIn((*cell)[3], 1, 127, velocity)
                || !detail::integerIn((*cell)[4], -63, 63, pan) || !detail::integerIn((*cell)[5], -24, 24, pitch)
                || !detail::integerIn((*cell)[6], 0, 100, probability)
                || (cell->size() == 10 && (!detail::integerIn((*cell)[7], 1, fullGate, gate) || !detail::integerIn((*cell)[8], 0, maxNudge, nudge)
                                           || !detail::integerIn((*cell)[9], 1, maxRatchet, ratchet))))
                return fail("A Channel Rack step is outside the supported range.");
            auto& step = built->steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
            step.on = on != 0; step.velocity = static_cast<std::uint8_t>(velocity); step.pan = static_cast<std::int8_t>(pan);
            step.pitch = static_cast<std::int8_t>(pitch); step.probability = static_cast<std::uint8_t>(probability);
            step.gate = static_cast<std::uint8_t>(gate); step.nudge = static_cast<std::uint8_t>(nudge); step.ratchet = static_cast<std::uint8_t>(ratchet);
        }
        // Optional for older projects; present but malformed is never silently ignored.
        if (pattern->hasProperty("noteRows")) {
            const auto rowsValue = pattern->getProperty("noteRows");
            const auto* rows = rowsValue.getArray();
            if (!rows || rows->size() > parsed.channelCount) return fail("Invalid rack note rows.");
            for (const auto& rowValue : *rows) {
                const auto* row = rowValue.getDynamicObject();
                int c = 0;
                if (!row || !detail::integerIn(row->getProperty("channel"), 0, parsed.channelCount - 1, c)
                    || built->notes[static_cast<std::size_t>(c)]) return fail("Invalid or duplicate rack note channel.");
                const auto eventsValue = row->getProperty("notes");
                const auto* events = eventsValue.getArray();
                if (!events || events->size() > static_cast<int>(PianoRollPattern::maxNotes)) return fail("Invalid rack note list.");
                auto sequence = std::make_shared<PianoRollPattern>(); sequence->loopSteps = length;
                for (const auto& eventValue : *events) {
                    const auto* event = eventValue.getArray();
                    int pitch = 0, start = 0, duration = 0; float velocity = 0, probability = 0;
                    if (!event || event->size() != 6 || !((*event)[0].isInt() || (*event)[0].isInt64())
                        || static_cast<juce::int64>((*event)[0]) <= 0 || static_cast<juce::int64>((*event)[0]) > 0xffffffffLL
                        || !detail::integerIn((*event)[1], 0, 127, pitch)
                        || !detail::integerIn((*event)[2], 0, length * PianoRollPattern::ticksPerStep - 1, start)
                        || !detail::integerIn((*event)[3], 1, length * PianoRollPattern::ticksPerStep - start, duration)
                        || !detail::floatIn((*event)[4], 0, 1, velocity) || velocity <= 0
                        || !detail::floatIn((*event)[5], 0, 1, probability)) return fail("Invalid rack note event.");
                    if (!sequence->add(pitch, PianoRollPattern::fromTicks(start), PianoRollPattern::fromTicks(duration), velocity, probability))
                        return fail("Cannot load rack note event.");
                    auto& note = sequence->notes[sequence->count - 1];
                    note.id = static_cast<std::uint32_t>(static_cast<juce::int64>((*event)[0]));
                    note.velocity = velocity; note.probability = probability;
                }
                if (!sequence->valid()) return fail("Duplicate or invalid rack note IDs.");
                built->notes[static_cast<std::size_t>(c)] = std::move(sequence);
                built->noteRevisions[static_cast<std::size_t>(c)] = nextNoteRevision();
            }
        }
        parsed.patterns[static_cast<std::size_t>(p)] = std::move(built);
        std::string patternName = Project::clean(name.toString().toStdString());
        if (patternName.empty()) patternName = "Pattern " + std::to_string(p + 1);
        parsed.patternNames[static_cast<std::size_t>(p)] = std::move(patternName);
    }
    if (!parsed.valid()) return fail("The Channel Rack contains invalid data.");
    out = std::move(parsed);
    error.clear();
    return true;
}

} // namespace alien::rack
