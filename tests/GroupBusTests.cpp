// Group buses (MIX-03): track output routing to a bus or the master, bus level/pan/mute/solo and
// FX, solo across tracks and buses, delay compensation through a latent bus, no allocation while
// mixing, the project format (round trip, older files, damage), undo, stems and freeze with
// buses, and the mixer and plugin panels.
#include "ToneEngine.h"
#include "WavExport.h"
#include "PluginHost.h"
#include "PluginPanel.h"
#include "ProjectController.h"
#include "TrackMixerPanel.h"
#include <cstdlib>
#include <iostream>
#include <memory>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);} void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

static void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
static bool approx(float a, float b) { return std::abs(a - b) < 1.0e-4f; }

// Plays a steady 0.2 x gain while a note is held (an instrument), or scales and optionally delays what it is given (an effect).
class TestProcessor final : public juce::AudioProcessor {
public:
    explicit TestProcessor(bool instrument = false, float gain = 1, int latency = 0)
        : juce::AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo(), !instrument).withOutput("Out", juce::AudioChannelSet::stereo(), true)),
          synth(instrument), factor(gain), delay(latency) { setLatencySamples(latency); }
    void prepareToPlay(double, int) override { position = 0; ring.fill(0); active = false; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override {
        ++processCalls;
        auto it = midi.begin(); const auto end = midi.end();
        for (int i = 0; i < b.getNumSamples(); ++i) {
            while (it != end && (*it).samplePosition <= i) { const auto m = (*it).getMessage(); if (m.isNoteOn()) active = true; else if (m.isNoteOff() || m.isAllNotesOff() || m.isAllSoundOff()) active = false; ++it; }
            for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, i, (synth ? (active ? 0.2f : 0.0f) : b.getSample(c, i)) * factor);
            if (delay) { for (int c = 0; c < 2; ++c) { const auto value = b.getSample(c, i); b.setSample(c, i, ring[static_cast<std::size_t>(c * 16 + position)]); ring[static_cast<std::size_t>(c * 16 + position)] = value; } position = (position + 1) % delay; }
        }
    }
    const juce::String getName() const override { return "Original bus test processor"; }
    bool acceptsMidi() const override { return synth; } bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; } void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; } juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {} void setStateInformation(const void*, int) override {}
    int processCalls{};
    bool synth{}, active{}; float factor; int delay{}, position{}; std::array<float, 32> ring{};
};

// An engine where track 0 holds a note on a test instrument (0.2) and track 1 on another (0.1).
struct Rig {
    std::unique_ptr<ToneEngine> engine = std::make_unique<ToneEngine>();
    std::array<float, 1024> left{}, right{};
    Rig() {
        engine->prepare(48000); engine->patternMode.store(1); engine->launchQuantizationSteps.store(4);
        instrument(0, 1.0f); instrument(1, 0.5f);
        engine->playing.store(true);
    }
    void instrument(int t, float gain) {
        auto& rack = engine->plugins->racks[static_cast<std::size_t>(t)]; rack.instrumentAssigned = true;
        rack.instrument.processor = std::make_unique<TestProcessor>(true, gain); engine->plugins->prepareSlot(rack.instrument);
        alien::SessionClipLaunch clip; clip.pattern.clear(); clip.pattern.add(60, 0.0, 64.0); clip.clipIndex = 0;
        engine->pianoRollLaunches[static_cast<std::size_t>(t)].publish(clip);
    }
    void effect(int rackIndex, float gain, int latency = 0) {
        auto& rack = engine->plugins->racks[static_cast<std::size_t>(rackIndex)];
        rack.effects[static_cast<std::size_t>(rack.effectCount)].processor = std::make_unique<TestProcessor>(false, gain, latency);
        engine->plugins->prepareSlot(rack.effects[static_cast<std::size_t>(rack.effectCount++)]);
    }
    void render(int blocks = 1) {
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 1024, {});
    }
    std::pair<float, float> settled() { render(12); return {left[1023], right[1023]}; }
    alien::TrackControls& track(int t) { return engine->tracks[static_cast<std::size_t>(t)]; }
    alien::BusControls& bus(int b) { return engine->buses[static_cast<std::size_t>(b)]; }
    void route(int t, int b) { bus(b).active.store(true); track(t).output.store(b); }
};

