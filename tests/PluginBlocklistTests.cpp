// The plugin blocklist (T1-09): a plugin that crashed while hosted is remembered against its file, refused
// on later loads until the file changes or the user allows it again, and the list and the startup-scan
// switch survive a restart. The Scan menu's Blocked plugins submenu lists and allows them.
#include "PluginBlocklist.h"
#include "PluginPanel.h"
#include <cstdlib>
#include <iostream>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static juce::PluginDescription describe(const juce::File& bundle, const char* name) {
    juce::PluginDescription d; d.name = name; d.pluginFormatName = "VST3"; d.fileOrIdentifier = bundle.getFullPathName();
    return d;
}

static void blocking(const juce::File& folder) {
    const auto storage = folder.getChildFile("PluginBlocklist.xml");
    const auto bundle = folder.getChildFile("Crashy.vst3"), other = folder.getChildFile("Fine.vst3");
    bundle.replaceWithText("not really a plugin");
    other.replaceWithText("nor this");
    const auto crashy = describe(bundle, "Crashy"), fine = describe(other, "Fine");
    {
        PluginBlocklist list(storage);
        require(list.refusal(crashy).isEmpty() && list.blocked().empty() && list.scanAtStartup(), "nothing is blocked to begin with; startup scans are on");
        require(list.recordCrash(crashy, "it failed while processing audio: access violation"), "a crash blocks the plugin");
        require(!list.recordCrash(crashy, "again"), "a second crash of the same file is not a new entry");
        const auto why = list.refusal(crashy);
        require(why.contains("Crashy") && why.contains("crashed earlier") && why.contains("Blocked plugins"), "a later load is refused, saying why and how to allow it");
        require(list.refusal(fine).isEmpty(), "other plugins are not affected");
        juce::PluginDescription stock = crashy; stock.pluginFormatName = "Alien";
        require(!list.recordCrash(stock, "x") && list.refusal(stock).isEmpty(), "only VST3 files are blocked");
        list.setScanAtStartup(false);
    }
    {
        PluginBlocklist reopened(storage);
        require(reopened.refusal(crashy).isNotEmpty() && reopened.blocked().size() == 1 && reopened.blocked()[0].name == "Crashy",
                "the block survives a restart");
        require(!reopened.scanAtStartup(), "and so does the startup-scan switch");
        // An update changes the file: the plugin may load again, and is forgotten.
        bundle.appendText(" - version 2");
        require(reopened.refusal(crashy).isEmpty() && reopened.blocked().empty(), "an updated plugin is allowed again");
    }
    {
        PluginBlocklist list(storage);
        require(list.blocked().empty(), "and stays forgotten after a restart");
        list.recordCrash(crashy, "it crashed while loading");
        list.recordCrash(fine, "it failed while preparing to play");
        require(list.blocked().size() == 2 && list.blocked()[0].name == "Crashy", "the list is in name order");
        require(list.allow(bundle.getFullPathName()) && list.refusal(crashy).isEmpty() && list.refusal(fine).isNotEmpty(), "one plugin can be allowed again");
        require(!list.allow(bundle.getFullPathName()), "allowing one that is not blocked does nothing");
        require(list.allowAll() == 1 && list.blocked().empty(), "or all of them");
    }
    // A damaged file is ignored rather than trusted.
    storage.replaceWithText("<PluginBlocklist><Blocked path=\"\"/><Blocked/></PluginBlock");
    require(PluginBlocklist(storage).blocked().empty(), "a damaged list blocks nothing");
    std::puts("PASS: crashed plugins are blocked, remembered and allowed again");
}

static void menu() {
    PluginPanel panel;
    juce::StringArray names{"Crashy", "Fine"};
    std::vector<int> allowed;
    panel.blockedPlugins = [&] { return names; };
    panel.onAllowBlocked = [&](int index) { allowed.push_back(index); };
    const auto items = panel.blockedMenu();
    juce::PopupMenu::MenuItemIterator it(items);
    juce::StringArray texts;
    while (it.next()) if (!it.getItem().isSeparator) texts.add(it.getItem().text);
    require(texts == juce::StringArray({"Allow Crashy to load again", "Allow Fine to load again", "Allow all of them again"}), "the submenu lists each blocked plugin");
    panel.chooseScanItem(PluginPanel::blockedBase + 1);
    panel.chooseScanItem(PluginPanel::allowAllBlocked);
    require(allowed == std::vector<int>({1, -1}), "choosing one allows it; Allow all allows every one");
    names.clear();
    const auto empty = panel.blockedMenu();
    juce::PopupMenu::MenuItemIterator none(empty);
    require(none.next() && !none.getItem().isEnabled && none.getItem().text == "No plugin is blocked", "an empty list says so");

    std::vector<ScanCommand> received;
    panel.onScanCommand = [&](ScanCommand c) { received.push_back(c); };
    panel.setStartupScanEnabled(true);
    panel.chooseScanItem(10);
    require(!panel.startupScanIsEnabled() && received.back() == ScanCommand::toggleStartupScan, "the startup-scan item switches it and tells the owner");
    std::puts("PASS: the Blocked plugins menu and the startup-scan switch");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-blocklist-test", "");
    folder.createDirectory();
    blocking(folder);
    menu();
    folder.deleteRecursively();
    return 0;
}
