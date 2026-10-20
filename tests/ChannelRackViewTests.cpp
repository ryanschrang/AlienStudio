// The Channel Rack screen driven through its real mouse handlers: geometry,
// paint and erase strokes, mute/solo, knobs, menus, rename, the graph editor,
// pattern commands, scrolling, the top-bar button, what the painted pattern
// sounds like, and screenshots. No audio device or window is needed: pop-up
// menus and the rename box go through test seams.
#include "ChannelRackView.h"
#include "TopBar.h"
#include "StudioTheme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

using namespace alien;
using namespace alien::rack;

static const juce::ModifierKeys leftButton(juce::ModifierKeys::leftButtonModifier);
static const juce::ModifierKeys rightButton(juce::ModifierKeys::rightButtonModifier);

static juce::MouseEvent mouse(juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1, bool dragged = false) {
    const auto now = juce::Time::getCurrentTime();
    const auto pf = p.toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pf, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pf, now, clicks, dragged);
}

// Button::triggerClick() posts a message; running the message loop delivers it.
static void clickButton(juce::Button& button) {
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
}

struct Rig {
    ToneEngine engine;
    ProjectController controller{engine};
    ChannelRackView view{controller};
    ChannelRackGrid& grid{view.rackGrid()};
    ChannelRackGraph& graph{view.graphEditor()};
    std::array<float, 256> left{}, right{};

    explicit Rig(int width = 1180, int height = 640) {
        engine.prepare(48000);
        controller.pushDocumentToEngine();
        view.setSize(width, height);
        view.refresh();
    }
    const Project& rack() const { return controller.channelRack(); }
    const Step& step(int channel, int index) const {
        return rack().pattern(rack().selectedPattern).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(index)];
    }
    int onCount(int channel) const { int n = 0; for (int s = 0; s < 64; ++s) n += step(channel, s).on ? 1 : 0; return n; }
    int totalOn() const { int n = 0; for (int c = 0; c < rack().channelCount; ++c) n += onCount(c); return n; }
    juce::Point<int> cellCentre(int row, int s) const { return grid.cellBounds(row, s).getCentre(); }
    void press(juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1) {
        c.mouseDown(mouse(c, p, mods, clicks)); c.mouseUp(mouse(c, p, mods, clicks));
    }
    void click(juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1) { press(grid, p, mods, clicks); }
    void stroke(juce::Point<int> from, std::initializer_list<juce::Point<int>> via, juce::ModifierKeys mods = leftButton) {
        grid.mouseDown(mouse(grid, from, mods));
        juce::Point<int> last = from;
        for (const auto& p : via) { grid.mouseDrag(mouse(grid, p, mods, 1, true)); last = p; }
        grid.mouseUp(mouse(grid, last, mods));
    }
    // Runs the audio callback and returns the loudest sample.
    float render(int samples) {
        float peak = 0.0f;
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
            float* outputs[]{left.data(), nullptr, right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 3, count, {});
            for (int i = 0; i < count; ++i) peak = std::max(peak, std::abs(left[static_cast<std::size_t>(i)]));
            samples -= count;
        }
        return peak;
    }
};

// ---- menus ------------------------------------------------------------------------
struct MenuEntry { int id; bool enabled; bool ticked; juce::String text; };
static std::vector<MenuEntry> entries(const juce::PopupMenu& menu) {
    std::vector<MenuEntry> out;
    juce::PopupMenu::MenuItemIterator it(menu, true);
    while (it.next()) {
        const auto& item = it.getItem();
        if (item.itemID != 0 && !item.isSeparator) out.push_back({item.itemID, item.isEnabled, item.isTicked, item.text});
    }
    return out;
}
static const MenuEntry* entry(const std::vector<MenuEntry>& list, int id) {
    for (const auto& e : list) if (e.id == id) return &e;
    return nullptr;
}
struct ShownMenu {
    bool shown{};
    juce::PopupMenu menu;
    juce::Rectangle<int> area;
    std::function<void(int)> done;
    void reset() { shown = false; done = nullptr; menu = juce::PopupMenu(); }
};

static juce::ScrollBar* verticalBarOf(juce::Component& view) {
    for (auto* child : view.getChildren())
        if (auto* bar = dynamic_cast<juce::ScrollBar*>(child); bar != nullptr && bar->isVertical()) return bar;
    return nullptr;
}

static juce::ScrollBar* horizontalBarOf(juce::Component& view) {
    for (auto* child : view.getChildren())
        if (auto* bar = dynamic_cast<juce::ScrollBar*>(child); bar != nullptr && !bar->isVertical()) return bar;
    return nullptr;
}

// ---- geometry ---------------------------------------------------------------------
static void geometryChecks() {
    Rig rig;
    auto& g = rig.grid;
    const int channels = rig.rack().channelCount;
    require(channels == 4, "the starting rack has four channels");
    const int cell = std::min(64, (1180 - ChannelRackGrid::headerWidth) / 16);
    require(g.getWidth() == 1180 && g.cellWidth() == cell && g.contentWidth() == cell * 16 && g.maxScroll() == 0 && g.maxScrollY() == 0,
            "16 steps and 4 channels fit without scrolling");
    // Rows tile the grid without overlap, and every header control sits inside the header.
    for (int row = 0; row < channels; ++row) {
        const auto bounds = g.rowBounds(row);
        require(row == 0 || bounds.getY() == g.rowBounds(row - 1).getBottom(), "rows are contiguous");
        const std::array<juce::Rectangle<int>, 6> parts{g.muteRect(row), g.nameRect(row), g.panRect(row), g.volumeRect(row), g.soloRect(row), g.routeRect(row)};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            require(juce::Rectangle<int>(0, bounds.getY(), ChannelRackGrid::headerWidth, bounds.getHeight()).contains(parts[i]),
                    "header controls stay inside the header and their row");
            for (std::size_t j = i + 1; j < parts.size(); ++j) require(!parts[i].intersects(parts[j]), "header controls never overlap");
        }
        for (int s = 0; s < 16; ++s) {
            require(g.cellBounds(row, s).getX() >= ChannelRackGrid::headerWidth && g.stepAtX(g.cellBounds(row, s).getCentreX()) == s
                    && g.rowAt(g.cellBounds(row, s).getCentreY()) == row, "every cell maps back to itself");
        }
    }
    require(g.rowAt(g.rowBounds(channels).getY() + 4) == -1, "below the last channel is empty");
    require(g.rowAt(3) == -1, "the ruler is not a row");
    require(g.stepAtX(10) == -1 && g.stepAtX(ChannelRackGrid::headerWidth - 1) == -1, "the header is not a step");
    require(g.stepAtX(ChannelRackGrid::headerWidth) == 0 && g.stepAtX(ChannelRackGrid::headerWidth + cell * 16 - 1) == 15
            && g.stepAtX(ChannelRackGrid::headerWidth + cell * 16) == -1, "the steps end where the pattern ends");

    // The graph lines up with the grid and sits below it.
    require(rig.graph.getX() == 0 && rig.graph.plot().getX() == ChannelRackGrid::headerWidth && rig.graph.getWidth() == g.getWidth(), "the graph editor spans the grid's columns");
    require(rig.graph.getY() >= g.getBottom() && rig.graph.getBottom() <= rig.view.getHeight(), "the graph sits below the grid");
    require(verticalBarOf(rig.view) != nullptr && !verticalBarOf(rig.view)->isVisible(), "four channels need no vertical scroll bar");

    // 64 steps in a narrow window scroll sideways; cells never get narrower than 15 px.
    Rig narrow(900, 600);
    narrow.controller.editRack("Length", [](Project& p) { return p.setPatternLength(p.selectedPattern, 64); });
    narrow.view.refresh();
    auto& n = narrow.grid;
    require(n.length() == 64 && n.cellWidth() == ChannelRackGrid::minCell && n.contentWidth() == 64 * 15, "64 steps use the minimum cell width");
    require(n.maxScroll() == 64 * 15 - (900 - ChannelRackGrid::headerWidth), "and scroll by the amount that does not fit");
    require(n.stepAtX(ChannelRackGrid::headerWidth + 3) == 0, "unscrolled, the first column is step 1");
    int scrolled = -1;
    n.onScrolled = [&](int x) { scrolled = x; };
    n.setScrollPosition(300);
    require(scrolled == 300 && n.scrollPosition() == 300 && n.stepAtX(ChannelRackGrid::headerWidth + 3) == 20
            && n.cellBounds(0, 20).getX() == ChannelRackGrid::headerWidth, "scrolling shifts the columns and reports it");
    n.setScrollPosition(99999);
    require(n.scrollPosition() == n.maxScroll() && n.stepAtX(899) == 63, "scrolling stops at the end");
    n.setScrollPosition(-5);
    require(n.scrollPosition() == 0, "and at the start");
    juce::MouseWheelDetails sideways{}; sideways.deltaX = -0.5f;
    n.mouseWheelMove(mouse(n, {600, 100}), sideways);
    require(n.scrollPosition() == 200, "the wheel scrolls sideways");
    n.setScrollPosition(n.maxScroll());
    narrow.controller.editRack("Length", [](Project& p) { return p.setPatternLength(p.selectedPattern, 16); });
    narrow.view.refresh();
    require(n.maxScroll() == 0 && n.scrollPosition() == 0, "shortening the pattern scrolls back to the start");

    // 32 steps in a roomy window: no scroll.
    Rig mid(1180, 600);
    mid.controller.editRack("Length", [](Project& p) { return p.setPatternLength(p.selectedPattern, 32); });
    mid.view.refresh();
    require(mid.grid.cellWidth() == (1180 - ChannelRackGrid::headerWidth) / 32 && mid.grid.maxScroll() == 0, "32 steps fit in a wide window");
    std::cout << "PASS: rack grid geometry, hit mapping and sideways scrolling\n";
}

