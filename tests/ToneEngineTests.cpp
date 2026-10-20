#include "ToneEngine.h"
#include "PianoRollProcessing.h"
#include "PianoRollClipboard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#include <new>

// Count C++ heap allocation on the calling thread, including actual audio callbacks.
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

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// Render the real device callback offline. A null output between two valid
// outputs also checks the callback's device-channel handling. No audio device,
// GUI, MIDI hardware, or timing sleeps are involved.
struct OfflineEngine {
    ToneEngine engine;
    std::array<float, 256> left{}, right{};
    OfflineEngine() { engine.prepare(48000); }
    double render(int samples) {
        double energy = 0;
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
            left.fill(std::numeric_limits<float>::quiet_NaN());
            right.fill(std::numeric_limits<float>::quiet_NaN());
            float* outputs[]{left.data(), nullptr, right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 3, count, {});
            for (int i = 0; i < count; ++i) {
                require(std::isfinite(left[static_cast<std::size_t>(i)])
                    && left[static_cast<std::size_t>(i)] == right[static_cast<std::size_t>(i)]
                    && std::abs(left[static_cast<std::size_t>(i)]) <= 1.00001f,
                    "device callback must write finite matching samples to every valid output");
                energy += left[static_cast<std::size_t>(i)] * left[static_cast<std::size_t>(i)];
            }
            samples -= count;
        }
        return energy;
    }
    double settledEnergy() {
        render(12000); // Longer than every existing synth preset's release.
        return render(512);
    }
};

static std::vector<float> captureSamples(ToneEngine& engine, int sampleCount) {
    std::vector<float> captured(static_cast<std::size_t>(sampleCount));
    std::array<float, 256> left{}, right{};
    int offset = 0;
    while (offset < sampleCount) {
        const int count = std::min(sampleCount - offset, static_cast<int>(left.size()));
        float* outputs[]{left.data(), right.data()};
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
        std::copy_n(left.begin(), count, captured.begin() + offset);
        offset += count;
    }
    return captured;
}

static std::vector<float> renderSequencerNote(int pitch, int velocity, ToneEngine& engine) {
    engine.prepare(48000);
    for (auto& step : engine.steps) step.store(false);
    engine.steps[0].store(true);
    engine.stepPitches[0].store(pitch);
    engine.stepVelocities[0].store(velocity);
    engine.patternMode.store(0);
    engine.playing.store(true);
    return captureSamples(engine, 2800);
}

static void perStepPitchAndVelocityTests() {
    ToneEngine pitchEngine;
    const auto pitchSamples = renderSequencerNote(69, 127, pitchEngine);
    int zeroCrossings = 0;
    constexpr int firstSample = 800;
    constexpr int lastSample = 2700;
    for (int i = firstSample + 1; i < lastSample; ++i)
        if (pitchSamples[static_cast<std::size_t>(i - 1)] <= 0.0f
            && pitchSamples[static_cast<std::size_t>(i)] > 0.0f) ++zeroCrossings;
    const double estimatedHz = static_cast<double>(zeroCrossings) * 48000.0
        / static_cast<double>(lastSample - firstSample);
    require(pitchEngine.currentStep.load() == 0 && estimatedHz > 400.0 && estimatedHz < 480.0,
        "selected step pitch controls the rendered oscillator frequency");

    ToneEngine loudEngine, quietEngine;
    const auto loud = renderSequencerNote(69, 127, loudEngine);
    const auto quiet = renderSequencerNote(69, 64, quietEngine);
    double loudEnergy = 0.0, quietEnergy = 0.0;
    for (int i = firstSample; i < lastSample; ++i) {
        const auto index = static_cast<std::size_t>(i);
        loudEnergy += static_cast<double>(loud[index]) * loud[index];
        quietEnergy += static_cast<double>(quiet[index]) * quiet[index];
    }
    const double rmsRatio = std::sqrt(quietEnergy / loudEnergy);
    require(rmsRatio > 0.45 && rmsRatio < 0.56,
        "selected step velocity scales the rendered note level");
    const auto gatedRelease = captureSamples(pitchEngine, 10000);
    require(std::all_of(gatedRelease.end() - 512, gatedRelease.end(),
        [](float value) { return std::abs(value) < 1.0e-10f; }),
        "step gate releases its selected pitch without leaving a stuck voice");
}

static alien::PianoRollPattern heldPattern(int pitch = 60) {
    alien::PianoRollPattern pattern;
    require(pattern.add(pitch, 0, pattern.loopSteps, 0.8f) != 0, "test pattern creation");
    return pattern;
}

