#pragma once
#include "PianoRollEuclidean.h"

static void euclideanTests() {
    using namespace alien;
    for (int steps = 1; steps <= 64; ++steps) for (int hits = 0; hits <= steps; ++hits) {
        for (int rotation : {0, steps / 2, steps - 1}) {
            PianoRollPattern pattern; pattern.loopSteps = 64;
            const auto result = insertEuclidean(pattern, 0, {steps, hits, rotation, 60, false});
            require(pattern.valid() && pattern.count == static_cast<std::size_t>(hits), "Euclidean hit count matches every valid steps/hits combination");
            require(result.changed() == (hits != 0) && result.selection.count == pattern.count, "generated notes are the entire returned selection");
            int minGap = 65, maxGap = 0;
            for (std::size_t i = 0; i < pattern.count; ++i) {
                const auto& note = pattern.notes[i];
                const int cell = static_cast<int>(note.startStep);
                require(note.pitch == 60 && note.lengthSteps == .5 && note.probability == 1.f && result.selection.contains(note.id), "Euclidean gates, chance and identities are valid");
                const int original = (cell - rotation + steps) % steps;
                require((original * hits) % steps < hits, "positive rotation shifts the rhythm right");
                const int next = static_cast<int>(pattern.notes[(i + 1) % pattern.count].startStep) + (i + 1 == pattern.count ? steps : 0);
                const int gap = next - cell; minGap = std::min(minGap, gap); maxGap = std::max(maxGap, gap);
            }
            require(!hits || maxGap - minGap <= 1, "cyclic hit gaps differ by at most one step");
        }
    }
    PianoRollPattern pattern;
    const auto old = pattern.add(42, 2, 3, .4f); const auto original = pattern;
    auto result = insertEuclidean(pattern, 4.25, {8, 3, 0, 61, false});
    require(result.changed() && pattern.count == 4 && pattern.notes[1].startStep == 4.25
        && pattern.notes[2].startStep == 7.25 && pattern.notes[3].startStep == 10.25, "3/8 rhythm inserts at fractional cursor with known positions");
    require(sameNote(*pattern.find(old), *original.find(old)), "Euclidean insertion preserves existing notes");
    auto duplicate = original; insertEuclidean(duplicate, 4.25, {8, 3, 0, 61, false});
    require(samePianoRollPattern(pattern, duplicate), "same inputs reproduce pitches, timing and IDs without a seed");
    for (int root = 0; root < 12; ++root) for (int mode = 0; mode < 10; ++mode) for (int pitch : {0, 1, 61, 126, 127}) {
        PianoRollPattern generated;
        const ProjectMusicalScale context{true, root, static_cast<ScaleType>(mode)};
        require(insertEuclidean(generated, 0, {8, 1, 0, pitch, true}, context).changed(), "all project scale contexts generate");
        require(isPitchInScale(generated.notes[0].pitch, {root, context.type}), "project following keeps generated pitches in scale at MIDI edges");
    }
    PianoRollPattern chromatic;
    insertEuclidean(chromatic, 0, {8, 1, 0, 61, true});
    require(chromatic.notes[0].pitch == 61, "undefined project context preserves chromatic pitch");
    PianoRollPattern local;
    insertEuclidean(local, 0, {8, 1, 0, 61, false}, {true, 0, ScaleType::major});
    require(local.notes[0].pitch == 61, "disabled project follow does not snap");
    for (auto bad : {EuclideanSettings{0, 0}, EuclideanSettings{65, 1}, EuclideanSettings{8, -1}, EuclideanSettings{8, 9},
                     EuclideanSettings{8, 3, -1}, EuclideanSettings{8, 3, 8}, EuclideanSettings{8, 3, 0, -1}, EuclideanSettings{8, 3, 0, 128}}) {
        auto unchanged = original;
        require(insertEuclidean(unchanged, 0, bad).status == EuclideanStatus::invalidInput && samePianoRollPattern(unchanged, original), "invalid settings refuse atomically");
    }
    for (double bad : {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto unchanged = original;
        require(!insertEuclidean(unchanged, bad, {}).changed() && samePianoRollPattern(unchanged, original), "invalid cursor refuses atomically");
    }
    auto unchanged = original;
    require(insertEuclidean(unchanged, original.loopSteps - 7, {8, 1}).status == EuclideanStatus::outOfBounds
        && samePianoRollPattern(unchanged, original), "entire cycle must fit, even with few hits");
    require(insertEuclidean(unchanged, 0, {8, 0}).status == EuclideanStatus::unchanged && samePianoRollPattern(unchanged, original), "zero hits is a no-op");
    require(insertEuclidean(unchanged, 0, {8, 1, 0, 60, true}, {true, 12, ScaleType::major}).status == EuclideanStatus::invalidInput, "invalid project context refuses when following");
    PianoRollPattern full;
    for (std::size_t i = 0; i < full.maxNotes; ++i) require(full.add(60, 0, 1) != 0, "fill generator capacity fixture");
    const auto before = full;
    require(insertEuclidean(full, 0, {8, 3}).status == EuclideanStatus::capacityExceeded && samePianoRollPattern(full, before), "capacity refusal never leaves partial notes");
}
