#pragma once
#include "ProjectDocument.h"
#include <array>
#include <cmath>
#include <optional>

// Pure document edits for the Channel Rack's steps and patterns, and its swing. The controller and
// command Preview share them, so what Preview predicts is what the live edit does. Each edit names
// its pattern, so it does not depend on which pattern happens to be shown when it runs.
namespace alien::rack_pattern {
enum class Kind { setStep, setStepValue, fill, rotate, reverse, repeat, clearSteps, length, swing, clone, clear, rename, remove };
struct Edit {
    Kind kind{Kind::clear};
    int pattern{-1}, channel{-1}, step{-1};
    int value{};           // step value; fill interval; rotate distance; repeat period; length
    double amount{};       // swing
    bool on{};             // setStep
    juce::String text;     // setStepValue: the field's name; rename: the name
};

// Indexed like rack::StepField.
inline constexpr std::array<const char*, rack::stepFieldCount> fieldNames{"velocity", "pan", "pitch", "probability", "gate", "nudge", "ratchet"};
inline const char* fieldName(rack::StepField field) { return fieldNames[static_cast<std::size_t>(field)]; }
inline std::optional<rack::StepField> fieldNamed(const juce::String& name) {
    for (std::size_t i = 0; i < fieldNames.size(); ++i) if (name == fieldNames[i]) return static_cast<rack::StepField>(i);
    return std::nullopt;
}

// Changes `p` and returns true, or returns false when there is nothing to change.
inline bool editRack(rack::Project& p, const Edit& e) {
    switch (e.kind) {
    case Kind::setStep: return p.setStepOn(e.pattern, e.channel, e.step, e.on);
    case Kind::setStepValue: {
        const auto field = fieldNamed(e.text);
        return field && p.setStepField(e.pattern, e.channel, e.step, *field, e.value);
    }
    case Kind::fill: return p.fillEvery(e.pattern, e.channel, e.value);
    case Kind::rotate: return p.rotateRow(e.pattern, e.channel, e.value);
    case Kind::reverse: return p.reverseRow(e.pattern, e.channel);
    case Kind::repeat: return p.repeatRow(e.pattern, e.channel, e.value);
    case Kind::clearSteps: return p.clearChannelSteps(e.pattern, e.channel);
    case Kind::length: return p.setPatternLength(e.pattern, e.value);
    case Kind::swing: return p.setSwing(static_cast<float>(e.amount));
    case Kind::clone: return p.addPattern(e.pattern);
    case Kind::clear: return p.clearPattern(e.pattern);
    case Kind::rename: return p.renamePattern(e.pattern, e.text.toStdString());
    case Kind::remove: return p.deletePattern(e.pattern);
    }
    return false;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto& p = d.channelRack;
    if (e.kind == Kind::swing) {
        if (!std::isfinite(e.amount) || e.amount < 0 || e.amount > 1) return "Swing must be a finite number from 0 to 1.";
    } else if (!p.validPattern(e.pattern)) return "There is no rack pattern " + juce::String(e.pattern + 1) + ".";
    switch (e.kind) {
    case Kind::setStep: case Kind::setStepValue: case Kind::fill: case Kind::rotate: case Kind::reverse: case Kind::repeat: case Kind::clearSteps:
        if (!p.validChannel(e.channel)) return "There is no rack channel " + juce::String(e.channel + 1) + ".";
        break;
    default: break;
    }
    switch (e.kind) {
    case Kind::setStep:
        if (!p.validStep(e.pattern, e.channel, e.step)) return "This pattern has no step " + juce::String(e.step + 1) + ".";
        break;
    case Kind::setStepValue: {
        if (!p.validStep(e.pattern, e.channel, e.step)) return "This pattern has no step " + juce::String(e.step + 1) + ".";
        const auto field = fieldNamed(e.text);
        if (!field) return "A step has no \"" + e.text + "\" value.";
        if (e.value < rack::fieldMinimum(*field) || e.value > rack::fieldMaximum(*field))
            return "A step's " + e.text + " runs from " + juce::String(rack::fieldMinimum(*field)) + " to " + juce::String(rack::fieldMaximum(*field)) + ".";
        break;
    }
    case Kind::fill:
        if (e.value != 2 && e.value != 4 && e.value != 8) return "Steps can be filled every 2, 4 or 8 steps.";
        break;
    case Kind::repeat:
        if (e.value < 1) return "Steps repeat over a period of at least 1.";
        break;
    case Kind::length:
        if (!rack::validLength(e.value)) return "A pattern has 16, 32, 64 or 128 steps.";
        break;
    case Kind::clone:
        if (p.patternCount >= rack::maxPatterns) return "The Channel Rack holds " + juce::String(rack::maxPatterns) + " patterns.";
        break;
    case Kind::rename:
        if (e.text.trim().isEmpty()) return "A pattern name cannot be blank.";
        break;
    case Kind::remove:
        if (p.patternCount <= 1) return "The Channel Rack needs at least one pattern.";
        break;
    case Kind::rotate: case Kind::reverse: case Kind::clearSteps: case Kind::swing: case Kind::clear: break;
    }
    auto next = p;
    if (!editRack(next, e)) {
        if (e.kind == Kind::length && p.pattern(e.pattern).length != e.value) return "Length unchanged: move or shorten notes that extend past the chosen end.";
        return {};   // an edit that changes nothing is accepted
    }
    if (!next.valid()) return "The Channel Rack cannot take that edit.";
    if (e.kind == Kind::remove) return {};   // removing a pattern writes nothing to a frozen track's channels
    for (int c = 0; c < next.channelCount; ++c) {
        const int output = next.channels[static_cast<std::size_t>(c)].output;
        if (output >= 0 && output < d.trackCount() && d.tracks[static_cast<std::size_t>(output)].settings.frozen())
            return "Unfreeze the tracks receiving rack channels before editing their patterns.";
    }
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    auto next = d.channelRack;
    if (!editRack(next, e)) return {};
    if (d.setRack(std::move(next))) return {};
    return e.kind == Kind::remove ? juce::String("Remove this pattern from the Session and Song before deleting it.")
                                  : juce::String("The Channel Rack could not take that edit.");
}
} // namespace alien::rack_pattern
