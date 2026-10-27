// Sidechain inputs (MIX-04). Part 1: the stock compressor keyed by another track's signal - the
// detector, the compressor's sidechain bus, the runtime handing a key to a keyed slot without
// allocating, the host switching the bus on and off, saving, track moves and deletes, and the
// Plugins panel's choice of key. Part 2: the stock gate, keys from a track after its fader, from a
// bus and from a return, which racks may use which keys, format 25, and bus/return removal.
#include "PluginHost.h"
#include "PluginPanel.h"
#include "ProjectController.h"
#include "StockEffects.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }

static alien::PluginState compressorState(int sidechain = -1, alien::KeySource from = alien::KeySource::track, std::size_t effect = 1) {
    alien::PluginState state(alien::stock::describe(alien::stock::catalog[effect]).createXml()->toString(), {});
    state.sidechain = sidechain;
    state.sidechainFrom = sidechain >= 0 ? from : alien::KeySource::track;
    return state;
}
static constexpr std::size_t gateIndex = 7;
static float peak(const float* data, int count) { float p = 0; for (int i = 0; i < count; ++i) p = std::max(p, std::abs(data[i])); return p; }

static void detectorTests() {
    alien::dsp::Compressor compressor;
    compressor.prepare(48000);
    alien::dsp::Compressor::Settings settings;   // -18 dB threshold, 4:1
    std::vector<float> left(4800, 0.05f), right(4800, 0.05f), loud(4800, 0.9f), silent(4800, 0.0f);
    auto quiet = left; auto quietRight = right;
    compressor.process(quiet.data(), quietRight.data(), 4800, settings);
    require(std::abs(quiet.back() - 0.05f) < 1.0e-4f, "a quiet signal on its own is not compressed");
    compressor.reset(); quiet = left; quietRight = right;
    compressor.process(quiet.data(), quietRight.data(), 4800, settings, loud.data(), loud.data());
    require(quiet.back() < 0.05f * 0.3f, "a loud key pulls the quiet signal down");
    compressor.reset(); auto loudCopy = loud, loudRight = loud;
    compressor.process(loudCopy.data(), loudRight.data(), 4800, settings, silent.data(), silent.data());
    require(std::abs(loudCopy.back() - 0.9f) < 1.0e-4f, "a silent key leaves even a loud signal alone");
    std::vector<float> broken(4800, std::nanf(""));
    compressor.reset(); quiet = left; quietRight = right;
    compressor.process(quiet.data(), quietRight.data(), 4800, settings, broken.data(), broken.data());
    require(std::isfinite(quiet.back()) && std::abs(quiet.back() - 0.05f) < 1.0e-4f, "a broken key is treated as silence");
}

static void effectTests() {
    auto compressor = alien::stock::create(alien::stock::catalog[1].id);
    require(compressor->getBusCount(true) == 2 && !compressor->getBus(true, 1)->isEnabled() && compressor->getTotalNumInputChannels() == 2,
            "the compressor has a sidechain input, off until the host switches it on");
    for (std::size_t i = 0; i < alien::stock::catalog.size(); ++i)
        if (i != 1 && i != gateIndex) require(alien::stock::create(alien::stock::catalog[i].id)->getBusCount(true) == 1, "the other stock effects have no sidechain input");
    alien::PluginRuntime::Slot slot; slot.processor = std::move(compressor);
    alien::PluginHost::applySidechain(slot, compressorState(3), 0);
    require(slot.keyIndex == 3 && slot.processor->getTotalNumInputChannels() == 4, "keying it switches the input on in stereo");
    alien::PluginHost::applySidechain(slot, compressorState(-1), 0);
    require(slot.keyIndex == -1 && slot.processor->getTotalNumInputChannels() == 2, "and unkeying switches it off");
    alien::PluginRuntime::Slot eq; eq.processor = alien::stock::create(alien::stock::catalog[0].id);
    auto eqState = compressorState(2);
    alien::PluginHost::applySidechain(eq, eqState, 0);
    require(eq.keyIndex == -1 && eq.processor->getTotalNumInputChannels() == 2, "an effect without a sidechain input is never keyed");
    alien::PluginHost::applySidechain(slot, compressorState(2), -1);
    require(slot.keyIndex == -1, "an instrument slot is never keyed");
    alien::PluginRuntime::Slot gate; gate.processor = alien::stock::create(alien::stock::catalog[gateIndex].id);
    alien::PluginHost::applySidechain(gate, compressorState(2, alien::KeySource::bus, gateIndex), 0);
    require(gate.keyIndex == 2 && gate.keyFrom == alien::KeySource::bus && gate.processor->getTotalNumInputChannels() == 4, "the gate can be keyed, here by a bus");
}

