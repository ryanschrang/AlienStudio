#pragma once
#include "PresetBrowser.h"
#include "TypingPianoRouter.h"
#include <set>

static void presetBrowserChecks() {
    alien::PresetBrowser browser; browser.setSize(500, 220);
    juce::TextEditor* search = nullptr; juce::ListBox* list = nullptr;
    juce::Button* audition = nullptr; juce::Button* stop = nullptr; juce::Button* apply = nullptr;
    for (auto* child : browser.getChildren()) {
        if (child->getName() == "Factory preset search") search = dynamic_cast<juce::TextEditor*>(child);
        if (child->getName() == "Factory preset results") list = dynamic_cast<juce::ListBox*>(child);
        if (child->getName() == "Audition factory preset") audition = dynamic_cast<juce::Button*>(child);
        if (child->getName() == "Stop preset audition") stop = dynamic_cast<juce::Button*>(child);
        if (child->getName() == "Use factory preset") apply = dynamic_cast<juce::Button*>(child);
    }
    require(search && list && audition && stop && apply, "browser exposes searchable list and explicit audition/stop/apply controls");
    int previewed = -1, applied = -1, stops = 0, applies = 0;
    browser.onAudition = [&](int id) { previewed = id; };
    browser.onApply = [&](int id) { applied = id; ++applies; browser.setCurrentPreset(id); };
    browser.onStop = [&] { ++stops; };
    const auto query = [&](const char* text) { search->setText(text, false); search->onTextChange(); };
    require(browser.resultCount() == alien::presetCount && browser.selectedPreset() == 0 && !apply->isEnabled(), "factory list starts complete without reapplying current preset");
    query("  BaSS   square  ");
    require(browser.resultCount() == 1 && browser.selectedPreset() == 1, "case-insensitive multi-term search matches name and sound type with stable preset identity");
    clickButton(*audition); require(previewed == 1 && applied == -1 && applies == 0, "audition does not apply a preset");
    search->onReturnKey(); require(previewed == 1 && applies == 0, "search Return auditions rather than mutating the project");
    clickButton(*apply); require(applied == 1 && applies == 1 && !apply->isEnabled(), "Use preset explicitly applies once and identifies the current sound");
    query("pad"); require(browser.resultCount() == 2, "category tags find soft pad and strings");
    list->selectRow(1); require(browser.selectedPreset() == 6, "filtered row resolves original factory index");
    browser.setApplyEnabled(false); require(!apply->isEnabled() && audition->isEnabled(), "busy state blocks applying while independent preview remains available");
    browser.setApplyEnabled(true); clickButton(*apply); require(applied == 6 && applies == 2, "apply uses filtered factory identity");
    const auto previousStops = stops; query("nothing matches this");
    require(browser.resultCount() == 0 && browser.selectedPreset() == -1 && !apply->isEnabled() && !audition->isEnabled() && stops > previousStops,
        "empty search stops preview and disables actions instead of using a stale row");
    query(""); require(browser.resultCount() == alien::presetCount, "clearing search restores complete list");
    clickButton(*stop); search->onEscapeKey(); require(stops > previousStops + 1, "Stop and Escape cancel preview");
    for (int width : {360, 500, 800}) {
        browser.setSize(width, 200);
        for (auto* child : browser.getChildren()) require(browser.getLocalBounds().contains(child->getBounds()), "browser controls fit compact and wide sizes");
    }
    juce::Component root; root.setSize(600, 300); root.addAndMakeVisible(browser); root.addToDesktop(0); root.setVisible(true);
    juce::MidiKeyboardState notes; std::set<int> held;
    {
        TypingPianoRouter router(root, notes, [&](int key) { return held.count(key) != 0; });
        root.getPeer()->handleFocusGain(); search->grabKeyboardFocus();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        held.insert('A'); root.getPeer()->handleKeyPress(juce::KeyPress('a', {}, 'a'));
        require(search->getText().containsIgnoreCase("a") && !notes.isNoteOn(2, 60), "typing into preset search enters text without playing the typing piano");
        held.clear(); root.getPeer()->handleKeyUpOrDown(false);
    }
    root.removeChildComponent(&browser); root.removeFromDesktop();
    // Clear callbacks before referenced local counters leave scope.
    browser.onStop = {}; browser.onAudition = {}; browser.onApply = {};
    std::cout << "PASS: factory preset search, stable filtered IDs, explicit apply, empty states and audition controls\n";
}