static void busLevels() {
    Rig rig;
    require(approx(rig.settled().first, 0.3f), "both tracks reach the master");
    rig.route(0, 0);
    require(approx(rig.settled().first, 0.3f), "a bus at unity passes its track on unchanged");
    require(rig.bus(0).peak.load() > 0.19f && rig.bus(0).peak.load() < 0.21f && rig.bus(1).peak.load() == 0.0f, "the bus meter shows its track; an empty bus stays dark");
    rig.bus(0).gain.store(0.5f);
    require(approx(rig.settled().first, 0.2f), "the bus fader scales only the tracks routed to it");
    rig.bus(0).gain.store(1.0f); rig.bus(0).pan.store(-1.0f);
    auto [l, r] = rig.settled();
    require(approx(l, 0.3f) && approx(r, 0.1f), "the bus pan uses the track balance law");
    rig.bus(0).pan.store(0.0f); rig.bus(0).mute.store(true);
    require(approx(rig.settled().first, 0.1f), "a muted bus silences its tracks");
    rig.bus(0).mute.store(false);
    rig.effect(alien::PluginRuntime::busIndex(0), 2.0f);
    require(approx(rig.settled().first, 0.5f), "the bus FX process what reaches the bus");
    rig.route(1, 0);
    require(approx(rig.settled().first, 0.6f), "two tracks sum into one bus before its FX");
    rig.track(1).gain.store(0.0f);
    require(approx(rig.settled().first, 0.4f), "a track's fader still applies before the bus");
    rig.track(1).gain.store(1.0f);
    rig.track(1).output.store(3);   // bus 3 does not exist
    require(approx(rig.settled().first, 0.5f), "a track routed to a missing bus plays through the master");
    rig.bus(0).active.store(false);
    require(approx(rig.settled().first, 0.3f), "and so does a track whose bus is deleted");
}

static void soloAcrossBuses() {
    Rig rig;
    rig.route(0, 0);
    rig.track(0).solo.store(true);
    require(approx(rig.settled().first, 0.2f), "soloing a track keeps its bus audible and silences the others");
    rig.track(0).solo.store(false); rig.track(1).solo.store(true);
    require(approx(rig.settled().first, 0.1f), "soloing a track on the master silences the bus and its tracks");
    rig.track(1).solo.store(false); rig.bus(0).solo.store(true);
    require(approx(rig.settled().first, 0.2f), "soloing a bus keeps its tracks audible and silences the rest");
    rig.route(1, 1); rig.bus(1).solo.store(false);
    require(approx(rig.settled().first, 0.2f), "a soloed bus silences another bus");
    rig.bus(1).solo.store(true);
    require(approx(rig.settled().first, 0.3f), "two soloed buses are both heard");
    rig.bus(1).solo.store(false); rig.bus(0).mute.store(true);
    require(approx(rig.settled().first, 0.0f), "mute beats solo on a bus");
    rig.bus(0).mute.store(false); rig.bus(0).solo.store(false);
    rig.track(0).mute.store(true);
    require(approx(rig.settled().first, 0.1f), "a muted track on a bus is silent");
    rig.track(0).mute.store(false);
    // A soloed track is still heard through its sends; the tracks solo silences send nothing.
    rig.track(0).send[0].store(1.0f); rig.track(1).send[0].store(1.0f);
    rig.track(0).solo.store(true);
    require(approx(rig.settled().first, 0.4f), "a soloed track on a bus is heard through the bus and its return");
    rig.track(0).solo.store(false); rig.bus(1).solo.store(true);
    require(approx(rig.settled().first, 0.2f), "soloing a bus keeps its tracks' sends; the others are silenced with their sends");
    // A bus that does not exist cannot be soloed.
    rig.bus(1).solo.store(false); rig.bus(5).solo.store(true);
    require(approx(rig.settled().first, 0.6f), "the solo switch of a missing bus does nothing");
}

