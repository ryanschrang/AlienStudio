#pragma once
#include "PianoRollSequence.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace alien {

// GUI-side selection uses stable note IDs, so changing pitches or chopping one
// note cannot silently select a different note at the same array position.
struct PianoRollSelection {
    std::array<std::uint32_t, PianoRollPattern::maxNotes> ids{};
    std::size_t count{};

    bool contains(std::uint32_t id) const noexcept {
        if (id == 0) return false;
        for (std::size_t i = 0; i < std::min(count, ids.size()); ++i)
            if (ids[i] == id) return true;
        return false;
    }
    bool add(std::uint32_t id) noexcept {
        if (id == 0 || count > ids.size()) return false;
        if (contains(id)) return true;
        if (count == ids.size()) return false;
        ids[count++] = id;
        return true;
    }
    bool remove(std::uint32_t id) noexcept {
        if (count > ids.size()) return false;
        for (std::size_t i = 0; i < std::min(count, ids.size()); ++i) if (ids[i] == id) {
            for (std::size_t j = i + 1; j < std::min(count, ids.size()); ++j) ids[j - 1] = ids[j];
            ids[--count] = 0;
            return true;
        }
        return false;
    }
    bool toggle(std::uint32_t id) noexcept {
        if (contains(id)) { remove(id); return false; }
        return add(id);
    }
    void clear() noexcept { ids = {}; count = 0; }
    void retain(const PianoRollPattern& pattern) noexcept {
        std::size_t destination = 0;
        for (std::size_t i = 0; i < std::min(count, ids.size()); ++i)
            if (pattern.find(ids[i])) ids[destination++] = ids[i];
        for (std::size_t i = destination; i < ids.size(); ++i) ids[i] = 0;
        count = destination;
    }
    void selectAll(const PianoRollPattern& pattern) noexcept {
        clear();
        for (std::size_t i = 0; i < std::min(pattern.count, pattern.maxNotes); ++i) add(pattern.notes[i].id);
    }
    bool valid() const noexcept {
        if (count > ids.size()) return false;
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == 0) return false;
            for (std::size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return false;
        }
        return true;
    }
};

inline bool samePianoRollPattern(const PianoRollPattern& a, const PianoRollPattern& b) noexcept {
    if (a.count != b.count || a.loopSteps != b.loopSteps || a.count > a.maxNotes) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        const auto& x = a.notes[i];
        const auto& y = b.notes[i];
        if (x.id != y.id || x.pitch != y.pitch || x.startStep != y.startStep
            || x.lengthSteps != y.lengthSteps || x.velocity != y.velocity) return false;
    }
    return true;
}

enum class ProcessingStatus { unchanged, changed, capacityExceeded, invalidInput };
struct ProcessingResult {
    ProcessingStatus status{ProcessingStatus::unchanged};
    PianoRollSelection selection;
    bool changed() const noexcept { return status == ProcessingStatus::changed; }
};

// Times use sixteenth-note steps. One beat is four steps; the editor stores
// 240 ticks per step. Values outside these documented limits are rejected.
struct StrumSettings {
    double delaySteps{0.125};            // 0..4 per note within each chord
    float velocityVariation{0.08f};      // 0..1, additive +/- velocity
    bool descending{};
    std::uint32_t seed{1};
};
struct ChopSettings {
    double divisionSteps{0.25};          // 1/240..256, including a partial tail
    double gate{1.0};                    // 0.05..1 of each fragment's duration
};
struct RandomizeSettings {
    double timingSteps{0.125};           // 0..4, independent +/- note offset
    float velocityVariation{0.08f};      // 0..1, additive +/- velocity
    int pitchSemitones{};               // 0..24, independent +/- semitones
    std::uint32_t seed{1};
};
struct QuantizeSettings {
    double gridSteps{1.0};               // 1/240..256
    double strength{1.0};                // 0..1; zero leaves timing unchanged
    double swing{};                     // 0..0.75, delays odd grid points
};

