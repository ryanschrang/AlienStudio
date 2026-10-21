// Scanning for plugins: where to look, what is inside a bundle, what a dead or hung scanner means,
// and a whole scan with some plugins that crash, hang or do not belong. Most of this runs against a
// pretend scanner so the cases are exact; the last part runs the real process handling against a
// helper program that really crashes, hangs and floods its output.
#include "PluginScan.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>

using namespace alien;
using namespace alien::scan;

static int failures = 0;
static void check(bool ok, const juce::String& message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; ++failures; } }

// ---- fixtures -------------------------------------------------------------------------------
static const char* archFolder(Platform p) { return p == Platform::Windows ? "x86_64-win" : p == Platform::Mac ? "MacOS" : "x86_64-linux"; }
static const char* libExtension(Platform p) { return p == Platform::Windows ? ".vst3" : p == Platform::Mac ? "" : ".so"; }
static juce::File makeBundle(const juce::File& parent, const juce::String& name, Platform p = currentPlatform(), const juce::String& content = "binary") {
    auto bundle = parent.getChildFile(name + ".vst3");
    auto lib = bundle.getChildFile("Contents").getChildFile(archFolder(p)).getChildFile(name + libExtension(p));
    lib.getParentDirectory().createDirectory(); lib.replaceWithText(content);
    return bundle;
}
static juce::File makeWrongArchBundle(const juce::File& parent, const juce::String& name) {
    auto bundle = parent.getChildFile(name + ".vst3");
    auto lib = bundle.getChildFile("Contents").getChildFile(currentPlatform() == Platform::Windows ? "x86-win" : "i386-linux").getChildFile(name + ".vst3");
    lib.getParentDirectory().createDirectory(); lib.replaceWithText("32 bit");
    return bundle;
}
static std::vector<juce::String> names(const std::vector<juce::File>& files, const juce::File& root) {
    std::vector<juce::String> out; for (const auto& f : files) out.push_back(f.getRelativePathFrom(root).replace("\\", "/")); return out;
}
static bool has(const std::vector<juce::String>& v, const juce::String& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static void writePE(const juce::File& f, int machine) {
    juce::MemoryBlock block(256, true); auto* p = static_cast<juce::uint8*>(block.getData());
    p[0] = 'M'; p[1] = 'Z'; p[0x3c] = 0x80; p[0x80] = 'P'; p[0x81] = 'E'; p[0x84] = static_cast<juce::uint8>(machine & 255); p[0x85] = static_cast<juce::uint8>(machine >> 8);
    f.replaceWithData(block.getData(), block.getSize());
}

// ---- discovery ------------------------------------------------------------------------------
static void discoveryTests(const juce::File& tmp) {
    auto root = tmp.getChildFile("vst3"); root.createDirectory();
    makeBundle(root, "Plain", Platform::Windows);
    makeBundle(root.getChildFile("Vendor A"), "Nested", Platform::Windows);
    makeBundle(root.getChildFile("Deep/One/Two/Three"), "Buried", Platform::Windows);
    root.getChildFile("Single.vst3").replaceWithText("old style single file");
    makeBundle(root.getChildFile("UPPER"), "Shout", Platform::Windows);
    root.getChildFile("UPPER/Shout.vst3").moveFileTo(root.getChildFile("UPPER/SHOUT2.VST3"));
    root.getChildFile("Docs").createDirectory(); root.getChildFile("Docs/readme.txt").replaceWithText("not a plugin");
    root.getChildFile("Empty").createDirectory();
    root.getChildFile("Other.dll").replaceWithText("x");
    root.getChildFile("Vendor A/Presets.vst3preset").replaceWithText("x");

    const auto found = discoverVst3Bundles({root});
    const auto n = names(found.bundles, root);
    check(has(n, "Plain.vst3") && has(n, "Vendor A/Nested.vst3") && has(n, "Deep/One/Two/Three/Buried.vst3") && has(n, "Single.vst3") && has(n, "UPPER/SHOUT2.VST3"), "bundles and single files are found at any depth, in any letter case");
    check(n.size() == 5, "nothing else is listed (got " + juce::String(static_cast<int>(n.size())) + ")");
    bool inner = false; for (const auto& s : n) inner = inner || s.contains("Contents/");
    check(!inner, "the library inside a bundle is not listed as a second plugin");
    check(found.problems.empty() && found.foldersVisited >= 8, "a normal tree has no problems and is fully walked");
    check(std::is_sorted(n.begin(), n.end(), [](const juce::String& a, const juce::String& b) { return a.compareIgnoreCase(b) < 0; }), "results are sorted");

    const auto overlapping = discoverVst3Bundles({root, root.getChildFile("Vendor A"), root});
    check(overlapping.bundles.size() == 5, "overlapping roots do not duplicate plugins");
    const auto missing = discoverVst3Bundles({root.getChildFile("nowhere"), root.getChildFile("Single.vst3"), root.getChildFile("Other.dll")});
    check(missing.bundles.size() == 1 && missing.problems.size() == 2, "a missing folder and a non-folder are reported; a root that is itself a plugin is accepted");
    check(discoverVst3Bundles({root.getChildFile("Empty")}).bundles.empty(), "an empty folder finds nothing");

    const auto shallow = discoverVst3Bundles({root}, 2);
    check(!has(names(shallow.bundles, root), "Deep/One/Two/Three/Buried.vst3") && !shallow.problems.empty(), "the depth limit stops the search and says so");

   #if !JUCE_WINDOWS
    // A link back to an ancestor must not trap the search.
    auto loop = tmp.getChildFile("loop"); loop.createDirectory(); makeBundle(loop, "Looped", Platform::Windows);
    juce::File link = loop.getChildFile("up");
    if (std::system(("ln -s \"" + loop.getFullPathName() + "\" \"" + link.getFullPathName() + "\"").toRawUTF8()) == 0) {
        const auto looped = discoverVst3Bundles({loop});
        check(looped.bundles.size() == 1, "a symbolic link loop is not followed");
    }
   #endif
}

// ---- inspecting bundles ---------------------------------------------------------------------
static void inspectTests(const juce::File& tmp) {
    auto root = tmp.getChildFile("inspect"); root.createDirectory();
    for (auto p : {Platform::Windows, Platform::Linux, Platform::Mac}) {
        auto b = makeBundle(root.getChildFile(archFolder(p)), "Good", p);
        const auto info = inspectBundle(b, p);
        check(info.problem.isEmpty() && info.binary.existsAsFile() && info.binary.getParentDirectory().getFileName() == archFolder(p), juce::String("a ") + archFolder(p) + " bundle is accepted and its library found");
    }
    auto only32 = root.getChildFile("only32.vst3"); only32.getChildFile("Contents/x86-win").createDirectory(); only32.getChildFile("Contents/x86-win/only32.vst3").replaceWithText("x");
    check(inspectBundle(only32, Platform::Windows).problem.contains("32-bit"), "a bundle with only a 32-bit build is rejected with that reason");
    auto arm = root.getChildFile("arm.vst3"); arm.getChildFile("Contents/arm64ec-win").createDirectory(); arm.getChildFile("Contents/arm64ec-win/arm.vst3").replaceWithText("x");
    check(inspectBundle(arm, Platform::Windows).problem.contains("ARM"), "an ARM-only bundle is rejected with that reason");
    auto empty = root.getChildFile("empty.vst3"); empty.createDirectory();
    check(inspectBundle(empty, Platform::Windows).problem.contains("Contents"), "a bundle without Contents is rejected");
    auto hollow = root.getChildFile("hollow.vst3"); hollow.getChildFile("Contents/x86_64-win").createDirectory();
    check(inspectBundle(hollow, Platform::Windows).problem.contains("no plugin library"), "a bundle whose architecture folder is empty is rejected");
    auto odd = root.getChildFile("odd.vst3"); odd.getChildFile("Contents/riscv-win").createDirectory();
    check(inspectBundle(odd, Platform::Windows).problem.contains("riscv-win"), "unknown contents are named in the reason");
    check(inspectBundle(root.getChildFile("gone.vst3"), Platform::Windows).problem.isNotEmpty(), "a missing file is rejected");
    // The library named like the bundle wins over helpers.
    auto multi = makeBundle(root, "Main", Platform::Windows); multi.getChildFile("Contents/x86_64-win/Helper.vst3").replaceWithText("x");
    check(inspectBundle(multi, Platform::Windows).binary.getFileName() == "Main.vst3", "the library named like the bundle is the one inspected");

    auto x64 = root.getChildFile("x64.vst3"), x86 = root.getChildFile("x86.vst3"), arm64 = root.getChildFile("arm64.vst3"), junk = root.getChildFile("junk.vst3");
    writePE(x64, 0x8664); writePE(x86, 0x014c); writePE(arm64, 0xaa64); junk.replaceWithText("this is not an executable at all, just text padding padding padding padding padding");
    check(peArchitecture(x64) == Arch::x64 && peArchitecture(x86) == Arch::x86 && peArchitecture(arm64) == Arch::arm64 && peArchitecture(junk) == Arch::unknown, "executable headers are read correctly");
    check(inspectBundle(x64, Platform::Windows).problem.isEmpty(), "a 64-bit single-file plugin is accepted");
    check(inspectBundle(x86, Platform::Windows).problem.contains("32-bit"), "a 32-bit single-file plugin is rejected");
    check(inspectBundle(arm64, Platform::Windows).problem.contains("ARM"), "an ARM single-file plugin is rejected");
    check(inspectBundle(junk, Platform::Windows).problem.isEmpty(), "a file that is not an executable is left for the real loader to judge");
}

// ---- reading what happened to a child -------------------------------------------------------
static ChildResult result(juce::uint32 exitCode, const juce::String& stage, const juce::String& xml = {}, const juce::String& printed = {}) {
    ChildResult r; r.exitCode = exitCode; r.stage = stage; r.xml = xml; r.printed = printed; r.milliseconds = 1500; return r;
}
static void classifyTests() {
    check(classify(result(0, "done"), 2, 60000).status == Status::ok, "a finished scan with plugins is OK");
    check(classify(result(0, "done"), 0, 60000).status == Status::failed, "...but not if its result cannot be read");
    const auto died = classify(result(0, "load"), 0, 60000);
    check(died.status == Status::crashed && died.detail.contains("loading the plugin") && died.detail.contains("most likely crashed"), "exit code 0 without a finished protocol is a crash (POSIX reports signals that way): " + died.detail);
    const auto av = classify(result(0xC0000005u, "load"), 0, 60000);
    check(av.status == Status::crashed && av.detail.contains("0xC0000005") && av.detail.contains("access violation") && av.detail.contains("loading the plugin"), "a Windows access violation is named: " + av.detail);
    check(classify(result(0xC00000FDu, "load"), 0, 60000).detail.contains("stack overflow") && classify(result(0xC0000409u, "load"), 0, 60000).detail.contains("fatal error") && classify(result(0xC0000374u, "load"), 0, 60000).detail.contains("heap corruption"), "stack overflow, fail-fast and heap corruption are named");
    check(classify(result(0xC0000135u, "start"), 0, 60000).detail.contains("DLL was not found"), "a missing dependency DLL is named");
    ChildResult hung = result(0, "load"); hung.outcome = ChildResult::Outcome::timedOut;
    const auto timed = classify(hung, 0, 60000);
    check(timed.status == Status::timedOut && timed.detail.contains("60 s") && timed.detail.contains("dialog"), "a hang says how long it was given and what it may be waiting for: " + timed.detail);
    check(classify(result(ChildExit::exitNoPlugins, "diagnose"), 0, 60000).status == Status::noPlugins, "a library with no plugin in it");
    const auto lib = classify(result(ChildExit::exitLibraryFailed, "diagnose", {}, "The plugin library could not be loaded: Windows error 126: The specified module could not be found.\nmore"), 0, 60000);
    check(lib.status == Status::incompatible && lib.detail.contains("error 126") && !lib.detail.contains("more"), "a library the system cannot load is incompatible, with the system's reason");
    ChildResult none = result(0, ""); none.outcome = ChildResult::Outcome::couldNotStart;
    check(classify(none, 0, 60000).status == Status::couldNotStart, "a scanner that could not be started is not blamed on the plugin");
    check(classify(result(9, "load"), 0, 60000).status == Status::failed && classify(result(9, "load"), 0, 60000).detail.contains("code 9"), "an unexpected exit code is reported");
    check(std::string(statusLabel(statusFromKey(statusKey(Status::timedOut)))) == "TIMED OUT", "status names survive being saved");
}

// ---- the database ---------------------------------------------------------------------------
static void databaseTests(const juce::File& tmp) {
    ScanDatabase db; auto bundle = makeBundle(tmp.getChildFile("db"), "Keep");
    ScanDatabase::Entry e; e.path = bundle.getFullPathName(); e.fingerprint = fingerprintOf(bundle); e.status = Status::crashed; e.detail = "boom \"quoted\" & <tagged>"; e.stage = "load"; e.plugins = 0; e.attempts = 3; e.scannedAt = 123456789012;
    db.put(e);
    const auto file = tmp.getChildFile("db/state.xml");
    check(db.save(file), "the database saves");
    ScanDatabase loaded; check(loaded.load(file), "...and loads");
    const auto* back = loaded.find(bundle);
    check(back && back->status == Status::crashed && back->detail == e.detail && back->stage == "load" && back->attempts == 3 && back->scannedAt == e.scannedAt && back->fingerprint == e.fingerprint, "every field survives, including awkward text");
    check(loaded.failures().size() == 1, "failures can be listed");
    auto before = fingerprintOf(bundle);
    bundle.getChildFile("Contents").getChildFile(archFolder(currentPlatform())).getChildFile(std::string("Keep") + libExtension(currentPlatform())).replaceWithText("a different, longer binary");
    check(!(fingerprintOf(bundle) == before), "replacing the library changes the bundle's fingerprint");
    ScanDatabase bad; juce::File(tmp.getChildFile("db/garbage.xml")).replaceWithText("<not-ours/>");
    check(!bad.load(tmp.getChildFile("db/garbage.xml")) && !bad.load(tmp.getChildFile("db/missing.xml")) && bad.all().empty(), "a damaged or missing database loads as empty instead of failing");
}

// ---- a whole scan with a pretend scanner ----------------------------------------------------
struct Scenario { std::function<ChildResult(const ChildRequest&)> run; };
static juce::String xmlFor(const juce::String& bundle, std::initializer_list<std::pair<const char*, bool>> plugins) {
    juce::KnownPluginList list; int uid = 1;
    for (const auto& [name, instrument] : plugins) {
        juce::PluginDescription d; d.name = name; d.pluginFormatName = "VST3"; d.fileOrIdentifier = bundle; d.isInstrument = instrument; d.uniqueId = d.deprecatedUid = uid++; list.addType(d);
    }
    return list.createXml()->toString();
}

static void scanTests(const juce::File& tmp) {
    auto root = tmp.getChildFile("scan"); root.createDirectory();
    auto vendor = root.getChildFile("Vendor");
    auto okTwo = makeBundle(vendor, "A_two"); auto crash = makeBundle(vendor, "B_crash"); auto hang = makeBundle(root, "C_hang");
    auto none = makeBundle(root, "D_none"); auto lib = makeBundle(root, "E_lib"); auto wrongArch = makeWrongArchBundle(root, "F_wrongarch");
    auto okOne = makeBundle(root.getChildFile("Deep/Er"), "G_one"); auto nt = makeBundle(root, "H_ntcrash"); auto cannot = makeBundle(root, "I_cannotstart");

    std::map<juce::String, int> calls; int total = 0;
    std::map<juce::String, std::function<ChildResult(const ChildRequest&)>> behaviour;
    behaviour["A_two"] = [&](const ChildRequest& r) { return result(0, "done", xmlFor(r.bundle, {{"Alpha Synth", true}, {"Alpha Verb", false}})); };
    behaviour["B_crash"] = [&](const ChildRequest&) { return result(0, "load"); };
    behaviour["C_hang"] = [&](const ChildRequest&) { auto r = result(0, "load"); r.outcome = ChildResult::Outcome::timedOut; return r; };
    behaviour["D_none"] = [&](const ChildRequest&) { return result(ChildExit::exitNoPlugins, "diagnose"); };
    behaviour["E_lib"] = [&](const ChildRequest&) { return result(ChildExit::exitLibraryFailed, "diagnose", {}, "The plugin library could not be loaded: Windows error 126"); };
    behaviour["G_one"] = [&](const ChildRequest& r) { return result(0, "done", xmlFor(r.bundle, {{"Gamma", false}})); };
    behaviour["H_ntcrash"] = [&](const ChildRequest&) { return result(0xC0000005u, "load"); };
    behaviour["I_cannotstart"] = [&](const ChildRequest&) { auto r = result(0, ""); r.outcome = ChildResult::Outcome::couldNotStart; return r; };
    std::set<juce::String> outputs;   // the result file each scanner process was told to write
    ChildRunner runner = [&](const ChildRequest& r) { const auto n = juce::File(r.bundle).getFileNameWithoutExtension(); ++calls[n]; ++total; outputs.insert(r.output.getFullPathName()); return behaviour.at(n)(r); };

    const auto logFile = tmp.getChildFile("scan.log");
    DiagnosticLog log(logFile);
    ScanDatabase db; juce::KnownPluginList known; std::atomic<bool> cancel{false};
    ScanOptions options; options.folders = {root}; options.appVersion = "test"; options.scratchFolder = tmp;
    int checkpoints = 0, lastDone = -1, lastTotal = -1; bool progressMonotonic = true;
    ScanHooks hooks;
    hooks.checkpoint = [&] { ++checkpoints; };
    hooks.progress = [&](int done, int count, const juce::String&) { progressMonotonic = progressMonotonic && done >= lastDone; lastDone = done; lastTotal = count; };

    auto report = runScan(options, db, known, runner, log, cancel, hooks);
    check(report.bundles.size() == 9, "all nine plugin files are examined (" + juce::String(static_cast<int>(report.bundles.size())) + ")");
    check(report.count(Status::ok) == 2 && report.count(Status::crashed) == 2 && report.count(Status::timedOut) == 1 && report.count(Status::noPlugins) == 1 && report.count(Status::incompatible) == 2 && report.count(Status::couldNotStart) == 1, "each outcome is counted: " + report.summary());
    check(known.getNumTypes() == 3, "the three plugins in the two good files are added (got " + juce::String(known.getNumTypes()) + ")");
    check(calls["F_wrongarch"] == 0 && total == 8, "a file that cannot work is rejected without starting a scanner for it");
    check(outputs.size() == 8, "every scanner process gets a result file of its own, so scans running together cannot read each other's answers (" + juce::String(static_cast<int>(outputs.size())) + " different for 8 processes)");
    check(db.find(crash) && db.find(crash)->status == Status::crashed && db.find(hang)->status == Status::timedOut && db.find(wrongArch)->status == Status::incompatible && db.find(okTwo)->plugins == 2, "what happened to each file is remembered");
    check(db.find(cannot) == nullptr, "a scanner that could not start is not recorded as a plugin failure");
    check(checkpoints == 10 && progressMonotonic && lastDone == 9 && lastTotal == 9, "progress and checkpoints are reported after every file and at the end");
    const auto text = log.read();
    check(text.contains("Plugin scan started") && text.contains("Found 9 plugin file(s)") && text.contains("B_crash.vst3") && text.contains("CRASHED") && text.contains("TIMED OUT") && text.contains("access violation") && text.contains("Alpha Synth [instrument]") && text.contains("Alpha Verb [FX]"), "the log names every file, outcome and plugin");
    check(text.contains("Needs attention") && text.contains("Summary:"), "the log ends with a summary and the list of failures");
    check(report.attention().contains("B_crash") && report.attention().contains("H_ntcrash") && report.attention().contains("dialog"), "the failure list explains each failure");

    // A second scan: nothing has changed, so nothing is loaded again - except the file that never got scanned.
    calls.clear(); total = 0;
    report = runScan(options, db, known, runner, log, cancel, hooks);
    check(total == 1 && calls["I_cannotstart"] == 1, "unchanged files are not scanned again, whether they were good or bad; only the one never scanned is retried (calls=" + juce::String(total) + ")");
    check(report.skippedUnchanged() == 2 && report.count(Status::crashed) == 2 && known.getNumTypes() == 3, "good files are 'unchanged', failed ones stay failed, the list is intact");
    check(log.read().contains("Use \"Retry failed plugins\""), "the log says how to try a failed file again");

    // Retry failed.
    calls.clear(); total = 0; options.retryFailed = true;
    runScan(options, db, known, runner, log, cancel, hooks);
    check(calls["B_crash"] == 1 && calls["C_hang"] == 1 && calls["D_none"] == 1 && calls["E_lib"] == 1 && calls["H_ntcrash"] == 1 && calls["A_two"] == 0 && calls["G_one"] == 0, "Retry failed scans exactly the failed files");
    options.retryFailed = false;

    // A changed file is scanned again, and a crash that has been fixed clears.
    calls.clear(); total = 0;
    crash.getChildFile("Contents").getChildFile(archFolder(currentPlatform())).getChildFile(std::string("B_crash") + libExtension(currentPlatform())).replaceWithText("fixed, and a different length");
    behaviour["B_crash"] = [&](const ChildRequest& r) { return result(0, "done", xmlFor(r.bundle, {{"Beta", false}})); };
    report = runScan(options, db, known, runner, log, cancel, hooks);
    check(calls["B_crash"] == 1 && total == 1 + 1 && db.find(crash)->status == Status::ok && known.getNumTypes() == 4, "updating a plugin makes it scan again, and the fix is picked up");
    check(db.find(crash)->attempts == 3, "the number of attempts is kept (scanned, retried, scanned after the update)");

    // Rescan everything.
    calls.clear(); total = 0; options.rescanUnchanged = true;
    runScan(options, db, known, runner, log, cancel, hooks);
    check(calls["A_two"] == 1 && calls["G_one"] == 1 && calls["B_crash"] == 1 && calls["C_hang"] == 1 && known.getNumTypes() == 4, "'rescan everything' scans every loadable file, and does not duplicate plugins");
    options.rescanUnchanged = false;

    // A plugin that now offers a different set replaces the old entries instead of adding to them.
    behaviour["A_two"] = [&](const ChildRequest& r) { return result(0, "done", xmlFor(r.bundle, {{"Alpha Synth 2", true}})); };
    options.rescanUnchanged = true; runScan(options, db, known, runner, log, cancel, hooks); options.rescanUnchanged = false;
    bool oldGone = true, newThere = false; for (const auto& d : known.getTypes()) { oldGone = oldGone && d.name != "Alpha Verb"; newThere = newThere || d.name == "Alpha Synth 2"; }
    check(oldGone && newThere && known.getNumTypes() == 3, "a rescanned plugin replaces its earlier entries");

    // Uninstalled plugins leave the list.
    okOne.deleteRecursively();
    report = runScan(options, db, known, runner, log, cancel, hooks);
    bool gammaGone = true; for (const auto& d : known.getTypes()) gammaGone = gammaGone && d.name != "Gamma";
    check(report.pluginsRemoved == 1 && gammaGone, "a plugin whose file has been deleted is removed from the list");

    // Cancelling.
    {
        ScanDatabase freshDb; juce::KnownPluginList freshKnown; std::atomic<bool> stop{false}; int started = 0;
        ScanHooks cancelHooks; cancelHooks.progress = [&](int done, int, const juce::String&) { if (done >= 2) stop.store(true); };
        ChildRunner counting = [&](const ChildRequest& r) { ++started; return runner(r); };
        auto cancelled = runScan(options, freshDb, freshKnown, counting, log, stop, cancelHooks);
        check(cancelled.cancelled && started <= 3 && cancelled.bundles.size() < 9 && log.read().contains("cancelled by the user"), "a cancelled scan stops promptly and says so");
        check(freshDb.find(hang) != nullptr, "what finished before cancelling is kept");
    }
    // Cancelling while a file is being scanned: the runner reports it was stopped part-way. Nothing is recorded about that file
    // (it was not found to be bad), the scan stops there, and what came before is kept.
    {
        ScanDatabase freshDb; juce::KnownPluginList freshKnown; std::atomic<bool> stop{false}; std::vector<juce::String> started;
        ChildRunner stopsPartWay = [&](const ChildRequest& r) {
            started.push_back(juce::File(r.bundle).getFileNameWithoutExtension());
            if (started.size() == 2) { ChildResult cut; cut.outcome = ChildResult::Outcome::cancelled; cut.stage = "loading"; return cut; }
            return runner(r);
        };
        auto cut = runScan(options, freshDb, freshKnown, stopsPartWay, log, stop, hooks);
        check(cut.cancelled && started.size() == 2, "a scan cancelled in the middle of a file stops there (" + juce::String(static_cast<int>(started.size())) + " files started)");
        const auto second = std::find_if(cut.bundles.begin(), cut.bundles.end(), [&](const BundleReport& b) { return b.bundle.getFileNameWithoutExtension() == started[1]; });
        check(second == cut.bundles.end(), "the file that was cut off is not listed as scanned, crashed or failed");
        check(cut.bundles.size() == 1 && cut.bundles[0].bundle.getFileNameWithoutExtension() == started[0], "the file before it is kept in the report");
        check(log.read().contains("cancelled by the user while scanning"), "the log says which file it was cancelled in");
        check(cut.summary().contains("Cancelled before finishing"), "and the summary says the scan did not finish");
        // The next scan starts that file again rather than treating it as already seen.
        std::vector<juce::String> second_run; ChildRunner again = [&](const ChildRequest& r) { second_run.push_back(juce::File(r.bundle).getFileNameWithoutExtension()); return runner(r); };
        std::atomic<bool> go{false};
        runScan(options, freshDb, freshKnown, again, log, go, hooks);
        check(std::find(second_run.begin(), second_run.end(), started[1]) != second_run.end() && std::find(second_run.begin(), second_run.end(), started[0]) == second_run.end(),
              "the next scan scans the file that was cut off, and not the one that finished");
    }
    // Saved state survives a restart: the next session skips the same files.
    const auto dbFile = tmp.getChildFile("scan-state.xml");
    check(db.save(dbFile), "state saves");
    ScanDatabase reloaded; reloaded.load(dbFile); calls.clear(); total = 0;
    juce::KnownPluginList sameKnown; sameKnown.recreateFromXml(*known.createXml());
    runScan(options, reloaded, sameKnown, runner, log, cancel, hooks);
    check(total == 1, "after a restart only the never-scanned file is scanned (calls=" + juce::String(total) + ")");
}

// ---- the text of the last scan ---------------------------------------------------------------
static void uniqueOutputTests(const juce::File& tmp) {
    // Four threads asking at once, none of the files created yet (a child process creates its file only when it finishes).
    std::vector<std::vector<juce::String>> picked(4); std::vector<std::thread> threads;
    for (auto& mine : picked) threads.emplace_back([&mine, &tmp] { for (int i = 0; i < 500; ++i) mine.push_back(uniqueOutput(tmp, "AlienPluginScan").getFullPathName()); });
    for (auto& t : threads) t.join();
    std::set<juce::String> all; int total = 0;
    for (const auto& mine : picked) for (const auto& p : mine) { all.insert(p); ++total; }
    check(total == 2000 && static_cast<int>(all.size()) == 2000, "2000 result files picked by 4 threads are all different (" + juce::String(static_cast<int>(all.size())) + ")");
    const auto one = uniqueOutput(tmp, "AlienPluginCheck");
    check(one.getParentDirectory() == tmp && one.getFileName().startsWith("AlienPluginCheck-") && one.hasFileExtension(".xml") && !one.exists(), "a result file is named after its purpose, lives in the folder asked for, and is not created yet");
    check(siblingOf(one, ".stage").getParentDirectory() == tmp && siblingOf(one, ".stage") != siblingOf(uniqueOutput(tmp, "AlienPluginCheck"), ".stage"), "and so are the progress and output files beside it");
}

static void fullTextTests() {
    ScanReport report; report.milliseconds = 1500; report.problems.push_back("D:\\Gone is not a folder");
    BundleReport good; good.bundle = juce::File("/plugins/Good.vst3"); good.status = Status::ok; good.action = BundleReport::Action::scanned; good.plugins.add("Good FX [FX]");
    BundleReport bad; bad.bundle = juce::File("/plugins/Bad.vst3"); bad.status = Status::crashed; bad.action = BundleReport::Action::scanned; bad.detail = "It crashed while loading the plugin and reading its class list.";
    BundleReport same; same.bundle = juce::File("/plugins/Same.vst3"); same.status = Status::ok; same.action = BundleReport::Action::skippedUnchanged;
    report.bundles = {good, bad, same};
    const auto text = report.fullText();
    check(text.contains("Alien Studio plugin scan") && text.contains("Took 1.5 s"), "the scan text has a header with the time taken");
    check(text.contains("Good.vst3") && text.contains("Bad.vst3") && text.contains("Same.vst3"), "it has a line for every file");
    check(text.contains("Good FX [FX]") && text.contains("crashed while loading the plugin and reading its class list"), "with what was found, and why a file failed and at which stage");
    check(text.contains("UNCHANGED") && text.contains("Folder problem: D:"), "unchanged files and folder problems are labelled");
    check(text.contains("Bad.vst3\n      It crashed while loading the plugin and reading its class list.\n") && text.contains("Good.vst3\n      Good FX [FX]\n"), "each file's detail and plugins are listed right under it, not only in the summary at the end");
    check(text.contains("NEEDS ATTENTION") && text.indexOf("NEEDS ATTENTION") > text.indexOf("Same.vst3"), "failures are repeated with their advice at the end");
    ScanReport clean; clean.bundles = {good};
    check(!clean.fullText().contains("NEEDS ATTENTION"), "a scan with no failures has no attention section");
}

// ---- real processes -------------------------------------------------------------------------
static void realProcessTests(const juce::File& tmp) {
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("ScanChildHelper");
   #if JUCE_WINDOWS
    exe = exe.withFileExtension("exe");
   #endif
    if (!exe.existsAsFile()) { std::cerr << "FAILED: helper program missing: " << exe.getFullPathName() << '\n'; ++failures; return; }
    auto folder = tmp.getChildFile("real"); folder.createDirectory();
    juce::File lastOutput;
    const auto run = [&](const juce::String& name, int timeoutMs = 30000) {
        auto bundle = folder.getChildFile(name + ".vst3"); bundle.replaceWithText("x");
        ChildRequest request; request.executable = exe; request.bundle = bundle.getFullPathName(); request.timeoutMs = timeoutMs;
        request.output = tmp.getNonexistentChildFile("realscan", ".xml", false);
        lastOutput = request.output;
        const auto r = runChildProcess(request);
        juce::KnownPluginList types; if (auto xml = juce::parseXML(r.xml)) types.recreateFromXml(*xml);
        return std::make_pair(r, classify(r, types.getNumTypes(), timeoutMs));
    };
    { auto [r, v] = run("ok_synth"); check(v.status == Status::ok && r.stage == "done" && r.xml.isNotEmpty(), "a well-behaved child scans fine: " + v.detail); }
    { auto [r, v] = run("crash_now"); check(v.status == Status::crashed && v.detail.contains("loading"), "a child that really crashes is reported as crashed, not as an empty scan: " + v.detail); }
    { auto [r, v] = run("abort_now"); check(v.status == Status::crashed, "so is one that aborts: " + v.detail); }
    { auto [r, v] = run("hang_now", 2000); check(v.status == Status::timedOut && r.milliseconds < 20000, "a child that hangs is stopped at the time limit: " + v.detail + " (" + juce::String(r.milliseconds) + " ms)");
      // "Stopped" means the process is gone, not merely that the parent stopped waiting: its heartbeat must have ended.
      const auto beat = lastOutput.getSiblingFile(lastOutput.getFileName() + ".beat");
      const auto before = beat.loadFileAsString();
      juce::Thread::sleep(600);
      check(before.isNotEmpty(), "the hanging child was really running (it left a heartbeat)");
      check(beat.loadFileAsString() == before, "a child that timed out has been killed, not left running (heartbeat " + before + " -> " + beat.loadFileAsString() + ")"); }
    { auto [r, v] = run("none_here"); check(v.status == Status::noPlugins, "exit code 2 means no plugins"); }
    { auto [r, v] = run("lib_missing"); check(v.status == Status::incompatible && v.detail.contains("error 126"), "the child's explanation reaches the verdict: " + v.detail); }
    { auto [r, v] = run("exit9_now"); check(v.status == Status::failed && v.detail.contains("code 9"), "an odd exit code is reported"); }
    { auto [r, v] = run("chatty_synth", 60000); check(v.status == Status::ok && r.printed.length() > 1000 && r.milliseconds < 50000, "a child that prints megabytes does not stall the scan (" + juce::String(r.milliseconds) + " ms)"); }
    {   // Cancelling while the plugin is stuck: the scan does not wait out the time limit, and the child is killed.
        auto bundle = folder.getChildFile("hang_cancel.vst3"); bundle.replaceWithText("x");
        std::atomic<bool> stop{false};
        ChildRequest request; request.executable = exe; request.bundle = bundle.getFullPathName(); request.timeoutMs = 60000; request.cancel = &stop;
        request.output = tmp.getNonexistentChildFile("cancelscan", ".xml", false);
        std::thread canceller([&] { juce::Thread::sleep(700); stop.store(true); });
        const auto r = runChildProcess(request);
        canceller.join();
        check(r.outcome == ChildResult::Outcome::cancelled && r.milliseconds < 15000, "cancelling a scan of a hung plugin returns promptly instead of waiting for the time limit (" + juce::String(r.milliseconds) + " ms)");
        check(classify(r, 0, 60000).status != Status::timedOut, "and is not mistaken for the plugin timing out");
        const auto beat = request.output.getSiblingFile(request.output.getFileName() + ".beat");
        const auto before = beat.loadFileAsString(); juce::Thread::sleep(600);
        check(before.isNotEmpty() && beat.loadFileAsString() == before, "the child was really running and is killed once cancelled");
    }
    {   // The same, through the whole scan as the application runs it: the cancel flag must reach the child.
        auto hung = makeBundle(folder, "hang_whole_scan");
        ScanOptions options; options.executable = exe; options.bundles.push_back(hung); options.timeoutMs = 60000;
        ScanDatabase db; juce::KnownPluginList known; std::atomic<bool> stop{false};
        const DiagnosticLog quiet;
        std::thread canceller([&] { juce::Thread::sleep(700); stop.store(true); });
        const auto started = juce::Time::getMillisecondCounter();
        const auto report = runScan(options, db, known, &runChildProcess, quiet, stop);
        const auto took = juce::Time::getMillisecondCounter() - started;
        canceller.join();
        check(report.cancelled && took < 15000, "cancelling a whole scan while a plugin hangs returns promptly (" + juce::String(static_cast<int>(took)) + " ms)");
        check(report.bundles.empty() && db.find(hung) == nullptr, "and the file it cut off is not recorded as a failure");
    }
    { ChildRequest request; request.executable = tmp.getChildFile("no-such-program"); request.bundle = "x.vst3"; request.output = tmp.getChildFile("never.xml");
      auto r = runChildProcess(request); check(classify(r, 0, 1000).status == Status::couldNotStart, "a missing scanner program is reported as such"); }
}

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-scan-test", "", false); tmp.createDirectory();
    discoveryTests(tmp); inspectTests(tmp); classifyTests(); databaseTests(tmp); uniqueOutputTests(tmp); scanTests(tmp); fullTextTests(); realProcessTests(tmp);
    tmp.deleteRecursively();
    if (failures != 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS: discovery, bundle inspection, crash/hang/timeout classification, scan database, whole-scan scenarios and real child processes\n";
    return 0;
}
