// The plugin compatibility battery, run in-process against a plugin that misbehaves on request.
// Each test names one way a real plugin goes wrong and checks that the report says so, in the right
// step, without taking the test program down.
#include "PluginCheck.h"
#include "PluginPreflight.h"
#include "TestPlugin/TestPluginCore.h"
#include <cstring>
#include <iostream>

using namespace alien;
using alien::check::Outcome;
namespace tp = alien::testplugin;

static int failures = 0;
static void test(bool ok, const juce::String& what) { if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; } }

static juce::PluginDescription describe(const tp::Behaviour& b) {
    juce::PluginDescription d;
    d.name = "Alien Test Plugin"; d.pluginFormatName = "VST3"; d.fileOrIdentifier = "in-memory/AlienTestPlugin.vst3";
    d.manufacturerName = "Alien Studio tests"; d.version = "1.0"; d.category = b.instrument ? "Instrument" : "Fx";
    d.isInstrument = b.instrument; d.uniqueId = d.deprecatedUid = 4242;
    d.numInputChannels = b.instrument ? 0 : 2; d.numOutputChannels = 2;
    return d;
}
static PluginFactory factoryFor(const tp::Behaviour& b) {
    return [b](const juce::PluginDescription&, double, int, juce::String&) -> std::unique_ptr<juce::AudioProcessor> { return std::make_unique<tp::Processor>(b); };
}
static check::Report run(const juce::String& spec, bool quick = false, bool instrument = false) {
    auto b = tp::Behaviour::parse(spec); b.instrument = b.instrument || instrument;
    check::Options options; options.quick = quick; options.editor = false;
    return check::Battery(describe(b), factoryFor(b), options).run();
}
static const check::Step* find(const check::Report& r, const juce::String& startsWith) {
    for (const auto& s : r.steps) if (s.name.startsWith(startsWith)) return &s;
    return nullptr;
}
static juce::String outcomeOf(const check::Report& r, const juce::String& step) { const auto* s = find(r, step); return s ? juce::String(check::label(s->outcome)) : juce::String("(no such step)"); }
static const check::Step* firstOf(const check::Report& r, Outcome o) { for (const auto& s : r.steps) if (s.outcome == o) return &s; return nullptr; }
static juce::String describeAll(const check::Report& r) { return "\n" + r.toText(); }

static void healthy() {
    const auto fx = run("");
    test(fx.overall() == Outcome::pass && fx.verdict() == "COMPATIBLE", "a well-behaved effect is compatible" + describeAll(fx));
    test(fx.parameters == 3 && fx.programs == 1 && fx.hasEditor && fx.inputChannels == 2 && fx.outputChannels == 2, "the report records parameters, programs, editor and channels");
    test(fx.count(Outcome::skipped) == 1 && outcomeOf(fx, "Editor") == "SKIP", "with the editor step disabled it is skipped, not passed");
    test(find(fx, "Play at 44100 Hz / 64 samples") && find(fx, "Play at 96000 Hz / 512 samples") && find(fx, "Odd block sizes") && find(fx, "Latency and tail") && find(fx, "Two instances") && find(fx, "Parameter changes") && find(fx, "Damaged state") && find(fx, "Unload and reload") && find(fx, "Unload"), "the full battery covers every step");

    const auto synth = run("", false, true);
    test(synth.overall() == Outcome::pass && synth.instrument, "a well-behaved instrument is compatible" + describeAll(synth));
    test(synth.inputChannels == 0 && synth.outputChannels == 2, "an instrument has no inputs");

    const auto quick = run("", true);
    test(quick.overall() == Outcome::pass && quick.steps.size() == 5, "the quick check is five steps (got " + juce::String(static_cast<int>(quick.steps.size())) + ")" + describeAll(quick));
}