// A compressor on track 1 keyed by track 0, run through the plugin runtime.
static void runtimeTests() {
    alien::PluginRuntime runtime;
    auto& rack = runtime.racks[1];
    rack.effects[0].processor = alien::stock::create(alien::stock::catalog[1].id);
    alien::PluginHost::applySidechain(rack.effects[0], compressorState(0), 0);
    rack.effectCount = 1;
    runtime.prepareSlot(rack.effects[0], 1, 0);
    const auto run = [&](float key, float own, int blocks) {
        for (int b = 0; b < blocks; ++b) {
            for (std::size_t t = 0; t < runtime.tracks.size(); ++t) runtime.tracks[t].clear();
            for (int c = 0; c < 2; ++c) {
                juce::FloatVectorOperations::fill(runtime.tracks[0].getWritePointer(c), key, 512);
                juce::FloatVectorOperations::fill(runtime.tracks[1].getWritePointer(c), own, 512);
            }
            runtime.effectsAndAlign(512);
        }
        return peak(runtime.tracks[1].getReadPointer(0) + 256, 256);
    };
    require(std::abs(run(0.0f, 0.05f, 20) - 0.05f) < 1.0e-3f, "with its key silent, the keyed track plays at its own level");
    require(run(0.9f, 0.05f, 20) < 0.05f * 0.3f, "when the key track plays, the keyed track is ducked");
    require(std::abs(peak(runtime.tracks[0].getReadPointer(0), 512) - 0.9f) < 1.0e-6f, "the key track itself is untouched");
    require(std::abs(run(0.0f, 0.05f, 150) - 0.05f) < 1.0e-3f, "and the duck releases when the key stops");
    const auto before = allocations;
    run(0.9f, 0.05f, 50);
    require(allocations == before, "a keyed effect allocates nothing on the audio thread");
    // The same compressor unkeyed reacts only to its own input.
    alien::PluginHost::applySidechain(rack.effects[0], compressorState(-1), 0);
    runtime.prepareSlot(rack.effects[0], 1, 0);
    require(std::abs(run(0.9f, 0.05f, 20) - 0.05f) < 1.0e-3f, "unkeyed, another track playing changes nothing");
}

static void projectTests() {
    // Saved with the effect; absent in older files.
    alien::PluginRackState rack; rack.effects.push_back(compressorState(5));
    alien::PluginRackState loaded;
    require(alien::rackFromVar(alien::rackToVar(rack), loaded) && loaded.effects[0].sidechain == 5, "the key track is saved with the effect");
    auto old = alien::rackToVar(rack);
    old.getProperty("effects", {}).getArray()->getReference(0).getDynamicObject()->removeProperty("sidechain");
    alien::PluginRackState older;
    require(alien::rackFromVar(old, older) && older.effects[0].sidechain == -1, "an effect saved without one has no key");
    auto bad = alien::rackToVar(rack);
    bad.getProperty("effects", {}).getArray()->getReference(0).getDynamicObject()->setProperty("sidechain", "kick");
    alien::PluginRackState refused;
    require(!alien::rackFromVar(bad, refused), "a key that is not a track number is rejected");
    auto beyond = compressorState(alien::maxSessionTracks);
    require(!beyond.valid() && compressorState(alien::maxSessionTracks - 1).valid(), "a key past the last possible track is invalid");
    alien::SessionProjectData project;
    project.plugins[0].effects.push_back(compressorState(1));
    require(project.valid(), "a key on an existing track is valid");
    project.plugins[0].effects[0].sidechain = project.trackCount();
    require(!project.valid(), "a key on a track the project does not have is invalid");
    project.plugins[0].effects[0].sidechain = 1;
    require(static_cast<int>(juce::JSON::parse(alien::serializeSessionProject(project)).getProperty("version", 0)) >= 21, "written in format 21 or later");

    // Track moves and deletes carry keys with them; a deleted key track leaves the effect unkeyed.
    ToneEngine engine;
    alien::ProjectController c(engine);
    for (int i = 0; i < 2; ++i) c.addTrack();   // tracks 0-5
    std::vector<alien::PluginRackState> states;
    for (const auto& track : c.document().tracks) states.push_back(track.plugins);
    states[0].effects.push_back(compressorState(3));
    states[1].effects.push_back(compressorState(4));
    c.setPluginRacks(states, c.document().masterPlugins, c.returnRacks(), c.busRacks());
    const auto keyOf = [&](int track) { return c.document().tracks[static_cast<std::size_t>(track)].plugins.effects[0].sidechain; };
    // Swap tracks 3 and 1: track 0's key follows its track to 1; track 1's effect, now on track 3, keeps key 4.
    require(c.moveTrack(3, -2) && keyOf(0) == 1 && keyOf(3) == 4, "moving the key track moves the key with it");
    require(c.deleteTrack(2) && keyOf(0) == 1 && keyOf(2) == 3, "deleting an earlier track renumbers the key");
    require(c.deleteTrack(3) && keyOf(2) == -1 && keyOf(0) == 1, "deleting the key track removes the key, never re-points it");
    require(alien::toProjectData(c.document()).valid(), "the project stays valid");
}

