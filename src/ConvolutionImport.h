#pragma once

#include "StockEffects.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace alien {
struct ConvolutionImport {
    double rate{};
    std::vector<float> left, right;
    juce::String error;
};

// Worker only. Check metadata before allocating or reading the response.
inline ConvolutionImport decodeConvolutionWav(const juce::File& file) {
    if (!file.hasFileExtension("wav")) return {.error = "Choose a WAV impulse response."};
    juce::WavAudioFormat format;
    auto stream = file.createInputStream();
    if (!stream) return {.error = "Could not open the WAV impulse response."};
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));
    if (!reader) return {.error = "Could not open the WAV impulse response."};
    const double rate = reader->sampleRate;
    const auto frames = reader->lengthInSamples;
    if (!std::isfinite(rate) || rate < 8000 || rate > dsp::ConvolutionReverb::maxSourceRate ||
        (reader->numChannels != 1 && reader->numChannels != 2) ||
        frames <= 0 || frames > rate * dsp::ConvolutionReverb::maxCustomResponseSeconds)
        return {.error = "Use a mono or stereo WAV, 8–192 kHz and at most one second long."};
    ConvolutionImport result;
    result.rate = rate;
    result.left.resize(static_cast<std::size_t>(frames));
    if (reader->numChannels == 2) result.right.resize(result.left.size());
    float* channels[]{result.left.data(), result.right.data()};
    if (!reader->read(channels, static_cast<int>(reader->numChannels), 0, static_cast<int>(frames)) ||
        !dsp::ConvolutionReverb::validCustomResponse(rate, result.left, result.right))
        return {.error = "Could not read valid impulse coefficients from the WAV."};
    return result;
}
} // namespace alien