namespace piano_roll_processing_detail {
inline bool within(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}
inline bool validInputs(const PianoRollPattern& pattern, const PianoRollSelection& selection) noexcept {
    return pattern.valid() && selection.valid();
}
inline ProcessingResult result(ProcessingStatus status, const PianoRollPattern& pattern,
                               const PianoRollSelection& selection) noexcept {
    auto retained = selection;
    if (retained.valid()) retained.retain(pattern); else retained.clear();
    return {status, retained};
}
inline ProcessingResult commit(PianoRollPattern& pattern, const PianoRollPattern& edited,
                               const PianoRollSelection& selection) noexcept {
    if (!edited.valid()) return result(ProcessingStatus::invalidInput, pattern, selection);
    if (samePianoRollPattern(pattern, edited)) return result(ProcessingStatus::unchanged, pattern, selection);
    pattern = edited;
    return result(ProcessingStatus::changed, pattern, selection);
}
inline int latestStartTicks(const PianoRollPattern& pattern, const PianoRollNote& note) noexcept {
    return pattern.loopSteps * pattern.ticksPerStep - pattern.toTicks(note.lengthSteps);
}
inline void setStart(PianoRollNote& note, int ticks, const PianoRollPattern& pattern) noexcept {
    note.startStep = pattern.fromTicks(std::clamp(ticks, 0, latestStartTicks(pattern, note)));
}
inline float velocity(float original, double offset) noexcept {
    return static_cast<float>(std::clamp(static_cast<double>(original) + offset, 1.0 / 127.0, 1.0));
}

// Specified integer operations give the same seed identical results on Windows
// and Linux, independent of standard-library random-distribution algorithms.
class Random {
public:
    explicit Random(std::uint32_t seed) noexcept : state(seed == 0 ? 0x9e3779b9u : seed) {}
    std::uint32_t next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    double signedUnit() noexcept { return static_cast<double>(next() >> 8) * (2.0 / 16777215.0) - 1.0; }
    int pitchOffset(int amount) noexcept {
        const auto range = static_cast<std::uint32_t>(2 * amount + 1);
        return static_cast<int>((static_cast<std::uint64_t>(next()) * range) >> 32) - amount;
    }
private:
    std::uint32_t state;
};
}

inline ProcessingResult processStrum(PianoRollPattern& pattern, const PianoRollSelection& selection,
                                    const StrumSettings& settings = {}) noexcept {
    using namespace piano_roll_processing_detail;
    if (!validInputs(pattern, selection) || !within(settings.delaySteps, 0, 4)
        || !within(settings.velocityVariation, 0, 1))
        return result(ProcessingStatus::invalidInput, pattern, selection);
    if (settings.delaySteps == 0 && settings.velocityVariation == 0)
        return result(ProcessingStatus::unchanged, pattern, selection);
    auto edited = pattern;
    std::array<std::size_t, PianoRollPattern::maxNotes> indices{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < pattern.count; ++i)
        if (selection.contains(pattern.notes[i].id)) indices[count++] = i;
    std::sort(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(count),
        [&](std::size_t a, std::size_t b) {
            const auto& x = pattern.notes[a]; const auto& y = pattern.notes[b];
            if (x.startStep != y.startStep) return x.startStep < y.startStep;
            if (x.pitch != y.pitch) return settings.descending ? x.pitch > y.pitch : x.pitch < y.pitch;
            return x.id < y.id;
        });
    Random random(settings.seed);
    const int delay = pattern.toTicks(settings.delaySteps);
    int chordStart = -1, rank = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = indices[i];
        const auto& original = pattern.notes[index];
        const int onset = pattern.toTicks(original.startStep);
        if (onset != chordStart) { chordStart = onset; rank = 0; }
        auto& note = edited.notes[index];
        if (delay > 0) setStart(note, onset + rank * delay, edited);
        ++rank;
        if (settings.velocityVariation > 0)
            note.velocity = velocity(original.velocity, random.signedUnit() * settings.velocityVariation);
    }
    return commit(pattern, edited, selection);
}