static void panelTests() {
    alien::PluginPanel panel;
    panel.setSize(900, 500);
    alien::PluginRackState rack; rack.effects.push_back(compressorState(-1));
    rack.effects.push_back(alien::PluginState(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(), {}));
    panel.hasSidechainInput = [](int, int index) { return index == 0; };
    std::pair<int, int> chosen{-9, -9};
    alien::KeySource chosenFrom{alien::KeySource::bus};
    panel.onSidechain = [&](int index, alien::KeySource from, int track) { chosen = {index, track}; chosenFrom = from; };
    juce::PopupMenu shown; std::function<void(int)> pick;
    panel.sidechainMenuLauncher = [&](juce::PopupMenu m, std::function<void(int)> done) { shown = std::move(m); pick = std::move(done); };
    juce::KnownPluginList none;
    panel.refresh({"Kick", "Bass", "Pads"}, rack, none);
    juce::Button* sidechain = nullptr;
    for (auto* child : panel.getChildren()) if (child->getName() == "Sidechain source") sidechain = dynamic_cast<juce::Button*>(child);
    require(sidechain != nullptr, "the panel has a Sidechain button");
    juce::ComboBox* slot = nullptr;
    for (auto* child : panel.getChildren()) if (child->getName() == "Instrument or FX slot") slot = dynamic_cast<juce::ComboBox*>(child);
    slot->setSelectedId(2, juce::sendNotificationSync);   // FX 1: the compressor
    require(sidechain->isEnabled(), "it is offered for an effect with a sidechain input");
    panel.showSidechainMenu();
    // The rack shown is track 0's (Kick), so Kick is not offered as its own key.
    int items = 0; bool bassThere = false, ownThere = false;
    for (juce::PopupMenu::MenuItemIterator it(shown); it.next();) {
        ++items; bassThere = bassThere || it.getItem().text == "Key from Bass"; ownThere = ownThere || it.getItem().text == "Key from Kick";
    }
    require(items == 3 && bassThere && !ownThere, "the menu offers Off and every other track, not the effect's own");
    pick(3);   // Key from Bass
    require(chosen.first == 0 && chosen.second == 1 && chosenFrom == alien::KeySource::track, "choosing a track asks for that key");
    slot->setSelectedId(3, juce::sendNotificationSync);   // FX 2: the EQ
    require(!sidechain->isEnabled(), "an effect without a sidechain input cannot be keyed");
    rack.effects[0].sidechain = 1;
    panel.refresh({"Kick", "Bass", "Pads"}, rack, none);
    require(slot->getItemText(1).contains("[keyed by Bass]"), "the slot list shows what keys an effect");

    // The master's FX may hear everything the mix has made: tracks before their FX and after their faders,
    // the buses and the returns. A bus's FX may not hear buses or returns; a return's FX may not hear returns.
    std::array<juce::String, alien::maxBuses> busNames{}; busNames[2] = "Drums";
    std::array<juce::String, alien::maxReturns> returnNames{}; returnNames[0] = "Return A"; returnNames[5] = "Hall";
    const auto offered = [&](int destination) {
        juce::ComboBox* target = nullptr;
        for (auto* child : panel.getChildren()) if (auto* box = dynamic_cast<juce::ComboBox*>(child); box != nullptr && box->getName() == "Plugin destination track, return or master") target = box;
        require(target != nullptr, "the panel has a destination list");
        target->setSelectedId(destination + 1, juce::dontSendNotification);
        rack.effects[0].sidechain = -1;
        panel.refresh({"Kick", "Bass", "Pads"}, rack, none, busNames, returnNames);
        slot->setSelectedId(2, juce::sendNotificationSync);
        panel.showSidechainMenu();
        juce::StringArray texts;
        for (juce::PopupMenu::MenuItemIterator it(shown); it.next();) if (!it.getItem().isSectionHeader) texts.add(it.getItem().text);
        return texts;
    };
    auto master = offered(alien::maxSessionTracks);
    require(master.contains("Key from Kick") && master.contains("Key from Bass (after its fader)") && master.contains("Key from Drums")
            && master.contains("Key from Hall") && master.size() == 1 + 3 + 3 + 1 + 2, "the master offers every key the mix makes");
    pick(alien::PluginPanel::keyReturnBase + 5);
    require(chosen.second == 5 && chosenFrom == alien::KeySource::returnTrack, "choosing a return asks for that return's key");
    pick(alien::PluginPanel::keyPostBase + 1);
    require(chosen.second == 1 && chosenFrom == alien::KeySource::trackPostFader, "and a track after its fader");
    auto ret = offered(alien::PluginRuntime::returnIndex(0));
    require(ret.contains("Key from Drums") && !ret.contains("Key from Hall") && ret.contains("Key from Pads (after its fader)"), "a return offers buses but no returns");
    auto bus = offered(alien::PluginRuntime::busIndex(2));
    require(!bus.contains("Key from Drums") && !bus.contains("Key from Hall") && bus.contains("Key from Kick (after its fader)"), "a bus offers tracks only");
}

