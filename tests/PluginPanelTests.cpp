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

static juce::String statusText(juce::Component& panel) {
    juce::String text; for (auto* c : panel.getChildren()) if (auto* l = dynamic_cast<juce::Label*>(c)) text = l->getText(); return text;
}
static juce::PluginDescription described(const juce::String& name, const juce::String& maker, const juce::String& category, bool instrument) {
    juce::PluginDescription d; d.name = name; d.manufacturerName = maker; d.category = category; d.isInstrument = instrument;
    d.pluginFormatName = "VST3"; d.fileOrIdentifier = "C:/VST3/" + name + ".vst3"; d.uniqueId = d.deprecatedUid = name.hashCode();
    return d;
}

// T1-10: the plugin browser lists stock effects and scanned plugins, searches and filters them, keeps
// choice ids stable for the add path, marks blocked plugins, and adds only what the destination takes.
static void browserTests() {
    juce::KnownPluginList known;
    // Added out of name order; the browser sorts by name, but choice ids follow the known list.
    for (const auto& d : {described("Zeta Verb", "Acme Audio", "Fx|Reverb", false), described("Mono Bass", "Synth Co", "Instrument|Synth", true),
                          described("Crashy Comp", "Flaky Ltd", "Fx|Dynamics", false)})
        known.addType(d);
    const auto types = known.getTypes();
    const auto idOf = [&](const juce::String& name) { for (int i = 0; i < types.size(); ++i) if (types[i].name == name) return i + 1; return 0; };
    const auto blocked = [](const juce::PluginDescription& d) { return d.name == "Crashy Comp"; };

    auto rows = PluginBrowser::entriesFor(known, blocked);
    check(rows.size() == stock::catalog.size() + 3, "one row per stock effect and per scanned plugin");
    check(rows[0].stock && rows[0].choice == PluginBrowser::stockBase && rows[0].name == stock::catalog[0].name, "stock effects come first, with their stock ids");
    const auto& firstPlugin = rows[stock::catalog.size()];
    check(firstPlugin.name == "Crashy Comp" && firstPlugin.choice == idOf("Crashy Comp") && firstPlugin.blocked, "plugins are sorted by name, keep their list ids and show blocks");
    check(firstPlugin.category == "Fx / Dynamics", "VST3 category paths read as words: " + firstPlugin.category);
    check(PluginBrowser::matching(rows, "bass synth", PluginBrowser::Kind::all).size() == 1, "every search word must match (name and category)");
    check(PluginBrowser::matching(rows, "ACME", PluginBrowser::Kind::all).size() == 1, "search ignores case and matches the maker");
    check(PluginBrowser::matching(rows, "", PluginBrowser::Kind::instruments).size() == 1, "the Instruments filter shows only instruments");
    check(PluginBrowser::matching(rows, "", PluginBrowser::Kind::effects).size() == stock::catalog.size() + 2, "the Effects filter shows stock and scanned effects");
    check(PluginBrowser::matching(rows, "stock", PluginBrowser::Kind::all).size() == stock::catalog.size(), "searching for stock finds the stock effects");
    check(PluginBrowser::matching(rows, "no such plugin", PluginBrowser::Kind::all).empty(), "a search can match nothing");

    PluginPanel panel; panel.setSize(1400, 640); panel.isBlocked = blocked;
    std::vector<std::pair<bool, int>> added; panel.onAdd = [&](bool instrument, int choice) { added.push_back({instrument, choice}); };
    PluginRackState rack; panel.refresh({"Track 1"}, rack, known);
    auto& browser = panel.pluginBrowser();
    auto* useInstrument = button(panel, "Use instrument"); auto* addFx = button(panel, "Add FX");
    check(useInstrument != nullptr && addFx != nullptr, "the add buttons exist");
    check(browser.selectedChoice() == PluginBrowser::stockBase && addFx->isEnabled() && !useInstrument->isEnabled(),
          "a new panel selects the first stock effect: Add FX on, Use instrument off");
    check(browser.getRight() <= panel.getWidth() && browser.getX() > button(panel, "Remove")->getRight() && browser.getHeight() > 300,
          "on a wide panel the browser sits beside the controls with room to browse");

    browser.setQuery("bass");
    check(browser.resultCount() == 1 && browser.selectedChoice() == idOf("Mono Bass") && useInstrument->isEnabled() && !addFx->isEnabled(),
          "searching selects the match; an instrument enables Use instrument only");
    useInstrument->onClick();
    check(added.size() == 1 && added[0] == std::make_pair(true, idOf("Mono Bass")), "Use instrument adds the selected plugin's id");
    browser.choose();
    check(added.size() == 2 && added[1] == std::make_pair(true, idOf("Mono Bass")), "double-click or Enter adds an instrument to the instrument slot");

    panel.refresh({"Track 1"}, rack, known);
    check(browser.selectedChoice() == idOf("Mono Bass") && browser.resultCount() == 1, "a refresh keeps the search and the selection");

    // Master FX takes no instrument: the button is off and double-click does nothing.
    combos(panel)[0]->setSelectedId(maxSessionTracks + 1, juce::dontSendNotification); combos(panel)[0]->onChange();
    check(!useInstrument->isEnabled(), "Use instrument is off for Master FX");
    browser.choose(); check(added.size() == 2, "an instrument is not added to an effects-only rack");
    check(statusText(panel).contains("effects only"), "and the status says why: " + statusText(panel));
    combos(panel)[0]->setSelectedId(1, juce::dontSendNotification); combos(panel)[0]->onChange();

    browser.setQuery("crashy");
    check(browser.selectedChoice() == idOf("Crashy Comp") && !addFx->isEnabled() && !useInstrument->isEnabled(), "a blocked plugin cannot be added");
    browser.choose(); check(added.size() == 2, "not even by double-click");
    check(statusText(panel).contains("Crashy Comp is blocked"), "and the status says it is blocked: " + statusText(panel));
    juce::ListBox* results = nullptr;
    for (auto* c : browser.getChildren()) if (auto* l = dynamic_cast<juce::ListBox*>(c)) results = l;
    check(results != nullptr && results->getModel() != nullptr && results->getModel()->getNameForRow(0) == "Crashy Comp, blocked, Flaky Ltd",
          "screen readers hear the plugin, its kind and maker rather than a row number");

    browser.setQuery("verb"); browser.setKind(PluginBrowser::Kind::effects);
    const std::vector<juce::String> expectedVerbs{"Alien Reverb", "Alien Convolution Reverb", "Zeta Verb"};
    std::vector<juce::String> matchedVerbs;
    for (int index : PluginBrowser::matching(rows, "verb", PluginBrowser::Kind::effects))
        matchedVerbs.push_back(rows[static_cast<std::size_t>(index)].name);
    check(matchedVerbs == expectedVerbs, "verb search includes both stock reverbs and the scanned reverb in stable order");
    check(browser.resultCount() == static_cast<int>(expectedVerbs.size()), "search and the Effects filter show all three reverb matches");
    browser.setKind(PluginBrowser::Kind::instruments);
    check(browser.resultCount() == 0 && browser.selected() == nullptr && !addFx->isEnabled() && !useInstrument->isEnabled(), "nothing listed, nothing to add");
    check(browser.select(idOf("Zeta Verb")) && browser.selectedChoice() == idOf("Zeta Verb") && addFx->isEnabled(), "select() clears a filter that hides the plugin");
    addFx->onClick();
    check(added.size() == 3 && added[2] == std::make_pair(false, idOf("Zeta Verb")), "Add FX adds the selected effect's id");

    // Narrow panels stack the browser under the destination instead.
    PluginPanel narrow; narrow.setSize(900, 700); narrow.refresh({"Track 1"}, rack, known);
    check(narrow.pluginBrowser().getWidth() > 0 && narrow.pluginBrowser().getRight() <= narrow.getWidth()
          && narrow.pluginBrowser().getBottom() < button(narrow, "Use instrument")->getY(), "a narrow panel shows the browser above the add buttons");
    // The stacked browser takes the spare height, leaving three lines of status under the slot buttons.
    const int spare = narrow.getHeight() - 20 - (button(narrow, "Remove")->getBottom() + 16);
    check(spare >= 72 && spare < 80 && narrow.pluginBrowser().getHeight() > 300,
          "a tall narrow panel grows the browser, not the empty space below the status (" + juce::String(spare) + " px left, list "
          + juce::String(narrow.pluginBrowser().getHeight()) + " px)");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    {
        PluginPanel responsePanel; responsePanel.setSize(900, 500);
        juce::KnownPluginList known;
        PluginRackState effects;
        effects.effects = {{stock::describe(stock::catalog[16]).createXml()->toString(), {}}, vst3("Other")};
        responsePanel.refresh({"Track 1"}, effects, known);
        auto* impulse = button(responsePanel, "Impulse response…");
        auto* sidechain = button(responsePanel, "Sidechain...");
        auto* selection = combos(responsePanel)[1];
        selection->setSelectedId(2, juce::dontSendNotification); selection->onChange();
        check(impulse && impulse->isVisible() && impulse->getWidth() > 0 && sidechain && !sidechain->isVisible(),
              "selected convolution slot shows its response control");
        selection->setSelectedId(3, juce::dontSendNotification); selection->onChange();
        check(impulse && !impulse->isVisible() && sidechain && sidechain->isVisible(),
              "other effects show the ordinary sidechain control");
    }
    PluginPanel panel; panel.setSize(900, 400);
    auto boxes = combos(panel);
    check(boxes.size() == 3, "the panel has a destination, a slot and a live MIDI chooser beside its plugin browser");
    auto& slot = *boxes[1];
    const juce::StringArray tracks{"Track 1", "Track 2"};
    {
        juce::KnownPluginList known;
        panel.refresh(tracks, {}, known);
        auto* mastering = button(panel, "Add gentle mastering chain");
        int requests = 0;
        panel.onMasteringChain = [&] { ++requests; };
        check(mastering && !mastering->isVisible(), "mastering preset is hidden on a track");
        boxes[0]->setSelectedId(maxSessionTracks + 1, juce::dontSendNotification); boxes[0]->onChange();
        check(mastering && mastering->isVisible() && mastering->getWidth() > 0, "Master FX offers the mastering preset");
        if (mastering) mastering->onClick();
        check(requests == 1, "mastering button dispatches one request");
        boxes[0]->setSelectedId(1, juce::dontSendNotification); boxes[0]->onChange();
        panel.onMasteringChain = {};
    }
    int liveRoute=-99;
    panel.onLiveInput=[&](int value){liveRoute=value;};
    panel.refreshLiveInput(tracks,1);
    check(boxes[2]->getSelectedId()==3 && liveRoute==-99,"route refresh is silent and preserves track");
    boxes[2]->setSelectedId(2,juce::dontSendNotification); boxes[2]->onChange();
    check(liveRoute==0,"live input selects track zero");
    boxes[2]->setSelectedId(1,juce::dontSendNotification); boxes[2]->onChange();
    check(liveRoute==-1,"live input returns to standalone synth");
    // Channel Rack instruments as live MIDI destinations (T1-09).
    int liveRack=-99;
    panel.onLiveRackInput=[&](int channel){liveRack=channel;};
    panel.refreshLiveInput(tracks,0,{{0,"Keys"},{3,"Pad"}},3);
    check(boxes[2]->getNumItems()==5 && boxes[2]->getItemText(4)=="Live MIDI: Channel Rack 4: Pad","rack instruments are listed after the tracks");
    check(boxes[2]->getSelectedId()==PluginPanel::liveRackBase+3 && liveRack==-99,"the chosen rack instrument is shown, silently");
    boxes[2]->setSelectedId(PluginPanel::liveRackBase,juce::dontSendNotification); boxes[2]->onChange();
    check(liveRack==0 && liveRoute==-1,"choosing a rack instrument reports its channel, not a track");
    panel.refreshLiveInput(tracks,1,{{0,"Keys"}},3);
    check(boxes[2]->getSelectedId()==3,"a rack choice that is no longer listed shows the track route");
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

    browserTests();
    if (failures != 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS: the Plugins panel shows slot problems, scan progress, commands and its plugin browser\n";
    return 0;
}
