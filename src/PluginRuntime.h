#pragma once
#include "ParameterAutomation.h"
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
    // Channel Rack channels played by plugin instruments (T1-09): one slot per channel, reported in faults as
    // "rack" channelBase + channel (outside the mixer's racks).
    static constexpr int maxRackChannels = 64, channelBase = 1000;
    // T1-04 D9: the most notes one block gives a rack channel's instrument: the rack player's whole list for the
    // block can belong to one channel, so each channel's MIDI storage holds all of it (no note-off is cut off).
    static constexpr int rackNotesPerBlock = 1024, rackLiveEvents = 256;
    // Bytes a JUCE MidiBuffer takes for a three-byte message (time stamp, size, data), and the most a channel's
    // buffer holds in one block: the all-notes/sound-off panic on 16 channels, the live events and the notes.
    static constexpr std::size_t midiEventBytes = sizeof(std::int32_t) + sizeof(std::uint16_t) + 3;
    static constexpr std::size_t rackChannelMidiBytes = (32 + rackLiveEvents + rackNotesPerBlock) * midiEventBytes;
    static constexpr bool isRackChannel(int rack) noexcept { return rack >= channelBase && rack < channelBase + maxRackChannels; }
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
        juce::String description;   // the plugin's saved description (message thread), so a crash can be traced to its file
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
    std::array<Slot, maxRackChannels> rackChannels;   // the rack's plugin instruments, by channel
    std::array<juce::AudioBuffer<float>, maxSessionTracks> tracks;
    juce::AudioBuffer<float> live, master;
    std::array<juce::AudioBuffer<float>, maxReturns> returns;   // what the tracks send to each return, then its FX
    std::array<juce::AudioBuffer<float>, maxBuses> buses;       // the tracks routed to each group bus, then its FX
    std::array<juce::MidiBuffer, maxSessionTracks> midi;
    int sampleOffset{};
    // T1-04 D11: the most MIDI events (notes, controllers, pitch bend) a track's instrument is given in one block.
    // Past it the event is dropped and counted, and the next block tells every instrument to end its notes, so
    // a dropped note-off cannot leave one held. The track buffers hold that many, their panics and some silences.
    static constexpr int trackEventsPerBlock = 4096;
    static constexpr std::size_t trackMidiBytes = 65536;
    static_assert((trackEventsPerBlock + 32 * 8) * midiEventBytes <= trackMidiBytes, "a full block of track MIDI never reallocates");
    // Audio thread writes, any thread reads: events for track instruments dropped since the runtime was made.
    std::uint64_t droppedTrackEvents() const noexcept { return droppedTrack.load(std::memory_order_relaxed); }
    std::array<int,maxSessionTracks> eventCounts{};
    std::atomic<bool> panic{};
    FaultQueue faults;                          // read by the message thread (PluginHost::poll)
    // Which plugin parameter each automation target is, sorted (ParameterAutomation.h). Replaced by the
    // host while the device callback is detached, after every restore; read by the audio thread.
    std::vector<automation::ParameterBinding> parameterBindings;
    std::atomic<std::uint32_t> faultCount{0};   // every fault ever recorded here; an export compares before and after

    PluginRuntime() { prepare(44100); }
    PluginRuntime(const PluginRuntime&) = delete;
    PluginRuntime& operator=(const PluginRuntime&) = delete;
    ~PluginRuntime() {
        for (auto& stage : stages) { delete stage.pending.exchange(nullptr); delete stage.retired.exchange(nullptr); }
        for (auto& stage : channelStages) { delete stage.pending.exchange(nullptr); delete stage.retired.exchange(nullptr); }
        for (int r=0;r<rackCount;++r) {
            destroy(racks[static_cast<std::size_t>(r)].instrument,r,-1);
            for (int i=0;i<static_cast<int>(racks[static_cast<std::size_t>(r)].effects.size());++i) destroy(racks[static_cast<std::size_t>(r)].effects[static_cast<std::size_t>(i)],r,i);
        }
        for (int c=0;c<maxRackChannels;++c) destroy(rackChannels[static_cast<std::size_t>(c)],channelBase+c,-1);
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
        for (auto& m : midi) m.ensureSize(trackMidiBytes);
        for (auto& m : channelMidi) m.ensureSize(rackChannelMidiBytes);
        channelScratch.setSize(2, blockSize);
        emptyMidi.ensureSize(65536);
        for (int r=0;r<rackCount;++r) {
            auto& rack=racks[static_cast<std::size_t>(r)];
            prepareSlot(rack.instrument,r,-1);
            for (int i=0;i<static_cast<int>(rack.effects.size());++i) prepareSlot(rack.effects[static_cast<std::size_t>(i)],r,i);
        }
        for (int c=0;c<maxRackChannels;++c) prepareSlot(rackChannels[static_cast<std::size_t>(c)],channelBase+c,-1);
        // Every stage starts empty at its new size; the callback is detached, so adopt it now.
        reserveAlignment(true);
        adoptAlignment();
        reclaimAlignment();
        channelPanic = true;
        panic.store(true);
    }
    // ---- Alignment delay budgets (T1-04 D) --------------------------------------------------------------
    // The delay lines that line tracks, rack instruments, buses and returns up behind the slowest plugin
    // chain are sized to the latency the loaded plugins report, not to maxDelay: a project without latent
    // plugins holds no alignment audio at all. There are four stages, each sized on its own, so a stage
    // that grows does not disturb the others.
    enum AlignmentStage { trackAlignment, sourceAlignment, busAlignment, returnAlignment, alignmentStageCount };
    // A stage's line length is the latency it needs, rounded up to this many samples (0 stays 0), so a
    // plugin whose latency moves a little does not reallocate every time.
    static constexpr int alignmentGranule = 1024;
    static constexpr int roundedAlignment(int samples) noexcept {
        samples = std::clamp(samples, 0, maxDelay);
        return samples == 0 ? 0 : std::min(maxDelay, (samples + alignmentGranule - 1) / alignmentGranule * alignmentGranule);
    }
    // The latency each stage must hold for the plugins as they report themselves now, on every track
    // slot (not only those in use, so a track coming into use never needs more).
    std::array<int, alignmentStageCount> alignmentNeeded() const noexcept {
        int track = 0;
        for (int t = 0; t < maxSessionTracks; ++t) track = std::max(track, rackLatency(racks[static_cast<std::size_t>(t)]));
        return {track, rackChannelLatency(), busLatency(), returnLatency()};
    }
    // Message thread (or the owner of an idle engine). Allocates new storage for each stage that needs
    // more than it has, or, with `shrink`, a different amount; the audio thread adopts it at its next
    // begin(). Never blocks the callback. Returns how many stages were replaced.
    int reserveAlignment(bool shrink = false) {
        reclaimAlignment();
        const auto needed = alignmentNeeded();
        int replaced = 0;
        for (int s = 0; s < alignmentStageCount; ++s) {
            auto& stage = stages[static_cast<std::size_t>(s)];
            const int target = roundedAlignment(needed[static_cast<std::size_t>(s)]);
            if (stage.published >= 0 && (target == stage.published || (!shrink && target < stage.published))) continue;
            auto storage = std::make_unique<AlignmentStorage>();
            storage->capacity = target;
            storage->samples.assign(static_cast<std::size_t>(linesIn(static_cast<AlignmentStage>(s))) * 2u * static_cast<std::size_t>(target + 1), 0.0f);
            delete stage.pending.exchange(storage.release(), std::memory_order_acq_rel);   // one the callback never took
            stage.published = target;
            ++replaced;
        }
        // T1-04 D15: each loaded Channel Rack instrument's line, as long as the slowest instrument's latency (the
        // most it waits). A channel that never had an instrument holds nothing; one whose instrument went keeps
        // its line until a shrink. A freshly loaded instrument always gets a new, empty line (see prepareSlot).
        const int channelTarget = roundedAlignment(needed[sourceAlignment]);
        for (int c = 0; c < maxRackChannels; ++c) {
            auto& stage = channelStages[static_cast<std::size_t>(c)];
            const bool loaded = rackChannels[static_cast<std::size_t>(c)].processor != nullptr;
            const int target = loaded ? channelTarget : 0;
            if (!loaded && (stage.published < 0 || !shrink)) continue;
            if (stage.published >= 0 && (target == stage.published || (!shrink && target < stage.published))) continue;
            auto storage = std::make_unique<AlignmentStorage>();
            storage->capacity = target;
            storage->samples.assign(2u * static_cast<std::size_t>(target + 1), 0.0f);
            delete stage.pending.exchange(storage.release(), std::memory_order_acq_rel);
            stage.published = target;
            ++replaced;
        }
        return replaced;
    }
    // Message thread: frees the storage the audio thread has stopped using.
    void reclaimAlignment() noexcept {
        for (auto& stage : stages) delete stage.retired.exchange(nullptr, std::memory_order_acq_rel);
        for (auto& stage : channelStages) delete stage.retired.exchange(nullptr, std::memory_order_acq_rel);
    }
    // The samples a stage's lines can hold now (what the callback uses), or -1 before any was adopted.
    int alignmentCapacity(AlignmentStage stage) const noexcept {
        const auto& s = stages[static_cast<std::size_t>(stage)];
        return s.active ? s.active->capacity : -1;
    }
    // The samples Channel Rack channel `c`'s instrument line holds now (what the callback uses), or -1 if it has
    // none (D15).
    int channelAlignmentCapacity(int c) const noexcept {
        if (c < 0 || c >= maxRackChannels) return -1;
        const auto& s = channelStages[static_cast<std::size_t>(c)];
        return s.active ? s.active->capacity : -1;
    }
    // Bytes of alignment audio in use by the callback, across the four stages and the rack instruments' lines.
    std::size_t alignmentBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& stage : stages) if (stage.active) bytes += stage.active->samples.size() * sizeof(float);
        for (const auto& stage : channelStages) if (stage.active) bytes += stage.active->samples.size() * sizeof(float);
        return bytes;
    }
    // Blocks in which some line had to delay less than it should, because a plugin reported more latency
    // than its stage held (until reserveAlignment grows it). An export compares before and after.
    std::uint32_t alignmentShortfalls() const noexcept { return shortfalls.load(std::memory_order_acquire); }
    // The latency a chain's plugins report now, before it is clamped to maxDelay (T1-04 D13): mixer rack `rack`
    // (a track, the master, a bus or a return) or Channel Rack instrument channelBase + channel. Anything above
    // maxDelay cannot be lined up, so that chain plays late by the difference. Message thread.
    std::int64_t reportedLatency(int rack) const noexcept {
        const auto raw = [](const Slot& slot) -> std::int64_t {
            return slot.processor && !slot.bypass && !slot.faulted.get() ? std::max(0, slot.processor->getLatencySamples()) : 0;
        };
        if (isRackChannel(rack)) return raw(rackChannels[static_cast<std::size_t>(rack - channelBase)]);
        if (rack < 0 || rack >= rackCount) return 0;
        const auto& chain = racks[static_cast<std::size_t>(rack)];
        if (chain.frozen) return 0;
        std::int64_t latency = raw(chain.instrument);
        for (int i = 0; i < chain.effectCount; ++i) latency += raw(chain.effects[static_cast<std::size_t>(i)]);
        return latency;
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
        adoptAlignment(); shortBlock = false;
        if (panic.exchange(false)) for (auto& m : midi)
            for (int ch = 1; ch <= 16; ++ch) { m.addEvent(juce::MidiMessage::allSoundOff(ch),0); m.addEvent(juce::MidiMessage::allNotesOff(ch),0); }
    }
    bool hasInstrument(std::size_t track) const noexcept { return racks[track].instrumentAssigned; }
    // Track slots processed this block (T1-04 C), after begin(): slots 0..n-1. A slot leaving use stops keying
    // sidechains and its delay lines forget what they hold, so it never plays again; a slot coming into use tells
    // its instrument to end every note. Slots out of use cost nothing in instruments(), effectsAndAlign() or alignment.
    void useTracks(std::size_t n) noexcept {
        n = std::clamp<std::size_t>(n, 1, tracks.size());
        for (auto t = n; t < inUse; ++t) { keys[t] = {}; postKeys[t] = {}; delays[t].forget(); sourceDelays[t].forget(); }
        for (auto t = inUse; t < n; ++t) silence(t);
        inUse = n;
    }
    std::size_t tracksInUse() const noexcept { return inUse; }
    // The mixer hands over the keys it makes, sample by sample: each track after its fader and pan,
    // each bus after its strip, each return after its strip (MIX-04).
    void keyPostFader(std::size_t track, int i, float left, float right) noexcept { store(postKeys, track, i, left, right); }
    void keyBus(std::size_t bus, int i, float left, float right) noexcept { store(busKeys, bus, i, left, right); }
    // A bus that is silent for the whole block keys silence, as keyBus would for each of its samples (T1-04 D27).
    void silentBusKey(std::size_t bus, int samples) noexcept {
        if (bus >= busKeys.size()) return;
        const auto count = static_cast<std::size_t>(std::clamp(samples, 0, blockSize));
        for (auto& side : busKeys[bus]) std::fill_n(side.begin(), count, 0.0f);
    }
    void keyReturn(std::size_t r, int i, float left, float right) noexcept { store(returnKeys, r, i, left, right); }
    // Where in the mix a rack runs, which decides the keys it can hear.
    static constexpr RackStage stageOf(int rack) noexcept {
        return rack < maxSessionTracks ? RackStage::track : rack == masterIndex ? RackStage::master
             : isReturn(rack) ? RackStage::returnTrack : RackStage::bus;
    }
    void controller(std::size_t track,int channel,int number,int value) noexcept {
        if(track<tracks.size()&&!racks[track].frozen&&channel>=1&&channel<=16&&number>=0&&number<128&&value>=0&&value<128&&roomFor(track))
            midi[track].addEvent(juce::MidiMessage::controllerEvent(channel,number,value),sampleOffset);
    }
    void pitchWheel(std::size_t track,int channel,int value) noexcept {
        if(track<tracks.size()&&!racks[track].frozen&&channel>=1&&channel<=16&&roomFor(track))
            midi[track].addEvent(juce::MidiMessage::pitchWheel(channel,std::clamp(value,0,16383)),sampleOffset);
    }
    void note(std::size_t track, int note, float velocity, bool on, int channel = 1) noexcept {
        if(track>=tracks.size()||racks[track].frozen||!roomFor(track))return;
        midi[track].addEvent(on ? juce::MidiMessage::noteOn(channel,note,velocity) : juce::MidiMessage::noteOff(channel,note), sampleOffset);
    }
    // Audio thread: whether the track's MIDI has room for one more event this block (trackEventsPerBlock). If
    // not, the event is counted as dropped and the next block ends every note, instead of reallocating.
    bool roomFor(std::size_t track) noexcept {
        if (eventCounts[track] < trackEventsPerBlock) { ++eventCounts[track]; return true; }
        droppedTrack.store(droppedTrack.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        panic.store(true);
        return false;
    }
    int rackLatency(const Rack& rack) const noexcept {
        if(rack.frozen)return 0;
        int latency = slotLatency(rack.instrument);
        for (int i = 0; i < rack.effectCount; ++i) latency += slotLatency(rack.effects[i]);
        return std::clamp(latency,0,maxDelay);
    }
    // Audio thread, before the block is processed: sets each automated plugin parameter to its lane's value at
    // `tick`, so automation is applied once per block (at most blockSize samples late). A lane whose plugin is
    // missing, replaced or switched off does nothing. No allocation, no lock; the call into the plugin is
    // contained like every other. Returns how many parameters were changed.
    int applyParameterAutomation(const std::vector<automation::ParameterLane>& lanes, double tick) noexcept {
        int changed = 0;
        for (const auto& lane : lanes) {
            if (!lane.enabled || lane.points.empty()) continue;
            const auto* binding = automation::findSorted(lane.target, parameterBindings);
            if (binding == nullptr || binding->rack < 0 || (binding->rack >= rackCount && !isRackChannel(binding->rack))) continue;
            // A Channel Rack channel's instrument (T1-09) or a slot of a mixer rack.
            auto& slot = isRackChannel(binding->rack) ? rackChannels[static_cast<std::size_t>(binding->rack - channelBase)]
                       : binding->slot < 0 ? racks[static_cast<std::size_t>(binding->rack)].instrument
                       : racks[static_cast<std::size_t>(binding->rack)].effects[static_cast<std::size_t>(binding->slot)];
            if (!slot.processor || slot.faulted.get() || slot.instanceId != binding->instance) continue;
            fault::Fault fault;
            const bool completed = fault::guarded([&] {
                const auto& parameters = slot.processor->getParameters();
                if (binding->parameter >= parameters.size()) return;
                auto* parameter = parameters.getUnchecked(binding->parameter);
                const float current = parameter->getValue();
                const float value = lane.valueAt(tick, current);
                if (value != current) { parameter->setValue(value); ++changed; }
            }, fault);
            if (!completed) markFaulted(slot, binding->rack, binding->slot, PluginPhase::automation, fault);
        }
        return changed;
    }
    // Audio thread: plays one block of a rack channel's plugin instrument. `notes` are this block's notes for it
    // (sample offset, MIDI note, velocity; velocity 0 ends the note). Returns the plugin's stereo output (the
    // channel's fader and pan are the caller's), or nullptr when the channel has no working instrument. A plugin
    // that crashes or produces invalid audio is switched off and contained, as on a track.
    template <class Notes>
    const juce::AudioBuffer<float>* processRackChannel(int channel, int samples, Notes&& notes) noexcept {
        if (channel < 0 || channel >= maxRackChannels) return nullptr;
        auto& slot = rackChannels[static_cast<std::size_t>(channel)];
        if (!slot.processor || slot.faulted.get()) return nullptr;
        samples = std::clamp(samples, 1, blockSize);
        auto& delay = channelDelays[static_cast<std::size_t>(channel)];
        auto& messages = channelMidi[static_cast<std::size_t>(channel)];
        messages.clear();
        if (channelPanic) for (int ch = 1; ch <= 16; ++ch) { messages.addEvent(juce::MidiMessage::allSoundOff(ch), 0); messages.addEvent(juce::MidiMessage::allNotesOff(ch), 0); }
        for (int e = 0; e < liveCount; ++e)
            if (const auto& live = liveEvents[static_cast<std::size_t>(e)]; live.channel == channel) messages.addEvent(live.message, 0);
        int count = 0;
        notes([&](int sample, int note, float velocity) {
            if (++count > rackNotesPerBlock) return;   // never reached: the rack player gives at most that many (D9)
            const int at = std::clamp(sample, 0, samples - 1);
            messages.addEvent(velocity > 0.0f ? juce::MidiMessage::noteOn(1, note, velocity) : juce::MidiMessage::noteOff(1, note), at);
        });
        channelScratch.clear(0, samples);
        process(slot, channelBase + channel, -1, channelScratch, messages, samples);
        if (slot.faulted.get()) return nullptr;
        // Each instrument waits for the slowest one, so all of them come out channelStage samples late. A line too
        // short for that (a latency that grew since the last poll) goes through align() like the other stages (D15);
        // the source stage, sized from the same latency, is short in the same blocks.
        align(delay, channelScratch, samples, std::max(0, channelStage - slotLatency(slot)));
        return &channelScratch;
    }
    // The delay the Channel Rack's plugin instruments add (the slowest one): everything else that reaches the
    // tracks and the rack's master bus waits as long, so a latent rack instrument stays in time with them.
    int rackChannelLatency() const noexcept {
        int slowest = 0;
        for (const auto& slot : rackChannels) slowest = std::max(slowest, slotLatency(slot));
        return slowest;
    }
    // Audio thread, after everything but the rack's plugin instruments has reached the tracks and `live`, before
    // those instruments are played: delays it by the rack instruments' latency (see processRackChannel).
    void alignToRackChannels(int samples) noexcept {
        samples = std::min(samples, blockSize);
        channelStage = rackChannelLatency();
        for (std::size_t t = 0; t < inUse; ++t) align(sourceDelays[t],tracks[t], samples, channelStage);
        align(sourceDelays[maxSessionTracks],live, samples, channelStage);
    }
    // After the rack channels have been played this block.
    // Live MIDI for a rack channel's instrument (T1-09): the hardware input and the typing and on-screen pianos.
    // Audio thread, before the rack channels play: the message reaches that channel's instrument at the start of
    // this block. Each message keeps its channel, so a note-off queued for one instrument never reaches another;
    // a channel without a working instrument drops its messages. Bounded storage, no allocation.
    void liveRack(int channel, const juce::MidiMessage& message) noexcept {
        if (channel < 0 || channel >= maxRackChannels) return;
        // Full: every rack instrument is told all notes off this block, so a dropped note-off cannot leave one stuck.
        if (liveCount == static_cast<int>(liveEvents.size())) { channelPanic = true; return; }
        liveEvents[static_cast<std::size_t>(liveCount++)] = {channel, message};
    }
    // Ends every note a rack channel's instrument is playing from live input (on all MIDI channels).
    void silenceLiveRack(int channel) noexcept {
        for (int ch = 1; ch <= 16; ++ch) { liveRack(channel, juce::MidiMessage::allNotesOff(ch)); liveRack(channel, juce::MidiMessage::allSoundOff(ch)); }
    }
    // After the rack channels have been played this block.
    void rackChannelsDone() noexcept { channelPanic = false; liveCount = 0; }
    bool hasRackChannel(int channel) const noexcept {
        return channel >= 0 && channel < maxRackChannels && rackChannels[static_cast<std::size_t>(channel)].processor != nullptr;
    }
    // Instrument replaces built-in synth/drum audio. A bypassed instrument is
    // silent; FX bypass passes audio through. Imported clips are added after it.
    void instruments(int samples) {
        for (std::size_t t = 0; t < inUse; ++t) {
            auto& slot = racks[t].instrument;
            if (!racks[t].instrumentAssigned || racks[t].frozen) continue;
            tracks[t].clear();
            if (slot.processor && !slot.bypass) process(slot, static_cast<int>(t), -1, tracks[t], midi[t], samples);
        }
    }
    void effectsAndAlign(int samples) {
        // What each track plays before its own FX: the key any sidechain hears this block.
        samples = std::min(samples, blockSize);
        for (std::size_t t = 0; t < inUse; ++t)
            for (int c = 0; c < 2; ++c) std::memcpy(keys[t][static_cast<std::size_t>(c)].data(), tracks[t].getReadPointer(c), sizeof(float) * static_cast<std::size_t>(samples));
        int maximum = 0;
        for (std::size_t t = 0; t < inUse; ++t) {
            auto& rack = racks[t];
            if(!rack.frozen)for (int i = 0; i < rack.effectCount; ++i) { emptyMidi.clear(); process(rack.effects[i],static_cast<int>(t),i,tracks[t],emptyMidi,samples); }
            maximum = std::max(maximum,rackLatency(rack));
        }
        for (std::size_t t = 0; t < inUse; ++t) align(delays[t],tracks[t],samples,maximum-rackLatency(racks[t]));
        align(delays[maxSessionTracks],live,samples,maximum);
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
            align(busDelays[static_cast<std::size_t>(b)],buffer, samples, slowest - rackLatency(rack));
        }
        for (int r = 0; r < maxReturns; ++r)
            if (returnLive[static_cast<std::size_t>(r)]) align(sendDelays[static_cast<std::size_t>(r)],returns[static_cast<std::size_t>(r)], samples, slowest);
        align(directDelay,master, samples, slowest);
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
            if (returnLive[static_cast<std::size_t>(r)]) align(returnDelays[static_cast<std::size_t>(r)],returns[static_cast<std::size_t>(r)], samples,
                                                              slowest - rackLatency(racks[static_cast<std::size_t>(returnIndex(r))]));
        align(dryDelay,master, samples, slowest);
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
        // A rack instrument's delay line is only needed while one is loaded, as long as the slowest rack instrument
        // makes it wait (D15), and starts empty, so nothing an earlier instrument played comes out of it: forget
        // what was published, so the reserve below makes a new line, which the next block takes up.
        if (isRackChannel(rack)) channelStages[static_cast<std::size_t>(rack-channelBase)].published = -1;
        // The alignment stages grow to what this plugin reports. They never shrink here: prepare() and the
        // host, once a whole set of plugins is restored, call reserveAlignment(true) for that.
        reserveAlignment();
    }
    int latencySamples() const noexcept {
        int maximum = 0;
        for (int t = 0; t < maxSessionTracks; ++t) maximum = std::max(maximum,rackLatency(racks[t]));
        return rackChannelLatency() + maximum + busLatency() + returnLatency() + rackLatency(racks[masterIndex]);
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
        for(int c=0;c<maxRackChannels;++c){const double tail=slotTail(rackChannels[static_cast<std::size_t>(c)],channelBase+c,-1);longest=std::max(longest,std::isfinite(tail)?std::clamp(tail,0.0,30.0):30.0);}
        // A bus or a return rings on after the track that feeds it, and the master after that.
        for(int b=0;b<maxBuses;++b)busTail=std::max(busTail,rackTail(racks[static_cast<std::size_t>(busIndex(b))],busIndex(b)));
        for(int r=0;r<maxReturns;++r)returnTail=std::max(returnTail,rackTail(racks[static_cast<std::size_t>(returnIndex(r))],returnIndex(r)));
        return std::min(30.0,longest+std::max(busTail,returnTail)+rackTail(racks[masterIndex],masterIndex));
    }
