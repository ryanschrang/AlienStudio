// The Plugins panel: what it shows next to a slot that has a problem, the scan buttons, and the Scan menu's commands.
// Driven through the real buttons and callbacks. The pop-up menu itself needs a window and is not shown here.
#include "PluginPanel.h"
#include <algorithm>
#include <iostream>
#include <vector>

using namespace alien;

static int failures = 0;
static void check(bool ok, const juce::String& message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; ++failures; } }

static juce::TextButton* button(juce::Component& panel, const juce::String& text) {
    for (auto* c : panel.getChildren()) if (auto* b = dynamic_cast<juce::TextButton*>(c); b != nullptr && b->getButtonText() == text) return b;
    return nullptr;
}
static std::vector<juce::ComboBox*> combos(juce::Component& panel) {
    std::vector<juce::ComboBox*> out; for (auto* c : panel.getChildren()) if (auto* b = dynamic_cast<juce::ComboBox*>(c)) out.push_back(b); return out;
}
static PluginState vst3(const juce::String& name, bool bypass = false) {
    juce::PluginDescription d; d.name = name; d.pluginFormatName = "VST3"; d.fileOrIdentifier = "C:/VST3/" + name + ".vst3";
    return {d.createXml()->toString(), {}, bypass};
}
static juce::StringArray slotTexts(juce::ComboBox& box) { juce::StringArray t; for (int i = 0; i < box.getNumItems(); ++i) t.add(box.getItemText(i)); return t; }

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginPanel panel; panel.setSize(900, 400);
    auto boxes = combos(panel);
    check(boxes.size() == 3, "the panel has a destination, a plugin and a slot chooser");
    auto& slot = *boxes[2];
    const juce::StringArray tracks{"Track 1", "Track 2"};
    PluginRackState rack; rack.instrument = vst3("Big Synth"); rack.effects = {vst3("Verb"), vst3("Comp", true), vst3("Delay")};
    juce::KnownPluginList list;

    // Slot labels carry the health note.
    std::vector<std::pair<int, int>> asked;
    panel.health = [&](int where, int index) -> juce::String {
        asked.push_back({where, index});
        if (index == -1) return "NOT LOADED - blocked by the safety check";
        if (index == 0) return "FAILED - switched off";
        if (index == 2) return "editor crashed";
        return {};
    };
    panel.refresh(tracks, rack, list);
    auto texts = slotTexts(slot);
    check(texts.size() == 4, "one row for the instrument and one for each effect (" + juce::String(texts.size()) + ")");
    check(texts[0] == "Instrument: Big Synth  [NOT LOADED - blocked by the safety check]", "a plugin that was not loaded says so and why: " + texts[0]);
    check(texts[1] == "FX 1: Verb  [FAILED - switched off]", "a failed plugin says so: " + texts[1]);
    check(texts[2] == "FX 2: Comp [bypassed]", "a healthy plugin has no note: " + texts[2]);
    check(texts[3] == "FX 3: Delay  [editor crashed]", "an editor failure is shown: " + texts[3]);
    check(std::find(asked.begin(), asked.end(), std::make_pair(0, -1)) != asked.end() && std::find(asked.begin(), asked.end(), std::make_pair(0, 2)) != asked.end(),
          "the panel asks about the selected destination and each slot, instrument as -1");
    // Master FX has effects only, and its health is asked for under the master index.
    asked.clear();
    panel.refresh(tracks, rack, list);
    boxes[0]->setSelectedId(maxSessionTracks + 1, juce::dontSendNotification);
    panel.refresh(tracks, rack, list);
    texts = slotTexts(slot);
    check(texts.size() == 3 && texts[0].startsWith("FX 1:"), "Master FX has no instrument row");
    check(std::find(asked.begin(), asked.end(), std::make_pair(maxSessionTracks, 0)) != asked.end(), "and is asked about under the master index");
    boxes[0]->setSelectedId(1, juce::dontSendNotification);
    // No health callback: labels are plain.
    panel.health = nullptr; panel.refresh(tracks, rack, list);
    check(slotTexts(slot)[1] == "FX 1: Verb", "without a health source the labels are unchanged");

    // The slot buttons report what is selected; failures do not stop the buttons working.
    std::vector<std::pair<int, int>> actions;
    panel.onAction = [&](int op, int index) { actions.push_back({op, index}); };
    slot.setSelectedId(3, juce::dontSendNotification);            // FX 2
    for (const auto* name : {"Open editor", "Bypass / enable", "Remove", "Move FX up", "Move FX down"}) { auto* b = button(panel, name); check(b != nullptr, juce::String("button exists: ") + name); if (b && b->onClick) b->onClick(); }
    check(actions.size() == 5 && actions[0] == std::make_pair(0, 1) && actions[1] == std::make_pair(1, 1) && actions[2] == std::make_pair(2, 1) && actions[3] == std::make_pair(3, 1) && actions[4] == std::make_pair(4, 1),
          "Open editor, Bypass, Remove, Up and Down each report the selected slot");

    // Scanning: Scan is disabled, Cancel appears and works.
    auto* scanButton = button(panel, "Scan VST3..."); auto* cancelButton = button(panel, "Cancel scan");
    check(scanButton && cancelButton && scanButton->isEnabled() && !cancelButton->isVisible() && !panel.isScanning(), "before a scan: Scan is available and Cancel is hidden");
    int cancels = 0; panel.onCancelScan = [&] { ++cancels; };
    panel.setScanning(true);
    check(panel.isScanning() && !scanButton->isEnabled() && cancelButton->isVisible(), "during a scan: Scan is disabled and Cancel is shown");
    cancelButton->onClick();
    check(cancels == 1, "Cancel asks for the scan to stop");
    panel.setScanning(false);
    check(!panel.isScanning() && scanButton->isEnabled() && !cancelButton->isVisible(), "after the scan the panel goes back");
    check(cancelButton->getBounds().getWidth() > 0 && cancelButton->getRight() <= panel.getWidth(), "the Cancel button has room beside Scan");

    // Scan menu commands reach the owner, and the preflight toggle flips.
    std::vector<ScanCommand> received; panel.onScanCommand = [&](ScanCommand c) { received.push_back(c); };
    const ScanCommand all[] = {ScanCommand::standardFolders, ScanCommand::folder, ScanCommand::singleFile, ScanCommand::retryFailed, ScanCommand::rescanAll, ScanCommand::openScanLog, ScanCommand::openProblemLog, ScanCommand::recheckBlocked};
    for (auto c : all) panel.perform(c);
    check(received.size() == 8 && received[0] == ScanCommand::standardFolders && received[7] == ScanCommand::recheckBlocked, "every command is passed on");
    check(panel.preflightIsEnabled(), "the safety check starts switched on");
    panel.perform(ScanCommand::togglePreflight);
    check(!panel.preflightIsEnabled() && received.back() == ScanCommand::togglePreflight, "the toggle switches it off and tells the owner");
    panel.perform(ScanCommand::togglePreflight);
    check(panel.preflightIsEnabled(), "and back on");
    panel.setPreflightEnabled(false); check(!panel.preflightIsEnabled(), "the owner can set it too");

    if (failures != 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS: the Plugins panel shows slot problems, scan progress and commands\n";
    return 0;
}
