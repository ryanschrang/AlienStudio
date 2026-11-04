#pragma once
#include "SessionAudioClip.h"
#include <juce_audio_formats/juce_audio_formats.h>
namespace alien {
inline bool writeClipWav(const juce::File& target,const SessionAudioClip& clip,
    const std::atomic<bool>& cancelled,juce::String& error) {
    error.clear();
    if(!clip.valid()) {error="Invalid audio take.";return false;}
    juce::TemporaryFile temp(target);
    std::unique_ptr<juce::OutputStream> stream=temp.getFile().createOutputStream();
    if(!stream){error="Cannot create audio file.";return false;}
    juce::WavAudioFormat format;
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(clip.sampleRate).withNumChannels(clip.channels).withBitsPerSample(24));
    if(!writer){error="Cannot create WAV writer.";return false;}
    for(std::size_t offset=0;offset<clip.frames;offset+=512) {
        if(cancelled.load()){error="Cancelled. Existing file unchanged.";return false;}
        const auto& source = clip.sliceSource ? *clip.sliceSource : clip;
        const auto begin = offset + (clip.sliceSource ? clip.sliceOffset : 0);
        const float* data[]{source.left.data()+begin,clip.channels==2?source.right.data()+begin:nullptr};
        if(!writer->writeFromFloatArrays(data,clip.channels,static_cast<int>(std::min<std::size_t>(512,clip.frames-offset)))){error="Audio write failed.";return false;}
    }
    if(!writer->flush()){error="Audio flush failed.";return false;}writer.reset();
    if(cancelled.load()){error="Cancelled. Existing file unchanged.";return false;}
    if(!temp.overwriteTargetFileWithTemporary()){error="Cannot save destination WAV.";return false;}
    return true;
}
} // namespace alien
