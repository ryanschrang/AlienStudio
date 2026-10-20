#include "WavExport.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

static void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct MemoryFile final : alien::DiskAudioFile {
    struct Reader final : alien::DiskAudioReader {
        explicit Reader(const MemoryFile& owner) : file(owner) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            if (file.failReads || start < 0 || start + count > file.frames) return false;
            for (int i = 0; i < count; ++i) {
                left[i] = file.samples[static_cast<std::size_t>(start + i)];
                if (right) right[i] = file.stereo.empty()
                    ? left[i] : file.stereo[static_cast<std::size_t>(start + i)];
            }
            return true;
        }
        const MemoryFile& file;
    };
    MemoryFile(double rate, std::vector<float> mono, bool opens = true)
        : DiskAudioFile(rate, 1, static_cast<std::int64_t>(mono.size())),
          samples(std::move(mono)), opensOk(opens) {}
    std::unique_ptr<alien::DiskAudioReader> open() const override {
        ++openCount;
        return opensOk ? std::make_unique<Reader>(*this) : nullptr;
    }
    std::vector<float> samples, stereo;
    bool opensOk{true}, failReads{};
    mutable int openCount{};
};

static alien::SessionAudioClip streamedClip(const std::shared_ptr<MemoryFile>& source) {
    alien::SessionAudioClip clip;
    clip.sampleRate = source->sampleRate;
    clip.channels = source->channels;
    clip.frames = static_cast<std::size_t>(source->frames);
    clip.streamSource = source;
    return clip;
}

static void prepare(std::array<juce::AudioBuffer<float>, alien::maxSessionTracks>& tracks, int samples) {
    for (auto& track : tracks) { track.setSize(2, samples); track.clear(); }
}

int main() {
    std::atomic<bool> cancelled{false};
    juce::String error;

    auto source = std::make_shared<MemoryFile>(24000.0, std::vector<float>{0.0f, 1.0f, 0.0f});
    auto clip = streamedClip(source);
    auto song = std::make_unique<alien::ArrangementPlayback>();
    song->arrangement.lengthBars = 1;
    check(song->arrangement.add({0, 0, 0, 1}), "add streamed region");
    song->clips[0][0].audio = &clip;

    auto render = alien::DeterministicStreamedArrangement::create(*song, 120.0, 48000.0, 1.0f, cancelled, error);
    check(render != nullptr && source->openCount == 1, "preflight opens an independent reader");
    std::array<juce::AudioBuffer<float>, alien::maxSessionTracks> tracks;
    prepare(tracks, 5);
    render->addBlock(tracks, 5);
    const float expected[]{0.0f, 0.5f, 1.0f, 0.5f, 0.0f};
    for (int i = 0; i < 5; ++i)
        check(std::abs(tracks[0].getSample(0, i) - expected[i]) < 1.0e-6f,
              "sample-rate conversion uses deterministic interpolation");

    auto project = std::make_unique<alien::SessionProjectData>();
    project->arrangement = song->arrangement;
    const auto output = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("alien-streamed-export", ".wav");
    alien::DrumSamplePointers noDrums{};
    check(alien::exportArrangementWav(output, *project, *song, noDrums, cancelled, error),
          "streamed clip exports through the complete WAV path");
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> exported(formats.createReaderFor(output));
    check(exported && exported->lengthInSamples == 96000,
          "streamed WAV has the exact arrangement duration");
    juce::AudioBuffer<float> exportedStart(2, 8);
    check(exported->read(&exportedStart, 0, 8, 0, true, true),
          "read the streamed WAV result");
    float energy = 0.0f;
    for (int i=0;i<8;++i) energy += std::abs(exportedStart.getSample(0,i));
    check(energy > 0.1f, "streamed WAV contains source audio");
    exported.reset(); output.deleteFile();

    auto duplicates = std::make_unique<alien::ArrangementPlayback>();
    duplicates->arrangement.lengthBars = 1;
    check(duplicates->arrangement.add({0,0,0,1}) && duplicates->arrangement.add({1,0,0,1}),
          "duplicate source regions are valid on separate tracks");
    duplicates->clips[0][0].audio = &clip;
    duplicates->clips[0][1].audio = &clip;
    const int before = source->openCount;
    auto duplicateRender = alien::DeterministicStreamedArrangement::create(
        *duplicates, 120.0, 48000.0, 1.0f, cancelled, error);
    check(duplicateRender != nullptr && source->openCount == before + 2,
          "duplicate clips receive independent readers");
    prepare(tracks, 5);
    duplicateRender->addBlock(tracks, 5);
    for (int i = 0; i < 5; ++i)
        check(tracks[0].getSample(0,i) == tracks[1].getSample(0,i),
              "duplicate clips render identically");

    // Rendering the same timeline with different callback sizes must produce
    // identical samples: there is no prebuffer timing in the result.
    auto oneBlock = alien::DeterministicStreamedArrangement::create(
        *song, 120.0, 48000.0, 1.0f, cancelled, error);
    auto splitBlocks = alien::DeterministicStreamedArrangement::create(
        *song, 120.0, 48000.0, 1.0f, cancelled, error);
    std::array<juce::AudioBuffer<float>, alien::maxSessionTracks> whole, split;
    prepare(whole, 5); oneBlock->addBlock(whole, 5);
    prepare(split, 5);
    std::array<juce::AudioBuffer<float>, alien::maxSessionTracks> first, second;
    prepare(first, 2); splitBlocks->addBlock(first, 2);
    prepare(second, 3); splitBlocks->addBlock(second, 3);
    for (int i=0;i<2;++i) split[0].setSample(0,i,first[0].getSample(0,i));
    for (int i=0;i<3;++i) split[0].setSample(0,i+2,second[0].getSample(0,i));
    for (int i=0;i<5;++i)
        check(whole[0].getSample(0,i)==split[0].getSample(0,i),
              "output is independent of export block size");

    auto missing = std::make_shared<MemoryFile>(48000.0, std::vector<float>{1.0f}, false);
    auto missingClip = streamedClip(missing);
    auto missingSong = std::make_unique<alien::ArrangementPlayback>();
    missingSong->arrangement.lengthBars=1;
    missingSong->arrangement.add({0,0,0,1});
    missingSong->clips[0][0].audio=&missingClip;
    auto unavailable = alien::DeterministicStreamedArrangement::create(
        *missingSong,120.0,48000.0,1.0f,cancelled,error);
    check(!unavailable && error.containsIgnoreCase("missing"),
          "a missing file fails during preflight");

    cancelled.store(true);
    auto stopped = alien::DeterministicStreamedArrangement::create(
        *song,120.0,48000.0,1.0f,cancelled,error);
    prepare(tracks,5); stopped->addBlock(tracks,5);
    check(stopped->wasCancelled() && stopped->renderedFrames()==0,
          "cancellation stops before another block is read");
    cancelled.store(false);

    const auto hours = alien::arrangementFrameCount(
        alien::Arrangement::maxLengthBars, 60.0, 48000.0);
    check(hours == 3145728000LL && hours > std::numeric_limits<std::int32_t>::max(),
          "multi-hour export duration uses 64-bit frame counts");
    alien::Arrangement longSong;
    longSong.lengthBars=alien::Arrangement::maxLengthBars;
    check(longSong.valid(), "the arrangement model accepts a 18-hour timeline");

    std::cout << "Deterministic streamed export tests passed\n";
}
