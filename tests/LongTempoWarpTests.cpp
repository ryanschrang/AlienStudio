#include "StreamingTempoWarp.h"
#include "TempoWarp.h"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    std::atomic<bool> cancel{};
    // Compare the bounded renderer against the original whole-clip algorithm.
    alien::SessionAudioClip clip;clip.sampleRate=8000;clip.channels=2;clip.frames=12000;
    clip.left.resize(clip.frames);clip.right.resize(clip.frames);
    for(std::size_t n=0;n<clip.frames;++n){clip.left[n]=0.4f*std::sin(n*0.13);clip.right[n]=-clip.left[n];}clip.buildWaveform();
    for(double ratio:{0.5,0.8,1.0,1.5,2.0}){
        auto expected=alien::warpTempo(clip,ratio,cancel);check(bool(expected),"reference render");
        std::vector<float> left,right;
        check(alien::streamTempoWarp(clip.frames,8000,2,ratio,cancel,
            [&](float* const* data,int,std::int64_t start,int count){std::copy_n(clip.left.data()+start,count,data[0]);std::copy_n(clip.right.data()+start,count,data[1]);return true;},
            [&](const float* const* data,int,int count){left.insert(left.end(),data[0],data[0]+count);right.insert(right.end(),data[1],data[1]+count);return true;}),"stream render");
        check(left.size()==expected->frames,"exact output duration");
        for(std::size_t n=0;n<left.size();++n){check(std::abs(left[n]-expected->left[n])<0.00001f,"reference agreement across grain boundaries");check(std::abs(right[n]+left[n])<0.00001f,"linked antiphase stereo");}
    }
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienWarpTests","");root.createDirectory();
    juce::String error;const auto source=root.getChildFile("Long.wav"),target=root.getChildFile("Warped.wav");
    auto input=alien::RecordingFile::create(source,8000,1,error,root.getChildFile("Recovery"));check(bool(input),"source create");
    float samples[4096];for(int n=0;n<4096;++n)samples[n]=0.25f*std::sin(n*0.1);const float* data[]{samples};
    for(int n=0;n<245;++n)check(input->write(data,1,4096),"long source write");check(input->finish(false).file.existsAsFile(),"source finish");
    auto result=alien::warpTempoFile(source,target,0.5,cancel);check(result.error.isEmpty() && result.writtenFrames==245*2048,"125-second file warp");
    juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(target.createInputStream().release(),true));
    check(reader && reader->lengthInSamples==245*2048 && reader->numChannels==1,"output WAV metadata");
    check(!alien::warpTempoFile(source,target,1,cancel).error.isEmpty(),"existing output preserved");
    const auto highSource=root.getChildFile("192k.wav"),highTarget=root.getChildFile("192k-copy.wav");
    auto high=alien::RecordingFile::create(highSource,192000,1,error,root.getChildFile("Recovery"));check(bool(high),"high rate source");
    for(int n=0;n<7;++n)check(high->write(data,1,4096),"high rate write");high->finish(false);
    const auto highResult=alien::warpTempoFile(highSource,highTarget,1,cancel);
    check(highResult.error.isEmpty() && highResult.writtenFrames==7*4096,"large source read blocks split into writer blocks");
    cancel=true;const auto cancelled=root.getChildFile("Cancelled.wav");check(!alien::warpTempoFile(source,cancelled,1,cancel).error.isEmpty() && !cancelled.exists(),"cancel does not publish output");
    cancel=false;int calls=0;
    check(!alien::streamTempoWarp(100000,8000,1,0.8,cancel,[](float* const*,int,std::int64_t,int){return false;},[](const float* const*,int,int){return true;}),"read failure surfaced");
    check(!alien::streamTempoWarp(100000,8000,1,0.8,cancel,[](float* const* p,int,std::int64_t,int n){std::fill_n(p[0],n,0);return true;},[](const float* const*,int,int){return false;}),"write failure surfaced");
    // More than the Session decoded-memory limit, without a resident clip.
    std::int64_t copied=0;int largestRead=0;
    check(alien::streamTempoWarp(36000000,8000,1,1,cancel,
        [&](float* const* p,int,std::int64_t,int n){largestRead=std::max(largestRead,n);std::fill_n(p[0],n,0.2f);return true;},
        [&](const float* const* p,int,int n){check(p[0][n-1]==0.2f,"long copy samples");copied+=n;return true;}),"source larger than Session RAM budget");
    check(copied==36000000 && largestRead<1024,"long copy uses bounded blocks");
    // Virtual 64-bit source: cancellation after a few grains bounds the work.
    check(!alien::streamTempoWarp(5000000000LL,8000,1,1.5,cancel,[](float* const* p,int,std::int64_t,int n){std::fill_n(p[0],n,0);return true;},[&](const float* const*,int,int){if(++calls==4)cancel=true;return true;}),"long source cancellation");
    check(calls==4,"bounded cancellation response");
    root.deleteRecursively();std::cout<<"Long tempo warp, reference agreement, stereo, file output, failures and cancellation passed\n";
}
