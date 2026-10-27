// Flexible return tracks (MIX-05): adding, removing and renaming returns, more than four of them,
// sends that keep their return when another is removed, exact levels and delay alignment beyond
// return D, a return slot that comes back clean, no allocation while mixing, the project format
// (round trip, older files, damage), undo and redo, the mixer, Plugins and automation panels,
// stems and freeze. Also prints what each return slot costs the audio callback (no timing is
// asserted: the machine may be busy).
#include "ToneEngine.h"
#include "WavExport.h"
#include "PluginHost.h"
#include "PluginPanel.h"
#include "ProjectController.h"
#include "TrackMixerPanel.h"
#include "TimelineView.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);} void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

static void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
// The reason is read after the call that fills it in.
static void requireOk(bool ok, const juce::String& error, const char* what) { if (!ok) { std::cerr << "FAILED: " << what << ": " << error << '\n'; std::exit(1); } }
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
        auto it = midi.begin(); const auto end = midi.end();
        for (int i = 0; i < b.getNumSamples(); ++i) {
            while (it != end && (*it).samplePosition <= i) { const auto m = (*it).getMessage(); if (m.isNoteOn()) active = true; else if (m.isNoteOff() || m.isAllNotesOff() || m.isAllSoundOff()) active = false; ++it; }
            for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, i, (synth ? (active ? 0.2f : 0.0f) : b.getSample(c, i)) * factor);
            if (delay) { for (int c = 0; c < 2; ++c) { const auto value = b.getSample(c, i); b.setSample(c, i, ring[static_cast<std::size_t>(c * 16 + position)]); ring[static_cast<std::size_t>(c * 16 + position)] = value; } position = (position + 1) % delay; }
        }
    }
    const juce::String getName() const override { return "Original return test processor"; }
    bool acceptsMidi() const override { return synth; } bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; } void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; } juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {} void setStateInformation(const void*, int) override {}
    bool synth{}, active{}; float factor; int delay{}, position{}; std::array<float, 32> ring{};
};

// An engine whose track 0 holds a note on a test instrument (0.2) for the whole run.
struct Rig {
    std::unique_ptr<ToneEngine> engine = std::make_unique<ToneEngine>();
    std::array<float, 1024> left{}, right{};
    Rig() {
        engine->prepare(48000); engine->patternMode.store(1); engine->launchQuantizationSteps.store(4);
        auto& rack = engine->plugins->racks[0]; rack.instrumentAssigned = true;
        rack.instrument.processor = std::make_unique<TestProcessor>(true); engine->plugins->prepareSlot(rack.instrument);
        alien::SessionClipLaunch clip; clip.pattern.clear(); clip.pattern.add(60, 0.0, 64.0); clip.clipIndex = 0;
        engine->pianoRollLaunches[0].publish(clip); engine->playing.store(true);
    }
    void returnEffect(int r, float gain, int latency = 0) {
        auto& rack = engine->plugins->racks[static_cast<std::size_t>(alien::PluginRuntime::returnIndex(r))];
        rack.effects[static_cast<std::size_t>(rack.effectCount)].processor = std::make_unique<TestProcessor>(false, gain, latency);
        engine->plugins->prepareSlot(rack.effects[static_cast<std::size_t>(rack.effectCount++)]);
    }
    void render(int blocks = 1) {
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 1024, {});
    }
    std::pair<float, float> settled() { render(12); return {left[1023], right[1023]}; }
    alien::TrackControls& track(int t = 0) { return engine->tracks[static_cast<std::size_t>(t)]; }
    alien::ReturnControls& ret(int r) { return engine->returns[static_cast<std::size_t>(r)]; }
};