static void gateTests() {
    alien::dsp::Gate gate;
    gate.prepare(48000);
    alien::dsp::Gate::Settings settings;   // opens at -40 dB, closes 80 dB down
    std::vector<float> quiet(4800, 0.001f), loud(4800, 0.5f), silent(4800, 0.0f);
    auto a = quiet, b = quiet;
    gate.process(a.data(), b.data(), 4800, settings);
    require(std::abs(a.back()) < 0.001f * 0.001f, "below the threshold the gate stays closed (80 dB down)");
    a = loud; b = loud;
    gate.process(a.data(), b.data(), 4800, settings);
    require(gate.isOpen() && std::abs(a.back() - 0.5f) < 1.0e-3f, "a loud signal opens it and passes unchanged");
    a = silent; b = silent;
    gate.process(a.data(), b.data(), 1200, settings);   // 25 ms: inside the 50 ms hold
    require(gate.isOpen(), "it holds open for the hold time after the signal stops");
    gate.process(a.data(), b.data(), 4800, settings);
    require(!gate.isOpen(), "then it closes");
    settings.rangeDb = -12;
    std::vector<float> second(48000, 0.001f), secondRight = second;   // a second: the 150 ms release has settled
    gate.process(second.data(), secondRight.data(), 48000, settings);
    require(std::abs(second.back() / 0.001f - 0.2512f) < 1.0e-3f, "a smaller range lets some of the sound through (-12 dB)");
    settings.rangeDb = -80;
    gate.reset(); a = quiet; b = quiet;
    gate.process(a.data(), b.data(), 4800, settings, loud.data(), loud.data());
    require(std::abs(a.back() - 0.001f) < 1.0e-5f, "a loud key opens it for a quiet signal");
    gate.reset(); a = loud; b = loud;
    gate.process(a.data(), b.data(), 4800, settings, silent.data(), silent.data());
    require(std::abs(a.back()) < 0.5f * 0.001f, "a silent key keeps even a loud signal out");
    std::vector<float> broken(4800, std::nanf(""));
    gate.reset(); a = loud; b = loud;
    gate.process(a.data(), b.data(), 4800, settings, broken.data(), broken.data());
    require(std::isfinite(a.back()) && !gate.isOpen(), "a broken key reads as silence");
    // A level that hovers just under the threshold does not chatter once open.
    gate.reset(); a = loud; b = loud; gate.process(a.data(), b.data(), 4800, settings);
    std::vector<float> hover(48000, 0.0079f);   // -42 dB: inside the 4 dB band below the threshold
    auto h = hover, hr = hover;
    gate.process(h.data(), hr.data(), 48000, settings);
    require(gate.isOpen() && std::abs(h.back() - 0.0079f) < 1.0e-4f, "inside the hysteresis band an open gate stays open");
}

