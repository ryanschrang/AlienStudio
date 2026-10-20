#pragma once
#include "SessionCapacity.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>

namespace alien {
// Only the audio thread uses buffers/MIDI/delay state. Slot changes, native
// editors and state capture require the owner to detach the device callback.
class PluginRuntime {
public:
    static constexpr int blockSize = 512, maxDelay = 96000, masterIndex = maxSessionTracks;
    struct PlayHead final : juce::AudioPlayHead {
        PositionInfo info;
        juce::Optional<PositionInfo> getPosition() const override { return info; }
    } playHead;
    struct Slot {
        std::unique_ptr<juce::AudioProcessor> processor;
        bool bypass{}, prepared{};
    };
    struct Rack {
        Slot instrument;
        bool instrumentAssigned{};
        std::array<Slot,8> effects;
        int effectCount{};
    };
    std::array<Rack, maxSessionTracks + 1> racks;
    std::array<juce::AudioBuffer<float>, maxSessionTracks> tracks;
    juce::AudioBuffer<float> live, master;
    std::array<juce::MidiBuffer, maxSessionTracks> midi;
    int sampleOffset{};
    std::array<int,maxSessionTracks> eventCounts{};
    std::atomic<bool> panic{};

    PluginRuntime() { prepare(44100); }
    ~PluginRuntime() {
        for (auto& rack:racks) { release(rack.instrument); for(auto& slot:rack.effects)release(slot); }
    }
    static void release(Slot& slot) { if(slot.processor && slot.prepared){slot.processor->releaseResources();slot.prepared=false;} }
    void prepare(double sampleRate) {
        rate = sampleRate; renderedSamples = 0;
        for (auto& b : tracks) b.setSize(2, blockSize);
        live.setSize(2, blockSize); master.setSize(2, blockSize);
        for (auto& m : midi) m.ensureSize(65536);
        emptyMidi.ensureSize(65536);
        for (auto& delay : delays) delay.prepare();
        for (auto& rack : racks) {
            prepareSlot(rack.instrument);
            for (auto& slot : rack.effects) prepareSlot(slot);
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
    void begin(int samples, int bpm, bool playing, double ppq) noexcept {
        for (auto& b : tracks) b.clear(); live.clear(); master.clear();
        for (auto& m : midi) m.clear();
        sampleOffset = 0; currentSamples = samples; eventCounts.fill(0);
        playHead.info.setBpm(static_cast<double>(bpm));
        const auto timelineSamples = static_cast<juce::int64>(std::llround(ppq * rate * 60.0 / bpm));
        playHead.info.setTimeInSamples(timelineSamples);
        playHead.info.setTimeInSeconds(timelineSamples / rate);
        playHead.info.setPpqPosition(ppq);
        playHead.info.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
        playHead.info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4,4});
        playHead.info.setIsPlaying(playing);
        if (panic.exchange(false)) for (auto& m : midi)
            for (int ch = 1; ch <= 16; ++ch) { m.addEvent(juce::MidiMessage::allSoundOff(ch),0); m.addEvent(juce::MidiMessage::allNotesOff(ch),0); }
    }
    bool hasInstrument(std::size_t track) const noexcept { return racks[track].instrumentAssigned; }
    void controller(std::size_t track,int channel,int number,int value) noexcept {
        if(track<tracks.size()&&channel>=1&&channel<=16&&number>=0&&number<128&&value>=0&&value<128)
            midi[track].addEvent(juce::MidiMessage::controllerEvent(channel,number,value),sampleOffset);
    }
    void pitchWheel(std::size_t track,int channel,int value) noexcept {
        if(track<tracks.size()&&channel>=1&&channel<=16)
            midi[track].addEvent(juce::MidiMessage::pitchWheel(channel,std::clamp(value,0,16383)),sampleOffset);
    }
    void note(std::size_t track, int note, float velocity, bool on, int channel = 1) noexcept {
        // Bound events to the preallocated MIDI storage. Reserve the final
        // portion for a panic instead of reallocating under an event flood.
        auto& m = midi[track];
        if (++eventCounts[track] >= 4096) { panic.store(true); return; }
        m.addEvent(on ? juce::MidiMessage::noteOn(channel,note,velocity) : juce::MidiMessage::noteOff(channel,note), sampleOffset);
    }
    int rackLatency(const Rack& rack) const noexcept {
        int latency = slotLatency(rack.instrument);
        for (int i = 0; i < rack.effectCount; ++i) latency += slotLatency(rack.effects[i]);
        return std::clamp(latency,0,maxDelay);
    }
    // Instrument replaces built-in synth/drum audio. A bypassed instrument is
    // silent; FX bypass passes audio through. Imported clips are added after it.
    void instruments(int samples) {
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            auto& slot = racks[t].instrument;
            if (!racks[t].instrumentAssigned) continue;
            tracks[t].clear();
            if (slot.processor && !slot.bypass) process(slot, tracks[t], midi[t], samples);
        }
    }
    void effectsAndAlign(int samples) {
        int maximum = 0;
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            auto& rack = racks[t];
            for (int i = 0; i < rack.effectCount; ++i) { emptyMidi.clear(); process(rack.effects[i],tracks[t],emptyMidi,samples); }
            maximum = std::max(maximum,rackLatency(rack));
        }
        for (std::size_t t = 0; t < tracks.size(); ++t) delays[t].process(tracks[t],samples,maximum-rackLatency(racks[t]));
        delays[maxSessionTracks].process(live,samples,maximum);
    }
    void processMaster(int samples) {
        for (int i = 0; i < racks[masterIndex].effectCount; ++i) { emptyMidi.clear(); process(racks[masterIndex].effects[i], master, emptyMidi, samples); }
        renderedSamples += samples;
    }
    void prepareSlot(Slot& slot) {
        if (!slot.processor) return;
        release(slot);
        slot.processor->setPlayHead(&playHead);
        slot.processor->setRateAndBufferSizeDetails(rate,blockSize);
        slot.processor->prepareToPlay(rate,blockSize);
        slot.prepared=true;
    }
    int latencySamples() const noexcept {
        int maximum = 0;
        for (int t = 0; t < maxSessionTracks; ++t) maximum = std::max(maximum,rackLatency(racks[t]));
        return maximum + rackLatency(racks[masterIndex]);
    }
    double tailSeconds() const {
        auto rackTail = [](const Rack& rack) {
            double tail = rack.instrument.processor && !rack.instrument.bypass ? rack.instrument.processor->getTailLengthSeconds() : 0;
            for (int i=0;i<rack.effectCount;++i) if(rack.effects[i].processor && !rack.effects[i].bypass)
                tail += rack.effects[i].processor->getTailLengthSeconds();
            return std::isfinite(tail) ? std::clamp(tail,0.0,30.0) : 30.0;
        };
        double longest=0;
        for(int t=0;t<maxSessionTracks;++t)longest=std::max(longest,rackTail(racks[t]));
        return std::min(30.0,longest+rackTail(racks[masterIndex]));
    }