static void returnsBeyondD() {
    Rig rig;
    for (int r = 0; r < alien::maxReturns; ++r) require(rig.ret(r).active.load() == (r < alien::defaultReturns), "a new engine has returns A-D");
    rig.track().send[5].store(1.0f);
    require(approx(rig.settled().first, 0.2f), "a send to a return that does not exist is not heard");
    require(rig.ret(5).peak.load() == 0.0f, "and the missing return's meter stays dark");
    rig.ret(5).active.store(true);
    auto [l, r] = rig.settled();
    require(approx(l, 0.4f) && approx(r, 0.4f), "once return F exists, the send reaches it at its exact level");
    require(rig.ret(5).peak.load() > 0.19f, "and its meter shows it");
    // Every return in use at once, each with its own send level and fader: the sum is exact.
    float expected = 0.2f;
    for (int k = 0; k < alien::maxReturns; ++k) {
        rig.ret(k).active.store(true);
        const float send = 0.1f * static_cast<float>(k + 1), gain = k % 2 == 0 ? 1.0f : 0.5f;
        rig.track().send[static_cast<std::size_t>(k)].store(send); rig.ret(k).gain.store(gain);
        expected += 0.2f * send * gain;
    }
    require(approx(rig.settled().first, expected), "eight returns sum to exactly the expected level");
    rig.ret(7).pan.store(-1.0f);
    auto [pl, pr] = rig.settled();
    require(approx(pl, expected) && approx(pr, expected - 0.2f * 0.8f * 0.5f), "return H's pan uses the balance law");
    rig.ret(7).mute.store(true);
    require(approx(rig.settled().first, expected - 0.2f * 0.8f * 0.5f), "and muting return H takes it out");
    rig.track().sendPre[6].store(true); rig.track().gain.store(0.0f);
    require(approx(rig.settled().first, 0.2f * 0.7f), "a pre-fader send to return G ignores the track fader");
    rig.track().mute.store(true);
    require(approx(rig.settled().first, 0.0f), "and a muted track sends nothing, even to return G");
    rig.track().mute.store(false); rig.track(1).solo.store(true);
    require(approx(rig.settled().first, 0.0f), "soloing another track silences this one's sends beyond D too");
}

static void delayAlignment() {
    Rig rig;
    rig.ret(6).active.store(true);
    rig.returnEffect(6, 1.0f, 5);
    rig.track().send[6].store(1.0f); rig.track().send[1].store(1.0f);
    require(rig.engine->plugins->returnLatency() == 5, "a latent FX on return G sets the returns' latency");
    rig.settled();
    rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
    std::vector<float> out;
    for (int b = 0; b < 4; ++b) { rig.render(); out.insert(out.end(), rig.left.begin(), rig.left.end()); }
    std::size_t first = 0; while (first < out.size() && std::abs(out[first]) < 1.0e-6f) ++first;
    require(first == 5 && approx(out[first], 0.6f) && approx(out[first + 100], 0.6f),
            "dry, return B and the latent return G all start together, exactly 5 samples late");
}

// A slot that is removed and later reused must not play what was left in its delay lines.
static void slotComesBackClean() {
    Rig rig;
    rig.returnEffect(0, 1.0f, 4);         // return A is latent, so return B's line holds 4 samples
    rig.track().send[1].store(1.0f);
    rig.settled();
    rig.ret(1).active.store(false); rig.track().send[1].store(0.0f);
    rig.render();
    rig.track().mute.store(true);
    rig.settled();
    rig.ret(1).active.store(true);
    rig.render();
    float loudest = 0; for (const auto s : rig.left) loudest = std::max(loudest, std::abs(s));
    require(loudest == 0.0f, "a return that comes back starts silent, with nothing left from before");
}

static void realTimeSafety() {
    Rig rig;
    rig.returnEffect(7, 1.0f, 3);
    for (int r = 0; r < alien::maxReturns; ++r) {
        rig.ret(r).active.store(true);
        rig.track().send[static_cast<std::size_t>(r)].store(0.3f); rig.track().sendPre[static_cast<std::size_t>(r)].store(r % 2 == 0);
    }
    rig.render(4);
    const auto before = allocations;
    rig.render(4);
    rig.ret(2).active.store(false); rig.ret(5).active.store(false);
    rig.render(4);
    rig.ret(2).active.store(true); rig.ret(5).active.store(true);   // coming back clears their lines in place
    rig.render(4);
    require(allocations == before, "mixing eight returns, and removing and restoring returns, allocates nothing");
}

