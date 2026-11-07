#pragma once
#include "PianoRollChordStamp.h"
#include <cstdint>

namespace alien::timeline {
// Harmonic context, not a voicing or a request to play notes. Quality shares the
// existing Piano Roll chord vocabulary; root is a pitch class, not MIDI register.
struct SongChord {
    std::uint32_t id{};
    std::int64_t start{}, end{};
    int root{};
    ChordType quality{ChordType::major};
    bool operator==(const SongChord&) const = default;
};
inline bool validChordRoot(int root) noexcept { return root >= 0 && root < 12; }
inline bool validChordQuality(ChordType quality) noexcept { return static_cast<int>(quality) >= 0 && static_cast<int>(quality) < 9; }
inline const char* chordRootName(int root) noexcept {
    constexpr std::array<const char*, 12> names{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return validChordRoot(root) ? names[root] : "?";
}
inline const char* chordQualityName(ChordType quality) noexcept {
    constexpr std::array<const char*, 9> names{"Major", "Minor", "Diminished", "Augmented", "Sus2", "Sus4", "Dominant 7", "Major 7", "Minor 7"};
    return validChordQuality(quality) ? names[static_cast<int>(quality)] : "?";
}
} // namespace alien::timeline
