// VST3 instruments on Channel Rack channels (T1-09 part 2, INST-10): a channel can be played by a plugin
// instrument. Its steps and notes become MIDI for the plugin with the rack's own timing (gates, ratchets, pitch,
// mute, previews, stop); the plugin's output joins the channel's bus with its fader and pan; the plugin is
// loaded, saved and contained like any other; and the project keeps it (format 31).
#include "ProjectController.h"
#include "CommandEngine.h"
#include "PluginHost.h"
#include "RackProject.h"
#include "ToneEngine.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

using namespace alien;
using namespace alien::rack;

static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static volatile std::uintptr_t zeroAddress = 0;

// An instrument that plays a constant 0.5 while any note is held, and remembers the notes it was given.
struct TestInstrument final : juce::AudioProcessor {
    struct Event { std::int64_t at{}; int note{}; float velocity{}; };
    explicit TestInstrument(bool crashes) : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo())), crash(crashes) {
        events.reserve(4096);
    }
    const juce::String getName() const override { return "Test instrument"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        if (crash) *reinterpret_cast<volatile int*>(zeroAddress) = 1;
        int next = 0;
        for (const auto meta : midi) {
            const auto m = meta.getMessage();
            for (; next < meta.samplePosition; ++next) fill(buffer, next);
            if (m.isNoteOn()) { ++held; if (events.size() < events.capacity()) events.push_back({rendered + meta.samplePosition, m.getNoteNumber(), m.getFloatVelocity()}); }
            else if (m.isNoteOff()) { held = std::max(0, held - 1); if (events.size() < events.capacity()) events.push_back({rendered + meta.samplePosition, m.getNoteNumber(), 0.0f}); }
            else if (m.isAllNotesOff() || m.isAllSoundOff()) held = 0;
            else if (m.isPitchWheel()) pitch = m.getPitchWheelValue();
            else if (m.isController()) controllers[static_cast<std::size_t>(m.getControllerNumber())] = m.getControllerValue();
            if (m.getChannel() > 1) ++otherChannels;
        }
        for (; next < buffer.getNumSamples(); ++next) fill(buffer, next);
        rendered += buffer.getNumSamples();
    }
    void fill(juce::AudioBuffer<float>& buffer, int i) const {
        for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.setSample(c, i, held > 0 ? 0.5f : 0.0f);
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& d) override { d.append("tone", 4); }
    void setStateInformation(const void*, int) override {}
    std::vector<Event> events;
    std::int64_t rendered{};
    int held{}, pitch{8192}, otherChannels{};
    std::array<int, 128> controllers{};
    bool crash{};
};

// An instrument with a lookahead: it reports `latency` samples and plays a single full-scale sample that many
// samples after each note starts, so where the impulse lands in the mix shows whether the host made up for it.
struct LatentInstrument final : juce::AudioProcessor {
    explicit LatentInstrument(int latency) : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo())) {
        setLatencySamples(latency);
    }
    const juce::String getName() const override { return "Latent instrument"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        for (const auto meta : midi)
            if (meta.getMessage().isNoteOn() && pendingCount < static_cast<int>(pending.size()))
                pending[static_cast<std::size_t>(pendingCount++)] = rendered + meta.samplePosition + getLatencySamples();
        buffer.clear();
        for (int p = 0; p < pendingCount; ++p) {
            const auto at = pending[static_cast<std::size_t>(p)] - rendered;
            if (at < 0 || at >= buffer.getNumSamples()) continue;
            for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.setSample(c, static_cast<int>(at), 1.0f);
        }
        rendered += buffer.getNumSamples();
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    std::array<std::int64_t, 64> pending{};
    int pendingCount{};
    std::int64_t rendered{};
};

static juce::String description(const char* id) {
    juce::PluginDescription d; d.name = id; d.pluginFormatName = "VST3"; d.fileOrIdentifier = id; d.isInstrument = true;
    return d.createXml()->toString();
}
static PluginFactory factory() {
    return [](const juce::PluginDescription& d, double, int, juce::String&) -> std::unique_ptr<juce::AudioProcessor> {
        if (d.fileOrIdentifier.startsWith("latent")) return std::make_unique<LatentInstrument>(d.fileOrIdentifier.substring(6).getIntValue());
        return std::make_unique<TestInstrument>(d.fileOrIdentifier == "crashing");
    };
}