static void controllerAndUndo() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    const auto& d = c.document();
    for (int r = 0; r < alien::maxReturns; ++r)
        require(d.returns[static_cast<std::size_t>(r)].used == (r < 4) && engine.returns[static_cast<std::size_t>(r)].active.load() == (r < 4),
                "a new project has returns A-D, in the document and the engine");
    require(d.returns[2].name == "Return C", "with their default names");
    require(c.addReturn("  Reverb  ") == 4 && d.returns[4].used && d.returns[4].name == "Reverb" && engine.returns[4].active.load(),
            "adding a return takes the first free slot, trims its name and tells the engine");
    c.commit("Add return");
    require(c.addReturn() == 5 && d.returns[5].name == "Return F", "an unnamed return is named after its slot");
    c.commit("Add return");
    // Sends to B, D and F, and send automation for B and D.
    engine.tracks[0].send[1].store(0.3f); engine.tracks[0].send[3].store(0.6f); engine.tracks[0].sendPre[3].store(true);
    engine.tracks[0].send[5].store(0.9f); engine.tracks[2].send[1].store(0.4f); engine.tracks[2].sendPre[1].store(true);
    c.trackSettingsEdited(0); c.trackSettingsEdited(2); c.commit("Sends");
    require(c.editTimeline("Automate", [](alien::timeline::Timeline& t) {
        t.automationLanes.push_back({0, alien::automation::Parameter::sendB, true, {{0, 0.5f}}});
        t.automationLanes.push_back({0, alien::automation::Parameter::sendD, true, {{0, 0.25f}}});
        t.automationLanes.push_back({1, alien::automation::Parameter::sendF, true, {{0, 0.75f}}});
        return true; }), "send automation for existing returns, including F");
    require(!c.editTimeline("Automate", [](alien::timeline::Timeline& t) {
        t.automationLanes.push_back({0, alien::automation::Parameter::sendH, true, {{0, 0.5f}}}); return true; }),
            "send automation for a return that does not exist is refused");
    engine.returns[5].gain.store(0.5f); c.returnSettingsEdited(5); c.commit("Return F mix");
    require(d.returns[5].settings.gain == 0.5f, "the new return's strip reaches the document");
    engine.returns[6].gain.store(0.5f); c.returnSettingsEdited(6);
    require(d.returns[6].settings.gain == 1.0f, "an unused slot's strip never reaches the document");
    engine.returns[6].gain.store(1.0f);

    require(c.renameReturn(5, " Delay ") && d.returns[5].name == "Delay", "renaming trims the name");
    require(!c.renameReturn(5, "  ") && !c.renameReturn(6, "X") && !c.renameReturn(5, "Delay") && !c.renameReturn(-1, "X"),
            "an empty name, an unused slot, no change or a bad slot is refused");
    c.commit("Rename return");

    // Removing B: its sends go, and nothing slides onto the wrong return.
    require(c.deleteReturn(1) && !d.returns[1].used && d.returns[1].name.isEmpty() && !engine.returns[1].active.load(), "removing a return frees its slot");
    const auto& s0 = d.tracks[0].settings; const auto& s2 = d.tracks[2].settings;
    require(s0.send[1] == 0.0f && s2.send[1] == 0.0f && !s2.sendPre[1] && engine.tracks[0].send[1].load() == 0.0f && !engine.tracks[2].sendPre[1].load(),
            "every send to it is turned off, in the document and the engine");
    require(s0.send[3] == 0.6f && s0.sendPre[3] && s0.send[5] == 0.9f && s0.send[2] == 0.0f && s0.send[4] == 0.0f
            && approx(engine.tracks[0].send[3].load(), 0.6f) && approx(engine.tracks[0].send[5].load(), 0.9f),
            "the sends to D and F stay on D and F");
    int bLanes = 0, dLanes = 0, fLanes = 0;
    for (const auto& lane : d.timeline.automationLanes) {
        bLanes += lane.parameter == alien::automation::Parameter::sendB;
        dLanes += lane.parameter == alien::automation::Parameter::sendD;
        fLanes += lane.parameter == alien::automation::Parameter::sendF;
    }
    require(bLanes == 0 && dLanes == 1 && fLanes == 1, "its send automation goes; the other sends' automation stays on its return");
    require(d.returns[3].name == "Return D" && d.returns[5].name == "Delay" && d.returns[5].settings.gain == 0.5f, "the other returns keep their names and strips");
    c.commit("Remove return");
    require(alien::toProjectData(d).valid(), "the document stays valid");

    // Undo and redo, step by step.
    require(c.undo() && d.returns[1].used && d.returns[1].name == "Return B" && engine.returns[1].active.load()
            && d.tracks[0].settings.send[1] == 0.3f && engine.tracks[2].sendPre[1].load() && d.timeline.automationLanes.size() == 3,
            "undo brings the return back with its sends and automation");
    require(c.undo() && d.returns[5].name == "Return F", "undo the rename");
    require(c.undo() && d.returns[5].settings.gain == 1.0f && engine.returns[5].gain.load() == 1.0f, "undo the strip edit");
    require(c.undo() && d.timeline.automationLanes.empty(), "undo the automation");
    require(c.undo() && d.tracks[0].settings.send[5] == 0.0f && engine.tracks[0].send[5].load() == 0.0f, "undo the sends");
    require(c.undo() && !d.returns[5].used && !engine.returns[5].active.load(), "undo adding F");
    require(c.undo() && !d.returns[4].used && !engine.returns[4].active.load(), "undo adding the first new return");
    for (int i = 0; i < 7; ++i) require(c.redo(), "redo");
    require(!d.returns[1].used && d.returns[5].name == "Delay" && approx(engine.tracks[0].send[3].load(), 0.6f) && engine.tracks[0].send[1].load() == 0.0f,
            "redo replays all of it");

    // A new return reuses the free slot B, with its sends off.
    require(c.addReturn() == 1 && d.returns[1].name == "Return B" && d.tracks[0].settings.send[1] == 0.0f, "the next new return takes slot B, with every send to it off");
    c.commit("Add return");
    while (c.addReturn() >= 0) {}
    int used = 0; for (const auto& ret : d.returns) used += ret.used;
    require(used == alien::maxReturns, "there are at most eight returns");
    // And all of them can go.
    for (int r = 0; r < alien::maxReturns; ++r) require(c.deleteReturn(r), "remove every return");
    require(!c.deleteReturn(0) && alien::toProjectData(d).valid(), "a project with no returns is valid");
}