static void crashesAreContained() {
    {   const auto r = run("crash-load");
        test(r.overall() == Outcome::crash && r.verdict() == "CRASHED" && outcomeOf(r, "Load") == "CRASH", "a plugin that crashes in its constructor is reported as crashed in Load" + describeAll(r));
        test(find(r, "Load")->detail.containsIgnoreCase("access violation") || find(r, "Load")->detail.containsIgnoreCase("segmentation") || find(r, "Load")->detail.contains("Crashed"), "the crash is described: " + find(r, "Load")->detail);
        test(find(r, "Load")->detail.contains("while creating the plugin"), "and says it happened while creating the plugin: " + find(r, "Load")->detail);
        int notRun = 0; for (const auto& s : r.steps) notRun += s.outcome == Outcome::skipped && s.detail.startsWith("Not run") && s.detail.contains("crashed earlier (Load)") ? 1 : 0;
        test(notRun == static_cast<int>(r.steps.size()) - 2, "every later step says it was not run and why, instead of being silently absent (the other is the editor step, disabled for this run)" + describeAll(r)); }
    {   const auto r = run("throw-load");
        test(r.overall() == Outcome::fail && outcomeOf(r, "Load") == "FAIL" && find(r, "Load")->detail.contains("exception"), "an exception is a failure, not a crash" + describeAll(r));
        test(find(r, "Bus layout")->detail.contains("did not load") && r.count(Outcome::crash) == 0, "later steps say the plugin did not load" + describeAll(r)); }
    {   const auto r = run("crash-prepare");
        test(r.overall() == Outcome::crash && outcomeOf(r, "Load") == "PASS" && outcomeOf(r, "Bus layout") == "PASS" && outcomeOf(r, "Play at 44100 Hz / 64 samples") == "CRASH", "a crash in prepareToPlay is blamed on the first playing step" + describeAll(r));
        test(outcomeOf(r, "Play at 48000 Hz / 512 samples") == "SKIP" && find(r, "Play at 48000 Hz / 512 samples")->detail.contains("crashed earlier"), "after a crash nothing else is run on the damaged plugin" + describeAll(r)); }
    {   const auto r = run("crash-process=40");
        const auto* s = firstOf(r, Outcome::crash);
        test(s != nullptr && s->name.startsWith("Play at") && s->detail.startsWith("Crashed on the audio thread"), "a crash on the audio thread is reported against the step that was playing" + describeAll(r));
        bool laterAllSkipped = s != nullptr; bool after = false;
        for (const auto& step : r.steps) { if (after) laterAllSkipped = laterAllSkipped && step.outcome == Outcome::skipped; if (&step == s) after = true; }
        test(laterAllSkipped && after, "and nothing else is run on the damaged plugin afterwards" + describeAll(r)); }
    {   const auto r = run("fragile-truncated");
        test(outcomeOf(r, "State round trip") == "PASS" && outcomeOf(r, "Damaged state") == "CRASH" && find(r, "Damaged state")->detail.contains("Crashed"), "a plugin that chokes on a cut-short copy of its own state is caught, which random bytes would not find" + describeAll(r)); }
    {   const auto r = run("crash-process=5", false, true);
        test(r.overall() == Outcome::crash, "so is a crash in an instrument" + describeAll(r)); }
    {   const auto r = run("crash-save");
        test(outcomeOf(r, "State round trip") == "CRASH" && r.overall() == Outcome::crash, "a crash when saving state is reported" + describeAll(r)); }
    {   const auto r = run("crash-restore");
        test(outcomeOf(r, "State round trip") == "CRASH", "a crash when restoring state is reported" + describeAll(r)); }
    {   const auto r = run("fragile-state");
        test(outcomeOf(r, "State round trip") == "PASS" && outcomeOf(r, "Damaged state") == "CRASH", "a plugin that crashes on damaged state data is caught by that step, and only that one" + describeAll(r)); }
    {   const auto r = run("crash-unload");
        test(r.overall() == Outcome::crash && firstOf(r, Outcome::crash) != nullptr, "a crash when the plugin is destroyed is reported" + describeAll(r));
        const auto q = run("crash-unload", true);
        test(q.overall() == Outcome::crash, "also in the quick check" + describeAll(q)); }
}

