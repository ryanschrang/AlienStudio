#pragma once
#include "PianoRollChordStamp.h"

namespace alien {

struct ProgressionSettings {
    int tonic { 48 }; // MIDI tonic also chooses the register.
    bool minor {};
    std::array<int, 4> degrees { 1, 5, 6, 4 };
    double chordSteps { 16.0 }; // One 4/4 bar, on the sixteenth-note grid.
    float velocity { 0.8f };
};

// Message-thread operation. Build on a copy so a failed final chord cannot leave
// a partial progression, consume IDs, or alter any existing notes.
inline ChordStampResult insertProgression(PianoRollPattern& pattern, double start,
                                         const ProgressionSettings& settings) noexcept {
    ChordStampResult result;
    if (!pattern.valid() || settings.tonic < 0 || settings.tonic > 127
        || !std::isfinite(start) || !std::isfinite(settings.chordSteps)
        || settings.chordSteps < PianoRollPattern::minLength
        || settings.chordSteps > pattern.loopSteps || start < 0 || start > pattern.loopSteps)
        return result;
    const auto scale = settings.minor ? std::array<int, 7>{0, 2, 3, 5, 7, 8, 10}
                                      : std::array<int, 7>{0, 2, 4, 5, 7, 9, 11};
    const int lengthTicks = PianoRollPattern::toTicks(settings.chordSteps);
    const int startTicks = PianoRollPattern::toTicks(start);
    if (startTicks + 4 * lengthTicks > pattern.loopSteps * PianoRollPattern::ticksPerStep) {
        result.status = ChordStampStatus::outOfBounds;
        return result;
    }
    auto candidate = pattern;
    for (std::size_t chord = 0; chord < settings.degrees.size(); ++chord) {
        const int degree = settings.degrees[chord] - 1;
        if (degree < 0 || degree >= 7) return {};
        const int third = scale[(degree + 2) % 7] + ((degree + 2) / 7) * 12 - scale[degree];
        const int fifth = scale[(degree + 4) % 7] + ((degree + 4) / 7) * 12 - scale[degree];
        ChordStampSettings stamp;
        stamp.root = settings.tonic + scale[degree];
        stamp.type = fifth == 6 ? ChordType::diminished : third == 3 ? ChordType::minor : ChordType::major;
        stamp.lengthSteps = static_cast<double>(lengthTicks) / PianoRollPattern::ticksPerStep;
        stamp.velocity = settings.velocity;
        const auto added = stampChord(candidate,
            static_cast<double>(startTicks + static_cast<int>(chord) * lengthTicks) / PianoRollPattern::ticksPerStep, stamp);
        if (!added.changed()) return { added.status, {} };
        for (std::size_t i = 0; i < added.selection.count; ++i) result.selection.add(added.selection.ids[i]);
    }
    pattern = candidate;
    result.status = ChordStampStatus::stamped;
    return result;
}
} // namespace alien