static void projectFormat() {
    alien::SessionProjectData project;
    project.returns[1] = {};                                   // B removed
    project.returns[5] = {true, "Delay", {0.5f, -0.25f, true}};
    project.returns[7] = {true, "Room", {}};
    project.trackSettings[1].send = {0.25f, 0.0f, 1.0f, 0.5f, 0.0f, 0.75f, 0.0f, 0.1f};
    project.trackSettings[1].sendPre = {true, false, false, true, false, true, false, false};
    juce::PluginDescription desc; desc.name = "Missing reverb"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "/missing/reverb.vst3";
    project.returnPlugins[5].effects.push_back({desc.createXml()->toString(), "YWJj", false});
    project.timeline.automationLanes.push_back({1, alien::automation::Parameter::sendH, true, {{0, 0.5f}}});
    require(project.valid(), "a project with returns beyond D and a gap is valid");
    const auto json = alien::serializeSessionProject(project);
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 23, "projects are written as format 23 or later");
    alien::SessionProjectData back; juce::String error;
    requireOk(alien::parseSessionProject(json, back, error), error, "the format 23 round trip");
    require(back.returns == project.returns && back.returnPlugins == project.returnPlugins
            && back.trackSettings[1].send == project.trackSettings[1].send && back.trackSettings[1].sendPre == project.trackSettings[1].sendPre
            && back.timeline.automationLanes == project.timeline.automationLanes,
            "returns keep their slots, names, strips and FX, and sends and send automation their returns");
    const auto parsedJson = juce::JSON::parse(json);
    require(parsedJson.getProperty("returns", {}).getArray()->size() == 5, "only the returns in use are written");

    {   // Version 21: always A-D, four sends a track, no slot or name.
        alien::SessionProjectData four;
        four.trackSettings[0].send = {0.5f, 0.0f, 0.0f, 0.25f};
        four.returns[2].settings = {0.5f, 0.0f, false};
        auto old = juce::JSON::parse(alien::serializeSessionProject(four));
        old.getDynamicObject()->setProperty("version", 21);
        for (auto& ret : *old.getDynamicObject()->getProperty("returns").getArray()) {
            ret.getDynamicObject()->removeProperty("slot"); ret.getDynamicObject()->removeProperty("name");
        }
        for (auto& t : *old.getDynamicObject()->getProperty("trackSettings").getArray())
            t.getDynamicObject()->getProperty("sends").getArray()->removeRange(4, alien::maxReturns - 4);
        alien::SessionProjectData older;
        requireOk(alien::parseSessionProject(juce::JSON::toString(old), older, error), error, "a version 21 project opens");
        bool exact = true;
        for (int r = 0; r < alien::maxReturns; ++r) {
            const auto& ret = older.returns[static_cast<std::size_t>(r)];
            exact = exact && ret.used == (r < 4) && ret.name == (r < 4 ? alien::defaultReturnName(r) : juce::String{});
        }
        require(exact && older.returns[2].settings.gain == 0.5f && older.trackSettings[0].send[0] == 0.5f && older.trackSettings[0].send[3] == 0.25f
                && older.trackSettings[0].send[4] == 0.0f, "with returns A-D exactly as before, and its sends on them");
        auto five = old;
        { const auto first = five.getDynamicObject()->getProperty("returns").getArray()->getFirst(); five.getDynamicObject()->getProperty("returns").getArray()->add(first); }
        alien::SessionProjectData out;
        require(!alien::parseSessionProject(juce::JSON::toString(five), out, error), "a version 21 project with five returns is refused");
        auto eight = old;
        for (auto& t : *eight.getDynamicObject()->getProperty("trackSettings").getArray())
            for (int k = 4; k < alien::maxReturns; ++k) { juce::Array<juce::var> send; send.add(0.0); send.add(false); t.getDynamicObject()->getProperty("sends").getArray()->add(juce::var(send)); }
        require(!alien::parseSessionProject(juce::JSON::toString(eight), out, error), "and so is one with eight sends a track");
    }
    {   // Version 12 and older: no returns or sends at all.
        auto old = juce::JSON::parse(json);
        old.getDynamicObject()->setProperty("version", 12);
        old.getDynamicObject()->removeProperty("returns");
        for (auto& t : *old.getDynamicObject()->getProperty("trackSettings").getArray()) t.getDynamicObject()->removeProperty("sends");
        auto* timelineObject = old.getDynamicObject()->getProperty("timeline").getDynamicObject();
        timelineObject->setProperty("automation", juce::Array<juce::var>{});
        alien::SessionProjectData older;
        requireOk(alien::parseSessionProject(juce::JSON::toString(old), older, error), error, "a version 12 project opens");
        require(older.returns == alien::defaultSessionReturns() && older.returnPlugins == alien::ReturnRacks{}, "a version 12 project opens with returns A-D");
    }

    auto damage = [&](const char* what, auto&& mutate) {
        auto copy = juce::JSON::parse(json);
        mutate(copy.getDynamicObject());
        alien::SessionProjectData out; juce::String why;
        if (alien::parseSessionProject(juce::JSON::toString(copy), out, why)) { std::cerr << "accepted: " << what << '\n'; require(false, "damaged returns must be refused"); }
        require(why.isNotEmpty(), "and a reason is given");
    };
    auto sendsOf = [](juce::DynamicObject* root, int t) { return root->getProperty("trackSettings").getArray()->getReference(t).getDynamicObject()->getProperty("sends").getArray(); };
    auto returnAt = [](juce::DynamicObject* root, int i) { return root->getProperty("returns").getArray()->getReference(i).getDynamicObject(); };
    damage("a send to the removed return B", [&](auto* root) { sendsOf(root, 0)->getReference(1).getArray()->set(0, 0.5); });
    damage("a pre-fader switch on the removed return B", [&](auto* root) { sendsOf(root, 0)->getReference(1).getArray()->set(1, true); });
    damage("send automation for a removed return", [&](auto* root) {
        root->getProperty("returns").getArray()->removeLast();    // H goes, with track 1's send to it; its lane stays
        sendsOf(root, 1)->getReference(7).getArray()->set(0, 0.0); });
    damage("send automation for a never-used slot", [&](auto* root) {
        auto lanes = root->getProperty("timeline").getDynamicObject()->getProperty("automation");
        auto lane = juce::JSON::parse(juce::JSON::toString(lanes.getArray()->getReference(0)));
        lane.getDynamicObject()->setProperty("parameter", static_cast<int>(alien::automation::Parameter::sendG));
        lanes.getArray()->add(lane); });
    damage("four sends in a version 23 project", [&](auto* root) { sendsOf(root, 2)->removeRange(4, alien::maxReturns - 4); });
    damage("nine sends", [&](auto* root) { const auto first = sendsOf(root, 2)->getFirst(); sendsOf(root, 2)->add(first); });
    damage("two returns in one slot", [&](auto* root) { returnAt(root, 1)->setProperty("slot", 0); });
    damage("a slot past H", [&](auto* root) { returnAt(root, 1)->setProperty("slot", alien::maxReturns); });
    damage("a negative slot", [&](auto* root) { returnAt(root, 1)->setProperty("slot", -1); });
    damage("a text slot", [&](auto* root) { returnAt(root, 1)->setProperty("slot", "C"); });
    damage("a missing slot", [&](auto* root) { returnAt(root, 1)->removeProperty("slot"); });
    damage("an empty name", [&](auto* root) { returnAt(root, 1)->setProperty("name", "  "); });
    damage("a numeric name", [&](auto* root) { returnAt(root, 1)->setProperty("name", 3); });
    damage("nine returns", [&](auto* root) {
        auto* list = root->getProperty("returns").getArray();
        const auto first = list->getFirst(); for (int i = 0; i < 4; ++i) list->add(first); });
    damage("return gain 3", [&](auto* root) { returnAt(root, 0)->setProperty("gain", 3.0); });
    damage("an instrument on a return", [&](auto* root) {
        returnAt(root, 3)->setProperty("plugins", alien::rackToVar(alien::PluginRackState{{desc.createXml()->toString(), {}, false}, {}})); });
    // The rules hold for the in-memory form too.
    auto bad = project; bad.returnPlugins[1].effects.push_back({desc.createXml()->toString(), "YWJj", false});
    require(!bad.valid(), "an unused slot cannot hold FX");
    bad = project; bad.returns[6].name = "Ghost";
    require(!bad.valid(), "an unused slot has no name");
    bad = project; bad.returns[5].name = {};
    require(!bad.valid(), "a return in use has a name");
}

