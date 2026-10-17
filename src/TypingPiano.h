#pragma once
#include <array>
#include <algorithm>

// GUI-independent held-key state. Releases use the pitch captured at note-on.
class TypingPiano {
public:
    static constexpr std::array<int, 12> keys{'A','W','S','E','D','F','T','G','Y','H','U','J'};
    int octave() const noexcept { return baseOctave; }
    void shift(int delta) noexcept { baseOctave = std::clamp(baseOctave + delta, 0, 8); }
    template<class Send> bool press(int key, Send send) {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) {
                if (held[i] < 0) { held[i] = 12 * (baseOctave + 1) + static_cast<int>(i); send(held[i], true); }
                return true;
            }
        return false;
    }
    template<class IsDown, class Send> void releaseMissing(IsDown down, Send send) {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (held[i] >= 0 && !down(keys[i])) { const int note = held[i]; held[i] = -1; send(note, false); }
    }
    template<class Send> void releaseAll(Send send) { releaseMissing([](int) { return false; }, send); }
private:
    int baseOctave{4}; // MIDI 60 = C4
    std::array<int, 12> held = [] { std::array<int, 12> a{}; a.fill(-1); return a; }();
};