static void busDelayCompensation() {
    Rig rig;
    rig.route(0, 0);
    rig.effect(alien::PluginRuntime::busIndex(0), 1.0f, 4);   // the bus's FX delay it by 4 samples
    rig.track(1).send[0].store(1.0f);                         // track 1: dry to the master and through return A
    auto& plugins = *rig.engine->plugins;
    require(plugins.busLatency() == 4 && plugins.latencySamples() == 4, "the slowest bus chain adds to the reported latency");
    rig.settled();
    rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
    std::vector<float> out;
    for (int b = 0; b < 4; ++b) { rig.render(); out.insert(out.end(), rig.left.begin(), rig.left.end()); }
    std::size_t first = 0; while (first < out.size() && std::abs(out[first]) < 1.0e-6f) ++first;
    require(first == 4, "the first hit is late by exactly the bus latency");
    require(approx(out[first], 0.4f) && approx(out[first + 300], 0.4f), "the bus, the dry track and its return arrive together and stay together");

    // A latent return on top: the bus stage and the return stage add up.
    rig.effect(alien::PluginRuntime::returnIndex(0), 1.0f, 3);
    require(plugins.returnLatency() == 3 && plugins.latencySamples() == 7, "bus and return latencies add up");
    rig.settled();
    rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
    out.clear();
    for (int b = 0; b < 4; ++b) { rig.render(); out.insert(out.end(), rig.left.begin(), rig.left.end()); }
    first = 0; while (first < out.size() && std::abs(out[first]) < 1.0e-6f) ++first;
    require(first == 7 && approx(out[first], 0.4f), "everything still lines up behind both stages");
}

static void realTimeSafety() {
    Rig rig;
    rig.route(0, 0); rig.route(1, 7);
    rig.effect(alien::PluginRuntime::busIndex(0), 1.0f, 2);
    rig.track(0).send[1].store(0.5f);
    rig.render(4);
    const auto before = allocations;
    rig.render(6);
    rig.bus(0).solo.store(true); rig.bus(7).pan.store(0.4f); rig.track(1).output.store(-1);
    rig.render(6);
    rig.bus(0).active.store(false); rig.track(0).solo.store(true); rig.bus(7).mute.store(true);
    rig.render(6);
    require(allocations == before, "routing through buses, soloing and re-routing allocate nothing");
}

