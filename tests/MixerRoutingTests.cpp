// Return tracks and sends: pre/post-fader sends, mute and solo, return level/pan/mute and FX,
// delay compensation through the returns, no allocation while mixing, the project format,
// undo, and the mixer and plugin panels.
#include "ToneEngine.h"
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

// Plays a steady 0.2 while a note is held (an instrument), or scales and optionally delays what it is given (an effect).
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
    const juce::String getName() const override { return "Original routing test processor"; }
    bool acceptsMidi() const override { return synth; } bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; } void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; } juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {} void setStateInformation(const void*, int) override {}
    bool synth{}, active{}; float factor; int delay{}, position{}; std::array<float, 32> ring{};
};

// An engine whose track 0 holds a note on a test instrument for the whole run.
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
    // Lets every 5 ms glide settle and returns the last sample.
    std::pair<float, float> settled() { render(12); return {left[1023], right[1023]}; }
    alien::TrackControls& track(int t = 0) { return engine->tracks[static_cast<std::size_t>(t)]; }
    alien::ReturnControls& ret(int r = 0) { return engine->returns[static_cast<std::size_t>(r)]; }
};

static void sendLevels() {
    {
        Rig rig;
        require(approx(rig.settled().first, 0.2f), "with every send off the track plays alone");
        rig.track().send[0].store(1.0f);
        auto [l, r] = rig.settled();
        require(approx(l, 0.4f) && approx(r, 0.4f), "a post-fader send at 0 dB doubles the track through return A");
        require(rig.ret(0).peak.load() > 0.19f && rig.ret(1).peak.load() == 0.0f, "return A's meter shows the send; return B's stays dark");
        rig.track().send[0].store(0.5f);
        require(approx(rig.settled().first, 0.3f), "a send's level scales what reaches the return");
        rig.track().send[2].store(0.5f);
        require(approx(rig.settled().first, 0.4f), "two sends feed two returns");
    }
    {   // Pre- and post-fader.
        Rig rig;
        rig.track().send[0].store(1.0f); rig.track().gain.store(0.0f);
        require(approx(rig.settled().first, 0.0f), "a post-fader send follows the fader down to silence");
        rig.track().sendPre[0].store(true);
        require(approx(rig.settled().first, 0.2f), "a pre-fader send ignores the fader");
        rig.track().pan.store(-1.0f);
        auto [l, r] = rig.settled();
        require(approx(l, 0.2f) && approx(r, 0.2f), "and ignores the track's pan");
        rig.track().mute.store(true);
        require(approx(rig.settled().first, 0.0f), "but a muted track sends nothing, even pre-fader");
    }
    {   // Solo: another track's solo silences this one and its sends; a return is never soloed away.
        Rig rig;
        rig.track().send[0].store(1.0f); rig.track().sendPre[1].store(true); rig.track().send[1].store(1.0f);
        rig.track(1).solo.store(true);
        require(approx(rig.settled().first, 0.0f), "soloing another track silences this track and both its sends");
        rig.track(1).solo.store(false); rig.track().solo.store(true);
        require(approx(rig.settled().first, 0.6f), "a soloed track is still heard through its returns");
    }
}

static void returnStrips() {
    Rig rig;
    rig.track().send[0].store(1.0f);
    rig.ret(0).gain.store(0.5f);
    require(approx(rig.settled().first, 0.3f), "the return's fader sets its level");
    rig.ret(0).gain.store(1.0f); rig.ret(0).pan.store(-1.0f);
    auto [l, r] = rig.settled();
    require(approx(l, 0.4f) && approx(r, 0.2f), "the return's pan uses the track balance law");
    rig.ret(0).mute.store(true);
    require(approx(rig.settled().first, 0.2f), "a muted return passes nothing on");
    rig.ret(0).mute.store(false); rig.ret(0).pan.store(0.0f);
    rig.returnEffect(0, 2.0f);
    require(approx(rig.settled().first, 0.6f), "the return's FX process what is sent to it");
    // Glide: a send that jumps up lands over a few milliseconds, not in one sample.
    rig.track().send[1].store(0.0f); rig.settled();
    rig.track().send[1].store(1.0f);
    rig.render();
    require(rig.left[0] > 0.6f && rig.left[0] < 0.61f && rig.left[100] < rig.left[1023] && rig.left[1023] > 0.79f,
            "turning a send up glides instead of clicking");
    require(approx(rig.settled().first, 0.8f), "and then settles at the new level");
}

