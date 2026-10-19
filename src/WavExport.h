#pragma once
#include "ToneEngine.h"
#include "SessionProject.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace alien {
using DrumSamplePointers = std::array<std::array<const SessionAudioClip*, drumPadCount>,4>;

// Worker-only render, with an isolated engine using exactly the live DSP path.
// The caller retains all pointed-to sample assets until this function returns.
inline bool exportArrangementWav(const juce::File& target, const SessionProjectData& project,
    const ArrangementPlayback& song, const DrumSamplePointers& drums,
    const std::atomic<bool>& cancelled, juce::String& error) {
    if (!project.valid() || !song.arrangement.valid() || song.arrangement.count == 0) {
        error="Add at least one valid region before exporting."; return false;
    }
    auto engine=std::make_unique<ToneEngine>();
    engine->prepare(48000); engine->bpm.store(project.bpm); engine->patternMode.store(2);
    for (std::size_t t=0;t<4;++t) {
        engine->tracks[t].set(project.trackSettings[t]);
        for (std::size_t p=0;p<drumPadCount;++p) engine->drumKits[t].samples[p].store(drums[t][p]);
    }
    if(!engine->arrangementMailbox.publish(song)) { error="Invalid song data.";return false; }
    engine->playing.store(true);
    juce::TemporaryFile temporary(target);
    std::unique_ptr<juce::OutputStream> stream=temporary.getFile().createOutputStream();
    if(!stream) { error="Cannot create WAV file.";return false; }
    juce::WavAudioFormat format;
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    if(!writer) { error="Cannot create WAV writer.";return false; }
    juce::AudioBuffer<float> buffer(2,512);
    const auto frames=static_cast<std::int64_t>(std::ceil(song.arrangement.lengthBars * 4.0 * 60.0 * 48000.0 / project.bpm));
    for(std::int64_t offset=0;offset<frames;offset+=512) {
        if(cancelled.load()) { error="Export cancelled; existing file unchanged.";return false; }
        const auto count=static_cast<int>(std::min<std::int64_t>(512,frames-offset));
        float* outputs[]{buffer.getWritePointer(0),buffer.getWritePointer(1)};
        engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,count,{});
        if(!writer->writeFromAudioSampleBuffer(buffer,0,count)) { error="WAV write failed (check available space).";return false; }
    }
    if(!writer->flush()) { error="WAV flush failed.";return false; }
    writer.reset();
    if(cancelled.load()) { error="Export cancelled; existing file unchanged.";return false; }
    if(!temporary.overwriteTargetFileWithTemporary()) { error="Cannot replace destination WAV.";return false; }
    error.clear();return true;
}
} // namespace alien
