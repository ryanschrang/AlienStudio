// Disk-streamed clips on the Arrangement timeline are rendered by the same engine as everything else, with
// their streams read on the rendering thread. These tests check that the result is what the in-memory
// clips would give, whatever the block size, and that missing or failing files stop the export with a message.
#include "WavExport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace alien::timeline;

static void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct MemoryFile final : alien::DiskAudioFile {
    struct Reader final : alien::DiskAudioReader {
        explicit Reader(const MemoryFile& owner) : file(owner) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            if (file.readsBeforeFailure == 0 || start < 0 || start + count > file.frames) return false;
            if (file.readsBeforeFailure > 0) --file.readsBeforeFailure;
            ++file.readCount;
            for (int i = 0; i < count; ++i) {
                left[i] = file.samples[static_cast<std::size_t>(start + i)];
                if (right) right[i] = file.stereo.empty()
                    ? left[i] : file.stereo[static_cast<std::size_t>(start + i)];
            }
            return true;
        }
        const MemoryFile& file;
    };
    MemoryFile(double rate, std::vector<float> first, std::vector<float> second = {}, bool opens = true)
        : DiskAudioFile(rate, second.empty() ? 1 : 2, static_cast<std::int64_t>(first.size())),
          samples(std::move(first)), stereo(std::move(second)), opensOk(opens) {}
    std::unique_ptr<alien::DiskAudioReader> open() const override {
        ++openCount;
        return opensOk ? std::make_unique<Reader>(*this) : nullptr;
    }
    std::vector<float> samples, stereo;
    bool opensOk{true};
    mutable int readsBeforeFailure{-1};   // a read fails once this many have succeeded; -1: never
    mutable int openCount{}, readCount{};
};

// One piece of source material, available both decoded and streamed.
struct Material {
    std::shared_ptr<MemoryFile> file;
    alien::SessionAudioClip decoded, streamed;
};
static std::unique_ptr<Material> makeMaterial(double rate, int channels, double seconds, unsigned seed) {
    const auto frames = static_cast<std::size_t>(rate * seconds);
    std::vector<float> left(frames), right(channels == 2 ? frames : 0);
    unsigned state = seed * 2654435761u + 12345u;
    auto noise = [&] { state = state * 1664525u + 1013904223u; return static_cast<float>((state >> 8) & 0xffff) / 32768.0f - 1.0f; };
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        left[i] = static_cast<float>(0.5 * std::sin(6.2831853 * (110.0 + 40.0 * seed) * t) + 0.2 * noise());
        if (channels == 2) right[i] = static_cast<float>(0.5 * std::sin(6.2831853 * (170.0 + 30.0 * seed) * t + 1.0) + 0.2 * noise());
    }
    auto m = std::make_unique<Material>();
    m->file = std::make_shared<MemoryFile>(rate, left, right);
    m->decoded.sampleRate = rate; m->decoded.channels = channels; m->decoded.frames = frames;
    m->decoded.left = left; m->decoded.right = right; m->decoded.buildWaveform();
    m->streamed.sampleRate = rate; m->streamed.channels = channels; m->streamed.frames = frames;
    m->streamed.streamSource = m->file;
    check(m->decoded.valid() && m->streamed.valid(), "test material is valid");
    return m;
}

struct Wav { std::int64_t frames{}; juce::AudioBuffer<float> samples; };
static Wav readWav(const juce::File& file) {
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    check(reader != nullptr && reader->sampleRate == 48000.0 && reader->numChannels == 2, "exported WAV is 48 kHz stereo");
    Wav wav; wav.frames = reader->lengthInSamples;
    wav.samples.setSize(2, static_cast<int>(wav.frames));
    check(reader->read(&wav.samples, 0, static_cast<int>(wav.frames), 0, true, true), "read the exported WAV");
    return wav;
}
static double maxDifference(const Wav& a, const Wav& b) {
    check(a.frames == b.frames, "two renders of the same song are the same length");
    double worst = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < a.samples.getNumSamples(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(a.samples.getSample(c, i) - b.samples.getSample(c, i))));
    return worst;
}
static double energy(const Wav& w, int from, int to) {
    double sum = 0;
    for (int i = std::max(0, from); i < std::min<int>(to, static_cast<int>(w.frames)); ++i)
        sum += static_cast<double>(w.samples.getSample(0, i)) * w.samples.getSample(0, i);
    return sum;
}

