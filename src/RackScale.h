#pragma once
#include "PianoRollScale.h"
#include "RackModel.h"

namespace alien::rack {
// Nearest in-scale step offset, ties downward, searched inside the actual graph
// range. Clamping an unrestricted snapped note could otherwise escape the scale.
inline int snapStepPitchToScale(int value, const ProjectMusicalScale& context) noexcept {
    const int low = fieldMinimum(StepField::pitch), high = fieldMaximum(StepField::pitch);
    value = std::clamp(value, low, high);
    if (!context.defined || !context.valid()) return value;
    const ScaleSettings scale{context.rootPitchClass, context.type, false, true};
    for (int distance = 0; distance <= 12; ++distance) {
        const int lower = value - distance, upper = value + distance;
        if (lower >= low && isPitchInScale(stepPitchBaseNote + lower, scale)) return lower;
        if (upper <= high && isPitchInScale(stepPitchBaseNote + upper, scale)) return upper;
    }
    return value;
}
} // namespace alien::rack