static void playbackStopAndRestartTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    require(offline.render(256) == 0, "prepared engine is silent before transport or live input");
    require(engine.pianoRollNotes[0].publish(heldPattern()), "publish piano-roll clip");
    engine.patternMode.store(1);
    engine.playing.store(true);
    require(offline.render(9600) > 0.001, "piano-roll callback renders audible synth samples");
    require(engine.pianoRollPosition.load() >= 1599 && engine.pianoRollPosition.load() <= 1601,
        "piano-roll playhead follows rendered samples across callback boundaries");
    require(engine.currentStep.load() == -1, "piano-roll source leaves legacy sequencer inactive");
    engine.playing.store(false);
    require(offline.render(512) == 0 && engine.pianoRollPosition.load() < 0,
        "transport stop immediately silences clip and clears playhead");
    require(offline.render(1024) == 0, "stopped transport cannot emit stale clip notes");
    engine.playing.store(true);
    require(offline.render(256) > 0 && engine.pianoRollPosition.load() >= 41
        && engine.pianoRollPosition.load() <= 43, "restart begins sounding clip at step zero");
    engine.playing.store(false);
    engine.audioDeviceStopped();
    require(offline.render(256) == 0, "device stop clears rendered voices and transport clock");
    engine.prepare(48000);
    engine.playing.store(true);
    require(offline.render(256) > 0 && engine.pianoRollPosition.load() <= 43,
        "device prepare retains clip data and restarts scheduler without stale held-note state");
}

static void activeEditingTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    auto pattern = heldPattern();
    engine.pianoRollNotes[0].publish(pattern);
    engine.patternMode.store(1);
    engine.playing.store(true);
    require(offline.render(2000) > 0, "held clip starts before editing");
    require(pattern.erase(pattern.notes[0].id), "delete active clip note");
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.settledEnergy() == 0 && engine.pianoRollPosition.load() > 2000,
        "deleting active note releases its voice while transport continues");

    // Added notes covering the current position start on the next callback.
    pattern.add(64, 0, 64);
    pattern.add(67, 0, 64);
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.render(1024) > 0, "live snapshot insertion starts current-position chord");
    pattern.clear();
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.settledEnergy() == 0, "clearing a playing chord releases every clip voice");
    engine.playing.store(false);
    require(offline.render(256) == 0, "stop after live edits leaves no stuck notes");
}

static void sessionClipLaunchTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    engine.patternMode.store(1);
    engine.playing.store(true);
    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
        engine.requestedPianoRollClip[track].store(0);
        const int pitch = track < 2 ? 60 : 60 + static_cast<int>(track);
        auto firstClip = heldPattern(pitch);
        if (track == 0) {
            firstClip.loopSteps = 32;
            firstClip.notes[0].lengthSteps = 32;
        }
        require(engine.pianoRollLaunches[track].publish(firstClip),
                "queue first scene track clip");
    }
    require(offline.render(256) > 0, "first queued scene starts with the transport");
    for (const auto& active : engine.activePianoRollClip)
        require(active.load() == 0, "first scene starts all tracks together");
    alien::PianoRollPattern emptyTrack;
    emptyTrack.loopSteps = 32;
    require(engine.pianoRollNotes[0].publish(emptyTrack), "clear one track while matching pitch plays on another");
    require(offline.render(256) > 0.001, "editing one track preserves another track's same-pitch voice");

    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
        engine.requestedPianoRollClip[track].store(1);
        require(engine.pianoRollLaunches[track].publish(heldPattern(67 + static_cast<int>(track))),
                "queue replacement scene track clip");
    }
    engine.synchronizedSceneLaunch.store(true);
    offline.render(95488);
    for (const auto& active : engine.activePianoRollClip)
        require(active.load() == 0, "scene waits for the next bar despite different track loop lengths");
    require(offline.render(1) > 0, "replacement scene sounds on the bar boundary");
    for (const auto& active : engine.activePianoRollClip)
        require(active.load() == 1, "scene activates every track together on the bar boundary");

    engine.synchronizedSceneLaunch.store(true);
    for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
        engine.requestedPianoRollClip[track].store(-1);
        require(engine.pianoRollLaunches[track].publish({}), "queue session scene stop");
    }
    offline.render(95999);
    for (const auto& active : engine.activePianoRollClip)
        require(active.load() == 1, "quantized scene stop waits through the active loop");
    offline.render(1);
    for (const auto& active : engine.activePianoRollClip)
        require(active.load() == -1, "quantized scene stop clears each track");
    require(offline.settledEnergy() == 0, "quantized scene stop releases all tracks without stale audio");
}

