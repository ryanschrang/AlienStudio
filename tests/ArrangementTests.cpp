#include "WavExport.h"
#include "RackSounds.h"
#include <cstdlib>
#include <iostream>
static void check(bool ok,const char* message) { if(!ok){std::cerr<<message<<'\n';std::exit(1);} }
static void require_(bool ok,const char* message) { check(ok,message); }
// The mailbox hands a 16 MiB snapshot to the audio thread as a pointer swap.
static std::shared_ptr<alien::ArrangementPlayback> songOfBars(int bars) {
    auto song=std::make_shared<alien::ArrangementPlayback>(); song->timeline.setLengthBars(bars); return song;
}
static void mailboxTests() {
    alien::ArrangementMailbox box;
    require_(box.consume()==nullptr,"nothing is delivered before the first publish");
    auto first=songOfBars(4); require_(first->timeline.placeBars(0,0,0,2),"place a region");
    const alien::ArrangementPlayback* firstAddress=first.get();
    require_(box.publish(std::shared_ptr<const alien::ArrangementPlayback>(first)),"publish a shared snapshot");
    first.reset();
    const auto* got=box.consume();
    require_(got==firstAddress && got->timeline.regions.size()==1,"the audio thread reads the published object itself, not a copy");
    require_(box.consume()==nullptr && box.current()==got,"a snapshot is delivered once and stays current");
    auto second=songOfBars(8);
    auto third=songOfBars(12);
    const alien::ArrangementPlayback* thirdAddress=third.get();
    require_(box.publish(std::shared_ptr<const alien::ArrangementPlayback>(second)) && box.publish(std::shared_ptr<const alien::ArrangementPlayback>(third)),"publish twice before consuming");
    require_(box.consume()==thirdAddress && box.consume()==nullptr,"only the newest snapshot is delivered");
    // The reader still holds the third snapshot; earlier ones must be released by publishing, never by the reader.
    std::weak_ptr<const alien::ArrangementPlayback> weakSecond=second;
    second.reset(); third.reset();
    require_(box.current()==thirdAddress,"reader's snapshot stays alive while it is current");
    for(int i=0;i<4;++i) { auto next=songOfBars(5+i); box.publish(std::shared_ptr<const alien::ArrangementPlayback>(next)); }
    require_(weakSecond.expired(),"superseded snapshots are freed by the publishing thread");
    auto invalid=songOfBars(5);
    alien::timeline::Region beyond; beyond.id=1; beyond.row=0; beyond.source=0; beyond.start=0; beyond.length=9*alien::timeline::ticksPerBar;
    invalid->timeline.regions.push_back(beyond);   // runs past the end of the song
    const auto count=box.publishedCount();
    require_(!box.publish(*invalid) && !box.publish(std::shared_ptr<const alien::ArrangementPlayback>{}) && box.publishedCount()==count,"invalid or empty snapshots are rejected");
}

struct Wav { int frames{}; juce::AudioBuffer<float> samples; };
static Wav readWav(const juce::File& file) {
    juce::WavAudioFormat format; Wav wav;
    std::unique_ptr<juce::AudioFormatReader> r(format.createReaderFor(file.createInputStream().release(),true));
    check(r!=nullptr && r->sampleRate==48000 && r->numChannels==2 && r->bitsPerSample==24,"exported WAV is 48 kHz stereo 24-bit");
    wav.frames=static_cast<int>(r->lengthInSamples); wav.samples.setSize(2,wav.frames);
    r->read(&wav.samples,0,wav.frames,0,true,true); return wav;
}
static double energyOf(const Wav& wav,int from,int to) {
    double sum=0; const float* data=wav.samples.getReadPointer(0);
    for(int i=std::max(0,from);i<std::min(to,wav.frames);++i) sum+=static_cast<double>(data[i])*data[i];
    return sum;
}

