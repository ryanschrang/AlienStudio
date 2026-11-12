#pragma once
#include "PianoRollProcessing.h"
#include "PianoRollScale.h"
#include <optional>

namespace alien {
struct EuclideanScaleDegree {
    int degree{1}, octave{4}; // C4 is MIDI 60.
};

struct EuclideanSettings {
    int steps{16}, hits{5}, rotation{}, pitch{60};
    bool followProjectScale{};
    std::optional<EuclideanScaleDegree> scaleDegree;
};
enum class EuclideanStatus { generated, unchanged, invalidInput, capacityExceeded, outOfBounds, invalidScaleDegree };
struct EuclideanResult {
    EuclideanStatus status{EuclideanStatus::invalidInput};
    PianoRollSelection selection;
    bool changed() const noexcept { return status == EuclideanStatus::generated; }
};

inline std::optional<int> euclideanDegreePitch(const ProjectMusicalScale& context,
                                               EuclideanScaleDegree choice) noexcept {
    if (!context.defined || !context.valid() || choice.octave < -1 || choice.octave > 9
        || choice.degree < 1) return std::nullopt;
    const auto mask = scaleMask(context.type);
    for (int interval = 0; interval < 12; ++interval) {
        if ((mask & (1u << interval)) == 0 || --choice.degree != 0) continue;
        const int pitch = (choice.octave + 1) * 12 + context.rootPitchClass + interval;
        if (pitch <= 127) return pitch;
        break;
    }
    return std::nullopt;
}

// One sixteenth per cell, half-cell gates, velocity 102/127 and chance 100%.
// Integer modular distribution is deterministic without any random seed. A
// positive rotation moves the rhythm right; rotation zero begins with a hit.
inline EuclideanResult insertEuclidean(PianoRollPattern& pattern, double start,
    const EuclideanSettings& settings, const ProjectMusicalScale& context = {}) noexcept {
    EuclideanResult result;
    if (!pattern.valid() || !std::isfinite(start) || start < 0 || start > pattern.loopSteps
        || settings.steps < 1 || settings.steps > 64 || settings.hits < 0 || settings.hits > settings.steps
        || settings.rotation < 0 || settings.rotation >= settings.steps) return result;
    int pitch = settings.pitch;
    if (settings.scaleDegree) {
        const auto resolved = euclideanDegreePitch(context, *settings.scaleDegree);
        if (!resolved) { result.status = EuclideanStatus::invalidScaleDegree; return result; }
        pitch = *resolved;
    } else {
        if (pitch < 0 || pitch > 127 || (settings.followProjectScale && !context.valid())) return result;
        if (settings.followProjectScale && context.defined)
            pitch = snapPitchToScale(pitch, {context.rootPitchClass, context.type, false, true});
    }
    if (settings.hits == 0) { result.status = EuclideanStatus::unchanged; return result; }
    const int firstTick = PianoRollPattern::toTicks(start);
    if (firstTick + settings.steps * PianoRollPattern::ticksPerStep > pattern.loopSteps * PianoRollPattern::ticksPerStep) {
        result.status = EuclideanStatus::outOfBounds; return result;
    }
    if (pattern.count + static_cast<std::size_t>(settings.hits) > PianoRollPattern::maxNotes) {
        result.status = EuclideanStatus::capacityExceeded; return result;
    }
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