static void sessionAudioClipTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    alien::SessionAudioClip audio;
    audio.sampleRate = 48000;
    audio.channels = 1;
    audio.frames = 1024;
    audio.left.assign(audio.frames, 0.5f);
    audio.buildWaveform();
    require(audio.valid(), "decoded audio asset satisfies the playback contract");

    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.launchQuantizationSteps.store(4);
    engine.requestedPianoRollClip[0].store(2);
    alien::SessionClipLaunch launch;
    launch.audio = &audio;
    launch.clipIndex = 2;
    require(engine.pianoRollLaunches[0].publish(launch), "queue a beat-quantized Session audio clip");
    require(offline.render(256) > 0.1 && engine.activePianoRollClip[0].load() == 2,
        "Session audio clip starts at the running transport boundary and mixes into the output");
    require(offline.render(768) > 0.1 && engine.activePianoRollClip[0].load() == -1,
        "one-shot audio naturally ends and clears the active clip status");

    // Selecting a MIDI clip on the same track switches audio off at its
    // quantized boundary while the synthesizer continues to render notes.
    engine.launchQuantizationSteps.store(1);
    engine.requestedPianoRollClip[0].store(3);
    require(engine.pianoRollLaunches[0].publish(heldPattern(60)), "queue MIDI over the ended audio clip");
    require(offline.render(12000) > 0.01, "switching clip type preserves synth playback");
    engine.playing.store(false);
    require(offline.render(512) == 0, "transport stop clears audio and MIDI clip output");
}

static void launchQuantizationTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.requestedPianoRollClip[0].store(0);
    require(engine.pianoRollLaunches[0].publish(heldPattern(60)), "queue initial quantization-test clip");
    require(offline.render(256) > 0 && engine.activePianoRollClip[0].load() == 0,
        "initial clip starts at transport start");

    engine.launchQuantizationSteps.store(4);
    engine.requestedPianoRollClip[0].store(1);
    require(engine.pianoRollLaunches[0].publish(heldPattern(64)), "queue beat-quantized clip");
    offline.render(23744);
    require(engine.activePianoRollClip[0].load() == 0, "beat-quantized launch waits until the next beat");
    offline.render(1);
    require(engine.activePianoRollClip[0].load() == 1, "beat-quantized launch switches at the beat boundary");

    engine.launchQuantizationSteps.store(16);
    engine.requestedPianoRollClip[0].store(2);
    require(engine.pianoRollLaunches[0].publish(heldPattern(67)), "queue bar-quantized clip");
    offline.render(71999);
    require(engine.activePianoRollClip[0].load() == 1, "bar-quantized launch waits for the next bar");
    offline.render(1);
    require(engine.activePianoRollClip[0].load() == 2, "bar-quantized launch switches at the bar boundary");
    engine.playing.store(false);
    require(offline.render(256) == 0, "quantized clip changes still stop without held notes");
}

static void panicPresetAndSourceTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    engine.pianoRollNotes[0].publish(heldPattern());
    engine.patternMode.store(1);
    engine.playing.store(true);
    require(offline.render(15000) > 0 && engine.pianoRollPosition.load() >= 2499,
        "clip progresses before panic");
    engine.notes.panic.store(true);
    require(offline.render(256) > 0 && engine.pianoRollPosition.load() <= 43,
        "panic restarts active clip coherently after silencing the synth");
    engine.preset.store(1); // Match the GUI's preset-change panic request.
    engine.notes.panic.store(true);
    require(offline.render(256) > 0 && engine.pianoRollPosition.load() <= 43,
        "preset panic does not leave scheduler references muting the new patch");

    engine.steps[0].store(true);
    engine.patternMode.store(0);
    require(offline.render(256) > 0 && engine.currentStep.load() == 0
        && engine.pianoRollPosition.load() < 0, "switching to legacy sequencer restarts its clock and produces audio");
    require(offline.settledEnergy() == 0, "legacy step gate still releases when following steps are disabled");
    engine.patternMode.store(1);
    require(offline.render(256) > 0 && engine.currentStep.load() == -1
        && engine.pianoRollPosition.load() <= 43, "switching back starts clip at zero without stale step state");
    engine.playing.store(false);
    require(offline.render(256) == 0, "stopping after source switches silences both sources");

    alien::PianoRollPattern empty;
    engine.pianoRollNotes[0].publish(empty);
    engine.notes.push(80, 0.9f);
    engine.notes.panic.store(true);
    require(offline.render(512) == 0, "panic discards pending keyboard note-ons instead of starting a stuck note");
}

