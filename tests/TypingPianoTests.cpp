#include "TypingPiano.h"
#include "SynthEngine.h"
#include "NoteQueue.h"
#include <cstdlib>
#include <iostream>
#include <vector>
static void check(bool ok) { if (!ok) { std::cerr << "Typing piano lifecycle failed\n"; std::exit(1); } }
int main() {
    TypingPiano piano;
    alien::SynthEngine synth;
    synth.prepare(48000);
    NoteQueue queue;
    std::vector<NoteQueue::Event> events;
    auto send = [&](int n, bool on) { queue.push(n, on ? .8f : 0.f); };
    auto drain = [&] {
        NoteQueue::Event e{};
        while (queue.pop(e)) {
            events.push_back(e);
            if (e.velocity > 0) synth.noteOn(e.note, e.velocity); else synth.noteOff(e.note);
        }
    };
    for (std::size_t i = 0; i < piano.keys.size(); ++i) {
        check(piano.press(piano.keys[i], send));
        check(piano.press(piano.keys[i], send)); // repeat must not retrigger
    }
    check(!piano.press('Q', send));
    drain();
    check(events.size() == 12 && synth.activeVoices() == 12);
    for (int i = 0; i < 12; ++i) check(events[i].note == 60 + i && events[i].velocity > 0);
    piano.shift(1);
    piano.releaseMissing([](int k) { return k != 'A'; }, send);
    drain();
    check(events.back().note == 60 && events.back().velocity == 0);
    piano.press('A', send);
    drain();
    check(events.back().note == 72);
    piano.releaseAll(send); // focus loss, preset/transport changes, shutdown
    drain();
    check(events.size() == 26);
    piano.releaseAll(send);
    drain();
    check(events.size() == 26);
    for (int i = 0; i < 480000; ++i) synth.nextSample();
    check(synth.activeVoices() == 0);
    piano.shift(-100); check(piano.octave() == 0);
    piano.press('A', send); piano.releaseAll(send); drain();
    check(events[26].note == 12 && events[27].note == 12);
    piano.shift(100); check(piano.octave() == 8);
    piano.press('J', send); piano.releaseAll(send); drain();
    check(events[28].note == 119 && events[29].note == 119);
}