static void model() {
    auto p = Project::createDefault();
    require(!p.setPlugin(0, "", 5) && !p.setPlugin(0, "<x/>", 0) && !p.setPlugin(9, "<x/>", 5), "a plugin needs a description and an identity");
    require(p.setPlugin(0, description("synth").toStdString(), 5), "a channel takes a plugin instrument");
    require(p.channels[0].plugin && p.channels[0].isInstrument() && !p.channels[0].isSynth() && p.info[0].sample.empty() && p.valid(),
            "it replaces the sample");
    require(p.setPluginState(5, "c3RhdGU=") && p.info[0].pluginState() == "c3RhdGU=" && !p.setPluginState(5, "c3RhdGU=") && !p.setPluginState(6, "x"),
            "its state is kept by identity");
    require(p.pluginInstances()[0] == 5 && p.pluginInstances()[1] == 0, "the identities by channel");
    // Round trip through the project file's rack section.
    const auto saved = rackToVar(p, true);
    Project back; juce::String error;
    require(rackFromVar(saved, back, error, true) && back.info[0].pluginDescription() == p.info[0].pluginDescription()
            && back.info[0].pluginState() == "c3RhdGU=" && back.info[0].pluginInstance() == 5 && back.channels[0].plugin, "a plugin channel saves and loads");
    auto damaged = juce::JSON::parse(juce::JSON::toString(saved));
    damaged.getProperty("channels", {})[0].getDynamicObject()->setProperty("sample", "builtin:kick");
    require(!rackFromVar(damaged, back, error, true), "a channel cannot name both a plugin and a sample");
    require(p.setSample(0, "builtin:kick") && !p.channels[0].plugin && p.info[0].pluginDescription().empty() && p.info[0].pluginInstance() == 0 && p.valid(),
            "choosing a sample removes the plugin");
    p.setPlugin(0, "<x/>", 7);
    require(p.setInstrumentPreset(0, 1) && !p.channels[0].plugin && p.valid(), "and so does the built-in synth");
    std::puts("PASS: plugin channels in the rack model and file");
}

// The rack player turns a plugin channel's hits into notes and makes no sound of its own.
static void player() {
    auto p = Project::createDefault();
    p.setPlugin(0, "<x/>", 5);
    p.setStepOn(0, 0, 0, true);
    Step pitched; pitched.on = true; pitched.pitch = 3; pitched.ratchet = 2; pitched.velocity = 64;
    p.setStep(0, 0, 4, pitched);
    const auto snap = p.snapshot();
    RackPlayer player; player.prepare(48000);
    RackPlayer::SampleTable clips{};
    std::array<std::array<float, 512>, RackPlayer::busCount> leftStore{}, rightStore{};
    std::array<float*, RackPlayer::busCount> left{}, right{};
    for (std::size_t b = 0; b < left.size(); ++b) { left[b] = leftStore[b].data(); right[b] = rightStore[b].data(); }
    std::vector<RackPlayer::MidiNote> heard;
    std::int64_t at = 0;
    const auto run = [&](int blocks, bool running) {
        for (int b = 0; b < blocks; ++b) {
            player.process(*snap, clips, running, 120.0, 4, 512, left.data(), right.data());
            for (int n = 0; n < player.midiNoteCount(); ++n) { auto e = player.midiNotes()[n]; e.sample += static_cast<int>(at); heard.push_back(e); }
            player.clearMidiNotes();
            at += 512;
        }
    };
    run(60, true);   // 30720 samples: steps 0 to 5 at 6000 samples a step
    float sound = 0; for (auto& bus : leftStore) for (float v : bus) sound = std::max(sound, std::abs(v));
    require(sound == 0.0f, "a plugin channel makes no sound in the rack's own voices");
    require(heard.size() >= 6 && heard[0].sample == 0 && heard[0].note == 60 && std::abs(heard[0].velocity - 100.0f / 127.0f) < 1e-4f,
            "step 1 starts note C4 at its first sample, with its velocity");
    require(heard[1].velocity == 0.0f && heard[1].note == 60 && std::abs(heard[1].sample - 6000) <= 1, "and its gate ends it a step later");
    require(heard[2].note == 63 && heard[2].sample == 24000 && std::abs(heard[2].velocity - 64.0f / 127.0f) < 1e-4f
            && heard[3].velocity == 0.0f && heard[4].note == 63 && heard[4].sample == 27000 && heard[5].velocity == 0.0f,
            "a ratcheted, pitched step plays twice, three semitones up");
    // A held preview sounds until it is let go; stopping ends whatever is held.
    run(1, false);   // the transport stops first: that ends what the pattern was playing
    heard.clear();
    player.audition(*snap, 0, 0.9f, nullptr, 7, true);
    run(1, false);
    require(heard.size() == 1 && heard[0].note == 67 && heard[0].velocity > 0.89f, "a held preview starts its note");
    player.audition(*snap, 0, 0.0f, nullptr, 7, true);
    run(1, false);
    require(heard.size() == 2 && heard[1].note == 67 && heard[1].velocity == 0.0f, "and letting go ends it");
    player.audition(*snap, 0, 0.9f, nullptr, 0, true);
    player.stop();
    run(1, false);
    require(heard.size() == 4 && heard[3].velocity == 0.0f, "stopping ends a held note");
    std::puts("PASS: the rack plays plugin channels as notes, with its own timing");
}

