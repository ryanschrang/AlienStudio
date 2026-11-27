#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

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
    synth.prepare(48000); synth.setPatch({Waveform::sine,0,0,1,.01,20000});
    synth.noteOn(69,1,9); synth.setPitchWheel(9,16383);
    energy(synth,1000);
    int bentCrossings=0; float bentPrevious=synth.nextSample();
    for(int i=0;i<48000;++i) {
        const float value=synth.nextSample();
        if(bentPrevious<=0&&value>0) ++bentCrossings;
        bentPrevious=value;
    }
    require(std::abs(bentCrossings-494)<=2,"14-bit pitch wheel bends the active voice by two semitones");
    synth.setModulation(9,127);
    require(energy(synth,4800)>0,"modulation keeps the active voice finite and audible");
    synth.allSoundOff();
    {
        // T1-04 D25: a voice skips the bend and vibrato maths while its channel's wheel is centred and
        // modulation is zero. Centring the wheel explicitly must sound exactly like never touching it,
        // modulation alone must still move the pitch, and a wheel returned to centre plays in tune.
        const auto render = [](SynthEngine& s, int n) { std::vector<float> out(static_cast<std::size_t>(n));
            for (auto& x : out) x = s.nextSample(); return out; };
        const auto crossings = [](const std::vector<float>& v) { int n = 0;
            for (std::size_t i = 1; i < v.size(); ++i) if (v[i - 1] <= 0 && v[i] > 0) ++n; return n; };
        const Patch tone{Waveform::sine, 0, 0, 1, .01, 20000};
        SynthEngine plain, centred, modulated;
        for (auto* s : {&plain, &centred, &modulated}) { s->prepare(48000); s->setPatch(tone); s->noteOn(69, 1, 3); }
        centred.setPitchWheel(3, 8192); centred.setModulation(3, 0);
        modulated.setModulation(3, 127);
        const auto a = render(plain, 48000), b = render(centred, 48000), c = render(modulated, 48000);
        require(a == b, "a centred wheel and zero modulation sound exactly like untouched controls");
        require(a != c, "modulation alone still moves the pitch");
        SynthEngine returned; returned.prepare(48000); returned.setPatch(tone); returned.noteOn(69, 1, 3);
        returned.setPitchWheel(3, 0); render(returned, 4800); returned.setPitchWheel(3, 8192);
        require(std::abs(crossings(render(returned, 48000)) - 440) <= 1, "a wheel returned to centre plays in tune");
    }
    {
        // T1-04 D26: the synth only visits voices up to the last one a note can be sounding in. A note held above
        // finished ones keeps sounding and is still counted; a new note reuses the freed slot below it; a reset
        // starts from nothing; and the rack's one-note synths end their note exactly as before.
        const Patch quick{Waveform::sine, 0, 0, 1, .001, 20000};
        SynthEngine s; s.prepare(48000); s.setPatch(quick);
        s.noteOn(60, 1); s.noteOn(64, 1); s.noteOn(67, 1);
        s.noteOff(60); s.noteOff(64); energy(s, 480);
        require(s.activeVoices() == 1 && s.isNoteActive(67) && !s.isNoteActive(60), "a note held above finished ones is counted");
        require(energy(s, 480) > 0, "a note held above finished ones keeps sounding");
        s.noteOn(72, 1);
        require(s.activeVoices() == 2 && s.isNoteActive(72) && s.isNoteActive(67), "a new note takes a freed slot below");
        s.noteOff(67); s.noteOff(72); energy(s, 480);
        require(s.activeVoices() == 0 && energy(s, 64) == 0, "every finished note leaves silence");
        for (int n = 0; n < 16; ++n) s.noteOn(40 + n, 1);
        s.allSoundOff(); s.noteOn(50, 1);
        require(s.activeVoices() == 1 && energy(s, 64) > 0, "after a reset the first note sounds alone");
        s.allSoundOff(5); s.allSoundOff(0);
        require(s.activeVoices() == 0 && energy(s, 64) == 0, "a per-channel reset silences its voices");
    }
    {
        // T1-04 D30: a track whose synth has nothing sounding skips nextSample() and only moves its vibrato
        // clock on. A note played after that must sound exactly as if every silent sample had been rendered.
        const Patch quick{Waveform::sine, 0, 0, 1, .001, 20000};
        SynthEngine rendered, skipped;
        for (auto* s : {&rendered, &skipped}) { s->prepare(48000); s->setPatch(quick); s->setModulation(0, 127); }
        require(rendered.silent() && skipped.silent(), "a synth with no note is silent");
        rendered.noteOn(60, 1); skipped.noteOn(60, 1); rendered.noteOff(60); skipped.noteOff(60);
        for (int n = 0; n < 480; ++n) { rendered.nextSample(); skipped.nextSample(); }
        require(rendered.silent() && skipped.silent(), "a finished note leaves the synth silent");
        for (int n = 0; n < 3001; ++n) { require(rendered.nextSample() == 0.0f, "a silent synth renders zeros"); skipped.skipSilentSample(); }
        rendered.noteOn(64, 1); skipped.noteOn(64, 1);
        require(!rendered.silent() && !skipped.silent(), "a new note ends the silence");
        bool same = true; double sum = 0;
        for (int n = 0; n < 4800; ++n) { const float a = rendered.nextSample(), b = skipped.nextSample(); same = same && a == b; sum += a * a; }
        require(same && sum > 0, "a note after skipped silent samples hears the same vibrato as after rendered ones");
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
