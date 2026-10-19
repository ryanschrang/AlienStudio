#include "WavExport.h"
#include <cstdlib>
#include <iostream>
static void check(bool ok,const char* message) { if(!ok){std::cerr<<message<<'\n';std::exit(1);} }
int main() {
    alien::Arrangement a; a.lengthBars=3;
    check(a.add({0,0,0,1}),"add region");
    check(!a.add({0,1,0,2}) && a.count==1,"reject overlap without mutation");
    check(a.add({0,1,2,1}),"gap accepted");
    check(!a.replace(1,{0,1,3,1}) && a.regions[1].startBar==2,"reject out of bounds move");
    auto project=std::make_unique<alien::SessionProjectData>(); project->arrangement=a;
    auto restored=std::make_unique<alien::SessionProjectData>(); juce::String error;
    check(alien::parseSessionProject(alien::serializeSessionProject(*project),*restored,error)
        && restored->arrangement.count==2 && restored->arrangement.regions[1].startBar==2,"arrangement roundtrip");
    auto song=std::make_unique<alien::ArrangementPlayback>(); song->arrangement=a;
    song->clips[0][0].pattern.add(69,0,16,0.8f); song->clips[1][0].pattern.add(60,0,16,0.8f);
    auto engine=std::make_unique<ToneEngine>(); engine->prepare(48000);engine->patternMode.store(2);
    engine->arrangementMailbox.publish(*song);engine->playing.store(true);
    std::array<float,257> left{},right{}; std::array<double,3> energy{};
    int frame=0;
    while(frame<288257) {
        const int count=std::min(257,288257-frame);float* output[]{left.data(),right.data()};
        engine->audioDeviceIOCallbackWithContext(nullptr,0,output,2,count,{});
        for(int i=0;i<count;++i) { check(std::isfinite(left[i]),"finite output");
            if(frame+i<288000) energy[static_cast<std::size_t>((frame+i)/96000)]+=left[i]*left[i];
            else check(left[i]==0,"exact song end silence"); }
        frame+=count;
    }
    check(energy[0]>1 && energy[1]==0 && energy[2]>1,"regions render around silent gap");
    check(!engine->playing.load(),"song stops automatically");
    auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-export-test",".wav");
    std::atomic<bool> cancel{false}; alien::DrumSamplePointers drums{};
    check(alien::exportArrangementWav(file,*project,*song,drums,cancel,error),"export succeeds");
    juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(),true));
    check(reader && reader->lengthInSamples==288000 && reader->sampleRate==48000 && reader->bitsPerSample==24 && reader->numChannels==2,"WAV format and exact duration");reader.reset();
    juce::MemoryBlock before,after;file.loadFileAsData(before);cancel.store(true);
    check(!alien::exportArrangementWav(file,*project,*song,drums,cancel,error),"cancel rejects export");file.loadFileAsData(after);
    check(before==after,"cancel preserves every destination byte");
    file.deleteFile();std::cout<<"Arrangement playback, persistence and WAV export passed\n";
}
