#include "PianoRollChordStamp.h"
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using namespace alien;
    PianoRollPattern pattern;
    ChordStampSettings settings;
    settings.root = 60;
    settings.lengthSteps = 3.5;
    settings.velocity = 64.0f / 127.0f;
    auto result = stampChord(pattern, 4.25, settings);
    require(result.changed() && result.selection.count == 3 && pattern.count == 3,
            "major triad stamps transactionally and selects its notes");
    require(pattern.notes[0].pitch == 60 && pattern.notes[1].pitch == 64 && pattern.notes[2].pitch == 67,
            "major stamp uses root, third and fifth");
    for (std::size_t i = 0; i < result.selection.count; ++i) {
        const auto id = result.selection.ids[i];
        const auto* note = pattern.find(id);
        require(note != nullptr && note->startStep == 4.25 && note->lengthSteps == 3.5
                && note->velocity == settings.velocity, "stamp preserves shared timing, length and velocity");
    }

    settings.type = ChordType::minor7;
    settings.inversion = 2;
    const auto pitches = chordPitches(settings);
    require(pitches.count == 4 && pitches.values == std::array<int, 4> { 67, 70, 72, 75 },
            "second inversion raises the two lowest minor seventh tones");
    settings.type = ChordType::suspended4;
    settings.inversion = 0;
    require(chordPitches(settings).values[1] == 65, "suspended fourth replaces the third");

    const auto unchanged = pattern;
    settings.root = 127;
    require(stampChord(pattern, 8, settings).status == ChordStampStatus::invalidInput
            && samePianoRollPattern(pattern, unchanged), "out-of-range chord pitches leave pattern unchanged");
    settings.root = 60;
    settings.lengthSteps = 4;
    require(stampChord(pattern, 62, settings).status == ChordStampStatus::outOfBounds
            && samePianoRollPattern(pattern, unchanged), "clip-boundary failure is transactional");

    PianoRollPattern full;
    for (int i = 0; i < 126; ++i) full.add(i, 0, 1);
    const auto fullBefore = full;
    require(stampChord(full, 4, settings).status == ChordStampStatus::capacityExceeded
            && samePianoRollPattern(full, fullBefore), "capacity failure adds no partial chord");

    for (const auto type : { ChordType::major, ChordType::minor, ChordType::diminished,
                            ChordType::augmented, ChordType::suspended2, ChordType::suspended4,
                            ChordType::dominant7, ChordType::major7, ChordType::minor7 }) {
        settings.type = type;
        settings.inversion = 0;
        require(chordPitches(settings).count >= 3, "every exposed chord type generates pitches");
    }
    std::cout << "PASS: chord types, inversions, stamping and transactional limits\n";
}
