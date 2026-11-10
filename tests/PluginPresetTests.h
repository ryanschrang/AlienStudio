#pragma once
// Plugin presets (T1-10): finding .vstpreset files, listing and loading programs, the search, the preset browser and
// the Plugins panel's Presets button. PluginEndToEndTests loads a real .vstpreset into the real test VST3 through the host.
#include "PluginPresetBrowser.h"

namespace preset_tests {
// A plugin with programs, standing in for a hosted one. It is not a VST3 instance, so it loads no preset files.
struct ProgramPlugin final : juce::AudioProcessor {
    explicit ProgramPlugin(int programs) : count(programs) {}
    const juce::String getName() const override { return "Program Plugin"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return count; }
    int getCurrentProgram() override { return current; }
    void setCurrentProgram(int index) override { current = index; }
    const juce::String getProgramName(int index) override { return index == 1 ? juce::String("  ") : "Sound " + juce::String(index); }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    int count{}, current{};
};
inline juce::File touch(const juce::File& file) { file.getParentDirectory().createDirectory(); file.replaceWithText("x"); return file; }
}

static void pluginPresetTests() {
    using namespace preset_tests;
    namespace pp = plugin_presets;
    const auto scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("alien-presets-" + juce::Uuid().toDashedString());
    const auto user = scratch.getChildFile("user"), shared = scratch.getChildFile("shared"), bundle = scratch.getChildFile("Bundles").getChildFile("Maker Synth.vst3");
    touch(user.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("Init.vstpreset"));
    touch(user.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("Bass").getChildFile("Sub 10.vstpreset"));
    touch(user.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("Bass").getChildFile("Sub 9.vstpreset"));
    touch(user.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("Bass").getChildFile("Deep").getChildFile("Rumble.vstpreset"));
    touch(user.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("readme.txt"));
    touch(user.getChildFile("Maker").getChildFile("Other Synth").getChildFile("Not mine.vstpreset"));
    touch(shared.getChildFile("Maker").getChildFile("Maker Synth").getChildFile("Pad.vstpreset"));
    touch(bundle.getChildFile("Contents").getChildFile("Resources").getChildFile("Presets").getChildFile("Lead.vstpreset"));
    touch(user.getChildFile("A_B").getChildFile("C_D").getChildFile("Odd.vstpreset"));

    const auto found = pp::findFiles("Maker", "Maker Synth", {user, shared}, bundle);
    juce::StringArray seen;
    for (const auto& p : found.presets) seen.add(p.folder + "|" + p.name);
    check(seen.joinIntoString(",") == "|Init,Bass|Sub 9,Bass|Sub 10,Bass/Deep|Rumble,Factory|Lead,Shared|Pad",
          "a plugin's preset files are found in its user, shared and bundle folders, sorted by folder then name: " + seen.joinIntoString(","));
    check(!found.truncated && std::all_of(found.presets.begin(), found.presets.end(), [](const pp::Preset& p) { return !p.isProgram() && p.file.existsAsFile(); }),
          "each is a file, not a program");
    check(pp::findFiles("A/B", "C:D", {user}).presets.size() == 1, "a maker or plugin name with characters a folder cannot have is found under its safe folder name");
    check(pp::findFiles("", "Maker Synth", {user}).presets.empty() && pp::findFiles("Maker", "Nothing", {user}).presets.empty(), "a plugin with no folder has no files");
    check(pp::findFiles("Maker", "Maker Synth", {}, scratch.getChildFile("Bundles")).presets.empty(), "only a .vst3 bundle's own presets count");

    // Search: every word, in the name or the folder, ignoring case.
    const auto names = [&](const std::vector<int>& rows) { juce::StringArray a; for (const int r : rows) a.add(found.presets[static_cast<std::size_t>(r)].name); return a.joinIntoString(","); };
    check(names(pp::filter(found.presets, "bass")) == "Sub 9,Sub 10,Rumble", "searching for a folder finds what is in it");
    check(names(pp::filter(found.presets, "  SUB   10 ")) == "Sub 10", "every word must match, in any case");
    check(names(pp::filter(found.presets, "")).contains("Init") && pp::filter(found.presets, "").size() == found.presets.size(), "no search shows everything");
    check(pp::filter(found.presets, "zzz").empty(), "a search can find nothing");

    // Programs: listed when there are more than one, and loaded by number.
    ProgramPlugin one(1), many(3);
    check(pp::programs(one).empty(), "one program is just the current sound, so it is not listed");
    const auto programs = pp::programs(many);
    check(programs.size() == 3 && programs[0].name == "Sound 0" && programs[1].name == "Program 2" && programs[2].isProgram() && programs[2].folder == "Programs",
          "programs are listed in order, an unnamed one by number");
    check(pp::load(many, programs[2]).isEmpty() && many.current == 2, "loading a program selects it");
    many.count = 2;
    check(pp::load(many, programs[2]).contains("no longer") && many.current == 2, "a program the plugin no longer has is refused");
    check(pp::load(many, found.presets[0]).contains("Only VST3"), "a plugin that is not a VST3 loads no preset files");
    pp::Preset gone; gone.name = "Gone"; gone.file = scratch.getChildFile("gone.vstpreset");
    check(pp::load(many, gone).contains("is gone"), "a preset file that was deleted is refused");

    // The browser: search, select, load, and what it says.
    std::vector<pp::Preset> loaded;
    juce::String refusal;
    PluginPresetBrowser browser("Maker Synth", found, [&](const pp::Preset& p) { loaded.push_back(p); return refusal; });
    check(browser.resultCount() == static_cast<int>(found.presets.size()) && browser.selected() != nullptr && browser.selected()->name == "Init",
          "the browser lists every preset with the first selected");
    check(loaded.empty(), "opening or selecting loads nothing");
    browser.setQuery("deep");
    check(browser.resultCount() == 1 && browser.selected()->name == "Rumble", "the search narrows the list and selects what is left");
    check(browser.loadSelected() && loaded.size() == 1 && loaded[0].name == "Rumble" && browser.statusText().contains("Loaded \"Rumble\""), "Load preset loads the selection");
    refusal = "Maker Synth did not accept that.";
    check(!browser.loadSelected() && browser.statusText() == refusal, "a refusal is shown");
    browser.setQuery("nothing here");
    check(browser.resultCount() == 0 && browser.selected() == nullptr && browser.emptyText() == "No matching presets" && !browser.loadSelected() && loaded.size() == 2,
          "a search with no results has nothing to load");
    browser.setQuery("");
    check(browser.selected() != nullptr, "clearing the search shows the presets again");
    PluginPresetBrowser none("Bare", {}, [&](const pp::Preset&) { return juce::String{}; });
    check(none.resultCount() == 0 && none.emptyText().contains("VST3 Presets"), "a plugin with no presets says where they go");

    // The Plugins panel's Presets button asks for the selected slot's presets.
    PluginPanel panel; panel.setSize(900, 500);
    PluginRackState rack; rack.instrument = vst3("Big Synth"); rack.effects = {vst3("Verb"), vst3("Delay")};
    juce::KnownPluginList list;
    panel.refresh({"Track 1"}, rack, list);
    std::vector<int> asked;
    panel.onPresets = [&](int slot) { asked.push_back(slot); };
    auto* presets = button(panel, "Presets...");
    auto& slot = *combos(panel)[1];
    check(presets != nullptr && presets->isVisible() && presets->getWidth() > 0, "the panel has a Presets button");
    // A click is delivered from the message loop, so each one is let through before the selection changes.
    const auto click = [&] { if (presets != nullptr) presets->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil(50); };
    slot.setSelectedId(3, juce::dontSendNotification); click();
    slot.setSelectedId(1, juce::dontSendNotification); click();
    check(asked == std::vector<int>{1, -1}, "it names the selected effect, or the instrument as -1");
    scratch.deleteRecursively();
}