struct Rig {
    ToneEngine engine;
    PluginHost host{*engine.plugins, {}, factory()};
    Project project = Project::createDefault();
    std::array<float, 512> left{}, right{};
    Rig() {
        engine.prepare(48000);
        engine.bpm.store(120);
        for (auto& step : engine.steps) step.store(false);
    }
    TestInstrument& load(const char* id) {
        project.setPlugin(0, description(id).toStdString(), 9);
        std::vector<PluginState> channels(1);
        channels[0].descriptionXml = description(id); channels[0].instanceId = 9;
        require(host.restoreRackChannels(channels, 48000).isEmpty(), "the rack instrument loads");
        engine.rackMailbox.publish(project.snapshot());
        return *dynamic_cast<TestInstrument*>(engine.plugins->rackChannels[0].processor.get());
    }
    // Renders and returns the loudest left output.
    float render(int blocks) {
        float peak = 0;
        for (int b = 0; b < blocks; ++b) {
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
            for (float v : left) peak = std::max(peak, std::abs(v));
        }
        return peak;
    }
};

static void engine() {
    Rig rig;
    auto& instrument = rig.load("synth");
    rig.project.setStepOn(0, 0, 0, true);
    rig.engine.rackMailbox.publish(rig.project.snapshot());
    rig.engine.patternMode.store(3);
    rig.render(4);
    require(instrument.events.empty(), "nothing plays while stopped");
    rig.engine.playing.store(true);
    const auto start = instrument.rendered;
    const float during = rig.render(8);
    require(instrument.events.size() >= 1 && instrument.events[0].note == 60 && instrument.events[0].at == start, "the instrument gets step 1's note on time");
    require(during > 0.1f, "and is heard on the master");
    rig.render(8);   // past the gate
    require(instrument.events.size() >= 2 && instrument.events[1].velocity == 0.0f && std::abs(instrument.events[1].at - (start + 6000)) <= 1,
            "the gate ends the note a step later");
    // Muting the channel silences it at once.
    rig.render(24);
    auto muted = rig.project.channels[0]; muted.mute = true;
    rig.project.setChannel(0, muted);
    rig.engine.rackMailbox.publish(rig.project.snapshot());
    rig.render(2);
    const auto before = allocations;
    const float silent = rig.render(48);
    require(silent < 1e-3f, "a muted plugin channel is silent");
    require(allocations == before, "playing the rack's plugin instruments does not allocate");
    rig.engine.playing.store(false);
    rig.render(4);
    require(instrument.held == 0, "stopping leaves no note held");
    std::puts("PASS: a plugin channel plays through the engine on time, muted and stopped cleanly");
}

static void crashing() {
    Rig rig;
    rig.load("crashing");
    juce::String problem;
    rig.host.onProblem = [&](const juce::String& text) { problem = text; };
    rig.project.setStepOn(0, 0, 0, true);
    rig.engine.rackMailbox.publish(rig.project.snapshot());
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(true);
    rig.render(4);
    rig.host.poll();
    require(rig.engine.plugins->rackChannels[0].faulted.get() && problem.contains("Channel Rack channel 1"),
            "a rack instrument that crashes is switched off and reported by channel");
    require(rig.host.health(PluginRuntime::channelBase, -1).contains("FAILED"), "its health says so");
    rig.render(4);
    std::puts("PASS: a crashing rack instrument is contained");
}

