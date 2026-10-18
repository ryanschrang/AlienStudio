#include "ToneEngine.h"
#include "PianoRollProcessing.h"
#include "PianoRollClipboard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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

static alien::PianoRollPattern heldPattern(int pitch = 60) {
    alien::PianoRollPattern pattern;
    require(pattern.add(pitch, 0, pattern.loopSteps, 0.8f) != 0, "test pattern creation");
    return pattern;
}

static void playbackStopAndRestartTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    require(offline.render(256) == 0, "prepared engine is silent before transport or live input");
    require(engine.pianoRollNotes.publish(heldPattern()), "publish piano-roll clip");
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
    engine.pianoRollNotes.publish(pattern);
    engine.patternMode.store(1);
    engine.playing.store(true);
    require(offline.render(2000) > 0, "held clip starts before editing");
    require(pattern.erase(pattern.notes[0].id), "delete active clip note");
    engine.pianoRollNotes.publish(pattern);
    require(offline.settledEnergy() == 0 && engine.pianoRollPosition.load() > 2000,
        "deleting active note releases its voice while transport continues");

    // Added notes covering the current position start on the next callback.
    pattern.add(64, 0, 64);
    pattern.add(67, 0, 64);
    engine.pianoRollNotes.publish(pattern);
    require(offline.render(1024) > 0, "live snapshot insertion starts current-position chord");
    pattern.clear();
    engine.pianoRollNotes.publish(pattern);
    require(offline.settledEnergy() == 0, "clearing a playing chord releases every clip voice");
    engine.playing.store(false);
    require(offline.render(256) == 0, "stop after live edits leaves no stuck notes");
}

static void sessionClipLaunchTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.requestedPianoRollClip.store(0);
    require(engine.pianoRollLaunches.publish(heldPattern(60)), "queue first session clip");
    require(offline.render(256) > 0 && engine.activePianoRollClip.load() == 0,
            "first queued clip starts with the transport");

    engine.requestedPianoRollClip.store(1);
    require(engine.pianoRollLaunches.publish(heldPattern(67)), "queue replacement session clip");
    offline.render(383744);
    require(engine.activePianoRollClip.load() == 0,
            "replacement clip waits through the active four-bar loop");
    require(offline.render(1) > 0 && engine.activePianoRollClip.load() == 1,
            "replacement clip activates on the exact loop boundary");

    engine.requestedPianoRollClip.store(-1);
    require(engine.pianoRollLaunches.publish({}), "queue session clip stop");
    offline.render(383999);
    require(engine.activePianoRollClip.load() == 1, "quantized stop waits through the active loop");
    offline.render(1);
    require(engine.activePianoRollClip.load() == -1 && offline.settledEnergy() == 0,
            "quantized stop releases the clip without stale audio");
}

static void panicPresetAndSourceTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    engine.pianoRollNotes.publish(heldPattern());
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
    engine.pianoRollNotes.publish(empty);
    engine.notes.push(80, 0.9f);
    engine.notes.panic.store(true);
    require(offline.render(512) == 0, "panic discards pending keyboard note-ons instead of starting a stuck note");
}

static void liveKeyboardIsolationTests() {
    OfflineEngine offline;
    auto& engine = offline.engine;
    auto pattern = heldPattern(60);
    engine.pianoRollNotes.publish(pattern);
    engine.patternMode.store(1);
    engine.playing.store(true);
    engine.notes.push(60, 0.8f); // Same pitch as the clip, separate synth source.
    require(offline.render(2048) > 0, "clip and live keyboard render together");
    pattern.clear();
    engine.pianoRollNotes.publish(pattern);
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
    require(engine.pianoRollNotes.publish(pattern), "publish substep strum snapshot");
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
    require(engine.pianoRollNotes.publish(pattern), "publish chopped active clip");
    require(offline.render(24000) > 0.001, "rapid chopped notes render finite audio");
    alien::RandomizeSettings randomize;
    randomize.pitchSemitones = 12;
    randomize.timingSteps = 0.125;
    randomize.seed = 287;
    require(alien::processRandomize(pattern, chopped.selection, randomize).changed(),
        "randomize a sounding chopped clip");
    engine.pianoRollNotes.publish(pattern);
    offline.render(1024);
    alien::QuantizeSettings quantize;
    quantize.gridSteps = 0.5;
    quantize.swing = 0.3;
    alien::processQuantize(pattern, chopped.selection, quantize);
    require(engine.pianoRollNotes.publish(pattern), "publish swung quantized snapshot");
    offline.render(12000);
    pattern.clear();
    engine.pianoRollNotes.publish(pattern);
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
    engine.pianoRollNotes.publish(pattern);
    require(offline.render(2048) > 0, "source chord sounds before clipboard edits");
    require(clipboard.paste(pattern, 0.25).changed(), "paste overlapping chord with fresh IDs");
    engine.pianoRollNotes.publish(pattern);
    require(offline.render(2048) > 0, "pasted overlap keeps chord sounding");
    engine.pianoRollNotes.publish(original); // Undo Paste, keeping source notes.
    require(offline.render(2048) > 0, "undoing pasted overlap preserves original held pitches");
    pattern = original;
    require(alien::duplicatePianoRollSelection(pattern, selected).changed(), "duplicate after original chord end");
    engine.pianoRollNotes.publish(pattern);
    require(offline.render(48000) > 0, "duplicated chord plays across original end boundary");
    engine.pianoRollNotes.publish(original); // Undo while only the duplicate holds.
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
    auto clip = heldPattern(60); e.pianoRollNotes.publish(clip); offline.render(512);
    e.midi.push(replacement, 0x90, 60, 100); offline.render(512);
    e.midi.push(replacement, 0x80, 60, 0);
    require(offline.settledEnergy() > 0.001, "MIDI release preserves equal-pitch piano-roll playback");
    clip.clear(); e.pianoRollNotes.publish(clip);
    require(offline.settledEnergy() == 0, "clearing final clip owner releases shared pitch");
    e.midi.push(replacement, 0x90, 64, 100); require(offline.render(512) > 0, "MIDI plays over an empty running clip");
    e.patternMode.store(0);
    require(offline.render(512) == 0 && !e.midi.isNoteOn(64), "source switch clears MIDI state coherently");
}

int main() {
    playbackStopAndRestartTests();
    activeEditingTests();
    sessionClipLaunchTests();
    panicPresetAndSourceTests();
    liveKeyboardIsolationTests();
    processedPatternTests();
    clipboardPlaybackTests();
    hardwareMidiTests();
    std::cout << "Offline audio callback playback, edits, panic, transport, source switching and keyboard isolation passed\n";
}
