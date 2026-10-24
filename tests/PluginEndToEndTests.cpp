// The whole path with a real plugin binary: a genuine VST3 bundle (AlienTestPlugin, built beside the
// tests) that crashes, hangs, emits NaN or kills its own thread on request, driven through the
// scanner, the check tool, the isolated check and the host's plugin loader and audio runtime.
//
// Arguments (CTest supplies them): the AlienPluginCheck executable, which is also the helper process for
// scans and checks, and the AlienTestPlugin.vst3 bundle.
#include "PluginCheck.h"
#include "PluginPreflight.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace alien;
using alien::check::Outcome;

static int failures = 0, checksRun = 0;
static void test(bool ok, const juce::String& what) { ++checksRun; if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; } }

static void setBehaviour(const juce::String& spec) {
   #if JUCE_WINDOWS
    _putenv_s("ALIEN_TEST_PLUGIN", spec.toRawUTF8());
   #else
    setenv("ALIEN_TEST_PLUGIN", spec.toRawUTF8(), 1);
   #endif
}
static juce::String describeAll(const check::Report& r) { return "\n" + r.toText(); }
static const check::Step* find(const check::Report& r, const juce::String& startsWith) { for (const auto& s : r.steps) if (s.name.startsWith(startsWith)) return &s; return nullptr; }
static const check::Step* firstOf(const check::Report& r, Outcome o) { for (const auto& s : r.steps) if (s.outcome == o) return &s; return nullptr; }

struct Fixture {
    juce::File tool, bundle, slowBundle, scratch;   // slowBundle: the same plugin without moduleinfo.json, so scanning has to load it (as it does for most plugins)
    juce::PluginDescription description;
};