// Rack-plugin latency compensation (T1-09): a rack instrument that reports latency is lined up, to the sample,
// with the rack's sample channels and with the other rack instruments; the whole mix reports the delay (so an
// export trims it); and it costs no allocation.
static void latency() {
    struct Mix {
        ToneEngine engine;
        PluginHost host{*engine.plugins, {}, factory()};
        Project project = Project::createDefault();
        std::vector<float> left, right;
        std::shared_ptr<SessionAudioClip> dc = std::make_shared<SessionAudioClip>();
        Mix() {
            engine.prepare(48000);
            engine.bpm.store(120);
            for (auto& step : engine.steps) step.store(false);
            dc->sampleRate = 48000; dc->channels = 1; dc->frames = 48000; dc->left.assign(48000, 1.0f); dc->buildWaveform();
            left.reserve(1 << 16); right.reserve(1 << 16);
        }
        // Channel 0 hard left and channel 1 hard right, both on step 1; every other channel muted.
        void setUp(const std::vector<const char*>& plugins) {
            std::vector<PluginState> states(plugins.size());
            for (std::size_t c = 0; c < plugins.size(); ++c) {
                if (plugins[c] == nullptr) { project.setSample(static_cast<int>(c), "user:dc.wav"); continue; }
                project.setPlugin(static_cast<int>(c), description(plugins[c]).toStdString(), 20 + c);
                states[c].descriptionXml = description(plugins[c]); states[c].instanceId = 20 + c;
            }
            for (int c = 0; c < project.channelCount; ++c) {
                auto channel = project.channels[static_cast<std::size_t>(c)];
                channel.pan = c == 0 ? -1.0f : 1.0f; channel.mute = c > 1;
                project.setChannel(c, channel);
                project.setStepOn(0, c, 0, c <= 1);
            }
            require(host.restoreRackChannels(states, 48000).isEmpty(), "the rack instruments load");
            engine.rackMailbox.publish(project.snapshot());
            for (std::size_t c = 0; c < plugins.size(); ++c) if (plugins[c] == nullptr) engine.rackSamples[c].store(dc.get());
            engine.patternMode.store(3);
        }
        void render(int blocks) {
            std::array<float, 512> l{}, r{};
            for (int b = 0; b < blocks; ++b) {
                float* outputs[]{l.data(), r.data()};
                engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
                left.insert(left.end(), l.begin(), l.end()); right.insert(right.end(), r.begin(), r.end());
            }
        }
        static int onset(const std::vector<float>& side) {
            for (std::size_t i = 0; i < side.size(); ++i) if (std::abs(side[i]) > 1e-3f) return static_cast<int>(i);
            return -1;
        }
        // Plays step 1 from the start and returns where each side is first heard, counted from the start.
        std::pair<int, int> play() {
            render(4);   // stopped: the master gain settles
            left.clear(); right.clear();
            engine.playing.store(true);
            const auto before = allocations;
            render(8);
            require(allocations == before, "a latent rack instrument is lined up without allocating");
            return {onset(left), onset(right)};
        }
    };
    {
        Mix mix;
        mix.setUp({"latent300", nullptr});
        require(mix.engine.plugins->rackChannelLatency() == 300 && mix.engine.plugins->latencySamples() == 300,
                "the mix reports the rack instrument's latency, so an export trims it");
        const auto [instrument, sample] = mix.play();
        require(sample == 300, "a sample channel waits for the latent rack instrument");
        require(instrument == sample, "and the instrument's note lands on the same sample");
    }
    {
        Mix mix;
        mix.setUp({"latent300", "latent0"});
        const auto [slow, fast] = mix.play();
        require(slow == 300 && fast == slow, "two rack instruments with different latencies line up with each other");
    }
    {
        Mix mix;
        mix.setUp({"latent0", nullptr});
        const auto [instrument, sample] = mix.play();
        require(mix.engine.plugins->latencySamples() == 0 && instrument == 0 && sample == 0, "an instrument without latency delays nothing");
    }
    std::puts("PASS: a latent rack instrument stays in time with the rest of the mix, to the sample");
}