// Rack patterns on the timeline are part of the export, and an exported range is that part of the song.
static void rackAndRangeExportTests() {
    using namespace alien::timeline;
    auto project=std::make_unique<alien::SessionProjectData>();
    check(project->channelRack.setStepOn(0,0,0,true) && project->channelRack.setStepOn(0,0,8,true),"program a kick pattern");
    auto song=std::make_unique<alien::ArrangementPlayback>();
    song->timeline.setLengthBars(4);
    Region rackRegion; rackRegion.row=rackRowBase; rackRegion.source=0; rackRegion.start=1*ticksPerBar; rackRegion.length=ticksPerBar;
    check(song->timeline.place(rackRegion),"place the pattern on bar two");
    song->timeline.setLoop(0,ticksPerBar); song->timeline.setLoopEnabled(true);
    project->timeline=song->timeline;
    alien::WavExportOptions options;
    for(int c=0;c<project->channelRack.channelCount;++c) {
        const auto& info=project->channelRack.info[static_cast<std::size_t>(c)];
        if(info.isBuiltin()) options.rackClips[static_cast<std::size_t>(c)]=alien::rack::builtinClip(info.builtinId()).get();
    }
    juce::String error; std::atomic<bool> never{false}; alien::DrumSamplePointers noDrums{};
    const auto path=[](const char* name){ return juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile(name,".wav"); };

    // The whole song, although a loop is switched on: it plays through once.
    const auto whole=path("alien-rack-export");
    check(alien::exportArrangementWav(whole,*project,*song,noDrums,never,error,nullptr,options),"export a song that has a rack pattern");
    const auto all=readWav(whole);
    const int bar=96000;
    check(all.frames==4*bar,"the export is the whole song even with a loop range set");
    check(energyOf(all,0,bar)==0,"bar one is silent");
    check(energyOf(all,bar,2*bar)>0.01,"the rack pattern sounds on bar two");
    check(energyOf(all,3*bar,4*bar)==0 || energyOf(all,3*bar,4*bar)<energyOf(all,bar,2*bar)*0.001,"nothing plays after the pattern ends (bar one's tail aside)");

    // Only the pattern's bar.
    alien::WavExportOptions range=options; range.fromTick=ticksPerBar; range.toTick=2*ticksPerBar;
    const auto part=path("alien-range-export");
    check(alien::exportArrangementWav(part,*project,*song,noDrums,never,error,nullptr,range),"export one bar");
    const auto one=readWav(part);
    check(one.frames==bar,"an exported range is exactly that long");
    const double wholeBar=energyOf(all,bar,2*bar), partBar=energyOf(one,0,bar);
    check(std::abs(partBar/wholeBar-1.0)<1e-6,"the range sounds the same as that part of the whole song");

    // Starting in the middle of a pattern plays the second half only.
    alien::WavExportOptions half=options; half.fromTick=ticksPerBar+ticksPerBar/2; half.toTick=2*ticksPerBar;
    const auto halfFile=path("alien-half-export");
    check(alien::exportArrangementWav(halfFile,*project,*song,noDrums,never,error,nullptr,half),"export from the middle of the pattern");
    const auto tail=readWav(halfFile);
    check(tail.frames==bar/2,"half a bar is half a bar of audio");
    check(energyOf(tail,0,bar/2)>0.01,"the second kick of the bar plays");
    const double firstKickHalf=energyOf(all,bar,bar+bar/2), secondKickHalf=energyOf(all,bar+bar/2,2*bar);
    check(std::abs(energyOf(tail,0,bar/2)/secondKickHalf-1.0)<0.05 && firstKickHalf>0.01,"the half-bar render is the back half of the bar");

    // Bad ranges and empty songs are refused with a message.
    alien::WavExportOptions none=options; none.fromTick=2*ticksPerBar; none.toTick=2*ticksPerBar;
    check(!alien::exportArrangementWav(path("alien-empty-range"),*project,*song,noDrums,never,error,nullptr,none) && error.isNotEmpty(),"an empty range is refused");
    auto empty=std::make_unique<alien::ArrangementPlayback>(); empty->timeline.setLengthBars(4);
    check(!alien::exportArrangementWav(path("alien-empty-song"),*project,*empty,noDrums,never,error,nullptr,options) && error.isNotEmpty(),"a song with nothing on it is refused");
    whole.deleteFile(); part.deleteFile(); halfFile.deleteFile();
}

int main() {
    mailboxTests();
    alien::timeline::Timeline a; a.setLengthBars(3);
    check(a.placeBars(0,0,0,1),"add region");
    check(a.placeBars(0,1,2,1),"gap accepted");
    check(a.regions.size()==2,"two regions");
    auto project=std::make_unique<alien::SessionProjectData>(); project->timeline=a;
    auto restored=std::make_unique<alien::SessionProjectData>(); juce::String error;
    check(alien::parseSessionProject(alien::serializeSessionProject(*project),*restored,error)
        && restored->timeline.regions.size()==2 && restored->timeline.regions[1].start==2*alien::timeline::ticksPerBar,"timeline roundtrip");
    auto song=std::make_unique<alien::ArrangementPlayback>(); song->timeline=a;
    song->clips[0][0].pattern.add(69,0,16,0.8f); song->clips[1][0].pattern.add(60,0,16,0.8f);
    auto engine=std::make_unique<ToneEngine>(); engine->prepare(48000);engine->patternMode.store(2);
    engine->arrangementMailbox.publish(*song);engine->playing.store(true);
    std::array<float,257> left{},right{}; std::array<double,3> energy{}; double lateGap=0;
    int frame=0;
    while(frame<288257) {
        const int count=std::min(257,288257-frame);float* output[]{left.data(),right.data()};
        engine->audioDeviceIOCallbackWithContext(nullptr,0,output,2,count,{});
        for(int i=0;i<count;++i) { check(std::isfinite(left[i]),"finite output");
            if(frame+i<288000) {
                energy[static_cast<std::size_t>((frame+i)/96000)]+=left[i]*left[i];
                if(frame+i>=96000+48000 && frame+i<192000) lateGap+=left[i]*left[i];
            } else check(left[i]==0,"exact song end silence"); }
        frame+=count;
    }
    // A note that ends with its region rings out its release (only), then the gap is silent.
    check(energy[0]>1 && energy[1]<energy[0]*0.1 && lateGap==0 && energy[2]>1,"regions render around a gap");
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
    // The metronome is a monitoring aid: an engine left with it switched on must not put clicks in the file.
    {
        auto prepared=std::make_unique<ToneEngine>();
        alien::RecordSettings clicking; clicking.metronome=true; prepared->recording.apply(clicking);
        const auto out=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-click-test",".wav");
        std::atomic<bool> never{false}; alien::DrumSamplePointers noDrums{};
        check(alien::exportArrangementWav(out,*project,*song,noDrums,never,error,prepared.get()),"export on a prepared engine");
        juce::MemoryBlock plain,withMetronome; file.loadFileAsData(plain); out.loadFileAsData(withMetronome);
        check(plain.getSize()>1000 && plain==withMetronome,"the metronome is not part of an exported file");
        out.deleteFile();
    }
    file.deleteFile();
    rackAndRangeExportTests();
    std::cout<<"Arrangement playback, persistence and WAV export passed\n";
}
