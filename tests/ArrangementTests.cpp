#include "WavExport.h"
#include <cstdlib>
#include <iostream>
static void check(bool ok,const char* message) { if(!ok){std::cerr<<message<<'\n';std::exit(1);} }
static void require_(bool ok,const char* message) { check(ok,message); }
// The mailbox hands a 16 MiB snapshot to the audio thread as a pointer swap.
static void mailboxTests() {
    alien::ArrangementMailbox box;
    require_(box.consume()==nullptr,"nothing is delivered before the first publish");
    auto first=std::make_shared<alien::ArrangementPlayback>(); first->arrangement.lengthBars=4; first->arrangement.add({0,0,0,2});
    const alien::ArrangementPlayback* firstAddress=first.get();
    require_(box.publish(std::shared_ptr<const alien::ArrangementPlayback>(first)),"publish a shared snapshot");
    first.reset();
    const auto* got=box.consume();
    require_(got==firstAddress && got->arrangement.count==1,"the audio thread reads the published object itself, not a copy");
    require_(box.consume()==nullptr && box.current()==got,"a snapshot is delivered once and stays current");
    auto second=std::make_shared<alien::ArrangementPlayback>(); second->arrangement.lengthBars=8;
    auto third=std::make_shared<alien::ArrangementPlayback>(); third->arrangement.lengthBars=12;
    const alien::ArrangementPlayback* thirdAddress=third.get();
    require_(box.publish(std::shared_ptr<const alien::ArrangementPlayback>(second)) && box.publish(std::shared_ptr<const alien::ArrangementPlayback>(third)),"publish twice before consuming");
    require_(box.consume()==thirdAddress && box.consume()==nullptr,"only the newest snapshot is delivered");
    // The reader still holds the third snapshot; earlier ones must be released by publishing, never by the reader.
    std::weak_ptr<const alien::ArrangementPlayback> weakSecond=second;
    second.reset(); third.reset();
    require_(box.current()==thirdAddress,"reader's snapshot stays alive while it is current");
    for(int i=0;i<4;++i) { auto next=std::make_shared<alien::ArrangementPlayback>(); next->arrangement.lengthBars=2+i; box.publish(std::shared_ptr<const alien::ArrangementPlayback>(next)); }
    require_(weakSecond.expired(),"superseded snapshots are freed by the publishing thread");
    auto invalid=std::make_unique<alien::ArrangementPlayback>(); invalid->arrangement.lengthBars=2; invalid->arrangement.regions[0]={0,0,1,5}; invalid->arrangement.count=1;
    const auto count=box.publishedCount();
    require_(!box.publish(*invalid) && !box.publish(std::shared_ptr<const alien::ArrangementPlayback>{}) && box.publishedCount()==count,"invalid or empty snapshots are rejected");
}

static void layoutTests() {
    alien::Arrangement a; a.lengthBars=8; a.add({0,0,0,2}); a.add({1,2,2,2});
    auto b=a;
    require_(a.sameLayout(b),"identical arrangements share a layout");
    b.regions[1].scene=3; require_(!a.sameLayout(b),"a different source scene changes the layout");
    b=a; b.regions[0].startBar=1; require_(!a.sameLayout(b),"a moved region changes the layout");
    b=a; b.lengthBars=9; require_(!a.sameLayout(b),"a different song length changes the layout");
    b=a; b.erase(1); require_(!a.sameLayout(b),"a removed region changes the layout");
    b=a; b.regions[5]={7,7,7,7}; require_(a.sameLayout(b),"unused slots past the count are ignored");
}

int main() {
    mailboxTests();
    layoutTests();
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
    // Master volume is part of the mix, so the export must follow it.
    auto rms=[&](float masterGain) {
        auto loud=std::make_unique<alien::SessionProjectData>(*project); loud->masterGain=masterGain;
        const auto out=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-master-test",".wav");
        std::atomic<bool> never{false}; alien::DrumSamplePointers noDrums{};
        check(alien::exportArrangementWav(out,*loud,*song,noDrums,never,error),"export with master volume");
        std::unique_ptr<juce::AudioFormatReader> r(format.createReaderFor(out.createInputStream().release(),true));
        check(r!=nullptr,"read exported file");
        juce::AudioBuffer<float> samples(2,static_cast<int>(r->lengthInSamples));
        r->read(&samples,0,static_cast<int>(r->lengthInSamples),0,true,true);
        double sum=0; const float* data=samples.getReadPointer(0);
        for(int i=0;i<samples.getNumSamples();++i) sum+=static_cast<double>(data[i])*data[i];
        r.reset(); out.deleteFile();
        return std::sqrt(sum/samples.getNumSamples());
    };
    const double unity=rms(1.0f), half=rms(0.5f), silent=rms(0.0f);
    check(unity>0.001 && std::abs(half/unity-0.5)<0.01,"export follows the master volume");
    check(silent==0.0,"zero master volume exports silence");
    file.deleteFile();std::cout<<"Arrangement playback, persistence and WAV export passed\n";
}
