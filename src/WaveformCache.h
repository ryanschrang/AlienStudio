#pragma once
#include "SessionAudioClip.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <functional>
#include <vector>

namespace alien {

struct WaveformPeakData {
    double sampleRate{};
    int channels{};
    std::int64_t frames{};
    std::vector<AudioWaveformPeak> peaks;
};

struct WaveformCacheResult {
    std::shared_ptr<const WaveformPeakData> data;
    juce::String error;
    bool cacheHit{};
};

class LongWaveformCache {
public:
    static constexpr int version = 1;
    static constexpr std::size_t peakCount = 65536;
    static constexpr int readBlock = 8192;

    static juce::File fileFor(const juce::File& project, const juce::File& source) {
        juce::File root = project == juce::File{}
            ? juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("Alien Studio").getChildFile("Waveforms")
            : project.getSiblingFile(project.getFileName() + ".cache").getChildFile("waveforms");
        const auto canonical = source.getFullPathName().toLowerCase();
        const auto utf8 = canonical.toUTF8();
        const juce::MD5 digest(utf8.getAddress(), utf8.sizeInBytes() - 1);
        return root.getChildFile(digest.toHexString() + ".awp");
    }

    static WaveformCacheResult loadOrCreate(const juce::File& source, const juce::File& cache,
        const std::function<bool()>& cancelled = {}) {
        if (!source.existsAsFile()) return {{}, "Audio source is offline.", false};
        if (auto loaded = load(source, cache); loaded.data) return loaded;
        auto generated = generate(source, cancelled);
        if (!generated.data) return generated;
        write(source, cache, *generated.data);
        return generated;
    }

    static WaveformCacheResult load(const juce::File& source, const juce::File& cache) {
        if (!cache.existsAsFile()) return {};
        auto in = cache.createInputStream();
        if (!in || in->readInt() != magic || in->readInt() != version) return {};
        const auto savedPath = in->readString();
        const auto savedSize = in->readInt64();
        const auto savedTime = in->readInt64();
        const auto rate = in->readDouble();
        const auto channels = in->readInt();
        const auto frames = in->readInt64();
        const auto count = in->readInt();
        if (savedPath != source.getFullPathName() || savedSize != source.getSize()
            || savedTime != source.getLastModificationTime().toMilliseconds()
            || !std::isfinite(rate) || rate < 8000 || rate > 192000
            || channels < 1 || channels > 2 || frames <= 0
            || count <= 0 || count > static_cast<int>(peakCount)) return {};
        auto data = std::make_shared<WaveformPeakData>();
        data->sampleRate = rate; data->channels = channels; data->frames = frames;
        data->peaks.resize(static_cast<std::size_t>(count));
        for (auto& peak : data->peaks) {
            peak.leftMin = in->readFloat(); peak.leftMax = in->readFloat();
            peak.rightMin = in->readFloat(); peak.rightMax = in->readFloat();
            if (!valid(peak)) return {};
        }
        if (in->getPosition() != in->getTotalLength()) return {};
        return {std::move(data), {}, true};
    }

    static WaveformCacheResult generate(const juce::File& source,
        const std::function<bool()>& cancelled = {}) {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
        if (!reader) return {{}, "The waveform decoder could not open this audio file.", false};
        const auto frames = reader->lengthInSamples;
        const auto channels = static_cast<int>(reader->numChannels);
        if (frames <= 0 || channels < 1 || channels > 2 || !std::isfinite(reader->sampleRate))
            return {{}, "The waveform decoder found invalid audio metadata.", false};
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(frames, peakCount));
        auto data = std::make_shared<WaveformPeakData>();
        data->sampleRate = reader->sampleRate; data->channels = channels; data->frames = frames;
        data->peaks.assign(count, AudioWaveformPeak{1.0f,-1.0f,1.0f,-1.0f});
        std::vector<float> left(readBlock), right(channels == 2 ? readBlock : 0);
        for (std::int64_t start = 0; start < frames; start += readBlock) {
            if (cancelled && cancelled()) return {{}, "Waveform generation cancelled.", false};
            const int amount = static_cast<int>(std::min<std::int64_t>(readBlock, frames - start));
            float* destinations[]{left.data(), channels == 2 ? right.data() : nullptr};
            if (!reader->read(destinations, channels, start, amount))
                return {{}, "The waveform decoder stopped before the file ended.", false};
            for (int i=0;i<amount;++i) {
                const auto frame = start + i;
                const auto bucket = std::min(count - 1,
                    static_cast<std::size_t>((static_cast<long double>(frame) * count) / frames));
                const float l = std::isfinite(left[static_cast<std::size_t>(i)]) ? left[static_cast<std::size_t>(i)] : 0.0f;
                const float r = channels == 2 && std::isfinite(right[static_cast<std::size_t>(i)])
                    ? right[static_cast<std::size_t>(i)] : l;
                auto& p = data->peaks[bucket];
                p.leftMin=std::min(p.leftMin,l); p.leftMax=std::max(p.leftMax,l);
                p.rightMin=std::min(p.rightMin,r); p.rightMax=std::max(p.rightMax,r);
            }
        }
        for (auto& p : data->peaks) if (!valid(p)) p={0,0,0,0};
        return {std::move(data), {}, false};
    }

    static bool write(const juce::File& source, const juce::File& cache, const WaveformPeakData& data) {
        if (!cache.getParentDirectory().createDirectory().wasOk()) return false;
        const auto temporary = cache.getSiblingFile(cache.getFileName() + ".tmp");
        temporary.deleteFile();
        auto out = temporary.createOutputStream();
        if (!out || !out->openedOk()) return false;
        out->writeInt(magic); out->writeInt(version);
        out->writeString(source.getFullPathName());
        out->writeInt64(source.getSize());
        out->writeInt64(source.getLastModificationTime().toMilliseconds());
        out->writeDouble(data.sampleRate); out->writeInt(data.channels); out->writeInt64(data.frames);
        out->writeInt(static_cast<int>(data.peaks.size()));
        for (const auto& p : data.peaks) {
            out->writeFloat(p.leftMin); out->writeFloat(p.leftMax);
            out->writeFloat(p.rightMin); out->writeFloat(p.rightMax);
        }
        out->flush();
        if (out->getStatus().failed()) { out.reset(); temporary.deleteFile(); return false; }
        out.reset();
        cache.deleteFile();
        return temporary.moveFileTo(cache);
    }
private:
    static constexpr int magic = 0x41575031;
    static bool valid(const AudioWaveformPeak& p) noexcept {
        return std::isfinite(p.leftMin)&&std::isfinite(p.leftMax)
            &&std::isfinite(p.rightMin)&&std::isfinite(p.rightMax)
            &&p.leftMin<=p.leftMax&&p.rightMin<=p.rightMax
            &&p.leftMin>=-1.01f&&p.leftMax<=1.01f&&p.rightMin>=-1.01f&&p.rightMax<=1.01f;
    }
};

} // namespace alien
