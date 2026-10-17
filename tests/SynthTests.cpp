#include "audio/SequencerEngine.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<int> allocations{0};
}

void* operator new(std::size_t size) {
    if (countAllocations.load()) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(float value, float expected, float tolerance, const char* message) {
    require(std::abs(value - expected) <= tolerance, message);
}

void envelopeTest() {
    alien::Envelope e;
    e.start(1000, {0.010f, 0.020f, 0.5f, 0.010f});
    for (int i = 0; i < 10; ++i) near(e.next(), (i + 1) / 10.0f, 0.0001f, "attack ramp");
    for (int i = 0; i < 20; ++i) near(e.next(), 1.0f - (i + 1) * 0.025f, 0.0001f, "decay ramp");
    for (int i = 0; i < 20; ++i) near(e.next(), 0.5f, 0.0001f, "sustain level");
    e.noteOff();
    for (int i = 0; i < 10; ++i) near(e.next(), 0.5f - (i + 1) * 0.05f, 0.0001f, "release ramp");
    near(e.next(), 0, 0, "release silence");
    require(!e.active(), "release must reclaim voice");

    e.start(1000, {0.010f, 0.02f, 0.5f, 0.01f});
    for (int i = 0; i < 5; ++i) e.next();
    e.noteOff();
    near(e.next(), 0.45f, 0.0001f, "early note-off releases from current level");
    e.noteOff();
    near(e.next(), 0.40f, 0.0001f, "duplicate note-off must not restart release");

    e.start(1000, {0, 0, 0.25f, 0});
    near(e.next(), 0.25f, 0, "instant attack/decay");
    e.noteOff();
    near(e.next(), 0, 0, "instant release");
    require(!e.active(), "instant release inactive");
}

void voiceTest() {
    alien::SynthEngine synth;
    synth.prepare(48000);
    for (int note = 40; note < 56; ++note) synth.noteOn(1, note, 1);
    require(synth.activeVoiceCount() == 16, "16 simultaneous voices");
    synth.noteOn(1, 56, 1);
    require(synth.activeVoiceCount() == 16 && !synth.isNoteActive(1, 40), "steal oldest held voice");
    synth.noteOff(1, 50);
    synth.noteOn(1, 57, 1);
    require(!synth.isNoteActive(1, 50) && synth.isNoteActive(1, 41), "prefer released voice over held");
    synth.noteOn(1, 57, 0.5f);
    require(synth.activeVoiceCount() == 16, "retrigger reuses same key");
    synth.allNotesOff();
    for (int i = 0; i < 12000; ++i) synth.nextSample();
    require(synth.activeVoiceCount() == 0, "all voices finish release");
    near(synth.nextSample(), 0, 0, "silence after release");
    synth.noteOn(1, 60, 1);
    synth.noteOn(2, 60, 1);
    require(synth.activeVoiceCount() == 2, "channels isolate identical pitches");
    synth.noteOff(1, 60);
    for (int i = 0; i < 12000; ++i) synth.nextSample();
    require(!synth.isNoteActive(1, 60) && synth.isNoteActive(2, 60), "channel-specific note-off");
    synth.prepare(96000);
    require(synth.activeVoiceCount() == 0, "device preparation clears voices");
}

void midiTest() {
    alien::SynthEngine synth;
    synth.handleMidi(0x91, 60, 100);
    require(synth.isNoteActive(2, 60), "MIDI note-on channel parsing");
    synth.handleMidi(0x91, 60, 0);
    for (int i = 0; i < 10000; ++i) synth.nextSample();
    require(!synth.isNoteActive(2, 60), "zero velocity is note-off");
    synth.handleMidi(0x90, 64, 100);
    synth.handleMidi(0x80, 64, 64);
    for (int i = 0; i < 10000; ++i) synth.nextSample();
    require(synth.activeVoiceCount() == 0, "explicit MIDI note-off");
    synth.handleMidi(0x90, 60, 100);
    synth.handleMidi(0xb0, 120, 0);
    require(synth.activeVoiceCount() == 0, "all sound off is immediate");
    synth.handleMidi(0x90, 61, 100);
    synth.handleMidi(0xb0, 123, 0);
    for (int i = 0; i < 10000; ++i) synth.nextSample();
    require(synth.activeVoiceCount() == 0, "all notes off releases");
    synth.handleMidi(0x90, 255, 100);
    synth.handleMidi(0x90, 60, 255);
    synth.handleMidi(0xf8, 0, 0);
    synth.noteOn(0, 60, 1);
    synth.noteOn(1, 128, 1);
    synth.noteOn(1, 60, std::numeric_limits<float>::quiet_NaN());
    require(synth.activeVoiceCount() == 0, "malformed input must not start voices");
}

void waveformAndVelocityTest() {
    for (auto waveform : {alien::Waveform::sine, alien::Waveform::saw, alien::Waveform::square}) {
        alien::Oscillator oscillator;
        oscillator.start(48000, 69, waveform);
        double energy = 0;
        int crossings = 0;
        float previous = 0;
        for (int i = 0; i < 48000; ++i) {
            const auto sample = oscillator.next();
            require(std::isfinite(sample) && std::abs(sample) <= 1.001f, "bounded oscillator output");
            if (sample > 0 && previous <= 0) ++crossings;
            previous = sample;
            energy += sample * sample;
        }
        require(energy > 1000, "waveform has signal");
        require(crossings >= 439 && crossings <= 441, "A4 oscillator frequency");
    }
    for (auto preset : {alien::PresetId::lead, alien::PresetId::bass, alien::PresetId::pluck}) {
        alien::SynthEngine loud, quiet;
        loud.setPreset(preset); quiet.setPreset(preset);
        loud.noteOn(1, 60, 1); quiet.noteOn(1, 60, 0.25f);
        double loudEnergy = 0, quietEnergy = 0;
        for (int i = 0; i < 20000; ++i) {
            const float l = loud.nextSample(), q = quiet.nextSample();
            require(std::isfinite(l) && std::abs(l) <= 1, "finite bounded preset output");
            loudEnergy += l*l; quietEnergy += q*q;
        }
        require(loudEnergy > 0.1 && loudEnergy > quietEnergy * 8, "velocity changes audible energy");
    }
}

void queueTest() {
    alien::ControlQueue queue;
    alien::MidiEvent event;
    require(!queue.pop(event), "empty queue");
    for (std::size_t i = 0; i < alien::ControlQueue::capacity - 1; ++i)
        require(queue.push({0x90, static_cast<std::uint8_t>(i), 100}), "queue capacity");
    require(!queue.push({0x80, 60, 0}) && queue.takeOverflow(), "overflow detected");
    require(!queue.takeOverflow(), "overflow acknowledged once");
    for (std::size_t i = 0; i < alien::ControlQueue::capacity - 1; ++i) {
        require(queue.pop(event), "queued event available");
        require(event.data1 == i, "FIFO order");
    }
    require(queue.push({0x80, 60, 0}), "ring wraps");
    queue.discardPending();
    require(!queue.pop(event), "panic discards pending events");

    // Exercise the acquire/release publication across actual producer/consumer threads.
    std::atomic<bool> valid{true};
    std::thread producer([&] {
        for (int i = 0; i < 100000; ++i)
            while (!queue.push({0x90, static_cast<std::uint8_t>(i % 128), 100})) std::this_thread::yield();
    });
    for (int i = 0; i < 100000; ++i) {
        while (!queue.pop(event)) std::this_thread::yield();
        if (event.status != 0x90 || event.data1 != i % 128 || event.data2 != 100) valid = false;
    }
    producer.join();
    require(valid, "concurrent FIFO publication");
}

void transportTest() {
    alien::SequencerEngine engine;
    engine.prepare(44100);
    engine.steps[0] = true;
    engine.playing = true;
    std::vector<float> buffer(6000);
    float* outputs[]{buffer.data(), nullptr};
    engine.process(outputs, 2, 5513);
    require(engine.currentStep == 0, "first step lasts 5512.5 samples at 120 BPM");
    engine.process(outputs, 2, 1);
    require(engine.currentStep == 1, "fractional step boundary");
    engine.playing = false;
    engine.process(outputs, 2, 100);
    require(engine.currentStep == -1 && engine.activeVoiceCount() == 0, "transport stop clears voices and step");
    for (int i = 0; i < 100; ++i) near(buffer[i], 0, 0, "stop silence");
    engine.playing = true;
    engine.process(outputs, 2, 1);
    require(engine.currentStep == 0, "restart begins at step one");
    engine.playing = false;
    engine.process(outputs, 2, 1);
    engine.controls.push({0x91, 60, 100});
    engine.process(outputs, 2, 100);
    require(engine.activeVoiceCount() == 1, "audition while transport stopped");
    for (int i = 0; i < 200; ++i) engine.controls.push({0x91, 60, 100});
    engine.process(outputs, 2, 100);
    require(engine.activeVoiceCount() == 0, "queue overflow panics instead of sticking notes");
    engine.controls.push({0x91, 60, 100});
    engine.process(outputs, 2, 100);
    engine.requestPanic();
    engine.process(outputs, 2, 100);
    require(engine.activeVoiceCount() == 0, "user panic");
    engine.controls.push({0x91, 60, 100});
    engine.deviceStopped();
    engine.prepare(48000);
    engine.process(outputs, 2, 100);
    require(engine.activeVoiceCount() == 0, "device change drops stale notes");
}

void blockAndAllocationTest() {
    alien::SequencerEngine single, blocks;
    single.prepare(48000); blocks.prepare(48000);
    for (int i = 0; i < 16; ++i) single.steps[i] = blocks.steps[i] = i % 2 == 0;
    single.bpm = blocks.bpm = 137;
    single.playing = blocks.playing = true;
    std::vector<float> expected(100000), actual(100000);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        float* output[]{expected.data() + i};
        single.process(output, 1, 1);
    }
    allocations = 0;
    countAllocations = true;
    for (std::size_t i = 0; i < actual.size(); i += 257) {
        float* output[]{actual.data() + i};
        blocks.process(output, 1, static_cast<int>(std::min<std::size_t>(257, actual.size() - i)));
    }
    // Exercise allocation/stealing/release paths as part of the callback too.
    blocks.playing = false;
    float scratch[64]{};
    float* output[]{scratch};
    blocks.process(output, 1, 64);
    for (int i = 0; i < 32; ++i) blocks.controls.push({0x91, static_cast<std::uint8_t>(40 + i), 100});
    blocks.process(output, 1, 64);
    blocks.controls.push({0xb1, 123, 0});
    blocks.process(output, 1, 64);
    countAllocations = false;
    require(allocations == 0, "audio processing must not allocate");
    require(expected == actual, "timing/audio must be identical across block sizes");
}
}

int main() {
    const struct { const char* name; void (*run)(); } tests[]{
        {"ADSR stages and early release", envelopeTest},
        {"Voice lifecycle and stealing", voiceTest},
        {"MIDI note and panic parsing", midiTest},
        {"Oscillators, presets and velocity", waveformAndVelocityTest},
        {"Bounded SPSC event delivery", queueTest},
        {"Sequencer timing, stop and device reset", transportTest},
        {"Block invariance and allocation guard", blockAndAllocationTest}
    };
    int failures = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS: " << test.name << '\n'; }
        catch (const std::exception& error) {
            countAllocations = false;
            ++failures;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