// A little project and song: `streamedScenes` is a bit mask of the Session scenes whose clips are streamed.
struct Song {
    std::unique_ptr<alien::SessionProjectData> project = std::make_unique<alien::SessionProjectData>();
    std::unique_ptr<alien::ArrangementPlayback> playback = std::make_unique<alien::ArrangementPlayback>();
};
static Region region(int row, int scene, std::int64_t start, std::int64_t length, double audioOffset = 0.0, bool muted = false) {
    Region r; r.row = row; r.source = scene; r.start = start; r.length = length; r.audioOffset = audioOffset; r.muted = muted;
    return r;
}
static void put(Song& song, int track, int scene, Material& m, unsigned streamedScenes) {
    song.playback->clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)].audio =
        (streamedScenes >> scene & 1u) != 0 ? &m.streamed : &m.decoded;
}
static void finish(Song& song) { song.project->timeline = song.playback->timeline; }

static juce::File tempWav(const char* name) {
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile(name, ".wav");
}
static Wav render(const Song& song, const alien::WavExportOptions& options = {}) {
    std::atomic<bool> never{false};
    juce::String error;
    alien::DrumSamplePointers noDrums{};
    const auto file = tempWav("alien-streamed-export");
    check(alien::exportArrangementWav(file, *song.project, *song.playback, noDrums, never, error, nullptr, options),
          error.toRawUTF8());
    auto wav = readWav(file);
    file.deleteFile();
    return wav;
}

// A busy song: clips of several rates and channel counts, trimmed, split, replaced, muted, cut off by the
// end of the clip, and starting between samples.
static Song busySong(std::array<std::unique_ptr<Material>, 4>& m, unsigned streamedScenes) {
    Song song;
    song.project->bpm = 133;
    m[0] = makeMaterial(44100.0, 1, 6.0, 1);
    m[1] = makeMaterial(96000.0, 2, 5.0, 2);
    m[2] = makeMaterial(22050.0, 1, 4.0, 3);
    m[3] = makeMaterial(48000.0, 2, 1.5, 4);
    auto& tl = song.playback->timeline;
    tl.setLengthBars(5);
    const double bpm = 133.0;
    // Track 0: a clip in two pieces that continue each other, then a trimmed one after a gap.
    const std::int64_t firstLength = ticksPerBar + 37;
    check(tl.place(region(0, 0, 0, firstLength)), "place the first piece");
    check(tl.place(region(0, 0, firstLength, 3000, secondsForTicks(static_cast<double>(firstLength), bpm))), "place the piece that continues it");
    check(tl.place(region(0, 0, 3 * ticksPerBar + 123, 1000, 1.234)), "place a trimmed piece after a gap");
    // Track 1: a stereo 96 kHz clip trimmed at the front, starting between samples, then another clip right after it.
    check(tl.place(region(1, 1, 3 * ticksPerBeat + 17, 7000, 0.3)), "place the stereo clip");
    check(tl.place(region(1, 2, 3 * ticksPerBeat + 17 + 7000, 5000)), "place a different clip right after it");
    // Track 2: a muted region.
    check(tl.place(region(2, 0, ticksPerBar, ticksPerBar, 0.0, true)), "place a muted region");
    // Track 3: a region longer than its clip.
    check(tl.place(region(3, 3, 0, 2 * ticksPerBar)), "place a region longer than the clip");
    check(tl.place(region(3, 3, 4 * ticksPerBar, ticksPerBar, 1.2)), "place a region that starts near the clip's end");
    put(song, 0, 0, *m[0], streamedScenes);
    put(song, 1, 1, *m[1], streamedScenes);
    put(song, 1, 2, *m[2], streamedScenes);
    put(song, 2, 0, *m[0], streamedScenes);
    put(song, 3, 3, *m[3], streamedScenes);
    finish(song);
    return song;
}

