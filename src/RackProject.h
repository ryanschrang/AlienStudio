#pragma once
// The Channel Rack inside a project file (format version 7): channels with their
// sound and mixer settings, and the pattern bank. Built-in sounds are saved by
// name and regenerated on load; other samples are saved as file references and
// handled like drum-pad samples (relative paths, collected on a portable save,
// relinkable when missing).
#include "RackModel.h"
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

inline juce::var rackToVar(const Project& project) {
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
        channel->setProperty("sample", detail::text(project.info[i].sample));
        channel->setProperty("volume", static_cast<double>(project.channels[i].volume));
        channel->setProperty("pan", static_cast<double>(project.channels[i].pan));
        channel->setProperty("mute", project.channels[i].mute);
        channel->setProperty("solo", project.channels[i].solo);
        channel->setProperty("output", static_cast<int>(project.channels[i].output));
        channel->setProperty("chokeGroup", static_cast<int>(project.channels[i].chokeGroup));
        channels.add(value);
    }
    object->setProperty("channels", juce::var(channels));
    juce::Array<juce::var> patterns;
    for (int p = 0; p < project.patternCount; ++p) {
        const auto& source = project.pattern(p);
        auto value = juce::var(new juce::DynamicObject());
        auto* pattern = value.getDynamicObject();
        pattern->setProperty("name", detail::text(project.patternNames[static_cast<std::size_t>(p)]));
        pattern->setProperty("length", source.length);
        // Only steps that are on, or that keep non-default values drawn earlier,
        // are written: [channel, step, on, velocity, pan, pitch, probability].
        juce::Array<juce::var> steps;
        for (int c = 0; c < project.channelCount; ++c)
            for (int s = 0; s < maxSteps; ++s) {
                const auto& step = source.steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
                if (step == Step{}) continue;
                juce::Array<juce::var> cell;
                cell.add(c); cell.add(s); cell.add(step.on ? 1 : 0); cell.add(static_cast<int>(step.velocity));
                cell.add(static_cast<int>(step.pan)); cell.add(static_cast<int>(step.pitch)); cell.add(static_cast<int>(step.probability));
                steps.add(juce::var(cell));
            }
        pattern->setProperty("steps", juce::var(steps));
        patterns.add(value);
    }
    object->setProperty("patterns", juce::var(patterns));
    return root;
}

// Strict: anything unexpected rejects the whole rack, so a damaged file is
// reported instead of being opened as something different.
inline bool rackFromVar(const juce::var& value, Project& out, juce::String& error) {
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
        settings.mute = static_cast<bool>(mute); settings.solo = static_cast<bool>(solo);
        settings.output = static_cast<std::int8_t>(output); settings.chokeGroup = static_cast<std::uint8_t>(choke);
        const auto sampleText = sample.toString().toStdString();
        if (sampleText.size() > maxSampleBytes) return fail("A Channel Rack sample path is too long.");
        if (!parsed.addChannel(Project::clean(name.toString().toStdString()), sampleText)) return fail("The Channel Rack has too many channels.");
        parsed.channels[static_cast<std::size_t>(parsed.channelCount - 1)] = settings;
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
        int length = 0;
        const auto stepsValue = pattern->getProperty("steps");
        const auto* steps = stepsValue.getArray();
        if (!name.isString() || !detail::integerIn(pattern->getProperty("length"), 16, 64, length) || !validLength(length) || steps == nullptr
            || steps->size() > maxChannels * maxSteps)
            return fail("A Channel Rack pattern has missing or invalid values.");
        auto built = std::make_shared<Pattern>();
        built->length = length;
        for (const auto& item : *steps) {
            const auto* cell = item.getArray();
            int c = 0, s = 0, on = 0, velocity = 0, pan = 0, pitch = 0, probability = 0;
            if (cell == nullptr || cell->size() != 7
                || !detail::integerIn((*cell)[0], 0, parsed.channelCount - 1, c) || !detail::integerIn((*cell)[1], 0, maxSteps - 1, s)
                || !detail::integerIn((*cell)[2], 0, 1, on) || !detail::integerIn((*cell)[3], 1, 127, velocity)
                || !detail::integerIn((*cell)[4], -63, 63, pan) || !detail::integerIn((*cell)[5], -24, 24, pitch)
                || !detail::integerIn((*cell)[6], 0, 100, probability))
                return fail("A Channel Rack step is outside the supported range.");
            auto& step = built->steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
            step.on = on != 0; step.velocity = static_cast<std::uint8_t>(velocity); step.pan = static_cast<std::int8_t>(pan);
            step.pitch = static_cast<std::int8_t>(pitch); step.probability = static_cast<std::uint8_t>(probability);
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
