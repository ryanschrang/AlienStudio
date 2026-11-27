// Sidechain inputs (MIX-04). Part 1: the stock compressor keyed by another track's signal - the
// detector, the compressor's sidechain bus, the runtime handing a key to a keyed slot without
// allocating, the host switching the bus on and off, saving, track moves and deletes, and the
// Plugins panel's choice of key. Part 2: the stock gate, keys from a track after its fader, from a
// bus and from a return, which racks may use which keys, format 25, and bus/return removal.
#include "CommandEngine.h"
#include "PluginHost.h"
#include "PluginPanel.h"
#include "TrackMixerPanel.h"
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

// The command writes the saved key; what the host builds from that key is what plays.
static void commandKeyTests() {
    namespace cmd = alien::command;
    using alien::KeySource; using alien::RackStage;
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    cmd::CommandEngine commands(c);
    require(commands.execute(cmd::CreateBus{}).changed, "bus fixture");
    std::vector<alien::PluginRackState> states;
    for (const auto& track : c.document().tracks) states.push_back(track.plugins);
    states[0].effects.push_back(compressorState());
    auto master = c.document().masterPlugins;
    alien::PluginState eq(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(), {});
    master.effects.push_back(eq);
    c.setPluginRacks(states, master, c.returnRacks(), c.busRacks());
    const auto saved = [&] { return c.document().tracks[0].plugins.effects[0]; };

    alien::PluginRuntime::Slot slot; slot.processor = alien::stock::create(alien::stock::catalog[1].id);
    require(commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 2}).changed, "the command keys the compressor");
    alien::PluginHost::applySidechain(slot, saved(), 0);
    require(slot.keyIndex == 2 && slot.keyFrom == KeySource::track && slot.processor->getTotalNumInputChannels() == 4, "the saved key switches the real effect's input on");
    require(c.undo() && saved().sidechain == -1, "one Undo removes the key");
    alien::PluginHost::applySidechain(slot, saved(), 0);
    require(slot.keyIndex == -1 && slot.processor->getTotalNumInputChannels() == 2, "and the effect goes back to its own input");
    require(c.redo() && saved().sidechain == 2, "Redo restores it");

    // Keys set by the command follow the same track moves and deletions as any other key.
    require(c.moveTrack(2, -1) && saved().sidechain == 1, "a key follows its track when it moves");
    require(c.deleteTrack(1) && saved().sidechain == -1 && alien::toProjectData(c.document()).valid(), "and goes away with it");

    // Preview cannot ask a plugin, so an effect with no sidechain input takes the saved key and stays unkeyed.
    require(commands.execute(cmd::SetSidechain{RackStage::master, 0, 0, KeySource::bus, 0}).changed, "the command accepts a key for the EQ");
    alien::PluginRuntime::Slot plain; plain.processor = alien::stock::create(alien::stock::catalog[0].id);
    alien::PluginHost::applySidechain(plain, c.document().masterPlugins.effects[0], 0);
    require(plain.keyIndex == -1 && plain.processor->getTotalNumInputChannels() == 2, "an effect without a sidechain input is never keyed");
    std::cout << "PASS: SetSidechain drives the keys the real stock effects use\n";
}

