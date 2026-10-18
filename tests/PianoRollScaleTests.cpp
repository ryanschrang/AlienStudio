#include "PianoRollScale.h"
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using namespace alien;
    ScaleSettings cMajor;
    require(isPitchInScale(60, cMajor) && isPitchInScale(64, cMajor) && isPitchInScale(71, cMajor)
            && !isPitchInScale(61, cMajor), "C major membership spans each pitch class");
    require(snapPitchToScale(61, cMajor) == 60 && snapPitchToScale(61, cMajor, 1) == 62
            && snapPitchToScale(61, cMajor, -1) == 60, "nearest and directional snapping resolve ties predictably");
    require(snapPitchToScale(0, cMajor, -1) == 0 && snapPitchToScale(127, cMajor, 1) == 127,
            "MIDI boundaries remain valid");

    ScaleSettings eMinor { 4, ScaleType::naturalMinor, true, true };
    require(isPitchInScale(64, eMinor) && isPitchInScale(66, eMinor) && isPitchInScale(67, eMinor)
            && !isPitchInScale(65, eMinor), "scale roots transpose interval masks");
    require(snapPitchToScale(65, eMinor, 1) == 66 && snapPitchToScale(65, eMinor, -1) == 64,
            "transposed directional snapping uses the selected scale");

    for (int type = 0; type <= static_cast<int>(ScaleType::chromatic); ++type) {
        const auto value = static_cast<ScaleType>(type);
        require((scaleMask(value) & 1u) != 0, "every scale includes its root");
    }
    ScaleSettings chromatic { 7, ScaleType::chromatic, true, true };
    for (int pitch = 0; pitch < 128; ++pitch)
        require(snapPitchToScale(pitch, chromatic, 1) == pitch, "chromatic scale leaves every MIDI pitch unchanged");
    ScaleSettings invalid { 12, ScaleType::major, true, true };
    require(!isPitchInScale(60, invalid) && snapPitchToScale(61, invalid) == 61,
            "invalid scale settings fail without altering pitch");
    std::cout << "PASS: scale masks, transposition, snapping directions and MIDI limits\n";
}