static void liveKeyboardIsolationTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    auto pattern = heldPattern(60);
    engine.pianoRollNotes[0].publish(pattern);
    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.notes.push(60, 0.8f); // Same pitch as the clip, separate synth source.
    require(offline.render(2048) > 0, "clip and live keyboard render together");
    pattern.clear();
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.settledEnergy() > 0.001, "deleting clip note preserves same-pitch live keyboard voice");
    engine.notes.push(60, 0.0f);
    require(offline.settledEnergy() == 0, "live keyboard note-off releases surviving voice without stuck clip note");

    // The keyboard remains usable with transport stopped and an empty clip.
    engine.playing.store(false);
    offline.render(256);
    engine.notes.push(72, 0.7f);
    require(offline.render(2048) > 0, "live keyboard still works independently of transport");
    engine.notes.push(72, 0.0f);
    require(offline.settledEnergy() == 0, "stopped-transport keyboard note-off remains functional");
}

static void processedPatternTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    alien::PianoRollPattern pattern;
    alien::PianoRollSelection selection;
    selection.add(pattern.add(60, 0.5, 3));
    selection.add(pattern.add(64, 0.5, 3));
    selection.add(pattern.add(67, 0.5, 3));
    alien::StrumSettings strum;
    strum.delaySteps = 0.125;
    require(alien::processStrum(pattern, selection, strum).changed(), "strum chord for real callback");
    require(engine.pianoRollNotes[0].publish(pattern), "publish substep strum snapshot");
    engine.patternMode.store(1);
    engine.playing.store(true);
    require(offline.render(3000) == 0, "substep note must not sound before its exact half-step start");
    require(offline.render(2048) > 0.001, "processed fractional chord sounds through the real callback");

    // Replace sounding notes using the same snapshots published by the editor.
    // Removing the result must release every old and new pitch, including rapid
    // chopper fragments and randomized timing/pitch changes.
    alien::ChopSettings chop;
    chop.divisionSteps = 0.25;
    chop.gate = 0.5;
    const auto chopped = alien::processChop(pattern, selection, chop);
    require(chopped.changed() && pattern.count == 36, "chop creates bounded fractional fragments");
    require(engine.pianoRollNotes[0].publish(pattern), "publish chopped active clip");
    require(offline.render(24000) > 0.001, "rapid chopped notes render finite audio");
    alien::RandomizeSettings randomize;
    randomize.pitchSemitones = 12;
    randomize.timingSteps = 0.125;
    randomize.seed = 287;
    require(alien::processRandomize(pattern, chopped.selection, randomize).changed(),
        "randomize a sounding chopped clip");
    engine.pianoRollNotes[0].publish(pattern);
    offline.render(1024);
    alien::QuantizeSettings quantize;
    quantize.gridSteps = 0.5;
    quantize.swing = 0.3;
    alien::processQuantize(pattern, chopped.selection, quantize);
    require(engine.pianoRollNotes[0].publish(pattern), "publish swung quantized snapshot");
    offline.render(12000);
    pattern.clear();
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.settledEnergy() == 0, "processing then clearing must leave no held old or new pitch");
    engine.playing.store(false);
    require(offline.render(256) == 0, "transport stop after processing remains silent");
}

static void clipboardPlaybackTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    alien::PianoRollPattern pattern;
    alien::PianoRollSelection selected;
    selected.add(pattern.add(60, 0, 8));
    selected.add(pattern.add(64, 0, 8));
    const auto original = pattern;
    alien::PianoRollClipboard clipboard;
    require(clipboard.copy(pattern, selected).status == alien::ClipboardStatus::copied, "copy sounding chord");
    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.render(2048) > 0, "source chord sounds before clipboard edits");
    require(clipboard.paste(pattern, 0.25).changed(), "paste overlapping chord with fresh IDs");
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.render(2048) > 0, "pasted overlap keeps chord sounding");
    engine.pianoRollNotes[0].publish(original); // Undo Paste, keeping source notes.
    require(offline.render(2048) > 0, "undoing pasted overlap preserves original held pitches");
    pattern = original;
    require(alien::duplicatePianoRollSelection(pattern, selected).changed(), "duplicate after original chord end");
    engine.pianoRollNotes[0].publish(pattern);
    require(offline.render(48000) > 0, "duplicated chord plays across original end boundary");
    engine.pianoRollNotes[0].publish(original); // Undo while only the duplicate holds.
    require(offline.settledEnergy() == 0, "undoing sounding duplicate releases all copied pitches");
    engine.playing.store(false);
    require(offline.render(512) == 0, "Stop after clipboard undo leaves no stuck notes");
}