static void panels() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    alien::TrackMixerPanel panel(c, engine.tracks, engine.returns, engine.masterControls, engine.buses);
    panel.setSize(1600, 720);
    juce::StringArray routing; int returnEdits = 0; bool captured = false;
    panel.onRoutingChanged = [&](const juce::String& label) { routing.add(label); c.commit(label); };
    panel.onReturnEdited = [&](int r, bool) { c.returnSettingsEdited(r); ++returnEdits; };
    panel.beforeReturnDeleted = [&] { captured = true; };
    require(panel.returnStripCount() == 4 && panel.addReturnControl().isEnabled(), "a new project shows returns A-D and can add one");
    auto* strip = panel.trackStrip(0);
    require(strip != nullptr && strip->sendKnob(3).isVisible() && !strip->sendKnob(4).isVisible(), "a track shows a send knob for each return, and none for unused slots");
    panel.addReturnControl().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(panel.returnStripCount() == 5 && routing.size() == 1 && routing[0] == "Add return" && c.undoLabel() == "Add return", "Add return makes a strip and one undo step");
    require(panel.returnStrip(4) != nullptr && panel.returnStrip(4)->nameButton().getButtonText() == "Return E", "the new strip is return E");
    require(strip->sendKnob(4).isVisible() && strip->sendMode(4).getButtonText() == "E", "every track gets a send E knob");
    strip->sendKnob(4).setValue(50.0, juce::sendNotificationSync);
    require(approx(engine.tracks[0].send[4].load(), 0.5f), "which sets the send to E");
    require(panel.renameReturn(4, "Reverb") && panel.returnStrip(4)->nameButton().getButtonText() == "Reverb" && routing[routing.size() - 1] == "Rename return",
            "renaming a return renames its strip, as one undo step");
    require(strip->sendKnob(4).getTooltip().contains("Reverb"), "and the send knob's tooltip names it");
    juce::Slider* volume = nullptr;
    for (auto* child : panel.returnStrip(4)->getChildren()) if (child->getName() == "Return E volume") volume = dynamic_cast<juce::Slider*>(child);
    require(volume != nullptr, "the new strip has a volume fader");
    volume->setValue(50.0, juce::sendNotificationSync);
    require(approx(engine.returns[4].gain.load(), 0.5f) && returnEdits > 0 && c.document().returns[4].settings.gain == 0.5f, "its fader sets return E's level");

    require(panel.deleteReturn(1) && captured && panel.returnStrip(1) == nullptr && panel.returnStripCount() == 4 && routing[routing.size() - 1] == "Remove return",
            "removing return B removes its strip after the plugins are captured");
    require(!strip->sendKnob(1).isVisible() && strip->sendKnob(4).isVisible() && strip->sendKnob(4).getValue() == 50.0, "B's knob goes; E's stays with its level");
    for (int r = 1; r < alien::maxReturns; ++r) panel.addReturn();
    require(panel.returnStripCount() == alien::maxReturns && !panel.addReturnControl().isEnabled(), "Add return is disabled with eight returns");
    for (int r = 0; r < alien::maxReturns; ++r) {
        auto* ret = panel.returnStrip(r);
        require(ret != nullptr && ret->isVisible() && ret->getWidth() >= 60 && ret->getRight() <= panel.getWidth(), "every return strip is laid out");
        if (r > 0) require(panel.returnStrip(r - 1)->getRight() < ret->getX(), "side by side, in slot order");
    }
    require(strip->sendKnob(7).isVisible() && strip->sendKnob(7).getY() > strip->sendKnob(0).getY(), "eight send knobs take two rows");
    require(strip->sendKnob(7).getBottom() < strip->getHeight() - 200, "and leave room for the fader");
    // Undo outside the panel (the Edit menu): the strips follow the document.
    while (c.undoLabel() == "Add return") c.undo();
    require(c.undo() && c.undoLabel() == "Rename return", "undo the removal");
    panel.refreshSettings();
    require(panel.returnStripCount() == 5 && panel.returnStrip(1) != nullptr && strip->sendKnob(1).isVisible(), "an undone removal brings the strip and knob back");

    // Plugins offers the FX rack of each return that exists, by name.
    alien::PluginPanel plugins;
    juce::KnownPluginList none;
    std::array<juce::String, alien::maxReturns> names; names[0] = "Return A"; names[4] = "Reverb";
    plugins.refresh({"Track 1"}, {}, none, {}, names);
    juce::ComboBox* target = nullptr;
    for (auto* child : plugins.getChildren()) if (child->getName().startsWith("Plugin destination")) target = dynamic_cast<juce::ComboBox*>(child);
    require(target != nullptr, "the plugin panel has a destination chooser");
    juce::StringArray items; for (int i = 0; i < target->getNumItems(); ++i) items.add(target->getItemText(i));
    require(items.contains("Return A FX") && items.contains("Reverb FX (return E)") && !items.contains("Return B FX") && items.size() == 1 + 2 + 1,
            "it lists only the returns in use, renamed ones by name");
    target->setSelectedId(alien::PluginRuntime::returnIndex(4) + 1, juce::sendNotificationSync);
    plugins.refresh({"Track 1"}, {}, none, {}, names);
    require(plugins.destination() == alien::PluginRuntime::returnIndex(4), "choosing Reverb FX targets return E's rack");
    names[4] = {};
    plugins.refresh({"Track 1"}, {}, none, {}, names);
    require(plugins.destination() == alien::maxSessionTracks, "a removed return falls back to Master FX");

    // The automation editor offers a send lane for each return that exists.
    alien::TrackAutomationEditor automation(c);
    juce::ComboBox* parameter = nullptr;
    for (auto* child : automation.getChildren()) if (child->getName() == "Automation parameter") parameter = dynamic_cast<juce::ComboBox*>(child);
    require(parameter != nullptr, "the automation editor has a parameter chooser");
    auto texts = [&] { juce::StringArray t; for (int i = 0; i < parameter->getNumItems(); ++i) t.add(parameter->getItemText(i)); return t; };
    require(texts().contains("Send E (Reverb)") && texts().contains("Send B") && !texts().contains("Send F") && parameter->getNumItems() == 2 + 5,
            "volume, pan and one send per return, renamed ones with their name");
    parameter->setSelectedId(3 + 4, juce::dontSendNotification);
    require(c.deleteReturn(4), "remove return E");
    automation.refresh();
    require(!texts().contains("Send E (Reverb)") && parameter->getSelectedId() == 1, "a removed return's lane is no longer offered, and the choice falls back to Volume");
}

