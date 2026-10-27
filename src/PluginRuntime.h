#pragma once
#include "PluginFault.h"
#include "PluginState.h"
#include "SessionCapacity.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace alien {
// Only the audio thread uses buffers/MIDI/delay state. Slot changes, native
// editors and state capture require the owner to detach the device callback.
//
// Every call into plugin code is contained (FaultGuard.h): a crash, a thrown exception or NaN
// output disables that one slot, passes the audio on without it, and queues a report for the
// message thread. A plugin that has failed is never called again - not even to be destroyed,
// because its internal state (held locks, half-freed memory) is unknown. It is parked instead.
class PluginRuntime {
public:
    // Racks: one per Session track, then the master, then the return slots (A-H), then the group buses.
    static constexpr int blockSize = 512, maxDelay = 96000, masterIndex = maxSessionTracks;
    static constexpr int returnBase = masterIndex + 1, busBase = returnBase + maxReturns, rackCount = busBase + maxBuses;
    static constexpr int returnIndex(int r) noexcept { return returnBase + r; }
    static constexpr bool isReturn(int rack) noexcept { return rack >= returnBase && rack < busBase; }
    static constexpr int busIndex(int b) noexcept { return busBase + b; }
    static constexpr bool isBus(int rack) noexcept { return rack >= busBase && rack < rackCount; }
    struct PlayHead final : juce::AudioPlayHead {
        PositionInfo info;
        juce::Optional<PositionInfo> getPosition() const override { return info; }
    } playHead;
    // An atomic flag that can still be copied, so a Slot can be moved into a Rack.
    struct Flag {
        std::atomic<bool> value{false};
        Flag() = default;
        Flag(const Flag& other) noexcept : value(other.value.load()) {}
        Flag& operator=(const Flag& other) noexcept { value.store(other.value.load()); return *this; }
        bool get() const noexcept { return value.load(std::memory_order_acquire); }
        void set(bool state) noexcept { value.store(state, std::memory_order_release); }
    };
    struct Slot {
        std::unique_ptr<juce::AudioProcessor> processor;
        bool bypass{}, prepared{};
        std::uint64_t instanceId{}; // assigned by the host off the audio thread
        Flag faulted;               // set by whichever thread saw the failure; cleared only by unloading the slot
        juce::String name;          // for reports; the host fills it in when it loads the plugin
        int keyIndex{-1};           // what keys this effect's sidechain input (set while the slot is loaded), or -1
        KeySource keyFrom{KeySource::track};
    };
    struct Rack {
        Slot instrument;
        bool instrumentAssigned{}, frozen{};
        std::array<Slot,8> effects;
        int effectCount{};
    };
    std::array<Rack, rackCount> racks;
    std::array<juce::AudioBuffer<float>, maxSessionTracks> tracks;
    juce::AudioBuffer<float> live, master;
    std::array<juce::AudioBuffer<float>, maxReturns> returns;   // what the tracks send to each return, then its FX
    std::array<juce::AudioBuffer<float>, maxBuses> buses;       // the tracks routed to each group bus, then its FX
    std::array<juce::MidiBuffer, maxSessionTracks> midi;
    int sampleOffset{};
    std::array<int,maxSessionTracks> eventCounts{};
    std::atomic<bool> panic{};
    FaultQueue faults;                          // read by the message thread (PluginHost::poll)
    std::atomic<std::uint32_t> faultCount{0};   // every fault ever recorded here; an export compares before and after