static void misbehaviourIsReported() {
    {   const auto r = run("nan-process");
        test(r.overall() == Outcome::fail && firstOf(r, Outcome::fail)->detail.contains("NaN"), "NaN output is a failure" + describeAll(r));
        const auto late = run("nan-process=100");
        test(late.overall() == Outcome::fail, "even when it starts after a hundred blocks" + describeAll(late)); }
    {   const auto r = run("silent");
        test(r.overall() == Outcome::warn && firstOf(r, Outcome::warn)->detail.contains("Silent"), "a plugin that outputs nothing is a warning" + describeAll(r));
        const auto synth = run("silent", false, true);
        test(synth.overall() == Outcome::warn && firstOf(synth, Outcome::warn)->detail.contains("No sound"), "so is an instrument that makes no sound" + describeAll(synth)); }
    {   const auto r = run("forgets-state");
        test(outcomeOf(r, "State round trip") == "WARN" && find(r, "State round trip")->detail.contains("parameter(s) differ"), "settings that do not survive a save are a warning" + describeAll(r)); }
    {   const auto r = run("no-state");
        test(outcomeOf(r, "State round trip") == "WARN" && find(r, "State round trip")->detail.contains("saved nothing"), "a plugin that saves no state is a warning" + describeAll(r)); }
    {   const auto r = run("false-latency=256");
        test(outcomeOf(r, "Latency and tail") == "WARN" && find(r, "Latency and tail")->detail.contains("misaligned"), "latency that does not match the audio is a warning" + describeAll(r));
        test(r.latencySamples == 256, "the reported latency is recorded"); }
    {   const auto r = run("latency=128");
        test(outcomeOf(r, "Latency and tail") == "PASS" && find(r, "Latency and tail")->detail.contains("sample 128"), "honest latency passes and the impulse is seen where it was promised" + describeAll(r)); }
    {   const auto r = run("ring=2");
        const auto* s = find(r, "Latency and tail");
        test(s && s->outcome == Outcome::warn && s->detail.contains("longer than declared"), "a tail longer than declared is a warning" + describeAll(r));
        test(r.measuredTailSeconds > 1.2 && r.measuredTailSeconds < 2.1, "the measured tail is near the real one (" + juce::String(r.measuredTailSeconds, 2) + " s)");
        const auto honest = run("ring=2,tail=2.5");
        test(outcomeOf(honest, "Latency and tail") == "PASS", "declaring it makes it pass" + describeAll(honest)); }
    {   const auto r = run("sidechain");
        bool noted = false; for (const auto& n : r.notes) noted = noted || n.contains("Sidechain");
        test(r.overall() == Outcome::pass && noted, "a sidechain bus is recorded and the plugin still works with it switched off" + describeAll(r)); }
}

static void reportFormats() {
    const auto r = run("ring=2");
    auto xml = r.toXml(); check::Report back;
    test(xml != nullptr && check::Report::fromXml(*xml, back), "the report converts to XML and back");
    test(back.steps.size() == r.steps.size() && back.verdict() == r.verdict() && back.name == r.name && back.parameters == r.parameters && std::abs(back.measuredTailSeconds - r.measuredTailSeconds) < 1.0e-6, "nothing is lost on the way");
    bool same = true; for (std::size_t i = 0; i < r.steps.size() && i < back.steps.size(); ++i) same = same && r.steps[i].name == back.steps[i].name && r.steps[i].outcome == back.steps[i].outcome && r.steps[i].detail == back.steps[i].detail;
    test(same, "every step keeps its outcome and detail");
    const auto text = r.toText();
    test(text.contains("Result:  COMPATIBLE WITH WARNINGS") && text.contains("Alien Test Plugin") && text.contains("WARN"), "the text report leads to a verdict:\n" + text);
    juce::XmlElement wrong("Other"); check::Report none; test(!check::Report::fromXml(wrong, none), "an unrelated XML file is not a report");
}

