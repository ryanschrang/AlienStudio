#include "PianoRollProgression.h"
#include <cstdlib>
#include <iostream>
#include <limits>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    using namespace alien;
    PianoRollPattern pattern;
    ProgressionSettings settings;
    auto result = insertProgression(pattern, 0, settings);
    require(result.changed() && pattern.count == 12 && result.selection.count == 12,
            "four triads are inserted and selected");
    const std::array<int, 12> expected {48,52,55,55,59,62,57,60,64,53,57,60};
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(pattern.notes[i].pitch == expected[i] && pattern.notes[i].startStep == (i / 3) * 16
                && pattern.notes[i].lengthSteps == 16, "major I V vi IV has diatonic quality and contiguous bars");
    pattern = {};
    settings.minor = true;
    settings.degrees = {1, 6, 3, 7};
    result = insertProgression(pattern, 0, settings);
    require(result.changed() && pattern.notes[1].pitch == 51 && pattern.notes[4].pitch == 60,
            "minor uses minor tonic and major sixth-degree triad");
    settings.degrees = {2, 2, 2, 2};
    pattern = {};
    require(insertProgression(pattern, 0, settings).changed() && pattern.notes[2].pitch == 56,
            "natural minor second degree is diminished");
    const auto before = pattern;
    require(insertProgression(pattern, 1, settings).status == ChordStampStatus::outOfBounds
            && samePianoRollPattern(pattern, before), "loop overflow leaves existing notes unchanged");
    settings.tonic = 120;
    require(!insertProgression(pattern, 0, settings).changed() && samePianoRollPattern(pattern, before),
            "invalid pitch leaves notes and IDs unchanged");
    settings.tonic = 48;
    settings.degrees[3] = 8;
    require(!insertProgression(pattern, 0, settings).changed() && samePianoRollPattern(pattern, before),
            "invalid final degree adds no partial sequence");
    settings.degrees[3] = 2;
    settings.chordSteps = std::numeric_limits<double>::infinity();
    require(!insertProgression(pattern, 0, settings).changed(), "infinite duration is rejected before conversion");
    settings.chordSteps = 4.0;
    PianoRollPattern full;
    while (full.count + 11 < PianoRollPattern::maxNotes) full.add(60, 0, 1);
    const auto fullBefore = full;
    require(insertProgression(full, 0, settings).status == ChordStampStatus::capacityExceeded
            && samePianoRollPattern(full, fullBefore), "capacity failure on final chord is atomic");
    settings.chordSteps = 1.001;
    pattern = {};
    require(insertProgression(pattern, 0.25, settings).changed() && pattern.valid()
            && pattern.notes[3].startStep == 1.25, "fractional inputs share a rounded tick grid without gaps");
    std::cout << "PASS: progression harmony, timing, selection and atomic limits\n";
}