static void delayCompensation() {
    Rig rig;
    rig.returnEffect(0, 1.0f, 4);   // return A's FX delay everything by 4 samples
    require(rig.engine->plugins->returnLatency() == 4 && rig.engine->plugins->latencySamples() == 4, "the slowest return chain adds to the reported latency");
    rig.track().send[0].store(1.0f); rig.track().send[1].store(1.0f);
    rig.settled();   // let the sends glide up to their level
    // Restart so the first hit can be found: dry, return A (4 samples late) and return B must all arrive together.
    rig.engine->playing.store(false); rig.render(); rig.engine->playing.store(true);
    std::vector<float> out;
    for (int b = 0; b < 4; ++b) { rig.render(); out.insert(out.end(), rig.left.begin(), rig.left.end()); }
    std::size_t first = 0; while (first < out.size() && std::abs(out[first]) < 1.0e-6f) ++first;
    require(first < out.size() && approx(out[first], 0.6f), "dry, delayed return A and undelayed return B arrive on the same sample");
    require(first == 4, "the first hit is late by exactly the reported latency");
    require(approx(out[first + 100], 0.6f), "and stay together");
}

static void realTimeSafety() {
    Rig rig;
    rig.returnEffect(0, 1.0f, 2);
    for (int r = 0; r < alien::maxReturns; ++r) { rig.track().send[static_cast<std::size_t>(r)].store(0.3f); rig.track().sendPre[static_cast<std::size_t>(r)].store(r % 2 == 0); }
    rig.render(4);
    const auto before = allocations;
    rig.render(8);
    rig.track().send[0].store(0.9f); rig.ret(1).pan.store(0.5f); rig.ret(2).mute.store(true);
    rig.render(8);
    require(allocations == before, "mixing sends and returns allocates nothing");
}

