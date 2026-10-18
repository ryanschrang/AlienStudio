#pragma once

#include "PianoRollProcessing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace alien {

enum class ChordType { major, minor, diminished, augmented, suspended2, suspended4,
                       dominant7, major7, minor7 };

struct ChordStampSettings {
    int root { 60 };
    ChordType type { ChordType::major };
    int inversion {};
    double lengthSteps { 4.0 };
    float velocity { 0.8f };
};

enum class ChordStampStatus { stamped, invalidInput, capacityExceeded, outOfBounds };

struct ChordPitches {
    std::array<int, 4> values {};
    std::size_t count {};
};

struct ChordStampResult {
    ChordStampStatus status { ChordStampStatus::invalidInput };
    PianoRollSelection selection;
    bool changed() const noexcept { return status == ChordStampStatus::stamped; }
};

inline ChordPitches chordPitches(const ChordStampSettings& settings) noexcept {
    ChordPitches result;
    std::array<int, 4> intervals {};
    switch (settings.type) {
        case ChordType::major:      intervals = { 0, 4, 7, 0 }; result.count = 3; break;
        case ChordType::minor:      intervals = { 0, 3, 7, 0 }; result.count = 3; break;
        case ChordType::diminished: intervals = { 0, 3, 6, 0 }; result.count = 3; break;
        case ChordType::augmented:  intervals = { 0, 4, 8, 0 }; result.count = 3; break;
        case ChordType::suspended2: intervals = { 0, 2, 7, 0 }; result.count = 3; break;
        case ChordType::suspended4: intervals = { 0, 5, 7, 0 }; result.count = 3; break;
        case ChordType::dominant7:  intervals = { 0, 4, 7, 10 }; result.count = 4; break;
        case ChordType::major7:     intervals = { 0, 4, 7, 11 }; result.count = 4; break;
        case ChordType::minor7:     intervals = { 0, 3, 7, 10 }; result.count = 4; break;
    }
    if (settings.root < 0 || settings.root > 127 || settings.inversion < 0
        || settings.inversion >= static_cast<int>(result.count)) return {};
    for (std::size_t i = 0; i < result.count; ++i) result.values[i] = settings.root + intervals[i];
    for (int inversion = 0; inversion < settings.inversion; ++inversion) {
        const int raised = result.values[0] + 12;
        for (std::size_t i = 1; i < result.count; ++i) result.values[i - 1] = result.values[i];
        result.values[result.count - 1] = raised;
    }
    for (std::size_t i = 0; i < result.count; ++i)
        if (result.values[i] < 0 || result.values[i] > 127) return {};
    return result;
}

inline ChordStampResult stampChord(PianoRollPattern& pattern, double start,
                                   const ChordStampSettings& settings) noexcept {
    ChordStampResult result;
    const auto pitches = chordPitches(settings);
    if (!pattern.valid() || pitches.count == 0 || !std::isfinite(start)
        || !std::isfinite(settings.lengthSteps) || !std::isfinite(settings.velocity)
        || settings.lengthSteps < PianoRollPattern::minLength || settings.velocity <= 0.0f
        || settings.velocity > 1.0f) return result;
    const int startTicks = PianoRollPattern::toTicks(start);
    const int lengthTicks = PianoRollPattern::toTicks(settings.lengthSteps);
    if (startTicks < 0 || startTicks >= pattern.loopSteps * PianoRollPattern::ticksPerStep
        || lengthTicks < 1 || startTicks + lengthTicks > pattern.loopSteps * PianoRollPattern::ticksPerStep) {
        result.status = ChordStampStatus::outOfBounds;
        return result;
    }
    if (pattern.count + pitches.count > PianoRollPattern::maxNotes) {
        result.status = ChordStampStatus::capacityExceeded;
        return result;
    }
    auto candidate = pattern;
    for (std::size_t i = 0; i < pitches.count; ++i) {
        const auto id = candidate.add(pitches.values[i], start, settings.lengthSteps, settings.velocity);
        if (id == 0 || !result.selection.add(id)) return {};
    }
    pattern = candidate;
    result.status = ChordStampStatus::stamped;
    return result;
}

} // namespace alien