// Live MIDI into a rack instrument (T1-09): the hardware input (and the typing piano) can play a Channel Rack
// channel's plugin instrument, with velocity, sustain, controllers and pitch bend, and never leaves a note held
// when the destination changes, the transport stops, the device resets or the instrument goes.
static void liveInput() {
    Rig rig;
    rig.project.setPlugin(0, description("first").toStdString(), 31);
    rig.project.setPlugin(1, description("second").toStdString(), 32);
    std::vector<PluginState> channels(2);
    channels[0].descriptionXml = description("first"); channels[0].instanceId = 31;
    channels[1].descriptionXml = description("second"); channels[1].instanceId = 32;
    require(rig.host.restoreRackChannels(channels, 48000).isEmpty(), "two rack instruments load");
    rig.engine.rackMailbox.publish(rig.project.snapshot());
    rig.engine.patternMode.store(3);
    auto& first = *dynamic_cast<TestInstrument*>(rig.engine.plugins->rackChannels[0].processor.get());
    auto& second = *dynamic_cast<TestInstrument*>(rig.engine.plugins->rackChannels[1].processor.get());
    require(rig.project.channelWithPlugin(32) == 1 && rig.project.channelWithPlugin(99) == -1 && rig.project.channelWithPlugin(0) == -1,
            "a rack instrument is found by its identity");

    rig.engine.liveRackChannel.store(0);
    auto token = rig.engine.midi.beginSession();
    rig.render(2);
    const auto before = allocations;
    rig.engine.midi.push(token, 0x90, 60, 100);
    rig.render(1);
    require(first.held == 1 && !first.events.empty() && first.events.back().note == 60 && std::abs(first.events.back().velocity - 100.0f / 127.0f) < 1e-4f,
            "a key pressed on the MIDI keyboard plays the chosen rack instrument, with its velocity");
    require(second.events.empty(), "and only that one");
    rig.engine.midi.push(token, 0x80, 60, 0);
    rig.render(1);
    require(first.held == 0 && first.events.back().velocity == 0.0f, "letting go ends the note");

    // Sustain: the pedal holds a released note until it comes up (and reaches the plugin as CC 64).
    rig.engine.midi.push(token, 0xB0, 64, 127);
    rig.engine.midi.push(token, 0x90, 62, 90);
    rig.engine.midi.push(token, 0x80, 62, 0);
    rig.render(1);
    require(first.held == 1 && first.controllers[64] == 127, "the sustain pedal holds a released note");
    rig.engine.midi.push(token, 0xB0, 64, 0);
    rig.render(1);
    require(first.held == 0 && first.controllers[64] == 0, "and lifting it ends the note");
    rig.engine.midi.push(token, 0xE0, 0, 80);
    rig.engine.midi.push(token, 0xB0, 1, 77);
    rig.engine.midi.push(token, 0x91, 67, 64);   // MIDI channel 2
    rig.render(1);
    require(first.pitch == 80 << 7 && first.controllers[1] == 77, "pitch bend and the mod wheel reach it");
    require(first.held == 1 && first.otherChannels > 0, "a note on another MIDI channel keeps its channel");
    require(allocations == before, "playing a rack instrument live does not allocate");

    // Changing the destination ends what the old one was playing, and its controllers go back to rest.
    rig.engine.liveRackChannel.store(1);
    rig.render(1);
    require(first.held == 0 && first.pitch == 8192 && first.controllers[1] == 0, "choosing another destination releases the old one");
    rig.engine.midi.push(token, 0x90, 64, 100);
    rig.render(1);
    require(second.held == 1 && first.held == 0, "and the new one plays");
    // The typing piano follows the same destination.
    rig.engine.notes.push(65, 0.7f);
    rig.render(1);
    require(second.held == 2 && second.events.back().note == 65, "the typing piano plays it too");
    rig.engine.notes.push(65, 0.0f);
    rig.render(1);
    require(second.held == 1, "and lets go of its note");

    // Stopping the transport ends every live note, the typing piano's too.
    rig.engine.playing.store(true);
    rig.engine.notes.push(66, 0.7f);
    rig.render(2);
    require(second.held == 2, "a keyboard note and a typed note are held");
    rig.engine.playing.store(false);
    rig.render(2);
    require(second.held == 0, "stopping leaves no live note held");
    // So does a device reset (the input reopens), and the old device's late messages are ignored.
    rig.engine.midi.push(token, 0x90, 69, 100);
    rig.render(1);
    require(second.held == 1, "a note before the device resets");
    const auto old = token;
    token = rig.engine.midi.beginSession();
    rig.render(1);
    require(second.held == 0 && !rig.engine.midi.push(old, 0x90, 70, 100), "a device reset leaves no note held");
    // A track destination takes precedence over the rack.
    rig.engine.liveDrumTrack.store(0);
    rig.render(1);
    const auto heard = second.events.size();
    rig.engine.midi.push(token, 0x90, 71, 100);
    rig.render(1);
    require(second.events.size() == heard, "a track chosen for live MIDI wins over the rack channel");
    rig.engine.midi.push(token, 0x80, 71, 0);
    rig.engine.liveDrumTrack.store(-1);
    rig.render(1);

    // The instrument is removed while a key is held: nothing reaches anyone else, and nothing breaks.
    rig.engine.midi.push(token, 0x90, 72, 100);
    rig.render(1);
    require(second.held == 1, "a held note before the instrument goes");
    std::vector<PluginState> onlyFirst(1);
    onlyFirst[0] = channels[0];
    require(rig.host.restoreRackChannels(onlyFirst, 48000).isEmpty(), "the second instrument is removed");
    auto& reloaded = *dynamic_cast<TestInstrument*>(rig.engine.plugins->rackChannels[0].processor.get());
    rig.engine.midi.push(token, 0x80, 72, 0);
    rig.engine.midi.push(token, 0x90, 73, 100);
    rig.render(2);
    require(reloaded.events.empty() && rig.engine.plugins->rackChannels[1].processor == nullptr,
            "live notes for a channel without an instrument go nowhere");
    std::puts("PASS: live MIDI plays a rack instrument with sustain, controllers and pitch bend, and leaves nothing held");
}