// The new keys through the real mix: track 0 plays 0.2 and track 1 plays 0.1 (test instruments).
class Instrument final : public juce::AudioProcessor {
public:
    explicit Instrument(float level) : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)), value(level) {}
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int c = 0; c < b.getNumChannels(); ++c) juce::FloatVectorOperations::fill(b.getWritePointer(c), value, b.getNumSamples());
    }
    const juce::String getName() const override { return "Steady test instrument"; }
    bool acceptsMidi() const override { return true; } bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; } void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; } juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {} void setStateInformation(const void*, int) override {}
    float value;
};
struct MixRig {
    std::unique_ptr<ToneEngine> engine = std::make_unique<ToneEngine>();
    std::array<float, 1024> left{}, right{};
    MixRig() {
        engine->prepare(48000);
        for (int t = 0; t < 2; ++t) {
            auto& rack = engine->plugins->racks[static_cast<std::size_t>(t)]; rack.instrumentAssigned = true;
            rack.instrument.processor = std::make_unique<Instrument>(t == 0 ? 0.2f : 0.1f); engine->plugins->prepareSlot(rack.instrument);
        }
        engine->playing.store(true);
    }
    void keyed(int rackIndex, std::size_t effect, int key, alien::KeySource from) {
        auto& rack = engine->plugins->racks[static_cast<std::size_t>(rackIndex)];
        auto& slot = rack.effects[static_cast<std::size_t>(rack.effectCount++)];
        slot.processor = alien::stock::create(alien::stock::catalog[effect].id);
        alien::PluginHost::applySidechain(slot, compressorState(key, from, effect), 0);
        engine->plugins->prepareSlot(slot, rackIndex, rack.effectCount - 1);
    }
    float settled(int blocks = 30) {
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 1024, {});
        return left[1023];
    }
    alien::TrackControls& track(int t) { return engine->tracks[static_cast<std::size_t>(t)]; }
};

static void mixKeyTests() {
    // A gate on the master keyed by track 0 after its fader: the fader, not the instrument, opens it.
    {
        MixRig rig;
        const float open = rig.settled();
        rig.keyed(alien::maxSessionTracks, gateIndex, 0, alien::KeySource::trackPostFader);
        require(std::abs(rig.settled() - open) < 1.0e-3f && open > 0.25f, "with track 0 playing, the master's gate is open");
        rig.track(0).gain.store(0.0f);
        const float closed = rig.settled(150);   // three seconds: the 150 ms release has gone all the way down
        require(std::abs(closed) < 1.0e-4f, "pulling track 0's fader down closes it: the key is after the fader");
        const auto before = allocations;
        rig.settled(10);
        require(allocations == before, "keys made in the mix allocate nothing on the audio thread");
    }
    // The same gate keyed by track 0 before its FX ignores the fader.
    {
        MixRig rig;
        rig.keyed(alien::maxSessionTracks, gateIndex, 0, alien::KeySource::track);
        rig.track(0).gain.store(0.0f);
        require(rig.settled() > 0.09f, "keyed before the FX, the fader does not close it");
    }
    // A compressor on return A keyed by bus 0, which carries track 0.
    {
        MixRig rig;
        rig.engine->buses[0].active.store(true); rig.engine->buses[0].gain.store(2.0f); rig.track(0).output.store(0);
        rig.track(1).send[0].store(1.0f);
        rig.engine->returns[0].active.store(true);
        const float plain = rig.settled();   // 0.4 (bus) + 0.1 (track 1) + 0.1 (return A)
        rig.keyed(alien::PluginRuntime::returnIndex(0), 1, 0, alien::KeySource::bus);
        const float ducked = rig.settled();   // the bus key, -8 dB, is 10 dB over the compressor's threshold
        require(std::abs(plain - 0.6f) < 1.0e-3f && ducked < plain - 0.04f, "the bus playing ducks return A");
        rig.engine->buses[0].mute.store(true);
        require(rig.settled() > 0.19f, "with the bus muted the key is silent and return A comes back");
    }
    // A compressor on the master keyed by return A's output.
    {
        MixRig rig;
        rig.engine->returns[0].active.store(true); rig.engine->returns[0].gain.store(2.0f);
        rig.track(1).send[0].store(1.0f);
        rig.keyed(alien::maxSessionTracks, 1, 0, alien::KeySource::returnTrack);
        const float ducked = rig.settled();   // 0.3 + 0.2 from return A, turned down by its own -14 dB key
        rig.track(1).send[0].store(0.0f);
        const float free = rig.settled();     // no key: the master's 0.3 passes untouched
        require(std::abs(free - 0.3f) < 1.0e-3f && ducked < 0.5f - 0.05f && ducked > 0.0f, "the master ducks while return A plays, and recovers when it stops");
    }
    // A track's FX cannot hear a key the mix makes later: such a slot is not keyed.
    {
        MixRig rig;
        rig.keyed(1, gateIndex, 0, alien::KeySource::bus);
        require(rig.settled() > 0.29f, "a track's gate set to a bus key is left out rather than fed a stale key");
    }
}

