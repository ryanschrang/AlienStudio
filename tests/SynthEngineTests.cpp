#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

static std::size_t allocations = 0;
void* operator new(std::size_t size) {
    ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static double energy(alien::SynthEngine& synth, int count) {
    double result = 0;
    for (int i = 0; i < count; ++i) {
        const auto sample = synth.nextSample();
        require(std::isfinite(sample) && std::abs(sample) <= 1.00001, "invalid output");
        result += sample * sample;
    }
    return result;
}
int main() {
    using namespace alien;
    StepClock clock;
    int stepCount = 0;
    for (int sample = 0; sample < 480000; ++sample) {
        const auto tick = clock.next(48000, 137);
        if (tick.step >= 0) {
            const double ideal = stepCount * 48000.0 * 60 / (137 * 4);
            require(sample >= ideal - 1e-6 && sample < ideal + 1.000001, "sample accurate step timing");
            require(tick.step == stepCount % 16 && tick.noteOff, "step order and retrigger off");
            ++stepCount;
        }
    }
    require(stepCount == 92, "fractional tempo has no drift");
    clock.reset();
    require(clock.next(48000, 120).step == 0, "transport reset starts step zero");
    for (int i = 1; i < 3000; ++i) require(!clock.next(48000, 120).noteOff, "gate not early");
    require(clock.next(48000, 120).noteOff, "half-step gate note-off");
    NoteQueue queue;
    NoteQueue::Event event{};
    require(!queue.pop(event), "empty queue");
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 255; ++i) queue.push(i, .5f);
        queue.push(42, 0);
        require(queue.panic.exchange(false), "queue overflow requests panic");
        for (int i = 0; i < 255; ++i) {
            require(queue.pop(event) && event.note == i && event.velocity == .5f, "queue ordering/wrap");
        }
        require(!queue.pop(event), "queue drained");
    }
    // Exact ADSR boundaries at a deliberately low sample rate.
    Envelope env;
    const Patch p{Waveform::sine, .01, .02, .5, .01, 100};
    env.start(p, 1000);
    for (int i = 0; i < 10; ++i) env.next();
    require(std::abs(env.next() - .975) < 1e-9, "attack/decay boundary");
    for (int i = 0; i < 20; ++i) env.next();
    require(env.state() == Envelope::Stage::sustain, "sustain stage");
    require(std::abs(env.next() - .5) < 1e-9, "sustain level");
    env.stop();
    require(std::abs(env.next() - .45) < 1e-9, "release starts at current level");
    for (int i = 0; i < 11; ++i) env.next();
    require(env.state() == Envelope::Stage::idle, "release finishes");
    env.start(p, 1000); env.next(); env.stop();
    require(env.next() < .1, "early note-off releases attack");
    for (int i = 0; i < 20; ++i) env.next();
    require(env.state() == Envelope::Stage::idle, "early note-off finishes");

    SynthEngine synth;
    synth.prepare(48000); synth.setPatch(presets[0]);
    require(energy(synth, 512) == 0, "initial silence");
    synth.noteOn(-1, 1); synth.noteOn(128, 1);
    synth.noteOn(60, std::numeric_limits<float>::quiet_NaN());
    require(synth.activeVoices() == 0, "invalid input ignored");
    for (int note = 48; note < 64; ++note) synth.noteOn(note, 1);
    require(synth.activeVoices() == 16, "polyphony");
    require(energy(synth, 2000) > 0, "chord renders");
    synth.noteOn(80, 1);
    require(!synth.isNoteActive(48) && synth.isNoteActive(80), "oldest held voice stolen");
    synth.noteOff(55); synth.noteOn(81, 1);
    require(!synth.isNoteActive(55) && synth.isNoteActive(49), "released voice stolen first");
    synth.noteOff(48); // A late note-off for a stolen note must not stop its replacement.
    require(synth.isNoteActive(80), "late stolen note-off");
    synth.allSoundOff();
    require(synth.activeVoices() == 0 && energy(synth, 512) == 0, "panic silence");
    for (const auto& patch : presets) {
        synth.setPatch(patch); synth.noteOn(69, .5f);
        const double quiet = energy(synth, 4096);
        synth.allSoundOff(); synth.noteOn(69, 1);
        const double loud = energy(synth, 4096);
        require(quiet > 0 && std::abs(loud / quiet - 4) < 1e-5, "velocity scales amplitude");
        // Wait out this preset's release time (plus margin) before checking silence.
        synth.noteOn(69, 0); energy(synth, static_cast<int>(std::ceil((patch.release + 0.1) * 48000.0)));
        require(synth.activeVoices() == 0 && energy(synth, 256) == 0, "zero velocity note-off");
    }
    synth.setPatch(presets[2]); synth.noteOn(60, 1); energy(synth, 24000);
    require(synth.activeVoices() == 0, "pluck decays without note-off");
    synth.setPatch(presets[0]); synth.noteOn(60, 1); synth.noteOn(60, .5f);
    synth.noteOff(60); energy(synth, 48000);
    require(synth.activeVoices() == 0, "repeated notes release together");
    synth.setPatch(presets[0]);
    synth.noteOn(60, 1, 0); synth.noteOn(60, 1, 1);
    synth.noteOff(60, 1); energy(synth, 48000);
    require(synth.activeVoices() == 1, "sequencer note-off leaves keyboard voice held");
    synth.allSoundOff();
    // The sine preset should render A4 at 440 Hz at both common device rates.
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        synth.prepare(rate);
        synth.setPatch({Waveform::sine, 0, 0, 1, .01, 20000});
        synth.noteOn(69, 1);
        energy(synth, 1000);
        int crossings = 0;
        float previous = synth.nextSample();
        for (int i = 0; i < static_cast<int>(rate); ++i) {
            const float value = synth.nextSample();
            if (previous <= 0 && value > 0) ++crossings;
            previous = value;
        }
        require(std::abs(crossings - 440) <= 1, "oscillator pitch/sample-rate handling");
    }
    synth.noteOn(60, 1); synth.prepare(96000);
    require(synth.activeVoices() == 0, "device restart clears notes");
    const auto before = allocations;
    for (int n = 0; n < 100; ++n) {
        synth.setPatch(presets[n % 3]); synth.noteOn(n % 128, .7f);
        energy(synth, 512); synth.noteOff(n % 128);
    }
    synth.allSoundOff();
    require(allocations == before, "DSP allocated memory");
    std::cout << "Synth lifecycle, ADSR, allocation, presets and rendered DSP passed\n";
}
