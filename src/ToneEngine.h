#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include "PianoRollSequence.h"
#include <array>
#include <atomic>
#include <cmath>

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    std::array<std::atomic<bool>, 16> steps{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, preset{0};
    NoteQueue notes;
    alien::PianoRollMailbox pianoRollNotes;
    std::atomic<int> patternMode{0}, pianoRollPosition{-1};

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        prepare(device->getCurrentSampleRate());
    }
    // Shared by device startup and offline callback regression tests.
    void prepare(double rate) {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        roll.stop([this](int n, float v, bool on) { sendPattern(n, v, on); });
        synth.prepare(sampleRate);
        discardPendingNotes();
        resetTransport();
    }
    void audioDeviceStopped() override {
        roll.stop([this](int n, float v, bool on) { sendPattern(n, v, on); });
        synth.allSoundOff(); discardPendingNotes(); resetTransport();
    }
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        const bool playNow = playing.load();
        const int modeNow = patternMode.load();
        auto send = [this](int n, float v, bool on) { sendPattern(n, v, on); };
        if ((wasPlaying && !playNow) || wasMode != modeNow) {
            roll.stop(send); synth.allSoundOff(); resetTransport();
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
        if (panic) { roll.stop(send); synth.allSoundOff(); }
        roll.beginBlock(pianoRollNotes, playNow && modeNow == 1, sampleRate, bpm.load(), send);
        for (int i = 0; i < samples; ++i) {
            if (playNow && modeNow == 0) {
                const auto tick = clock.next(sampleRate, bpm.load());
                if (tick.noteOff) synth.noteOff(57, 1);
                if (tick.step >= 0) {
                    currentStep.store(tick.step);
                    if (steps[static_cast<std::size_t>(tick.step)].load()) synth.noteOn(57, 0.8f, 1);
                }
            }
            if (playNow && modeNow == 1) roll.nextSample(send);
            const float value = synth.nextSample();
            for (int c = 0; c < channels; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = value;
        }
        pianoRollPosition.store(static_cast<int>(roll.positionStep() * 1000.0));
    }
private:
    void sendPattern(int note, float velocity, bool on) noexcept {
        if (on) synth.noteOn(note, velocity, 2); else synth.noteOff(note, 2);
    }
    void discardPendingNotes() noexcept {
        NoteQueue::Event event{};
        for (int n = 0; n < 256 && notes.pop(event); ++n) {}
        notes.panic.store(false);
    }
    void resetTransport() noexcept {
        clock.reset(); wasPlaying = false; currentStep.store(-1); pianoRollPosition.store(-1000);
    }
    alien::SynthEngine synth;
    alien::StepClock clock;
    alien::PianoRollScheduler roll;
    double sampleRate{44100.0};
    bool wasPlaying{};
    int wasMode{};
};