static void hardwareMidiTests() {
    OfflineEngine offline;
    auto& e = offline.engine;
    const auto device = e.midi.beginSession();
    e.midi.push(device, 0x90, 60, 127);
    require(offline.render(2048) > 0 && e.midi.isNoteOn(60), "external MIDI plays with stopped transport");
    e.midi.push(device, 0xb0, 64, 127); e.midi.push(device, 0x90, 60, 0);
    require(offline.settledEnergy() > 0.001, "MIDI CC64 holds a velocity-zero release");
    e.midi.push(device, 0xb0, 64, 0);
    require(offline.settledEnergy() == 0 && !e.midi.isNoteOn(60), "pedal-up clears MIDI sound and highlight");
    e.midi.push(device, 0x90, 60, 20); const auto soft = offline.render(2048);
    e.midi.push(device, 0x80, 60, 0); offline.settledEnergy();
    e.midi.push(device, 0x90, 60, 120); const auto loud = offline.render(2048);
    require(loud > soft * 10, "hardware velocity changes actual rendered synth energy");

    // Three owners hold the same pitch: live typing and two MIDI channels.
    e.notes.push(60, 0.8f); e.midi.push(device, 0x91, 60, 100); offline.render(256);
    e.midi.push(device, 0xb0, 120, 0);
    require(offline.settledEnergy() > 0.001 && e.midi.isNoteOn(60), "MIDI CC120 does not silence other channels or typing piano");
    e.midi.push(device, 0x81, 60, 0);
    require(offline.settledEnergy() > 0.001 && !e.midi.isNoteOn(60), "MIDI note-off preserves same-pitch typing voice");
    e.notes.push(60, 0);
    require(offline.settledEnergy() == 0, "final typing release leaves no copied MIDI voice");

    e.notes.push(72, 0.8f); e.midi.push(device, 0x90, 60, 100); offline.render(256);
    for (int n = 0; n < 1200; ++n) e.midi.push(device, 0x90, n % 128, 100);
    require(offline.settledEnergy() > 0.001 && !e.midi.isNoteOn(60), "MIDI overflow resets MIDI while preserving live GUI input");
    e.notes.push(72, 0); require(offline.settledEnergy() == 0, "overflow recovery leaves no stuck MIDI notes");
    e.midi.push(device, 0x90, 60, 100); offline.render(512);
    e.midi.beginSession(); // Device change/disconnect.
    require(offline.settledEnergy() == 0 && !e.midi.isNoteOn(60), "device session change releases MIDI voices");
    require(!e.midi.push(device, 0x90, 60, 127), "late old-device notes cannot restart sound");
    const auto replacement = e.midi.beginSession();
    e.midi.push(replacement, 0x90, 60, 100); require(offline.render(512) > 0, "new device works without app restart");
    e.preset.store(2); e.notes.panic.store(true);
    require(offline.render(512) == 0 && !e.midi.isNoteOn(60), "preset panic clears MIDI gates and pedal state");
    e.midi.push(replacement, 0x90, 67, 100); require(offline.render(512) > 0, "MIDI remains enabled after preset panic");
    e.audioDeviceStopped();
    require(offline.render(512) == 0 && !e.midi.isNoteOn(67), "audio-device stop clears MIDI voices and highlights");
    e.midi.push(replacement, 0x90, 67, 100); e.prepare(48000);
    require(offline.render(512) == 0, "audio restart discards pre-reset queued note-ons");
    e.midi.push(replacement, 0x90, 67, 100); require(offline.render(512) > 0, "fresh MIDI plays after audio restart");
    e.midi.requestReset(); require(offline.settledEnergy() == 0, "manual/blur reset releases final MIDI voice");
    e.preset.store(0); e.patternMode.store(1); e.playing.store(true);
    auto clip = heldPattern(60); e.pianoRollNotes[0].publish(clip); offline.render(512);
    e.midi.push(replacement, 0x90, 60, 100); offline.render(512);
    e.midi.push(replacement, 0x80, 60, 0);
    require(offline.settledEnergy() > 0.001, "MIDI release preserves equal-pitch piano-roll playback");
    clip.clear(); e.pianoRollNotes[0].publish(clip);
    require(offline.settledEnergy() == 0, "clearing final clip owner releases shared pitch");
    e.midi.push(replacement, 0x90, 64, 100); require(offline.render(512) > 0, "MIDI plays over an empty running clip");
    e.patternMode.store(0);
    require(offline.render(512) == 0 && !e.midi.isNoteOn(64), "source switch clears MIDI state coherently");
}

