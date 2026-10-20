#pragma once
#include "DiskAudioStream.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace alien {
// Header-only metadata inspection happens on an import worker, never audio.
// Opening a source again produces a fresh decoder with an independent cursor.
class JuceDiskAudioFile final : public DiskAudioFile {
public:
    JuceDiskAudioFile(juce::File file, double rate, int channels, std::int64_t frames)
        : DiskAudioFile(rate, channels, frames), path(std::move(file)) {}
    static std::shared_ptr<JuceDiskAudioFile> inspect(const juce::File& path, juce::String& error) {
        if (!path.hasFileExtension("wav;mp3")) { error = "Choose a WAV or MP3 source."; return {}; }
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(path));
        if (!reader) { error = "Cannot read audio source: " + path.getFullPathName(); return {}; }
        auto source = std::make_shared<JuceDiskAudioFile>(path, reader->sampleRate,
            static_cast<int>(reader->numChannels), reader->lengthInSamples);
        if (!source->valid()) { error = "Use non-empty mono/stereo audio at 8-192 kHz."; return {}; }
        error.clear();
        return source;
    }
    std::unique_ptr<DiskAudioReader> open() const override {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(path));
        if (!reader || reader->sampleRate != sampleRate || reader->numChannels != static_cast<unsigned>(channels)
            || reader->lengthInSamples != frames) return {};
        return std::make_unique<Reader>(std::move(reader), channels, frames);
    }
    const juce::File path;
private:
    class Reader final : public DiskAudioReader {
    public:
        Reader(std::unique_ptr<juce::AudioFormatReader> reader, int channels, std::int64_t frames)
            : decoder(std::move(reader)), channels(channels), frames(frames) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            if (start < 0 || count < 0 || start > frames || count > frames - start
                || !left || (channels == 2 && !right)) return false;
            float* data[]{left, right};
            return decoder->read(data, channels, start, count);
        }
    private:
        std::unique_ptr<juce::AudioFormatReader> decoder;
        int channels;
        std::int64_t frames;
    };
};
} // namespace alien