static void projectFormat() {
    alien::SessionProjectData project;
    project.buses[2] = {true, "Drums", {0.5f, -0.25f, true, false}};
    project.buses[5] = {true, "Vocals", {1.5f, 0.0f, false, true}};
    project.trackSettings[0].output = 2; project.trackSettings[3].output = 5;
    juce::PluginDescription desc; desc.name = "Missing glue compressor"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "/missing/glue.vst3";
    project.busPlugins[2].effects.push_back({desc.createXml()->toString(), "YWJj", false});
    require(project.valid(), "a project with group buses is valid");
    const auto json = alien::serializeSessionProject(project);
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 19, "projects with buses are written as format 19 or later");
    alien::SessionProjectData back; juce::String error;
    require(alien::parseSessionProject(json, back, error), "and parse");
    require(back.buses == project.buses && back.busPlugins == project.busPlugins && back.trackSettings[0].output == 2
            && back.trackSettings[3].output == 5 && back.trackSettings[1].output == -1, "buses, their FX and track outputs round-trip");
    {   // The document keeps them too.
        const auto doc = alien::fromProjectData(back);
        require(doc.buses[2].used && doc.buses[2].name == "Drums" && doc.buses[2].plugins == project.busPlugins[2] && !doc.buses[0].used
                && alien::toProjectData(doc).buses == project.buses, "the document round-trips buses");
    }
    {   // Format 17 (before buses): every track goes to the master.
        auto old = juce::JSON::parse(json);
        old.getDynamicObject()->setProperty("version", 17);
        old.getDynamicObject()->removeProperty("buses");
        for (auto& t : *old.getDynamicObject()->getProperty("trackSettings").getArray()) t.getDynamicObject()->removeProperty("output");
        alien::SessionProjectData older;
        require(alien::parseSessionProject(juce::JSON::toString(old), older, error), error.toRawUTF8());
        bool master = true;
        for (const auto& t : older.trackSettings) master = master && t.output == -1;
        require(master && older.buses == alien::SessionBuses{} && older.busPlugins == alien::BusRacks{}, "an older project opens with every track to the master and no buses");
    }
    auto damage = [&](const char* what, auto&& mutate) {
        auto copy = juce::JSON::parse(json);
        mutate(copy.getDynamicObject());
        alien::SessionProjectData out; juce::String why;
        if (alien::parseSessionProject(juce::JSON::toString(copy), out, why)) { std::cerr << "accepted: " << what << '\n'; require(false, "damaged bus routing must be refused"); }
        require(why.isNotEmpty(), "and a reason is given");
    };
    auto track = [](juce::DynamicObject* root, int t) { return root->getProperty("trackSettings").getArray()->getReference(t).getDynamicObject(); };
    auto bus = [](juce::DynamicObject* root, int i) { return root->getProperty("buses").getArray()->getReference(i).getDynamicObject(); };
    damage("output to slot 99", [&](auto* root) { track(root, 0)->setProperty("output", 99); });
    damage("output -2", [&](auto* root) { track(root, 0)->setProperty("output", -2); });
    damage("output to an unused slot", [&](auto* root) { track(root, 0)->setProperty("output", 4); });
    damage("text output", [&](auto* root) { track(root, 0)->setProperty("output", "Drums"); });
    damage("format 19 without an output", [&](auto* root) { track(root, 1)->removeProperty("output"); });
    damage("format 19 without buses", [&](auto* root) { root->removeProperty("buses"); });
    damage("buses not a list", [&](auto* root) { root->setProperty("buses", 3); });
    damage("slot 8", [&](auto* root) { bus(root, 0)->setProperty("slot", alien::maxBuses); });
    damage("two buses in one slot", [&](auto* root) { bus(root, 1)->setProperty("slot", 2); });
    damage("bus gain 3", [&](auto* root) { bus(root, 0)->setProperty("gain", 3.0); });
    damage("bus pan -2", [&](auto* root) { bus(root, 0)->setProperty("pan", -2.0); });
    damage("numeric bus solo", [&](auto* root) { bus(root, 0)->setProperty("solo", 1); });
    damage("empty bus name", [&](auto* root) { bus(root, 0)->setProperty("name", "  "); });
    damage("bus without plugins", [&](auto* root) { bus(root, 0)->removeProperty("plugins"); });
    damage("an instrument on a bus", [&](auto* root) {
        bus(root, 1)->setProperty("plugins", alien::rackToVar(alien::PluginRackState{{desc.createXml()->toString(), {}, false}, {}})); });
    {   // In memory: FX in an unused slot, or a track pointing at one, is not a valid project.
        auto bad = project; bad.busPlugins[0] = project.busPlugins[2];
        require(!bad.valid(), "FX in an unused bus slot are refused");
        bad = project; bad.trackSettings[2].output = 1;
        require(!bad.valid() && alien::serializeSessionProject(bad).isEmpty(), "a track routed to an unused slot is never written");
    }
}

static void controllerAndUndo() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    const int drums = c.addBus("Drums");
    require(drums == 0 && c.document().buses[0].used && c.document().buses[0].name == "Drums" && engine.buses[0].active.load(), "adding a bus takes the first slot and tells the engine");
    c.commit("Add bus");
    require(c.addBus() == 1 && c.document().buses[1].name == "Bus 1", "an unnamed bus gets the next free default name");
    c.commit("Add bus");
    require(c.setTrackOutput(2, 1) && engine.tracks[2].output.load() == 1 && c.document().tracks[2].settings.output == 1, "a track's output goes to the engine and the document");
    require(!c.setTrackOutput(2, 1) && !c.setTrackOutput(2, 6) && !c.setTrackOutput(9, 0) && !c.setTrackOutput(0, -3), "no change, an unused bus, a missing track or a bad output is refused");
    c.commit("Change track output");
    engine.buses[1].gain.store(0.25f); engine.buses[1].solo.store(true); c.busSettingsEdited(1); c.commit("Bus mix");
    require(c.document().buses[1].settings == (alien::BusSettings{0.25f, 0.0f, false, true}), "bus strip edits reach the document");
    engine.buses[1].gain.store(5.0f); c.busSettingsEdited(1);
    require(c.document().buses[1].settings.gain == 0.25f, "an out-of-range bus level never reaches the document");
    engine.buses[1].gain.store(0.25f);
    require(c.renameBus(1, "  Keys  ") && c.document().buses[1].name == "Keys" && !c.renameBus(1, " ") && !c.renameBus(4, "X"), "renaming trims, and refuses empty names and unused slots");
    c.commit("Rename bus");
    require(c.deleteBus(1) && !c.document().buses[1].used && c.document().tracks[2].settings.output == -1 && engine.tracks[2].output.load() == -1
            && !engine.buses[1].active.load(), "deleting a bus sends its tracks back to the master");
    c.commit("Delete bus");
    require(alien::toProjectData(c.document()).valid(), "the document stays valid");
    require(c.undo() && c.document().buses[1].used && c.document().buses[1].name == "Keys" && c.document().tracks[2].settings.output == 1
            && engine.tracks[2].output.load() == 1 && engine.buses[1].active.load() && engine.buses[1].solo.load(), "undo brings the bus, its strip and its tracks back");
    require(c.undo() && c.document().buses[1].name == "Bus 1", "undo rename");
    require(c.undo() && c.document().buses[1].settings == alien::BusSettings{} && engine.buses[1].gain.load() == 1.0f, "undo the bus strip edit");
    require(c.undo() && c.document().tracks[2].settings.output == -1 && engine.tracks[2].output.load() == -1, "undo the output change");
    require(c.undo() && !c.document().buses[1].used && !engine.buses[1].active.load(), "undo the second bus");
    require(c.redo() && c.redo() && c.redo() && c.redo() && c.redo() && !c.document().buses[1].used && c.document().tracks[2].settings.output == -1, "redo replays all of it");
    for (int b = 2; b <= alien::maxBuses; ++b) c.addBus();
    require(c.addBus() == -1, "there are at most eight buses");
}