static void controller() {
    ToneEngine engine;
    ProjectController c(engine);
    c.pushDocumentToEngine();
    int reloads = 0;
    c.onRackPluginsChanged = [&] { ++reloads; };
    command::CommandEngine commands(c);
    require(commands.execute(command::SetRackChannelPlugin{0, description("synth"), "Synth"}).changed, "a channel gets a plugin instrument through a command");
    const auto& rack = c.document().channelRack;
    require(reloads == 1 && rack.channels[0].plugin && rack.info[0].name == "Synth" && rack.info[0].pluginInstance() != 0, "named after it, and the owner reloads");
    require(c.rackPlugins().size() == static_cast<std::size_t>(rack.channelCount) && c.rackPlugins()[0].instanceId == rack.info[0].pluginInstance()
            && c.rackPlugins()[1].descriptionXml.isEmpty(), "the host is given the instruments by channel");
    require(commands.execute(command::DuplicateRackChannel{0}).changed, "duplicate it through a command");
    require(reloads == 2 && c.document().channelRack.info[1].hasPlugin()
            && c.document().channelRack.info[1].pluginInstance() != c.document().channelRack.info[0].pluginInstance(),
            "the copy is a second instance with an identity of its own");
    c.setRackPluginStates({{c.document().channelRack.info[0].pluginInstance(), "dG9uZQ=="}});
    require(c.document().channelRack.info[0].pluginState() == "dG9uZQ==" && c.document().channelRack.info[1].pluginState().empty(), "states are kept by instance");
    // The whole project saves with format 31 or later and opens again with the instrument.
    const auto text = serializeSessionProject(toProjectData(c.document()));
    require(static_cast<int>(juce::JSON::parse(text).getProperty("version", 0)) >= 31, "projects are format 31 or later");
    auto parsed = std::make_unique<SessionProjectData>(); juce::String error;
    require(parseSessionProject(text, *parsed, error) && parsed->channelRack.info[0].pluginState() == "dG9uZQ==" && parsed->channelRack.channels[1].plugin,
            "and open with their rack instruments");
    require(c.undo() && c.undo() && !c.document().channelRack.channels[0].plugin, "undo takes the instrument away again");
    std::puts("PASS: choosing, duplicating, saving and undoing rack instruments");
}