private:
    struct Delay {
        std::vector<float> left,right;
        int position{};
        void prepare() { left.assign(maxDelay+1,0); right.assign(maxDelay+1,0); position=0; }
        void process(juce::AudioBuffer<float>& buffer,int samples,int count) noexcept {
            for (int i=0;i<samples;++i) {
                left[position]=buffer.getSample(0,i); right[position]=buffer.getSample(1,i);
                const int read=(position-count+maxDelay+1)%(maxDelay+1);
                buffer.setSample(0,i,left[read]); buffer.setSample(1,i,right[read]);
                position=(position+1)%(maxDelay+1);
            }
        }
    };
    static int slotLatency(const Slot& slot) noexcept { return slot.processor && !slot.bypass ? std::clamp(slot.processor->getLatencySamples(),0,maxDelay) : 0; }
    static void process(Slot& slot,juce::AudioBuffer<float>& buffer,juce::MidiBuffer& messages,int samples) {
        if (!slot.processor || slot.bypass) return;
        if (slot.processor->getTotalNumOutputChannels()!=2 || slot.processor->getTotalNumInputChannels()>2) return;
        // Stack view, no heap or channel-pointer allocation for stereo buffers.
        float* pointers[]{buffer.getWritePointer(0),buffer.getWritePointer(1)};
        juce::AudioBuffer<float> view(pointers,2,samples);
        slot.processor->processBlock(view,messages);
    }
    std::array<Delay,maxSessionTracks+1> delays;
    juce::MidiBuffer emptyMidi;
    double rate{44100};
    juce::int64 renderedSamples{};
    int currentSamples{};
};
}