// A one-bar song where track 0 plays the built-in synth.
static std::unique_ptr<alien::ArrangementPlayback> songWithNotes(alien::SessionProjectData& project) {
    auto song = std::make_unique<alien::ArrangementPlayback>(); song->timeline.setLengthBars(1);
    alien::PianoRollPattern notes; notes.loopSteps = 16; notes.add(60, 0, 16, 1);
    alien::timeline::Region region; region.row = 0; region.source = 0;
    require(song->timeline.place(region), "song region");
    song->clips[0][0].pattern = notes;
    project.timeline = song->timeline;
    return song;
}

static void stems() {
    {   // Capture: a return beyond D lines up with the dry track behind a latent return.
        Rig rig;
        rig.ret(6).active.store(true);
        rig.returnEffect(6, 1.0f, 3);
        rig.track().send[6].store(0.5f); rig.track().send[0].store(0.5f);
        rig.settled();
        rig.engine->stemCapture = std::make_unique<alien::StemCapture>();
        rig.engine->stemCapture->prepare();
        rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
        std::array<float, 512> l{}, r{};
        float* outputs[]{l.data(), r.data()};
        const auto before = allocations;
        rig.engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        require(allocations == before, "capturing return stems allocates nothing on the audio callback");
        const auto& buses = rig.engine->stemCapture->buses;
        for (int i = 0; i < 512; ++i) {
            float sum = 0; for (const auto& bus : buses) sum += bus.getSample(0, i);
            require(approx(sum, l[static_cast<std::size_t>(i)]), "track, return and rack stems reconstruct the pre-master mix");
        }
        const auto first = [&](int bus) { int i = 0; while (i < 512 && std::abs(buses[static_cast<std::size_t>(bus)].getSample(0, i)) < 1e-7f) ++i; return i; };
        require(first(0) == 3 && first(alien::StemCapture::returnBase) == 3 && first(alien::StemCapture::returnBase + 6) == 3,
                "the dry track, return A and the latent return G stems line up");
        require(first(alien::StemCapture::returnBase + 5) == 512, "an unused slot captures nothing");
    }
    {   // Files: one per return that exists, named by slot (and by name once renamed).
        auto project = std::make_unique<alien::SessionProjectData>();
        project->returns[1] = {};
        project->returns[5] = {true, "Long Delay", {}};
        project->trackSettings[0].send[5] = 1.0f;
        auto song = songWithNotes(*project);
        require(project->valid(), "stem fixture");
        const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienReturnStems", "", false);
        alien::WavExportOptions options; options.stems = true;
        alien::DrumSamplePointers drums{}; std::atomic<bool> stop{false}; juce::String error;
        requireOk(alien::exportArrangementWav(folder, *project, *song, drums, stop, error, nullptr, options), error, "stem export");
        require(folder.getChildFile("Return A.wav").existsAsFile() && folder.getChildFile("Return C.wav").existsAsFile()
                && folder.getChildFile("Return D.wav").existsAsFile() && folder.getChildFile("Return F - Long Delay.wav").existsAsFile(),
                "a stem for each return that exists");
        require(!folder.getChildFile("Return B.wav").existsAsFile() && !folder.getChildFile("Return E.wav").existsAsFile(), "and none for unused slots");
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        auto peak = [&](const juce::String& name) {
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(folder.getChildFile(name)));
            require(reader != nullptr, "read a stem");
            juce::AudioBuffer<float> data(2, static_cast<int>(reader->lengthInSamples));
            reader->read(&data, 0, data.getNumSamples(), 0, true, true);
            // After the first 50 ms: a send glides up from off over its first few milliseconds.
            const int from = std::min(4800, data.getNumSamples());
            return data.getMagnitude(from, data.getNumSamples() - from);
        };
        const float returnPeak = peak("Return F - Long Delay.wav"), trackPeak = peak("01 - Track.wav");
        require(returnPeak > 0.01f && std::abs(returnPeak - trackPeak) < 1.0e-3f, "return F's file holds what track 1 sends to it");
        require(peak("Return A.wav") == 0.0f, "and return A's file is silent");
        folder.deleteRecursively();
    }
}