private:
    // A stereo delay line over storage it does not own (a stage's AlignmentStorage, or a rack channel's own).
    struct Delay {
        float* left{};
        float* right{};
        int length{};     // samples held per channel: the longest delay it can give is length - 1
        int position{};
        int written{};    // samples written since bind()/forget(), up to length; older ones read as silence
        bool bound() const noexcept { return left != nullptr; }
        void bind(float* l, float* r, int samples) noexcept { left=l; right=r; length=samples; position=0; written=0; }
        // Constant time: what the line holds reads as silence from now on.
        void clear() noexcept { position=0; written=0; }
        void forget() noexcept { written=0; }
        // Delays `buffer` by `count` samples. Answers false when the line is too short for `count`: it then
        // delays by as much as it holds, and the caller counts a shortfall.
        bool process(juce::AudioBuffer<float>& buffer,int samples,int count) noexcept {
            if (left == nullptr) return count <= 0;
            const bool fits = count < length;
            count = std::clamp(count, 0, length - 1);
            if (samples <= 0) return fits;
            if (count == 0) {
                // T1-04 D27: no delay hands the block back as it is. The line still keeps the newest samples,
                // exactly as the loop below would leave it, so a delay asked for later reads the right past.
                const float* inLeft = buffer.getReadPointer(0);
                const float* inRight = buffer.getReadPointer(1);
                int from = 0;
                if (samples > length) { from = samples - length; position = static_cast<int>((static_cast<long long>(position) + from) % length); }
                while (from < samples) {
                    const int run = std::min(samples - from, length - position);
                    std::memcpy(left + position, inLeft + from, sizeof(float) * static_cast<std::size_t>(run));
                    std::memcpy(right + position, inRight + from, sizeof(float) * static_cast<std::size_t>(run));
                    from += run; position += run;
                    if (position == length) position = 0;
                }
                written = static_cast<int>(std::min<long long>(length, static_cast<long long>(written) + samples));
                return fits;
            }
            // T1-04 D27: the ring wraps with a compare instead of two divisions per sample (same positions).
            float* outLeft = buffer.getWritePointer(0);
            float* outRight = buffer.getWritePointer(1);
            for (int i=0;i<samples;++i) {
                left[position]=outLeft[i]; right[position]=outRight[i];
                if (written<length) ++written;
                int read=position-count;
                if (read<0) read+=length;
                const bool fresh=count<written;
                outLeft[i]=fresh?left[read]:0.0f; outRight[i]=fresh?right[read]:0.0f;
                if (++position==length) position=0;
            }
            return fits;
        }
    };
    // One stage's lines, end to end: left then right for each line, each capacity + 1 samples long.
    struct AlignmentStorage { int capacity{}; std::vector<float> samples; };
    struct AlignmentStageState {
        std::unique_ptr<AlignmentStorage> active;              // audio thread (adoptAlignment)
        std::atomic<AlignmentStorage*> pending{nullptr};       // published by reserveAlignment, taken by the callback
        std::atomic<AlignmentStorage*> retired{nullptr};       // handed back by the callback, freed by reclaimAlignment
        int published{-1};                                     // message thread: the capacity last published
    };
    static constexpr int linesIn(AlignmentStage stage) noexcept {
        switch (stage) {
        case trackAlignment: case sourceAlignment: return maxSessionTracks + 1;    // the tracks and `live`
        case busAlignment: return maxBuses + maxReturns + 1;                       // bus chains, sends, direct mix
        case returnAlignment: return maxReturns + 1;                               // return chains, dry mix
        case alignmentStageCount: break;
        }
        return 0;
    }
    template <class Fn> void forEachLine(AlignmentStage stage, Fn&& fn) noexcept {
        switch (stage) {
        case trackAlignment: for (auto& d : delays) fn(d); break;
        case sourceAlignment: for (auto& d : sourceDelays) fn(d); break;
        case busAlignment: for (auto& d : busDelays) fn(d); for (auto& d : sendDelays) fn(d); fn(directDelay); break;
        case returnAlignment: for (auto& d : returnDelays) fn(d); fn(dryDelay); break;
        case alignmentStageCount: break;
        }
    }
    // Audio thread, at the start of each block: takes up any storage reserveAlignment published, once the
    // storage it replaced last time has been freed. The lines start empty (silence), so a stage that grows
    // loses at most the audio it was delaying, at a moment its latency changed anyway. No allocation, no lock.
    void adoptAlignment() noexcept {
        for (int s = 0; s < alignmentStageCount; ++s) {
            auto& stage = stages[static_cast<std::size_t>(s)];
            if (stage.retired.load(std::memory_order_acquire) != nullptr) continue;
            auto* next = stage.pending.exchange(nullptr, std::memory_order_acq_rel);
            if (next == nullptr) continue;
            stage.retired.store(stage.active.release(), std::memory_order_release);
            stage.active.reset(next);
            const int length = next->capacity + 1;
            float* at = next->samples.data();
            forEachLine(static_cast<AlignmentStage>(s), [&](Delay& d) { d.bind(at, at + length, length); at += 2 * length; });
        }
        for (int c = 0; c < maxRackChannels; ++c) {
            auto& stage = channelStages[static_cast<std::size_t>(c)];
            if (stage.retired.load(std::memory_order_acquire) != nullptr) continue;
            auto* next = stage.pending.exchange(nullptr, std::memory_order_acq_rel);
            if (next == nullptr) continue;
            stage.retired.store(stage.active.release(), std::memory_order_release);
            stage.active.reset(next);
            const int length = next->capacity + 1;
            float* at = next->samples.data();
            channelDelays[static_cast<std::size_t>(c)].bind(at, at + length, length);
        }
    }
    // Delays one budgeted line, counting a block in which some line was too short (once per block).
    void align(Delay& delay, juce::AudioBuffer<float>& buffer, int samples, int count) noexcept {
        if (!delay.process(buffer, samples, count) && !shortBlock) { shortBlock = true; shortfalls.fetch_add(1, std::memory_order_acq_rel); }
    }
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
    std::array<Delay,maxSessionTracks+1> sourceDelays;   // the tracks and `live`, waiting for the rack's instruments
    std::size_t inUse{maxSessionTracks};                 // audio thread: useTracks
    std::array<Delay,maxRackChannels> channelDelays;     // each rack instrument, waiting for the slowest (while loaded)
    int channelStage{};                                  // this block's rackChannelLatency (audio thread)
    std::array<bool,maxReturns> returnLive = [] { std::array<bool,maxReturns> live{}; live.fill(true); return live; }();   // audio thread: setReturnActive
    Delay dryDelay, directDelay;
    std::array<AlignmentStageState, alignmentStageCount> stages;   // the storage behind the lines above
    std::array<AlignmentStageState, maxRackChannels> channelStages;  // the storage behind channelDelays (D15)
    std::atomic<std::uint32_t> shortfalls{0};
    std::atomic<std::uint64_t> droppedTrack{0};
    bool shortBlock{};   // audio thread: a shortfall was already counted this block
    juce::MidiBuffer emptyMidi;
    std::array<juce::MidiBuffer, maxRackChannels> channelMidi;
    juce::AudioBuffer<float> channelScratch;
    bool channelPanic{true};   // the rack's instruments are told all notes off on their first block after a prepare
    struct LiveEvent { int channel{-1}; juce::MidiMessage message; };
    std::array<LiveEvent, rackLiveEvents> liveEvents{};   // this block's live MIDI for the rack's instruments (liveRack)
    int liveCount{};
    double rate{44100};
    juce::int64 renderedSamples{};
    int currentSamples{};
};
}
