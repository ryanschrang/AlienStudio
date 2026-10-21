#pragma once
// The safety check that runs before a VST3 plugin is loaded into a project.
//
// Loading a plugin runs its code inside Alien Studio. If that code crashes in a way the host cannot
// catch (heap corruption, a crash on a thread the plugin started) or never returns, the session and
// any unsaved work go with it. So the first time a plugin file is seen, the short compatibility check
// runs in a separate process. If the plugin crashes or hangs there, it is not loaded: the slot stays
// in the project with its saved settings, and the message says why. A plugin that passes is trusted
// until its file changes (the verdict is remembered against the file's size and modification time).
#include "PluginCheck.h"
#include <mutex>
#include <optional>

namespace alien {

class Preflight {
public:
    using Checker = std::function<check::Report(const juce::PluginDescription&)>;
    struct Entry {
        scan::Fingerprint fingerprint;
        bool allowed{true};
        juce::String why;                 // empty when allowed
        juce::int64 checkedAt{0};
    };

    // cacheFile: where verdicts are remembered (empty = in memory only). log: where checks are noted.
    Preflight(juce::File cacheFile, Checker checkerToUse, DiagnosticLog logToUse = DiagnosticLog())
        : file(std::move(cacheFile)), checker(std::move(checkerToUse)), log(std::move(logToUse)) { load(); }

    bool enabled() const { return on; }
    void setEnabled(bool shouldRun) { if (on == shouldRun) return; on = shouldRun; save(); }
    int checksRun() const { return checks; }

    // A report blocks hosting if loading the plugin into the session could take the session down:
    // it crashed, or it hung. (NaN output, odd latency and lost state are handled while hosting.)
    static bool blocks(const check::Report& report, juce::String& why) {
        if (report.hung) { why = "it stopped responding during the safety check"; if (const auto* s = firstProblemStep(report)) why += " (" + s->name + ")"; return true; }
        for (const auto& step : report.steps)
            if (step.outcome == check::Outcome::crash) { why = "it crashed during the safety check, in the step \"" + step.name + "\": " + step.detail; return true; }
        return false;
    }

    // Message thread. May block for the length of the check the first time a plugin is seen.
    // Returns an empty string if the plugin may be loaded, otherwise the reason it was not.
    juce::String verify(const juce::PluginDescription& description) {
        if (!on || description.pluginFormatName != "VST3") return {};
        const juce::File bundle(description.fileOrIdentifier);
        if (!bundle.exists()) return {};                         // the loader will say that the file is missing
        const auto fingerprint = scan::fingerprintOf(bundle);
        if (const auto* known = find(bundle); known != nullptr && known->fingerprint == fingerprint) return message(description, *known);

        ++checks;
        log.write("info", "Safety check before loading " + description.name + " (" + bundle.getFullPathName() + ")");
        const auto report = checker(description);
        Entry entry; entry.fingerprint = fingerprint; entry.checkedAt = juce::Time::currentTimeMillis();
        entry.allowed = !blocks(report, entry.why);
        log.write(entry.allowed ? "info" : "fault", description.name + ": " + report.verdict() + (entry.allowed ? "" : " - not loaded: " + entry.why));
        if (entry.allowed && report.overall() == check::Outcome::fail) log.write("warn", description.name + ": " + report.firstProblem() + " (loaded anyway: this is handled while hosting)");
        put(bundle, entry);
        return message(description, entry);
    }

    // Forget one verdict so the next load checks again.
    void forget(const juce::PluginDescription& description) { entries.erase(juce::File(description.fileOrIdentifier).getFullPathName()); save(); }
    // Forget every verdict that blocked a plugin. Returns how many there were.
    int forgetBlocked() {
        int n = 0;
        for (auto it = entries.begin(); it != entries.end();) { if (!it->second.allowed) { it = entries.erase(it); ++n; } else ++it; }
        if (n > 0) save();
        return n;
    }
    const Entry* find(const juce::File& bundle) const {
        const auto it = entries.find(bundle.getFullPathName());
        return it == entries.end() ? nullptr : &it->second;
    }
    std::size_t size() const { return entries.size(); }

private:
    static const check::Step* firstProblemStep(const check::Report& r) { for (const auto& s : r.steps) if (s.outcome == check::Outcome::fail || s.outcome == check::Outcome::crash) return &s; return nullptr; }
    static juce::String message(const juce::PluginDescription& d, const Entry& e) {
        if (e.allowed) return {};
        return d.name + " was not loaded because " + e.why + ". Its settings are kept in the project. "
               "It will be checked again if the plugin is updated, or use Scan > Re-check blocked plugins.";
    }
    void put(const juce::File& bundle, const Entry& entry) { entries[bundle.getFullPathName()] = entry; save(); }

    void load() {
        if (file == juce::File()) return;
        auto xml = juce::parseXML(file);
        if (!xml || xml->getTagName() != "PluginPreflight") return;
        on = xml->getBoolAttribute("enabled", true);
        for (auto* e : xml->getChildWithTagNameIterator("Check")) {
            Entry entry; entry.fingerprint = {e->getStringAttribute("size").getLargeIntValue(), e->getStringAttribute("modified").getLargeIntValue()};
            entry.allowed = e->getBoolAttribute("allowed", true); entry.why = e->getStringAttribute("why"); entry.checkedAt = e->getStringAttribute("at").getLargeIntValue();
            entries[e->getStringAttribute("path")] = entry;
        }
    }
    void save() const {
        if (file == juce::File()) return;
        juce::XmlElement root("PluginPreflight"); root.setAttribute("enabled", on);
        for (const auto& [path, entry] : entries) {
            auto* e = root.createNewChildElement("Check");
            e->setAttribute("path", path); e->setAttribute("size", juce::String(entry.fingerprint.size)); e->setAttribute("modified", juce::String(entry.fingerprint.modified));
            e->setAttribute("allowed", entry.allowed); e->setAttribute("why", entry.why); e->setAttribute("at", juce::String(entry.checkedAt));
        }
        file.getParentDirectory().createDirectory();
        root.writeTo(file);
    }

    juce::File file;
    Checker checker;
    DiagnosticLog log;
    std::map<juce::String, Entry> entries;
    bool on{true};
    int checks{0};
};

// The checker the application uses: the short compatibility check in a separate process.
inline Preflight::Checker isolatedPreflightChecker(juce::File executable, int timeoutMs = 60000) {
    return [executable = std::move(executable), timeoutMs](const juce::PluginDescription& d) {
        check::IsolatedOptions options; options.executable = executable; options.timeoutMs = timeoutMs;
        options.battery.quick = true; options.battery.editor = false;
        return check::checkIsolated(d, options);
    };
}

} // namespace alien