static scan::ChildResult scanOnce(const Fixture& f, const juce::String& behaviour, int timeoutMs, scan::Verdict* verdict = nullptr, juce::PluginDescription* description = nullptr, bool fastPath = false) {
    setBehaviour(behaviour);
    scan::ChildRequest request; request.executable = f.tool; request.bundle = (fastPath ? f.bundle : f.slowBundle).getFullPathName(); request.timeoutMs = timeoutMs;
    request.output = f.scratch.getNonexistentChildFile("scan", ".xml", false);
    const auto result = scan::runChildProcess(request);
    juce::KnownPluginList types; if (auto xml = juce::parseXML(result.xml)) types.recreateFromXml(*xml);
    if (description != nullptr && types.getNumTypes() > 0) *description = *types.getTypes().begin();
    if (verdict != nullptr) *verdict = scan::classify(result, types.getNumTypes(), timeoutMs);
    return result;
}
static check::Report checkOnce(const Fixture& f, const juce::String& behaviour, bool quick = false, int timeoutMs = 120000) {
    setBehaviour(behaviour);
    check::IsolatedOptions isolated; isolated.executable = f.tool; isolated.scratch = f.scratch; isolated.timeoutMs = timeoutMs; isolated.battery.quick = quick; isolated.battery.editor = false;
    return check::checkIsolated(f.description, isolated);
}
struct CliResult { int exitCode{-1}; juce::String output; };
static CliResult runTool(const Fixture& f, const juce::StringArray& arguments, const juce::String& behaviour) {
    setBehaviour(behaviour);
    juce::StringArray command{f.tool.getFullPathName()}; command.addArray(arguments);
    juce::ChildProcess process; CliResult r;
    if (!process.start(command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return r;
    r.output = process.readAllProcessOutput();
    // The output ends when the process closes its pipe, which can be a moment before it has exited: the exit code
    // read at that instant can still be 0.
    if (!process.waitForProcessToFinish(120000)) { process.kill(); return r; }
    r.exitCode = static_cast<int>(process.getExitCode());
    return r;
}

static void scanning(Fixture& f) {
    scan::Verdict v; const auto r = scanOnce(f, "", 60000, &v, &f.description);
    test(v.status == scan::Status::ok && r.stage == "done", "the real bundle scans: " + v.detail + " (stage " + r.stage + ")");
    test(f.description.name == "Alien Test Plugin" && f.description.pluginFormatName == "VST3" && !f.description.isInstrument && f.description.numOutputChannels == 2, "its description is read by loading it: " + f.description.name + ", " + f.description.pluginFormatName + ", " + juce::String(f.description.numInputChannels) + " in " + juce::String(f.description.numOutputChannels) + " out");

    scan::Verdict crash; scanOnce(f, "crash-load", 60000, &crash);
    test(crash.status == scan::Status::crashed, "a plugin that crashes while loading is a crashed scan, not an empty one: " + crash.detail);
    scan::Verdict hang; const auto started = juce::Time::getMillisecondCounter(); scanOnce(f, "hang-load", 4000, &hang);
    test(hang.status == scan::Status::timedOut && juce::Time::getMillisecondCounter() - started < 30000, "a plugin that hangs while loading is stopped at the limit: " + hang.detail);
    scan::Verdict thrown; scanOnce(f, "throw-load", 60000, &thrown);
    test(thrown.status != scan::Status::ok && thrown.detail.isNotEmpty(), "a plugin that throws while loading does not scan as fine: " + juce::String(scan::statusLabel(thrown.status)) + " " + thrown.detail);
    scan::Verdict again; scanOnce(f, "", 60000, &again);
    test(again.status == scan::Status::ok, "and the same file scans fine again afterwards");
    // A plugin that ships a module manifest is scanned without being loaded at all, so a scan cannot find out that it crashes.
    // That is what the safety check before loading is for.
    scan::Verdict fast; juce::PluginDescription fastDescription; scanOnce(f, "crash-load", 60000, &fast, &fastDescription, true);
    test(fast.status == scan::Status::ok && fastDescription.name == "Alien Test Plugin", "a plugin with a module manifest scans fine even though it crashes when created (it is not created): " + fast.detail);

    {   // A damaged file that is not a library at all. Windows answers loading it with a "Bad Image" dialog unless the process
        // has turned such dialogs off, and a dialog nobody clicks holds the scan until its time limit. The helper turns them off
        // itself, so this test starts it with the system's default setting (dialogs shown), as Alien Studio does, whatever
        // setting the test was started with.
       #if JUCE_WINDOWS
        const auto previous = SetErrorMode(0);
       #endif
        const auto broken = f.scratch.getChildFile("damaged").getChildFile("Broken.vst3");
        broken.getParentDirectory().createDirectory();
        broken.replaceWithText("this is not a plugin");
        setBehaviour("");
        scan::ChildRequest request; request.executable = f.tool; request.bundle = broken.getFullPathName(); request.timeoutMs = 30000;
        request.output = f.scratch.getNonexistentChildFile("scan", ".xml", false);
        const auto started = juce::Time::getMillisecondCounter();
        const auto result = scan::runChildProcess(request);
        const auto elapsed = juce::Time::getMillisecondCounter() - started;
        const auto verdict = scan::classify(result, 0, request.timeoutMs);
       #if JUCE_WINDOWS
        SetErrorMode(previous);
       #endif
        test(verdict.status == scan::Status::incompatible && elapsed < 15000,
             "a damaged file is reported as incompatible at once, with no system error dialog holding the scan: "
             + juce::String(scan::statusLabel(verdict.status)) + " after " + juce::String(elapsed) + " ms, " + verdict.detail);
       #if JUCE_WINDOWS
        test((scan::quietSystemErrorDialogs() & SEM_FAILCRITICALERRORS) != 0, "turning the dialogs off sticks");
        SetErrorMode(previous);
       #endif
    }
}

static void checking(Fixture& f) {
    {   const auto r = checkOnce(f, "");
        test(r.acceptable() && r.steps.size() >= 15 && r.count(Outcome::crash) == 0 && r.count(Outcome::fail) == 0, "a healthy real plugin passes the whole battery in its own process" + describeAll(r));
        test(r.name == "Alien Test Plugin" && r.parameters >= 3 && r.hasEditor && r.outputChannels == 2, "its details come through from the child process" + describeAll(r));
        test(find(r, "State round trip") && find(r, "State round trip")->outcome == Outcome::pass, "its settings survive a save and restore into a second instance (the battery plays a few blocks first so the real wrapper has seen them, and sets switches to a valid position)" + describeAll(r)); }
    {   const auto r = checkOnce(f, "latency=64");
        test(r.latencySamples == 64 && find(r, "Latency and tail") && find(r, "Latency and tail")->outcome == Outcome::pass, "reported latency is read through the real VST3 wrapper and matches the audio" + describeAll(r)); }
    {   const auto r = checkOnce(f, "false-latency=300");
        test(find(r, "Latency and tail") && find(r, "Latency and tail")->outcome == Outcome::warn, "a lie about latency is caught through the real wrapper" + describeAll(r)); }
    {   const auto r = checkOnce(f, "nan-process=10");
        test(r.overall() == Outcome::fail && firstOf(r, Outcome::fail)->detail.contains("NaN"), "NaN from a real plugin is a failure" + describeAll(r)); }
    {   const auto r = checkOnce(f, "crash-process=20");
        const auto* s = firstOf(r, Outcome::crash);
        test(r.overall() == Outcome::crash && s != nullptr && s->name.startsWith("Play at") && find(r, "Unload")->outcome == Outcome::skipped, "a crash on the audio thread is caught inside the check, and the rest is skipped" + describeAll(r)); }
    {   const auto r = checkOnce(f, "crash-save");
        test(r.overall() == Outcome::crash && r.verdict() == "CRASHED" && firstOf(r, Outcome::crash) != nullptr, "a crash when saving state is caught, whichever step the real wrapper saves it in" + describeAll(r)); }
    {   // The real VST3 wrapper creates the plugin's editor when asked whether it has one, so an editor that crashes shows up while loading:
        // the report must say so, instead of blaming the constructor.
        const auto r = checkOnce(f, "crash-editor", false);
        const auto* s = firstOf(r, Outcome::crash);
        test(r.overall() == Outcome::crash && s != nullptr && s->name == "Load" && s->detail.contains("asking the plugin for its name, editor"), "an editor that crashes is reported as such, not as a crash in the constructor" + describeAll(r)); }
    {   const auto r = checkOnce(f, "crash-restore", false);
        const auto* s = firstOf(r, Outcome::crash);
        test(r.overall() == Outcome::crash && s != nullptr && find(r, "Play at 44100 Hz / 64 samples")->outcome == Outcome::pass, "a crash when restoring state is caught, and the playing steps before it passed" + describeAll(r)); }
    {   const auto r = checkOnce(f, "fragile-state", false);
        // The JUCE VST3 wrapper checks the saved-state container before the plugin sees it, so garbage cannot reach this plugin's own parser
        // through the real wrapper; the in-process PluginCheckTests cover that code path. Here: the step runs and reports what it fed.
        const auto* damaged = find(r, "Damaged state");
        test(damaged != nullptr && damaged->outcome != Outcome::fail && damaged->detail.contains("damaged states"), "the damaged-state step runs against a real plugin" + describeAll(r)); }
    // What the in-process guard can never catch: the process dies, and the report still arrives.
    {   const auto r = checkOnce(f, "crash-thread=5");
        const auto* s = firstOf(r, Outcome::crash);
        test(r.overall() == Outcome::crash && s != nullptr && s->name.startsWith("Play at") && (s->detail.contains("ended without finishing") || s->detail.contains("The process crashed (exit code")), "a crash on a thread the plugin started ends the check process, and the report says which step it died in" + describeAll(r));
        test(find(r, "Load")->outcome == Outcome::pass && find(r, "Bus layout")->outcome == Outcome::pass, "with the steps that finished before it kept" + describeAll(r)); }
    {   const auto started = juce::Time::getMillisecondCounter();
        const auto r = checkOnce(f, "hang-process=10", false, 8000);
        const auto elapsed = juce::Time::getMillisecondCounter() - started;
        test(r.overall() != Outcome::pass && r.overall() != Outcome::warn && r.firstProblem().contains("Hung") && elapsed < 60000, "a plugin that hangs is stopped after the time limit and reported (" + juce::String(elapsed) + " ms)" + describeAll(r)); }
    {   const auto r = checkOnce(f, "", true);
        test(r.acceptable() && r.steps.size() == 5, "the quick check, as used before a project loads a plugin, is five steps" + describeAll(r)); }
    {   // Checks that start together (a scan beside a project load, two copies of the app, two runs of the tool) share the temp folder.
        // Each must get its own answer: they used to pick the same result file, delete each other's progress markers and report
        // healthy plugins as crashed.
        setBehaviour("");
        std::vector<check::Report> reports(4); std::vector<std::thread> threads;
        for (auto& report : reports)
            threads.emplace_back([&f, &report] {
                check::IsolatedOptions isolated; isolated.executable = f.tool; isolated.scratch = f.scratch; isolated.timeoutMs = 120000; isolated.battery.quick = true; isolated.battery.editor = false;
                report = check::checkIsolated(f.description, isolated); });
        for (auto& t : threads) t.join();
        int healthy = 0; for (const auto& r : reports) if (r.acceptable() && r.count(Outcome::crash) == 0 && r.steps.size() == 5) ++healthy;
        test(healthy == 4, "four checks started at the same moment each get their own, correct answer (" + juce::String(healthy) + " of 4 did)" + describeAll(reports.front()));
        int leftovers = 0; for (const auto& entry : juce::RangedDirectoryIterator(f.scratch, false, "AlienPluginCheck-*")) { (void) entry; ++leftovers; }
        test(leftovers == 0, "and none of them leaves its working files behind (" + juce::String(leftovers) + " left)"); }
}

// The safety check before loading, against the real plugin: only what could take the session down is refused.
static void safetyCheck(Fixture& f) {
    // Returns the reason the plugin was refused, or an empty string if it was allowed.
    const auto reason = [&](const juce::String& behaviour, int timeoutMs, bool fastPath) {
        juce::PluginDescription description = f.description;
        if (fastPath) { scan::Verdict v; scanOnce(f, "", 60000, &v, &description, true); }   // scan first: the scan sets the behaviour itself
        setBehaviour(behaviour);
        Preflight preflight({}, isolatedPreflightChecker(f.tool, timeoutMs));
        return preflight.verify(description);
    };
    {   const auto why = reason("", 60000, false);
        test(why.isEmpty(), "a healthy plugin is allowed: " + why); }
    {   const auto why = reason("nan-process=5", 60000, false);
        test(why.isEmpty(), "one that returns NaN is allowed, because the host handles that while playing: " + why); }
    {   const auto why = reason("crash-process=5", 60000, false);
        test(why.contains("crashed during the safety check") && why.contains("Play at"), "one that crashes while playing is refused, and the step is named: " + why); }
    {   const auto why = reason("crash-load", 60000, true);
        test(why.contains("crashed during the safety check") && why.contains("Load"), "one that crashes when created is refused even though its scan said nothing: " + why); }
    {   const auto why = reason("crash-thread=3", 60000, false);
        test(why.contains("crashed"), "one whose own thread crashes is refused: the check process dies, not the session: " + why); }
    {   const auto started = juce::Time::getMillisecondCounter();
        const auto why = reason("hang-process=2", 8000, false);
        test(why.contains("stopped responding") && juce::Time::getMillisecondCounter() - started < 60000, "one that hangs is refused after the time limit: " + why); }
    {   const auto why = reason("crash-save", 60000, false);
        test(why.isNotEmpty(), "one that crashes when saving state is refused: " + why); }
    setBehaviour("");
}

static void commandLine(Fixture& f) {
    const auto out = f.scratch.getChildFile("cli-report");
    {   const auto r = runTool(f, {"check", f.bundle.getFullPathName(), "--quick", "--no-editor", "--out", out.getFullPathName()}, "");
        const auto report = out.getChildFile("plugin-check-report.txt").loadFileAsString();
        test(r.exitCode == 0 && report.contains("COMPATIBLE") && report.contains("Alien Test Plugin") && out.getChildFile("plugin-check-report.xml").existsAsFile() && out.getChildFile("plugin-scan.log").existsAsFile(), "the tool checks a plugin, exits 0 and writes its reports (exit " + juce::String(r.exitCode) + ")\n" + r.output);
        auto xml = juce::parseXML(out.getChildFile("plugin-check-report.xml"));
        test(xml != nullptr && xml->getTagName() == "PluginChecks" && xml->getNumChildElements() == 1, "the XML report is well formed"); }
    {   const auto crashing = f.scratch.getChildFile("cli-crash");
        const auto r = runTool(f, {"check", f.bundle.getFullPathName(), "--quick", "--no-editor", "--out", crashing.getFullPathName()}, "crash-process=2");
        const auto report = crashing.getChildFile("plugin-check-report.txt").loadFileAsString();
        test(r.exitCode == 1 && report.contains("CRASHED") && report.contains("SUMMARY"), "a crashing plugin gives exit code 1 and a CRASHED line in the summary (exit " + juce::String(r.exitCode) + ")\n" + r.output); }
    {   // A folder tree: the plugin nested two levels down, a file that is not a plugin at all, and a bundle-like folder to be left alone.
        const auto root = f.scratch.getChildFile("tree");
        const auto nested = root.getChildFile("Vendor").getChildFile("Effects"); nested.createDirectory();
        test(f.bundle.copyDirectoryTo(nested.getChildFile(f.bundle.getFileName())), "(test setup) the plugin bundle copies");   // same name: the binary inside must match its bundle
        root.getChildFile("Broken.vst3").replaceWithText("this is not a plugin");
        const auto folderOut = f.scratch.getChildFile("cli-folder");
        const auto r = runTool(f, {"check", root.getFullPathName(), "--quick", "--no-editor", "--out", folderOut.getFullPathName()}, "");
        const auto report = folderOut.getChildFile("plugin-check-report.txt").loadFileAsString();
        const auto log = folderOut.getChildFile("plugin-scan.log").loadFileAsString();
        test(report.contains("Alien Test Plugin") && report.contains("Broken") && log.contains("Effects") && log.contains("Broken.vst3"), "a folder is searched recursively, and both the good and the broken plugin are accounted for\n" + r.output + "\n" + log);
        bool pluginPassed = false, brokenListed = false;
        for (const auto& line : juce::StringArray::fromLines(report)) {
            if (line.startsWith("COMPATIBLE") && line.contains("Alien Test Plugin")) pluginPassed = true;
            if (line.startsWith("NOT COMPATIBLE") && line.contains("Broken")) brokenListed = true;
        }
        test(pluginPassed && brokenListed, "the plugin found two levels down is tested and passes, and the broken file is listed as not compatible\n" + report);
        test(r.exitCode == 1, "the broken file makes the exit code 1 (exit " + juce::String(r.exitCode) + ")");
        const auto filtered = f.scratch.getChildFile("cli-filter");
        const auto r2 = runTool(f, {"check", root.getFullPathName(), "--quick", "--no-editor", "--name", "Alien", "--out", filtered.getFullPathName()}, "");
        const auto filteredReport = filtered.getChildFile("plugin-check-report.txt").loadFileAsString();
        test(r2.exitCode == 0 && filteredReport.contains("Alien Test Plugin") && !filteredReport.contains("Broken"), "--name tests only the plugins whose name matches, so the broken file is left out and the exit code is 0 (exit " + juce::String(r2.exitCode) + ")\n" + r2.output);
        const auto r3 = runTool(f, {"check", root.getFullPathName(), "--quick", "--no-editor", "--name", "Zebra", "--out", f.scratch.getChildFile("cli-none").getFullPathName()}, "");
        test(r3.exitCode == 3 && r3.output.contains("No plugin matching \"Zebra\""), "a name that matches nothing says so and exits 3 instead of reporting success (exit " + juce::String(r3.exitCode) + ")\n" + r3.output); }
    {   // Warnings are not failures: the tool exits 0 and says so.
        const auto warned = f.scratch.getChildFile("cli-warn");
        const auto r = runTool(f, {"check", f.bundle.getFullPathName(), "--no-editor", "--out", warned.getFullPathName()}, "false-latency=300");
        const auto report = warned.getChildFile("plugin-check-report.txt").loadFileAsString();
        test(r.exitCode == 0 && report.contains("COMPATIBLE WITH WARNINGS"), "a plugin with only warnings still gives exit code 0 (exit " + juce::String(r.exitCode) + ")\n" + r.output); }
    {   // Nothing to find.
        const auto empty = f.scratch.getChildFile("empty-folder"); empty.createDirectory();
        const auto r = runTool(f, {"check", empty.getFullPathName(), "--out", f.scratch.getChildFile("cli-empty").getFullPathName()}, "");
        test(r.exitCode == 3 && r.output.contains("No VST3 plugins were found"), "a folder with no plugins says so and exits 3 (exit " + juce::String(r.exitCode) + ")\n" + r.output); }
    {   const auto r = runTool(f, {"check", "--bogus"}, ""); test(r.exitCode == 2, "unknown options are refused with exit code 2 (exit " + juce::String(r.exitCode) + ")");
        const auto r2 = runTool(f, {"check", f.scratch.getChildFile("does-not-exist.vst3").getFullPathName()}, ""); test(r2.exitCode == 2, "a path that does not exist is refused (exit " + juce::String(r2.exitCode) + ")");
        const auto r3 = runTool(f, {}, ""); test(r3.exitCode == 0 && r3.output.contains("AlienPluginCheck"), "no arguments prints the help"); }
}

// The host itself, in this process, with the real plugin binary behind it.
static void hostWithRealPlugin(Fixture& f) {
    const PluginState state{f.description.createXml()->toString(), {}, false};
    {   setBehaviour("");
        juce::String error;
        auto plugin = instantiatePlugin(state, false, 48000.0, error);
        test(plugin != nullptr && error.isEmpty(), "the real plugin loads through the host's loader: " + error);
        if (plugin) { plugin->prepareToPlay(48000.0, 512); plugin->releaseResources(); plugin.reset(); } }
    {   setBehaviour("crash-load");
        juce::String error; fault::Fault fault;
        auto plugin = instantiatePlugin(state, false, 48000.0, error, &createVst3Instance, &fault);
        test(plugin == nullptr && error.contains("crashed while loading") && fault.occurred(), "a plugin that crashes while the host loads it is contained: " + error); }
    {   setBehaviour("throw-load");
        juce::String error;
        auto plugin = instantiatePlugin(state, false, 48000.0, error);
        test(plugin == nullptr && error.isNotEmpty(), "and so is one that throws: " + error); }
    {   // Audio: a real plugin that starts crashing after 30 blocks is switched off, the track hears its input, and the host carries on.
        setBehaviour("crash-process=30");
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, f.scratch.getChildFile("host.log"));
        std::vector<PluginRackState> racks(1); racks[0].effects = {state}; PluginRackState master;
        const auto errors = host.restore(racks, master, 48000.0);
        auto& slot = runtime->racks[0].effects[0];
        test(errors.isEmpty() && slot.processor != nullptr && !slot.faulted.get(), "the real plugin is in the rack: " + errors);
        bool processedFirst = true, passedThrough = true;
        for (int block = 0; block < 60; ++block) {
            runtime->begin(64, 120, true, 0);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 64; ++i) runtime->tracks[0].setSample(c, i, 0.5f);
            runtime->effectsAndAlign(64);
            const float heard = runtime->tracks[0].getSample(0, 10);
            if (block < 30 && std::abs(heard - 0.375f) > 1.0e-4f) processedFirst = false;      // the plugin really ran: gain 1, tone 0.5, mix 1
            if (block >= 30 && std::abs(heard - 0.5f) > 1.0e-6f) passedThrough = false;
        }
        test(processedFirst, "until it failed, the real plugin processed the audio");
        test(slot.faulted.get() && runtime->faultCount.load() == 1, "it was switched off when it crashed (faults: " + juce::String(static_cast<int>(runtime->faultCount.load())) + ")");
        test(passedThrough, "from then on the track carries its unprocessed signal");
        host.poll();
        const auto log = host.diagnosticLog().read();
        test(log.contains("Alien Test Plugin") && log.contains("processing audio"), "the failure is logged with the plugin's name and what it was doing:\n" + log);
        test(host.health(0, 0).isNotEmpty(), "and the panel can show that it is off");
        const auto before = PluginRuntime::quarantinedCount();
        host.restore(racks, master, 48000.0);
        test(PluginRuntime::quarantinedCount() == before + 1, "reloading the project parks the crashed instance rather than destroying it"); }
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    Fixture f;
    if (argc != 3) { std::cerr << "usage: PluginEndToEndTests <AlienPluginCheck executable> <AlienTestPlugin.vst3>\n"; return 1; }
    f.tool = juce::File(argv[1]);
    f.bundle = juce::File(argv[2]);
    if (!f.tool.existsAsFile() || !f.bundle.exists()) { std::cerr << "FAILED: the tool (" << f.tool.getFullPathName() << ") and the plugin (" << f.bundle.getFullPathName() << ") must both exist\n"; return 1; }
    // A name no other run can share: getNonexistentChildFile is not atomic, so two runs started together
    // would pick the same folder and delete it from under each other.
    f.scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("alien-e2e-" + juce::Uuid().toDashedString()); f.scratch.createDirectory();
    f.slowBundle = f.scratch.getChildFile("slow").getChildFile(f.bundle.getFileName()); f.slowBundle.getParentDirectory().createDirectory();
    if (!f.bundle.copyDirectoryTo(f.slowBundle)) { std::cerr << "FAILED: could not copy the plugin bundle\n"; return 1; }
    f.slowBundle.getChildFile("Contents").getChildFile("Resources").getChildFile("moduleinfo.json").deleteFile();
    scanning(f);
    if (f.description.name.isEmpty()) { std::cerr << "FAILED: the test plugin could not be scanned, so the rest cannot run\n"; return 1; }
    checking(f); safetyCheck(f); commandLine(f); hostWithRealPlugin(f);
    setBehaviour("");
    f.scratch.deleteRecursively();
    if (failures > 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS (" << checksRun << " checks): a real VST3 is scanned, checked, hosted and survives crashes, hangs, NaN and thread crashes through the scanner, the tool and the host\n";
    return 0;
}
