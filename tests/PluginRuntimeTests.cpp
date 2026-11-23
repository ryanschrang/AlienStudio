#include "ToneEngine.h"
#include "PluginHost.h"
#include <iostream>
#include <cstdlib>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);} void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
static void require(bool b,const char* message){if(!b){std::cerr<<message<<'\n';std::exit(1);}}
class TestProcessor final : public juce::AudioProcessor {
public:
    explicit TestProcessor(bool instrument=false,float gain=1,int latency=0)
        : juce::AudioProcessor(BusesProperties().withInput("In",juce::AudioChannelSet::stereo(),!instrument).withOutput("Out",juce::AudioChannelSet::stereo(),true)),synth(instrument),factor(gain),delay(latency){setLatencySamples(latency);}
    void prepareToPlay(double,int) override{position=0;ring.fill(0);active=false;}
    void releaseResources() override{}
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer& midi) override {
        auto it=midi.begin();const auto end=midi.end();
        for(int i=0;i<b.getNumSamples();++i){
            while(it!=end && (*it).samplePosition<=i){
                const auto msg=(*it).getMessage(); const int channel=msg.getChannel();
                if(msg.isController()) {
                    lastController=msg.getControllerNumber(); controlChannel=channel;
                    if(lastController==64) {
                        sustain[channel]=msg.getControllerValue()>=64;
                        if(!sustain[channel] && !held && noteChannel==channel) active=false;
                    }
                }
                if(msg.isPitchWheel()) {lastWheel=msg.getPitchWheelValue();controlChannel=channel;}
                if(msg.isNoteOn()) {active=held=true;noteChannel=channel;}
                else if(msg.isAllSoundOff() || msg.isAllNotesOff()) {if(noteChannel==channel) active=held=false;}
                else if(msg.isNoteOff() && noteChannel==channel) {held=false;if(!sustain[channel])active=false;}
                ++it;
            }
            for(int c=0;c<b.getNumChannels();++c)b.setSample(c,i,(synth?(active?0.2f:0.0f):b.getSample(c,i))*factor);
            if(delay){const auto value=b.getSample(0,i);const auto out=ring[position];ring[position]=value;position=(position+1)%delay;b.setSample(0,i,out);b.setSample(1,i,out);}
        }
        auto info=getPlayHead()?getPlayHead()->getPosition():juce::Optional<juce::AudioPlayHead::PositionInfo>{};
        sawTempo=info && info->getBpm() && *info->getBpm()==120;
    }
    const juce::String getName() const override{return "Original test processor";}
    bool acceptsMidi() const override{return synth;} bool producesMidi() const override{return false;}
    double getTailLengthSeconds() const override{return 0;}
    int getNumPrograms() override{return 1;}int getCurrentProgram() override{return 0;}void setCurrentProgram(int) override{}
    const juce::String getProgramName(int) override{return {}; }void changeProgramName(int,const juce::String&) override{}
    bool hasEditor() const override{return false;}juce::AudioProcessorEditor* createEditor() override{return nullptr;}
    void getStateInformation(juce::MemoryBlock& data) override{data.append(&factor,sizeof(factor));}
    void setStateInformation(const void* data,int n) override{if(n==sizeof(factor))std::memcpy(&factor,data,n);}
    std::array<bool,17> sustain{}; bool held{},synth{},active{},sawTempo{};float factor;int delay{},position{},noteChannel{},controlChannel{},lastController{-1},lastWheel{};std::array<float,16> ring{};
};
int main(){
    auto engine=std::make_unique<ToneEngine>();engine->prepare(48000);engine->patternMode.store(1);engine->launchQuantizationSteps.store(4);
    auto& rack=engine->plugins->racks[0];rack.instrumentAssigned=true;
    auto instrument=std::make_unique<TestProcessor>(true);auto* raw=instrument.get();rack.instrument.processor=std::move(instrument);engine->plugins->prepareSlot(rack.instrument);
    rack.effectCount=2;rack.effects[0].processor=std::make_unique<TestProcessor>(false,2);rack.effects[1].processor=std::make_unique<TestProcessor>(false,0.5f);
    for(auto& slot:rack.effects)engine->plugins->prepareSlot(slot);
    auto& master=engine->plugins->racks[alien::maxSessionTracks];master.effectCount=1;master.effects[0].processor=std::make_unique<TestProcessor>(false,0.5f);engine->plugins->prepareSlot(master.effects[0]);
    alien::SessionClipLaunch clip;clip.pattern.clear();clip.pattern.add(60,1.0,2.0);clip.clipIndex=0;engine->pianoRollLaunches[0].publish(clip);engine->playing.store(true);
    // 1024-sample callbacks cross the internal 512-sample block boundary. MIDI
    // starts exactly at sample 6000 (one sixteenth at 120 BPM / 48 kHz).
    std::array<float,1024> l{},r{};float* outputs[]{l.data(),r.data()};bool heard=false;int first=-1;
    for(int block=0;block<20;++block){const auto before=allocations;engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});require(allocations==before,"plugin callback allocated host memory");
        for(int i=0;i<1024;++i){if(std::abs(l[i])>0.001f){heard=true;if(first<0)first=block*1024+i;require(std::abs(l[i]-0.1f)<0.0001f,"instrument/track/master gain chain incorrect");}}
    }
    require(heard && first==6000,"MIDI event was not sample accurate across callback chunks");require(raw->sawTempo,"tempo/playhead missing");
    engine->playing.store(false);engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});require(!raw->active,"transport stop left plugin notes held");
    // Live input works while stopped and releases the old plugin on route changes.
    engine->liveDrumTrack.store(0);
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    engine->notes.push(60,1);
    const auto liveAllocations=allocations;
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(raw->active,"typing piano did not reach selected VST instrument");
    require(allocations==liveAllocations,"live instrument route allocated callback memory");
    engine->liveDrumTrack.store(-1);
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(!raw->active,"changing live route left plugin note held");
    engine->liveDrumTrack.store(0);
    const auto token=engine->midi.beginSession();
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    engine->midi.push(token,0x9f,60,100);
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(raw->active && raw->noteChannel==16,"hardware MIDI channel did not reach selected VST instrument");
    engine->midi.push(token,0xbf,1,75);
    engine->midi.push(token,0xef,0,80);
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(raw->lastController==1 && raw->controlChannel==16 && raw->lastWheel==10240,"live CC/pitch wheel routing failed");
    engine->midi.push(token,0xbf,64,127);
    engine->midi.push(token,0x8f,60,0);
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(raw->active,"sustain pedal failed to retain live note");
    engine->midi.requestReset();
    engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,1024,{});
    require(!raw->active,"device reset left plugin note held");
    // PDC: both an inherent-latency track and an immediate track align at 4.
    auto runtime=std::make_unique<alien::PluginRuntime>();runtime->racks[0].effectCount=1;runtime->racks[0].effects[0].processor=std::make_unique<TestProcessor>(false,1,4);runtime->prepare(48000);
    runtime->begin(16,120,true,0);runtime->tracks[0].setSample(0,0,1);runtime->tracks[0].setSample(1,0,1);runtime->tracks[1].setSample(0,0,1);runtime->tracks[1].setSample(1,0,1);runtime->effectsAndAlign(16);
    require(runtime->tracks[0].getSample(0,4)==1 && runtime->tracks[1].getSample(0,4)==1 && runtime->tracks[1].getSample(0,0)==0,"track latency compensation failed");
    // T1-04 C: a slot that leaves use forgets the audio its alignment delay holds. Track 1 waits 4 samples for
    // track 0; an impulse at sample 14 is still in its delay when the slot leaves, and must not play when it returns.
    {
        auto pdc=std::make_unique<alien::PluginRuntime>();pdc->racks[0].effectCount=1;pdc->racks[0].effects[0].processor=std::make_unique<TestProcessor>(false,1,4);pdc->prepare(48000);
        pdc->begin(16,120,true,0);pdc->useTracks(2);pdc->tracks[1].setSample(0,14,1);pdc->tracks[1].setSample(1,14,1);pdc->effectsAndAlign(16);
        require(pdc->tracks[1].getSample(0,14)==0,"the impulse waits in the alignment delay");
        pdc->begin(16,120,true,0);pdc->useTracks(1);pdc->effectsAndAlign(16);   // the second track is deleted
        pdc->begin(16,120,true,0);pdc->useTracks(2);pdc->effectsAndAlign(16);   // and a new one takes its slot
        bool silent=true;for(int i=0;i<16;++i)silent=silent&&pdc->tracks[1].getSample(0,i)==0&&pdc->tracks[1].getSample(1,i)==0;
        require(silent && pdc->tracksInUse()==2,"a reused track slot played audio from before it left use");
        pdc->begin(16,120,true,0);pdc->tracks[1].setSample(0,0,1);pdc->effectsAndAlign(16);
        require(pdc->tracks[1].getSample(0,4)==1,"a reused slot still aligns its new audio");
    }
    // T1-04 D: the alignment lines are sized to the latency the plugins report, not to maxDelay.
    {
        using R=alien::PluginRuntime;
        auto idle=std::make_unique<R>();idle->prepare(48000);
        for(int s=0;s<R::alignmentStageCount;++s)require(idle->alignmentCapacity(static_cast<R::AlignmentStage>(s))==0,"a project without latent plugins reserves alignment audio");
        require(idle->alignmentBytes()<4096,"empty alignment stages still hold audio");
        require(runtime->alignmentCapacity(R::trackAlignment)==R::alignmentGranule,"a 4-sample plugin's stage is one granule long");
        require(runtime->alignmentCapacity(R::returnAlignment)==0 && runtime->alignmentCapacity(R::busAlignment)==0,"stages without latent plugins grew");

        // A plugin that reports more latency while playing: the callback clamps and counts, allocates nothing,
        // and the host's poll grows only that stage, which the next block adopts.
        auto grow=std::make_unique<R>();grow->racks[0].effectCount=1;grow->racks[0].effects[0].processor=std::make_unique<TestProcessor>(false,1,4);grow->prepare(48000);
        alien::PluginHost growHost(*grow);
        juce::String latencyText;int latencyReports=0;
        growHost.onLatencyProblem=[&](const juce::String& text){latencyText=text;++latencyReports;};
        growHost.poll();
        require(latencyReports==0,"a poll with nothing new reported plugin latency");
        const auto before=grow->alignmentShortfalls();
        grow->racks[0].effects[0].processor->setLatencySamples(3000);
        auto block=[&](int impulseAt){grow->begin(512,120,true,0);grow->useTracks(2);if(impulseAt>=0){grow->tracks[1].setSample(0,impulseAt,1);grow->tracks[1].setSample(1,impulseAt,1);}grow->effectsAndAlign(512);};
        const auto quiet=allocations;block(-1);block(-1);
        require(allocations==quiet,"a too-short alignment line allocated in the callback");
        require(grow->alignmentShortfalls()==before+2,"each short block is counted once");
        growHost.poll();
        // T1-04 D13: the moment the tracks were out of step is reported, once.
        require(latencyReports==1 && latencyText.contains("latency grew while playing") && latencyText.contains("out of step"),
                "a shortfall while playing was not reported");
        require(grow->alignmentCapacity(R::trackAlignment)==R::alignmentGranule,"the callback's storage changed before it adopted the new one");
        const auto adopt=allocations;block(0);
        require(allocations==adopt,"adopting grown alignment storage allocated in the callback");
        require(grow->alignmentCapacity(R::trackAlignment)==3072 && grow->alignmentCapacity(R::returnAlignment)==0,"only the track stage grew, to whole granules");
        int heardAt=grow->tracks[1].getSample(0,0)!=0?0:-1;
        for(int b=1;b<7&&heardAt<0;++b){block(-1);for(int i=0;i<512;++i)if(grow->tracks[1].getSample(0,i)!=0){heardAt=b*512+i;break;}}
        require(heardAt==3000,"after growing, the faster track waits exactly as long as the latent one");
        require(grow->alignmentShortfalls()==before+2,"the grown stage is long enough");
        growHost.poll();   // frees the storage the callback gave back; nothing more to grow
        require(latencyReports==1,"a shortfall was reported twice"); 
        require(grow->reserveAlignment()==0,"a stage that fits is not replaced");
        // Latency falls again: polling never shrinks (so a wobbling plugin does not reallocate); a restore does.
        grow->racks[0].effects[0].processor->setLatencySamples(4);
        growHost.poll();block(-1);
        require(grow->alignmentCapacity(R::trackAlignment)==3072,"polling shrank a stage");
        require(grow->reserveAlignment(true)==1,"a restore fits the stage to the plugins");
        block(-1);
        require(grow->alignmentCapacity(R::trackAlignment)==R::alignmentGranule,"the stage shrank after a restore");
    }
    // T1-04 D13: a chain whose plugins report more latency than maxDelay cannot be lined up; it is named, once,
    // until it is back within the limit. So is a Channel Rack instrument.
    {
        using R=alien::PluginRuntime;
        require(alien::PluginHost::latencyLimitText("track 2",120000)
                ==juce::String("The plugins on track 2 report 120,000 samples of latency, more than the 96,000 that can be lined up, "
                               "so track 2 plays 24,000 samples late. Bypass or remove a high-latency plugin there."),
                "the latency limit message changed");
        auto over=std::make_unique<R>();
        over->racks[1].effectCount=2;
        for(int i=0;i<2;++i)over->racks[1].effects[static_cast<std::size_t>(i)].processor=std::make_unique<TestProcessor>(false,1,4);
        over->rackChannels[2].processor=std::make_unique<TestProcessor>(true,1,0);
        over->prepare(48000);
        alien::PluginHost overHost(*over);
        juce::String text;int reports=0;
        overHost.onLatencyProblem=[&](const juce::String& t){text=t;++reports;};
        require(over->reportedLatency(1)==8 && over->reportedLatency(R::channelBase+2)==0,"reported latency is not the chain's sum");
        for(int i=0;i<2;++i)over->racks[1].effects[static_cast<std::size_t>(i)].processor->setLatencySamples(60000);
        overHost.poll();
        require(over->reportedLatency(1)==120000,"reported latency was clamped");
        require(reports==1 && text.contains("The plugins on track 2 report 120,000 samples") && text.contains("plays 24,000 samples late"),
                "a chain past the latency limit was not reported");
        require(!text.contains("track 1") && !text.contains("Channel Rack"),"a chain within the limit was named");
        overHost.poll();
        require(reports==1,"a chain past the limit was named again while nothing changed");
        over->rackChannels[2].processor->setLatencySamples(100000);
        overHost.poll();
        require(reports==2 && text.contains("Channel Rack channel 3 report 100,000") && !text.contains("track 2"),
                "only the newly late Channel Rack instrument should be named");
        over->racks[1].effects[1].bypass=true;   // back within the limit: forgotten, so a later excess is new
        overHost.poll();
        require(reports==2,"a chain back within the limit was reported");
        over->racks[1].effects[1].bypass=false;
        overHost.poll();
        require(reports==3 && text.contains("track 2 report 120,000"),"a chain past the limit again was not reported");
        over->racks[1].frozen=true;   // a frozen track plays its frozen audio, without plugin latency
        overHost.poll(); over->racks[1].frozen=false;
        require(reports==3,"a frozen chain was reported");
        // Without the callback, poll() leaves them for an export to collect.
        alien::PluginHost quiet(*over);
        quiet.poll();
        juce::String collected; quiet.latencyProblems(collected);
        require(collected.contains("track 2 report 120,000") && collected.contains("Channel Rack channel 3"),
                "poll() without a latency callback used up the report an export collects");
        std::cout<<"PASS: plugin latency past the limit and alignment shortfalls are reported\n";
    }
    runtime->racks[0].effects[0].bypass=true;runtime->begin(16,120,true,0);runtime->tracks[0].setSample(0,0,0.25f);runtime->effectsAndAlign(16);require(runtime->tracks[0].getSample(0,0)==0.25f,"FX bypass altered audio");
    alien::SessionProjectData project;juce::PluginDescription desc;desc.name="Missing instrument";desc.pluginFormatName="VST3";desc.fileOrIdentifier="/missing/original.vst3";desc.isInstrument=true;
    project.plugins[0].instrument={desc.createXml()->toString(),"YWJj",true};project.plugins[1].effects.push_back({desc.createXml()->toString(),{},false});
    juce::String error;alien::SessionProjectData parsed;require(alien::parseSessionProject(alien::serializeSessionProject(project),parsed,error) && parsed.plugins==project.plugins,"plugin identity/state/bypass roundtrip failed");
    auto hostRuntime=std::make_unique<alien::PluginRuntime>();alien::PluginHost host(*hostRuntime);auto states=project.plugins;const auto saved=states;
    require(host.restore(states,project.masterPlugins,48000).isNotEmpty(),"missing plugin did not report an error");host.snapshot(states,project.masterPlugins);
    require(states==saved && hostRuntime->hasInstrument(0),"missing plugin state lost or replaced by built-in synth");
    std::cout<<"PASS: MIDI offsets, FX/master, transport stop, PDC, bypass, allocation, state and missing plugins\n";
    // T1-04 D11: a track instrument is given at most trackEventsPerBlock MIDI events a block. The rest (notes,
    // controllers and pitch bend alike) are counted, the next block ends every note, and nothing reallocates.
    {
        using R=alien::PluginRuntime;
        auto flood=std::make_unique<R>();
        auto& track=flood->racks[0];track.instrumentAssigned=true;
        auto synth=std::make_unique<TestProcessor>(true);auto* heard=synth.get();track.instrument.processor=std::move(synth);
        flood->prepare(48000);
        flood->begin(512,120,true,0);flood->instruments(512);   // the panic every prepare sends
        const auto before=allocations;
        flood->begin(512,120,true,0);
        for(int i=0;i<R::trackEventsPerBlock;++i)flood->note(0,60,1.0f,true);
        require(flood->droppedTrackEvents()==0 && flood->midi[0].getNumEvents()==R::trackEventsPerBlock,"a full block of track MIDI is kept");
        flood->note(0,60,0.0f,false);flood->controller(0,1,64,0);flood->pitchWheel(0,1,8192);
        require(flood->droppedTrackEvents()==3 && flood->midi[0].getNumEvents()==R::trackEventsPerBlock,"events past the limit are counted, not kept");
        flood->instruments(512);
        require(heard->active,"the note plays, though its note-off was dropped");
        flood->begin(512,120,true,0);flood->instruments(512);
        require(!heard->active,"the next block ends it");
        flood->begin(512,120,true,0);flood->note(0,62,1.0f,true);
        require(flood->droppedTrackEvents()==3 && flood->midi[0].getNumEvents()==1,"each block starts with room again");
        require(allocations==before,"a flood of track MIDI does not allocate");
    }
    std::cout<<"PASS: track instrument MIDI is bounded per block, counted when dropped and never left held\n";
}
