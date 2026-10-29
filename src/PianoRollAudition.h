#pragma once
// Piano-roll key previews (PROD-06): where each previewed note plays, remembered per note, so a note-off goes to
// the instrument that got the note-on even if the piano roll has meanwhile switched to another track or Channel
// Rack channel. Without this, a key held while the target changes would leave a note sounding.
//
// No JUCE and no audio code. Message thread only.
#include <array>

namespace alien {

class PianoRollAuditionRouter {
public:
    // What the piano roll edits: a Session track (its clip's track, or a Song clip's track), or a rack channel.
    struct Target {
        bool rack{};
        int index{-1};   // track or channel; -1: nothing to play on
        bool operator==(const Target&) const = default;
    };
    // Plays (velocity > 0) or releases (velocity 0) `note` on `current`, calling send(target, note, velocity) for
    // each note-on or note-off to deliver. A note-off goes to where that note was started. A note started again
    // while it is still held is released first, so it is never doubled. Notes outside 0..127 are ignored.
    template <typename Send>
    void play(int note, float velocity, Target current, Send&& send) {
        if (note < 0 || note > 127) return;
        auto& held = notes[static_cast<std::size_t>(note)];
        if (held.index >= 0) {
            send(held, note, 0.0f);
            held = {};
        }
        if (velocity <= 0.0f || current.index < 0) return;
        send(current, note, velocity);
        held = current;
    }
    // Where a held note is playing (index -1 when it is not held).
    Target heldOn(int note) const noexcept { return note >= 0 && note <= 127 ? notes[static_cast<std::size_t>(note)] : Target{}; }
    // Releases every held note where it plays.
    template <typename Send>
    void releaseAll(Send&& send) {
        for (int note = 0; note < 128; ++note) play(note, 0.0f, {}, send);
    }

private:
    std::array<Target, 128> notes{};
};

} // namespace alien
