#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include "PianoRollSequence.h"
#include "MidiInputEngine.h"
#include "MidiControllerMappings.h"
#include <array>
#include <atomic>
#include <cmath>

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    static constexpr std::size_t sessionTracks = 4;
    std::array<std::atomic<bool>, 16> steps{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, preset{0};
    NoteQueue notes;
    alien::MidiInputPipe midi;
    alien::MidiControllerMappings controllerMappings;
    alien::MidiControllerActionQueue controllerActions;
    std::array<alien::PianoRollMailbox, sessionTracks> pianoRollNotes;
    std::array<alien::PianoRollMailbox, sessionTracks> pianoRollLaunches;
    std::array<std::atomic<int>, sessionTracks> sessionTrackPositions{-1000, -1000, -1000, -1000};
    std::atomic<int> patternMode{0}, pianoRollPosition{-1};
    std::array<std::atomic<int>, sessionTracks> requestedPianoRollClip{-1, -1, -1, -1};
    std::array<std::atomic<int>, sessionTracks> activePianoRollClip{-1, -1, -1, -1};
    std::atomic<bool> synchronizedSceneLaunch{false};
    std::atomic<int> launchQuantizationSteps{16}; // One bar by default (16 sixteenth-note steps).

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        prepare(device->getCurrentSampleRate());
    }
    // Shared by device startup and offline callback regression tests.
    void prepare(double rate) {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        synth.prepare(sampleRate);
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
        discardPendingNotes();
        resetTransport();
    }
    void audioDeviceStopped() override {
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        synth.allSoundOff(); discardPendingNotes(); resetTransport();
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
    }
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        const bool playNow = playing.load();
        const int modeNow = patternMode.load();
        if (synchronizedSceneLaunch.exchange(false)) {
            synchronizedScenePending = true;
            synchronizedSceneTracksQueued = 0;
            synchronizedSceneQuantizationSteps = launchQuantizationSteps.load();
        }
        if ((wasPlaying && !playNow) || wasMode != modeNow) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            synth.allSoundOff(); resetTransport();
            midi.requestReset();
        }
        wasPlaying = playNow; wasMode = modeNow;
        synth.setPatch(alien::presets[static_cast<std::size_t>(juce::jlimit(0, 2, preset.load()))]);
        NoteQueue::Event event{};
        const bool panic = notes.panic.exchange(false);
        // Bound work per callback even if the producer keeps publishing.
        for (int n = 0; n < 256 && notes.pop(event); ++n) {
            if (!panic) {
                if (event.velocity > 0) synth.noteOn(event.note, event.velocity);
                else synth.noteOff(event.note);
            }
        }
        if (panic) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            synth.allSoundOff(); midi.requestReset();
        }
        midi.process([this](int n, float v, int ch) { sendMidi(n, v, ch); },
            [this](int channel, int controller, int value) {
                if (channel < 0) { controllerMappings.resetEdges(); return; }
                const int action = controllerMappings.processCC(channel, controller, value);
                if (action >= 0) controllerActions.push(action);
            });
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            alien::PianoRollPattern requestedPattern;
            if (pianoRollLaunches[track].consume(requestedPattern)) {
                const int quantization = synchronizedScenePending
                    ? synchronizedSceneQuantizationSteps : launchQuantizationSteps.load();
                rolls[track].queuePattern(requestedPattern, requestedPianoRollClip[track].load(),
                    synchronizedScenePending, quantization);
                if (synchronizedScenePending)
                    synchronizedSceneTracksQueued |= (1u << track);
            }
            rolls[track].beginBlock(pianoRollNotes[track], playNow && modeNow == 1, sampleRate, bpm.load(),
                [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        }
        for (int i = 0; i < samples; ++i) {
            if (playNow && modeNow == 0) {
                const auto tick = clock.next(sampleRate, bpm.load());
                if (tick.noteOff) synth.noteOff(57, 1);
                if (tick.step >= 0) {
                    currentStep.store(tick.step);
                    if (steps[static_cast<std::size_t>(tick.step)].load()) synth.noteOn(57, 0.8f, 1);
                }
            }
            if (playNow && modeNow == 1) {
                constexpr unsigned allSessionTracks = (1u << sessionTracks) - 1u;
                if (synchronizedScenePending && synchronizedSceneTracksQueued == allSessionTracks
                    && rolls[0].launchBoundaryDue(synchronizedSceneQuantizationSteps)) {
                    for (std::size_t track = 0; track < sessionTracks; ++track)
                        rolls[track].launchPendingNow([this, track](int n, float v, bool on) {
                            sendPattern(track, n, v, on);
                        });
                    synchronizedScenePending = false;
                    synchronizedSceneTracksQueued = 0;
                }
                for (std::size_t track = 0; track < sessionTracks; ++track)
                    rolls[track].nextSample([this, track](int n, float v, bool on) {
                        sendPattern(track, n, v, on);
                    });
            }
            const float value = synth.nextSample();
            for (int c = 0; c < channels; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = value;
        }
        pianoRollPosition.store(static_cast<int>(rolls[0].positionStep() * 1000.0));
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            sessionTrackPositions[track].store(static_cast<int>(rolls[track].positionStep() * 1000.0));
            activePianoRollClip[track].store(rolls[track].activeClipIndex());
        }
    }
private:
    void sendMidi(int note, float velocity, int channel) noexcept {
        const int source = channel + 8; // Reserve source channels 2..5 for four Session tracks.
        if (note < 0) synth.allSoundOff(source);
        else if (velocity > 0) synth.noteOn(note, velocity, source);
        else synth.noteOff(note, source);
    }
    void sendPattern(std::size_t track, int note, float velocity, bool on) noexcept {
        const int source = static_cast<int>(track) + 2;
        if (on) synth.noteOn(note, velocity, source); else synth.noteOff(note, source);
    }
    void discardPendingNotes() noexcept {
        NoteQueue::Event event{};
        for (int n = 0; n < 256 && notes.pop(event); ++n) {}
        notes.panic.store(false);
    }
    void resetTransport() noexcept {
        clock.reset(); wasPlaying = false; currentStep.store(-1); pianoRollPosition.store(-1000);
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            activePianoRollClip[track].store(-1);
            sessionTrackPositions[track].store(-1000);
        }
    }
    alien::SynthEngine synth;
    alien::StepClock clock;
    std::array<alien::PianoRollScheduler, sessionTracks> rolls{};
    double sampleRate{44100.0};
    bool wasPlaying{};
    int wasMode{};
    bool synchronizedScenePending{};
    unsigned synchronizedSceneTracksQueued{};
    int synchronizedSceneQuantizationSteps{16};
};