static void projectFormat() {
    alien::SessionProjectData project;
    project.trackSettings[1].send = {0.25f, 0.0f, 1.0f, 0.5f};
    project.trackSettings[1].sendPre = {true, false, false, true};
    project.returns[2] = {0.5f, -0.25f, true};
    juce::PluginDescription desc; desc.name = "Missing reverb"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "/missing/reverb.vst3";
    project.returnPlugins[2].effects.push_back({desc.createXml()->toString(), "YWJj", false});
    require(project.valid(), "a project with sends and returns is valid");
    const auto json = alien::serializeSessionProject(project);
    alien::SessionProjectData back; juce::String error;
    require(alien::parseSessionProject(json, back, error), "and parses");
    require(back.trackSettings[1].send == project.trackSettings[1].send && back.trackSettings[1].sendPre == project.trackSettings[1].sendPre
            && back.returns == project.returns && back.returnPlugins == project.returnPlugins, "sends, return strips and return FX round-trip");
    require(juce::JSON::parse(json).getProperty("version", 0) == juce::var(13), "projects are written as version 13");
    {   // Version 12 and older: no returns and no sends.
        auto old = juce::JSON::parse(json);
        old.getDynamicObject()->setProperty("version", 12);
        old.getDynamicObject()->removeProperty("returns");
        for (auto& t : *old.getDynamicObject()->getProperty("trackSettings").getArray()) t.getDynamicObject()->removeProperty("sends");
        alien::SessionProjectData older;
        require(alien::parseSessionProject(juce::JSON::toString(old), older, error), "a version 12 project opens");
        bool off = true;
        for (const auto& t : older.trackSettings) for (std::size_t r = 0; r < t.send.size(); ++r) off = off && t.send[r] == 0.0f && !t.sendPre[r];
        require(off && older.returns == std::array<alien::ReturnSettings, alien::maxReturns>{} && older.returnPlugins == alien::ReturnRacks{},
                "with every send off and the returns at unity with no FX");
    }
    auto damage = [&](const char* what, auto&& mutate) {
        auto copy = juce::JSON::parse(json);
        mutate(copy.getDynamicObject());
        alien::SessionProjectData out; juce::String why;
        if (alien::parseSessionProject(juce::JSON::toString(copy), out, why)) { std::cerr << "accepted: " << what << '\n'; require(false, "damaged routing must be refused"); }
        require(why.isNotEmpty(), "and a reason is given");
    };
    auto sendsOf = [](juce::DynamicObject* root) { return root->getProperty("trackSettings").getArray()->getReference(1).getDynamicObject()->getProperty("sends").getArray(); };
    auto returnOf = [](juce::DynamicObject* root, int r) { return root->getProperty("returns").getArray()->getReference(r).getDynamicObject(); };
    damage("send above 0 dB", [&](auto* root) { sendsOf(root)->getReference(0).getArray()->set(0, 1.5); });
    damage("negative send", [&](auto* root) { sendsOf(root)->getReference(0).getArray()->set(0, -0.1); });
    damage("text pre flag", [&](auto* root) { sendsOf(root)->getReference(0).getArray()->set(1, "pre"); });
    damage("three sends", [&](auto* root) { sendsOf(root)->remove(3); });
    damage("three returns", [&](auto* root) { root->getProperty("returns").getArray()->remove(3); });
    damage("return gain 3", [&](auto* root) { returnOf(root, 0)->setProperty("gain", 3.0); });
    damage("return pan 2", [&](auto* root) { returnOf(root, 0)->setProperty("pan", 2.0); });
    damage("numeric return mute", [&](auto* root) { returnOf(root, 0)->setProperty("mute", 1); });
    damage("an instrument on a return", [&](auto* root) {
        returnOf(root, 1)->setProperty("plugins", alien::rackToVar(alien::PluginRackState{{desc.createXml()->toString(), {}, false}, {}})); });
}

