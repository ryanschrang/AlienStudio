// VST3 instruments on Channel Rack channels (T1-09 part 2, INST-10): a channel can be played by a plugin
// instrument. Its steps and notes become MIDI for the plugin with the rack's own timing (gates, ratchets, pitch,
// mute, previews, stop); the plugin's output joins the channel's bus with its fader and pan; the plugin is
// loaded, saved and contained like any other; and the project keeps it (format 31).
#include "ProjectController.h"
#include "PluginHost.h"
#include "RackProject.h"
#include "ToneEngine.h"
#include <cstdlib>
#include <iostream>

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
    int held{};
    bool crash{};
};

static juce::String description(const char* id) {
    juce::PluginDescription d; d.name = id; d.pluginFormatName = "VST3"; d.fileOrIdentifier = id; d.isInstrument = true;
    return d.createXml()->toString();
}
static PluginFactory factory() {
    return [](const juce::PluginDescription& d, double, int, juce::String&) -> std::unique_ptr<juce::AudioProcessor> {
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

static void controller() {
    ToneEngine engine;
    ProjectController c(engine);
    c.pushDocumentToEngine();
    int reloads = 0;
    c.onRackPluginsChanged = [&] { ++reloads; };
    require(c.setRackPlugin(0, description("synth"), "Synth"), "a channel gets a plugin instrument");
    const auto& rack = c.document().channelRack;
    require(reloads == 1 && rack.channels[0].plugin && rack.info[0].name == "Synth" && rack.info[0].pluginInstance() != 0, "named after it, and the owner reloads");
    require(c.rackPlugins().size() == static_cast<std::size_t>(rack.channelCount) && c.rackPlugins()[0].instanceId == rack.info[0].pluginInstance()
            && c.rackPlugins()[1].descriptionXml.isEmpty(), "the host is given the instruments by channel");
    require(c.editRack("Duplicate channel", [](Project& p) { return p.duplicateChannel(0); }), "duplicate it");
    require(reloads == 2 && c.document().channelRack.info[1].hasPlugin()
            && c.document().channelRack.info[1].pluginInstance() != c.document().channelRack.info[0].pluginInstance(),
            "the copy is a second instance with an identity of its own");
    c.setRackPluginStates({{c.document().channelRack.info[0].pluginInstance(), "dG9uZQ=="}});
    require(c.document().channelRack.info[0].pluginState() == "dG9uZQ==" && c.document().channelRack.info[1].pluginState().empty(), "states are kept by instance");
    // The whole project saves with format 31 and opens again with the instrument.
    const auto text = serializeSessionProject(toProjectData(c.document()));
    require(juce::JSON::parse(text).getProperty("version", 0) == juce::var(31), "projects are format 31");
    auto parsed = std::make_unique<SessionProjectData>(); juce::String error;
    require(parseSessionProject(text, *parsed, error) && parsed->channelRack.info[0].pluginState() == "dG9uZQ==" && parsed->channelRack.channels[1].plugin,
            "and open with their rack instruments");
    require(c.undo() && c.undo() && !c.document().channelRack.channels[0].plugin, "undo takes the instrument away again");
    std::puts("PASS: choosing, duplicating, saving and undoing rack instruments");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    model();
    player();
    engine();
    crashing();
    controller();
    return 0;
}
