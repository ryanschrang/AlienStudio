#pragma once
#include "MasteringChain.h"
#include <cmath>
// T2-03: the gentle mastering chain preset for Master FX. Its contents and settings, one Undo step,
// existing devices kept, refusals, and what the three effects do to a quiet and a hot signal.

// The effect a rack device describes, built the way the host builds it: from its identifier, then its saved state.
static std::unique_ptr<alien::stock::Effect> masteringEffect(const alien::PluginState& device) {
    juce::PluginDescription description;
    const auto xml = juce::parseXML(device.descriptionXml);
    require(xml != nullptr && description.loadFromXml(*xml) && alien::stock::isStock(description), "a chain device is a stock effect");
    auto effect = alien::stock::create(description.fileOrIdentifier);
    require(effect != nullptr, "the stock effect exists");
    juce::MemoryBlock bytes;
    require(device.stateBase64.isNotEmpty() && bytes.fromBase64Encoding(device.stateBase64), "a chain device carries its settings");
    effect->setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
    return effect;
}
static double masteringValue(alien::stock::Effect& effect, const char* id) {
    auto* parameter = effect.parameters.getParameter(id);
    require(parameter != nullptr, "the parameter exists");
    return alien::stock::plainValue(*parameter);
}
template <std::size_t Count>
static void masteringSettingsHold(alien::stock::Effect& effect, const std::array<alien::mastering::Setting, Count>& settings) {
    for (const auto& setting : settings) require(std::abs(masteringValue(effect, setting.id) - setting.value) < 1.0e-3, "a chain setting survives the saved state");
}
// Peak of the last tenth of a second of a 1 kHz sine of `amplitude` sent through the chain in rack order.
static float masteringChainPeak(float amplitude) {
    constexpr double rate = 48000.0; constexpr int block = 512;
    std::vector<std::unique_ptr<alien::stock::Effect>> effects;
    for (const auto& device : alien::mastering::chain()) {
        effects.push_back(masteringEffect(device));
        effects.back()->setRateAndBufferSizeDetails(rate, block);
        effects.back()->prepareToPlay(rate, block);
    }
    juce::AudioBuffer<float> buffer(2, block); juce::MidiBuffer midi;
    float peak = 0.0f; std::int64_t frame = 0;
    const std::int64_t total = static_cast<std::int64_t>(rate) * 2, tail = total - static_cast<std::int64_t>(rate / 10);
    while (frame < total) {
        for (int i = 0; i < block; ++i) {
            const auto sample = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(frame + i) / rate));
            buffer.setSample(0, i, sample); buffer.setSample(1, i, sample);
        }
        for (auto& effect : effects) effect->processBlock(buffer, midi);
        if (frame >= tail) peak = std::max(peak, buffer.getMagnitude(0, block));
        frame += block;
    }
    return peak;
}

