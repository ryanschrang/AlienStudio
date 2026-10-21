#pragma once
#include "ToneEngine.h"
#include "SessionProject.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <limits>
#include <memory>

namespace alien {
using DrumSamplePointers = std::array<std::array<const SessionAudioClip*, drumPadCount>, maxSessionTracks>;

// What to render besides the Session clips: the Channel Rack's sounds, and which part of the song.
struct WavExportOptions {
    std::int64_t fromTick{0};   // timeline ticks (960 to a quarter note)
    std::int64_t toTick{-1};    // -1: the end of the song
    std::array<const SessionAudioClip*, rack::maxChannels> rackClips{};
    int blockFrames{512};       // frames per render call (1..512); the result is the same for any size
};

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
    bool hasPlugins = !project.masterPlugins.effects.empty();
    for (const auto& rack : project.plugins)
        hasPlugins = hasPlugins || !rack.instrument.descriptionXml.isEmpty() || !rack.effects.empty();
    if (hasPlugins && !preparedEngine) {
        error="Prepare plugin instances on the message thread before export"; return false;
    }
    auto ownedEngine = preparedEngine ? std::unique_ptr<ToneEngine>{} : std::make_unique<ToneEngine>();
    auto* engine = preparedEngine ? preparedEngine : ownedEngine.get();
    engine->renderStreamsDeterministically();   // streamed clips are read here, on this thread, in order
    engine->masterControls.gain.store(project.masterGain);
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

    juce::TemporaryFile temporary(target);
    std::unique_ptr<juce::OutputStream> stream=temporary.getFile().createOutputStream();
    if(!stream) { error="Cannot create WAV file.";return false; }
    juce::WavAudioFormat format;
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}
        .withSampleRate(exportRate).withNumChannels(2).withBitsPerSample(24));
    if(!writer) { error="Cannot create WAV writer.";return false; }

    const int block = std::clamp(options.blockFrames, 1, 512);
    juce::AudioBuffer<float> buffer(2,block);
    const auto latency = static_cast<std::int64_t>(engine->plugins->latencySamples());
    const auto tail = static_cast<std::int64_t>(std::ceil(engine->plugins->tailSeconds() * exportRate));
    const auto frames = exportFrameCount(to - from, project.bpm, exportRate);
    if (frames <= 0 || frames > std::numeric_limits<std::int64_t>::max() - latency - tail) {
        error="Export duration is too long."; return false;
    }
    const auto total=frames+latency+tail;
    for(std::int64_t offset=0;offset<total;offset+=block) {
        if(cancelled.load()) {
            error="Export cancelled; existing file unchanged.";return false;
        }
        const auto count=static_cast<int>(std::min<std::int64_t>(block,total-offset));
        buffer.clear();
        float* outputs[]{buffer.getWritePointer(0),buffer.getWritePointer(1)};
        engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,count,{});
        if (cancelled.load()) {
            error="Export cancelled; existing file unchanged.";return false;
        }
        // Rendering offline never waits for a stream, so a stream that cannot supply a frame has failed.
        for (std::size_t t = 0; t < static_cast<std::size_t>(project.trackCount()); ++t)
            if (engine->sessionStreamFailed(t) || engine->sessionStreamUnderruns(t) != 0) {
                error="Reading streamed source failed for track " + juce::String(static_cast<int>(t) + 1) + ".";
                return false;
            }
        const int start=static_cast<int>(std::clamp<std::int64_t>(latency-offset,0,count));
        if(count>start && !writer->writeFromAudioSampleBuffer(buffer,start,count-start)) {
            error="WAV write failed (check available space).";return false;
        }
    }
    if(!writer->flush()) { error="WAV flush failed.";return false; }
    writer.reset();
    if(cancelled.load()) {
        error="Export cancelled; existing file unchanged.";return false;
    }
    if(!temporary.overwriteTargetFileWithTemporary()) {
        error="Cannot replace destination WAV.";return false;
    }
    error.clear();return true;
}
} // namespace alien
