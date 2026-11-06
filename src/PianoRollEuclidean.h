#pragma once
#include "PianoRollProcessing.h"
#include "PianoRollScale.h"

namespace alien {
struct EuclideanSettings {
    int steps{16}, hits{5}, rotation{}, pitch{60};
    bool followProjectScale{};
};
enum class EuclideanStatus { generated, unchanged, invalidInput, capacityExceeded, outOfBounds };
struct EuclideanResult {
    EuclideanStatus status{EuclideanStatus::invalidInput};
    PianoRollSelection selection;
    bool changed() const noexcept { return status == EuclideanStatus::generated; }
};

// One sixteenth per cell, half-cell gates, velocity 102/127 and chance 100%.
// Integer modular distribution is deterministic without any random seed. A
// positive rotation moves the rhythm right; rotation zero begins with a hit.
inline EuclideanResult insertEuclidean(PianoRollPattern& pattern, double start,
    const EuclideanSettings& settings, const ProjectMusicalScale& context = {}) noexcept {
    EuclideanResult result;
    if (!pattern.valid() || !std::isfinite(start) || start < 0 || start > pattern.loopSteps
        || settings.steps < 1 || settings.steps > 64 || settings.hits < 0 || settings.hits > settings.steps
        || settings.rotation < 0 || settings.rotation >= settings.steps || settings.pitch < 0 || settings.pitch > 127
        || (settings.followProjectScale && !context.valid())) return result;
    if (settings.hits == 0) { result.status = EuclideanStatus::unchanged; return result; }
    const int firstTick = PianoRollPattern::toTicks(start);
    if (firstTick + settings.steps * PianoRollPattern::ticksPerStep > pattern.loopSteps * PianoRollPattern::ticksPerStep) {
        result.status = EuclideanStatus::outOfBounds; return result;
    }
    if (pattern.count + static_cast<std::size_t>(settings.hits) > PianoRollPattern::maxNotes) {
        result.status = EuclideanStatus::capacityExceeded; return result;
    }
    const int pitch = settings.followProjectScale && context.defined
        ? snapPitchToScale(settings.pitch, {context.rootPitchClass, context.type, false, true}) : settings.pitch;
    auto candidate = pattern;
    for (int cell = 0; cell < settings.steps; ++cell) {
        const int unrotated = (cell - settings.rotation + settings.steps) % settings.steps;
        if ((unrotated * settings.hits) % settings.steps >= settings.hits) continue;
        const auto id = candidate.add(pitch, PianoRollPattern::fromTicks(firstTick + cell * PianoRollPattern::ticksPerStep), .5, 102.f / 127.f);
        if (!id || !result.selection.add(id)) return {};
    }
    if (!candidate.valid()) return {};
    pattern = candidate; result.status = EuclideanStatus::generated;
    return result;
}
} // namespace alien