static void independentTrackMixerTests() {
    ToneEngine e;
    e.prepare(48000);
    e.patternMode.store(1); e.playing.store(true);
    e.tracks[0].preset.store(0); e.tracks[1].preset.store(1);
    e.tracks[0].pan.store(-1); e.tracks[1].pan.store(1);
    e.pianoRollNotes[0].publish(heldPattern(60));
    e.pianoRollNotes[1].publish(heldPattern(60));
    std::array<float, 256> l{}, r{};
    auto render = [&](int blocks) {
        std::array<double, 3> energy{};
        for (int b = 0; b < blocks; ++b) {
            float* out[]{l.data(), r.data()};
            const auto allocationsBefore = allocationCount;
            e.audioDeviceIOCallbackWithContext(nullptr, 0, out, 2, 256, {});
            require(allocationCount == allocationsBefore, "independent mixer callback must not allocate");
            energy = {};
            for (std::size_t i = 0; i < l.size(); ++i) {
                require(std::isfinite(l[i]) && std::isfinite(r[i]), "mixer samples finite");
                energy[0] += l[i]*l[i]; energy[1] += r[i]*r[i];
                energy[2] += (l[i]-r[i])*(l[i]-r[i]);
            }
        }
        return energy;
    };
    auto energy = render(60);
    require(energy[0] > 0.001 && energy[1] > 0.001 && energy[2] > 0.001,
        "same-pitch tracks render independent preset waveforms to opposite channels");
    require(e.tracks[0].peak.load() > 0 && e.tracks[1].peak.load() > 0,
        "meters publish each track's output peak");
    e.tracks[0].mute.store(true);
    energy = render(60);
    require(energy[0] == 0 && energy[1] > 0.001, "muting left track preserves right track");
    e.tracks[0].mute.store(false); e.tracks[0].solo.store(true);
    energy = render(60);
    require(energy[0] > 0.001 && energy[1] == 0, "solo isolates track after gain ramp");
    e.tracks[1].solo.store(true);
    energy = render(60);
    require(energy[0] > 0.001 && energy[1] > 0.001, "multiple solos combine");
    e.tracks[0].gain.store(0);
    energy = render(60);
    require(energy[0] == 0 && energy[1] > 0.001, "zero gain silences only its track");
    e.tracks[0].gain.store(1);
    alien::PianoRollPattern empty;
    e.pianoRollNotes[0].publish(empty);
    energy = render(60);
    require(energy[0] == 0 && energy[1] > 0.001, "deleting equal-pitch note cannot release another track");
    alien::SessionAudioClip audio;
    audio.sampleRate = 48000; audio.channels = 1; audio.frames = 48000;
    audio.left.assign(audio.frames, 0.5f);
    audio.buildWaveform();
    alien::SessionClipLaunch launch;
    launch.clipIndex = 1; launch.audio = &audio;
    e.launchQuantizationSteps.store(4);
    e.pianoRollLaunches[0].publish(launch);
    e.tracks[0].gain.store(0.5f);
    render(100); // Cross a beat boundary and settle the ramp.
    require(std::abs(l.back() - 0.5f * 0.5f * ToneEngine::audioClipLevel) < 1.0e-5f,
        "imported audio receives the same track gain and stereo balance");
    e.tracks[0].mute.store(true);
    energy = render(60);
    require(energy[0] == 0 && energy[1] > 0.001,
        "audio-track mute preserves the independent MIDI track");
    e.playing.store(false);
    energy = render(1);
    require(energy[0] == 0 && energy[1] == 0, "stop clears every independent synth");
    e.audioDeviceStopped(); e.prepare(44100);
    energy = render(1);
    require(energy[0] == 0 && energy[1] == 0, "device restart cannot resurrect notes");
}

static void drumTrackIntegrationTests() {
    alien::SessionAudioClip sample;
    sample.sampleRate = 48000; sample.channels = 1; sample.frames = 48000;
    sample.left.assign(sample.frames, 0.25f); sample.buildWaveform();
    OfflineEngine offline;
    auto& e = offline.engine;
    e.tracks[0].instrument.store(1);
    e.drumKits[0].samples[0].store(&sample);
    e.patternMode.store(1); e.playing.store(true);
    e.pianoRollNotes[0].publish(heldPattern(36));
    require(offline.render(512) > 0, "piano-roll drum note renders a sample");
    e.playing.store(false);
    require(offline.render(512) == 0, "transport stop cuts sample tails");
    e.drumAuditions[0].push(36, 1);
    require(offline.render(512) > 0, "pad audition works with transport stopped");
    e.notes.panic.store(true);
    require(offline.render(512) == 0, "panic cuts audition");
    e.liveDrumTrack.store(0); offline.render(256);
    const auto token = e.midi.beginSession(); offline.render(256);
    e.midi.push(token, 0x90, 36, 100);
    require(offline.render(512) > 0, "hardware MIDI route plays drum pad");
    e.midi.push(token, 0x80, 36, 0);
    require(offline.render(512) > 0, "one-shot drum is not truncated by note-off");
    e.midi.requestReset(); require(offline.render(512) == 0, "MIDI reset stops released drum tails");
    e.midi.push(token, 0x90, 36, 100); offline.render(256);
    e.liveDrumTrack.store(-1);
    require(offline.render(512) == 0, "changing MIDI route stops previous drum");
    e.midi.push(token, 0x90, 60, 100); require(offline.render(512) > 0, "live synth route still works");
    e.liveDrumTrack.store(0); require(offline.render(512) == 0, "changing from synth to kit clears old MIDI synth");
    e.drumAuditions[0].push(36, 1); offline.render(256);
    e.drumKits[0].samples[0].store(nullptr);
    require(offline.render(512) == 0, "clearing a pad stops retained sample playback");
}

