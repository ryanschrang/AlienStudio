// The Song tab and the "Song mode" switch: which tab plays what, the top bar's six
// view buttons, the More... list, the switch's own button, and the layout at the
// smallest, a middle and a wide window. No audio device or window is needed.
#include "TopBar.h"
#include "ViewModes.h"
#include "StudioTheme.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

using namespace alien;

// Button::triggerClick() posts a message; running the message loop delivers it.
static void clickButton(juce::Button& button) {
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
}

static std::vector<juce::TextButton*> viewButtons(TopBar& bar) {
    std::vector<juce::TextButton*> views;
    for (auto* child : bar.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getName().startsWith("Show ")) views.push_back(b);
    std::sort(views.begin(), views.end(), [](auto* a, auto* b) { return a->getX() < b->getX(); });
    return views;
}

static juce::TextButton* named(TopBar& bar, const juce::String& name) {
    for (auto* child : bar.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getName() == name) return b;
    return nullptr;
}

static juce::ComboBox* moreList(TopBar& bar) {
    for (auto* child : bar.getChildren())
        if (auto* c = dynamic_cast<juce::ComboBox*>(child)) return c;
    return nullptr;
}

// ---- which tab plays what ----------------------------------------------------------------
static void viewModes() {
    constexpr int tabCount = 9;
    for (int tab = 0; tab < tabCount; ++tab)
        for (int mode = 0; mode <= 3; ++mode)
            require(patternModeForTab(tab, mode, false) == patternModeForTab(tab, mode),
                    "with Song mode off, every tab plays exactly what it always did");
    require(patternModeForTab(5, 0) == songMode, "the Song tab plays the song with the switch off");
    for (const int tab : {0, 1, 2, 5, rackTabIndex})
        for (int mode = 0; mode <= 3; ++mode)
            require(patternModeForTab(tab, mode, true) == songMode, "with Song mode on, Steps, Piano Roll, Session, Song and Rack all play the song");
    for (const int tab : {3, 4, 6, 7})
        for (int mode = 0; mode <= 3; ++mode)
            require(patternModeForTab(tab, mode, true) == mode, "Mixer, Drums, Audio lab and Plugins play nothing of their own, so the mode stays");
    std::cout << "PASS: Song mode picks the song on every playing tab\n";
}

// ---- the six view buttons ------------------------------------------------------------------
static void viewSwitch() {
    TopBar bar;
    bar.setSize(1400, TopBar::height);
    int chosen = -1;
    bar.onViewSelected = [&](int index) { chosen = index; };
    const auto views = viewButtons(bar);
    require(views.size() == 6, "the top bar has six view buttons");
    const std::array<std::pair<const char*, int>, 6> expected{{{"Steps", 0}, {"Piano Roll", 1}, {"Session", 2}, {"Song", 5}, {"Mixer", 3}, {"Rack", rackTabIndex}}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(views[i]->getButtonText() == expected[i].first, "the buttons read Steps, Piano Roll, Session, Song, Mixer, Rack from left to right");
        chosen = -1;
        clickButton(*views[i]);
        require(chosen == expected[i].second, "each button opens its own tab, and the old tab numbers did not change");
    }
    bar.setActiveView(5);
    for (auto* b : views) require(b->getToggleState() == (b->getButtonText() == "Song"), "the Song button lights while the Song is shown");
    bar.setActiveView(rackTabIndex);
    for (auto* b : views) require(b->getToggleState() == (b->getButtonText() == "Rack"), "and the Rack button for the rack");
    std::cout << "PASS: six view buttons in order, with the Song after Session\n";
}

// ---- More... ----------------------------------------------------------------------------------
static void moreListContents() {
    TopBar bar;
    bar.setSize(1400, TopBar::height);
    int chosen = -1;
    bar.onViewSelected = [&](int index) { chosen = index; };
    auto* more = ::moreList(bar);
    require(more != nullptr, "the More... list is there");
    juce::StringArray items;
    for (int i = 0; i < more->getNumItems(); ++i) items.add(more->getItemText(i));
    require(items == juce::StringArray({"Drums", "Audio lab", "Plugins"}), "More... lists Drums, Audio lab and Plugins, and no longer Arrange");
    const std::array<std::pair<int, int>, 3> ids{{{5, 4}, {7, 6}, {8, 7}}};
    for (const auto& [id, tab] : ids) {
        chosen = -1;
        more->setSelectedId(id, juce::sendNotificationSync);
        require(chosen == tab, "each More... entry opens the same tab it always did");
    }
    bar.setActiveView(4);
    require(more->getSelectedId() == 5, "the list shows Drums while Drums is open");
    for (auto* b : viewButtons(bar)) require(!b->getToggleState(), "and no view button is lit");
    bar.setActiveView(5);
    require(more->getSelectedId() == 0, "opening the Song clears the More... list");
    bar.setActiveView(0);
    require(more->getSelectedId() == 0, "and so does opening Steps");
    std::cout << "PASS: More... holds Drums, Audio lab and Plugins\n";
}