static void stemTests() {
    auto first = makeMaterial(48000,1,2,1), second = makeMaterial(48000,2,2,2);
    Song song; song.playback->timeline.setLengthBars(1);
    check(song.playback->timeline.place(region(0,0,0,ticksPerBar))
        && song.playback->timeline.place(region(1,1,0,ticksPerBar)), "stem fixture");
    put(song,0,0,*first,0); put(song,1,1,*second,2); finish(song);
    song.project->trackNames[0] = song.project->trackNames[1] = "Same";
    song.project->trackSettings[0].gain = 0.25f; song.project->trackSettings[1].gain = 0.35f;
    song.project->trackSettings[0].send[0] = 0.4f; song.project->trackSettings[1].send[0] = 0.2f;
    const auto reference = render(song);
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStemTest","",false);
    check(root.createDirectory().wasOk(), "stem test folder");
    alien::WavExportOptions options; options.stems = true;
    const auto target = root.getChildFile("stems"); std::atomic<bool> stop{false}; juce::String error;
    alien::DrumSamplePointers drums{};
    check(alien::exportArrangementWav(target,*song.project,*song.playback,drums,stop,error,nullptr,options), error.toRawUTF8());
    const auto files = target.findChildFiles(juce::File::findFiles,false,"*.wav");
    check(files.size() == song.project->trackCount() + alien::maxReturns + 1 && target.getChildFile("README.txt").existsAsFile(),
        "one file per track, return and direct-rack bus plus routing notes");
    std::vector<float> summed(static_cast<std::size_t>(reference.frames),0);
    for (const auto& file : files) {
        const auto wav = readWav(file); check(wav.frames == reference.frames, "common stem duration");
        for (int f=0; f<static_cast<int>(wav.frames); ++f) summed[static_cast<std::size_t>(f)] += wav.samples.getSample(0,f);
    }
    double worst=0;
    for (int f=0; f<static_cast<int>(reference.frames); ++f)
        worst=std::max(worst,static_cast<double>(std::abs(alien::softLimit(summed[static_cast<std::size_t>(f)])-reference.samples.getSample(0,f))));
    check(worst < 2e-6, "stems sum to original pre-master mix within PCM quantization");
    check(target.getChildFile("01 - Same.wav").existsAsFile() && target.getChildFile("02 - Same.wav").existsAsFile(),
        "duplicate track names have unique numbered files");
    check(!alien::exportArrangementWav(target,*song.project,*song.playback,drums,stop,error,nullptr,options)
        && target.getChildFile("01 - Same.wav").existsAsFile(), "existing destination is preserved");
    stop.store(true);
    check(!alien::exportArrangementWav(root.getChildFile("cancelled"),*song.project,*song.playback,drums,stop,error,nullptr,options)
        && !root.getChildFile("cancelled").exists(), "cancelled export publishes no partial folder");
    stop.store(false);
    song.project->masterGain=0.1f; options.blockFrames=113;
    const auto alternate=root.getChildFile("alternate");
    check(alien::exportArrangementWav(alternate,*song.project,*song.playback,drums,stop,error,nullptr,options), "alternate block-size stems");
    for (const auto& file : files)
        check(maxDifference(readWav(file),readWav(alternate.getChildFile(file.getFileName()))) < 3e-7,
            "stems ignore master gain and are independent of callback size");
    second->file->readsBeforeFailure=0;
    check(!alien::exportArrangementWav(root.getChildFile("failed-read"),*song.project,*song.playback,drums,stop,error,nullptr,options)
        && !root.getChildFile("failed-read").exists(), "mid-render source failure publishes no partial folder");
    const auto children=root.findChildFiles(juce::File::findDirectories,false);
    check(children.size() == 2, "staging folders cleaned after failure/cancellation");
    root.deleteRecursively();
}