    PluginRuntime() { prepare(44100); }
    ~PluginRuntime() {
        for (int r=0;r<rackCount;++r) {
            destroy(racks[static_cast<std::size_t>(r)].instrument,r,-1);
            for (int i=0;i<static_cast<int>(racks[static_cast<std::size_t>(r)].effects.size());++i) destroy(racks[static_cast<std::size_t>(r)].effects[static_cast<std::size_t>(i)],r,i);
        }
    }
    // Records that `slot` failed and disables it. Safe on the audio thread: no allocation, no lock.
    void markFaulted(Slot& slot,int rack,int index,PluginPhase phase,const fault::Fault& fault,bool nonFinite=false) noexcept {
        slot.faulted.set(true);
        PluginFault report; report.rack=rack; report.slot=index; report.phase=phase; report.fault=fault; report.nonFiniteOutput=nonFinite;
        report.samplePosition=renderedSamples;
        faults.push(report);
        faultCount.fetch_add(1,std::memory_order_acq_rel);
    }
    // Parks a failed processor. It is deliberately never destroyed: its destructor would run the
    // same broken code, possibly waiting on a lock the crash left held. Memory is reclaimed at exit.
    static void quarantine(std::unique_ptr<juce::AudioProcessor>& processor) {
        if (!processor) return;
        static std::mutex* const guard = new std::mutex;
        static std::vector<juce::AudioProcessor*>* const parked = new std::vector<juce::AudioProcessor*>;
        std::lock_guard<std::mutex> lock(*guard);
        parked->push_back(processor.release());
        quarantineTally().fetch_add(1);
    }
    static std::size_t quarantinedCount() { return quarantineTally().load(); }
    // Message thread. Releases and destroys a slot's processor, or parks it if it ever failed.
    void destroy(Slot& slot,int rack=-1,int index=-2) {
        if (!slot.processor) { slot.prepared=false; return; }
        if (!slot.faulted.get()) release(slot,rack,index);
        if (slot.faulted.get()) { quarantine(slot.processor); slot.prepared=false; return; }
        juce::AudioProcessor* raw=slot.processor.release();
        fault::Fault fault;
        if (!fault::guarded([&]{ delete raw; },fault)) {
            PluginFault report; report.rack=rack; report.slot=index; report.phase=PluginPhase::release; report.fault=fault;
            faults.push(report); faultCount.fetch_add(1,std::memory_order_acq_rel);
        }
        slot.prepared=false;
    }
    // Message thread (or the thread that owns an idle engine).
    void release(Slot& slot,int rack=-1,int index=-2) {
        if(!slot.processor || !slot.prepared) return;
        slot.prepared=false;
        if(slot.faulted.get()) return;
        fault::Fault fault;
        if (!fault::guarded([&]{ slot.processor->releaseResources(); },fault)) markFaulted(slot,rack,index,PluginPhase::release,fault);
    }
    void prepare(double sampleRate) {
        rate = sampleRate; renderedSamples = 0;
        for (auto& b : tracks) b.setSize(2, blockSize);
        live.setSize(2, blockSize); master.setSize(2, blockSize);
        for (auto& b : returns) b.setSize(2, blockSize);
        for (auto& b : buses) b.setSize(2, blockSize);
        for (auto& m : midi) m.ensureSize(65536);
        emptyMidi.ensureSize(65536);
        for (auto& delay : delays) delay.prepare();
        for (auto& delay : returnDelays) delay.prepare();
        for (auto& delay : busDelays) delay.prepare();
        for (auto& delay : sendDelays) delay.prepare();
        dryDelay.prepare(); directDelay.prepare();
        for (int r=0;r<rackCount;++r) {
            auto& rack=racks[static_cast<std::size_t>(r)];
            prepareSlot(rack.instrument,r,-1);
            for (int i=0;i<static_cast<int>(rack.effects.size());++i) prepareSlot(rack.effects[static_cast<std::size_t>(i)],r,i);
        }
        panic.store(true);
    }
    void silence(std::size_t track, int onlyChannel = 0) noexcept {
        for (int channel=1;channel<=16;++channel) {
            if (onlyChannel != 0 && channel != onlyChannel) continue;
            midi[track].addEvent(juce::MidiMessage::allSoundOff(channel),sampleOffset);
            midi[track].addEvent(juce::MidiMessage::allNotesOff(channel),sampleOffset);
        }
    }
    // `seconds` is the song time at `ppq` when a tempo map makes it more than ppq at `bpm`.
    // A song with a time signature map passes the meter and where the bar at `ppq` began.
    void begin(int samples, double bpm, bool playing, double ppq, double seconds = -1.0,
               int numerator = 4, int denominator = 4, double barStartPpq = -1.0) noexcept {
        for (auto& b : tracks) b.clear(); live.clear(); master.clear();
        for (auto& b : returns) b.clear();
        for (auto& b : buses) b.clear();
        for (auto& m : midi) m.clear();
        // The mixer writes every track's and bus's key each block; a return that does not exist is skipped,
        // so its key is cleared here rather than left over from when it did.
        for (auto& key : returnKeys) for (auto& channel : key) channel.fill(0.0f);
        sampleOffset = 0; currentSamples = samples; eventCounts.fill(0);
        playHead.info.setBpm(bpm);
        const auto timelineSamples = static_cast<juce::int64>(std::llround(seconds >= 0.0 ? seconds * rate : ppq * rate * 60.0 / bpm));
        playHead.info.setTimeInSamples(timelineSamples);
        playHead.info.setTimeInSeconds(timelineSamples / rate);
        playHead.info.setPpqPosition(ppq);
        playHead.info.setPpqPositionOfLastBarStart(barStartPpq >= 0.0 ? barStartPpq : std::floor(ppq / 4.0) * 4.0);
        playHead.info.setTimeSignature(juce::AudioPlayHead::TimeSignature{numerator,denominator});
        playHead.info.setIsPlaying(playing);
        if (panic.exchange(false)) for (auto& m : midi)
            for (int ch = 1; ch <= 16; ++ch) { m.addEvent(juce::MidiMessage::allSoundOff(ch),0); m.addEvent(juce::MidiMessage::allNotesOff(ch),0); }
    }
    bool hasInstrument(std::size_t track) const noexcept { return racks[track].instrumentAssigned; }
    // The mixer hands over the keys it makes, sample by sample: each track after its fader and pan,
    // each bus after its strip, each return after its strip (MIX-04).
    void keyPostFader(std::size_t track, int i, float left, float right) noexcept { store(postKeys, track, i, left, right); }
    void keyBus(std::size_t bus, int i, float left, float right) noexcept { store(busKeys, bus, i, left, right); }
    void keyReturn(std::size_t r, int i, float left, float right) noexcept { store(returnKeys, r, i, left, right); }
    // Where in the mix a rack runs, which decides the keys it can hear.
    static constexpr RackStage stageOf(int rack) noexcept {
        return rack < maxSessionTracks ? RackStage::track : rack == masterIndex ? RackStage::master
             : isReturn(rack) ? RackStage::returnTrack : RackStage::bus;
    }
    void controller(std::size_t track,int channel,int number,int value) noexcept {
        if(track<tracks.size()&&!racks[track].frozen&&channel>=1&&channel<=16&&number>=0&&number<128&&value>=0&&value<128)
            midi[track].addEvent(juce::MidiMessage::controllerEvent(channel,number,value),sampleOffset);
    }
    void pitchWheel(std::size_t track,int channel,int value) noexcept {
        if(track<tracks.size()&&!racks[track].frozen&&channel>=1&&channel<=16)
            midi[track].addEvent(juce::MidiMessage::pitchWheel(channel,std::clamp(value,0,16383)),sampleOffset);
    }
    void note(std::size_t track, int note, float velocity, bool on, int channel = 1) noexcept {
        if(racks[track].frozen)return;
        // Bound events to the preallocated MIDI storage. Reserve the final
        // portion for a panic instead of reallocating under an event flood.
        auto& m = midi[track];
        if (++eventCounts[track] >= 4096) { panic.store(true); return; }
        m.addEvent(on ? juce::MidiMessage::noteOn(channel,note,velocity) : juce::MidiMessage::noteOff(channel,note), sampleOffset);
    }
    int rackLatency(const Rack& rack) const noexcept {
        if(rack.frozen)return 0;
        int latency = slotLatency(rack.instrument);
        for (int i = 0; i < rack.effectCount; ++i) latency += slotLatency(rack.effects[i]);
        return std::clamp(latency,0,maxDelay);
    }
    // Instrument replaces built-in synth/drum audio. A bypassed instrument is
    // silent; FX bypass passes audio through. Imported clips are added after it.
    void instruments(int samples) {
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            auto& slot = racks[t].instrument;
            if (!racks[t].instrumentAssigned || racks[t].frozen) continue;
            tracks[t].clear();
            if (slot.processor && !slot.bypass) process(slot, static_cast<int>(t), -1, tracks[t], midi[t], samples);
        }
    }
    void effectsAndAlign(int samples) {
        // What each track plays before its own FX: the key any sidechain hears this block.
        samples = std::min(samples, blockSize);
        for (std::size_t t = 0; t < tracks.size(); ++t)
            for (int c = 0; c < 2; ++c) std::memcpy(keys[t][static_cast<std::size_t>(c)].data(), tracks[t].getReadPointer(c), sizeof(float) * static_cast<std::size_t>(samples));
        int maximum = 0;
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            auto& rack = racks[t];
            if(!rack.frozen)for (int i = 0; i < rack.effectCount; ++i) { emptyMidi.clear(); process(rack.effects[i],static_cast<int>(t),i,tracks[t],emptyMidi,samples); }
            maximum = std::max(maximum,rackLatency(rack));
        }
        for (std::size_t t = 0; t < tracks.size(); ++t) delays[t].process(tracks[t],samples,maximum-rackLatency(racks[t]));
        delays[maxSessionTracks].process(live,samples,maximum);
    }
    // Runs each group bus's FX, then lines everything up at the bus stage: every bus waits for the
    // slowest bus chain, and what skips the buses - the tracks summed straight into `master`, and
    // what the tracks send to the returns - waits as long, so a track arrives at the master (and
    // at its returns) at the same moment whichever way it goes.
    void processBuses(int samples) {
        const int slowest = busLatency();
        for (int b = 0; b < maxBuses; ++b) {
            auto& rack = racks[static_cast<std::size_t>(busIndex(b))];
            auto& buffer = buses[static_cast<std::size_t>(b)];
            if(!rack.frozen)for (int i = 0; i < rack.effectCount; ++i) { emptyMidi.clear(); process(rack.effects[i], busIndex(b), i, buffer, emptyMidi, samples); }
            busDelays[static_cast<std::size_t>(b)].process(buffer, samples, slowest - rackLatency(rack));
        }
        for (int r = 0; r < maxReturns; ++r)
            if (returnLive[static_cast<std::size_t>(r)]) sendDelays[static_cast<std::size_t>(r)].process(returns[static_cast<std::size_t>(r)], samples, slowest);
        directDelay.process(master, samples, slowest);
    }
    // The delay the bus chains add (the slowest one).
    int busLatency() const noexcept {
        int slowest = 0;
        for (int b = 0; b < maxBuses; ++b) slowest = std::max(slowest, rackLatency(racks[static_cast<std::size_t>(busIndex(b))]));
        return slowest;
    }
    // Runs each return's FX, then lines everything up: every return waits for the slowest return
    // chain, and the dry mix already summed into `master` waits as long, so what a track sends
    // arrives together with the track itself.
    void processReturns(int samples) {
        int slowest = 0;
        for (int r = 0; r < maxReturns; ++r) {
            if (!returnLive[static_cast<std::size_t>(r)]) continue;
            auto& rack = racks[static_cast<std::size_t>(returnIndex(r))];
            if(!rack.frozen)for (int i = 0; i < rack.effectCount; ++i) { emptyMidi.clear(); process(rack.effects[i], returnIndex(r), i, returns[static_cast<std::size_t>(r)], emptyMidi, samples); }
            slowest = std::max(slowest, rackLatency(rack));
        }
        for (int r = 0; r < maxReturns; ++r)
            if (returnLive[static_cast<std::size_t>(r)]) returnDelays[static_cast<std::size_t>(r)].process(returns[static_cast<std::size_t>(r)], samples,
                                                              slowest - rackLatency(racks[static_cast<std::size_t>(returnIndex(r))]));
        dryDelay.process(master, samples, slowest);
    }
    // Audio thread, before the mix: whether return slot `r` exists. A slot not in use is skipped (its
    // buffer stays silent and costs nothing); one that comes back starts with empty delay lines, so
    // nothing left over from the return that was there before can play.
    void setReturnActive(int r, bool active) noexcept {
        if (r < 0 || r >= maxReturns) return;
        const auto i = static_cast<std::size_t>(r);
        if (active && !returnLive[i]) { returnDelays[i].clear(); sendDelays[i].clear(); }
        returnLive[i] = active;
    }
    // The delay the return chains add (the slowest one).
    int returnLatency() const noexcept {
        int slowest = 0;
        for (int r = 0; r < maxReturns; ++r) slowest = std::max(slowest, rackLatency(racks[static_cast<std::size_t>(returnIndex(r))]));
        return slowest;
    }
    void processMaster(int samples) {
        for (int i = 0; i < racks[masterIndex].effectCount; ++i) { emptyMidi.clear(); process(racks[masterIndex].effects[i], masterIndex, i, master, emptyMidi, samples); }
        renderedSamples += samples;
    }
    void prepareSlot(Slot& slot,int rack=-1,int index=-2) {
        if (!slot.processor || slot.faulted.get()) return;
        release(slot,rack,index);
        if (slot.faulted.get()) return;
        fault::Fault fault;
        const bool ready=fault::guarded([&]{
            slot.processor->setPlayHead(&playHead);
            slot.processor->setRateAndBufferSizeDetails(rate,blockSize);
            slot.processor->prepareToPlay(rate,blockSize);
        },fault);
        if (!ready) { markFaulted(slot,rack,index,PluginPhase::prepare,fault); return; }
        slot.prepared=true;
    }
    int latencySamples() const noexcept {
        int maximum = 0;
        for (int t = 0; t < maxSessionTracks; ++t) maximum = std::max(maximum,rackLatency(racks[t]));
        return maximum + busLatency() + returnLatency() + rackLatency(racks[masterIndex]);
    }
    // Asks each plugin how long it rings on. A plugin that fails the question is disabled like any other.
    double tailSeconds() {
        auto slotTail = [this](Slot& slot,int rack,int index) {
            if (!slot.processor || slot.bypass || slot.faulted.get()) return 0.0;
            double value=0; fault::Fault fault;
            if (!fault::guarded([&]{ value=slot.processor->getTailLengthSeconds(); },fault)) { markFaulted(slot,rack,index,PluginPhase::query,fault); return 0.0; }
            return value;
        };
        auto rackTail = [&](Rack& rack,int rackIndex) {
            if(rack.frozen)return 0.0;
            double tail = slotTail(rack.instrument,rackIndex,-1);
            for (int i=0;i<rack.effectCount;++i) tail += slotTail(rack.effects[static_cast<std::size_t>(i)],rackIndex,i);
            return std::isfinite(tail) ? std::clamp(tail,0.0,30.0) : 30.0;
        };
        double longest=0, busTail=0, returnTail=0;
        for(int t=0;t<maxSessionTracks;++t)longest=std::max(longest,rackTail(racks[static_cast<std::size_t>(t)],t));
        // A bus or a return rings on after the track that feeds it, and the master after that.
        for(int b=0;b<maxBuses;++b)busTail=std::max(busTail,rackTail(racks[static_cast<std::size_t>(busIndex(b))],busIndex(b)));
        for(int r=0;r<maxReturns;++r)returnTail=std::max(returnTail,rackTail(racks[static_cast<std::size_t>(returnIndex(r))],returnIndex(r)));
        return std::min(30.0,longest+std::max(busTail,returnTail)+rackTail(racks[masterIndex],masterIndex));
    }