static void arrangementCaptureRealtimeTests() {
    auto engine=std::make_unique<ToneEngine>();engine->prepare(48000);
    auto song=std::make_unique<alien::ArrangementPlayback>();song->timeline.setLengthBars(1);
    require(song->timeline.placeBars(0,0,0,1),"place the region");song->clips[0][0].pattern=heldPattern();
    engine->arrangementMailbox.publish(*song);engine->patternMode.store(2);engine->playing.store(true);
    alien::AudioCapture take(48000,2,1);engine->capture.store(&take);
    std::array<float,256> l{},r{},in{};in.fill(0.3f);float* outputs[]{l.data(),r.data()};const float* inputs[]{in.data(),in.data()};
    const auto before=allocationCount;
    for(int b=0;b<400;++b)engine->audioDeviceIOCallbackWithContext(inputs,2,outputs,2,256,{});
    require(before==allocationCount,"arrangement plus capture callback must not allocate");
    require(take.done.load() && take.frames.load()==48000 && !engine->playing.load(),"capture capacity and arrangement stop independently");
    engine->capture.store(nullptr);
}

// Two one-bar regions on track 0: scene 0 (note 60) then scene 1 (silent unless sceneOneNote >= 0).
static std::shared_ptr<alien::ArrangementPlayback> twoRegionSong(int lengthBars, int sceneOneNote) {
    auto song = std::make_shared<alien::ArrangementPlayback>();
    song->timeline.setLengthBars(lengthBars);
    require(song->timeline.placeBars(0, 0, 0, 1) && song->timeline.placeBars(0, 1, 1, 1), "test song regions");
    song->clips[0][0].pattern = heldPattern(60);
    if (sceneOneNote >= 0) song->clips[1][0].pattern = heldPattern(sceneOneNote);
    return song;
}

// Publishing a song snapshot (a note, mixer or tempo edit elsewhere in the
// project, or an edit to the timeline itself) must not restart the song: it keeps
// playing from where it is. Snapshot hand-over must not allocate.
static void arrangementRepublishTests() {
    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    engine->arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(twoRegionSong(4, -1)));
    engine->patternMode.store(2); engine->playing.store(true);
    std::array<float, 256> l{}, r{}; float* outputs[]{l.data(), r.data()};
    long frame = 0; std::array<double, 4> energy{};
    auto render = [&] {
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 256, {});
        for (int i = 0; i < 256; ++i) {
            const auto bar = static_cast<std::size_t>((frame + i) / 96000);
            if (bar < energy.size()) energy[bar] += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)];
        }
        frame += 256;
    };
    for (int guard = 0; engine->timelinePosition.load() < 1920 && guard < 1000; ++guard) render();
    require(engine->timelinePosition.load() >= 1920, "the timeline playhead advances while the song plays");
    require(energy[0] > 1 && engine->playing.load(), "first region sounds");

    // Same timeline, different clip contents: playback continues without a jump.
    engine->arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(twoRegionSong(4, 67)));
    const auto before = allocationCount;
    render();
    require(before == allocationCount, "taking a new song snapshot must not allocate");
    std::int64_t last = engine->timelinePosition.load();
    require(last > 1920, "republishing an unchanged timeline keeps the playhead moving");
    while (frame < 96000 * 2 - 256 && engine->playing.load()) {
        render();
        const auto now = engine->timelinePosition.load();
        require(now >= last, "playhead never jumps back after a same-layout republish");
        last = now;
    }
    require(engine->playing.load(), "song still playing after same-layout republish");
    while (frame < 96000 * 2 - 256) render();
    require(energy[1] > 1, "edited clip is heard when its region starts");

    // An edit to the timeline does not restart the song either: playing carries on from the same place.
    const auto beforeEdit = engine->timelinePosition.load();
    engine->arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(twoRegionSong(3, 67)));
    render();
    const auto afterEdit = engine->timelinePosition.load();
    require(engine->playing.load() && afterEdit > beforeEdit, "a changed timeline keeps playing from the same place");
}