// T1-04 D9: a block with more plugin notes than its list holds reaches the instrument with every note-off: the
// rack player keeps room for them, and the channel's MIDI storage takes the whole list without allocating.
static void denseNotes() {
    Rig rig;
    ProjectController reporter(rig.engine);   // only reads the engine's count of hits it could not play (D10)
    juce::String said, why;
    reporter.onProjectStatus = [&](const juce::String& text, const juce::String& tip) { said = text; why = tip; };
    require(!reporter.reportDroppedNotes(1000) && said.isEmpty(), "nothing to report before anything is dropped");
    auto& instrument = rig.load("synth");
    PianoRollPattern notes; notes.loopSteps = 16;
    for (int n = 0; n < 1000; ++n) notes.add(n % 128, 0.0, 1.0 / PianoRollPattern::ticksPerStep, 1.0f);
    require(rig.project.setChannelNotes(0, 0, notes), "1,000 notes on one tick");
    rig.engine.rackMailbox.publish(rig.project.snapshot());
    rig.engine.patternMode.store(3);
    rig.render(2);
    rig.engine.playing.store(true);
    const auto before = allocations;
    rig.render(6);
    require(allocations == before, "a full block of plugin notes does not allocate");
    std::array<int, 128> sounding{};
    int ons = 0, offs = 0;
    for (const auto& e : instrument.events) {
        auto& s = sounding[static_cast<std::size_t>(e.note)];
        if (e.velocity > 0.0f) { ++ons; ++s; } else { ++offs; s = std::max(0, s - 1); }
    }
    require(ons == RackPlayer::maxMidiNotes / 2 && ons == offs && rig.engine.droppedRackHits() == 1000u - static_cast<unsigned>(ons),
            "the instrument gets every note played, each with its note-off, and the rest are counted");
    require(std::all_of(sounding.begin(), sounding.end(), [](int s) { return s == 0; }) && instrument.held == 0, "no note is left held");
    rig.engine.playing.store(false);
    rig.render(2);
    // T1-04 D10: the status bar says so, once, and again only for new ones, at most once a second.
    require(reporter.reportDroppedNotes(5000) && said == "488 Channel Rack hits were not played" && why.contains("Thin out"),
            "the status bar says how many hits were not played, and why");
    said.clear();
    require(!reporter.reportDroppedNotes(5100) && said.isEmpty(), "and does not repeat it");
    rig.engine.playing.store(true);
    rig.render(6);   // the same step again: 488 more
    rig.engine.playing.store(false);
    rig.render(2);
    require(!reporter.reportDroppedNotes(5500) && said.isEmpty(), "new ones wait until a second has passed");
    require(reporter.reportDroppedNotes(6100) && said == "488 Channel Rack hits were not played", "then only the new ones are reported");
    require(rackDropText(1) == "1 Channel Rack hit was not played", "one hit reads as one");
    // T1-04 D11: a track instrument given more MIDI than a block holds is reported the same way.
    for (int i = 0; i <= PluginRuntime::trackEventsPerBlock + 4; ++i) rig.engine.plugins->note(0, 60, 1.0f, true);
    said.clear(); why.clear();
    require(reporter.reportDroppedNotes(7200) && said == "5 MIDI events for track instruments were not sent, and their notes were stopped"
                && why.contains("4,096") && !why.contains("Channel Rack"),
            "dropped track MIDI is reported, with its own reason");
    require(droppedNotesText(2, 1) == "2 Channel Rack hits were not played; 1 MIDI event for a track instrument was not sent, and their notes were stopped"
                && droppedNotesText(0, 0).isEmpty(),
            "both kinds read together, and nothing reads as nothing");
    std::puts("PASS: a dense block of rack plugin notes loses no note-off");
}