private:
    struct Delay {
        std::vector<float> left,right;
        int position{};
        void prepare() { left.assign(maxDelay+1,0); right.assign(maxDelay+1,0); position=0; }
        void clear() noexcept { std::fill(left.begin(),left.end(),0.0f); std::fill(right.begin(),right.end(),0.0f); position=0; }
        void process(juce::AudioBuffer<float>& buffer,int samples,int count) noexcept {
            for (int i=0;i<samples;++i) {
                left[position]=buffer.getSample(0,i); right[position]=buffer.getSample(1,i);
                const int read=(position-count+maxDelay+1)%(maxDelay+1);
                buffer.setSample(0,i,left[read]); buffer.setSample(1,i,right[read]);
                position=(position+1)%(maxDelay+1);
            }
        }
    };
    // A disabled plugin adds no delay, so the other tracks stop waiting for it.
    static int slotLatency(const Slot& slot) noexcept { return slot.processor && !slot.bypass && !slot.faulted.get() ? std::clamp(slot.processor->getLatencySamples(),0,maxDelay) : 0; }
    // index -1 is the instrument. An effect that fails hands back the signal it was given; an
    // instrument that fails goes silent. Either way this block is clean and the slot is off.
    void process(Slot& slot,int rack,int index,juce::AudioBuffer<float>& buffer,juce::MidiBuffer& messages,int samples) {
        if (!slot.processor || slot.bypass || slot.faulted.get()) return;
        // A keyed effect takes its sidechain as channels 2 and 3 (a copy, so it cannot spoil the key for another).
        // Only a key the mix has made by the time this rack runs is handed over.
        const auto* key=index>=0 && slot.keyIndex>=0 && slot.keyIndex<keySlots(slot.keyFrom) && keyReady(stageOf(rack),slot.keyFrom)
            ? &keyFor(slot.keyFrom,slot.keyIndex) : nullptr;
        const bool keyed=key!=nullptr && slot.processor->getTotalNumInputChannels()==4;
        if (slot.processor->getTotalNumOutputChannels()!=2 || (slot.processor->getTotalNumInputChannels()>2 && !keyed)) return;
        samples=std::min(samples,blockSize);
        // Stack view, no heap or channel-pointer allocation for stereo buffers.
        float* pointers[]{buffer.getWritePointer(0),buffer.getWritePointer(1),keyScratch[0].data(),keyScratch[1].data()};
        const bool effect=index>=0;
        if (effect) for (int c=0;c<2;++c) std::memcpy(dry[static_cast<std::size_t>(c)].data(),pointers[c],sizeof(float)*static_cast<std::size_t>(samples));
        if (keyed) for (int c=0;c<2;++c)
            std::memcpy(keyScratch[static_cast<std::size_t>(c)].data(),(*key)[static_cast<std::size_t>(c)].data(),sizeof(float)*static_cast<std::size_t>(samples));
        juce::AudioBuffer<float> view(pointers,keyed?4:2,samples);
        fault::Fault fault;
        const bool completed=fault::guarded([&]{ slot.processor->processBlock(view,messages); },fault);
        const bool producedGarbage=completed && !(alien::allFinite(pointers[0],samples) && alien::allFinite(pointers[1],samples));
        if (completed && !producedGarbage) return;
        markFaulted(slot,rack,index,PluginPhase::process,fault,producedGarbage);
        for (int c=0;c<2;++c) {
            if (effect) std::memcpy(pointers[c],dry[static_cast<std::size_t>(c)].data(),sizeof(float)*static_cast<std::size_t>(samples));
            else std::memset(pointers[c],0,sizeof(float)*static_cast<std::size_t>(samples));
        }
    }
    static std::atomic<std::size_t>& quarantineTally() { static std::atomic<std::size_t> tally{0}; return tally; }
    using Key = std::array<std::array<float,blockSize>,2>;
    template <std::size_t N>
    static void store(std::array<Key,N>& set, std::size_t which, int i, float left, float right) noexcept {
        if (which >= N || i < 0 || i >= blockSize) return;
        set[which][0][static_cast<std::size_t>(i)] = left; set[which][1][static_cast<std::size_t>(i)] = right;
    }
    const Key& keyFor(KeySource from, int index) const noexcept {
        const auto i = static_cast<std::size_t>(index);
        switch (from) {
        case KeySource::trackPostFader: return postKeys[i];
        case KeySource::bus: return busKeys[i];
        case KeySource::returnTrack: return returnKeys[i];
        case KeySource::track: break;
        }
        return keys[i];
    }
    std::array<std::array<float,blockSize>,2> dry{};
    std::array<Key,maxSessionTracks> keys{};       // each track before its FX, this block
    std::array<Key,maxSessionTracks> postKeys{};   // each track after its fader and pan
    std::array<Key,maxBuses> busKeys{};            // each bus after its strip
    std::array<Key,maxReturns> returnKeys{};       // each return after its strip
    std::array<std::array<float,blockSize>,2> keyScratch{};
    std::array<Delay,maxSessionTracks+1> delays;
    std::array<Delay,maxReturns> returnDelays;
    std::array<Delay,maxBuses> busDelays;
    std::array<Delay,maxReturns> sendDelays;   // what the tracks send, waiting for the bus stage
    std::array<bool,maxReturns> returnLive = [] { std::array<bool,maxReturns> live{}; live.fill(true); return live; }();   // audio thread: setReturnActive
    Delay dryDelay, directDelay;
    juce::MidiBuffer emptyMidi;
    double rate{44100};
    juce::int64 renderedSamples{};
    int currentSamples{};
};
}
