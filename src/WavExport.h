#pragma once
#include "ToneEngine.h"
#include "SessionProject.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <limits>
#include <memory>
#include <filesystem>

namespace alien {
using DrumSamplePointers = std::array<std::array<const SessionAudioClip*, drumPadCount>, maxSessionTracks>;

// What to render besides the Session clips: the Channel Rack's sounds, and which part of the song.
struct WavExportOptions {
    std::int64_t fromTick{0};   // timeline ticks (960 to a quarter note)
    std::int64_t toTick{-1};    // -1: the end of the song
    std::array<const SessionAudioClip*, rack::maxChannels> rackClips{};
    bool stems{false};         // target is a new folder; capture aligned buses before master processing.
    int blockFrames{512};       // frames per render call (1..512); the result is the same for any size
};

// Owned sibling staging directory. Only this generated directory is cleaned on
// failure/cancellation; an existing destination is never intentionally replaced.
struct StemExportFolder {
    juce::File target, staging;
    bool created{};
    explicit StemExportFolder(juce::File destination) : target(std::move(destination)),
        staging(target.getSiblingFile(".alien-stems-" + juce::Uuid().toString())) {}
    ~StemExportFolder() { if (created) staging.deleteRecursively(); }
    bool begin() {
        if (target.exists() || !target.getParentDirectory().isDirectory()) return false;
        created = staging.createDirectory().wasOk(); return created;
    }
    bool publish() {
        if (target.exists()) return false;
        std::error_code error;
        std::filesystem::rename(std::filesystem::u8path(staging.getFullPathName().toStdString()),
            std::filesystem::u8path(target.getFullPathName().toStdString()), error);
        if (error) return false;
        created = false; return true;
    }
};
inline std::unique_ptr<juce::AudioFormatWriter> openExportWriter(const juce::File& file) {
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (!stream) return {};
    return juce::WavAudioFormat().createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
}

// How many 64-bit output frames `ticks` of timeline take at `bpm`: an 18-hour song is over three billion.
inline std::int64_t exportFrameCount(std::int64_t ticks, double bpm, double sampleRate) noexcept {
    if (ticks <= 0 || !std::isfinite(bpm) || bpm <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0) return 0;
    const long double frames = static_cast<long double>(ticks) * 60.0L * static_cast<long double>(sampleRate)
        / (static_cast<long double>(alien::timeline::ticksPerBeat) * static_cast<long double>(bpm));
    if (frames >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::ceil(frames - 1.0e-9L));
}

// Worker-only render, with an isolated engine using exactly the live DSP path.
// The caller retains all pointed-to sample assets until this function returns.
inline bool exportArrangementWav(const juce::File& target, const SessionProjectData& project,
    const ArrangementPlayback& livesong, const DrumSamplePointers& drums,
    const std::atomic<bool>& cancelled, juce::String& error, ToneEngine* preparedEngine = nullptr,
    const WavExportOptions& options = {}) {
    constexpr double exportRate = 48000.0;
    if (!project.valid() || !livesong.timeline.valid(project.trackCount(), project.sceneCount(), project.channelRack.patternCount)
        || !livesong.timeline.hasRegions()) {
        error="Add at least one clip or pattern to the Arrangement before exporting."; return false;
    }
    // The export plays the song through once from the requested start, never looping.
    // (A snapshot is megabytes: it lives on the heap, never on this thread's stack.)
    auto song = std::make_shared<ArrangementPlayback>(livesong);
    song->timeline.loopEnabled = false;
    song->timeline.tempo = project.bpm;
    const std::int64_t songEnd = song->timeline.length();
    const std::int64_t from = std::clamp<std::int64_t>(options.fromTick, 0, songEnd);
    const std::int64_t to = options.toTick < 0 ? songEnd : std::clamp<std::int64_t>(options.toTick, 0, songEnd);
    if (to <= from) { error="Nothing to export in that range."; return false; }

    // Open every streamed source this render will play before touching the destination, so a missing
    // file is reported up front. Each voice later opens its own reader, so a clip placed twice is read
    // independently each time.
    for (const auto& region : song->timeline.regions) {
        if (region.muted || region.row < 0 || region.row >= project.trackCount()
            || region.end() <= from || region.start >= to) continue;
        const auto* clip = song->clips[static_cast<std::size_t>(region.source)][static_cast<std::size_t>(region.row)].audio;
        if (clip == nullptr || !clip->streamed()) continue;
        std::unique_ptr<DiskAudioReader> reader;
        try { reader = clip->streamSource->valid() ? clip->streamSource->open() : nullptr; } catch (...) {}
        if (!reader) {
            error = "Cannot open streamed source for track " + juce::String(region.row + 1)
                + ", scene " + juce::String(region.source + 1)
                + ". The file may be missing or unreadable.";
            return false;
        }
    }
    bool hasPlugins = !options.stems && !project.masterPlugins.effects.empty();
    for (const auto& rack : project.returnPlugins) hasPlugins = hasPlugins || !rack.effects.empty();
    for (const auto& rack : project.plugins)
        hasPlugins = hasPlugins || !rack.instrument.descriptionXml.isEmpty() || !rack.effects.empty();
    if (hasPlugins && !preparedEngine) {
        error="Prepare plugin instances on the message thread before export"; return false;
    }
    auto ownedEngine = preparedEngine ? std::unique_ptr<ToneEngine>{} : std::make_unique<ToneEngine>();
    auto* engine = preparedEngine ? preparedEngine : ownedEngine.get();
    struct RestoreMasterBypass {
        PluginRuntime::Rack& rack; std::array<bool,8> previous{};
        explicit RestoreMasterBypass(PluginRuntime::Rack& value) : rack(value) {
            for (std::size_t i=0; i<previous.size(); ++i) previous[i]=rack.effects[i].bypass;
        }
        ~RestoreMasterBypass() { for (std::size_t i=0; i<previous.size(); ++i) rack.effects[i].bypass=previous[i]; }
    } restoreMaster(engine->plugins->racks[PluginRuntime::masterIndex]);
    if (options.stems) {
        engine->stemCapture = std::make_unique<StemCapture>();
        for (auto& slot : engine->plugins->racks[PluginRuntime::masterIndex].effects) slot.bypass = true;
    } else engine->stemCapture.reset();
    const auto faultsBefore = engine->plugins->faultCount.load();
    engine->renderStreamsDeterministically();   // streamed clips are read here, on this thread, in order
    engine->masterControls.gain.store(project.masterGain);
    for (std::size_t r = 0; r < engine->returns.size(); ++r) engine->returns[r].set(project.returns[r]);
    engine->prepare(exportRate);
    engine->recording.metronome.store(false);
    for (auto& rack : engine->plugins->racks) {
        if (rack.instrument.processor) rack.instrument.processor->setNonRealtime(true);
        for (auto& slot : rack.effects)
            if (slot.processor) slot.processor->setNonRealtime(true);
    }
    engine->activeTracks.store(project.trackCount());
    engine->bpm.store(project.bpm);
    engine->patternMode.store(2);
    for (std::size_t t=0;t<static_cast<std::size_t>(project.trackCount());++t) {
        engine->tracks[t].set(project.trackSettings[t]);
        for (std::size_t p=0;p<drumPadCount;++p)
            engine->drumKits[t].samples[p].store(drums[t][p]);
    }
    if(!engine->arrangementMailbox.publish(std::shared_ptr<const ArrangementPlayback>(song))) { error="Invalid song data.";return false; }
    for (std::size_t c = 0; c < options.rackClips.size(); ++c) engine->rackSamples[c].store(options.rackClips[c]);
    engine->rackMailbox.publish(project.channelRack.snapshot());
    engine->timelineSeek.store(from);
    engine->timelineEnd.store(to < songEnd ? to : -1);
    engine->playing.store(true);

    // Declare the folder before its writers so failure closes handles before cleanup.
    std::unique_ptr<StemExportFolder> folder;
    std::unique_ptr<juce::TemporaryFile> temporary;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> stemWriters;
    std::vector<int> stemBuses;
    if (options.stems) {
        folder = std::make_unique<StemExportFolder>(target);
        if (!folder->begin()) { error="Choose a new stem folder inside an existing directory."; return false; }
        auto add = [&](int bus, const juce::String& name) {
            auto output = openExportWriter(folder->staging.getChildFile(name));
            if (!output) return false;
            stemBuses.push_back(bus); stemWriters.push_back(std::move(output)); return true;
        };
        for (int t=0; t<project.trackCount(); ++t) {
            auto name = juce::File::createLegalFileName(project.trackNames[static_cast<std::size_t>(t)]).substring(0,64);
            if (name.isEmpty()) name="Track";
            if (!add(t, juce::String(t+1).paddedLeft('0',2) + " - " + name + ".wav")) {
                error="Cannot create track stem."; return false;
            }
        }
        for (int r=0; r<maxReturns; ++r)
            if (!add(StemCapture::returnBase+r, "Return " + juce::String::charToString(static_cast<juce::juce_wchar>('A'+r)) + ".wav")) {
                error="Cannot create return stem."; return false;
            }
        if (!add(StemCapture::directBus,"Channel Rack - Master.wav")) { error="Cannot create rack stem."; return false; }
    } else {
        temporary=std::make_unique<juce::TemporaryFile>(target);
        writer=openExportWriter(temporary->getFile());
        if (!writer) { error="Cannot create WAV writer."; return false; }
    }

    const int block = std::clamp(options.blockFrames, 1, 512);
    juce::AudioBuffer<float> buffer(2,block);
    const auto latency = static_cast<std::int64_t>(engine->plugins->latencySamples()
        - (options.stems ? engine->plugins->rackLatency(engine->plugins->racks[PluginRuntime::masterIndex]) : 0));
    const auto tail = static_cast<std::int64_t>(std::ceil(engine->plugins->tailSeconds() * exportRate));
    const auto frames = exportFrameCount(to - from, project.bpm, exportRate);
    if (frames <= 0 || frames > std::numeric_limits<std::int64_t>::max() - latency - tail) {
        error="Export duration is too long."; return false;
    }
    const auto total=frames+latency+tail;
    for(std::int64_t offset=0;offset<total;offset+=block) {
        if(cancelled.load()) {
            error=options.stems ? "Stem export cancelled; no folder published." : "Export cancelled; existing file unchanged.";return false;
        }
        const auto count=static_cast<int>(std::min<std::int64_t>(block,total-offset));
        buffer.clear();
        float* outputs[]{buffer.getWritePointer(0),buffer.getWritePointer(1)};
        engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,count,{});
        if (cancelled.load()) {
            error=options.stems ? "Stem export cancelled; no folder published." : "Export cancelled; existing file unchanged.";return false;
        }
        // Rendering offline never waits for a stream, so a stream that cannot supply a frame has failed.
        for (std::size_t t = 0; t < static_cast<std::size_t>(project.trackCount()); ++t)
            if (engine->sessionStreamFailed(t) || engine->sessionStreamUnderruns(t) != 0) {
                error="Reading streamed source failed for track " + juce::String(static_cast<int>(t) + 1) + ".";
                return false;
            }
        const int start=static_cast<int>(std::clamp<std::int64_t>(latency-offset,0,count));
        if (options.stems && engine->plugins->faultCount.load() != faultsBefore) {
            error="Plugin failed during stem export; no folder published."; return false;
        }
        if (count > start) {
            if (options.stems) {
                for (std::size_t b=0; b<stemWriters.size(); ++b) {
                    const auto& source = engine->stemCapture->buses[static_cast<std::size_t>(stemBuses[b])];
                    if (!allFinite(source.getReadPointer(0)+start,count-start)
                        || !allFinite(source.getReadPointer(1)+start,count-start)
                        || !stemWriters[b]->writeFromAudioSampleBuffer(source,start,count-start)) {
                        error="Stem write failed (invalid audio or unavailable disk)."; return false;
                    }
                }
            } else if (!writer->writeFromAudioSampleBuffer(buffer,start,count-start)) {
                error="WAV write failed (check available space)."; return false;
            }
        }
    }
    if (options.stems) {
        for (auto& output : stemWriters) if (!output->flush()) { error="Stem flush failed."; return false; }
        stemWriters.clear();
        if (!folder->staging.getChildFile("README.txt").replaceWithText(
            "Alien Studio stems: 48 kHz, 24-bit stereo. All files share the same start and length.\n"
            "Track files include track FX, faders, pan and volume/pan automation. Returns are separate files.\n"
            "Channel Rack - Master contains channels routed directly to master. Muting/soloing follows the project.\n"
            "Master FX, master gain and final limiter are excluded. Sum reconstructs the pre-master bus, subject to 24-bit quantization/clipping.\n"
            "Plugin tails use the host's existing 30-second maximum. Silence is retained for alignment.\n")) {
            error="Cannot write stem notes."; return false;
        }
    } else {
        if(!writer->flush()) { error="WAV flush failed.";return false; }
        writer.reset();
    }
    if(cancelled.load()) {
        error=options.stems ? "Stem export cancelled; no folder published." : "Export cancelled; existing file unchanged.";return false;
    }
    if (options.stems ? !folder->publish() : !temporary->overwriteTargetFileWithTemporary()) {
        error=options.stems ? "Cannot publish stem folder; destination may already exist." : "Cannot replace destination WAV.";return false;
    }
    error.clear();return true;
}
} // namespace alien
