#pragma once

#include "DiskAudioFile.h"
#include "SessionAudioClip.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>
#include <memory>

namespace alien {

struct AudioClipImportResult {
    std::shared_ptr<SessionAudioClip> clip;
    juce::String error;
};

inline AudioClipImportResult inspectStreamedSessionAudio(const juce::File& file) {
    juce::String error;
    auto source = JuceDiskAudioFile::inspect(file, error);
    if (!source) return {{}, error};
    if (static_cast<std::uint64_t>(source->frames) > std::numeric_limits<std::size_t>::max())
        return {{}, "This audio file is too long for this build."};
    auto clip = std::make_shared<SessionAudioClip>();
    clip->sampleRate = source->sampleRate;
    clip->channels = source->channels;
    clip->frames = static_cast<std::size_t>(source->frames);
    clip->streamSource = std::move(source);
    if (!clip->valid()) return {{}, "The streamed audio metadata is invalid."};
    return {std::move(clip), {}};
}

// Called only from an import worker. With allowStreaming, files beyond the
// decoded-memory threshold become metadata-backed Session clips.
inline AudioClipImportResult decodeSessionAudioClip(const juce::File& file, bool allowStreaming = false) {
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
    if (frames64 <= 0) return {{}, "The selected audio file is empty."};

    const auto decodedLimit = SessionAudioClip::maxDecodedBytes
        / (static_cast<std::size_t>(channels) * sizeof(float));
    if (static_cast<std::uint64_t>(frames64) > decodedLimit)
        return allowStreaming ? inspectStreamedSessionAudio(file)
            : AudioClipImportResult{{}, "The decoded audio clip exceeds the 128 MiB sampler limit."};

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

inline AudioClipImportResult importSessionAudioClip(const juce::File& file) {
    return decodeSessionAudioClip(file, true);
}

} // namespace alien