int main() {
    stemTests();
    std::atomic<bool> cancelled{false};
    juce::String error;
    alien::DrumSamplePointers noDrums{};

    // --- The same song, from memory or from disk ---------------------------------------------------------------
    std::array<std::unique_ptr<Material>, 4> materials;
    const auto decoded = render(busySong(materials, 0));
    check(decoded.frames == alien::exportFrameCount(5 * ticksPerBar, 133.0, 48000.0), "the whole song is exported");
    check(energy(decoded, 0, static_cast<int>(decoded.frames)) > 1.0, "the busy song makes sound");
    for (const unsigned mask : {0xFu, 0x2u, 0x5u}) {   // every scene streamed, then mixes where decoded and streamed clips meet
        std::array<std::unique_ptr<Material>, 4> again;
        const auto streamed = render(busySong(again, mask));
        const double difference = maxDifference(decoded, streamed);
        std::cerr << "scene mask " << mask << ": largest difference " << difference << '\n';
        check(difference < 3.0e-7, "streamed clips render like decoded ones, including where decoded and streamed clips meet");
    }
    {   // A part of the song, starting off the grid.
        alien::WavExportOptions range; range.fromTick = ticksPerBar + 777; range.toTick = 4 * ticksPerBar + 333;
        std::array<std::unique_ptr<Material>, 4> a, b;
        const auto reference = render(busySong(a, 0), range);
        const auto streamed = render(busySong(b, 0xF), range);
        check(reference.frames > 0 && maxDifference(reference, streamed) < 3.0e-7, "an exported range of streamed clips matches the decoded one");
    }

    // --- The result does not depend on the size of the blocks it is rendered in ---------------------------------
    {
        std::array<std::unique_ptr<Material>, 4> a, b, c;
        alien::WavExportOptions big, small, tiny;
        small.blockFrames = 100; tiny.blockFrames = 33;
        const auto one = render(busySong(a, 0xF), big);
        const auto two = render(busySong(b, 0xF), small);
        const auto three = render(busySong(c, 0xF), tiny);
        std::cerr << "block sizes: " << maxDifference(one, two) << ' ' << maxDifference(one, three) << '\n';
        check(maxDifference(one, two) == 0.0 && maxDifference(one, three) == 0.0, "streamed output is independent of the render block size");
    }

    // --- A clip placed twice is read by independent readers ----------------------------------------------------
    {
        Song song;
        auto m = makeMaterial(24000.0, 1, 0.5, 5);
        song.playback->timeline.setLengthBars(1);
        check(song.playback->timeline.place(region(0, 0, 0, ticksPerBar)) && song.playback->timeline.place(region(1, 0, 0, ticksPerBar)),
              "the same scene is placed on two tracks");
        song.playback->clips[0][0].audio = &m->streamed;
        song.playback->clips[0][1].audio = &m->streamed;
        finish(song);
        const auto both = render(song);
        check(m->file->openCount >= 4, "each placement was checked, and each is played from its own reader");
        auto single = std::make_unique<Song>();
        single->playback->timeline.setLengthBars(1);
        single->playback->timeline.place(region(0, 0, 0, ticksPerBar));
        single->playback->clips[0][0].audio = &m->streamed;
        finish(*single);
        const auto one = render(*single);
        check(energy(both, 0, 48000) > 1.5 * energy(one, 0, 48000), "two tracks playing the same clip are both heard");
    }

    // --- Sample-rate conversion is plain interpolation ----------------------------------------------------------
    {
        Song song;
        auto source = std::make_shared<MemoryFile>(24000.0, std::vector<float>{0.0f, 1.0f, 0.0f});
        alien::SessionAudioClip clip;
        clip.sampleRate = 24000.0; clip.channels = 1; clip.frames = 3; clip.streamSource = source;
        song.playback->timeline.setLengthBars(1);
        check(song.playback->timeline.place(region(0, 0, 0, ticksPerBar)), "place a three-frame clip");
        song.playback->clips[0][0].audio = &clip;
        finish(song);
        const auto wav = render(song);
        check(wav.frames == 96000, "streamed WAV has the exact arrangement duration");
        const float peak = wav.samples.getSample(0, 2), halfway = wav.samples.getSample(0, 1);
        check(peak > 0.05f && std::abs(halfway / peak - 0.5f) < 1.0e-4f && std::abs(wav.samples.getSample(0, 3) / peak - 0.5f) < 1.0e-4f
              && wav.samples.getSample(0, 0) == 0.0f && std::abs(wav.samples.getSample(0, 4)) < 1.0e-6f,
              "a 24 kHz clip played at 48 kHz is interpolated between its frames");
        check(energy(wav, 5, 96000) < 1.0e-9, "and then it is silent");
    }

    // --- Failures stop the export with a message and leave the destination alone --------------------------------
    {
        Song song;
        auto m = makeMaterial(48000.0, 1, 1.0, 6);
        m->file->opensOk = false;
        song.playback->timeline.setLengthBars(1);
        song.playback->timeline.place(region(0, 0, 0, ticksPerBar));
        song.playback->clips[0][0].audio = &m->streamed;
        finish(song);
        const auto target = tempWav("alien-streamed-missing");
        std::atomic<bool> never{false};
        check(!alien::exportArrangementWav(target, *song.project, *song.playback, noDrums, never, error) && error.containsIgnoreCase("missing")
              && error.contains("track 1") && !target.exists(), "a missing file fails during preflight, before anything is written");

        m->file->opensOk = true;
        m->file->readsBeforeFailure = 3;
        check(!alien::exportArrangementWav(target, *song.project, *song.playback, noDrums, never, error)
              && error.containsIgnoreCase("streamed source failed") && !target.exists(), "a file that stops being readable fails the export");

        m->file->readsBeforeFailure = -1;
        cancelled.store(true);
        check(!alien::exportArrangementWav(target, *song.project, *song.playback, noDrums, cancelled, error)
              && error.containsIgnoreCase("cancel") && !target.exists(), "cancelling before the first block writes nothing");
        cancelled.store(false);
    }
    {   // Cancelling part-way through a long render leaves an existing file as it was.
        Song song;
        auto m = makeMaterial(44100.0, 2, 30.0, 7);
        song.playback->timeline.setLengthBars(400);
        for (int i = 0; i < 4; ++i) song.playback->timeline.place(region(0, 0, static_cast<std::int64_t>(i) * 100 * ticksPerBar, 100 * ticksPerBar));
        song.playback->clips[0][0].audio = &m->streamed;
        finish(song);
        const auto target = tempWav("alien-streamed-cancel");
        check(target.replaceWithText("original"), "an existing file to protect");
        std::atomic<bool> stop{false};
        std::thread canceller([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); stop.store(true); });
        const bool ok = alien::exportArrangementWav(target, *song.project, *song.playback, noDrums, stop, error);
        canceller.join();
        check(!ok && error.containsIgnoreCase("cancel") && target.loadFileAsString() == "original", "cancelling mid-render leaves the existing file unchanged");
        target.deleteFile();
    }

    // --- Eighteen hours -----------------------------------------------------------------------------------------
    {
        const auto frames = alien::exportFrameCount(maxBars * ticksPerBar, 60.0, 48000.0);
        check(frames == 3145728000LL && frames > std::numeric_limits<std::int32_t>::max(), "multi-hour export duration uses 64-bit frame counts");
        auto m = makeMaterial(44100.0, 2, 4.0, 8);
        alien::WavExportOptions lastBar; lastBar.fromTick = (maxBars - 1) * ticksPerBar; lastBar.toTick = -1;
        Wav references[2];
        for (int streamed = 0; streamed < 2; ++streamed) {
            Song song;
            check(song.playback->timeline.setLengthBars(maxBars) && song.playback->timeline.lengthBars == maxBars, "the timeline reaches 16384 bars");
            check(song.playback->timeline.place(region(0, 0, (maxBars - 1) * ticksPerBar, ticksPerBar, 0.25)), "place a clip in the last bar");
            check(song.playback->timeline.valid(song.project->trackCount(), song.project->sceneCount(), song.project->channelRack.patternCount),
                  "an 18-hour timeline is valid");
            song.playback->clips[0][0].audio = streamed != 0 ? &m->streamed : &m->decoded;
            finish(song);
            references[streamed] = render(song, lastBar);
        }
        check(references[0].frames == 96000 && energy(references[0], 0, 96000) > 0.1, "the last bar of an 18-hour song renders");
        check(maxDifference(references[0], references[1]) < 3.0e-7, "and a streamed clip there matches a decoded one");
    }

    std::cout << "Deterministic streamed export tests passed\n";
}
