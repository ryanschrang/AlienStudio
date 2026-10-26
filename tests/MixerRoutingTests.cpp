// Return tracks and sends: pre/post-fader sends, mute and solo, return level/pan/mute and FX,
// delay compensation through the returns, no allocation while mixing, the project format,
// undo, and the mixer and plugin panels.
#include "ToneEngine.h"
#include "WavExport.h"
#include <limits>
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
        ++processCalls;
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
    int processCalls{};
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
    require(juce::JSON::parse(json).getProperty("version", 0) == juce::var(alien::sessionProjectFormatVersion), "projects are written using the current format version");
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
    alien::TrackMixerPanel panel(c, engine.tracks, engine.returns, engine.masterControls, engine.buses);
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

static void stemCaptureTiming() {
    Rig rig; rig.returnEffect(0,1.0f,4);
    rig.track().send[0].store(0.5f); rig.track().send[1].store(0.5f);
    rig.engine->stemCapture=std::make_unique<alien::StemCapture>();
    rig.engine->stemCapture->prepare();
    float* outputs[]{rig.left.data(),rig.right.data()};
    const auto before=allocations;
    rig.engine->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,512,{});
    require(allocations == before, "capturing stems allocates nothing on audio callback");
    const auto& buses=rig.engine->stemCapture->buses;
    for (int i=0;i<512;++i) {
        float sum=0; for (const auto& bus : buses) sum+=bus.getSample(0,i);
        require(approx(sum,rig.left[static_cast<std::size_t>(i)]), "captured buses reconstruct the pre-master mix");
    }
    const auto first=[&](int bus) { int i=0; while(i<512 && std::abs(buses[static_cast<std::size_t>(bus)].getSample(0,i))<1e-7f) ++i; return i; };
    require(first(0)==4 && first(alien::StemCapture::returnBase)==4 && first(alien::StemCapture::returnBase+1)==4,
        "dry track, delayed return and immediate return stems align");
}

static void stemPluginFailure() {
    auto project=std::make_unique<alien::SessionProjectData>();
    auto song=std::make_unique<alien::ArrangementPlayback>(); song->timeline.setLengthBars(1);
    alien::timeline::Region region; region.row=0; region.source=0;
    require(song->timeline.place(region), "failure fixture region"); project->timeline=song->timeline;
    auto engine=std::make_unique<ToneEngine>();
    auto& master=engine->plugins->racks[alien::PluginRuntime::masterIndex];
    master.effectCount=1; master.effects[0].processor=std::make_unique<TestProcessor>(false,0.5f);
    auto& bad=engine->plugins->racks[alien::PluginRuntime::returnIndex(0)];
    bad.effectCount=1; bad.effects[0].processor=std::make_unique<TestProcessor>(false,std::numeric_limits<float>::quiet_NaN());
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienFaultStems","",false);
    alien::WavExportOptions options; options.stems=true;
    alien::DrumSamplePointers drums{}; std::atomic<bool> stop{false}; juce::String error;
    require(!alien::exportArrangementWav(folder,*project,*song,drums,stop,error,engine.get(),options)
        && error.containsIgnoreCase("plugin failed") && !folder.exists(), "plugin failure never publishes partial stems");
    require(!master.effects[0].bypass, "export restores the supplied engine's master bypass state");
}