static void progressIsReported() {
    std::vector<juce::String> began; std::vector<std::size_t> sizes;
    const auto b = tp::Behaviour::parse("");
    check::Options options; options.quick = true; options.editor = false;
    check::Battery battery(describe(b), factoryFor(b), options, [&](const juce::String& s) { began.push_back(s); }, [&](const check::Report& r) { sizes.push_back(r.steps.size()); });
    const auto r = battery.run();
    test(began.size() == r.steps.size() && began.front() == "Load" && began.back() == "Unload", "each step announces itself before it runs, so a crash can be attributed");
    bool growing = !sizes.empty(); for (std::size_t i = 0; i < sizes.size(); ++i) growing = growing && sizes[i] == i + 1;
    test(growing, "the partial report grows one step at a time");
}

static void editor() {
    if (!check::displayAvailable()) { std::cout << "(no display: editor step not exercised)\n"; return; }
    const auto b = tp::Behaviour::parse("");
    check::Options options; options.editor = true;
    const auto r = check::Battery(describe(b), factoryFor(b), options).run();
    test(outcomeOf(r, "Editor") == "PASS" && find(r, "Editor")->detail.contains("320 x 180"), "the editor is opened, resized and closed" + describeAll(r));
    for (const char* spec : {"crash-editor", "throw-editor"}) {
        const auto bad = tp::Behaviour::parse(spec);
        const auto rr = check::Battery(describe(bad), factoryFor(bad), options).run();
        test(outcomeOf(rr, "Editor") == (juce::String(spec) == "crash-editor" ? "CRASH" : "FAIL"), juce::String(spec) + " is reported by the editor step" + describeAll(rr));
        test(rr.count(Outcome::pass) >= 10, "and everything before it still passed" + describeAll(rr));
    }
}

// ---- the safety check before a plugin is loaded into a project ---------------------------------------------------
static check::Report reportWith(Outcome o, const juce::String& step = "Play at 48000 Hz / 512 samples", const juce::String& detail = "something", bool hung = false) {
    check::Report r; r.name = "Fake";
    r.steps.push_back({"Load", Outcome::pass, {}, 1});
    r.steps.push_back({step, o, detail, 1});
    r.hung = hung;
    return r;
}
static juce::PluginDescription fileDescription(const juce::File& bundle, const juce::String& name = "Fake Plugin") {
    juce::PluginDescription d; d.name = name; d.pluginFormatName = "VST3"; d.fileOrIdentifier = bundle.getFullPathName(); d.uniqueId = d.deprecatedUid = 77; return d;
}

// How a set of step outcomes becomes a verdict, and which verdicts count as acceptable (exit code 0 in the tool).
static void verdicts() {
    const auto make = [](std::initializer_list<Outcome> outcomes) { check::Report r; int i = 0; for (auto o : outcomes) r.steps.push_back({"step " + juce::String(++i), o, {}, 1}); return r; };
    const auto crashAndFail = make({Outcome::pass, Outcome::fail, Outcome::crash, Outcome::warn});
    test(crashAndFail.overall() == Outcome::crash && crashAndFail.verdict() == "CRASHED" && !crashAndFail.acceptable(), "a crash outranks a failure: " + crashAndFail.verdict());
    const auto failAndWarn = make({Outcome::warn, Outcome::fail, Outcome::pass});
    test(failAndWarn.overall() == Outcome::fail && failAndWarn.verdict() == "NOT COMPATIBLE" && !failAndWarn.acceptable(), "a failure outranks a warning");
    const auto warned = make({Outcome::pass, Outcome::warn, Outcome::skipped});
    test(warned.overall() == Outcome::warn && warned.verdict() == "COMPATIBLE WITH WARNINGS" && warned.acceptable(), "warnings are acceptable");
    const auto clean = make({Outcome::pass, Outcome::skipped});
    test(clean.overall() == Outcome::pass && clean.verdict() == "COMPATIBLE" && clean.acceptable(), "a clean run is compatible");
    const auto nothing = make({Outcome::skipped, Outcome::skipped});
    test(nothing.overall() == Outcome::skipped && nothing.verdict() == "NOT RUN" && !nothing.acceptable(), "a report in which nothing ran is not called compatible: " + nothing.verdict());
}