static void freezeIgnoresReturns() {
    auto project = std::make_unique<alien::SessionProjectData>();
    project->returns[6] = {true, "Wash", {}};
    project->trackSettings[0].send[6] = 1.0f;
    auto song = songWithNotes(*project);
    auto renderer = std::make_unique<ToneEngine>(); auto& rack = renderer->plugins->racks[0];
    rack.instrumentAssigned = true; rack.instrument.processor = std::make_unique<TestProcessor>(true);
    auto& returnRack = renderer->plugins->racks[static_cast<std::size_t>(alien::PluginRuntime::returnIndex(6))];
    returnRack.effectCount = 1; returnRack.effects[0].processor = std::make_unique<TestProcessor>(false, 4.0f, 3);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienReturnFreeze", ".wav");
    alien::WavExportOptions options; options.freezeTrack = 0; options.toTick = alien::timeline::ticksPerBar;
    alien::DrumSamplePointers drums{}; std::atomic<bool> stop{false}; juce::String error;
    requireOk(alien::exportArrangementWav(file, *project, *song, drums, stop, error, renderer.get(), options), error, "freeze");
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    juce::AudioBuffer<float> probe(2, 512); require(reader && reader->read(&probe, 0, 512, 512, true, true), "read frozen audio");
    require(approx(probe.getSample(0, 200), 0.2f), "freezing a track that sends to return G renders the track alone");
    require(!returnRack.frozen, "export restores the return rack afterwards");
    reader.reset(); file.deleteFile();
}

