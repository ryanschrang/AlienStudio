#pragma once
// Plugins that crashed while Alien Studio was hosting them (T1-09).
//
// Crash containment keeps the session running when a plugin crashes while loading, preparing or
// processing audio, and switches that plugin off. This list remembers it: the next time a project
// loads the same plugin file, it is not loaded (its slot and saved settings stay in the project)
// until the file changes, which is what an update does, or the user allows it again from
// Scan VST3 > Blocked plugins. Invalid audio (NaN output) switches a plugin off but does not block it:
// it is often a setting, not the plugin.
//
// The same file holds the "scan for new plugins at startup" switch.
#include "PluginScan.h"
#include <map>
#include <vector>

namespace alien {

class PluginBlocklist {
public:
    struct Entry {
        juce::String path, name, why;
        scan::Fingerprint fingerprint;
        juce::int64 blockedAt{0};
    };

    explicit PluginBlocklist(juce::File storage = {}) : file(std::move(storage)) { load(); }
    static juce::File defaultFile() { return scan::ScanDatabase::defaultFile().getSiblingFile("PluginBlocklist.xml"); }

    // Message thread. Blocks the plugin's file after it crashed while being hosted. Returns true when
    // the plugin is blocked by this call (false: not a VST3 file, or already blocked).
    bool recordCrash(const juce::PluginDescription& description, const juce::String& why) {
        if (description.pluginFormatName != "VST3" || description.fileOrIdentifier.isEmpty()) return false;
        const juce::File bundle(description.fileOrIdentifier);
        const auto fingerprint = scan::fingerprintOf(bundle);
        if (const auto* known = find(bundle); known != nullptr && known->fingerprint == fingerprint) return false;
        Entry entry; entry.path = bundle.getFullPathName(); entry.name = description.name.isNotEmpty() ? description.name : bundle.getFileNameWithoutExtension();
        entry.why = why; entry.fingerprint = fingerprint; entry.blockedAt = juce::Time::currentTimeMillis();
        entries[key(bundle)] = std::move(entry);
        save();
        return true;
    }
    // Empty when the plugin may be loaded, otherwise why it is not. A file that has changed since it was
    // blocked (an update or reinstall) is allowed again, and forgotten.
    juce::String refusal(const juce::PluginDescription& description) {
        if (description.pluginFormatName != "VST3") return {};
        const juce::File bundle(description.fileOrIdentifier);
        const auto* known = find(bundle);
        if (known == nullptr) return {};
        if (!bundle.exists() || known->fingerprint != scan::fingerprintOf(bundle)) { entries.erase(key(bundle)); save(); return {}; }
        return known->name + " was not loaded because it crashed earlier (" + known->why + "). Its settings are kept in the project. "
               "It is allowed again when the plugin is updated, or from Scan VST3 > Blocked plugins.";
    }
    bool isBlocked(const juce::File& bundle) const { return find(bundle) != nullptr; }
    // Lets a blocked plugin load again. Returns false if it was not blocked.
    bool allow(const juce::String& path) {
        if (entries.erase(key(juce::File(path))) == 0) return false;
        save();
        return true;
    }
    int allowAll() { const auto n = static_cast<int>(entries.size()); if (n > 0) { entries.clear(); save(); } return n; }
    // Every blocked plugin, by name.
    std::vector<Entry> blocked() const {
        std::vector<Entry> list;
        for (const auto& [k, e] : entries) list.push_back(e);
        std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.name.compareIgnoreCase(b.name) < 0; });
        return list;
    }

    bool scanAtStartup() const noexcept { return startupScan; }
    void setScanAtStartup(bool on) { if (startupScan != on) { startupScan = on; save(); } }

private:
    static juce::String key(const juce::File& f) { return f.getFullPathName().toLowerCase(); }
    const Entry* find(const juce::File& bundle) const { const auto it = entries.find(key(bundle)); return it == entries.end() ? nullptr : &it->second; }
    void load() {
        if (file == juce::File()) return;
        auto xml = juce::parseXML(file);
        if (!xml || xml->getTagName() != "PluginBlocklist") return;
        startupScan = xml->getBoolAttribute("scanAtStartup", true);
        for (auto* x : xml->getChildWithTagNameIterator("Blocked")) {
            Entry e; e.path = x->getStringAttribute("path"); if (e.path.isEmpty()) continue;
            e.name = x->getStringAttribute("name", juce::File(e.path).getFileNameWithoutExtension()); e.why = x->getStringAttribute("why");
            e.fingerprint = {x->getStringAttribute("size").getLargeIntValue(), x->getStringAttribute("modified").getLargeIntValue()};
            e.blockedAt = x->getStringAttribute("at").getLargeIntValue();
            entries[key(juce::File(e.path))] = std::move(e);
        }
    }
    void save() const {
        if (file == juce::File()) return;
        juce::XmlElement root("PluginBlocklist"); root.setAttribute("version", 1); root.setAttribute("scanAtStartup", startupScan);
        for (const auto& [k, e] : entries) {
            auto* x = root.createNewChildElement("Blocked");
            x->setAttribute("path", e.path); x->setAttribute("name", e.name); x->setAttribute("why", e.why);
            x->setAttribute("size", juce::String(e.fingerprint.size)); x->setAttribute("modified", juce::String(e.fingerprint.modified));
            x->setAttribute("at", juce::String(e.blockedAt));
        }
        file.getParentDirectory().createDirectory();
        root.writeTo(file);
    }

    juce::File file;
    std::map<juce::String, Entry> entries;
    bool startupScan{true};
};

} // namespace alien