static void masteringChainChecks() {
    using alien::RackStage;
    namespace mastering = alien::mastering;
    const auto names = [](const alien::PluginRackState& rack) {
        juce::StringArray list; for (const auto& effect : rack.effects) list.add(alien::pluginDisplayName(effect));
        return list.joinIntoString(",");
    };
    const juce::String chainNames = "Alien EQ,Alien Compressor,Alien Limiter";

    // Contents: EQ, compressor, limiter, in that order, enabled, unkeyed, each with its own identity.
    const auto chain = mastering::chain();
    require(static_cast<int>(chain.size()) == mastering::devices && mastering::devices == 3, "the chain is three devices");
    alien::PluginRackState asRack; asRack.effects = chain;
    require(names(asRack) == chainNames && asRack.valid(), "EQ, then compressor, then limiter");
    for (const auto& device : chain) require(device.valid() && !device.bypass && device.sidechain == -1 && device.instanceId != 0, "each device is enabled and unkeyed");
    require(chain[0].instanceId != chain[1].instanceId && chain[1].instanceId != chain[2].instanceId && chain[0].instanceId != chain[2].instanceId, "identities differ");
    require(mastering::chain()[0].instanceId != chain[0].instanceId, "a second chain is new devices");

    // Settings: what the constants say is what the saved state restores, and they are conservative.
    {
        auto eq = masteringEffect(chain[0]); auto compressor = masteringEffect(chain[1]); auto limiter = masteringEffect(chain[2]);
        masteringSettingsHold(*eq, mastering::eqSettings); masteringSettingsHold(*compressor, mastering::compressorSettings); masteringSettingsHold(*limiter, mastering::limiterSettings);
        using namespace alien::stock;
        for (const auto* gain : {eq::lowGain, eq::m1Gain, eq::m2Gain, eq::highGain, eq::output}) require(std::abs(masteringValue(*eq, gain)) <= 1.0 + 1.0e-3, "no EQ band moves more than 1 dB");
        require(masteringValue(*eq, eq::lowGain) == 0.0 && masteringValue(*eq, eq::m2Gain) == 0.0 && masteringValue(*eq, eq::m1Gain) < 0.0 && masteringValue(*eq, eq::highGain) > 0.0, "a low-mid trim and a little air; the rest is flat");
        require(masteringValue(*compressor, comp::ratio) <= 2.0 && masteringValue(*compressor, comp::makeup) == 0.0 && masteringValue(*compressor, comp::attack) >= 20.0 && masteringValue(*compressor, comp::mix) == 1.0, "low ratio, slow attack, no make-up gain");
        require(masteringValue(*limiter, limiter::gain) == 0.0 && masteringValue(*limiter, limiter::ceiling) <= -1.0 + 1.0e-3, "the limiter adds no gain and holds a -1 dBFS ceiling");
    }

    // What it does to audio: a quiet signal passes almost untouched; a hot one stays under the ceiling.
    {
        const float quiet = masteringChainPeak(0.1f), hot = masteringChainPeak(2.0f);
        const double quietChangeDb = 20.0 * std::log10(static_cast<double>(quiet) / 0.1);
        require(std::abs(quietChangeDb) < 1.0, "a -20 dBFS tone changes by less than 1 dB");
        require(hot > 0.5f && hot <= std::pow(10.0f, -1.0f / 20.0f) + 1.0e-3f, "a +6 dBFS tone stays at or under -1 dBFS");
    }

    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto master = [&]() -> const alien::PluginRackState& { return c.document().masterPlugins; };
    int captures = 0, rebuilds = 0;
    const auto add = [&] { return mastering::add(c, [&] { ++captures; }, [&] { ++rebuilds; }); };

    // An empty master: one Undo step, named, that Undo removes whole and Redo restores.
    const auto empty = c.document();
    const auto added = add();
    require(added.ok && added.changed && names(master()) == chainNames && captures == 1 && rebuilds == 1, "the chain lands on an empty master");
    require(c.undoLabel() == mastering::undoLabel && alien::toProjectData(c.document()).valid(), "one named step, and the project saves");
    masteringSettingsHold(*masteringEffect(master().effects[1]), mastering::compressorSettings);
    const auto withChain = c.document();
    require(c.undo() && alien::sameContent(empty, c.document()) && !c.canUndo(), "one Undo removes all three");
    require(c.redo() && alien::sameContent(withChain, c.document()), "Redo restores all three");
    require(c.undo(), "back to empty");

    // Preview of the same commands matches what the batch does, and changes nothing.
    {
        const auto list = mastering::commands(c.document());
        cmd::CommandEngine commands(c);
        const auto preview = commands.preview(list);
        require(preview.ok && preview.changed && preview.changes.size() == 3 && preview.changes[2].contains("Alien Limiter") && preview.changes[2].contains("FX slot 3"), "Preview names each insert");
        require(alien::sameContent(empty, c.document()) && !c.canUndo(), "Preview leaves the project alone");
        require(commands.batch(list, mastering::undoLabel).changed && alien::sameContent(preview.after, c.document()) && c.undo(), "the batch matches Preview");
    }

    // Existing devices keep their slots, identities and state; the chain goes after them. Five plus three fills the rack.
    cmd::CommandEngine commands(c);
    for (int i = 0; i < 5; ++i) require(commands.execute(cmd::InsertDevice{RackStage::master, 0, i, deviceEffect("Mine " + juce::String(i + 1))}).changed, "existing master effects");
    require(commands.execute(cmd::SetDeviceBypass{RackStage::master, 0, 1, true}).changed, "one of them bypassed");
    const auto five = c.document();
    require(add().changed && names(master()) == "Mine 1,Mine 2,Mine 3,Mine 4,Mine 5," + chainNames, "the chain follows what was there");
    for (std::size_t i = 0; i < 5; ++i) require(master().effects[i] == five.masterPlugins.effects[i], "an existing device is untouched");
    require(static_cast<int>(master().effects.size()) == alien::device_edit::maxEffects && alien::toProjectData(c.document()).valid(), "a full master rack saves");

    // Not enough room refuses before anything is captured, and leaves the project and its history alone.
    const auto expectRefusal = [&](const char* fragment) {
        const auto before = c.document(); const auto history = historyOf(c);
        const int capturesBefore = captures, rebuildsBefore = rebuilds;
        const auto result = add();
        require(!result.ok && !result.changed && result.error.contains("three free Master FX slots") && result.error.contains(fragment), "too few free slots refuse");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history && captures == capturesBefore && rebuilds == rebuildsBefore, "a refusal changes nothing");
    };
    expectRefusal("none is");
    require(c.undo() && alien::sameContent(five, c.document()), "one Undo takes the chain off again, leaving the five");
    require(commands.execute(cmd::InsertDevice{RackStage::master, 0, 5, deviceEffect("Mine 6")}).changed, "six in the rack");
    expectRefusal("only two are");
    require(commands.execute(cmd::InsertDevice{RackStage::master, 0, 6, deviceEffect("Mine 7")}).changed, "seven in the rack");
    expectRefusal("only one is");
    require(mastering::refusal(empty).isEmpty() && mastering::refusal(five).isEmpty(), "three free slots are enough");

    // Only the master rack changes.
    require(c.undo() && c.undo() && add().changed, "chain after five again");
    {
        auto expected = five; expected.masterPlugins = master();
        require(alien::sameContent(expected, c.document()), "nothing but Master FX changed");
    }
    require(c.undo(), "chain off");

    // A busy project, recording and count-in all refuse, before any capture.
    c.setExternalBusy(true);
    {
        const auto before = c.document(); const int capturesBefore = captures;
        const auto busy = add();
        require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()) && captures == capturesBefore, "a busy project refuses");
    }
    c.setExternalBusy(false);
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        const auto before = c.document(); const auto history = historyOf(c); const int capturesBefore = captures;
        const auto during = add();
        require(!during.ok && during.error.contains("Stop recording") && alien::sameContent(before, c.document()) && historyOf(c) == history && captures == capturesBefore, "recording and count-in refuse");
        engine->recording.phase.store(alien::RecordControls::idle);
    }
    require(add().changed && c.undoLabel() == mastering::undoLabel, "and it works again afterwards");
    require(!alien::mastering::add(c).ok, "the plain form refuses a full rack too");
    {
        auto freshEngine = std::make_unique<ToneEngine>(); alien::ProjectController fresh(*freshEngine);
        fresh.pushDocumentToEngine(); fresh.markAsNewBaseline();
        require(mastering::add(fresh).changed && mastering::add(fresh).changed && names(fresh.document().masterPlugins) == chainNames + "," + chainNames, "a second chain fits after the first");
        require(!mastering::add(fresh).ok && fresh.undo() && fresh.undo() && !fresh.canUndo(), "a third does not, and each chain is its own Undo step");
    }
    std::cout << "PASS: mastering chain\n";
}