// The damaged copies of saved state that the battery feeds a plugin.
static void damagedStates() {
    juce::MemoryBlock real(16); for (int i = 0; i < 16; ++i) static_cast<unsigned char*>(real.getData())[i] = static_cast<unsigned char>(0x41 + i);
    const juce::MemoryBlock untouched(real);
    juce::Random random(99);
    const auto variants = check::Battery::damagedVariants(real, random);
    test(variants.size() == 13, "5 random blocks, 5 truncations and 3 corruptions (got " + juce::String(static_cast<int>(variants.size())) + ")");
    test(real == untouched, "the real state is not changed while making copies");
    if (variants.size() == 13) {
        const std::size_t randomSizes[] = {0, 1, 7, 64, 4096}, cut[] = {15, 12, 8, 4, 1};
        bool sizesRight = true;
        for (int i = 0; i < 5; ++i) sizesRight = sizesRight && variants[static_cast<std::size_t>(i)].getSize() == randomSizes[i] && variants[static_cast<std::size_t>(5 + i)].getSize() == cut[i];
        test(sizesRight, "the random blocks and truncations have the sizes promised");
        bool prefixes = true; for (int i = 5; i < 10; ++i) prefixes = prefixes && std::memcmp(variants[static_cast<std::size_t>(i)].getData(), real.getData(), variants[static_cast<std::size_t>(i)].getSize()) == 0;
        test(prefixes, "a truncation is the start of the real state, so it still looks like it");
        bool overwritten = true;
        for (int i = 10; i < 13; ++i) {
            const auto& v = variants[static_cast<std::size_t>(i)]; int differing = 0;
            if (v.getSize() != real.getSize()) { overwritten = false; continue; }
            for (std::size_t k = 0; k < v.getSize(); ++k) differing += static_cast<const unsigned char*>(v.getData())[k] != static_cast<const unsigned char*>(real.getData())[k];
            overwritten = overwritten && differing >= 1 && differing <= 8;
        }
        test(overwritten, "a corruption has the real size and differs in between 1 and 8 bytes");
    }
    juce::MemoryBlock tiny(3); juce::Random again(99);
    test(check::Battery::damagedVariants(tiny, again).size() == 5, "a state too small to cut up gets only the random blocks");
}

