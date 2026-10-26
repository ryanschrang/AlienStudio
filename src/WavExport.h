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
    int freezeTrack{-1};       // >=0: one post-FX, pre-fader Session track WAV.
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
inline std::unique_ptr<juce::AudioFormatWriter> openExportWriter(const juce::File& file, bool floatingPoint=false) {
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (!stream) return {};
    return juce::WavAudioFormat().createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(48000).withNumChannels(2).withBitsPerSample(floatingPoint?32:24)
        .withSampleFormat(floatingPoint?juce::AudioFormatWriterOptions::SampleFormat::floatingPoint:juce::AudioFormatWriterOptions::SampleFormat::automatic));
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
// The same on a song with tempo changes: frames from tick `from` to tick `to`.
inline std::int64_t exportFrameCount(const timeline::TempoMap& tempo, std::int64_t from, std::int64_t to, double sampleRate) noexcept {
    if (to <= from || !std::isfinite(sampleRate) || sampleRate <= 0) return 0;
    const long double frames = static_cast<long double>(tempo.secondsBetween(static_cast<double>(from), static_cast<double>(to)))
        * static_cast<long double>(sampleRate);
    if (!(frames >= 0.0L)) return 0;
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
    const bool freezing=options.freezeTrack>=0;
    const bool capture=options.stems || freezing;
    if(options.freezeTrack < -1 || options.freezeTrack>=project.trackCount() || (freezing && options.stems)
        || (freezing && project.trackSettings[static_cast<std::size_t>(options.freezeTrack)].frozen())) {
        error="Invalid freeze target.";return false;
    }
    if (!project.valid() || !livesong.timeline.valid(project.trackCount(), project.sceneCount(), project.channelRack.patternCount)
        || !livesong.timeline.hasRegions()) {
        error="Add at least one clip or pattern to the Arrangement before exporting."; return false;
    }
    // The export plays the song through once from the requested start, never looping.
    // (A snapshot is megabytes: it lives on the heap, never on this thread's stack.)
    auto song = std::make_shared<ArrangementPlayback>(livesong);
    if(freezing)std::erase_if(song->timeline.regions,[&](const timeline::Region& region){return region.row!=options.freezeTrack;});
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
    bool hasPlugins = !capture && !project.masterPlugins.effects.empty();
    if(!freezing)for (const auto& rack : project.returnPlugins) hasPlugins = hasPlugins || !rack.effects.empty();
    if(!freezing)for (const auto& rack : project.busPlugins) hasPlugins = hasPlugins || !rack.effects.empty();

    for (std::size_t t=0;t<project.plugins.size();++t) {
        if(project.trackSettings[t].frozen() || (freezing && static_cast<int>(t)!=options.freezeTrack))continue;
        const auto& rack=project.plugins[t];hasPlugins=hasPlugins || !rack.instrument.descriptionXml.isEmpty() || !rack.effects.empty();
    }
    if (hasPlugins && !preparedEngine) {
        error="Prepare plugin instances on the message thread before export"; return false;
    }
    auto ownedEngine = preparedEngine ? std::unique_ptr<ToneEngine>{} : std::make_unique<ToneEngine>();
    auto* engine = preparedEngine ? preparedEngine : ownedEngine.get();
    struct RestoreOfflineFlags {
        PluginRuntime& runtime;
        std::array<bool,PluginRuntime::rackCount> frozen{};
        std::array<std::array<bool,8>,PluginRuntime::rackCount> bypass{};
        explicit RestoreOfflineFlags(PluginRuntime& owner) : runtime(owner) {
            for(std::size_t r=0;r<frozen.size();++r){frozen[r]=runtime.racks[r].frozen;for(std::size_t i=0;i<8;++i)bypass[r][i]=runtime.racks[r].effects[i].bypass;}
        }
        ~RestoreOfflineFlags(){for(std::size_t r=0;r<frozen.size();++r){runtime.racks[r].frozen=frozen[r];for(std::size_t i=0;i<8;++i)runtime.racks[r].effects[i].bypass=bypass[r][i];}}
    } restoreFlags(*engine->plugins);
    if (capture) {
        engine->stemCapture = std::make_unique<StemCapture>();engine->stemCapture->freezeTrack=options.freezeTrack;
        if(freezing) {
            for(int t=0;t<maxSessionTracks;++t)if(t!=options.freezeTrack)engine->plugins->racks[static_cast<std::size_t>(t)].frozen=true;
            for(int k=0;k<maxReturns;++k)engine->plugins->racks[PluginRuntime::returnIndex(k)].frozen=true;
            for(int b=0;b<maxBuses;++b)engine->plugins->racks[PluginRuntime::busIndex(b)].frozen=true;

        }
        for (auto& slot : engine->plugins->racks[PluginRuntime::masterIndex].effects) slot.bypass = true;
    } else engine->stemCapture.reset();
    const auto faultsBefore = engine->plugins->faultCount.load();
    engine->renderStreamsDeterministically();   // streamed clips are read here, on this thread, in order
    engine->masterControls.gain.store(project.masterGain);
    for (std::size_t r = 0; r < engine->returns.size(); ++r) engine->returns[r].set(project.returns[r]);
    // A freeze renders one track on its own, so it goes through no bus.
    for (std::size_t b = 0; b < engine->buses.size(); ++b) {
        engine->buses[b].set(project.buses[b].settings);
        engine->buses[b].active.store(!freezing && project.buses[b].used);
    }

    for(int t=0;t<project.trackCount();++t)engine->plugins->racks[static_cast<std::size_t>(t)].frozen=
        project.trackSettings[static_cast<std::size_t>(t)].frozen() || (freezing && t!=options.freezeTrack);
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
        auto settings=project.trackSettings[t];
        if(freezing){ settings.send.fill(0);settings.mute=false;settings.solo=false;settings.output=-1;if(static_cast<int>(t)!=options.freezeTrack)settings.freezeScene=0; }

        engine->tracks[t].set(settings);
        for (std::size_t p=0;p<drumPadCount;++p)
            engine->drumKits[t].samples[p].store(drums[t][p]);
    }
    double tailSeconds=engine->plugins->tailSeconds();
    if(freezing) {
        if(project.plugins[static_cast<std::size_t>(options.freezeTrack)].instrument.descriptionXml.isEmpty()) {
            const auto& settings=project.trackSettings[static_cast<std::size_t>(options.freezeTrack)];
            double sourceTail=settings.instrument==0?presets[static_cast<std::size_t>(settings.preset)].release:0.0;
            if(settings.instrument==1)for(const auto* sample:drums[static_cast<std::size_t>(options.freezeTrack)])
                if(sample && sample->sampleRate>0)sourceTail=std::max(sourceTail,static_cast<double>(sample->frames)/sample->sampleRate);
            tailSeconds+=sourceTail;
        }
        tailSeconds=std::min(30.0,tailSeconds);
        const auto tailTicks=static_cast<std::int64_t>(std::ceil(song->timeline.tickAfter(static_cast<double>(to),tailSeconds)-static_cast<double>(to)));
        const auto end=to+tailTicks+timeline::ticksPerStep;
        if(end>timeline::maxLength){error="Frozen tail exceeds the supported song length.";return false;}
        song->timeline.lengthBars=std::max(song->timeline.lengthBars,static_cast<int>(song->timeline.meterMap().barsCovering(end)));
    }
    if(!engine->arrangementMailbox.publish(std::shared_ptr<const ArrangementPlayback>(song))) { error="Invalid song data.";return false; }
    for (std::size_t c = 0; c < options.rackClips.size(); ++c) engine->rackSamples[c].store(options.rackClips[c]);
    engine->rackMailbox.publish(project.channelRack.snapshot());
    engine->timelineSeek.store(from);
    engine->timelineEnd.store(!freezing && to < songEnd ? to : -1);
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
            // A track that goes to a group bus says so: its sound is in that bus's file too.
            const int output = project.trackSettings[static_cast<std::size_t>(t)].output;
            if (output >= 0) name += " (into " + juce::File::createLegalFileName(project.buses[static_cast<std::size_t>(output)].name).substring(0,64) + ")";
            if (!add(t, juce::String(t+1).paddedLeft('0',2) + " - " + name + ".wav")) {
                error="Cannot create track stem."; return false;
            }
        }
        for (int b=0; b<maxBuses; ++b) {
            if (!project.buses[static_cast<std::size_t>(b)].used) continue;
            auto name = juce::File::createLegalFileName(project.buses[static_cast<std::size_t>(b)].name).substring(0,64);
            if (name.isEmpty()) name="Bus";
            if (!add(StemCapture::busBase+b, "Bus " + juce::String(b+1) + " - " + name + ".wav")) {
                error="Cannot create bus stem."; return false;
            }
        }

        for (int r=0; r<maxReturns; ++r)
            if (!add(StemCapture::returnBase+r, "Return " + juce::String::charToString(static_cast<juce::juce_wchar>('A'+r)) + ".wav")) {
                error="Cannot create return stem."; return false;
            }
        if (!add(StemCapture::directBus,"Channel Rack - Master.wav")) { error="Cannot create rack stem."; return false; }
    } else {
        temporary=std::make_unique<juce::TemporaryFile>(target);
        writer=openExportWriter(temporary->getFile(),freezing);
        if (!writer) { error="Cannot create WAV writer."; return false; }
    }

    const int block = std::clamp(options.blockFrames, 1, 512);
    juce::AudioBuffer<float> buffer(2,block);
    const auto latency = static_cast<std::int64_t>(engine->plugins->latencySamples()
        - (options.stems ? engine->plugins->rackLatency(engine->plugins->racks[PluginRuntime::masterIndex]) : 0));
    const auto tail = static_cast<std::int64_t>(std::ceil(tailSeconds * exportRate));
    const auto frames = exportFrameCount(song->timeline.tempoMap(), from, to, exportRate);
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
        if(freezing)for(int c=0;c<2;++c)buffer.copyFrom(c,0,engine->stemCapture->buses[static_cast<std::size_t>(options.freezeTrack)],c,0,count);
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
        if (capture && engine->plugins->faultCount.load() != faultsBefore) {
            error="Plugin failed during offline capture; no output published."; return false;
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
            } else if ((freezing && (!allFinite(buffer.getReadPointer(0)+start,count-start) || !allFinite(buffer.getReadPointer(1)+start,count-start)))
                || !writer->writeFromAudioSampleBuffer(buffer,start,count-start)) {
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
            "Group buses are separate files (\"Bus N - name\") after their FX and fader. A track marked \"(into name)\" goes to that bus,\n"
            "so its sound is also inside the bus file; leave it out when summing.\n"
            "Channel Rack - Master contains channels routed directly to master. Muting/soloing follows the project.\n"
            "Master FX, master gain and final limiter are excluded. The sum of the bus files, the tracks not marked \"(into ...)\", the returns\n"
            "and Channel Rack - Master reconstructs the pre-master bus, subject to 24-bit quantization/clipping.\n"

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
