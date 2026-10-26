// Sidechain inputs (MIX-04, part 1): the stock compressor keyed by another track's signal. The
// detector, the compressor's sidechain bus, the runtime handing a key to a keyed slot without
// allocating, the host switching the bus on and off, saving, track moves and deletes, and the
// Plugins panel's choice of key.
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

static alien::PluginState compressorState(int sidechain = -1) {
    alien::PluginState state(alien::stock::describe(alien::stock::catalog[1]).createXml()->toString(), {});
    state.sidechain = sidechain;
    return state;
}
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
        if (i != 1) require(alien::stock::create(alien::stock::catalog[i].id)->getBusCount(true) == 1, "the other stock effects have no sidechain input");
    alien::PluginRuntime::Slot slot; slot.processor = std::move(compressor);
    alien::PluginHost::applySidechain(slot, compressorState(3), 0);
    require(slot.keyTrack == 3 && slot.processor->getTotalNumInputChannels() == 4, "keying it switches the input on in stereo");
    alien::PluginHost::applySidechain(slot, compressorState(-1), 0);
    require(slot.keyTrack == -1 && slot.processor->getTotalNumInputChannels() == 2, "and unkeying switches it off");
    alien::PluginRuntime::Slot eq; eq.processor = alien::stock::create(alien::stock::catalog[0].id);
    auto eqState = compressorState(2);
    alien::PluginHost::applySidechain(eq, eqState, 0);
    require(eq.keyTrack == -1 && eq.processor->getTotalNumInputChannels() == 2, "an effect without a sidechain input is never keyed");
    alien::PluginHost::applySidechain(slot, compressorState(2), -1);
    require(slot.keyTrack == -1, "an instrument slot is never keyed");
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
    panel.onSidechain = [&](int index, int track) { chosen = {index, track}; };
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
    require(chosen.first == 0 && chosen.second == 1, "choosing a track asks for that key");
    slot->setSelectedId(3, juce::sendNotificationSync);   // FX 2: the EQ
    require(!sidechain->isEnabled(), "an effect without a sidechain input cannot be keyed");
    rack.effects[0].sidechain = 1;
    panel.refresh({"Kick", "Bass", "Pads"}, rack, none);
    require(slot->getItemText(1).contains("[keyed by Bass]"), "the slot list shows what keys an effect");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    detectorTests();
    effectTests();
    runtimeTests();
    projectTests();
    panelTests();
    std::cout << "Sidechain tests passed\n";
    return 0;
}