static void panels() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    alien::TrackMixerPanel panel(c, engine.tracks, engine.returns, engine.masterControls, engine.buses);
    panel.setSize(1600, 720);
    juce::StringArray routing; int busEdits = 0;
    panel.onRoutingChanged = [&](const juce::String& label) { routing.add(label); c.commit(label); };
    panel.onBusEdited = [&](int b, bool) { c.busSettingsEdited(b); ++busEdits; };
    require(panel.busStripCount() == 0 && panel.addBusControl().isEnabled(), "a new project has no buses and can add one");
    panel.addBusControl().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(panel.busStripCount() == 1 && routing.size() == 1 && routing[0] == "Add bus" && c.undoLabel() == "Add bus", "Add bus makes a strip and one undo step");
    auto* strip = panel.trackStrip(1);
    require(strip != nullptr && strip->outputBox().getNumItems() == 2 && strip->outputBox().getText() == "Master", "a track's Output lists the master and the bus");
    strip->outputBox().setSelectedId(2, juce::sendNotificationSync);
    require(c.document().tracks[1].settings.output == 0 && engine.tracks[1].output.load() == 0 && routing[1] == "Change track output",
            "choosing the bus routes the track");
    require(panel.trackStrip(1)->outputBox().getText() == "Bus 1", "and the menu shows it");
    auto* busStrip = panel.busStrip(0);
    require(busStrip != nullptr && busStrip->isVisible() && busStrip->getWidth() > 60, "the bus strip is laid out");
    require(busStrip->getX() > panel.trackStrip(3)->getRight(), "after the track strips");
    busStrip->volume().setValue(50.0, juce::sendNotificationSync);
    require(approx(engine.buses[0].gain.load(), 0.5f) && busEdits > 0 && c.document().buses[0].settings.gain == 0.5f, "its fader sets the bus level");
    busStrip->soloButton().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(engine.buses[0].solo.load(), "its solo button solos the bus");
    require(panel.renameBus(0, "Drums") && panel.trackStrip(1)->outputBox().getText() == "Drums", "renaming the bus renames it in every Output menu");
    for (int b = 1; b < alien::maxBuses; ++b) panel.addBus();
    require(panel.busStripCount() == alien::maxBuses && !panel.addBusControl().isEnabled(), "Add bus is disabled with eight buses");
    bool captured = false;
    panel.beforeBusDeleted = [&] { captured = true; };
    require(panel.deleteBus(0) && captured && panel.busStrip(0) == nullptr && panel.trackStrip(1)->outputBox().getText() == "Master"
            && c.document().tracks[1].settings.output == -1, "deleting a bus removes its strip and sends its track to the master");
    require(c.undo() && c.document().buses[0].used && c.document().tracks[1].settings.output == 0, "deleting a bus is undoable");

    // The plugin panel offers each bus's FX rack, without an instrument slot.
    alien::PluginPanel plugins;
    juce::KnownPluginList none;
    std::array<juce::String, alien::maxBuses> names; names[3] = "Strings";
    plugins.refresh({"Track 1"}, {}, none, names);
    juce::ComboBox* target = nullptr;
    for (auto* child : plugins.getChildren()) if (child->getName().startsWith("Plugin destination")) target = dynamic_cast<juce::ComboBox*>(child);
    require(target != nullptr, "the plugin panel has a destination chooser");
    juce::StringArray items; for (int i = 0; i < target->getNumItems(); ++i) items.add(target->getItemText(i));
    require(items.contains("Strings FX (bus)") && items.size() == 1 + 1 + alien::defaultReturns + 1, "it lists only the buses in use");
    target->setSelectedId(alien::PluginRuntime::busIndex(3) + 1, juce::sendNotificationSync);
    plugins.refresh({"Track 1"}, {}, none, names);
    require(plugins.destination() == alien::PluginRuntime::busIndex(3), "choosing a bus targets its rack");
    plugins.refresh({"Track 1"}, {}, none, {});
    require(plugins.destination() == alien::maxSessionTracks, "a deleted bus falls back to Master FX");

    // The plugin host keeps a bus's saved FX even when the plugin is missing.
    auto runtime = std::make_unique<alien::PluginRuntime>();
    alien::PluginHost host(*runtime);
    juce::PluginDescription desc; desc.name = "Missing bus EQ"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "/missing/eq.vst3";
    std::vector<alien::PluginRackState> tracks(4); alien::PluginRackState master; alien::ReturnRacks returns; alien::BusRacks buses;
    buses[6].effects.push_back({desc.createXml()->toString(), "YWJj", true});
    const auto saved = buses;
    const auto errors = host.restore(tracks, master, returns, buses, 48000);
    require(errors.isNotEmpty() && runtime->racks[static_cast<std::size_t>(alien::PluginRuntime::busIndex(6))].effectCount == 1, "a missing bus FX is reported and its slot kept");
    host.snapshot(tracks, master, returns, buses);
    require(buses == saved, "and its saved state survives a snapshot");
}

