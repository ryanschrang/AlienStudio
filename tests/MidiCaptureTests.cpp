// Capture MIDI (MIDI-04): the history's note pairing, phrase finding and timing, the insert into a clip, and the
// engine path from live notes to the capture ring, offline through the real audio callback.
#include "MidiCapture.h"
#include "ToneEngine.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>

static thread_local std::size_t allocationCount = 0;
void* operator new(std::size_t size) {
    ++allocationCount;
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace alien;
using namespace alien::capture;

static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
static bool approx(double a, double b, double tolerance = 1.0e-6) { return std::fabs(a - b) <= tolerance; }

static Event on(double seconds, int pitch, float velocity = 0.8f, int source = typingSource, float bpm = 120.0f) {
    return {seconds, bpm, velocity, static_cast<std::int16_t>(pitch), static_cast<std::uint8_t>(source)};
}
static Event off(double seconds, int pitch, int source = typingSource, float bpm = 120.0f) {
    return {seconds, bpm, 0.0f, static_cast<std::int16_t>(pitch), static_cast<std::uint8_t>(source)};
}

static void pairingAndTiming() {
    CaptureHistory history;
    check(history.capture(10.0).empty(), "nothing played captures nothing");
    // 120 BPM: a beat is half a second, four sixteenths.
    history.add(on(5.0, 60, 0.5f)); history.add(off(5.25, 60));
    history.add(on(5.5, 64)); history.add(off(6.0, 64));
    const auto phrase = history.capture(7.0);
    check(phrase.notes.size() == 2, "two played notes are captured");
    check(approx(phrase.bpm, 120.0), "the phrase keeps the tempo it was played at");
    if (phrase.notes.size() == 2) {
        check(phrase.notes[0].pitch == 60 && approx(phrase.notes[0].startStep, 0) && approx(phrase.notes[0].lengthSteps, 2),
              "the first note starts the block and lasts as long as it was held");
        check(approx(phrase.notes[0].velocity, 0.5f), "velocity is kept");
        check(phrase.notes[1].pitch == 64 && approx(phrase.notes[1].startStep, 4) && approx(phrase.notes[1].lengthSteps, 4),
              "later notes keep their distance from the first in sixteenths");
    }
    check(phrase.lengthTicks == barTicks && phrase.stillHeld == 0 && phrase.leftOut == 0, "a short phrase is one bar");

    CaptureHistory slow;   // 60 BPM: a second is one beat
    slow.add(on(0.0, 48, 0.8f, typingSource, 60.0f)); slow.add(off(1.0, 48, typingSource, 60.0f));
    const auto at60 = slow.capture(2.0);
    check(at60.notes.size() == 1 && approx(at60.notes[0].lengthSteps, 4) && approx(at60.bpm, 60.0), "seconds become ticks at the phrase's tempo");

    CaptureHistory blip;
    blip.add(on(1.0, 50)); blip.add(off(1.0, 50));
    const auto tiny = blip.capture(1.0);
    check(tiny.notes.size() == 1 && PianoRollPattern::toTicks(tiny.notes[0].lengthSteps) == minimumLengthTicks,
          "a note released in the block it was pressed still gets a short length");
}

static void phrases() {
    CaptureHistory history;
    history.add(on(0.0, 60)); history.add(off(0.5, 60));
    history.add(on(3.0, 62)); history.add(off(3.5, 62));   // 2.5 s with no key down: a new phrase
    history.add(on(4.5, 64)); history.add(off(5.0, 64));   // 1 s later: the same phrase
    const auto phrase = history.capture(6.0);
    check(phrase.notes.size() == 2 && phrase.notes[0].pitch == 62 && approx(phrase.notes[0].startStep, 0)
          && phrase.notes[1].pitch == 64 && approx(phrase.notes[1].startStep, 12),
          "only the phrase after the last pause is captured, starting at its first note");

    CaptureHistory held;   // a long-held note bridges what would be a gap
    held.add(on(0.0, 36)); held.add(on(0.5, 60)); held.add(off(1.0, 60));
    held.add(on(4.0, 62)); held.add(off(4.5, 62)); held.add(off(5.0, 36));
    check(held.capture(6.0).notes.size() == 3, "a pause counts only while no key is down");
}

static void pairingRepairs() {
    CaptureHistory history;
    history.add(on(0.0, 60)); history.add(on(0.25, 60)); history.add(off(0.5, 60));
    auto phrase = history.capture(1.0);
    check(phrase.notes.size() == 2 && approx(phrase.notes[0].lengthSteps, 2) && approx(phrase.notes[1].startStep, 2),
          "pressing a key that is down ends the first note there");

    CaptureHistory stuck;
    stuck.add(on(0.0, 60)); stuck.add(off(0.1, 61));   // a stray note-off ends nothing
    phrase = stuck.capture(1.0);
    check(phrase.notes.size() == 1 && approx(phrase.notes[0].lengthSteps, 8) && phrase.stillHeld == 1,
          "a key still down is ended at the moment of capture and counted");

    CaptureHistory sources;
    sources.add(on(0.0, 60, 0.8f, typingSource)); sources.add(on(0.0, 60, 0.8f, 2));
    sources.add(Event{0.5, 120.0f, 0.0f, -1, typingSource});   // the typing piano lets go of everything
    sources.add(off(1.0, 60, 2));
    phrase = sources.capture(2.0);
    check(phrase.notes.size() == 2 && approx(phrase.notes[0].lengthSteps, 4) && approx(phrase.notes[1].lengthSteps, 8) && phrase.stillHeld == 0,
          "the same key on two sources is two notes, and one source's all-notes-off ends only its own");

    CaptureHistory junk;
    junk.add(on(std::numeric_limits<double>::quiet_NaN(), 60)); junk.add(Event{0.0, 120.0f, 1.0f, 128, 0});
    junk.add(Event{0.0, 120.0f, 1.0f, 60, sourceCount});
    check(junk.size() == 0 && junk.capture(1.0).empty(), "damaged events are ignored");
}

static void longPhrases() {
    CaptureHistory history;
    // A note every half second for 40 s at 120 BPM, never a pause: 20 bars, more than the longest clip (16).
    for (int i = 0; i < 80; ++i) { history.add(on(i * 0.5, 60 + i % 12)); history.add(off(i * 0.5 + 0.4, 60 + i % 12)); }
    const auto phrase = history.capture(41.0);
    check(phrase.leftOut > 0 && phrase.leftOut + static_cast<int>(phrase.notes.size()) == 80, "a phrase too long for a clip keeps its latest part");
    check(!phrase.notes.empty() && approx(phrase.notes.front().startStep, 0), "the kept part starts at its first note");
    bool inside = true;
    for (const auto& n : phrase.notes) inside = inside && PianoRollPattern::toTicks(n.startStep) < maxTicks;
    check(inside && phrase.lengthTicks == maxTicks, "every kept note starts inside the longest clip");
    check(phrase.notes.back().pitch == 60 + 79 % 12, "the last note played is kept");

    CaptureHistory full;
    for (int i = 0; i < 5000; ++i) full.add(i % 2 == 0 ? on(i * 0.1, 60) : off(i * 0.1, 60));
    check(full.size() == CaptureHistory::capacity, "the history keeps a fixed number of events");
    const auto recent = full.capture(500.0);
    check(!recent.empty() && recent.leftOut > 0, "the history still captures after the oldest events go");
}

static void ring() {
    CaptureRing r;
    int pushed = 0;
    while (r.push(on(pushed, 60))) ++pushed;
    check(pushed == static_cast<int>(CaptureRing::capacity) - 1 && r.droppedCount() == 1, "a full ring drops and counts");
    Event e; int popped = 0;
    while (r.pop(e)) ++popped;
    check(popped == pushed && e.seconds == pushed - 1, "the ring gives events back in order");
}

static void insertIntoClip() {
    CaptureHistory history;
    for (int i = 0; i < 9; ++i) { history.add(on(i * 1.0, 60)); history.add(off(i * 1.0 + 0.5, 60)); }   // 4.5 bars at 120
    const auto phrase = history.capture(10.0);
    check(phrase.lengthTicks == 5 * barTicks, "a phrase into a fifth bar is five bars long");
    PianoRollPattern pattern;   // 64 sixteenths
    pattern.add(72, 1.0, 1.0, 0.8f);
    const auto growth = loopGrowth(phrase);
    const auto result = insertNotes(pattern, phrase.notes, 0.0, &growth);
    check(result.added == 9 && pattern.count == 10 && pattern.loopSteps == 80, "the clip grows to hold the capture and keeps its notes");
    const auto text = describe(phrase, result);
    check(text.find("Captured 9 notes played at 120 BPM") == 0 && text.find("from 4 bars to 5 bars") != std::string::npos, "the status says what happened");

    PianoRollPattern fixed;
    const auto cut = insertNotes(fixed, phrase.notes, 0.0);
    check(cut.added == 8 && cut.skipped == 1 && fixed.loopSteps == 64, "without growth a capture keeps the clip's length");
    check(describe(Phrase{}, {}).find("nothing has been played") != std::string::npos, "an empty capture says why");
}

// The real callback: live notes reach the ring with the capture clock, previews and patterns do not, and nothing
// on the audio thread allocates.
static void engineCapture() {
    ToneEngine engine;
    engine.prepare(48000);
    std::array<float, 256> left{}, right{};
    const auto render = [&](int samples) {
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            samples -= count;
        }
    };
    render(4800);
    const auto device = engine.midi.beginSession();
    allocationCount = 0;
    engine.notes.push(60, 0.8f); render(256);
    engine.midi.push(device, 0x91, 67, 100); render(24000);   // half a second
    engine.notes.push(60, 0.0f); engine.midi.push(device, 0x81, 67, 0); render(256);
    engine.trackAuditions.push(72, 0.8f); render(256);         // a piano-roll key preview is not playing
    engine.trackAuditions.push(72, 0.0f); render(256);
    check(allocationCount == 0, "capturing live notes does not allocate on the audio thread");
    const double clock = engine.captureClockNow.load();
    check(approx(clock, (4800 + 256 + 24000 + 256 + 512) / 48000.0, 1.0e-9), "the capture clock counts rendered audio");

    CaptureHistory history;
    history.drain(engine.capturedNotes);
    const auto phrase = history.capture(clock);
    check(phrase.notes.size() == 2, "typing and MIDI notes are captured; previews are not");
    if (phrase.notes.size() == 2) {
        const double expected = (24000 + 256) / 48000.0 * 8.0;   // sixteenths at 120 BPM
        check(phrase.notes[0].pitch == 60 && approx(phrase.notes[0].lengthSteps, expected, 0.1), "a typed note's length follows the audio clock");
        check(phrase.notes[1].pitch == 67 && approx(phrase.notes[1].startStep, 256 / 48000.0 * 8.0, 0.01)
              && approx(phrase.notes[1].velocity, 100.0f / 127.0f, 0.01), "a MIDI note keeps its place and velocity");
    }
    check(phrase.stillHeld == 0, "released notes are not reported as held");

    // A device change releases held MIDI notes, and the capture sees those releases.
    render(144000);   // three seconds of silence: a new phrase
    engine.midi.push(device, 0x90, 50, 90); render(4800);
    engine.midi.beginSession(); render(4800);
    history.drain(engine.capturedNotes);
    const auto released = history.capture(engine.captureClockNow.load() + 10.0);
    check(released.notes.size() == 1 && released.stillHeld == 0, "notes a device change releases end in the capture too");
}

int main() {
    pairingAndTiming();
    phrases();
    pairingRepairs();
    longPhrases();
    ring();
    insertIntoClip();
    engineCapture();
    if (failures != 0) { std::cerr << failures << " Capture MIDI checks failed\n"; return 1; }
    std::cout << "Capture MIDI: pairing, phrases, timing, limits, clip insert and the engine path passed\n";
}