// A streamed source whose reads fail from `failFrom` on, or wait from `stallFrom` on until released.
struct StreamFile final : DiskAudioFile {
    struct Reader final : DiskAudioReader {
        explicit Reader(const StreamFile& owner) : file(owner) {}
        bool read(std::int64_t start, float* left, float*, int count) override {
            if (start >= file.failFrom) return false;
            const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (start >= file.stallFrom && !file.released.load() && std::chrono::steady_clock::now() < giveUp)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            for (int i = 0; i < count; ++i) left[i] = 0.25f;
            return true;
        }
        const StreamFile& file;
    };
    StreamFile(std::int64_t fail, std::int64_t stall) : DiskAudioFile(48000.0, 1, 96000), failFrom(fail), stallFrom(stall) {}
    std::unique_ptr<DiskAudioReader> open() const override { return std::make_unique<Reader>(*this); }
    std::int64_t failFrom, stallFrom;
    std::atomic<bool> released{};
};

static void streamReports() {
    auto owned = std::make_unique<ToneEngine>();
    auto& engine = *owned;
    engine.prepare(48000); engine.patternMode.store(2); engine.bpm.store(120); engine.setActiveTracks(4);
    ProjectController reporter(engine);
    juce::String said, why;
    reporter.onProjectStatus = [&](const juce::String& text, const juce::String& tip) { said = text; why = tip; };
    require(!reporter.reportStreamProblems(1000) && said.isEmpty(), "no stream problem, nothing to say");

    constexpr auto never = std::numeric_limits<std::int64_t>::max();
    auto unreadable = std::make_shared<StreamFile>(0, never);       // track 3: cannot be read at all
    auto slow = std::make_shared<StreamFile>(never, 12288);         // track 1: the disk stalls after a quarter second
    SessionAudioClip bad, late;
    for (auto* c : {&bad, &late}) { c->sampleRate = 48000; c->channels = 1; c->frames = 96000; }
    bad.streamSource = unreadable; late.streamSource = slow;
    require(engine.retainDiskAudioSource(unreadable) && engine.retainDiskAudioSource(slow), "the engine keeps both sources");
    auto song = std::make_shared<ArrangementPlayback>();
    song->timeline.setLengthBars(4);
    song->clips[0][0].audio = &late; song->clips[0][2].audio = &bad;   // clips[scene][track]
    for (int row : {0, 2}) {
        timeline::Region r; r.row = row; r.source = 0; r.start = 0; r.length = 2 * timeline::ticksPerBar;
        std::uint32_t id = 0;
        require(song->timeline.place(r, &id) && id != 0, "place a streamed region");
    }
    require(engine.arrangementMailbox.publish(std::shared_ptr<const ArrangementPlayback>(song)), "publish the song");
    engine.timelineSeek.store(0); engine.playing.store(true);
    std::vector<float> l(256), r(256);
    auto render = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) { float* out[]{l.data(), r.data()}; engine.audioDeviceIOCallbackWithContext(nullptr, 0, out, 2, 256, {}); }
    };
    render(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // track 1 prebuffers, track 3 fails
    render(150);                                                   // 0.8 s: track 1 plays past what the stalled disk gave
    require(engine.sessionStreamFailures(2) == 1 && engine.sessionStreamUnderruns(2) == 0, "track 3's stream failed once");
    require(engine.sessionStreamUnderruns(0) > 0 && engine.sessionStreamFailures(0) == 0, "track 1's stream ran dry");
    require(reporter.reportStreamProblems(2000)
                && said == "Streamed audio dropped out on track 1: the disk could not keep up; Streamed audio on track 3 could not be read, so it stopped"
                && why.contains("faster, local drive") && why.contains("Search for missing audio"),
            "the status bar names each track and why");
    said.clear();
    render(20);
    require(!reporter.reportStreamProblems(2500) && said.isEmpty(), "more of the same waits a second");
    require(reporter.reportStreamProblems(3100) && said == "Streamed audio dropped out on track 1: the disk could not keep up"
                && !why.contains("missing audio"),
            "then only what is new is reported");
    slow->released.store(true);
    engine.playing.store(false); render(2);
    require(trackListText({2, 5, 7}) == "tracks 2, 5 and 7" && trackListText({4, 9}) == "tracks 4 and 9", "track lists read naturally");
    require(streamProblemText({}, {}).isEmpty(), "nothing reads as nothing");
    std::puts("PASS: streamed audio that drops out or cannot be read is reported by track");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    streamReports();
    denseNotes();
    model();
    player();
    engine();
    crashing();
    latency();
    liveInput();
    controller();
    return 0;
}
