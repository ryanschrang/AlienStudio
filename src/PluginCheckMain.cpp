// AlienPluginCheck: a command-line tool that finds VST3 plugins and tests whether Alien Studio can
// host them. Run it on the machine whose plugins you care about and send back the report.
//
//   AlienPluginCheck check                         every plugin in the standard VST3 folders
//   AlienPluginCheck check "C:\path\Plugin.vst3"   one plugin
//   AlienPluginCheck check "D:\MyPlugins" --quick  every plugin under a folder, short version of the tests
//   AlienPluginCheck scan                          only find and list the plugins (no tests)
//
// Every plugin is scanned and tested in a separate process, so a plugin that crashes or hangs
// ends up as a line in the report instead of ending the tool.
#include "PluginCheck.h"
#include <iostream>

namespace {
constexpr const char* toolVersion = "0.1";

void say(const juce::String& text) { std::cout << text.toRawUTF8() << std::endl; }

struct Arguments {
    juce::String command;
    std::vector<juce::File> paths;
    bool quick{false}, editor{true}, rescan{true};
    juce::String nameFilter;
    juce::File outFolder;
    int timeoutSeconds{180};
    bool bad{false};
    juce::String problem;
};

Arguments parseArguments(const juce::StringArray& args) {
    Arguments a;
    if (args.isEmpty()) { a.command = "help"; return a; }
    a.command = args[0];
    for (int i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        const auto next = [&](const char* flag) -> juce::String {
            if (i + 1 >= args.size()) { a.bad = true; a.problem = juce::String(flag) + " needs a value."; return {}; }
            return args[++i];
        };
        if (arg == "--quick") a.quick = true;
        else if (arg == "--no-editor") a.editor = false;
        else if (arg == "--name") a.nameFilter = next("--name");
        else if (arg == "--out") a.outFolder = juce::File::getCurrentWorkingDirectory().getChildFile(next("--out"));
        else if (arg == "--timeout") a.timeoutSeconds = juce::jlimit(10, 3600, next("--timeout").getIntValue());
        else if (arg.startsWith("--")) { a.bad = true; a.problem = "Unknown option " + arg + "."; }
        else a.paths.push_back(juce::File::getCurrentWorkingDirectory().getChildFile(arg));
    }
    return a;
}

void usage() {
    say("AlienPluginCheck " + juce::String(toolVersion) + " - finds VST3 plugins and tests whether Alien Studio can host them.\n"
        "\n"
        "  AlienPluginCheck check [path...] [options]    scan and test plugins\n"
        "  AlienPluginCheck scan  [path...] [options]    only find plugins and say which ones load\n"
        "\n"
        "  path          a .vst3 plugin, or a folder to search (including subfolders).\n"
        "                With no path, the standard VST3 folders are searched.\n"
        "Options:\n"
        "  --name TEXT   only test plugins whose name contains TEXT (for example: --name Supermassive)\n"
        "  --quick       the short version of the tests (about a second per plugin)\n"
        "  --no-editor   do not open the plugin's window\n"
        "  --timeout S   give up on a plugin after S seconds (default 180)\n"
        "  --out FOLDER  where to write the reports (default: a new folder here named AlienPluginCheck-<date>)\n"
        "\n"
        "Reports: plugin-check-report.txt (read this), plugin-check-report.xml, plugin-scan.log.\n"
        "Exit code: 0 if every plugin is compatible, 1 if any failed or crashed, 2 for bad arguments, 3 if no plugins were found.");
}

// A report for a file that could not even be scanned, so the final list has a line for it.
alien::check::Report unscannable(const juce::File& bundle, alien::scan::Status status, const juce::String& detail) {
    alien::check::Report r;
    r.name = bundle.getFileNameWithoutExtension(); r.path = bundle.getFullPathName(); r.format = "VST3";
    r.host = "Alien Studio plugin check, " + juce::SystemStats::getOperatingSystemName();
    const auto outcome = status == alien::scan::Status::crashed || status == alien::scan::Status::timedOut ? alien::check::Outcome::crash : alien::check::Outcome::fail;
    r.steps.push_back({"Scan", outcome, juce::String(alien::scan::statusLabel(status)) + ": " + detail, 0});
    return r;
}
} // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::StringArray raw;
    for (int i = 1; i < argc; ++i) raw.add(argv[i]);

    // Hidden modes: this program is also the helper process that the scan and the checks run in.
    if (raw.size() == 3 && raw[0] == "--scan-vst3") return alien::scan::scanBundleInChild(raw[1], juce::File(raw[2]));
    if (raw.size() == 5 && raw[0] == "--check-vst3") return alien::check::checkVst3InChild(juce::File(raw[1]), juce::File(raw[2]), raw[3] == "quick", raw[4] == "editor");

    const auto args = parseArguments(raw);
    if (args.command == "help" || args.command == "--help" || args.command == "-h" || args.command == "/?") { usage(); return 0; }
    if (args.bad || (args.command != "check" && args.command != "scan")) { say(args.problem.isNotEmpty() ? args.problem : "Unknown command \"" + args.command + "\"."); say(""); usage(); return 2; }
    const bool testing = args.command == "check";

    auto out = args.outFolder != juce::File() ? args.outFolder : juce::File::getCurrentWorkingDirectory().getChildFile("AlienPluginCheck-" + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H%M%S"));
    if (!out.createDirectory().wasOk()) { say("Cannot create the report folder " + out.getFullPathName()); return 2; }
    alien::DiagnosticLog scanLog(out.getChildFile("plugin-scan.log"), 8 * 1024 * 1024);

    // 1. What to look at.
    alien::scan::ScanOptions options;
    options.timeoutMs = args.timeoutSeconds * 1000; options.appVersion = juce::String("plugin check ") + toolVersion;
    options.retryFailed = true; options.rescanUnchanged = true;
    for (const auto& p : args.paths) {
        if (p.isDirectory() && !p.hasFileExtension("vst3")) options.folders.push_back(p);
        else if (p.exists()) options.bundles.push_back(p);
        else { say("Not found: " + p.getFullPathName()); return 2; }
    }
    if (args.paths.empty()) options.folders = alien::scan::standardVst3Folders();
    say("AlienPluginCheck " + juce::String(toolVersion) + " on " + juce::SystemStats::getOperatingSystemName() + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : ""));
    for (const auto& f : options.folders) say("Searching (including subfolders): " + f.getFullPathName());
    for (const auto& b : options.bundles) say("Plugin: " + b.getFullPathName());
    say("Reports will be written to " + out.getFullPathName());
    say("");

    // 2. Scan: every file in its own process.
    alien::scan::ScanDatabase database; juce::KnownPluginList known; std::atomic<bool> cancel{false};
    alien::scan::ScanHooks hooks;
    hooks.progress = [](int done, int total, const juce::String& current) { if (current.isNotEmpty()) say("Scanning " + juce::String(done + 1) + "/" + juce::String(total) + "  " + current); };
    const auto scanReport = alien::scan::runScan(options, database, known, &alien::scan::runChildProcess, scanLog, cancel, hooks);
    say("");
    say("Scan: " + scanReport.summary());
    if (scanReport.bundles.empty()) { say("No VST3 plugins were found. Check the folder, or pass the path to a .vst3 file."); return 3; }

    // 3. Test each plugin that loaded.
    std::vector<alien::check::Report> reports;
    for (const auto& b : scanReport.bundles)
        if (b.status != alien::scan::Status::ok && (args.nameFilter.isEmpty() || b.bundle.getFileName().containsIgnoreCase(args.nameFilter)))
            reports.push_back(unscannable(b.bundle, b.status, b.detail));
    if (testing) {
        std::vector<juce::PluginDescription> toTest;
        for (const auto& d : known.getTypes()) if (args.nameFilter.isEmpty() || d.name.containsIgnoreCase(args.nameFilter) || juce::File(d.fileOrIdentifier).getFileName().containsIgnoreCase(args.nameFilter)) toTest.push_back(d);
        say(juce::String(static_cast<int>(toTest.size())) + " plugin(s) to test" + (args.nameFilter.isNotEmpty() ? " matching \"" + args.nameFilter + "\"" : juce::String()) + (args.quick ? " (quick)." : "."));
        int n = 0;
        for (const auto& d : toTest) {
            say(""); say("[" + juce::String(++n) + "/" + juce::String(static_cast<int>(toTest.size())) + "] Testing " + d.name + " ...");
            alien::check::IsolatedOptions isolated; isolated.timeoutMs = args.timeoutSeconds * 1000; isolated.battery.quick = args.quick; isolated.battery.editor = args.editor;
            auto report = alien::check::checkIsolated(d, isolated);
            say(report.toText());
            scanLog.write(report.acceptable() ? "info" : "fault", "Check " + d.name + ": " + report.verdict() + (report.firstProblem().isNotEmpty() ? " - " + report.firstProblem() : juce::String()));
            reports.push_back(std::move(report));
        }
    } else {
        for (const auto& d : known.getTypes()) if (args.nameFilter.isEmpty() || d.name.containsIgnoreCase(args.nameFilter)) {
            alien::check::Report r = alien::check::Report::forDescription(d); r.steps.push_back({"Scan", alien::check::Outcome::pass, "Loads and lists as a plugin.", 0}); reports.push_back(std::move(r));
        }
    }

    if (reports.empty()) {
        say(args.nameFilter.isNotEmpty() ? "No plugin matching \"" + args.nameFilter + "\" was found among the VST3 files that were scanned (" + scanReport.summary() + "). Check the spelling, or pass the path to the .vst3 file."
                                         : "Nothing could be tested.");
        say("The scan log is in " + out.getChildFile("plugin-scan.log").getFullPathName());
        return 3;
    }

    // 4. The reports.
    int acceptable = 0, problems = 0;
    juce::String text;
    text << "Alien Studio plugin check " << toolVersion << "\n" << juce::Time::getCurrentTime().toString(true, true) << "\n" << juce::SystemStats::getOperatingSystemName()
         << (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "") << "\nmode: " << args.command << (args.quick ? " (quick)" : "") << (args.editor ? "" : ", no editor") << "\n\n";
    text << "SUMMARY\n=======\n";
    for (const auto& r : reports) {
        (r.acceptable() ? acceptable : problems)++;
        text << r.verdict().paddedRight(' ', 26) << r.name << (r.firstProblem().isNotEmpty() ? "  -  " + r.firstProblem() : juce::String()) << "\n";
    }
    text << "\n" << reports.size() << " plugin(s): " << acceptable << " compatible, " << problems << " with problems.\n" << "Scan: " << scanReport.summary() << "\n\nDETAILS\n=======\n\n";
    for (const auto& r : reports) text << r.toText() << "\n";
    out.getChildFile("plugin-check-report.txt").replaceWithText(text);

    juce::XmlElement root("PluginChecks");
    root.setAttribute("tool", toolVersion); root.setAttribute("date", juce::Time::getCurrentTime().toISO8601(true)); root.setAttribute("os", juce::SystemStats::getOperatingSystemName());
    root.setAttribute("quick", args.quick); root.setAttribute("compatible", acceptable); root.setAttribute("problems", problems);
    for (const auto& r : reports) if (auto x = r.toXml()) root.addChildElement(x.release());
    root.writeTo(out.getChildFile("plugin-check-report.xml"));

    say(""); say("================================================================");
    if (testing) {
        for (const auto& r : reports) say(r.verdict().paddedRight(' ', 26) + r.name);
        say(""); say(juce::String(static_cast<int>(reports.size())) + " plugin(s): " + juce::String(acceptable) + " compatible, " + juce::String(problems) + " with problems.");
    }
    say("Send back: " + out.getChildFile("plugin-check-report.txt").getFullPathName());
    say("            " + out.getChildFile("plugin-scan.log").getFullPathName());
    return problems > 0 ? 1 : 0;
}
