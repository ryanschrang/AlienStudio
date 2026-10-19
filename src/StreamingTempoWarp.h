#pragma once
#include "DiskRecording.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace alien {
// Offline, linked-stereo WSOLA. Only a source search region and one output
// grain are resident; positions and duration are 64-bit throughout.
template<class Read, class Write>
bool streamTempoWarp(std::int64_t length,double rate,int channels,double ratio,
                     const std::atomic<bool>& cancel,Read&& read,Write&& write) {
    if(length<=0 || !std::isfinite(rate) || rate<8000 || rate>192000 || channels<1 || channels>2
        || !std::isfinite(ratio) || ratio<0.5 || ratio>2 || cancel.load()
        || static_cast<long double>(length)*ratio>static_cast<long double>(INT64_MAX)-8192)return false;
    const auto total=static_cast<std::int64_t>(std::ceil(static_cast<double>(length)*ratio));
    const int grain=std::max(64,static_cast<int>(rate*0.04)),hop=grain/4,seek=std::max(8,static_cast<int>(rate*0.006));
    const int region=grain+2*seek;
    juce::AudioBuffer<float> source(channels,region),previous(channels,grain),sum(channels,grain);
    std::vector<float> weights(grain),window(grain);
    sum.clear();previous.clear();
    for(int n=0;n<grain;++n)window[n]=static_cast<float>(0.5-0.5*std::cos(6.283185307179586*(n+0.5)/grain));
    auto load=[&](juce::AudioBuffer<float>& buffer,std::int64_t start,int count){
        buffer.clear();const int valid=static_cast<int>(std::min<std::int64_t>(count,std::max<std::int64_t>(0,length-start)));
        if(valid>0 && !read(buffer.getArrayOfWritePointers(),channels,start,valid))return false;
        for(int c=0;c<channels;++c)for(int n=0;n<valid;++n)if(!std::isfinite(buffer.getSample(c,n)))buffer.setSample(c,n,0);
        return true;
    };
    // Exact ratio-one copy avoids WSOLA coloration and avoids search work.
    if(ratio==1) {
        for(std::int64_t pos=0;pos<length;) {
            if(cancel.load())return false;
            const int count=static_cast<int>(std::min<std::int64_t>(region,length-pos));
            if(!load(source,pos,count) || !write(source.getArrayOfReadPointers(),channels,count))return false;
            pos+=count;
        }return !cancel.load();
    }
    for(std::int64_t pos=0;pos<total;pos+=hop) {
        if(cancel.load())return false;
        const auto expected=static_cast<std::int64_t>(pos/ratio);
        const auto start=std::max<std::int64_t>(0,expected-seek);
        if(!load(source,start,region))return false;
        int best=static_cast<int>(expected-start);
        if(pos>0) {
            double bestScore=-2;
            for(int candidate=0;candidate<=static_cast<int>(expected+seek-start);++candidate) {
                if((candidate&63)==0 && cancel.load())return false;
                double dot=0,aa=0,bb=0;
                for(int c=0;c<channels;++c)for(int n=0;n<grain-hop;n+=8) {
                    const double a=previous.getSample(c,hop+n),b=source.getSample(c,candidate+n);
                    dot+=a*b;aa+=a*a;bb+=b*b;
                }
                const double score=dot/std::sqrt(aa*bb+1.0e-20)-1.0e-6*std::abs(start+candidate-expected);
                if(score>bestScore){bestScore=score;best=candidate;}
            }
        }
        for(int n=0;n<grain;++n) {
            weights[n]+=window[n];
            for(int c=0;c<channels;++c) {
                const float value=source.getSample(c,best+n);
                previous.setSample(c,n,value);sum.addSample(c,n,window[n]*value);
            }
        }
        const int count=static_cast<int>(std::min<std::int64_t>(hop,total-pos));
        for(int c=0;c<channels;++c)for(int n=0;n<count;++n)if(weights[n]>0)sum.setSample(c,n,sum.getSample(c,n)/weights[n]);
        if(!write(sum.getArrayOfReadPointers(),channels,count))return false;
        for(int c=0;c<channels;++c){auto* data=sum.getWritePointer(c);std::memmove(data,data+hop,(grain-hop)*sizeof(float));std::fill(data+grain-hop,data+grain,0);}
        std::move(weights.begin()+hop,weights.end(),weights.begin());std::fill(weights.end()-hop,weights.end(),0);
    }
    return !cancel.load();
}

inline RecordingResult warpTempoFile(const juce::File& source,const juce::File& destination,double ratio,
                                    const std::atomic<bool>& cancel) {
    RecordingResult result;
    juce::AudioFormatManager formats;formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if(!reader){result.error="Cannot read tempo-warp source audio";return result;}
    if(reader->numChannels<1 || reader->numChannels>2 || reader->lengthInSamples<=0
        || !std::isfinite(ratio) || ratio<0.5 || ratio>2){result.error="Use mono/stereo audio and a duration ratio between 0.5 and 2";return result;}
    auto output=RecordingFile::create(destination,reader->sampleRate,static_cast<int>(reader->numChannels),result.error);
    if(!output)return result;
    std::uint64_t written=0,checkpoint=0;
    const bool ok=streamTempoWarp(reader->lengthInSamples,reader->sampleRate,static_cast<int>(reader->numChannels),ratio,cancel,
        [&](float* const* data,int channels,std::int64_t start,int count){return reader->read(data,channels,start,count);},
        [&](const float* const* data,int channels,int count){
            // RecordingFile encodes at most 4096 frames per call.
            for(int offset=0;offset<count;offset+=4096){const float* block[]{data[0]+offset,channels==2?data[1]+offset:nullptr};
                const int n=std::min(4096,count-offset);if(!output->write(block,channels,n))return false;written+=n;}
            if(written-checkpoint>=static_cast<std::uint64_t>(reader->sampleRate)){checkpoint=written;return output->checkpoint();}return true;
        });
    if(cancel.load()){output->discard();result.error="Tempo render cancelled; source preserved";return result;}
    result=output->finish(!ok);
    if(!ok)result.error="Tempo render failed reading or writing audio. Partial output retained for recovery. "+result.error;
    return result;
}
}