// Device commands through the same capture, execute and rebuild steps the app uses, with real stock
// effects in a real host, so live settings are shown to survive a rack edit and its Undo.
static void deviceHostTests() {
    namespace cmd = alien::command;
    using alien::RackStage;
    ToneEngine engine;
    alien::PluginHost host(*engine.plugins);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    cmd::CommandEngine commands(c);
    auto& rack = engine.plugins->racks[0];
    const auto rebuild = [&] {
        std::vector<alien::PluginRackState> states;
        for (const auto& track : c.document().tracks) states.push_back(track.plugins);
        const auto errors = host.restore(states, c.document().masterPlugins, c.returnRacks(), c.busRacks(), 48000);
        require(errors.isEmpty(), "the stock effects load");
    };
    const auto capture = [&] {
        std::vector<alien::PluginRackState> states;
        for (const auto& track : c.document().tracks) states.push_back(track.plugins);
        auto master = c.document().masterPlugins; auto returns = c.returnRacks(); auto buses = c.busRacks();
        host.snapshot(states, master, returns, buses);
        c.setPluginRacks(states, master, returns, buses);
    };
    const auto run = [&](const cmd::Command& edit) { return commands.executeWithPlugins(edit, capture, rebuild); };
    const auto description = [](std::size_t effect) { return alien::stock::describe(alien::stock::catalog[effect]).createXml()->toString(); };
    const auto loaded = [&](int slot) { return rack.effects[static_cast<std::size_t>(slot)].processor.get(); };
    const auto firstParameter = [&](int slot) { return loaded(slot)->getParameters()[0]; };

    const alien::PluginState eq(description(0), {}), compressor(description(1), {});
    require(run(cmd::InsertDevice{RackStage::track, 0, 0, eq}).changed && rack.effectCount == 1 && loaded(0)->getName() == alien::stock::create(alien::stock::catalog[0].id)->getName(), "inserting loads the effect");
    require(run(cmd::InsertDevice{RackStage::track, 0, 1, compressor}).changed && rack.effectCount == 2, "and a second one after it");
    require(c.undoLabel() == "Insert device", "the host's capture does not take over the Undo label");

    // A setting changed only in the live plugin, not yet in the document.
    firstParameter(1)->setValueNotifyingHost(.81f);
    const auto eqName = loaded(0)->getName(), compressorName = loaded(1)->getName();
    require(run(cmd::MoveDevice{RackStage::track, 0, 1, 0}).changed, "the move runs");
    require(rack.effectCount == 2 && loaded(0)->getName() == compressorName && loaded(1)->getName() == eqName, "the rebuilt rack has the new order");
    require(std::abs(firstParameter(0)->getValue() - .81f) < .01f, "the moved effect keeps its live setting");
    require(c.undoLabel() == "Move device", "one Undo step is named for the move");

    require(c.undo(), "Undo the move");
    rebuild();
    require(loaded(0)->getName() == eqName && loaded(1)->getName() == compressorName, "Undo restores the order");
    require(std::abs(firstParameter(1)->getValue() - .81f) < .01f, "and keeps the setting captured before the move");

    require(run(cmd::SetDeviceBypass{RackStage::track, 0, 1, true}).changed && c.document().tracks[0].plugins.effects[1].bypass, "bypass runs");
    require(rack.effects[1].bypass, "the rebuilt slot is bypassed");
    require(std::abs(firstParameter(1)->getValue() - .81f) < .01f, "bypass keeps the setting");
    require(c.undo(), "Undo the bypass"); rebuild();
    require(!rack.effects[1].bypass, "Undo enables it again");

    require(run(cmd::RemoveDevice{RackStage::track, 0, 1}).changed && rack.effectCount == 1 && loaded(0)->getName() == eqName, "removing unloads the effect");
    require(c.undo(), "Undo the removal"); rebuild();
    require(rack.effectCount == 2 && std::abs(firstParameter(1)->getValue() - .81f) < .01f, "Undo brings it back with its setting");

    // Nothing to do: no capture and no rebuild, and a refusal never reaches the plugins.
    int captures = 0, rebuilds = 0;
    const auto counted = [&](const cmd::Command& edit) { return commands.executeWithPlugins(edit, [&] { ++captures; }, [&] { ++rebuilds; }); };
    require(counted(cmd::SetDeviceBypass{RackStage::track, 0, 1, false}).ok && captures == 0 && rebuilds == 0, "an unchanged bypass captures nothing");
    require(!counted(cmd::RemoveDevice{RackStage::track, 0, 5}).ok && captures == 0 && rebuilds == 0, "a refused edit captures nothing");
    engine.recording.phase.store(alien::RecordControls::recording);
    const auto recording = counted(cmd::RemoveDevice{RackStage::track, 0, 1});
    require(!recording.ok && recording.error.contains("Stop recording") && captures == 0 && rebuilds == 0, "recording refuses before any capture");
    const auto keyWhileRecording = counted(cmd::SetSidechain{RackStage::track, 0, 1, alien::KeySource::track, 1});
    require(!keyWhileRecording.ok && captures == 0 && rebuilds == 0, "and so does a sidechain key");
    engine.recording.phase.store(alien::RecordControls::idle);
    host.restore({}, {}, {}, {}, 48000);
    std::cout << "PASS: device commands keep live plugin settings through the host\n";
}

