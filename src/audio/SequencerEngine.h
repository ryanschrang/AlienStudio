#pragma once

#include "ControlQueue.h"
#include "../synth/SynthEngine.h"
#include <array>
#include <atomic>

namespace alien {

// GUI writes only atomics or the SPSC queue. All timing and synth state belongs
// to the audio thread. Kept independent of JUCE to exercise the actual callback
// processing path in small, headless tests.
class SequencerEngine {
public:
    static_assert(std::atomic<int>::is_always_lock_free);
    std::array<std::atomic<bool>, 16> steps{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, selectedPreset{0};
    ControlQueue controls;

    void requestPanic() noexcept { panic.store(true, std::memory_order_release); }

    // Called while device callbacks are stopped.
    void prepare(double rate) noexcept {
        sampleRate = std::isfinite(rate) && rate >= 8000.0 ? rate : 44100.0;
        synth.prepare(sampleRate);
        resetTransport();
        controls.discardPending();
        controls.takeOverflow();
        wasPlaying = false;
    }
    void deviceStopped() noexcept {
        synth.reset();
        controls.discardPending();
        resetTransport();
        wasPlaying = false;
    }

    void process(float* const* outputs, int channels, int samples) noexcept {
        const bool running = playing.load(std::memory_order_relaxed);
        const bool mustReset = panic.exchange(false, std::memory_order_acq_rel)
            | controls.takeOverflow() | (wasPlaying && !running);
        if (mustReset) {
            synth.reset();
            controls.discardPending();
            resetTransport();
        }
        if (running && !wasPlaying) resetTransport();
        wasPlaying = running;
        synth.setPreset(static_cast<PresetId>(std::clamp(selectedPreset.load(std::memory_order_relaxed), 0, 2)));

        MidiEvent event;
        for (std::size_t i = 0; i < ControlQueue::capacity && controls.pop(event); ++i)
            synth.handleMidi(event.status, event.data1, event.data2);

        const auto stepLength = sampleRate * 60.0 / (std::clamp(bpm.load(std::memory_order_relaxed), 40, 240) * 4.0);
        for (int i = 0; i < samples; ++i) {
            if (running) {
                if (gateRemaining > 0.0 && --gateRemaining <= 0.0) synth.noteOff(1, 57);
                if (samplesToStep <= 0.0) {
                    synth.noteOff(1, 57);
                    currentStep.store(step, std::memory_order_relaxed);
                    if (steps[static_cast<std::size_t>(step)].load(std::memory_order_relaxed)) {
                        synth.noteOn(1, 57, 0.8f);
                        gateRemaining = stepLength * 0.5;
                    }
                    step = (step + 1) % 16;
                    samplesToStep += stepLength;
                }
                samplesToStep -= 1.0;
            }
            const float sample = synth.nextSample();
            for (int channel = 0; channel < channels; ++channel)
                if (outputs[channel] != nullptr) outputs[channel][i] = sample;
        }
    }

    std::size_t activeVoiceCount() const noexcept { return synth.activeVoiceCount(); }

private:
    void resetTransport() noexcept {
        samplesToStep = gateRemaining = 0.0;
        step = 0;
        currentStep.store(-1, std::memory_order_relaxed);
    }
    SynthEngine synth;
    std::atomic<bool> panic{false};
    double sampleRate{44100.0}, samplesToStep{0.0}, gateRemaining{0.0};
    int step{0};
    bool wasPlaying{false};
};

} // namespace alien