// The scroll bar under the grid mirrors the grid's own scroll position (and the reverse).
static void scrollBarFollowsGrid() {
    Rig rig(900, 600);
    rig.controller.editRack("Length", [](Project& p) { return p.setPatternLength(p.selectedPattern, 64); });
    rig.view.refresh();
    auto* bar = horizontalBarOf(rig.view);
    require(bar != nullptr && bar->getCurrentRangeStart() == 0.0 && rig.grid.maxScroll() > 300, "a 64-step pattern in a narrow window has a scroll bar at the start");
    rig.grid.setScrollPosition(300);
    require(bar->getCurrentRangeStart() == 300.0, "scrolling the grid moves the scroll bar with it");
    juce::MouseWheelDetails sideways{}; sideways.deltaX = 0.25f;
    rig.grid.mouseWheelMove(mouse(rig.grid, {600, 100}), sideways);
    require(rig.grid.scrollPosition() == 200 && bar->getCurrentRangeStart() == 200.0, "so does the wheel");
    bar->setCurrentRangeStart(120.0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    require(rig.grid.scrollPosition() == 120, "and dragging the bar scrolls the grid");
    std::cout << "PASS: the horizontal scroll bar and the grid agree\n";
}

// The selected channel keeps pointing at the same channel as others come and go.
static void selectionFollowsChannelEdits() {
    Rig rig;
    auto& g = rig.grid;
    for (int i = 0; i < 2; ++i) g.addChannelWithSound("cowbell");
    require(rig.rack().channelCount == 6 && g.selectedChannel() == 5, "a new channel is selected");
    g.selectChannel(4);
    rig.controller.editRack("Name", [](Project& p) { return p.renameChannel(4, "Mine"); });
    g.runChannelCommand(1, ChannelRackGrid::cmdDelete);
    require(rig.rack().channelCount == 5 && g.selectedChannel() == 3 && rig.rack().info[3].name == "Mine",
            "deleting a channel above the selected one keeps the same channel selected");
    g.runChannelCommand(4, ChannelRackGrid::cmdDelete);
    require(g.selectedChannel() == 3 && rig.rack().info[3].name == "Mine", "deleting one below it changes nothing");
    g.runChannelCommand(3, ChannelRackGrid::cmdDelete);
    require(rig.rack().channelCount == 3 && g.selectedChannel() == 2, "deleting the selected channel keeps a valid selection");
    g.runChannelCommand(0, ChannelRackGrid::cmdDelete);
    require(g.selectedChannel() == 1, "and deleting the first channel shifts it down with its neighbours");

    // After the rack shrinks behind the view's back (undo), the selection is pulled back inside it.
    Rig undo;
    undo.grid.addChannelWithSound("cowbell");
    undo.grid.addChannelWithSound("cowbell");
    require(undo.grid.selectedChannel() == 5, "the newest channel is selected");
    require(undo.controller.undo() && undo.controller.undo() && undo.rack().channelCount == 4, "undoing both additions");
    undo.view.refresh();
    require(undo.grid.selectedChannel() == 3, "the selection moves to the last channel that is left");
    std::cout << "PASS: the selection follows deleted and undone channels\n";
}

static void tallRackScrolls() {
    Rig rig(1180, 560);
    auto& g = rig.grid;
    for (int i = 0; i < 12; ++i) g.addChannelWithSound("cowbell");
    require(rig.rack().channelCount == maxChannels, "sixteen channels");
    require(g.maxScrollY() > 40 && g.rowHeight() == ChannelRackGrid::minRow, "sixteen channels do not fit a 560 px window: the rows scroll");
    require(g.getBottom() <= rig.graph.getY() && rig.graph.getBottom() <= rig.view.getHeight(), "and the graph does not overlap the grid");
    auto* bar = verticalBarOf(rig.view);
    require(bar != nullptr && bar->isVisible() && bar->getX() == g.getRight() && bar->getBottom() <= g.getBottom() && bar->getY() >= g.getY() + ChannelRackGrid::rulerHeight,
            "a vertical scroll bar appears beside the rows");
    // Adding the last channel scrolled it into view.
    require(g.selectedChannel() == 15 && g.rowBounds(15).getBottom() <= g.getHeight() && g.rowBounds(15).getY() >= ChannelRackGrid::rulerHeight,
            "a new channel is scrolled into view");
    require(g.scrollPositionY() == g.maxScrollY() && static_cast<int>(bar->getCurrentRangeStart()) == g.scrollPositionY(), "at the bottom, and the bar agrees");
    // Clicks land on the row they show, wherever it has scrolled to.
    rig.click(rig.cellCentre(15, 3));
    require(rig.step(15, 3).on && rig.totalOn() == 1, "a click on the last row paints the last channel");
    require(g.rowAt(rig.cellCentre(15, 3).y) == 15 && g.rowAt(ChannelRackGrid::rulerHeight + 1) == g.scrollPositionY() / g.rowHeight(), "rows map through the scroll");
    // The wheel scrolls the rows.
    int movedTo = -1;
    g.onScrolledY = [&](int y) { movedTo = y; };
    juce::MouseWheelDetails up{}; up.deltaY = 0.05f;
    const int before = g.scrollPositionY();
    g.mouseWheelMove(mouse(g, {700, 100}), up);
    require(g.scrollPositionY() == before - 20 && movedTo == g.scrollPositionY(), "the wheel scrolls the rows");
    g.setScrollPositionY(-100);
    require(g.scrollPositionY() == 0 && g.rowBounds(0).getY() == ChannelRackGrid::rulerHeight, "and stops at the top");
    g.setScrollPositionY(100000);
    require(g.scrollPositionY() == g.maxScrollY(), "and at the bottom");
    g.scrollRowIntoView(0);
    require(g.scrollPositionY() == 0 && g.rowBounds(0).getY() == ChannelRackGrid::rulerHeight, "scrolling a row into view brings it to the top");
    g.scrollRowIntoView(4);
    require(g.scrollPositionY() == 0, "a row that is already visible does not move the view");
    g.scrollRowIntoView(15);
    require(g.scrollPositionY() == g.maxScrollY() && g.rowBounds(15).getBottom() <= g.getHeight(), "a row below the view scrolls up just far enough");
    g.setScrollPositionY(0);
    g.scrollRowIntoView(14);                           // below the view, but not the last row
    require(g.scrollPositionY() == 15 * g.rowHeight() - g.visibleRowsHeight() && g.rowBounds(14).getBottom() == g.getHeight(),
            "a row below the view scrolls just far enough to show it at the bottom edge, not all the way to the top");
    g.setScrollPositionY(100);
    g.scrollRowIntoView(3);
    require(g.scrollPositionY() == 3 * g.rowHeight() && g.rowBounds(3).getY() == ChannelRackGrid::rulerHeight, "a row above the view scrolls down to it");
    g.setScrollPositionY(0);
    // Dragging the scroll bar moves the rows.
    bar->setCurrentRangeStart(0.0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    require(g.scrollPositionY() == 0, "dragging the scroll bar scrolls the rows");
    // A drag across the scrolled rows paints the cells it crosses.
    rig.controller.editRack("Clear", [](Project& p) { return p.clearPattern(p.selectedPattern); });
    g.setScrollPositionY(g.maxScrollY());
    rig.stroke(rig.cellCentre(13, 2), {rig.cellCentre(15, 2)});
    require(rig.step(13, 2).on && rig.step(14, 2).on && rig.step(15, 2).on && rig.totalOn() == 3, "a vertical drag works on scrolled rows");
    // The ruler stays inert while the rows scroll under it.
    const auto frozen = rig.rack();
    rig.click({ChannelRackGrid::headerWidth + 40, 5});
    require(rig.rack() == frozen, "clicking the ruler changes nothing");
    // Removing channels takes the scrolling away again.
    for (int i = 0; i < 12; ++i) g.runChannelCommand(g.selectedChannel(), ChannelRackGrid::cmdDelete);
    rig.view.refresh();
    require(rig.rack().channelCount == 4 && g.maxScrollY() == 0 && g.scrollPositionY() == 0 && !bar->isVisible(), "with few channels the scrolling goes away");
    std::cout << "PASS: sixteen channels scroll vertically\n";
}

// ---- painting ---------------------------------------------------------------------
static void paintAndEraseStrokes() {
    Rig rig;
    auto& g = rig.grid;
    // A click paints one step and selects its channel.
    rig.click(rig.cellCentre(1, 4));
    require(rig.step(1, 4).on && rig.onCount(1) == 1 && g.selectedChannel() == 1, "a click paints a step and selects the channel");
    require(rig.controller.undoLabel() == "Add steps", "and is one undo step");
    rig.click(rig.cellCentre(1, 4));
    require(!rig.step(1, 4).on && rig.controller.undoLabel() == "Remove steps", "a second click erases it");

    // A drag paints every cell between press and release, as one undo step.
    rig.stroke(rig.cellCentre(0, 2), {rig.cellCentre(0, 5), rig.cellCentre(0, 9)});
    bool run = true;
    for (int s = 0; s < 16; ++s) run = run && (rig.step(0, s).on == (s >= 2 && s <= 9));
    require(run && rig.onCount(0) == 8, "dragging paints steps 3 to 10 and nothing else");
    require(rig.controller.undo() && rig.onCount(0) == 0 && !rig.step(1, 4).on, "the whole stroke undoes together");
    require(rig.controller.redo() && rig.onCount(0) == 8, "and redoes together");

    // A fast drag that skips cells still leaves no gaps.
    rig.stroke(rig.cellCentre(2, 0), {rig.cellCentre(2, 15)});
    require(rig.onCount(2) == 16, "a jump across the row paints all 16 steps");

    // Starting on a painted step erases while dragging, and only touches what the stroke passes.
    rig.stroke(rig.cellCentre(0, 5), {rig.cellCentre(0, 7)});
    require(!rig.step(0, 5).on && !rig.step(0, 6).on && !rig.step(0, 7).on && rig.step(0, 4).on && rig.step(0, 8).on,
            "pressing a painted step erases along the drag");
    // Dragging back over cells does not undo the stroke's own work.
    rig.stroke(rig.cellCentre(3, 0), {rig.cellCentre(3, 6), rig.cellCentre(3, 2)});
    require(rig.step(3, 0).on && rig.step(3, 6).on && rig.step(3, 2).on && rig.onCount(3) == 7, "a stroke that turns back keeps what it painted");

    // A drag across rows paints each row it crosses.
    rig.stroke(rig.cellCentre(0, 12), {rig.cellCentre(3, 12)});
    require(rig.step(0, 12).on && rig.step(1, 12).on && rig.step(2, 12).on && rig.step(3, 12).on, "vertical drags paint the cells they pass");

    // Right button always erases, even from an empty cell, and only along its path.
    const int others = rig.onCount(0) + rig.onCount(1) + rig.onCount(3);
    rig.stroke(rig.cellCentre(2, 0), {rig.cellCentre(2, 15)}, rightButton);
    require(rig.onCount(2) == 0 && rig.onCount(0) + rig.onCount(1) + rig.onCount(3) == others, "right-drag erases a whole row and leaves the others");
    rig.click(rig.cellCentre(2, 9), rightButton);
    require(!rig.step(2, 9).on, "a right-click on an empty step does not paint");
    rig.click(rig.cellCentre(3, 0), rightButton);
    require(!rig.step(3, 0).on, "a right-click on a painted step erases it");

    // Dragging beyond the edges stays on the grid.
    rig.stroke(rig.cellCentre(1, 14), {{g.getWidth() + 80, rig.cellCentre(1, 14).y}});
    require(rig.step(1, 14).on && rig.step(1, 15).on, "dragging past the right edge reaches the last step");
    bool hiddenOff = true;
    for (int s = 16; s < 64; ++s) hiddenOff = hiddenOff && !rig.step(1, s).on;
    require(hiddenOff, "...and never paints steps beyond the pattern's length");
    rig.stroke(rig.cellCentre(1, 1), {{5, rig.cellCentre(1, 1).y}});
    require(rig.step(1, 0).on, "dragging into the header reaches the first step");
    rig.stroke(rig.cellCentre(2, 3), {{rig.cellCentre(2, 3).x, g.getHeight() + 200}});
    require(rig.step(2, 3).on && rig.step(3, 3).on && rig.rack().channelCount == 4, "dragging below the rack stops at the last channel");

    // Presses on the ruler, the empty area and the right margin do nothing.
    const auto snapshot = rig.rack();
    rig.click({ChannelRackGrid::headerWidth + 30, 5});
    rig.click({ChannelRackGrid::headerWidth + 30, g.rowBounds(3).getBottom() + 3});
    rig.click({g.getWidth() - 2, rig.cellCentre(0, 0).y});
    require(rig.rack() == snapshot, "clicks outside the cells change nothing");

    // Painting previews the sound of the channel at the step's velocity.
    NoteQueue::Event event{};
    while (rig.engine.rackAuditions.pop(event)) {}
    rig.click(rig.cellCentre(0, 14));
    require(rig.engine.rackAuditions.pop(event) && event.note == 0 && event.velocity > 0.0f, "painting a step plays the channel");
    while (rig.engine.rackAuditions.pop(event)) {}
    rig.click(rig.cellCentre(0, 14));      // erasing is silent
    require(!rig.engine.rackAuditions.pop(event), "erasing a step makes no sound");
    std::cout << "PASS: paint, erase and drag strokes\n";
}

// ---- the channel header -------------------------------------------------------------
static void headerControls() {
    Rig rig;
    auto& g = rig.grid;
    auto centre = [](juce::Rectangle<int> r) { return r.getCentre(); };
    int structure = 0, selection = 0;
    g.onStructureChanged = [&] { ++structure; };
    g.onSelectionChanged = [&] { ++selection; };

    // Mute light: left click mutes, right click solos.
    rig.click(centre(g.muteRect(1)));
    require(rig.rack().channels[1].mute && !rig.rack().audible(1) && rig.rack().audible(0) && g.selectedChannel() == 1 && selection == 1,
            "the light mutes the channel (and selects it)");
    rig.click(centre(g.muteRect(1)));
    require(!rig.rack().channels[1].mute, "and unmutes it");
    rig.click(centre(g.muteRect(2)), rightButton);
    require(rig.rack().channels[2].solo && !rig.rack().audible(0) && rig.rack().audible(2), "right-clicking the light solos");
    rig.click(centre(g.soloRect(2)));
    require(!rig.rack().channels[2].solo && rig.rack().audible(0), "the S chip toggles solo");
    rig.click(centre(g.soloRect(3)));
    require(rig.rack().channels[3].solo && !rig.rack().audible(2), "the S chip solos another channel");
    rig.click(centre(g.soloRect(3)));
    require(rig.rack().audible(0) && rig.rack().audible(2) && rig.rack().audible(3), "and un-solos it");

    // Name: a click selects and previews.
    NoteQueue::Event event{};
    while (rig.engine.rackAuditions.pop(event)) {}
    rig.click(centre(g.nameRect(2)));
    require(g.selectedChannel() == 2 && rig.engine.rackAuditions.pop(event) && event.note == 2 && event.velocity > 0.0f, "clicking a name selects it and plays it");

    // Volume knob: dragging up raises it; the whole drag is one undo step.
    const float start = rig.rack().channels[0].volume;
    const auto knob = centre(g.volumeRect(0));
    g.mouseDown(mouse(g, knob));
    g.mouseDrag(mouse(g, {knob.x, knob.y - 10}, leftButton, 1, true));
    g.mouseDrag(mouse(g, {knob.x, knob.y - 30}, leftButton, 1, true));
    g.mouseUp(mouse(g, {knob.x, knob.y - 30}));
    require(rig.rack().channels[0].volume > start + 0.1f && rig.rack().channels[0].volume <= 1.0f, "dragging up raises the volume");
    require(rig.controller.undoLabel() == "Change channel volume", "and is labelled for the undo menu");
    require(rig.controller.undo() && rig.rack().channels[0].volume == start, "a whole drag is one undo step");
    g.mouseDown(mouse(g, knob));
    g.mouseDrag(mouse(g, {knob.x, knob.y + 300}, leftButton, 1, true));
    g.mouseUp(mouse(g, knob));
    require(rig.rack().channels[0].volume == 0.0f, "dragging down stops at silence");
    g.mouseDown(mouse(g, knob));
    g.mouseDrag(mouse(g, {knob.x, knob.y - 5000}, leftButton, 1, true));
    g.mouseUp(mouse(g, knob));
    require(rig.rack().channels[0].volume == 1.0f, "and dragging up stops at the top");
    rig.press(g, knob, leftButton, 2);
    require(rig.rack().channels[0].volume == defaultChannelVolume, "double-click resets the volume");
    // Shift gives a finer drag.
    const juce::ModifierKeys shift(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    g.mouseDown(mouse(g, knob, shift));
    g.mouseDrag(mouse(g, {knob.x, knob.y - 20}, shift, 1, true));
    g.mouseUp(mouse(g, knob, shift));
    require(rig.rack().channels[0].volume > defaultChannelVolume && rig.rack().channels[0].volume < defaultChannelVolume + 0.06f, "shift-drag is a fine adjustment");
    rig.press(g, knob, leftButton, 2);

    // Pan knob: dragging up pans right and snaps to centre near zero; double-click centres.
    const auto pan = centre(g.panRect(1));
    g.mouseDown(mouse(g, pan));
    g.mouseDrag(mouse(g, {pan.x, pan.y - 1}, leftButton, 1, true));
    require(rig.rack().channels[1].pan == 0.0f, "a tiny pan move snaps to the centre");
    g.mouseDrag(mouse(g, {pan.x, pan.y - 30}, leftButton, 1, true));
    g.mouseUp(mouse(g, {pan.x, pan.y - 30}));
    require(rig.rack().channels[1].pan > 0.3f, "dragging up pans right");
    g.mouseDown(mouse(g, pan));
    g.mouseDrag(mouse(g, {pan.x, pan.y + 400}, leftButton, 1, true));
    g.mouseUp(mouse(g, pan));
    require(rig.rack().channels[1].pan == -1.0f, "and dragging down pans hard left");
    rig.press(g, pan, leftButton, 2);
    require(rig.rack().channels[1].pan == 0.0f, "double-click centres the pan");

    // Wheel on a knob.
    juce::MouseWheelDetails wheel{}; wheel.deltaY = 0.5f;
    g.mouseWheelMove(mouse(g, centre(g.volumeRect(3))), wheel);
    require(rig.rack().channels[3].volume > defaultChannelVolume, "the wheel turns a volume knob up");
    wheel.deltaY = -0.5f;
    g.mouseWheelMove(mouse(g, centre(g.panRect(3))), wheel);
    require(rig.rack().channels[3].pan < 0.0f, "and a pan knob left");

    // The channel menu: what it offers and every command.
    const auto menuTexts = entries(g.channelMenu(0));
    for (const int id : {ChannelRackGrid::cmdRename, ChannelRackGrid::cmdMute, ChannelRackGrid::cmdSolo, ChannelRackGrid::cmdClearSteps, ChannelRackGrid::cmdMoveUp,
                         ChannelRackGrid::cmdMoveDown, ChannelRackGrid::cmdDelete, ChannelRackGrid::cmdSilent, ChannelRackGrid::cmdLoadSample,
                         ChannelRackGrid::outputMaster, ChannelRackGrid::chokeNone})
        require(entry(menuTexts, id) != nullptr, "the channel menu offers every command");
    for (std::size_t i = 0; i < builtinSounds.size(); ++i) require(entry(menuTexts, ChannelRackGrid::builtinBase + static_cast<int>(i)) != nullptr, "and every built-in sound");
    for (int t = 0; t < rig.controller.document().trackCount(); ++t) require(entry(menuTexts, ChannelRackGrid::outputBase + t) != nullptr, "and every Session track");
    for (int group = 0; group < maxChokeGroup; ++group) require(entry(menuTexts, ChannelRackGrid::chokeBase + group) != nullptr, "and every choke group");
    require(!entry(menuTexts, ChannelRackGrid::cmdMoveUp)->enabled && entry(menuTexts, ChannelRackGrid::cmdMoveDown)->enabled, "the first channel cannot move up");
    require(!entry(entries(g.channelMenu(3)), ChannelRackGrid::cmdMoveDown)->enabled, "and the last cannot move down");
    require(entry(menuTexts, ChannelRackGrid::builtinBase)->ticked && entry(menuTexts, ChannelRackGrid::outputMaster)->ticked && entry(menuTexts, ChannelRackGrid::chokeNone)->ticked,
            "the menu ticks the current sound, output and choke group");
    require(entries(g.channelMenu(99)).empty() && entries(g.outputMenu(-1)).empty(), "there is no menu for a missing channel");

    ShownMenu shown;
    g.menuLauncher = [&](juce::PopupMenu menu, juce::Rectangle<int> area, std::function<void(int)> done) {
        shown.shown = true; shown.menu = std::move(menu); shown.area = area; shown.done = std::move(done);
    };
    rig.click(centre(g.nameRect(1)), rightButton);
    require(shown.shown && shown.area == g.nameRect(1) && g.selectedChannel() == 1 && entry(entries(shown.menu), ChannelRackGrid::cmdRename) != nullptr,
            "right-clicking a name opens the channel menu beside it");
    shown.done(ChannelRackGrid::cmdMute);
    require(rig.rack().channels[1].mute, "choosing Mute mutes the channel");
    shown.reset();
    rig.click(centre(g.panRect(2)), rightButton);
    require(shown.shown && shown.area == g.nameRect(2), "right-clicking a knob opens the menu too");
    const auto before = rig.rack();
    shown.done(0);
    require(rig.rack() == before, "dismissing the menu changes nothing");
    shown.reset();
    rig.click(centre(g.routeRect(2)));
    const auto routeEntries = entries(shown.menu);
    require(shown.shown && shown.area == g.routeRect(2) && entry(routeEntries, ChannelRackGrid::outputMaster) != nullptr && entry(routeEntries, ChannelRackGrid::outputMaster)->ticked
            && entry(routeEntries, ChannelRackGrid::cmdRename) == nullptr, "the M chip opens a short menu of outputs");
    shown.done(ChannelRackGrid::outputBase + 1);
    require(rig.rack().channels[2].output == 1, "choosing a track routes the channel to it");
    shown.reset();
    rig.click(centre(g.routeRect(2)));
    require(entry(entries(shown.menu), ChannelRackGrid::outputBase + 1)->ticked, "and the menu ticks it next time");
    shown.reset();
    g.menuLauncher = nullptr;

    // Rename: double-click a name asks for a new one.
    juce::String asked, current;
    juce::String reply = "Boom";
    g.nameRequester = [&](const juce::String& title, const juce::String& now, std::function<void(const juce::String&)> done) {
        asked = title; current = now; done(reply);
    };
    rig.press(g, centre(g.nameRect(0)), leftButton, 2);
    require(asked == "Rename channel" && current == "Kick" && rig.rack().info[0].name == "Boom", "double-clicking a name renames the channel");
    require(rig.controller.undoLabel() == "Rename channel" && rig.controller.undo() && rig.rack().info[0].name == "Kick", "and it can be undone");
    const int structureBefore = structure;
    reply = "   ";
    g.runChannelCommand(0, ChannelRackGrid::cmdRename);
    require(rig.rack().info[0].name == "Kick", "a blank name is refused");
    reply = "  Spaced out  ";
    g.runChannelCommand(0, ChannelRackGrid::cmdRename);
    require(rig.rack().info[0].name == "Spaced out" && structure > structureBefore, "names are trimmed and the screen is told");
    g.nameRequester = [&](const juce::String&, const juce::String&, std::function<void(const juce::String&)>) { /* cancelled */ };
    const auto beforeCancel = rig.rack();
    g.runChannelCommand(0, ChannelRackGrid::cmdRename);
    require(rig.rack() == beforeCancel, "cancelling the rename changes nothing");
    g.nameRequester = nullptr;

    // The rest of the menu commands.
    g.runChannelCommand(0, ChannelRackGrid::cmdMute);
    require(rig.rack().channels[0].mute, "menu: mute");
    g.runChannelCommand(0, ChannelRackGrid::cmdMute);
    g.runChannelCommand(1, ChannelRackGrid::cmdSolo);
    require(rig.rack().channels[1].solo, "menu: solo");
    g.runChannelCommand(1, ChannelRackGrid::cmdSolo);
    g.runChannelCommand(0, ChannelRackGrid::builtinBase + 5);
    require(rig.rack().info[0].sample == "builtin:tom" && rig.rack().info[0].name == "Tom", "menu: choose a built-in sound (renames the channel)");
    while (rig.engine.rackAuditions.pop(event)) {}
    g.runChannelCommand(0, ChannelRackGrid::builtinBase + 1);
    require(rig.engine.rackAuditions.pop(event) && event.note == 0, "and the new sound is played");
    g.runChannelCommand(0, ChannelRackGrid::cmdSilent);
    require(rig.rack().info[0].sample.empty(), "menu: silent");
    g.runChannelCommand(1, ChannelRackGrid::outputBase + 2);
    require(rig.rack().channels[1].output == 2, "menu: route to track 3");
    g.runChannelCommand(1, ChannelRackGrid::outputMaster);
    require(rig.rack().channels[1].output == masterOutput, "menu: route to master");
    g.runChannelCommand(2, ChannelRackGrid::chokeBase + 2);
    require(rig.rack().channels[2].chokeGroup == 3, "menu: choke group 3");
    g.runChannelCommand(2, ChannelRackGrid::chokeNone);
    require(rig.rack().channels[2].chokeGroup == 0, "menu: no choke group");
    g.runChannelCommand(99, ChannelRackGrid::cmdDelete);
    g.runChannelCommand(-1, ChannelRackGrid::cmdMute);
    require(rig.rack().channelCount == 4, "commands for a missing channel do nothing");
    rig.click(rig.cellCentre(3, 0));
    g.runChannelCommand(3, ChannelRackGrid::cmdClearSteps);
    require(rig.onCount(3) == 0, "menu: clear the channel's steps");
    const auto nameOf = [&](int c) { return rig.rack().info[static_cast<std::size_t>(c)].name; };
    const auto first = nameOf(0), second = nameOf(1);
    g.runChannelCommand(0, ChannelRackGrid::cmdMoveDown);
    require(nameOf(0) == second && nameOf(1) == first && g.selectedChannel() == 1, "menu: move down (selection follows)");
    g.runChannelCommand(1, ChannelRackGrid::cmdMoveUp);
    require(nameOf(0) == first && g.selectedChannel() == 0, "menu: move up");
    g.runChannelCommand(0, ChannelRackGrid::cmdMoveUp);
    require(nameOf(0) == first && g.selectedChannel() == 0, "moving the first channel up does nothing");
    rig.click(rig.cellCentre(2, 5));
    g.runChannelCommand(3, ChannelRackGrid::cmdDelete);
    require(rig.rack().channelCount == 3 && g.selectedChannel() <= 2, "menu: delete a channel");
    g.runChannelCommand(0, ChannelRackGrid::cmdDelete);
    require(rig.rack().channelCount == 2 && g.selectedChannel() <= 1, "menu: delete the first channel (selection stays valid)");
    require(rig.controller.undo() && rig.rack().channelCount == 3, "deleting is undoable");
    g.addChannelWithSound("cowbell");
    require(rig.rack().channelCount == 4 && rig.rack().info[3].sample == "builtin:cowbell" && rig.rack().info[3].name == "Cowbell" && g.selectedChannel() == 3,
            "adding a drum channel selects it");
    g.addChannelWithSound("no such sound");
    require(rig.rack().channelCount == 4, "an unknown sound adds nothing");
    for (int i = 0; i < 20; ++i) g.addChannelWithSound("kick");
    require(rig.rack().channelCount == maxChannels, "the rack stops at sixteen channels");
    require(rig.controller.undo() && rig.rack().channelCount == maxChannels - 1, "each add is undoable");
    std::cout << "PASS: mute/solo lights, knobs, name clicks, menus and rename\n";
}

// ---- graph editor ----------------------------------------------------------------------
static void graphEditor() {
    Rig rig;
    auto& g = rig.grid;
    auto& graph = rig.graph;
    for (int s : {0, 4, 8, 12}) rig.click(rig.cellCentre(0, s));
    g.selectChannel(0);
    const auto plot = graph.plot();
    const auto xOf = [&](int s) { return g.stepLeft(s) + g.cellWidth() / 2; };
    require(plot.getX() == ChannelRackGrid::headerWidth && graph.field() == StepField::velocity, "the graph lines up with the grid and starts on velocity");
    require(graph.valueAtY(plot.getY()) == 127 && graph.valueAtY(plot.getBottom()) == 1 && graph.valueAtY(plot.getCentreY()) > 55 && graph.valueAtY(plot.getCentreY()) < 72,
            "velocity runs 1 to 127 top to bottom");
    for (int y = plot.getY(); y < plot.getBottom(); ++y) require(graph.valueAtY(y) >= graph.valueAtY(y + 1), "higher on screen means a higher value");
    require(graph.valueAtY(-500) == 127 && graph.valueAtY(5000) == 1, "positions beyond the plot clamp to the ends");

    auto press = [&](juce::Point<int> p, juce::ModifierKeys mods = leftButton) { graph.mouseDown(mouse(graph, p, mods)); };
    auto drag = [&](juce::Point<int> p) { graph.mouseDrag(mouse(graph, p, leftButton, 1, true)); };
    auto release = [&](juce::Point<int> p) { graph.mouseUp(mouse(graph, p)); };

    // Click on a bar sets its velocity; the other bars are unchanged.
    press({xOf(4), plot.getY() + plot.getHeight() / 4});
    release({xOf(4), plot.getY() + plot.getHeight() / 4});
    const int v = rig.step(0, 4).velocity;
    require(v > 85 && v < 105 && rig.step(0, 0).velocity == defaultVelocity && rig.step(0, 8).velocity == defaultVelocity, "a click sets one step's velocity");
    // Drag across: a line from the first value to the last, steps in between interpolated, off steps untouched.
    press({xOf(0), plot.getY() + 4});
    drag({xOf(12), plot.getBottom() - 4});
    release({xOf(12), plot.getBottom() - 4});
    require(rig.step(0, 0).velocity >= 120 && rig.step(0, 12).velocity <= 8 && rig.step(0, 4).velocity > rig.step(0, 8).velocity && rig.step(0, 8).velocity > rig.step(0, 12).velocity,
            "dragging draws a line through the steps it crosses");
    require(rig.step(0, 5).velocity == defaultVelocity && !rig.step(0, 5).on, "steps that are off keep their values");
    require(rig.controller.undoLabel() == "Edit step velocity", "labelled for the undo menu");
    const auto drawn = rig.step(0, 8).velocity;
    require(rig.controller.undo() && rig.step(0, 8).velocity != drawn && rig.step(0, 4).velocity == v && rig.step(0, 0).velocity == defaultVelocity,
            "a whole drawing stroke undoes in one step");
    require(rig.controller.redo() && rig.step(0, 8).velocity == drawn, "and redoes in one step");
    // Dragging right to left works too.
    press({xOf(12), plot.getY() + 4});
    drag({xOf(0), plot.getBottom() - 4});
    release({xOf(0), plot.getBottom() - 4});
    require(rig.step(0, 12).velocity >= 120 && rig.step(0, 0).velocity <= 8 && rig.step(0, 4).velocity < rig.step(0, 8).velocity, "and in the other direction");
    // Right click resets to the default, and never draws while dragging.
    press({xOf(0), plot.getCentreY()}, rightButton);
    graph.mouseDrag(mouse(graph, {xOf(8), plot.getY()}, rightButton, 1, true));
    release({xOf(8), plot.getY()});
    require(rig.step(0, 0).velocity == defaultVelocity && rig.step(0, 8).velocity != 127, "right-click resets a bar and a right-drag draws nothing");

    // Pan: centre snaps to zero, extremes reach +-63.
    graph.setField(StepField::pan);
    press({xOf(8), plot.getY()}); release({xOf(8), plot.getY()});
    require(rig.step(0, 8).pan == 63, "pan: top is hard right");
    press({xOf(8), plot.getBottom()}); release({xOf(8), plot.getBottom()});
    require(rig.step(0, 8).pan == -63, "pan: bottom is hard left");
    press({xOf(8), plot.getCentreY() + 1}); release({xOf(8), plot.getCentreY() + 1});
    require(rig.step(0, 8).pan == 0, "pan: the middle snaps to centre");
    // Pitch.
    graph.setField(StepField::pitch);
    press({xOf(12), plot.getY()}); release({xOf(12), plot.getY()});
    require(rig.step(0, 12).pitch == 24, "pitch: top is +24 semitones");
    press({xOf(12), graph.yForValue(-7)}); release({xOf(12), graph.yForValue(-7)});
    require(rig.step(0, 12).pitch == -7, "pitch: a bar lands on the semitone it was drawn at");
    // Probability.
    graph.setField(StepField::probability);
    press({xOf(4), graph.yForValue(40)}); release({xOf(4), graph.yForValue(40)});
    require(rig.step(0, 4).probability == 40, "chance: a bar is a percentage");
    press({xOf(4), plot.getY() + 1}, rightButton); release({xOf(4), plot.getY() + 1});
    require(rig.step(0, 4).probability == 100, "chance: right-click restores 100%");
    for (const auto field : {StepField::velocity, StepField::pan, StepField::pitch, StepField::probability}) {
        graph.setField(field);
        for (const int value : {fieldMinimum(field), fieldDefault(field), fieldMaximum(field)})
            require(graph.yForValue(value) >= plot.getY() && graph.yForValue(value) <= plot.getBottom(), "values map inside the plot");
        require(graph.yForValue(fieldMaximum(field)) < graph.yForValue(fieldMinimum(field)), "and higher values sit higher");
    }
    // Off steps and the area left of the grid are not editable.
    const auto frozen = rig.rack();
    press({xOf(2), plot.getCentreY()}); release({xOf(2), plot.getCentreY()});
    press({20, plot.getCentreY()}); release({20, plot.getCentreY()});
    require(rig.rack() == frozen, "off steps and the header do not respond");
    // Choosing another channel edits that channel.
    rig.click(rig.cellCentre(1, 2));
    graph.setField(StepField::velocity);
    press({xOf(2), plot.getY() + 4}); release({xOf(2), plot.getY() + 4});
    require(rig.step(1, 2).velocity >= 120 && rig.step(0, 2).velocity == defaultVelocity, "the graph edits the selected channel");
    // The tab buttons switch what the graph draws.
    int tabs = 0;
    for (auto* child : graph.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(child)) { ++tabs; clickButton(*b); }
    require(tabs == 4 && graph.field() == StepField::probability, "four tabs choose velocity, pan, pitch or chance");
    std::cout << "PASS: graph editor draws velocity, pan, pitch and chance\n";
}

// ---- pattern bank and toolbar ----------------------------------------------------------
static void patternBankAndToolbar() {
    Rig rig;
    auto& v = rig.view;
    int titles = 0;
    v.onTitleChanged = [&] { ++titles; };
    rig.click(rig.cellCentre(0, 0));
    v.patternCommand(ChannelRackView::clonePattern);
    require(rig.rack().patternCount == 2 && rig.rack().selectedPattern == 1 && rig.step(0, 0).on && titles > 0, "clone copies the pattern and selects the copy");
    rig.click(rig.cellCentre(0, 1));
    require(rig.step(0, 1).on && !rig.rack().pattern(0).steps[0][1].on, "editing the clone leaves the original alone");
    v.patternCommand(ChannelRackView::newPattern);
    require(rig.rack().patternCount == 3 && rig.onCount(0) == 0 && rig.rack().selectedPattern == 2, "a new pattern is empty");
    rig.click(rig.cellCentre(2, 3));
    v.patternCommand(ChannelRackView::clearPattern);
    require(rig.onCount(2) == 0 && rig.totalOn() == 0, "clear empties the pattern");
    rig.controller.undo();
    require(rig.onCount(2) == 1, "and clearing can be undone");
    rig.controller.selectRackPattern(0);
    v.refresh();
    require(rig.step(0, 0).on && !rig.step(0, 1).on, "selecting a pattern shows its steps");

    // Rename goes through the same dialog seam as channels.
    rig.grid.nameRequester = [&](const juce::String& title, const juce::String& current, std::function<void(const juce::String&)> done) {
        require(title == "Rename pattern" && current == "Pattern 1", "the pattern dialog starts with the pattern's name");
        done("Groove A");
    };
    v.patternCommand(ChannelRackView::renamePattern);
    rig.grid.nameRequester = nullptr;
    require(rig.rack().patternNames[0] == "Groove A" && v.subtitle().contains("Groove A"), "renaming a pattern shows in the subtitle");

    v.patternCommand(ChannelRackView::deletePattern);
    require(rig.rack().patternCount == 2, "delete removes the selected pattern");
    while (rig.rack().patternCount > 1) v.patternCommand(ChannelRackView::deletePattern);
    v.patternCommand(ChannelRackView::deletePattern);
    require(rig.rack().patternCount == 1, "the last pattern cannot be deleted");
    require(!entry(entries(v.patternMenu()), ChannelRackView::deletePattern)->enabled, "and its menu entry is greyed out");
    require(v.subtitle().contains("Pattern") && v.subtitle().contains("16 steps") && v.subtitle().contains("swing 0%"), "the subtitle describes the pattern");
    rig.controller.editRack("Swing", [](Project& p) { return p.setSwing(0.5f); });
    require(v.subtitle().contains("swing 50%"), "and the swing");

    // Toolbar widgets reflect and edit the document.
    juce::Slider* swing = nullptr;
    juce::ComboBox *length = nullptr, *patterns = nullptr;
    juce::TextButton *newButton = nullptr, *menuButton = nullptr, *addButton = nullptr;
    for (auto* child : v.getChildren()) {
        if (auto* s = dynamic_cast<juce::Slider*>(child)) swing = s;
        if (auto* b = dynamic_cast<juce::ComboBox*>(child)) { if (b->getName() == "Pattern length") length = b; if (b->getName() == "Pattern") patterns = b; }
        if (auto* b = dynamic_cast<juce::TextButton*>(child)) {
            if (b->getName() == "New pattern") newButton = b;
            if (b->getName() == "Pattern menu") menuButton = b;
            if (b->getName() == "Add channel") addButton = b;
        }
    }
    require(swing && length && patterns && newButton && menuButton && addButton, "the toolbar has its widgets");
    v.refresh();
    require(swing->getValue() == 50.0, "the swing slider shows the swing");
    swing->setValue(25.0, juce::sendNotificationSync);
    require(std::abs(rig.rack().swing - 0.25f) < 1e-6f, "moving the swing slider edits the rack");
    swing->setValue(30.0, juce::sendNotificationSync);
    swing->setValue(35.0, juce::sendNotificationSync);
    require(rig.controller.undoLabel() == "Change swing" && rig.controller.undo() && std::abs(rig.rack().swing - 0.5f) < 1e-6f, "a swing drag is one undo step");
    v.refresh();
    require(swing->getValue() == 50.0, "and the slider follows undo");
    swing->setValue(0.0, juce::sendNotificationSync);

    require(length->getSelectedId() == 16, "the length box shows 16 steps");
    length->setSelectedId(32, juce::sendNotificationSync);
    require(rig.rack().pattern(rig.rack().selectedPattern).length == 32 && rig.grid.length() == 32, "choosing 32 steps lengthens the pattern");
    length->setSelectedId(64, juce::sendNotificationSync);
    require(rig.grid.length() == 64, "and 64");
    length->setSelectedId(16, juce::sendNotificationSync);
    require(rig.grid.length() == 16, "and back to 16");

    clickButton(*newButton);
    require(rig.rack().patternCount == 2 && patterns->getNumItems() == 2 && patterns->getSelectedId() == 2, "the + button adds a pattern and the chooser lists it");
    const auto labelBefore = rig.controller.undoLabel();
    patterns->setSelectedId(1, juce::sendNotificationSync);
    require(rig.rack().selectedPattern == 0, "choosing a pattern selects it");
    require(rig.controller.undoLabel() == labelBefore, "choosing a pattern is a view change, not an undo step");

    // The "..." and "+ Channel" menus.
    ShownMenu shown;
    v.menuLauncher = [&](juce::PopupMenu menu, std::function<void(int)> done) { shown.shown = true; shown.menu = std::move(menu); shown.done = std::move(done); };
    clickButton(*menuButton);
    auto items = entries(shown.menu);
    require(shown.shown && items.size() == 5 && entry(items, ChannelRackView::deletePattern)->enabled, "the pattern menu offers its five commands");
    shown.done(ChannelRackView::newPattern);
    require(rig.rack().patternCount == 3, "and runs the one chosen");
    shown.reset();
    clickButton(*addButton);
    items = entries(shown.menu);
    require(shown.shown && items.size() == builtinSounds.size() + 1, "the + Channel menu lists every drum and a sampler");
    for (std::size_t i = 0; i < builtinSounds.size(); ++i)
        require(entry(items, ChannelRackView::addSoundBase + static_cast<int>(i))->text == builtinSounds[i].name, "in the order of the built-in sounds");
    shown.done(ChannelRackView::addSoundBase + 6);
    require(rig.rack().channelCount == 5 && rig.rack().info[4].sample == "builtin:rim" && rig.grid.selectedChannel() == 4, "choosing a drum adds it as a new channel");
    shown.done(0);
    require(rig.rack().channelCount == 5, "dismissing it adds nothing");
    for (int i = 0; i < 12; ++i) v.addChannelCommand(ChannelRackView::addSoundBase);
    require(rig.rack().channelCount == maxChannels && !addButton->isEnabled(), "the + Channel button turns off when the rack is full");
    require(!entry(entries(v.addChannelMenu()), ChannelRackView::addSampler)->enabled, "and so does the sampler entry");
    v.addChannelCommand(ChannelRackView::addSoundBase + 99);
    v.addChannelCommand(0);
    require(rig.rack().channelCount == maxChannels, "unknown commands do nothing");
    v.menuLauncher = nullptr;
    std::cout << "PASS: pattern bank, length, swing and the toolbar\n";
}

// ---- top bar -----------------------------------------------------------------------
static void topBarButton() {
    TopBar bar;
    bar.setSize(1400, TopBar::height);
    int chosen = -1;
    bar.onViewSelected = [&](int index) { chosen = index; };
    std::vector<juce::TextButton*> views;
    for (auto* child : bar.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getName().startsWith("Show ")) views.push_back(b);
    require(views.size() == 5, "the top bar has five view buttons");
    juce::TextButton* rackButton = nullptr;
    for (auto* b : views) if (b->getName() == "Show Rack view") rackButton = b;
    require(rackButton != nullptr, "one of them is the Rack");
    for (auto* b : views) {
        require(b->getRight() <= bar.getWidth() && b->getWidth() >= 50 && b->getHeight() > 16, "every view button fits on the bar");
        for (auto* other : views) if (other != b) require(!b->getBounds().intersects(other->getBounds()), "and none overlap");
    }
    clickButton(*rackButton);
    require(chosen == 8, "it opens the Channel Rack tab (index 8) without renumbering the others");
    const std::array<std::pair<const char*, int>, 4> others{{{"Show Steps view", 0}, {"Show Piano Roll view", 1}, {"Show Session view", 2}, {"Show Mixer view", 3}}};
    for (const auto& [name, tab] : others)
        for (auto* b : views) if (b->getName() == name) { clickButton(*b); require(chosen == tab, "the older view buttons keep their tabs"); }
    bar.setActiveView(8);
    require(rackButton->getToggleState(), "the button lights while the rack is shown");
    bar.setActiveView(4);
    require(!rackButton->getToggleState(), "and goes out for other views");
    bar.setActiveView(0);
    for (auto* b : views) require(b->getToggleState() == (b->getName() == "Show Steps view"), "only the shown view is lit");
    std::cout << "PASS: top-bar Rack button\n";
}

// ---- what you paint is what you hear ----------------------------------------------------
static void paintedPatternPlays() {
    Rig rig;
    rig.engine.patternMode.store(3);
    rig.engine.bpm.store(120);
    auto settle = [&] { rig.engine.playing.store(false); rig.render(60000); };    // lets previews and tails die away
    settle();
    require(rig.render(2048) == 0.0f, "nothing sounds before anything is painted");
    rig.click(rig.cellCentre(0, 0));                     // kick on step 1
    settle();
    rig.engine.playing.store(true);
    require(rig.render(2048) > 0.05f, "a step painted with the mouse plays when the transport runs");
    require(rig.engine.rackStep.load() == 0, "and the engine reports the step it is on");
    rig.render(6000 * 4);
    require(rig.engine.rackStep.load() >= 4, "the step display advances");
    settle();
    rig.click(rig.cellCentre(0, 0));                     // erase it again
    settle();
    rig.engine.playing.store(true);
    require(rig.render(24000) == 0.0f, "erasing the step silences it");
    settle();
    rig.click(rig.cellCentre(0, 0));
    settle();
    rig.click(rig.grid.muteRect(0).getCentre());         // mute light
    settle();
    rig.engine.playing.store(true);
    require(rig.render(24000) == 0.0f, "a muted channel is silent");
    settle();
    rig.click(rig.grid.muteRect(0).getCentre());
    rig.click(rig.grid.soloRect(1).getCentre());         // solo another channel
    settle();
    rig.engine.playing.store(true);
    require(rig.render(24000) == 0.0f, "soloing another channel silences this one");
    settle();
    rig.click(rig.grid.soloRect(1).getCentre());
    settle();
    rig.engine.playing.store(true);
    require(rig.render(2048) > 0.05f, "and everything returns when the solo is released");
    // A volume of zero is silent too.
    settle();
    const auto knob = rig.grid.volumeRect(0).getCentre();
    rig.grid.mouseDown(mouse(rig.grid, knob));
    rig.grid.mouseDrag(mouse(rig.grid, {knob.x, knob.y + 400}, leftButton, 1, true));
    rig.grid.mouseUp(mouse(rig.grid, knob));
    settle();
    rig.engine.playing.store(true);
    require(rig.render(24000) < 1e-4f, "a channel turned all the way down is silent");
    // Playhead repaint plumbing.
    rig.view.setPlayhead(7);
    rig.view.setPlayhead(7);
    rig.view.setPlayhead(-1);
    std::cout << "PASS: painted patterns are heard\n";
}

// ---- screenshots ----------------------------------------------------------------------
static void writePng(const juce::Image& image, const char* directory, const juce::String& name) {
    if (directory == nullptr) return;
    juce::File(directory).createDirectory();
    juce::FileOutputStream out(juce::File(directory).getChildFile(name));
    require(out.openedOk(), "can write the screenshot");
    out.setPosition(0); out.truncate();
    juce::PNGImageFormat().writeImageToStream(image, out);
}
static bool hasDetail(const juce::Image& image) {
    std::set<juce::uint32> colours;
    for (int y = 0; y < image.getHeight(); y += 3) for (int x = 0; x < image.getWidth(); x += 3) colours.insert(image.getPixelAt(x, y).getARGB());
    return colours.size() > 12;
}
static void snapshots() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const char* directory = std::getenv("ALIEN_SNAPSHOT_DIR");
    for (const int length : {16, 32, 64}) {
        Rig rig(length == 64 ? 1000 : 1280, 720);
        rig.controller.editRack("Busy", [&](Project& p) {
            p.addChannel("Tom", "builtin:tom");
            p.addChannel("My sample", "C:/missing/boom.wav");
            p.setPatternLength(0, length);
            for (int s = 0; s < length; s += 4) p.setStepOn(0, 0, s, true);
            for (int s = 4; s < length; s += 8) { p.setStepOn(0, 1, s, true); p.setStepField(0, 1, s, StepField::velocity, 60 + (s * 7) % 60); }
            for (int s = 0; s < length; s += 2) { p.setStepOn(0, 2, s, true); p.setStepField(0, 2, s, StepField::velocity, 40 + (s * 13) % 87); }
            p.setStepOn(0, 3, 6, true); p.setStepField(0, 3, 6, StepField::pan, -50); p.setStepField(0, 3, 6, StepField::pitch, 5);
            p.setStepOn(0, 4, 2, true); p.setStepField(0, 4, 2, StepField::probability, 50);
            p.setStepOn(0, 5, 3, true); p.setStepOn(0, 5, 11, true);
            auto c = p.channels[1]; c.mute = true; c.output = 2; c.pan = 0.4f; p.setChannel(1, c);
            c = p.channels[2]; c.volume = 0.5f; p.setChannel(2, c);
            return true; });
        rig.view.refresh();
        rig.grid.selectChannel(2);
        rig.view.setPlayhead(length == 16 ? 5 : 3);
        const auto image = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
        require(image.getWidth() == rig.view.getWidth() && image.getHeight() == rig.view.getHeight(), "snapshot has the view's size");
        require(hasDetail(image), "the rack draws real content");
        writePng(image, directory, "rack-" + juce::String(length) + ".png");
    }
    {   // Default window, graph tabs, and the full rack with its scroll bar.
        Rig rig(1180, 640);
        for (int s : {0, 3, 4, 8, 10, 12}) rig.click(rig.cellCentre(0, s));
        rig.grid.selectChannel(0);
        for (const auto field : {StepField::velocity, StepField::pan, StepField::pitch, StepField::probability}) {
            rig.graph.setField(field);
            const auto image = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
            require(hasDetail(image), "every graph tab draws");
            if (field == StepField::velocity || field == StepField::probability) writePng(image, directory, "rack-graph-" + juce::String(static_cast<int>(field)) + ".png");
        }
        for (int i = 0; i < 12; ++i) rig.grid.addChannelWithSound(builtinSounds[static_cast<std::size_t>(i % 8)].id);
        for (int s = 0; s < 16; s += 3) for (int row = 0; row < 16; row += 5) rig.click(rig.cellCentre(row, s));
        rig.grid.setScrollPositionY(rig.grid.maxScrollY());
        const auto tall = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
        require(hasDetail(tall), "a full rack draws");
        writePng(tall, directory, "rack-16-channels.png");
        TopBar bar; bar.setSize(1180, TopBar::height); bar.setActiveView(8);
        writePng(bar.createComponentSnapshot(bar.getLocalBounds(), true, 1.0f), directory, "topbar.png");
    }
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::cout << "PASS: rack screens render\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    geometryChecks();
    scrollBarFollowsGrid();
    selectionFollowsChannelEdits();
    tallRackScrolls();
    paintAndEraseStrokes();
    headerControls();
    graphEditor();
    patternBankAndToolbar();
    topBarButton();
    paintedPatternPlays();
    snapshots();
    std::cout << "ChannelRackViewTests passed\n";
    return 0;
}