// The final stage: master volume, the output meter and the soft clipper that
// replaced hard clipping.
static void masterStageTests() {
    require(alien::softLimit(0.0f) == 0.0f && alien::softLimit(0.8f) == 0.8f && alien::softLimit(-0.8f) == -0.8f,
        "soft limiter leaves the range up to -2 dBFS untouched");
    for (float x = -0.8f; x <= 0.8f; x += 0.01f)
        require(alien::softLimit(x) == x, "soft limiter is exact below the knee");
    float previous = alien::softLimit(0.8f);
    for (float x = 0.81f; x < 50.0f; x += 0.01f) {
        const float y = alien::softLimit(x);
        require(y >= previous && y <= 1.0f && y >= 0.8f, "soft limiter rises monotonically and never exceeds full scale");
        require(alien::softLimit(-x) == -y, "soft limiter is symmetric");
        previous = y;
    }
    require(alien::softLimit(1.0f) < 0.99f && alien::softLimit(1.0f) > 0.9f, "a full-scale peak is bent, not chopped");
    require(alien::softLimit(1.0e9f) <= 1.0f, "enormous input stays bounded");
    require(std::abs((alien::softLimit(0.801f) - alien::softLimit(0.8f)) / 0.001f - 1.0f) < 0.02f,
        "the knee is smooth: slope is 1 where the curve starts");

    alien::SessionAudioClip sample;
    sample.sampleRate = 48000; sample.channels = 1; sample.frames = 48000 * 4;
    sample.left.assign(sample.frames, 0.95f); sample.buildWaveform(); // loud enough that +12 dB master drives the mix past full scale
    OfflineEngine offline;
    auto& e = offline.engine;
    e.tracks[0].instrument.store(1);
    e.drumKits[0].samples[0].store(&sample);
    e.patternMode.store(1); e.playing.store(true);
    e.pianoRollNotes[0].publish(heldPattern(36));
    offline.render(4800);
    auto settledLevel = [&](float masterGain) {
        e.masterControls.gain.store(masterGain);
        offline.render(4800); // several ramp time constants
        offline.render(256);
        return offline.left.back();
    };
    const float unity = settledLevel(1.0f);
    require(unity > 0.001f && unity < 0.7f, "test sample plays below the knee at unity master volume");
    require(std::abs(settledLevel(2.0f) - 2.0f * unity) < 1.0e-4f, "master volume scales the mix linearly");
    require(std::abs(settledLevel(0.5f) - 0.5f * unity) < 1.0e-4f, "master volume can also lower the mix");
    require(settledLevel(0.0f) == 0.0f, "zero master volume silences the output exactly");
    require(std::abs(settledLevel(1.0f) - unity) < 1.0e-5f, "returning to unity restores the level");

    // Overdriven: output stays within full scale while the meter reports the real level.
    e.tracks[0].gain.store(2.0f);
    e.masterControls.gain.store(alien::maxMasterGain);
    offline.render(9600);
    require(e.masterControls.peak.load() > 1.0f, "master meter reads above 0 dBFS when the soft clipper is working");
    float worst = 0.0f;
    for (int block = 0; block < 20; ++block) { offline.render(256); for (float v : offline.left) worst = std::max(worst, std::abs(v)); }
    require(worst <= 1.0f && worst > 0.8f, "an overdriven mix is bent under full scale rather than chopped");
    // The steady signal is constant, so the output must be exactly the soft-limited value of the
    // level the meter reports (a hard clamp would give 1.0 here instead).
    const float metered = e.masterControls.peak.load();
    require(std::abs(worst - alien::softLimit(metered)) < 1.0e-4f && worst < 1.0f,
        "the output stage is the soft limiter applied to the metered level");

    // Master volume and the clip level never allocate in the callback.
    const auto before = allocationCount;
    e.masterControls.gain.store(0.7f);
    offline.render(2048);
    require(before == allocationCount, "the master stage must not allocate");
    require(alien::validMasterGain(0.0f) && alien::validMasterGain(alien::maxMasterGain)
        && !alien::validMasterGain(-0.1f) && !alien::validMasterGain(alien::maxMasterGain * 1.01f)
        && !alien::validMasterGain(std::numeric_limits<float>::quiet_NaN()), "master volume validity range");
}

int main() {
    masterStageTests();
    arrangementRepublishTests();
    arrangementCaptureRealtimeTests();
    drumTrackIntegrationTests();
    independentTrackMixerTests();
    perStepPitchAndVelocityTests();
    playbackStopAndRestartTests();
    activeEditingTests();
    sessionClipLaunchTests();
    sessionAudioClipTests();
    launchQuantizationTests();
    panicPresetAndSourceTests();
    liveKeyboardIsolationTests();
    processedPatternTests();
    clipboardPlaybackTests();
    hardwareMidiTests();
    std::cout << "Offline audio callback playback, edits, panic, transport, source switching and keyboard isolation passed\n";
}