// ---- the switch ---------------------------------------------------------------------------------
static void songModeSwitch() {
    TopBar bar;
    bar.setSize(1400, TopBar::height);
    auto* button = named(bar, "Song mode");
    require(button != nullptr && button->getButtonText() == "Song mode", "there is a Song mode button");
    require(!button->getToggleState() && !bar.isSongMode(), "it starts off");
    require(button->getTooltip().isNotEmpty(), "and explains itself in a tooltip");
    std::vector<bool> events;
    bar.onSongModeToggled = [&](bool on) { events.push_back(on); };
    clickButton(*button);
    require(events == std::vector<bool>{true} && bar.isSongMode() && button->getToggleState(), "a click turns it on and says so");
    clickButton(*button);
    require(events == std::vector<bool>{true, false} && !bar.isSongMode(), "another turns it off");
    bar.setSongMode(true);
    require(bar.isSongMode() && events.size() == 2, "setSongMode lights the button without sending the click again");
    bar.setSongMode(false);
    require(!bar.isSongMode() && events.size() == 2, "and unlights it the same way");
    std::cout << "PASS: Song mode button\n";
}

// ---- layout ---------------------------------------------------------------------------------------
static void layout() {
    for (const int width : {960, 1180, 1320, 1439, 1440, 1600, 1920}) {
        TopBar bar;
        bar.setSize(width, TopBar::height);
        std::vector<juce::Component*> shown;
        for (auto* child : bar.getChildren()) if (child->isVisible() && child->getWidth() > 0) shown.push_back(child);
        for (auto* c : shown) {
            require(c->getX() >= 0 && c->getRight() <= width && c->getY() >= 0 && c->getBottom() <= TopBar::height, "every control stays inside the bar");
            if (dynamic_cast<juce::TextButton*>(c) == nullptr && dynamic_cast<juce::ComboBox*>(c) == nullptr) continue;
            for (auto* other : shown) {
                if (other == c || (dynamic_cast<juce::TextButton*>(other) == nullptr && dynamic_cast<juce::ComboBox*>(other) == nullptr)) continue;
                require(!c->getBounds().intersects(other->getBounds()), "and no two buttons overlap");
            }
        }
        const auto views = viewButtons(bar);
        require(views.size() == 6, "six view buttons at every width");
        for (auto* b : views) require(b->getWidth() >= 50 && b->getHeight() >= 24, "each one stays big enough to click");
        auto* button = named(bar, "Song mode");
        auto* more = ::moreList(bar);
        require(button != nullptr && more != nullptr && button->getWidth() >= 90 && !button->getBounds().intersects(more->getBounds()),
                "Song mode sits beside More... with room to read");
        // Six buttons must not squeeze the transport: it is what shows the bar, beat and tempo.
        int displayWidth = 0;
        for (auto* c : shown) if (dynamic_cast<ui::TransportDisplay*>(c) != nullptr) displayWidth = c->getWidth();
        std::cout << "  width " << width << ": transport display " << displayWidth << " px\n";
        require(displayWidth >= (width < 1180 ? 136 : 200), "the transport display keeps its room");
    }
    std::cout << "PASS: the top bar fits from 960 to 1920 pixels\n";
}

#include "ApplicationToolbarTests.h"
#include "DetachableViewTests.h"
#include "PresetBrowserTests.h"
#include "ProgressionEditorTests.h"

int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    viewModes();
    viewSwitch();
    moreListContents();
    songModeSwitch();
    layout();
    progressionEditorChecks(argc > 2 ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]) : juce::File{});
    applicationToolbarChecks();
    presetBrowserChecks();
    detachableViewChecks();
    detachedTypingChecks();
    if(argc>1)renderToolbarPreview(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]));
    std::cout << "All Song flow tests passed\n";
    return 0;
}