static void freezeWorkflow() {
    auto engine=std::make_unique<ToneEngine>();engine->prepare(48000);
    alien::ProjectController c(*engine);c.pushDocumentToEngine();
    alien::PianoRollPattern notes;notes.loopSteps=16;notes.add(60,0,16,1);
    c.setClipPattern(0,0,notes,false);c.commit("Notes");
    require(c.editTimeline("Length",[](alien::timeline::Timeline& t){return t.setLengthBars(1);}),"short freeze song");
    require(c.placeRegion(0,0,0),"freeze source region");
    auto project = alien::toProjectData(c.document());
    project.plugins[0].effects.push_back({"<PLUGIN name=\"Original FX\"/>", {}, false});
    const auto instance = project.plugins[0].effects[0].instanceId;
    c.setPluginRacks(project.plugins, project.masterPlugins);
    require(c.editTimeline("Parameter automation", [=](alien::timeline::Timeline& t) {
        t.parameterLanes.push_back({{instance, "cutoff"}, true, {{0, 0.25f}, {960, 0.75f}}}); return true;
    }), "parameter lane added before freeze");
    const auto original=c.timeline();
    auto cached=std::make_shared<alien::SessionAudioClip>();cached->sampleRate=48000;cached->channels=1;cached->frames=144000;
    cached->left.assign(cached->frames,0.4f);cached->buildWaveform();
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienFreezeWorkflow",".wav");
    {auto writer=alien::openExportWriter(file,true);juce::AudioBuffer<float> data(2,144000);data.clear();
     require(writer && writer->writeFromAudioSampleBuffer(data,0,144000) && writer->flush(),"freeze fixture WAV");}
    require(c.installFrozenTrack(0,file,cached,original) && c.isTrackFrozen(0),"install freezes one track");
    require(c.undoLabel()=="Freeze track" && c.timeline().frozenOriginals.size()==1 && c.timeline().lengthBars==2,"freeze archives source and extends for tail");
    const auto frozen=c.timeline();
    require(frozen.parameterLanes == original.parameterLanes
        && c.document().tracks[0].plugins.effects[0].instanceId == instance,
        "freezing retains saved parameter target and original plugin identity");
    require(!c.editTimeline("Delete cache",[](alien::timeline::Timeline& t){t.regions.clear();return true;}),"frozen cache is read-only");
    require(!c.removeAudio(0,c.document().tracks[0].settings.freezeScene) && !c.deleteScene(0),"original and cache source cells protected");
    alien::PianoRollPattern changed;changed.add(80,0,1);c.setClipPattern(0,0,changed,false);
    require(c.document().clip(0,0).notes().notes[0].pitch==60,"original MIDI is read-only while frozen");
    require(c.undo() && !c.isTrackFrozen(0) && c.timeline()==original && file.existsAsFile(),"undo freeze restores original without deleting file");
    require(c.redo() && c.isTrackFrozen(0) && c.timeline()==frozen,"redo freeze restores cache");
    require(c.moveScene(0,1),"reorder scenes with frozen cache");c.commit("Move scene");
    require(c.document().tracks[0].settings.freezeScene==0 && engine->tracks[0].freezeScene.load()==0
        && alien::toProjectData(c.document()).valid(),"frozen cache references follow reordered scenes");
    require(c.undo() && c.timeline()==frozen,"scene reorder undo restores frozen references");

    require(c.unfreezeTrack(0) && !c.isTrackFrozen(0) && c.timeline().lengthBars==1
        && c.timeline().find(original.regions.front().id)!=nullptr && c.timeline().frozenOriginals.empty(),"unfreeze restores originals and song length");
    require(c.timeline().parameterLanes == original.parameterLanes
        && c.document().tracks[0].plugins.effects[0].instanceId == instance,
        "unfreeze restores the same plugin automation target");
    require(c.undo() && c.isTrackFrozen(0),"unfreeze is undoable");
    c.setBpm(130);c.commit("Tempo");
    require(!c.anyFrozenTrack() && c.document().bpm==130 && c.timeline().frozenOriginals.empty(),"tempo change safely unfreezes");
    require(c.undo() && c.isTrackFrozen(0) && c.document().bpm==120,"tempo undo restores freeze state");
    require(alien::toProjectData(c.document()).valid(),"frozen controller state remains serializable");
    file.deleteFile();
}
static void freezeAudioAndSuspension() {
    auto project=std::make_unique<alien::SessionProjectData>();
    auto song=std::make_unique<alien::ArrangementPlayback>();song->timeline.setLengthBars(1);
    alien::timeline::Region region;region.row=0;region.source=0;
    require(song->timeline.place(region),"freeze render region");project->timeline=song->timeline;
    alien::PianoRollPattern notes;notes.loopSteps=16;notes.add(60,0,16,1);song->clips[0][0].pattern=notes;
    project->trackSettings[0].gain=0.1f;project->trackSettings[0].pan=0.5f;
    auto renderer=std::make_unique<ToneEngine>();auto& rack=renderer->plugins->racks[0];
    rack.instrumentAssigned=true;rack.instrument.processor=std::make_unique<TestProcessor>(true);
    rack.effectCount=1;rack.effects[0].processor=std::make_unique<TestProcessor>(false,12.0f,4);
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienFreezeAudio",".wav");
    alien::WavExportOptions options;options.freezeTrack=0;options.toTick=alien::timeline::ticksPerBar;
    alien::DrumSamplePointers drums{};std::atomic<bool> stop{false};juce::String error;
    require(alien::exportArrangementWav(file,*project,*song,drums,stop,error,renderer.get(),options),error.toRawUTF8());
    juce::AudioFormatManager formats;formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    require(reader && reader->usesFloatingPointData && reader->bitsPerSample==32,"freeze writes float32 headroom");
    juce::AudioBuffer<float> probe(2,512);require(reader->read(&probe,0,512,512,true,true),"read frozen audio");
    require(approx(probe.getSample(0,200),2.4f) && approx(probe.getSample(1,200),2.4f),"cache includes FX but ignores fader/pan and preserves values above full scale");
    reader.reset();auto imported=alien::importSessionAudioClip(file);require(imported.clip!=nullptr,"import frozen WAV");
    auto live=std::make_unique<ToneEngine>();live->prepare(48000);alien::ProjectController c(*live);c.pushDocumentToEngine();
    c.setClipPattern(0,0,notes,false);c.commit("Notes");require(c.placeRegion(0,0,0),"place original notes");
    auto& liveRack=live->plugins->racks[0];liveRack.instrumentAssigned=true;
    auto instrument=std::make_unique<TestProcessor>(true);auto* instrumentRaw=instrument.get();liveRack.instrument.processor=std::move(instrument);
    auto effect=std::make_unique<TestProcessor>(false,12.0f,4);auto* effectRaw=effect.get();liveRack.effects[0].processor=std::move(effect);liveRack.effectCount=1;
    live->plugins->prepare(48000);
    live->tracks[0].gain.store(0.1f);live->tracks[0].pan.store(0.5f);c.trackSettingsEdited(0);c.commit("Mix");
    const auto expected=c.timeline();require(c.installFrozenTrack(0,file,imported.clip,expected),"install rendered cache");
    live->patternMode.store(2);live->timelineSeek.store(0);live->playing.store(true);
    std::array<float,512> l{},right{};float* outputs[]{l.data(),right.data()};
    for(int block=0;block<24;++block)live->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,512,{});
    require(instrumentRaw->processCalls==0 && effectRaw->processCalls==0,"frozen track never processes original instrument/effects");
    require(approx(l[400],0.12f) && approx(right[400],0.24f),"cache gain compensation preserves audible track level");
    const auto before=allocations;live->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,512,{});
    require(before==allocations,"frozen playback allocates nothing in callback");
    require(c.editTimeline("Frozen automation",[](alien::timeline::Timeline& t){alien::automation::Lane lane{0,alien::automation::Parameter::volume};lane.put({0,0.2f});t.automationLanes.push_back(lane);return true;}),"automation remains editable");
    live->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,512,{});
    require(approx(l[400],0.24f) && approx(right[400],0.48f),"automation applies once after frozen cache");
    require(c.unfreezeTrack(0),"restore original DSP");live->timelineSeek.store(0);
    live->audioDeviceIOCallbackWithContext(nullptr,0,outputs,2,512,{});
    require(instrumentRaw->processCalls>0 && effectRaw->processCalls>0,"unfreeze resumes original processing");
    imported.clip.reset();file.deleteFile();
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    freezeWorkflow();
    freezeAudioAndSuspension();
    stemPluginFailure();
    stemCaptureTiming();
    sendLevels();
    returnStrips();
    delayCompensation();
    realTimeSafety();
    projectFormat();
    undoAndPanels();
    std::cout << "PASS: sends (pre/post, mute, solo), return strips and FX, return delay compensation, no allocation, format, undo, panels\n";
    return 0;
}