static void stemCaptureWithBuses() {
    Rig rig;
    rig.route(0, 0);
    rig.effect(alien::PluginRuntime::busIndex(0), 1.0f, 4);
    rig.effect(alien::PluginRuntime::returnIndex(0), 1.0f, 2);
    rig.track(1).send[0].store(0.5f);
    rig.settled();
    rig.engine->stemCapture = std::make_unique<alien::StemCapture>();
    rig.engine->stemCapture->prepare();
    rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
    std::array<float, 512> l{}, r{};
    float* outputs[]{l.data(), r.data()};
    const auto before = allocations;
    rig.engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    require(allocations == before, "capturing bus stems allocates nothing on the audio callback");
    const auto& buses = rig.engine->stemCapture->buses;
    for (int i = 0; i < 512; ++i) {
        // Track 0 goes into bus 0, so its own file is left out of the sum.
        float sum = 0; for (std::size_t b = 1; b < buses.size(); ++b) sum += buses[b].getSample(0, i);
        require(approx(sum, l[static_cast<std::size_t>(i)]), "bus, track, return and rack stems reconstruct the pre-master mix");
    }
    const auto first = [&](int bus) { int i = 0; while (i < 512 && std::abs(buses[static_cast<std::size_t>(bus)].getSample(0, i)) < 1e-7f) ++i; return i; };
    require(first(alien::StemCapture::busBase) == 6 && first(1) == 6 && first(0) == 6 && first(alien::StemCapture::returnBase) == 6,
            "bus, dry track, the track inside the bus and return stems all line up behind the bus and return latency");
    require(approx(buses[alien::StemCapture::busBase].getSample(0, 100), 0.2f) && approx(buses[0].getSample(0, 100), 0.2f), "the bus file holds its track");
}

