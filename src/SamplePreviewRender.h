#pragma once
#include "AudioClipImport.h"
#include "StreamingTempoWarp.h"

namespace alien {
inline bool validPreviewTempoRatio(double source, double project) noexcept {
    return std::isfinite(source) && source >= 30 && source <= 300
        && std::isfinite(project) && project >= 60 && project <= 200
        && source / project >= .5 && source / project <= 2;
}
inline constexpr const char* previewKeyNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
// Root-note shift from the sample's key to the target key, taking the nearer way round (-5 to +6 semitones).
inline int previewKeyShift(int sourceKey, int targetKey) noexcept {
    const int up = ((targetKey - sourceKey) % 12 + 12) % 12;
    return up > 6 ? up - 12 : up;
}
// A key shift stretches by 2^(semitones/12) before resampling back, so the
// combined stretch must stay inside the tempo warp's 0.5 to 2 range.
inline bool validPreviewSettings(bool matchTempo, double sourceBpm, double projectBpm, int semitones) noexcept {
    if (semitones < -6 || semitones > 6 || (matchTempo && !validPreviewTempoRatio(sourceBpm, projectBpm))) return false;
    const double stretch = (matchTempo ? sourceBpm / projectBpm : 1) * std::pow(2.0, semitones / 12.0);
    return stretch >= .5 && stretch <= 2;
}
// Worker-only. Decode/render bounded audio directly into the monitor asset;
// neither the source file nor any project-owned clip is modified.
// A nonzero semitone shift changes pitch while keeping the (matched) duration.
inline AudioClipImportResult renderSamplePreview(const juce::File& file, bool matchTempo,
    double sourceBpm, double projectBpm, const std::atomic<bool>& cancel, int semitones = 0) try {
    if (cancel.load()) return {{}, "Preview cancelled."};
    if (matchTempo && !validPreviewTempoRatio(sourceBpm, projectBpm))
        return {{}, "Set source BPM to give a tempo ratio between 0.5 and 2."};
    if (!validPreviewSettings(matchTempo, sourceBpm, projectBpm, semitones))
        return {{}, "This key shift and tempo ratio together stretch the sample too far."};
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
    // Without a key shift the warp writes straight into the clip. With one, it
    // stretches by the pitch factor into scratch buffers that are then read
    // back faster (or slower) to land on the clip's length at the new pitch.
    const double pitch = std::pow(2.0, semitones / 12.0);
    std::vector<float> stretchedLeft, stretchedRight;
    std::vector<float>& outLeft = semitones == 0 ? clip->left : stretchedLeft;
    std::vector<float>& outRight = semitones == 0 ? clip->right : stretchedRight;
    if (semitones != 0) {
        const auto stretched = static_cast<std::size_t>(std::ceil(length * ratio * pitch));
        stretchedLeft.assign(stretched + 16, 0.f); if (clip->channels == 2) stretchedRight.assign(stretched + 16, 0.f);
    }
    const auto expected = semitones == 0 ? clip->frames : outLeft.size() - 16;
    std::size_t written = 0;
    const bool rendered = streamTempoWarp(length, clip->sampleRate, clip->channels, ratio * pitch, cancel,
        [&](float* const* data, int channels, std::int64_t start, int count) { return !cancel.load() && reader->read(data, channels, start, count); },
        [&](const float* const* data, int channels, int count) {
            if (cancel.load() || static_cast<std::size_t>(count) > expected - written) return false;
            std::copy_n(data[0], count, outLeft.data() + written);
            if (channels == 2) std::copy_n(data[1], count, outRight.data() + written);
            written += static_cast<std::size_t>(count); return true;
        });
    if (!rendered || cancel.load() || written != expected) return {{}, "Preview cancelled or audio could not be read."};
    if (semitones != 0) {
        juce::LagrangeInterpolator left, right;
        left.process(pitch, stretchedLeft.data(), clip->left.data(), static_cast<int>(clip->frames));
        if (clip->channels == 2) right.process(pitch, stretchedRight.data(), clip->right.data(), static_cast<int>(clip->frames));
    }
    clip->buildWaveform(); return {std::move(clip), {}};
} catch (const std::bad_alloc&) { return {{}, "Not enough memory for sample preview."}; }
} // namespace alien