inline ProcessingResult processChop(PianoRollPattern& pattern, const PianoRollSelection& selection,
                                   const ChopSettings& settings = {}) noexcept {
    using namespace piano_roll_processing_detail;
    if (!validInputs(pattern, selection) || !within(settings.divisionSteps, pattern.timeQuantum, 256)
        || !within(settings.gate, 0.05, 1))
        return result(ProcessingStatus::invalidInput, pattern, selection);
    const int division = pattern.toTicks(settings.divisionSteps);
    std::size_t required = pattern.count;
    for (std::size_t i = 0; i < pattern.count; ++i) if (selection.contains(pattern.notes[i].id)) {
        const int length = pattern.toTicks(pattern.notes[i].lengthSteps);
        const auto fragments = static_cast<std::size_t>((length + division - 1) / division);
        if (fragments - 1 > pattern.maxNotes - required)
            return result(ProcessingStatus::capacityExceeded, pattern, selection);
        required += fragments - 1;
    }
    auto edited = pattern;
    auto selected = selection;
    selected.retain(pattern);
    for (std::size_t i = 0; i < pattern.count; ++i) if (selection.contains(pattern.notes[i].id)) {
        const auto& original = pattern.notes[i];
        const int length = pattern.toTicks(original.lengthSteps);
        const int onset = pattern.toTicks(original.startStep);
        for (int offset = 0; offset < length; offset += division) {
            const int extent = std::min(division, length - offset);
            const int gated = std::clamp(static_cast<int>(std::lround(extent * settings.gate)), 1, extent);
            if (offset == 0) {
                edited.find(original.id)->lengthSteps = pattern.fromTicks(gated);
            } else {
                const auto id = edited.add(original.pitch, pattern.fromTicks(onset + offset),
                                           pattern.fromTicks(gated), original.velocity);
                if (id == 0 || !selected.add(id))
                    return result(ProcessingStatus::capacityExceeded, pattern, selection);
            }
        }
    }
    return commit(pattern, edited, selected);
}

inline ProcessingResult processRandomize(PianoRollPattern& pattern, const PianoRollSelection& selection,
                                        const RandomizeSettings& settings = {}) noexcept {
    using namespace piano_roll_processing_detail;
    if (!validInputs(pattern, selection) || !within(settings.timingSteps, 0, 4)
        || !within(settings.velocityVariation, 0, 1) || settings.pitchSemitones < 0 || settings.pitchSemitones > 24)
        return result(ProcessingStatus::invalidInput, pattern, selection);
    if (settings.timingSteps == 0 && settings.velocityVariation == 0 && settings.pitchSemitones == 0)
        return result(ProcessingStatus::unchanged, pattern, selection);
    auto edited = pattern;
    Random random(settings.seed);
    for (std::size_t i = 0; i < pattern.count; ++i) if (selection.contains(pattern.notes[i].id)) {
        const auto& original = pattern.notes[i];
        auto& note = edited.notes[i];
        const double timing = random.signedUnit() * settings.timingSteps * pattern.ticksPerStep;
        const double variation = random.signedUnit() * settings.velocityVariation;
        const int pitch = random.pitchOffset(settings.pitchSemitones);
        if (settings.timingSteps > 0)
            setStart(note, pattern.toTicks(original.startStep) + static_cast<int>(std::lround(timing)), edited);
        if (settings.velocityVariation > 0) note.velocity = velocity(original.velocity, variation);
        if (settings.pitchSemitones > 0) note.pitch = std::clamp(original.pitch + pitch, 0, 127);
    }
    return commit(pattern, edited, selection);
}

inline ProcessingResult processQuantize(PianoRollPattern& pattern, const PianoRollSelection& selection,
                                       const QuantizeSettings& settings = {}) noexcept {
    using namespace piano_roll_processing_detail;
    if (!validInputs(pattern, selection) || !within(settings.gridSteps, pattern.timeQuantum, 256)
        || !within(settings.strength, 0, 1) || !within(settings.swing, 0, 0.75))
        return result(ProcessingStatus::invalidInput, pattern, selection);
    if (settings.strength == 0) return result(ProcessingStatus::unchanged, pattern, selection);
    auto edited = pattern;
    const int grid = pattern.toTicks(settings.gridSteps);
    for (std::size_t i = 0; i < pattern.count; ++i) if (selection.contains(pattern.notes[i].id)) {
        const auto& original = pattern.notes[i];
        const int onset = pattern.toTicks(original.startStep);
        const int nearestStraight = onset / grid;
        int closest = 0;
        int distance = std::numeric_limits<int>::max();
        for (int point = std::max(0, nearestStraight - 2); point <= nearestStraight + 2; ++point) {
            const double swung = point * grid + ((point % 2) != 0 ? grid * settings.swing : 0.0);
            // Pick the nearest real grid point before the final duration-safe
            // clamp. Clamping every candidate would invent an off-grid point
            // at the note's current/latest legal start and prevent snapping.
            const int target = static_cast<int>(std::lround(swung));
            const int candidateDistance = std::abs(target - onset);
            if (candidateDistance < distance || (candidateDistance == distance && target < closest)) {
                closest = target;
                distance = candidateDistance;
            }
        }
        const int blended = onset + static_cast<int>(std::lround((closest - onset) * settings.strength));
        setStart(edited.notes[i], blended, edited);
    }
    return commit(pattern, edited, selection);
}
}