// What a return slot costs: the callback with 16 tracks sending to every return in use, for 0,
// 4 and 8 returns, and the memory each slot holds. Printed for the checklist; not asserted.
static void measureCost() {
    Rig rig;
    for (int t = 0; t < alien::maxSessionTracks; ++t)
        for (int r = 0; r < alien::maxReturns; ++r) rig.track(t).send[static_cast<std::size_t>(r)].store(0.2f);
    auto timeBlocks = [&](int returns) {
        for (int r = 0; r < alien::maxReturns; ++r) rig.ret(r).active.store(r < returns);
        std::array<float, 512> l{}, r{};
        float* outputs[]{l.data(), r.data()};
        for (int i = 0; i < 50; ++i) rig.engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        double best = 1.0e9;
        for (int round = 0; round < 5; ++round) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 200; ++i) rig.engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
            best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 200.0);
        }
        return best;
    };
    const double none = timeBlocks(0), four = timeBlocks(4), eight = timeBlocks(8);
    const double perSlotMb = 2.0 * 2.0 * (alien::PluginRuntime::maxDelay + 1) * sizeof(float) / (1024.0 * 1024.0);
    std::cout << "cost: 512-sample callback, 16 tracks sending to every return: " << none << " us with no returns, "
              << four << " us with 4, " << eight << " us with 8 (" << (eight - four) / 4.0 << " us a return); "
              << perSlotMb << " MB of delay lines a slot\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    returnsBeyondD();
    delayAlignment();
    slotComesBackClean();
    realTimeSafety();
    controllerAndUndo();
    projectFormat();
    panels();
    stems();
    freezeIgnoresReturns();
    measureCost();
    std::cout << "PASS: flexible returns (beyond D, alignment, clean slots, no allocation, undo, format, panels, stems, freeze)" << std::endl;
    return 0;
}