static void undoAndPanels() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    // Sends and return strips are part of the document, of undo and of the engine.
    engine.tracks[0].send[1].store(0.7f); engine.tracks[0].sendPre[1].store(true);
    c.trackSettingsEdited(0); c.commit("Send");
    engine.returns[3].gain.store(0.25f); engine.returns[3].mute.store(true);
    c.returnSettingsEdited(3); c.commit("Return");
    require(c.document().tracks[0].settings.send[1] == 0.7f && c.document().tracks[0].settings.sendPre[1]
            && c.document().returns[3].settings == (alien::ReturnSettings{0.25f, 0.0f, true}), "mixer edits reach the document");
    require(c.undo() && c.document().returns[3].settings == alien::ReturnSettings{} && engine.returns[3].gain.load() == 1.0f && !engine.returns[3].mute.load(),
            "undo restores the return strip, in the document and the engine");
    require(c.undo() && c.document().tracks[0].settings.send[1] == 0.0f && engine.tracks[0].send[1].load() == 0.0f, "and then the send");
    require(c.redo() && c.redo() && engine.returns[3].gain.load() == 0.25f && engine.tracks[0].sendPre[1].load(), "redo brings both back");
    engine.returns[3].gain.store(3.0f); c.returnSettingsEdited(3);
    require(c.document().returns[3].settings.gain == 0.25f, "an out-of-range return level never reaches the document");
    engine.returns[3].gain.store(0.25f);

    // The mixer: send knobs and pre switches on each track strip, and four return strips.
    alien::TrackMixerPanel panel(c, engine.tracks, engine.returns, engine.masterControls);
    panel.setSize(1400, 640);
    int trackEdits = 0, returnEdits = 0, lastReturn = -1;
    panel.onTrackEdited = [&](int, bool) { ++trackEdits; };
    panel.onReturnEdited = [&](int r, bool) { ++returnEdits; lastReturn = r; };
    auto* strip = panel.trackStrip(0);
    require(strip != nullptr && strip->sendKnob(1).getValue() == 70.0 && strip->sendMode(1).getToggleState(), "a strip shows its sends and pre switches");
    strip->sendKnob(2).setValue(50.0, juce::sendNotificationSync);
    require(approx(engine.tracks[0].send[2].load(), 0.5f) && trackEdits > 0, "turning a send knob sets the send");
    strip->sendMode(2).triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(engine.tracks[0].sendPre[2].load(), "its switch makes it pre-fader");
    require(strip->sendKnob(0).getTooltip().contains("off") && strip->sendKnob(2).getTooltip().contains("pre-fader"), "the knob's tooltip says its level and mode");
    auto& returnB = panel.returnStrip(1);
    juce::Slider* volume = nullptr;
    for (auto* child : returnB.getChildren()) if (child->getName() == "Return B volume") volume = dynamic_cast<juce::Slider*>(child);
    require(volume != nullptr, "a return strip has a volume fader");
    volume->setValue(50.0, juce::sendNotificationSync);
    require(approx(engine.returns[1].gain.load(), 0.5f) && returnEdits > 0 && lastReturn == 1, "moving it sets that return's level");
    for (int r = 0; r < alien::maxReturns; ++r) {
        const auto b = panel.returnStrip(r).getBounds();
        require(panel.returnStrip(r).isVisible() && b.getWidth() > 60 && b.getRight() <= panel.getWidth(), "every return strip is laid out");
        if (r > 0) require(panel.returnStrip(r - 1).getRight() < b.getX(), "side by side, in order");
    }
    panel.refreshSettings();
    require(strip->sendKnob(1).getValue() == 70.0, "refreshing keeps what the engine holds");

    // The plugin panel offers each return's FX rack, without an instrument slot.
    alien::PluginPanel plugins;
    juce::KnownPluginList none;
    plugins.refresh({"Track 1"}, {}, none);
    juce::ComboBox* target = nullptr;
    for (auto* child : plugins.getChildren()) if (child->getName().startsWith("Plugin destination")) target = dynamic_cast<juce::ComboBox*>(child);
    require(target != nullptr, "the plugin panel has a destination chooser");
    juce::StringArray items; for (int i = 0; i < target->getNumItems(); ++i) items.add(target->getItemText(i));
    require(items.contains("Return A FX") && items.contains("Return D FX") && items.contains("Master FX"), "it lists the returns' FX racks");
    target->setSelectedId(alien::PluginRuntime::returnIndex(1) + 1, juce::sendNotificationSync);
    plugins.refresh({"Track 1"}, {}, none);
    require(plugins.destination() == alien::PluginRuntime::returnIndex(1), "choosing Return B FX targets return B's rack");

    // The plugin host keeps a return's saved FX even when the plugin is missing.
    auto runtime = std::make_unique<alien::PluginRuntime>();
    alien::PluginHost host(*runtime);
    juce::PluginDescription desc; desc.name = "Missing delay"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "/missing/delay.vst3";
    std::vector<alien::PluginRackState> tracks(4); alien::PluginRackState master; alien::ReturnRacks returns;
    returns[2].effects.push_back({desc.createXml()->toString(), "YWJj", true});
    const auto saved = returns;
    const auto errors = host.restore(tracks, master, returns, 48000);
    require(errors.isNotEmpty() && runtime->racks[static_cast<std::size_t>(alien::PluginRuntime::returnIndex(2))].effectCount == 1, "a missing return FX is reported and its slot kept");
    host.snapshot(tracks, master, returns);
    require(returns == saved, "and its saved state survives a snapshot");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    sendLevels();
    returnStrips();
    delayCompensation();
    realTimeSafety();
    projectFormat();
    undoAndPanels();
    std::cout << "PASS: sends (pre/post, mute, solo), return strips and FX, return delay compensation, no allocation, format, undo, panels\n";
    return 0;
}
