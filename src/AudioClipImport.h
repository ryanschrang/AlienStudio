#pragma once

#include "SessionAudioClip.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

namespace alien {

struct AudioClipImportResult {
    std::shared_ptr<SessionAudioClip> clip;
    juce::String error;
};

// Called only from an import worker. File reads, decoder setup, allocation,
// sample validation and waveform analysis must never run in the audio callback.
inline AudioClipImportResult decodeSessionAudioClip(const juce::File& file) {
    const auto extension = file.getFileExtension().toLowerCase();
    if (extension != ".wav" && extension != ".mp3")
        return {{}, "Choose a WAV or MP3 audio file."};
    if (!file.existsAsFile()) return {{}, "The selected audio file no longer exists."};

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader)
        return {{}, extension == ".mp3"
            ? "This build could not decode the MP3 file. On Windows, check that the system MP3 codec is available."
            : "The WAV file is damaged or uses an unsupported encoding."};

    const auto channels = static_cast<int>(reader->numChannels);
    const auto frames64 = reader->lengthInSamples;
    if (!std::isfinite(reader->sampleRate) || reader->sampleRate < 8000.0 || reader->sampleRate > 192000.0)
        return {{}, "Audio sample rates must be between 8 kHz and 192 kHz."};
    if (channels < 1 || channels > 2)
        return {{}, "Only mono and stereo audio files are supported."};
    if (frames64 <= 0 || static_cast<std::uint64_t>(frames64)
        > SessionAudioClip::maxDecodedBytes / (static_cast<std::size_t>(channels) * sizeof(float)))
        return {{}, "The decoded audio clip exceeds the 128 MiB session limit or is empty."};

    const auto frames = static_cast<std::size_t>(frames64);
    auto clip = std::make_shared<SessionAudioClip>();
    clip->sampleRate = reader->sampleRate;
    clip->channels = channels;
    clip->frames = frames;
    clip->left.resize(frames);
    if (channels == 2) clip->right.resize(frames);
    float* destinations[]{clip->left.data(), channels == 2 ? clip->right.data() : nullptr};
    if (!reader->read(destinations, channels, 0, static_cast<int>(frames)))
        return {{}, "The selected audio file could not be decoded completely."};
    for (std::size_t i = 0; i < frames; ++i) {
        if (!std::isfinite(clip->left[i])) clip->left[i] = 0.0f;
        if (channels == 2 && !std::isfinite(clip->right[i])) clip->right[i] = 0.0f;
    }
    clip->buildWaveform();
    if (!clip->valid()) return {{}, "The decoded audio data is invalid."};
    return {std::move(clip), {}};
}

} // namespace alien
