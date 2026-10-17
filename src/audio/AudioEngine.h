#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "SequencerEngine.h"

class AudioEngine final : public juce::AudioIODeviceCallback {
public:
    alien::SequencerEngine sequencer;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sequencer.prepare(device->getCurrentSampleRate());
    }
    void audioDeviceStopped() override { sequencer.deviceStopped(); }
    void audioDeviceIOCallbackWithContext(const float* const*, int,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        sequencer.process(outputs, channels, samples);
    }
};
