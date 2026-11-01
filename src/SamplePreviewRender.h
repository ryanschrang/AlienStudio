#pragma once
#include "AudioClipImport.h"
#include "StreamingTempoWarp.h"

namespace alien {
inline bool validPreviewTempoRatio(double source, double project) noexcept {
    return std::isfinite(source) && source >= 30 && source <= 300
        && std::isfinite(project) && project >= 60 && project <= 200
        && source / project >= .5 && source / project <= 2;
}
// Worker-only. Decode/render bounded audio directly into the monitor asset;
// neither the source file nor any project-owned clip is modified.
inline AudioClipImportResult renderSamplePreview(const juce::File& file, bool matchTempo,
    double sourceBpm, double projectBpm, const std::atomic<bool>& cancel) try {
    if (cancel.load()) return {{}, "Preview cancelled."};
    if (matchTempo && !validPreviewTempoRatio(sourceBpm, projectBpm))
        return {{}, "Set source BPM to give a tempo ratio between 0.5 and 2."};
    if (!file.hasFileExtension("wav;mp3")) return {{}, "Choose a WAV or MP3 sample."};
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate)
        || reader->sampleRate < 8000 || reader->sampleRate > 192000 || reader->lengthInSamples <= 0)
        return {{}, "Cannot preview this audio file."};
    const double ratio = matchTempo ? sourceBpm / projectBpm : 1;
    const auto limit = static_cast<std::int64_t>(reader->sampleRate * 5);
    auto length = std::min<std::int64_t>(reader->lengthInSamples,
        static_cast<std::int64_t>(std::floor(limit / std::max(1.0, ratio))));
    while (length > 0 && std::ceil(length * ratio) > limit) --length;
    if (length <= 0) return {{}, "The sample is empty."};
    auto clip = std::make_shared<SessionAudioClip>();
    clip->sampleRate = reader->sampleRate; clip->channels = static_cast<int>(reader->numChannels);
    clip->frames = static_cast<std::size_t>(std::ceil(length * ratio));
    clip->left.resize(clip->frames); if (clip->channels == 2) clip->right.resize(clip->frames);
    std::size_t written = 0;
    const bool rendered = streamTempoWarp(length, clip->sampleRate, clip->channels, ratio, cancel,
        [&](float* const* data, int channels, std::int64_t start, int count) { return !cancel.load() && reader->read(data, channels, start, count); },
        [&](const float* const* data, int channels, int count) {
            if (cancel.load() || static_cast<std::size_t>(count) > clip->frames - written) return false;
            std::copy_n(data[0], count, clip->left.data() + written);
            if (channels == 2) std::copy_n(data[1], count, clip->right.data() + written);
            written += static_cast<std::size_t>(count); return true;
        });
    if (!rendered || cancel.load() || written != clip->frames) return {{}, "Preview cancelled or audio could not be read."};
    clip->buildWaveform(); return {std::move(clip), {}};
} catch (const std::bad_alloc&) { return {{}, "Not enough memory for sample preview."}; }
} // namespace alien