// A one-bar song where tracks 0 and 1 play the built-in synth.
static std::unique_ptr<alien::ArrangementPlayback> songWithNotes(alien::SessionProjectData& project) {
    auto song = std::make_unique<alien::ArrangementPlayback>(); song->timeline.setLengthBars(1);
    alien::PianoRollPattern notes; notes.loopSteps = 16; notes.add(60, 0, 16, 1);
    for (int t = 0; t < 2; ++t) {
        alien::timeline::Region region; region.row = t; region.source = 0;
        require(song->timeline.place(region), "song region");
        song->clips[0][static_cast<std::size_t>(t)].pattern = notes;
    }
    project.timeline = song->timeline;
    return song;
}

static void stemExportFiles() {
    auto project = std::make_unique<alien::SessionProjectData>();
    project->buses[1] = {true, "Drums", {}};
    project->trackSettings[0].output = 1;
    project->trackNames[0] = "Kick"; project->trackNames[1] = "Bass";
    auto song = songWithNotes(*project);
    require(project->valid(), "stem fixture");
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienBusStems", "", false);
    alien::WavExportOptions options; options.stems = true;
    alien::DrumSamplePointers drums{}; std::atomic<bool> stop{false}; juce::String error;
    require(alien::exportArrangementWav(folder, *project, *song, drums, stop, error, nullptr, options), error.toRawUTF8());
    require(folder.getChildFile("01 - Kick (into Drums).wav").existsAsFile() && folder.getChildFile("02 - Bass.wav").existsAsFile()
            && folder.getChildFile("Bus 2 - Drums.wav").existsAsFile(), "stems include each bus, and name the tracks that go into one");
    require(folder.getChildFile("README.txt").loadFileAsString().contains("Group buses"), "the notes explain bus files");
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto peak = [&](const juce::String& name) {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(folder.getChildFile(name)));
        require(reader != nullptr, "read a stem");
        juce::AudioBuffer<float> data(2, static_cast<int>(reader->lengthInSamples));
        reader->read(&data, 0, data.getNumSamples(), 0, true, true);
        return data.getMagnitude(0, data.getNumSamples());
    };
    const float busPeak = peak("Bus 2 - Drums.wav"), trackPeak = peak("01 - Kick (into Drums).wav");
    require(busPeak > 0.01f && std::abs(busPeak - trackPeak) < 1.0e-3f, "the bus file holds the track routed to it");
    folder.deleteRecursively();
}

static void freezeIgnoresBus() {
    auto project = std::make_unique<alien::SessionProjectData>();
    project->buses[0] = {true, "Glue", {0.25f, 0.0f, true, false}};
    project->trackSettings[0].output = 0;
    auto song = songWithNotes(*project);
    auto renderer = std::make_unique<ToneEngine>(); auto& rack = renderer->plugins->racks[0];
    rack.instrumentAssigned = true; rack.instrument.processor = std::make_unique<TestProcessor>(true);
    rack.effectCount = 1; rack.effects[0].processor = std::make_unique<TestProcessor>(false, 12.0f);
    auto& busRack = renderer->plugins->racks[static_cast<std::size_t>(alien::PluginRuntime::busIndex(0))];
    busRack.effectCount = 1; busRack.effects[0].processor = std::make_unique<TestProcessor>(false, 0.5f, 3);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienBusFreeze", ".wav");
    alien::WavExportOptions options; options.freezeTrack = 0; options.toTick = alien::timeline::ticksPerBar;
    alien::DrumSamplePointers drums{}; std::atomic<bool> stop{false}; juce::String error;
    require(alien::exportArrangementWav(file, *project, *song, drums, stop, error, renderer.get(), options), error.toRawUTF8());
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    juce::AudioBuffer<float> probe(2, 512); require(reader && reader->read(&probe, 0, 512, 512, true, true), "read frozen audio");
    require(approx(probe.getSample(0, 200), 2.4f), "freezing a track on a bus renders the track alone: no bus fader, mute or FX");
    require(!busRack.frozen, "export restores the bus rack afterwards");
    reader.reset(); file.deleteFile();
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    busLevels();
    soloAcrossBuses();
    busDelayCompensation();
    realTimeSafety();
    projectFormat();
    controllerAndUndo();
    panels();
    stemCaptureWithBuses();
    stemExportFiles();
    freezeIgnoresBus();
    std::cout << "PASS: group buses (levels, solo, delay compensation, no allocation, format, undo, panels, stems, freeze)\n";
    return 0;
}