static void preflight() {
    juce::String why;
    test(!Preflight::blocks(reportWith(Outcome::pass), why) && !Preflight::blocks(reportWith(Outcome::warn), why), "passing and warning plugins are not blocked");
    test(!Preflight::blocks(reportWith(Outcome::fail, "Play at 48000 Hz / 512 samples", "Audio: 9 samples were NaN"), why), "NaN or odd behaviour is not a reason to refuse: the host handles it while playing");
    test(Preflight::blocks(reportWith(Outcome::crash, "State round trip", "Crashed: access violation"), why) && why.contains("State round trip") && why.contains("access violation"), "a crash blocks, and the reason names the step: " + why);
    test(Preflight::blocks(reportWith(Outcome::fail, "Play at 48000 Hz / 512 samples", "Hung: no result after 60 s.", true), why) && why.contains("stopped responding"), "a hang blocks: " + why);

    auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-preflight", "", false); tmp.createDirectory();
    const auto bundle = tmp.getChildFile("Fake.vst3"); bundle.replaceWithText("version one");
    const auto cache = tmp.getChildFile("PluginPreflight.xml");
    int calls = 0; Outcome next = Outcome::pass; bool nextHung = false;
    const Preflight::Checker checker = [&](const juce::PluginDescription&) { ++calls; return reportWith(next, "Play at 48000 Hz / 512 samples", next == Outcome::crash ? "Crashed: access violation" : "fine", nextHung); };
    const auto description = fileDescription(bundle);
    {
        Preflight pre(cache, checker);
        test(pre.verify(description).isEmpty() && calls == 1, "a plugin seen for the first time is checked and allowed");
        test(pre.verify(description).isEmpty() && calls == 1, "and not checked again while the file is unchanged");
        bundle.replaceWithText("version two, which is longer");
        next = Outcome::crash;
        const auto message = pre.verify(description);
        test(calls == 2 && message.contains("Fake Plugin was not loaded") && message.contains("crashed") && message.contains("settings are kept"), "an updated file is checked again, and a crash gives a message that says the settings are kept: " + message);
        test(pre.verify(description) == message && calls == 2, "a blocked plugin stays blocked without running the check again");
        next = Outcome::pass;
        test(pre.verify(description).isNotEmpty() && calls == 2, "even if it would now pass: the verdict is for this file");
        pre.forget(description);
        test(pre.verify(description).isEmpty() && calls == 3, "forgetting a verdict makes the next load check again");
    }
    {   // Verdicts survive a restart.
        Preflight again(cache, checker);
        test(again.verify(description).isEmpty() && calls == 3 && again.size() == 1, "an allowed verdict is remembered across restarts");
        bundle.replaceWithText("version three, longer still than before");
        next = Outcome::crash; again.verify(description);
        Preflight third(cache, checker);
        test(third.verify(description).isNotEmpty() && calls == 4, "a blocked verdict is remembered too");
        const auto other = tmp.getChildFile("Other.vst3"); other.replaceWithText("x"); next = Outcome::pass;
        test(third.verify(fileDescription(other, "Other")).isEmpty() && calls == 5, "other plugins are unaffected");
        test(third.forgetBlocked() == 1, "Re-check blocked plugins forgets exactly the blocked verdicts");
        test(third.find(other) != nullptr && third.find(other)->allowed, "and leaves the allowed ones alone");
        test(third.verify(description).isEmpty() && calls == 6, "the forgotten plugin is checked again (and now passes)");
    }
    {   // Hang.
        const auto hangs = tmp.getChildFile("Hangs.vst3"); hangs.replaceWithText("h");
        Preflight pre({}, checker); next = Outcome::fail; nextHung = true;
        const auto message = pre.verify(fileDescription(hangs, "Hangs"));
        test(message.contains("Hangs was not loaded") && message.contains("stopped responding"), "a plugin that hangs is not loaded: " + message);
        nextHung = false;
    }
    {   // Cases that need no check.
        const int before = calls;
        Preflight pre({}, checker);
        juce::PluginDescription stock = fileDescription(bundle); stock.pluginFormatName = "AlienStock";
        test(pre.verify(stock).isEmpty() && pre.verify(fileDescription(tmp.getChildFile("Missing.vst3"))).isEmpty() && calls == before, "stock effects and missing files are not checked (the loader reports a missing file itself)");
        const auto fresh = tmp.getChildFile("Fresh.vst3"); fresh.replaceWithText("never seen before"); next = Outcome::crash;
        test(pre.verify(fileDescription(fresh, "Fresh")).isNotEmpty() && calls == before + 1, "(control) a file that exists and has never been seen is checked");
        const auto fresher = tmp.getChildFile("Fresher.vst3"); fresher.replaceWithText("also never seen");
        pre.setEnabled(false);
        test(!pre.enabled() && pre.verify(fileDescription(fresher, "Fresher")).isEmpty() && calls == before + 1, "when switched off nothing is checked, even a plugin that would be blocked");
        pre.setEnabled(true); next = Outcome::pass;
        Preflight reloaded(cache, checker);
        test(reloaded.enabled(), "the cache file from before still has it on");
        Preflight off(cache, checker); off.setEnabled(false);
        Preflight offAgain(cache, checker);
        test(!offAgain.enabled(), "switching it off is remembered");
    }
    {   // A damaged cache file is ignored.
        cache.replaceWithText("<<< not xml");
        Preflight pre(cache, checker);
        test(pre.size() == 0 && pre.enabled(), "a damaged cache file is ignored, not fatal");
    }
    tmp.deleteRecursively();
}

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    healthy(); crashesAreContained(); misbehaviourIsReported(); reportFormats(); progressIsReported(); verdicts(); damagedStates(); preflight(); editor();
    if (failures > 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS: the compatibility battery passes good plugins and names the step of crashes, NaN, bad latency, lost state and editor failures\n";
    return 0;
}