static void partTwoProjectTests() {
    alien::PluginRackState rack; rack.effects.push_back(compressorState(3, alien::KeySource::returnTrack));
    alien::PluginRackState loaded;
    require(alien::rackFromVar(alien::rackToVar(rack), loaded) && loaded.effects[0].sidechain == 3
            && loaded.effects[0].sidechainFrom == alien::KeySource::returnTrack, "the kind of key is saved with the effect");
    auto bad = alien::rackToVar(rack);
    bad.getProperty("effects", {}).getArray()->getReference(0).getDynamicObject()->setProperty("sidechainFrom", "aux");
    alien::PluginRackState refused;
    require(!alien::rackFromVar(bad, refused), "an unknown kind of key is rejected");
    require(!compressorState(alien::maxBuses, alien::KeySource::bus).valid() && compressorState(alien::maxBuses - 1, alien::KeySource::bus).valid(),
            "a bus key past the last bus slot is invalid");

    alien::SessionProjectData project;
    project.buses[1] = {true, "Drums", {}};
    project.masterPlugins.effects.push_back(compressorState(1, alien::KeySource::bus));
    require(project.valid(), "the master keyed by a bus that exists is valid");
    project.masterPlugins.effects[0].sidechain = 2;
    require(!project.valid(), "a key from a bus slot not in use is invalid");
    project.masterPlugins.effects[0] = compressorState(1, alien::KeySource::trackPostFader);
    project.plugins[0].effects.push_back(compressorState(1, alien::KeySource::trackPostFader));
    require(!project.valid(), "a track's FX keyed after another track's fader is invalid");
    project.plugins[0].effects.clear();
    project.busPlugins[1].effects.push_back(compressorState(1, alien::KeySource::bus));
    require(!project.valid(), "a bus's FX keyed by a bus is invalid");
    project.busPlugins[1].effects[0] = compressorState(1, alien::KeySource::trackPostFader);
    require(project.valid(), "a bus's FX keyed after a track's fader is valid");
    const auto json = alien::serializeSessionProject(project);
    alien::SessionProjectData back; juce::String error;
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 25 && alien::parseSessionProject(json, back, error)
            && back.masterPlugins.effects[0].sidechainFrom == alien::KeySource::trackPostFader, "written and read as format 25 or later");
    auto old = juce::JSON::parse(json);
    old.getDynamicObject()->setProperty("version", 24);
    require(!alien::parseSessionProject(juce::JSON::toString(old), back, error), "a format 24 file with such a key is refused");

    // Removing a bus or a return removes the keys from it; keys from other slots stay.
    ToneEngine engine;
    alien::ProjectController c(engine);
    require(c.addBus() >= 0 && c.addBus() >= 0, "two buses");
    const int r = c.addReturn("Hall");
    require(r >= 0, "a return");
    auto master = c.document().masterPlugins;
    master.effects.push_back(compressorState(0, alien::KeySource::bus));
    master.effects.push_back(compressorState(1, alien::KeySource::bus));
    master.effects.push_back(compressorState(r, alien::KeySource::returnTrack));
    std::vector<alien::PluginRackState> states;
    for (const auto& track : c.document().tracks) states.push_back(track.plugins);
    c.setPluginRacks(states, master, c.returnRacks(), c.busRacks());
    const auto keyOf = [&](std::size_t i) { return c.document().masterPlugins.effects[i]; };
    require(c.deleteBus(0) && keyOf(0).sidechain == -1 && keyOf(1).sidechain == 1 && keyOf(1).sidechainFrom == alien::KeySource::bus,
            "deleting a bus removes the keys from it and leaves the others");
    require(c.deleteReturn(r) && keyOf(2).sidechain == -1 && keyOf(2).sidechainFrom == alien::KeySource::track, "deleting a return removes the keys from it");
    require(alien::toProjectData(c.document()).valid(), "the project stays valid");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    detectorTests();
    effectTests();
    runtimeTests();
    projectTests();
    panelTests();
    gateTests();
    mixKeyTests();
    partTwoProjectTests();
    std::cout << "Sidechain tests passed\n";
    return 0;
}
