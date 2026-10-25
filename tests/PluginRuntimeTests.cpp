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
    runtime->racks[0].effects[0].bypass=true;runtime->begin(16,120,true,0);runtime->tracks[0].setSample(0,0,0.25f);runtime->effectsAndAlign(16);require(runtime->tracks[0].getSample(0,0)==0.25f,"FX bypass altered audio");
    alien::SessionProjectData project;juce::PluginDescription desc;desc.name="Missing instrument";desc.pluginFormatName="VST3";desc.fileOrIdentifier="/missing/original.vst3";desc.isInstrument=true;
    project.plugins[0].instrument={desc.createXml()->toString(),"YWJj",true};project.plugins[1].effects.push_back({desc.createXml()->toString(),{},false});
    juce::String error;alien::SessionProjectData parsed;require(alien::parseSessionProject(alien::serializeSessionProject(project),parsed,error) && parsed.plugins==project.plugins,"plugin identity/state/bypass roundtrip failed");
    auto hostRuntime=std::make_unique<alien::PluginRuntime>();alien::PluginHost host(*hostRuntime);auto states=project.plugins;const auto saved=states;
    require(host.restore(states,project.masterPlugins,48000).isNotEmpty(),"missing plugin did not report an error");host.snapshot(states,project.masterPlugins);
    require(states==saved && hostRuntime->hasInstrument(0),"missing plugin state lost or replaced by built-in synth");
    std::cout<<"PASS: MIDI offsets, FX/master, transport stop, PDC, bypass, allocation, state and missing plugins\n";
}