static void trackSidechainViewTests(const juce::File& preview) {
    namespace cmd = alien::command;
    auto engine = std::make_unique<ToneEngine>();
    alien::PluginHost host(*engine->plugins); alien::ProjectController c(*engine); c.pushDocumentToEngine();
    cmd::CommandEngine commands(c); int captures = 0, rebuilds = 0;
    const auto rebuild = [&] {
        ++rebuilds; std::vector<alien::PluginRackState> states;
        for (const auto& track : c.document().tracks) states.push_back(track.plugins);
        require(host.restore(states,c.document().masterPlugins,c.returnRacks(),c.busRacks(),48000).isEmpty(),"sidechain graph stock FX load");
    };
    const auto capture = [&] {
        ++captures; std::vector<alien::PluginRackState> states;
        for (const auto& track : c.document().tracks) states.push_back(track.plugins);
        auto master=c.document().masterPlugins; auto returns=c.returnRacks(); auto buses=c.busRacks();
        host.snapshot(states,master,returns,buses); c.setPluginRacks(states,master,returns,buses);
    };
    const auto run = [&](const cmd::Command& edit) { return commands.executeWithPlugins(edit,capture,rebuild); };
    const alien::PluginState eq(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(),{});
    require(run(cmd::InsertDevice{alien::RackStage::track,0,0,compressorState()}).changed
        && run(cmd::InsertDevice{alien::RackStage::track,0,1,eq}).changed,"graph FX fixture");
    c.markAsNewBaseline(); captures=0; rebuilds=0;
    alien::TrackMixerPanel panel(c,engine->tracks,engine->returns,engine->masterControls,engine->buses); panel.setSize(960,650);
    auto& graph = panel.trackSidechainView();
    graph.hasInput = [&](alien::RackStage stage,int track,int fx) { return host.hasSidechainInput(stage == alien::RackStage::master ? alien::PluginRuntime::masterIndex : stage == alien::RackStage::bus ? alien::PluginRuntime::busIndex(track) : stage == alien::RackStage::returnTrack ? alien::PluginRuntime::returnIndex(track) : track,fx); };
    graph.onKeyRequested = [&](alien::RackStage stage,int track,int fx,int source) {
        const auto result=run(cmd::SetSidechain{stage,track,fx,alien::KeySource::track,source});
        panel.refreshSettings(); return result.ok && result.changed;
    };
    panel.showSidechains(true);
    require(graph.isVisible() && !panel.outputRoutingView().isVisible(),"Mixer sidechains mode replaces output graph");
    const auto request=graph.requestFor(0,0);
    engine->plugins->racks[0].effects[0].processor->getParameters()[0]->setValueNotifyingHost(.78f);
    require(graph.choose(request,1) && captures==1 && rebuilds==1 && c.undoLabel()=="Set sidechain"
        && engine->plugins->racks[0].effects[0].keyIndex==1,"graph uses the command capture/rebuild path and keys the actual effect");
    require(std::abs(engine->plugins->racks[0].effects[0].processor->getParameters()[0]->getValue()-.78f)<.01f,"key change preserves live plugin settings");
    require(!graph.choose(request,1) && captures==1 && rebuilds==1,"same key is a no-op without plugin callbacks");
    require(c.undo(),"undo graph key"); rebuild(); panel.refreshSettings();
    require(engine->plugins->racks[0].effects[0].keyIndex==-1
        && std::abs(engine->plugins->racks[0].effects[0].processor->getParameters()[0]->getValue()-.78f)<.01f,"Undo removes the key but keeps the captured live setting");
    require(c.redo(),"redo graph key"); rebuild(); panel.refreshSettings();
    require(run(cmd::InsertDevice{alien::RackStage::master,0,0,compressorState()}).changed
        && run(cmd::InsertDevice{alien::RackStage::master,0,1,alien::PluginState(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(),{})}).changed,"master graph FX fixture");
    panel.refreshSettings();
    const auto masterRequest=graph.requestFor(0,0,alien::RackStage::master);
    auto& masterSlot=engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[0];
    masterSlot.processor->getParameters()[0]->setValueNotifyingHost(.63f);
    const int masterCaptures=captures, masterRebuilds=rebuilds;
    require(graph.choose(masterRequest,0) && captures==masterCaptures+1 && rebuilds==masterRebuilds+1
        && masterSlot.keyIndex==0 && masterSlot.keyFrom==alien::KeySource::track,
        "master graph permits track zero as a pre-FX key through capture/rebuild");
    require(std::abs(masterSlot.processor->getParameters()[0]->getValue()-.63f)<.01f,"master graph keeps live plugin settings");
    require(!graph.choose(masterRequest,0) && !graph.choose(graph.requestFor(0,1,alien::RackStage::master),1)
        && captures==masterCaptures+1 && rebuilds==masterRebuilds+1,"master no-op and unsupported effect skip plugin callbacks");
    require(c.undo(),"undo master graph key"); rebuild(); panel.refreshSettings();
    require(masterSlot.keyIndex==-1 && std::abs(masterSlot.processor->getParameters()[0]->getValue()-.63f)<.01f,
        "master Undo restores Off and retains captured settings");
    require(c.redo(),"redo master graph key"); rebuild(); panel.refreshSettings();
    require(masterSlot.keyIndex==0,"master Redo restores actual runtime key");
    c.setExternalBusy(true); require(!graph.choose(masterRequest,1),"busy master graph refuses"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    const bool masterCounting=graph.choose(masterRequest,1); engine->recording.phase.store(alien::RecordControls::idle);
    require(!masterCounting && !graph.choose(masterRequest,99),"master count-in and missing source refuse");
    require(run(cmd::MoveDevice{alien::RackStage::master,0,0,1}).changed && graph.choose(masterRequest,1)
        && engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[1].keyIndex==1,"master deferred choice follows effect instance across FX moves");
    bool masterNode=false;
    for (auto* child:graph.getChildren()) if(child->getName().startsWith("Master / FX 2")) masterNode=true;
    require(masterNode,"master effect has a visible graph node");
    require(run(cmd::SetSidechain{alien::RackStage::master,0,1,alien::KeySource::trackPostFader,1}).changed,"existing post-fader master key fixture");
    panel.refreshSettings();
    bool otherKeyLabel=false;
    for (auto* child:graph.getChildren())
        if(child->getName().startsWith("Master / FX 2") && child->getName().contains("key set in Plugins")) otherKeyLabel=true;
    require(otherKeyLabel && engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[1].keyFrom==alien::KeySource::trackPostFader,
        "refresh labels an existing other-stage key without changing it");
    require(graph.choose(masterRequest,1) && engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[1].keyFrom==alien::KeySource::track,
        "choosing the same track explicitly replaces its post-fader key with a pre-FX key");
    require(graph.choose(masterRequest,-1) && engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[1].keyIndex==-1
        && graph.choose(masterRequest,1),"master graph can turn a key Off and restore it");
    require(commands.execute(cmd::CreateBus{}).changed && commands.execute(cmd::RenameBus{0,"Drums"}).changed
        && run(cmd::InsertDevice{alien::RackStage::bus,0,0,compressorState()}).changed,"bus graph fixture");
    panel.rebuild();
    const auto busRequest=graph.requestFor(0,0,alien::RackStage::bus);
    auto& busSlot=engine->plugins->racks[alien::PluginRuntime::busIndex(0)].effects[0];
    busSlot.processor->getParameters()[0]->setValueNotifyingHost(.71f);
    require(graph.choose(busRequest,0) && busSlot.keyIndex==0 && busSlot.keyFrom==alien::KeySource::track
        && std::abs(busSlot.processor->getParameters()[0]->getValue()-.71f)<.01f,"bus graph keys the real effect and preserves live settings");
    require(c.undo(),"undo bus graph key"); rebuild(); panel.refreshSettings();
    require(busSlot.keyIndex==-1 && std::abs(busSlot.processor->getParameters()[0]->getValue()-.71f)<.01f,"bus Undo restores Off and captured settings");
    require(c.redo(),"redo bus graph key"); rebuild(); panel.refreshSettings();
    require(busSlot.keyIndex==0,"bus Redo restores runtime key");
    const int busCaptures=captures, busRebuilds=rebuilds;
    c.setExternalBusy(true); const bool busBusy=graph.choose(busRequest,1); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    const bool busCounting=graph.choose(busRequest,1); engine->recording.phase.store(alien::RecordControls::idle);
    graph.setEnabled(false); const bool busDisabled=graph.choose(busRequest,1); graph.setEnabled(true);
    require(!graph.choose(busRequest,0) && !graph.choose(busRequest,99) && !busBusy && !busCounting && !busDisabled
        && captures==busCaptures && rebuilds==busRebuilds,"bus no-op/invalid/busy/count-in/disabled choices skip plugin callbacks");
    require(run(cmd::SetSidechain{alien::RackStage::bus,0,0,alien::KeySource::trackPostFader,0}).changed,"post-fader bus key fixture");
    panel.refreshSettings();
    require(busSlot.keyFrom==alien::KeySource::trackPostFader && graph.choose(busRequest,0)
        && busSlot.keyFrom==alien::KeySource::track,"bus refresh preserves other key stages until an explicit graph choice");
    require(graph.choose(busRequest,-1) && busSlot.keyIndex==-1 && graph.choose(busRequest,1),"bus Off and fresh key choices work");
    require(run(cmd::InsertDevice{alien::RackStage::bus,0,1,alien::PluginState(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(),{})}).changed
        && !graph.choose(graph.requestFor(0,1,alien::RackStage::bus),0),"unsupported bus FX refuses graph routing");
    require(run(cmd::MoveDevice{alien::RackStage::bus,0,0,1}).changed && graph.choose(busRequest,2)
        && engine->plugins->racks[alien::PluginRuntime::busIndex(0)].effects[1].keyIndex==2,"bus deferred choice follows its effect across FX moves");
    require(commands.execute(cmd::RenameBus{0,"Drum group"}).changed,"rename graph bus"); panel.refreshSettings();
    bool busNode=false;
    for (auto* child:graph.getChildren()) if(child->getName().startsWith("Bus: Drum group / FX 2")) busNode=true;
    require(busNode,"bus rename refreshes its visible effect nodes");
    require(run(cmd::InsertDevice{alien::RackStage::returnTrack,0,0,compressorState()}).changed,"return graph fixture");
    panel.rebuild();
    const auto returnRequest=graph.requestFor(0,0,alien::RackStage::returnTrack);
    auto& returnSlot=engine->plugins->racks[alien::PluginRuntime::returnIndex(0)].effects[0];
    returnSlot.processor->getParameters()[0]->setValueNotifyingHost(.64f);
    require(graph.choose(returnRequest,0) && returnSlot.keyIndex==0 && returnSlot.keyFrom==alien::KeySource::track
        && std::abs(returnSlot.processor->getParameters()[0]->getValue()-.64f)<.01f,"return graph keys the real effect and keeps live settings");
    require(c.undo(),"undo return key"); rebuild(); panel.refreshSettings();
    require(returnSlot.keyIndex==-1 && std::abs(returnSlot.processor->getParameters()[0]->getValue()-.64f)<.01f,"return Undo keeps captured settings");
    require(c.redo(),"redo return key"); rebuild(); panel.refreshSettings(); require(returnSlot.keyIndex==0,"return Redo restores key");
    const int returnCaptures=captures, returnRebuilds=rebuilds;
    c.setExternalBusy(true); const bool returnBusy=graph.choose(returnRequest,1); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    const bool returnCounting=graph.choose(returnRequest,1); engine->recording.phase.store(alien::RecordControls::idle);
    graph.setEnabled(false); const bool returnDisabled=graph.choose(returnRequest,1); graph.setEnabled(true);
    require(!graph.choose(returnRequest,0) && !graph.choose(returnRequest,99) && !returnBusy && !returnCounting && !returnDisabled
        && captures==returnCaptures && rebuilds==returnRebuilds,"return no-op/invalid/busy/count-in/disabled choices skip plugin callbacks");
    require(run(cmd::SetSidechain{alien::RackStage::returnTrack,0,0,alien::KeySource::trackPostFader,0}).changed,"post-fader return key fixture"); panel.refreshSettings();
    require(returnSlot.keyFrom==alien::KeySource::trackPostFader && graph.choose(returnRequest,0)
        && returnSlot.keyFrom==alien::KeySource::track,"return other-stage keys stay intact until explicit replacement");
    require(graph.choose(returnRequest,-1) && returnSlot.keyIndex==-1 && graph.choose(returnRequest,1),"return Off and fresh keys work");
    require(run(cmd::InsertDevice{alien::RackStage::returnTrack,0,1,alien::PluginState(alien::stock::describe(alien::stock::catalog[0]).createXml()->toString(),{})}).changed
        && !graph.choose(graph.requestFor(0,1,alien::RackStage::returnTrack),0),"unsupported return FX refuses routing");
    require(run(cmd::MoveDevice{alien::RackStage::returnTrack,0,0,1}).changed && graph.choose(returnRequest,2)
        && engine->plugins->racks[alien::PluginRuntime::returnIndex(0)].effects[1].keyIndex==2,"return choice follows effect movement");
    require(commands.execute(cmd::RenameReturn{0,"Drum space"}).changed,"rename graph return"); panel.refreshSettings();
    bool returnNode=false;
    for (auto* child:graph.getChildren()) if(child->getName().startsWith("Return: Drum space / FX 2")) returnNode=true;
    require(returnNode,"return rename refreshes visible graph node");
    if (preview != juce::File{}) {
        juce::FileOutputStream out(preview);
        require(out.openedOk() && out.setPosition(0) && out.truncate().wasOk()
            && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()),out),"render sidechain graph");
    }
    const int captured=captures, rebuilt=rebuilds;
    c.setExternalBusy(true); require(!graph.choose(request,2),"busy sidechain graph refuses"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    const bool counting=graph.choose(request,2); engine->recording.phase.store(alien::RecordControls::idle);
    require(!counting && !graph.choose(request,0) && !graph.choose(request,99) && !graph.choose(graph.requestFor(0,1),1)
        && captures==captured && rebuilds==rebuilt,"count-in, self/missing source and unsupported FX refuse before capture");
    panel.setEnabled(false); require(!graph.choose(request,2),"disabled graph refuses"); panel.setEnabled(true);
    require(run(cmd::MoveDevice{alien::RackStage::track,0,0,1}).changed && graph.choose(request,2)
        && engine->plugins->racks[0].effects[1].keyIndex==2,"deferred key follows its effect instance across FX moves");
    require(commands.execute(cmd::MoveTrack{2,3}).changed,"move key source"); rebuild(); panel.rebuild();
    require(graph.choose(request,-1) && graph.choose(request,2) && engine->plugins->racks[0].effects[1].keyIndex==3,"deferred source follows track identity");
    require(commands.execute(cmd::MoveTrack{0,1}).changed,"move key destination"); rebuild(); panel.rebuild();
    require(graph.choose(request,1) && engine->plugins->racks[1].effects[1].keyIndex==0,"deferred destination follows track identity");
    require(run(cmd::RemoveDevice{alien::RackStage::track,1,1}).changed
        && run(cmd::InsertDevice{alien::RackStage::track,1,1,compressorState()}).changed && !graph.choose(request,-1),"replaced FX instance refuses stale choice");
    const auto fresh=graph.requestFor(1,1);
    require(graph.choose(returnRequest,-1) && graph.choose(returnRequest,2)
        && engine->plugins->racks[alien::PluginRuntime::returnIndex(0)].effects[1].keyIndex==3,"return choice follows reordered source identity");
    require(graph.choose(busRequest,-1) && graph.choose(busRequest,2)
        && engine->plugins->racks[alien::PluginRuntime::busIndex(0)].effects[1].keyIndex==3,
        "bus deferred source follows reordered track identity");
    require(graph.choose(masterRequest,2) && engine->plugins->racks[alien::PluginRuntime::masterIndex].effects[1].keyIndex==3,
        "master deferred source follows reordered track identity");
    require(run(cmd::RemoveDevice{alien::RackStage::master,0,1}).changed
        && run(cmd::InsertDevice{alien::RackStage::master,0,1,compressorState()}).changed && !graph.choose(masterRequest,-1),
        "replaced master effect refuses stale choice");
    const auto freshMaster=graph.requestFor(0,1,alien::RackStage::master);
    require(commands.execute(cmd::DeleteTrack{3}).changed,"delete source"); rebuild(); panel.rebuild();
    require(!graph.choose(fresh,3),"deleted source refuses stale choice");
    require(!graph.choose(freshMaster,3),"deleted master key source refuses stale choice");
    require(!graph.choose(busRequest,2),"deleted bus key source refuses stale choice");
    const auto retiredBusEffect=c.document().buses[0].plugins.effects[1];
    const auto oldBus=c.document().trackId(alien::TrackRole::bus,0);
    require(commands.execute(cmd::DeleteBus{0}).changed && !graph.choose(busRequest,-1)
        && commands.execute(cmd::CreateBus{}).changed
        && c.document().trackId(alien::TrackRole::bus,0)!=oldBus
        && run(cmd::InsertDevice{alien::RackStage::bus,0,0,retiredBusEffect}).changed && !graph.choose(busRequest,-1),
        "reused bus slot refuses a stale choice even when it contains the same saved effect identity");
    const auto freshBus=graph.requestFor(0,0,alien::RackStage::bus);
    require(!graph.choose(returnRequest,2),"deleted return key source refuses stale choice");
    const auto retiredReturnEffect=c.document().returns[0].plugins.effects[1];
    const auto oldReturn=c.document().trackId(alien::TrackRole::returnTrack,0);
    require(commands.execute(cmd::DeleteReturn{0}).changed && !graph.choose(returnRequest,-1)
        && commands.execute(cmd::CreateReturn{}).changed && c.document().trackId(alien::TrackRole::returnTrack,0)!=oldReturn
        && run(cmd::InsertDevice{alien::RackStage::returnTrack,0,0,retiredReturnEffect}).changed && !graph.choose(returnRequest,-1),
        "reused return slot refuses stale menu even with the original saved effect identity");
    const auto freshReturn=graph.requestFor(0,0,alien::RackStage::returnTrack);
    c.newProject(); rebuild(); panel.rebuild(); require(!graph.choose(fresh,-1) && !graph.choose(freshMaster,-1) && !graph.choose(freshBus,-1) && !graph.choose(freshReturn,-1),"replacement project refuses all stale FX choices");
    panel.showRouting(false); require(!graph.isVisible(),"Strips hides the sidechain view");
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    detectorTests();
    effectTests();
    runtimeTests();
    projectTests();
    panelTests();
    gateTests();
    mixKeyTests();
    partTwoProjectTests();
    commandKeyTests();
    deviceHostTests();
    trackSidechainViewTests(argc>1?juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]):juce::File{});
    std::cout << "Sidechain tests passed\n";
    return 0;
}
