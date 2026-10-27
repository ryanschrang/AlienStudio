// The Arrangement timeline screen driven through its real mouse and key handlers:
// geometry, snapping, placing clips and patterns, selection and marquee, moving,
// trimming, copy/paste/duplicate/group, keyboard shortcuts, zoom and scroll, the
// loop range, markers, playback from any position, and what a placed clip sounds
// like. No audio device or window is needed: pop-up menus and the rename box go
// through test seams.
#include "TimelineView.h"
#include "StudioTheme.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <tuple>
#include <vector>

static int failures = 0;
static void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
    if (std::getenv("TIMELINE_TESTS_KEEP_GOING") == nullptr) std::exit(1);   // a debugging aid: list every failure, not just the first
}

using namespace alien;
namespace tl = alien::timeline;
using Zone = TimelineCanvas::Zone;
using Edge = TimelineCanvas::Edge;
using Snap = tlui::Snap;
static constexpr std::int64_t bar = tl::ticksPerBar;

static const juce::ModifierKeys none;
static const juce::ModifierKeys leftButton(juce::ModifierKeys::leftButtonModifier);
static const juce::ModifierKeys rightButton(juce::ModifierKeys::rightButtonModifier);
static const juce::ModifierKeys shiftLeft = leftButton.withFlags(juce::ModifierKeys::shiftModifier);
static const juce::ModifierKeys ctrlLeft = leftButton.withFlags(juce::ModifierKeys::ctrlModifier);
static const juce::ModifierKeys altLeft = leftButton.withFlags(juce::ModifierKeys::altModifier);
static const juce::ModifierKeys ctrl(juce::ModifierKeys::ctrlModifier);
static const juce::ModifierKeys shift(juce::ModifierKeys::shiftModifier);
static const juce::ModifierKeys ctrlShift(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier);
static const juce::ModifierKeys alt(juce::ModifierKeys::altModifier);

static juce::MouseEvent mouse(juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1, bool dragged = false) {
    const auto now = juce::Time::getCurrentTime();
    const auto pf = p.toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pf, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pf, now, clicks, dragged);
}

static void clickButton(juce::Button& button) {
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
}

struct MenuEntry { int id; bool enabled; juce::String text; };
static std::vector<MenuEntry> entries(const juce::PopupMenu& menu) {
    std::vector<MenuEntry> out;
    juce::PopupMenu::MenuItemIterator it(menu, true);
    while (it.next()) {
        const auto& item = it.getItem();
        if (item.itemID != 0 && !item.isSeparator) out.push_back({item.itemID, item.isEnabled, item.text});
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
    std::vector<MenuEntry> items() const { return entries(menu); }
};

template <class T>
static T* childNamed(juce::Component& parent, const juce::String& name) {
    for (auto* child : parent.getChildren()) if (auto* typed = dynamic_cast<T*>(child); typed != nullptr && typed->getName() == name) return typed;
    return nullptr;
}
static juce::ScrollBar* scrollBar(juce::Component& view, bool vertical) {
    for (auto* child : view.getChildren())
        if (auto* bar = dynamic_cast<juce::ScrollBar*>(child); bar != nullptr && bar->isVertical() == vertical) return bar;
    return nullptr;
}

struct Rig {
    ToneEngine engine;
    ProjectController controller{engine};
    TimelineView view{controller};
    TimelineCanvas& c{view.timelineCanvas()};
    std::vector<std::int64_t> seeks;
    std::vector<std::pair<int, int>> opened;
    std::vector<juce::String> messages;
    std::vector<std::pair<std::int64_t, std::int64_t>> exports;
    int toggles{};
    ShownMenu menu, viewMenu;
    bool asked{};
    juce::String askTitle, askCurrent;
    std::function<void(const juce::String&)> answer;
    struct Ids { std::uint32_t a{}, b{}, au{}, p{}; } id;   // the song(): a, b, au on tracks 0-2 and p on pattern lane 1
    std::array<float, 512> left{}, right{};

    explicit Rig(int width = 1200, int height = 640) {
        TimelineCanvas::clipboard() = {};
        engine.prepare(48000);
        engine.patternMode.store(2); engine.bpm.store(120); engine.activeTracks.store(4);
        controller.pushDocumentToEngine();
        view.setSize(width, height);
        c.onSeek = [this](std::int64_t tick) { seeks.push_back(tick); engine.timelineSeek.store(tick); };
        c.onPlayToggle = [this] { ++toggles; };
        c.onOpenSource = [this](int row, int source) { opened.push_back({row, source}); };
        const auto previous = c.onMessage;
        c.onMessage = [this, previous](const juce::String& text) { messages.push_back(text); if (previous) previous(text); };
        c.menuLauncher = [this](juce::PopupMenu m, juce::Rectangle<int> area, std::function<void(int)> done) {
            menu.shown = true; menu.menu = std::move(m); menu.area = area; menu.done = std::move(done);
        };
        c.nameRequester = [this](const juce::String& title, const juce::String& current, std::function<void(const juce::String&)> done) {
            asked = true; askTitle = title; askCurrent = current; answer = std::move(done);
        };
        view.menuLauncher = [this](juce::PopupMenu m, std::function<void(int)> done) {
            viewMenu.shown = true; viewMenu.menu = std::move(m); viewMenu.done = std::move(done);
        };
        view.onExport = [this](std::int64_t from, std::int64_t to) { exports.push_back({from, to}); };
    }

    // ---- the document ----
    const tl::Timeline& t() const { return controller.timeline(); }
    const tl::Region& reg(std::uint32_t regionId) const {
        const auto* r = t().find(regionId);
        if (r == nullptr) { std::cerr << "FAILED: a region the test needs is missing\n"; std::exit(1); }
        return *r;
    }
    bool has(std::uint32_t regionId) const { return t().find(regionId) != nullptr; }
    int regionsOnRow(int row) const { int n = 0; for (const auto& r : t().regions) n += r.row == row ? 1 : 0; return n; }
    bool valid() const { return t().valid(controller.document().trackCount(), controller.document().sceneCount(), controller.channelRack().patternCount); }
    bool selectionIs(std::initializer_list<std::uint32_t> ids) const {
        auto have = c.selection();
        std::vector<std::uint32_t> want(ids);
        std::sort(have.begin(), have.end()); std::sort(want.begin(), want.end());
        return have == want;
    }

    bool deleteTrack(int track) {   // the Session view commits after deleting; so does this
        if (!controller.deleteTrack(track)) return false;
        controller.commit("Delete track");
        return true;
    }

    // ---- the clips the song is made of ----
    void sources() {
        PianoRollPattern one; one.loopSteps = 16; one.add(60, 0, 1); one.add(64, 4, 2); one.add(67, 8, 1);
        controller.setClipPattern(0, 0, one, false);
        PianoRollPattern two; two.loopSteps = 32; two.add(48, 0, 4); two.add(55, 16, 4);
        controller.setClipPattern(1, 1, two, false);
        auto audio = std::make_shared<SessionAudioClip>();
        audio->sampleRate = 8000; audio->channels = 1; audio->frames = 32000;   // 4 s: two bars at 120 BPM
        audio->left.resize(32000);
        for (std::size_t i = 0; i < audio->left.size(); ++i) audio->left[i] = 0.4f * std::sin(static_cast<float>(i) * 0.2f);
        audio->buildWaveform();
        require(controller.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("timeline-view-test.wav"), 2, 0, audio), "install the audio clip");
        controller.editRack("Beat", [](rack::Project& p) { p.setStepOn(0, 0, 0, true); p.setStepOn(0, 1, 4, true); p.setStepOn(0, 2, 8, true); return true; });
        view.refresh();
    }
    // a: track 1, bar 1 (one bar). b: track 2, bar 2 (two bars). au: track 3, bar 1 (two bars of audio). p: pattern lane 1, bar 3 (one bar).
    void song() {
        sources();
        require(controller.placeRegion(0, 0, 0, &id.a) && controller.placeRegion(1, 1, bar, &id.b)
                && controller.placeRegion(2, 0, 0, &id.au) && controller.placeRegion(tl::rackRowBase, 0, 2 * bar, &id.p), "place the song");
        view.refresh();
        c.setSnap(Snap::sixteenth);
    }

    // ---- the mouse ----
    juce::Point<int> at(std::int64_t tick, int row, int dy = 0) const {
        return {juce::roundToInt(c.xForTick(static_cast<double>(tick))), c.rowTop(c.displayRowOf(row)) + TimelineCanvas::rowHeight / 2 + dy};
    }
    juce::Point<int> markerStrip(std::int64_t tick) const { return {juce::roundToInt(c.xForTick(static_cast<double>(tick))), 8}; }
    juce::Point<int> loopStrip(std::int64_t tick) const { return {juce::roundToInt(c.xForTick(static_cast<double>(tick))), TimelineCanvas::markerStripHeight + 8}; }
    juce::Point<int> ruler(std::int64_t tick) const { return {juce::roundToInt(c.xForTick(static_cast<double>(tick))), TimelineCanvas::markerStripHeight + TimelineCanvas::loopStripHeight + 10}; }
    void press(juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1) { c.mouseDown(mouse(c, p, mods, clicks)); c.mouseUp(mouse(c, p, mods, clicks)); }
    void doubleClick(juce::Point<int> p, juce::ModifierKeys mods = leftButton) {
        press(p, mods, 1); press(p, mods, 2);
        c.mouseDoubleClick(mouse(c, p, mods, 2));
    }
    void rightClick(juce::Point<int> p) { menu.reset(); c.mouseDown(mouse(c, p, rightButton)); c.mouseUp(mouse(c, p, rightButton)); }
    void drag(juce::Point<int> from, std::initializer_list<juce::Point<int>> via, juce::ModifierKeys mods = leftButton) {
        c.mouseDown(mouse(c, from, mods));
        juce::Point<int> last = from;
        for (const auto& p : via) { c.mouseDrag(mouse(c, p, mods, 1, true)); last = p; }
        c.mouseUp(mouse(c, last, mods, 1, true));
    }
    bool key(int code, juce::ModifierKeys mods = none, juce::juce_wchar text = 0) { return c.keyPressed(juce::KeyPress(code, mods, text)); }
    bool letter(char ch, juce::ModifierKeys mods = none) { return key(ch, mods, static_cast<juce::juce_wchar>(ch)); }
    void wheel(juce::Point<int> p, float dx, float dy, juce::ModifierKeys mods = none) {
        juce::MouseWheelDetails details{dx, dy, false, false, false};
        c.mouseWheelMove(mouse(c, p, mods), details);
    }
    // Undoes the last thing and reports whether the timeline went back to `before` and redo brings the
    // same result back. It leaves things undone, or with the edit reapplied if `reapply` is set.
    bool oneUndoRestores(const tl::Timeline& before, bool reapply = false) {
        const auto after = t();
        if (!controller.undo()) return false;
        view.refresh();
        const bool back = t() == before;
        if (!controller.redo()) return false;
        view.refresh();
        const bool same = t() == after;
        if (!reapply) { controller.undo(); view.refresh(); }
        return back && same;
    }

    // ---- sound ----
    std::vector<float> rendered;
    void render(int frames) {
        while (frames > 0) {
            const int count = std::min(frames, static_cast<int>(left.size()));
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            for (int i = 0; i < count; ++i) rendered.push_back(left[static_cast<std::size_t>(i)]);
            frames -= count;
        }
    }
    double energy(std::size_t from, std::size_t to) const {
        double sum = 0.0;
        for (std::size_t i = from; i < std::min(to, rendered.size()); ++i) sum += static_cast<double>(rendered[i]) * rendered[i];
        return sum;
    }
};

// ---- geometry ----------------------------------------------------------------------------
static void geometryChecks() {
    Rig rig;
    auto& c = rig.c;
    require(c.getWidth() > 900 && c.getHeight() > 300, "the timeline fills the window");
    require(c.displayRowCount() == 8, "four tracks and four pattern lanes");
    for (int d = 0; d < c.displayRowCount(); ++d) {
        const int row = c.timelineRow(d);
        require(c.displayRowOf(row) == d, "rows map both ways");
        require(d < 4 ? row == d : row == tl::rackRowBase + (d - 4), "tracks first, then the four pattern lanes");
        if (d > 0) require(c.rowTop(d) - c.rowTop(d - 1) == TimelineCanvas::rowHeight, "rows tile");
        require(c.displayRowAt(c.rowTop(d) + 5) == d && c.displayRowAt(c.rowTop(d) + TimelineCanvas::rowHeight - 1) == d, "every pixel of a row maps to it");
    }
    require(c.displayRowAt(TimelineCanvas::topHeight - 1) == -1 && c.displayRowAt(c.rowTop(7) + TimelineCanvas::rowHeight + 1) == -1, "above the first row and below the last is no row");
    for (const double scroll : {0.0, 777.0, 5000.0}) {
        c.setScrollTicks(scroll);
        for (const std::int64_t tick : {0, 1, 960, 3840, 123456})
            require(std::abs(c.tickAtX(c.xForTick(static_cast<double>(tick))) - static_cast<double>(tick)) < 1e-6, "x and tick convert both ways");
    }
    c.setScrollTicks(0);

    require(c.hitTest({10, 5}).zone == Zone::corner, "the corner");
    require(c.hitTest({400, 5}).zone == Zone::markerStrip, "the marker strip");
    require(c.hitTest(rig.loopStrip(8 * bar)).zone == Zone::loopStrip, "the loop strip");
    require(c.hitTest(rig.ruler(8 * bar)).zone == Zone::ruler, "the ruler");
    require(c.hitTest({10, c.rowTop(0) + 10}).zone == Zone::header && c.hitTest({10, c.rowTop(0) + 10}).displayRow == 0, "a row header");
    require(c.hitTest(rig.at(8 * bar, 0)).zone == Zone::lane, "an empty lane");
    require(c.hitTest({400, 5000}).zone == Zone::none, "below everything");
    require(c.hitTest(rig.loopStrip(2 * bar)).loopPart == TimelineCanvas::Hit::LoopPart::body, "inside the default loop range");
    require(c.hitTest(rig.loopStrip(0)).loopPart == TimelineCanvas::Hit::LoopPart::start && c.hitTest(rig.loopStrip(4 * bar)).loopPart == TimelineCanvas::Hit::LoopPart::end, "the loop's two ends");
    require(c.hitTest(rig.loopStrip(8 * bar)).loopPart == TimelineCanvas::Hit::LoopPart::none, "outside the loop");

    rig.song();
    const auto box = c.regionBounds(rig.id.b);
    auto hit = c.hitTest(box.getCentre().toInt());
    require(hit.zone == Zone::region && hit.region == rig.id.b && hit.edge == Edge::none, "the middle of a clip");
    hit = c.hitTest({static_cast<int>(box.getX()) + 2, static_cast<int>(box.getCentreY())});
    require(hit.zone == Zone::region && hit.edge == Edge::left, "the left edge of a clip");
    hit = c.hitTest({static_cast<int>(box.getRight()) - 2, static_cast<int>(box.getCentreY())});
    require(hit.zone == Zone::region && hit.edge == Edge::right, "the right edge of a clip");
    require(c.hitTest(rig.at(30 * bar, 1)).zone == Zone::lane, "empty space on a lane that has clips");
    require(c.hitTest(rig.at(bar / 2, 1)).zone == Zone::lane, "the lane before a clip");
    // A clip too narrow to have grab handles still selects.
    std::uint32_t thin = 0;
    require(rig.controller.editTimeline("thin", [&](tl::Timeline& tlm) { tl::Region r; r.row = 3; r.source = 0; r.start = 20 * bar; r.length = tl::ticksPerStep; const bool ok = tlm.place(r, &thin); return ok; }), "place a one-step clip");
    rig.view.refresh();
    hit = c.hitTest(rig.at(20 * bar + 100, 3));
    require(hit.zone == Zone::region && hit.region == thin && hit.edge == Edge::none, "a clip narrower than its handles has no edges but can be hit");
    std::cout << "PASS: geometry and hit testing\n";
}

// ---- snapping ----------------------------------------------------------------------------
static void snapChecks() {
    Rig rig;
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    require(c.snapTick(5000) == bar && c.snapTick(6000) == 2 * bar && c.snapTick(-50) == 0 && c.snapTick(1e12) == tl::maxLength, "bar snap rounds to the nearest bar and stays in range");
    c.setSnap(Snap::halfBar);
    require(c.snapTick(1000) == 1920 && c.snapTick(900) == 0, "half-bar snap");
    c.setSnap(Snap::beat);
    require(c.snapTick(1400) == 960 && c.snapTick(1500) == 1920, "beat snap");
    c.setSnap(Snap::eighth);
    require(c.snapTick(300) == 480 && c.snapTick(200) == 0, "eighth snap");
    c.setSnap(Snap::sixteenth);
    require(c.snapTick(130) == 240 && c.snapTick(100) == 0 && c.snapTick(3840 + 119) == 3840, "sixteenth snap");
    c.setSnap(Snap::off);
    require(c.snapTick(1234.4) == 1234 && c.snapTick(1234.6) == 1235, "no snap keeps the exact tick");
    c.setSnap(Snap::bar);
    require(c.snapTick(5000, true) == 5000, "Alt bypasses the grid");
    require(c.snapTick(std::nan("")) == 0 && c.snapTick(INFINITY) == 0, "nonsense is not a position");
    // Automatic snap follows the zoom: coarse when zoomed out, fine when zoomed in.
    c.setSnap(Snap::automatic);
    const auto wide = c.gridTicks();
    require(wide >= tl::ticksPerBeat && static_cast<double>(wide) * c.pixelsPerTick() >= 18.0, "the automatic grid is at least 18 pixels wide");
    c.zoomBy(8.0);
    require(c.gridTicks() < wide && c.gridTicks() >= tl::ticksPerStep, "zooming in refines the automatic grid");
    for (int i = 0; i < 40; ++i) c.zoomBy(0.5);
    require(c.gridTicks() > wide, "zooming out coarsens it");
    for (int i = 0; i < 40; ++i) c.zoomBy(2.0);
    require(c.gridTicks() == tl::ticksPerStep, "fully zoomed in snaps to sixteenths");
    // The toolbar's choice reaches the canvas.
    auto* box = childNamed<juce::ComboBox>(rig.view, "Snap");
    require(box != nullptr && box->getNumItems() == static_cast<int>(tlui::allSnaps.size()), "the snap menu lists every grid");
    box->setSelectedId(2, juce::sendNotificationSync);
    require(c.snap() == Snap::bar, "choosing 1 bar in the menu sets the snap");
    box->setSelectedId(7, juce::sendNotificationSync);
    require(c.snap() == Snap::off, "and Off");
    std::cout << "PASS: snapping\n";
}

// ---- placing -----------------------------------------------------------------------------
static void placingChecks() {
    Rig rig;
    rig.sources();
    auto& c = rig.c;
    c.setSnap(Snap::sixteenth);
    const auto empty = rig.t();

    // Double-click in a track's lane puts that track's clip there, snapped.
    rig.doubleClick(rig.at(6970, 0));
    require(rig.t().regions.size() == 1, "double-click places one clip");
    const auto first = rig.t().regions[0];
    require(first.row == 0 && first.source == 0 && first.start == 6960 && first.length == bar, "a one-bar MIDI clip at the snapped position");
    require(rig.selectionIs({first.id}) && c.focusRow() == 0, "the new clip is selected");
    require(rig.oneUndoRestores(empty, true), "placing is one undo step");

    // The second track's selected scene is empty, so the first scene with something in it is used.
    rig.doubleClick(rig.at(0, 1));
    const auto* second = rig.t().regionAt(1, 0);
    require(second != nullptr && second->source == 1 && second->length == 2 * bar, "falls back to the track's first non-empty scene (two bars long)");
    // The audio clip is as long as it plays.
    rig.doubleClick(rig.at(0, 2));
    const auto* audio = rig.t().regionAt(2, 0);
    require(audio != nullptr && audio->source == 0 && audio->length == 2 * bar && audio->audioOffset == 0.0, "an audio clip lasts as long as the file at this tempo");
    // A track with nothing in it starts a new, empty clip instead (the longer checks are in newClipChecks).
    rig.messages.clear();
    const auto count = rig.t().regions.size();
    rig.doubleClick(rig.at(0, 3));
    require(rig.t().regions.size() == count + 1 && rig.t().regionAt(3, 0) != nullptr && !rig.messages.empty() && rig.messages.back().contains("New empty clip"),
            "an empty track gets a new, empty clip and says so");
    // A pattern lane places the selected Channel Rack pattern.
    rig.doubleClick(rig.at(3 * bar, tl::rackRowBase + 1));
    const auto* pattern = rig.t().regionAt(tl::rackRowBase + 1, 3 * bar);
    require(pattern != nullptr && pattern->source == 0 && pattern->length == bar && pattern->isPattern(), "a pattern on a lane lasts as long as the pattern");

    // Placing over a clip overwrites the part it covers.
    const auto before = rig.t();
    require(c.place(0, 0, 6960 + 1920), "place over the first clip");
    require(rig.valid() && rig.regionsOnRow(0) == 2, "no overlap is left behind");
    const auto* cut = rig.t().regionAt(0, 6960);
    require(cut != nullptr && cut->length == 1920, "the earlier clip is shortened where the new one begins");
    require(rig.oneUndoRestores(before), "overwriting is one undo step and undo puts the clip back");

    // Right-click on an empty lane offers what can be placed there.
    rig.rightClick(rig.at(20 * bar + 130, 2));
    require(rig.menu.shown, "right-click opens a menu");
    auto items = rig.menu.items();
    const auto* scene1 = entry(items, TimelineCanvas::placeSceneBase + 0);
    const auto* scene2 = entry(items, TimelineCanvas::placeSceneBase + 1);
    require(scene1 != nullptr && scene1->enabled && scene1->text.contains("audio"), "the track's audio clip is offered");
    require(scene2 != nullptr && !scene2->enabled && scene2->text.contains("empty"), "an empty scene is listed but cannot be placed");
    require(entry(items, TimelineCanvas::cmdPaste) != nullptr && !entry(items, TimelineCanvas::cmdPaste)->enabled, "paste is off with nothing copied");
    require(entry(items, TimelineCanvas::cmdAddMarker) != nullptr && entry(items, TimelineCanvas::cmdSelectAll) != nullptr, "markers and select-all are on the menu");
    rig.menu.done(TimelineCanvas::placeSceneBase + 0);
    const auto* viaMenu = rig.t().regionAt(2, 20 * bar + 130 - 130 % 240 + (130 % 240 >= 120 ? 240 : 0));
    require(viaMenu != nullptr && viaMenu->source == 0, "the chosen clip lands where the pointer was, snapped");
    rig.rightClick(rig.at(30 * bar, tl::rackRowBase + 2));
    items = rig.menu.items();
    require(entry(items, TimelineCanvas::placePatternBase + 0) != nullptr && entry(items, TimelineCanvas::placePatternBase + 0)->text.contains("Pattern 1"), "a pattern lane lists the patterns");
    rig.menu.done(TimelineCanvas::placePatternBase + 0);
    require(rig.t().regionAt(tl::rackRowBase + 2, 30 * bar) != nullptr, "a pattern is placed from the menu");

    // The Place button puts a clip at the play position on the selected row.
    c.setFocusRow(1); c.setCursor(24 * bar, false);
    rig.menu.reset();
    auto* place = childNamed<juce::TextButton>(rig.view, "Place clip");
    require(place != nullptr, "the toolbar has a Place button");
    clickButton(*place);
    require(rig.menu.shown && entry(rig.menu.items(), TimelineCanvas::placeSceneBase + 1) != nullptr && entry(rig.menu.items(), TimelineCanvas::placeSceneBase + 1)->enabled, "it offers the selected track's clips");
    rig.menu.done(TimelineCanvas::placeSceneBase + 1);
    require(rig.t().regionAt(1, 24 * bar) != nullptr, "at the play position");

    // Past the end of the song the song grows; far past the largest song it is refused.
    const auto length = rig.t().lengthBars;
    require(c.place(0, 0, (length + 2) * bar) && rig.t().lengthBars == length + 3 && rig.valid(), "placing past the end lengthens the song");
    rig.messages.clear();
    require(!c.place(0, 0, tl::maxLength) && !rig.messages.empty(), "a position the song cannot reach is refused with a message");
    // Double-clicking a clip opens what it plays.
    rig.opened.clear();
    rig.doubleClick(rig.at(6960 + 100, 0));
    require(rig.opened.size() == 1 && rig.opened[0] == std::make_pair(0, 0), "double-click on a clip opens its source");
    rig.opened.clear();
    rig.doubleClick(rig.at(3 * bar + 100, tl::rackRowBase + 1));
    require(rig.opened.size() == 1 && rig.opened[0] == std::make_pair(tl::rackRowBase + 1, 0), "and a pattern opens in the Channel Rack");
    std::cout << "PASS: placing clips and patterns\n";
}


// ---- a new clip from an empty lane --------------------------------------------------------------
static void newClipChecks() {
    Rig rig;
    rig.sources();   // clips on tracks 1 to 3; track 4 (row 3) has nothing
    auto& c = rig.c;
    auto& d = rig.controller;
    c.setSnap(Snap::bar);
    std::vector<std::tuple<int, int, bool>> created;
    c.onClipCreated = [&](int track, int scene, bool added) { created.emplace_back(track, scene, added); };
    const int scenes = d.document().sceneCount();
    const auto before = rig.t();

    rig.messages.clear();
    rig.doubleClick(rig.at(2 * bar + 100, 3));
    require(rig.t().regions.size() == 1, "double-click on an empty track's lane makes one clip");
    const auto first = rig.t().regions[0];
    require(first.row == 3 && first.start == 2 * bar && first.length == PianoRollPattern::defaultLoopSteps * PianoRollPattern::ticksPerStep && !first.isPattern(),
            "it is the track's, starts at the snapped position and is as long as a new clip loops");
    require(d.document().isValidCell(3, first.source) && d.document().clip(3, first.source).isEmpty() && !d.document().clip(3, first.source).isAudioClip(),
            "it is a MIDI clip with no notes in it yet");
    require(rig.selectionIs({first.id}) && c.focusRow() == 3, "the new clip is selected");
    require(d.document().selectedTrack == 3 && d.document().selectedScene == first.source, "and it is the Session's selected clip, ready for the piano roll");
    require(created.size() == 1 && created[0] == std::make_tuple(3, first.source, false) && d.document().sceneCount() == scenes, "no scene was needed, and the owner was told");
    require(!rig.messages.empty() && rig.messages.back().contains("New empty clip") && rig.messages.back().contains("Track 4"), "the message names the track");
    require(rig.oneUndoRestores(before, true) && d.document().sceneCount() == scenes, "making it is one undo step");

    // The second one is a clip of its own, not another reference to the first.
    rig.doubleClick(rig.at(8 * bar + 100, 3));
    require(rig.regionsOnRow(3) == 2, "a second double-click makes a second clip");
    const auto* a = rig.t().regionAt(3, 2 * bar);
    const auto* b = rig.t().regionAt(3, 8 * bar);
    require(a != nullptr && b != nullptr && a->source != b->source && d.document().clip(3, b->source).isEmpty(), "each new clip has its own Session cell");
    const int aSource = a->source, bSource = b->source;
    // Editing one clip's notes leaves the other empty.
    PianoRollPattern notes; notes.add(60, 0, 4);
    d.setClipPattern(3, aSource, notes, false);
    require(d.document().clip(3, aSource).notes().count == 1 && d.document().clip(3, bSource).isEmpty(), "writing notes into one does not touch the other");
    d.setClipPattern(3, aSource, PianoRollPattern{}, false);

    // Double-click on the new clip opens it.
    rig.opened.clear();
    rig.doubleClick(rig.at(2 * bar + 100, 3));
    require(rig.opened.size() == 1 && rig.opened[0] == std::make_pair(3, aSource), "double-clicking it opens the clip (the piano roll in the program)");

    // Every cell of the track is taken, so the next clip adds a scene, still as one undo step.
    bool added = false;
    int guard = 0;
    std::uint32_t id = 0;
    int scene = -1;
    const auto beforeScene = rig.t();
    const int scenesBefore = d.document().sceneCount();
    while (!added && guard++ < maxSessionScenes) {
        require(d.placeNewClip(3, (20 + guard * 4) * bar, &id, &scene, &added), "another new clip fits");
        if (!added) require(scene < scenesBefore, "while there are free cells the Session does not grow");
    }
    require(added && d.document().sceneCount() == scenesBefore + 1 && scene == scenesBefore, "when no cell is free a scene is added for it");
    require(d.document().clip(3, scene).isEmpty() && rig.t().find(id) != nullptr && rig.t().find(id)->source == scene, "and the new clip is in it");
    require(d.undo() && d.document().sceneCount() == scenesBefore && rig.t().find(id) == nullptr, "one undo removes the clip and the scene");
    require(d.redo() && d.document().sceneCount() == scenesBefore + 1 && rig.t().find(id) != nullptr, "and one redo brings both back");
    (void) beforeScene;
    // Every cell of the track is taken again, so a clip needs another scene; one that cannot be placed must not leave that scene behind.
    {
        const auto stateBefore = rig.t();
        const int scenesNow = d.document().sceneCount();
        require(!d.placeNewClip(3, tl::maxLength) && rig.t() == stateBefore && d.document().sceneCount() == scenesNow,
                "a clip that cannot be placed leaves no new scene behind");
        require(d.document().selectedTrack == 3 && d.document().selectedScene == scene, "and does not change the selection");
    }

    // When the Session is full nothing is created, and the person is told why.
    int n = 0;
    while (n++ < 4 * maxSessionScenes && d.placeNewClip(3, (200 + n * 4) * bar)) {}
    require(d.document().sceneCount() == maxSessionScenes && n <= 4 * maxSessionScenes, "the Session stops at its scene limit");
    const auto full = rig.t();
    const auto fullDoc = d.document().sceneCount();
    require(!d.placeNewClip(3, 0) && rig.t() == full && d.document().sceneCount() == fullDoc, "a refused clip changes nothing, not even the scenes");
    rig.view.refresh();
    rig.messages.clear();
    rig.doubleClick(rig.at(bar, 3));
    require(rig.t() == full && !rig.messages.empty() && rig.messages.back().contains("no room"), "double-click says there is no room");

    // Other rows, bad rows and a busy project are refused.
    Rig plain;
    plain.sources();
    const auto base = plain.t();
    require(!plain.controller.placeNewClip(tl::rackRowBase, 0) && !plain.controller.placeNewClip(-1, 0) && !plain.controller.placeNewClip(99, 0)
            && !plain.controller.placeNewClip(0, -1) && plain.t() == base, "a pattern lane, a missing track and a negative position make no clip");
    plain.controller.setExternalBusy(true);
    require(!plain.controller.placeNewClip(3, 0) && plain.t() == base, "nothing is made while a save or import runs");
    plain.messages.clear();
    plain.doubleClick(plain.at(0, 3));
    require(plain.t() == base && !plain.messages.empty() && plain.messages.back().contains("Wait"), "double-click says to wait");
    plain.controller.setExternalBusy(false);

    // The menus offer it on a track's lane, never on a pattern lane.
    Rig menus;
    menus.sources();
    menus.rightClick(menus.at(3 * bar, 3));
    auto items = menus.menu.items();
    const auto* offer = entry(items, TimelineCanvas::placeNewClip);
    require(offer != nullptr && offer->enabled && offer->text.contains("New empty clip"), "the lane menu offers a new empty clip");
    menus.menu.done(TimelineCanvas::placeNewClip);
    require(menus.t().regionAt(3, 3 * bar) != nullptr && menus.regionsOnRow(3) == 1, "choosing it makes the clip where the pointer was");
    menus.rightClick(menus.at(3 * bar, tl::rackRowBase));
    require(entry(menus.menu.items(), TimelineCanvas::placeNewClip) == nullptr, "a pattern lane does not offer it");
    menus.c.setFocusRow(3); menus.c.setCursor(12 * bar, false);
    menus.menu.reset();
    clickButton(*childNamed<juce::TextButton>(menus.view, "Place clip"));
    require(menus.menu.shown && entry(menus.menu.items(), TimelineCanvas::placeNewClip) != nullptr, "the Place button offers it too");
    menus.menu.done(TimelineCanvas::placeNewClip);
    require(menus.t().regionAt(3, 12 * bar) != nullptr, "at the play position");
    std::cout << "PASS: new empty clips from an empty lane\n";
}

// ---- the clip list --------------------------------------------------------------------------------
static void clipListChecks() {
    Rig rig;
    rig.view.setVisible(true);
    rig.view.refresh();
    auto& bin = rig.view.clipList();
    using Item = clipbin::Item;
    require(rig.view.clipListShown() && bin.isVisible(), "the clip list is showing when the Song opens");
    require(bin.getWidth() == ClipBin::width && bin.getRight() == rig.view.getWidth() && rig.c.getRight() <= bin.getX() && !bin.getBounds().intersects(rig.c.getBounds()),
            "it sits to the right of the lanes without covering them");

    // An empty project: the pattern, and a note instead of clips.
    require(bin.rowCount() == 4 && bin.row(0).type == ClipBin::Row::Type::header && bin.row(1).type == ClipBin::Row::Type::item
            && bin.row(2).type == ClipBin::Row::Type::header && bin.row(3).type == ClipBin::Row::Type::note, "an empty project lists its pattern and says there are no clips");
    require(bin.row(1).item == Item{Item::Kind::pattern, 0, -1} && bin.row(1).left.contains("Pattern 1") && bin.row(1).right == "16 steps" && bin.row(1).placed == 0,
            "the pattern row names the pattern, its length, and that it is not in the Song");
    require(bin.dragDescriptionForRow(0).isVoid() && bin.dragDescriptionForRow(3).isVoid() && bin.dragDescriptionForRow(99).isVoid() && bin.dragDescriptionForRow(-1).isVoid(),
            "headings, the note and rows that do not exist cannot be dragged");

    rig.song();   // clips (0,0), (1,1), audio (2,0); a pattern placed once
    require(bin.rowCount() == 6 && bin.indexOf({Item::Kind::clip, 0, 0}) >= 0 && bin.indexOf({Item::Kind::clip, 1, 1}) >= 0 && bin.indexOf({Item::Kind::clip, 2, 0}) >= 0,
            "every clip with something in it is listed");
    require(bin.indexOf({Item::Kind::clip, 3, 0}) < 0 && bin.indexOf({Item::Kind::clip, 0, 1}) < 0, "empty cells are not");
    const auto& one = bin.row(bin.indexOf({Item::Kind::clip, 0, 0}));
    const auto& two = bin.row(bin.indexOf({Item::Kind::clip, 1, 1}));
    const auto& audio = bin.row(bin.indexOf({Item::Kind::clip, 2, 0}));
    require(one.left.contains("Track 1") && one.left.contains("Scene 1") && one.right == "3 notes" && two.right == "2 notes" && audio.right == "audio", "each row says whose clip it is and what is in it");
    require(bin.row(1).placed == 1 && one.placed == 1 && two.placed == 1 && audio.placed == 1, "rows show how many times the Song already holds them");
    rig.controller.placeRegion(0, 0, 10 * bar);
    rig.controller.placeRegion(tl::rackRowBase + 1, 0, 10 * bar);
    rig.view.refresh();
    require(bin.row(bin.indexOf({Item::Kind::clip, 0, 0})).placed == 2 && bin.row(1).placed == 2, "and the counts follow the Song");
    require(bin.row(bin.indexOf({Item::Kind::clip, 2, 0})).placed == 1, "other rows are unaffected");

    // The list is read again when the Song is shown.
    rig.view.setVisible(false);
    PianoRollPattern more; more.add(60, 0, 1);
    rig.controller.setClipPattern(3, 0, more, false);
    rig.view.setVisible(true);
    require(bin.indexOf({Item::Kind::clip, 3, 0}) >= 0, "a clip written while the Song was hidden is listed when it comes back");
    rig.controller.setClipPattern(3, 0, PianoRollPattern{}, false);
    rig.view.refresh();
    require(bin.indexOf({Item::Kind::clip, 3, 0}) < 0, "and a clip that was emptied is not");

    // What a drag carries survives the trip.
    const auto index = bin.indexOf({Item::Kind::clip, 1, 1});
    const auto description = bin.dragDescriptionForRow(index);
    require(description.isString() && clipbin::decode(description) == Item{Item::Kind::clip, 1, 1}, "a dragged clip row carries the clip");
    require(clipbin::decode(bin.dragDescriptionForRow(1)) == Item{Item::Kind::pattern, 0, -1}, "and a pattern row the pattern");
    for (const char* bad : {"", "alien-song-item", "alien-song-item clip 1", "alien-song-item clip 1 2 3", "alien-song-item clip -1 2", "alien-song-item clip x 2",
                            "alien-song-item pattern", "alien-song-item pattern 1 2", "alien-song-item pattern 1234567", "alien-song-item gizmo 1", "some other drag 1 2",
                            "alien-song-item clip 1 2.5", "ALIEN-SONG-ITEM clip 1 2"})
        require(!clipbin::decode(juce::var(juce::String(bad))).valid(), "a drag that is not one of ours is ignored");
    require(!clipbin::decode(juce::var(7)).valid() && !clipbin::decode(juce::var()).valid(), "and so is one that is not even text");
    require(clipbin::decode(juce::var("alien-song-item clip -1 2")).kind == Item::Kind::none
            && clipbin::decode(juce::var("alien-song-item pattern -3")).kind == Item::Kind::none, "a negative number is not read as an item at all");

    // Double-click or Enter on a row puts it at the play position; headings do nothing.
    std::vector<Item> asked;
    bin.onPlace = [&](const Item& item) { asked.push_back(item); };
    bin.activateRow(index); bin.activateRow(0); bin.activateRow(-5); bin.activateRow(500);
    require(asked.size() == 1 && asked[0] == Item{Item::Kind::clip, 1, 1}, "activating a row asks for that clip, and headings and missing rows ask for nothing");

    // The Clips button hides it and gives the lanes the room back.
    auto* button = childNamed<juce::TextButton>(rig.view, "Show clip list");
    require(button != nullptr && button->getToggleState(), "there is a Clips button, lit while the list shows");
    const int narrow = rig.c.getWidth();
    clickButton(*button);
    require(!bin.isVisible() && !rig.view.clipListShown() && rig.c.getWidth() == narrow + ClipBin::width, "the button hides the list and the lanes get wider");
    clickButton(*button);
    require(bin.isVisible() && rig.c.getWidth() == narrow, "and shows it again");
    std::cout << "PASS: the clip list\n";
}

// ---- dropping a row from the clip list on the lanes ---------------------------------------------
static void dropChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    using Item = clipbin::Item;
    using Details = juce::DragAndDropTarget::SourceDetails;
    const auto details = [&](const Item& item, juce::Point<int> p) { return Details(juce::var(clipbin::encode(item)), &rig.view.clipList(), p); };
    const Item clipA{Item::Kind::clip, 0, 0}, clipB{Item::Kind::clip, 1, 1}, pattern{Item::Kind::pattern, 0, -1};

    require(c.isInterestedInDragSource(details(clipA, {300, 100})) && c.isInterestedInDragSource(details(pattern, {300, 100})), "the lanes accept a clip row and a pattern row");
    require(!c.isInterestedInDragSource(Details(juce::var("a file"), nullptr, {300, 100})) && !c.isInterestedInDragSource(Details(juce::var(3), nullptr, {300, 100})),
            "and nothing else");

    // Dragging over a lane shows where the clip would land, on the clip's own track, and says so.
    rig.messages.clear();
    c.itemDragEnter(details(clipB, rig.at(5 * bar + 700, 3)));
    require(c.dropPreview().valid && c.dropPreview().row == 1 && c.dropPreview().source == 1 && c.dropPreview().tick == 5 * bar,
            "a clip always goes on its own track's lane, whichever lane the pointer is over, at the snapped position");
    require(c.dropPreview().length == 2 * bar, "the ghost is as long as the clip");
    require(!rig.messages.empty() && rig.messages.back().contains("Drop to put") && rig.messages.back().contains("Track 2") && rig.messages.back().contains("bar 6"),
            "the help line says what will happen");
    c.itemDragMove(details(clipB, rig.at(7 * bar + 100, 1)));
    require(c.dropPreview().valid && c.dropPreview().tick == 7 * bar, "moving updates the position");
    const auto snapshotBefore = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    c.itemDragExit(details(clipB, {0, 0}));
    require(!c.dropPreview().valid && rig.messages.back().isEmpty(), "leaving clears the ghost and the message");
    const auto snapshotAfter = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    bool differs = false;
    for (int y = 0; y < snapshotBefore.getHeight() && !differs; y += 3)
        for (int x = 0; x < snapshotBefore.getWidth() && !differs; x += 3) differs = snapshotBefore.getPixelAt(x, y) != snapshotAfter.getPixelAt(x, y);
    require(differs, "and the screen shows the ghost only while dragging");

    // Patterns go on the pattern lane under the pointer, else the first.
    c.itemDragEnter(details(pattern, rig.at(2 * bar, tl::rackRowBase + 2)));
    require(c.dropPreview().valid && c.dropPreview().row == tl::rackRowBase + 2 && c.dropPreview().length == bar, "a pattern lands on the pattern lane it is over");
    c.itemDragMove(details(pattern, rig.at(2 * bar, 0)));
    require(c.dropPreview().row == tl::rackRowBase, "over a track's lane it goes to the first pattern lane");
    c.itemDragMove(details(pattern, {juce::roundToInt(c.xForTick(2 * bar)), 5}));
    require(c.dropPreview().row == tl::rackRowBase, "over the ruler too");
    c.itemDragMove(details(pattern, {20, rig.at(0, 0).y}));
    require(!c.dropPreview().valid && rig.messages.back().contains("right of the track names"), "over the track names nothing can be dropped, and it says where to drop");
    c.itemDragMove(details(Item{Item::Kind::pattern, 7, -1}, rig.at(bar, tl::rackRowBase)));
    require(!c.dropPreview().valid && rig.messages.back().contains("no longer exists"), "a pattern that is gone is refused");
    c.itemDragMove(details(Item{Item::Kind::clip, 3, 3}, rig.at(bar, 3)));
    require(c.dropPreview().valid == rig.controller.document().isValidCell(3, 3), "an empty clip cell is still a real clip");
    c.itemDragExit(details(pattern, {0, 0}));

    // Dropping places it: one undo step, selected, on the grid.
    const auto before = rig.t();
    c.itemDragEnter(details(clipB, rig.at(5 * bar + 700, 1)));
    c.itemDropped(details(clipB, rig.at(5 * bar + 700, 1)));
    const auto* dropped = rig.t().regionAt(1, 5 * bar);
    require(dropped != nullptr && dropped->source == 1 && dropped->length == 2 * bar && dropped->start == 5 * bar, "the drop puts the clip where the ghost was");
    require(rig.selectionIs({dropped->id}) && !c.dropPreview().valid, "it is selected and the ghost is gone");
    require(rig.oneUndoRestores(before), "dropping is one undo step");

    const auto beforePattern = rig.t();
    c.itemDropped(details(pattern, rig.at(6 * bar + 10, tl::rackRowBase + 3)));
    const auto* droppedPattern = rig.t().regionAt(tl::rackRowBase + 3, 6 * bar);
    require(droppedPattern != nullptr && droppedPattern->isPattern() && droppedPattern->source == 0, "a pattern is dropped on its lane");
    require(rig.oneUndoRestores(beforePattern), "also one undo step");

    // A drop that cannot happen changes nothing and explains itself.
    const auto beforeBad = rig.t();
    rig.messages.clear();
    c.itemDropped(details(pattern, {20, 100}));
    c.itemDropped(details(Item{Item::Kind::pattern, 9, -1}, rig.at(bar, tl::rackRowBase)));
    c.itemDropped(Details(juce::var("junk"), nullptr, rig.at(bar, 0)));
    require(rig.t() == beforeBad && !rig.messages.empty(), "a bad drop changes nothing");

    // Dropping on a busy project is refused with a message.
    rig.controller.setExternalBusy(true);
    rig.messages.clear();
    c.itemDropped(details(clipA, rig.at(12 * bar, 0)));
    require(rig.t() == beforeBad && !rig.messages.empty(), "nothing is placed while a save or import runs");
    rig.controller.setExternalBusy(false);

    // The clip list's double-click: at the play position, exactly (not snapped), on the focused pattern lane or the clip's own track.
    c.setCursor(5 * bar + 7, false);
    require(c.placeItemAtCursor(clipA) && rig.t().regionAt(0, 5 * bar + 7) != nullptr && rig.t().regionAt(0, 5 * bar + 7)->start == 5 * bar + 7, "a clip is placed exactly at the play position");
    c.setFocusRow(tl::rackRowBase + 2); c.setCursor(9 * bar, false);
    require(c.placeItemAtCursor(pattern) && rig.t().regionAt(tl::rackRowBase + 2, 9 * bar) != nullptr, "a pattern goes on the focused pattern lane");
    c.setFocusRow(1); c.setCursor(13 * bar, false);
    require(c.placeItemAtCursor(pattern) && rig.t().regionAt(tl::rackRowBase, 13 * bar) != nullptr, "or the first lane when a track is focused");
    const auto beforeGone = rig.t();
    rig.messages.clear();
    require(!c.placeItemAtCursor(Item{Item::Kind::clip, 0, 40}) && rig.t() == beforeGone && !rig.messages.empty(), "a clip that is gone is refused with a message");
    require(rig.valid(), "the song is still valid");
    std::cout << "PASS: dropping clips and patterns from the list\n";
}

// ---- selection ---------------------------------------------------------------------------
static void selectionChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    const auto& id = rig.id;
    const auto before = rig.t();
    rig.press(rig.at(1000, 0));
    require(rig.selectionIs({id.a}), "click selects a clip");
    rig.press(rig.at(bar + 1000, 1));
    require(rig.selectionIs({id.b}), "clicking another replaces the selection");
    rig.press(rig.at(1000, 0), shiftLeft);
    require(rig.selectionIs({id.a, id.b}), "Shift-click adds");
    rig.press(rig.at(1000, 0), shiftLeft);
    require(rig.selectionIs({id.b}), "Shift-click on a selected clip removes it");
    rig.press(rig.at(1000, 0), ctrlLeft);
    require(rig.selectionIs({id.a, id.b}), "Ctrl-click adds too");
    rig.press(rig.at(1000, 0), ctrlLeft);
    require(rig.selectionIs({id.b}), "and a second Ctrl-click takes it out");
    // A press that takes a clip out of the selection is not the start of a drag, even if the mouse moves on.
    rig.press(rig.at(1000, 0), shiftLeft);
    require(rig.selectionIs({id.a, id.b}), "Shift-click puts it back");
    rig.drag(rig.at(1000, 0), {rig.at(2 * bar + 1000, 0), rig.at(3 * bar + 1000, 2)}, shiftLeft);
    require(rig.selectionIs({id.b}) && rig.t() == before && !c.dragging(), "Shift-pressing a selected clip deselects it, and dragging on from there moves nothing");
    rig.seeks.clear();
    rig.press(rig.at(12 * bar, 3));
    require(rig.selectionIs({}) && rig.t() == before, "clicking empty space deselects and edits nothing");
    require(rig.seeks.size() == 1 && rig.seeks[0] == c.cursorTick() && c.cursorTick() == 12 * bar - 12 * bar % 240, "and puts the play position there");
    require(rig.key(juce::KeyPress::escapeKey), "Escape is ours");
    rig.letter('a', ctrl);
    require(rig.selectionIs({id.a, id.b, id.au, id.p}), "Ctrl+A selects every clip");
    rig.key(juce::KeyPress::escapeKey);
    require(rig.selectionIs({}), "Escape clears the selection");
    rig.press(rig.at(2 * bar + 100, tl::rackRowBase + 1));
    require(rig.selectionIs({}), "empty space on another pattern lane selects nothing");
    rig.press(rig.at(2 * bar + 100, tl::rackRowBase));
    require(rig.selectionIs({id.p}), "a pattern is selected like a clip");

    // Marquee.
    c.clearSelection();
    rig.drag(rig.at(12 * bar, 3), {rig.at(10 * bar, 2), rig.at(1000, 0)});
    require(rig.selectionIs({id.a, id.b, id.au}), "a box over three tracks selects the clips it touches");
    require(rig.t() == before, "and edits nothing");
    c.clearSelection();
    rig.drag(rig.at(12 * bar, 2), {rig.at(5000, 0)});
    require(rig.selectionIs({id.b, id.au}), "the box only takes clips it overlaps: a (ends before it) is out");
    // The selection follows the box while it is dragged.
    c.clearSelection();
    rig.c.mouseDown(mouse(c, rig.at(12 * bar, 3)));
    rig.c.mouseDrag(mouse(c, rig.at(1000, 0), leftButton, 1, true));
    require(rig.selectionIs({id.a, id.b, id.au}), "while dragging, the box selects");
    rig.c.mouseDrag(mouse(c, rig.at(11 * bar, 2), leftButton, 1, true));
    require(rig.selectionIs({}), "shrinking the box deselects again");
    rig.c.mouseUp(mouse(c, rig.at(11 * bar, 2), leftButton, 1, true));
    // A box that only touches a clip's edge still takes it; a box beside it does not.
    c.clearSelection();
    rig.drag(rig.at(12 * bar, 0), {rig.at(bar - 240, 0)});
    require(rig.selectionIs({id.a}), "four pixels of overlap are enough");
    c.clearSelection();
    rig.drag(rig.at(12 * bar, 0), {rig.at(bar + 240, 0)});
    require(rig.selectionIs({}), "a box that starts after the clip ends takes nothing");
    // Backwards boxes work, and so does a box on the pattern lanes.
    c.clearSelection();
    rig.drag(rig.at(1000, 3), {rig.at(12 * bar, 0)});
    require(rig.selectionIs({id.a, id.b, id.au}), "a box dragged up and to the right");
    c.clearSelection();
    rig.drag(rig.at(12 * bar, tl::rackRowBase), {rig.at(2 * bar + 100, tl::rackRowBase)});
    require(rig.selectionIs({id.p}), "a box on a pattern lane");
    // Shift adds to what was selected.
    rig.press(rig.at(bar + 1000, 1));
    rig.drag(rig.at(12 * bar, 0), {rig.at(1000, 0)}, shiftLeft);
    require(rig.selectionIs({id.a, id.b}), "Shift+box adds to the selection");
    rig.drag(rig.at(12 * bar, 0), {rig.at(1000, 0)});
    require(rig.selectionIs({id.a}), "a plain box replaces it");

    // A plain click on one of several selected clips picks just that one, when the button comes up (a drag moves them all).
    c.setSelection({id.a, id.b});
    rig.press(rig.at(1000, 0));
    require(rig.selectionIs({id.a}), "clicking one of several selected clips selects only it");
    c.setSelection({id.a, id.b});
    rig.drag(rig.at(1000, 0), {rig.at(1000, 0) + juce::Point<int>(64, 0)});
    require(rig.selectionIs({id.a, id.b}), "but dragging it keeps them all");
    rig.controller.undo(); rig.view.refresh();
    // Ctrl-dragging a clip that is not selected copies just that clip.
    c.setSelection({id.b});
    rig.drag(rig.at(1000, 0), {rig.at(1000, 0) + juce::Point<int>(5 * 64, 0)}, ctrlLeft);
    require(rig.regionsOnRow(0) == 2 && rig.regionsOnRow(1) == 1, "Ctrl-drag of an unselected clip copies only that clip");
    rig.controller.undo(); rig.view.refresh();
    c.setSelection({id.a, id.b});
    rig.drag(rig.at(1000, 0), {rig.at(1000, 0) + juce::Point<int>(5 * 64, 0)}, ctrlLeft);
    require(rig.regionsOnRow(0) == 2 && rig.regionsOnRow(1) == 2, "and of a selected one copies the whole selection");
    rig.controller.undo(); rig.view.refresh();

    // Selection survives a refresh and loses clips that are gone.
    c.setSelection({id.a, id.b});
    rig.view.refresh();
    require(rig.selectionIs({id.a, id.b}), "a refresh keeps the selection");
    rig.controller.editTimeline("remove", [&](tl::Timeline& tlm) { return tlm.remove({id.b}); });
    rig.view.refresh();
    require(rig.selectionIs({id.a}), "a clip that disappears leaves the selection");
    std::cout << "PASS: selection and marquee\n";
}

// ---- moving, trimming, copying -------------------------------------------------------------
static void dragChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    const auto& id = rig.id;
    const auto before = rig.t();
    const auto aMid = rig.at(1920, 0);
    constexpr int pixelsPerBar = 64;   // at the starting zoom

    // A small movement is a click, not an edit.
    rig.drag(aMid, {aMid + juce::Point<int>(1, 1)});
    require(rig.t() == before && rig.selectionIs({id.a}), "a one-pixel wobble moves nothing");

    // Moving: previewed while dragging, committed on release as one undo step.
    c.mouseDown(mouse(c, aMid));
    c.mouseDrag(mouse(c, aMid + juce::Point<int>(2 * pixelsPerBar, 0), leftButton, 1, true));
    require(rig.t() == before, "the document is untouched until the button is released");
    require(c.shown().find(id.a)->start == 2 * bar, "but the screen shows where the clip is going");
    require(c.dragging(), "a drag is under way");
    c.mouseUp(mouse(c, aMid + juce::Point<int>(2 * pixelsPerBar, 0), leftButton, 1, true));
    require(rig.reg(id.a).start == 2 * bar && rig.reg(id.a).length == bar && rig.reg(id.a).row == 0, "dragging by two bars moves the clip two bars");
    require(!c.dragging() && c.shown().find(id.a)->start == 2 * bar, "the preview is gone and the real clip is where it was dropped");
    require(rig.oneUndoRestores(before), "the whole drag is one undo step");

    // Escape while dragging cancels.
    c.mouseDown(mouse(c, aMid));
    c.mouseDrag(mouse(c, aMid + juce::Point<int>(200, 0), leftButton, 1, true));
    require(c.dragging() && rig.key(juce::KeyPress::escapeKey) && !c.dragging(), "Escape stops a drag");
    c.mouseUp(mouse(c, aMid + juce::Point<int>(200, 0), leftButton, 1, true));
    require(rig.t() == before && c.shown().find(id.a)->start == 0, "and nothing is changed or left on the screen");

    // Snap decides where it lands; Alt does not snap.
    c.setSnap(Snap::bar);
    rig.drag(aMid, {aMid + juce::Point<int>(90, 0)});
    require(rig.reg(id.a).start == bar, "with a bar grid a clip lands on a bar (90 px is 1.4 bars)");
    rig.controller.undo(); rig.view.refresh();
    c.setSnap(Snap::sixteenth);
    rig.drag(aMid, {aMid + juce::Point<int>(10, 0)}, altLeft);
    require(std::abs(rig.reg(id.a).start - 600) <= 1 && rig.reg(id.a).start % 240 != 0, "Alt moves by the exact distance, off the grid");
    rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "back to the start");

    // A clip cannot go before the start of the song.
    rig.drag(aMid, {aMid + juce::Point<int>(-170, 0)});
    require(rig.t() == before, "dragging left of the start changes nothing when the clip is there already");
    rig.drag(rig.at(bar + 1000, 1), {rig.at(bar + 1000, 1) + juce::Point<int>(-300, 0)});
    require(rig.reg(id.b).start == 0 && rig.reg(id.b).length == 2 * bar, "a clip dragged far left stops at the start");
    rig.controller.undo(); rig.view.refresh();

    // Several clips move together.
    c.setSelection({id.a, id.b});
    rig.drag(aMid, {aMid + juce::Point<int>(pixelsPerBar, 0)});
    require(rig.reg(id.a).start == bar && rig.reg(id.b).start == 2 * bar, "a selection moves as one, keeping the distance between clips");
    require(rig.selectionIs({id.a, id.b}), "and stays selected");
    rig.controller.undo(); rig.view.refresh();

    // Pattern clips change lane when dragged up or down; track clips stay on their track.
    const int lane = tl::rackRowBase;
    rig.drag(rig.at(2 * bar + 1000, lane), {rig.at(2 * bar + 1000, lane + 1)});
    require(rig.reg(id.p).row == lane + 1 && rig.reg(id.p).start == 2 * bar, "dragging a pattern down one lane");
    rig.drag(rig.at(2 * bar + 1000, lane + 1), {rig.at(2 * bar + 1000, lane + 1, 46 * 5)});
    require(rig.reg(id.p).row == lane + 3, "it stops at the last lane");
    rig.drag(rig.at(2 * bar + 1000, lane + 3), {rig.at(2 * bar + 1000, lane + 3, -46 * 9)});
    require(rig.reg(id.p).row == lane, "and at the first");
    rig.controller.undo(); rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "three drags, three undo steps");
    rig.drag(aMid, {aMid + juce::Point<int>(0, 92)});
    require(rig.t() == before, "a track's clip dragged up or down stays on its track");

    // Dropping on another clip overwrites the part it covers.
    require(c.place(0, 0, 10 * bar), "another clip on track 1");
    const auto crowded = rig.t();
    rig.drag(aMid, {aMid + juce::Point<int>(static_cast<int>(std::lround(static_cast<double>(10 * bar - 1920) * c.pixelsPerTick())), 0)});
    const auto* later = rig.t().regionAt(0, 10 * bar + 1920);
    require(rig.valid() && rig.regionsOnRow(0) == 2 && later != nullptr && later->length == 1920 && later->offset == 1920, "the clip it landed on is shortened from the front, its music left in place");
    require(rig.oneUndoRestores(crowded), "undo restores both clips");
    rig.controller.undo(); rig.view.refresh();   // the extra clip
    require(rig.t() == before, "back to the song");

    // Moving past the end lengthens the song.
    c.zoomBy(0.25);
    c.setScrollTicks(0);
    rig.drag(rig.at(1920, 0), {rig.at(1920 + 62000, 0)});
    require(rig.valid() && rig.t().lengthBars > before.lengthBars && rig.reg(id.a).start > 55000, "dragging a clip past the end makes the song longer");
    require(rig.oneUndoRestores(before), "and undo makes it shorter again");
    c.zoomBy(4.0);
    c.setScrollTicks(0);

    // Trimming the end and the start.
    c.clearSelection();
    const auto bBox = c.regionBounds(id.b);
    const juce::Point<int> rightEdge{static_cast<int>(bBox.getRight()) - 2, static_cast<int>(bBox.getCentreY())};
    rig.drag(rightEdge, {rightEdge + juce::Point<int>(-pixelsPerBar, 0)});
    require(rig.reg(id.b).length == bar && rig.reg(id.b).start == bar && rig.reg(id.b).offset == 0, "dragging the right edge back by a bar shortens the clip");
    const juce::Point<int> shortEdge{static_cast<int>(c.regionBounds(id.b).getRight()) - 2, rightEdge.y};
    rig.drag(shortEdge, {shortEdge + juce::Point<int>(2 * pixelsPerBar, 0)});
    require(rig.reg(id.b).length == 3 * bar, "and dragging it out lengthens it (the music repeats)");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "two trims, two undo steps");
    const juce::Point<int> leftEdge{static_cast<int>(c.regionBounds(id.b).getX()) + 2, rightEdge.y};
    rig.drag(leftEdge, {leftEdge + juce::Point<int>(pixelsPerBar, 0)});
    require(rig.reg(id.b).start == 2 * bar && rig.reg(id.b).length == bar && rig.reg(id.b).offset == bar, "dragging the left edge in hides the start: the music is not moved");
    const juce::Point<int> movedLeft{static_cast<int>(c.regionBounds(id.b).getX()) + 2, rightEdge.y};
    rig.drag(movedLeft, {movedLeft + juce::Point<int>(-pixelsPerBar, 0)});
    require(rig.reg(id.b).start == bar && rig.reg(id.b).offset == 0 && rig.reg(id.b).length == 2 * bar, "dragging it back out reveals it again");
    const auto revealed = rig.t();
    const juce::Point<int> again{static_cast<int>(c.regionBounds(id.b).getX()) + 2, rightEdge.y};
    rig.drag(again, {again + juce::Point<int>(-pixelsPerBar, 0)});
    require(rig.t() == revealed, "and no further than the start of the music");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "back to the song");
    // A trim only ever touches one clip, even if several are selected.
    c.setSelection({id.a, id.b});
    rig.drag(rightEdge, {rightEdge + juce::Point<int>(-pixelsPerBar, 0)});
    require(rig.reg(id.a).length == bar && rig.reg(id.b).length == bar && rig.selectionIs({id.b}), "trimming works on the clip whose edge was grabbed");
    rig.controller.undo(); rig.view.refresh();
    // An audio clip trims without moving its sound.
    const juce::Point<int> auLeft{static_cast<int>(c.regionBounds(id.au).getX()) + 2, static_cast<int>(c.regionBounds(id.au).getCentreY())};
    rig.drag(auLeft, {auLeft + juce::Point<int>(32, 0)});
    require(rig.reg(id.au).start == 1920 && rig.reg(id.au).audioOffset > 0.95 && rig.reg(id.au).audioOffset < 1.05, "trimming audio hides the first second of the file");
    rig.controller.undo(); rig.view.refresh();

    // Ctrl-drag copies.
    rig.drag(aMid, {aMid + juce::Point<int>(4 * pixelsPerBar, 0)}, ctrlLeft);
    require(rig.reg(id.a).start == 0 && rig.regionsOnRow(0) == 2, "Ctrl-drag leaves the original and makes a copy");
    const auto* copy = rig.t().regionAt(0, 4 * bar);
    require(copy != nullptr && copy->source == 0 && copy->id != id.a && rig.selectionIs({copy->id}), "the copy is where it was dropped and is selected");
    require(rig.oneUndoRestores(before), "copying is one undo step");
    std::cout << "PASS: moving, trimming and copying by dragging\n";
}

// ---- arming tracks and choosing their input ------------------------------------------------------
static void recordingSetupChecks() {
    Rig rig; rig.song();
    auto& c = rig.c;
    c.inputNames = [] { return juce::StringArray{"Mic", "Line L", "Line R"}; };
    const auto settings = [&](int track) { return rig.controller.document().tracks[static_cast<std::size_t>(track)].settings; };
    const auto arm = c.armRect(c.displayRowOf(1)).getCentre();
    require(!settings(1).armed && !rig.engine.tracks[1].armed.load(), "tracks start disarmed");
    rig.messages.clear(); c.clearSelection();
    rig.press(arm);
    require(settings(1).armed && rig.engine.tracks[1].armed.load(), "clicking a track's arm button arms it, in the project and the engine");
    require(c.selection().empty() && rig.messages.empty(), "without selecting anything or warning (its input is enabled)");
    require(rig.controller.undo() && !settings(1).armed && !rig.engine.tracks[1].armed.load(), "arming is one undo step");
    require(rig.controller.redo() && settings(1).armed && rig.engine.tracks[1].armed.load(), "and redo arms it again");
    rig.press(arm);
    require(!settings(1).armed, "a second click disarms it");
    rig.press({20, arm.y});
    require(!settings(1).armed, "clicking elsewhere on the header only selects the row");

    const auto header = juce::Point<int>{40, c.rowTop(c.displayRowOf(1)) + 20};
    rig.rightClick(header);
    require(rig.menu.shown && entry(rig.menu.items(), TimelineCanvas::cmdArm) != nullptr
            && entry(rig.menu.items(), TimelineCanvas::cmdInputMonoBase + 2) != nullptr
            && entry(rig.menu.items(), TimelineCanvas::cmdInputStereoBase) != nullptr
            && entry(rig.menu.items(), TimelineCanvas::cmdInputStereoBase + 2) == nullptr,
            "the header menu arms, and lists each enabled input and each pair of them");
    rig.menu.done(TimelineCanvas::cmdInputStereoBase);
    require(settings(1).input == 0 && settings(1).stereoInput && rig.engine.tracks[1].stereoInput.load(), "choosing inputs 1+2 makes the track stereo");
    rig.rightClick(header);
    rig.menu.done(TimelineCanvas::cmdInputMonoBase + 2);
    require(settings(1).input == 2 && !settings(1).stereoInput && rig.engine.tracks[1].input.load() == 2, "choosing input 3 makes it mono on that channel");
    require(c.inputText(1) == "In 3", "the header shows the input");
    rig.rightClick(header);
    rig.menu.done(TimelineCanvas::cmdMonitorBase + static_cast<int>(InputMonitor::off));
    require(settings(1).monitor == static_cast<int>(InputMonitor::off) && rig.engine.tracks[1].monitor.load() == static_cast<int>(InputMonitor::off),
            "monitoring can be turned off");
    rig.rightClick(header);
    rig.menu.done(TimelineCanvas::cmdArm);
    require(settings(1).armed, "and the menu arms the track");

    c.inputNames = [] { return juce::StringArray{"Mic"}; };
    rig.messages.clear();
    rig.press(c.armRect(c.displayRowOf(2)).getCentre());
    require(settings(2).armed && rig.messages.empty(), "a track on input 1 arms quietly when the device has it");
    require(!c.inputAvailable(1) && c.inputAvailable(2), "input 3 is reported missing when only one input is enabled");
    rig.press(c.armRect(c.displayRowOf(1)).getCentre()); rig.press(c.armRect(c.displayRowOf(1)).getCentre());
    require(settings(1).armed && !rig.messages.empty() && rig.messages.back().contains("not enabled"), "arming a track whose input is not enabled says so");

    rig.menu.reset();
    rig.rightClick({40, c.rowTop(c.displayRowOf(tl::rackRowBase)) + 20});
    require(!rig.menu.shown, "pattern lanes have no recording menu");
    rig.press(c.armRect(c.displayRowOf(tl::rackRowBase)).getCentre());
    require(rig.valid(), "and clicking where an arm button would be does nothing harmful");

    c.updateInputMeters();
    require(c.inputLevel(1) == 0.0f, "with no audio coming in the meters read zero");
    rig.engine.tracks[1].inputPeak.store(0.5f);
    c.updateInputMeters();
    require(std::abs(c.inputLevel(1) - 0.5f) < 1.0e-6f && rig.engine.tracks[1].inputPeak.load() == 0.0f, "a meter takes the engine's peak");
    c.updateInputMeters();
    require(c.inputLevel(1) > 0.0f && c.inputLevel(1) < 0.5f, "and falls back gradually");
    (void)c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);   // headers with arm buttons, inputs and meters paint
    std::cout << "PASS: arming tracks and choosing inputs\n";
}

static void audioSlipDragChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    const auto audio = rig.id.au;
    const auto from = rig.at(1920, 2);
    const auto to = from + juce::Point<int>(10, 0);  // 600 ticks, off the sixteenth grid
    c.setSnap(Snap::bar);
    const auto before = rig.t();
    c.mouseDown(mouse(c, from, altLeft));
    c.mouseDrag(mouse(c, from + juce::Point<int>(8, 0), altLeft, 1, true));
    c.mouseDrag(mouse(c, to, altLeft, 1, true));
    const auto* preview = c.shown().find(audio);
    require(rig.t() == before && preview->offset == 600 && std::abs(preview->audioOffset - .3125) < 1e-9
        && preview->start == 0 && preview->length == 2 * bar,
        "Alt-drag previews an unsnapped source slip relative to the press, leaving boundaries and document unchanged");
    // The gesture chosen on press stays a slip if Alt is released while dragging.
    c.mouseUp(mouse(c, to, leftButton, 1, true));
    auto expected = before;
    expected.find(audio)->offset = 600; expected.find(audio)->audioOffset = .3125;
    require(rig.t() == expected && rig.controller.undoLabel() == "Slip audio clip" && !c.dragging(),
        "release changes only the audio source offsets, even if Alt is released first");
    require(rig.oneUndoRestores(before), "one undo/redo restores the complete slip gesture");

    // Clamping at the start, click jitter, returning to the press, and Escape add no edit.
    const auto label = rig.controller.undoLabel();
    rig.drag(from, {from + juce::Point<int>(-10, 0)}, altLeft);
    rig.drag(from, {from + juce::Point<int>(1, 0)}, altLeft);
    rig.drag(from, {to, from}, altLeft);
    c.mouseDown(mouse(c, from, altLeft));
    c.mouseDrag(mouse(c, to, altLeft, 1, true));
    require(c.dragging() && rig.key(juce::KeyPress::escapeKey), "Escape cancels a slip preview");
    c.mouseUp(mouse(c, to, altLeft, 1, true));
    require(rig.t() == before && c.shown() == before && rig.controller.undoLabel() == label && rig.controller.canRedo(),
        "cancelled and zero-distance slips change neither document nor undo/redo history");

    rig.drag(from, {to}, altLeft);
    rig.drag(from, {from + juce::Point<int>(-20, 0)}, altLeft);
    require(rig.reg(audio).offset == 0 && rig.reg(audio).audioOffset == 0 && rig.reg(audio).start == 0,
        "dragging back past the beginning clamps source offsets to zero without moving the clip");
    require(rig.controller.undo() && rig.controller.undo(), "undo the two source slips");
    rig.view.refresh();

    // Mixed selections slip audio only, including several placements of the same source.
    std::uint32_t other{};
    require(rig.controller.placeRegion(2, 0, 4 * bar, &other), "place another audio occurrence");
    rig.view.refresh();
    c.setSelection({rig.id.a, audio, other, rig.id.p});
    const auto mixed = rig.t();
    rig.drag(from, {to}, altLeft);
    expected = mixed;
    for (const auto id : {audio, other}) { expected.find(id)->offset = 600; expected.find(id)->audioOffset = .3125; }
    require(rig.t() == expected && rig.selectionIs({rig.id.a, audio, other, rig.id.p})
        && rig.controller.undoLabel() == "Slip audio clips", "a mixed selection slips only its audio occurrences and preserves selection");
    require(rig.oneUndoRestores(mixed), "several audio slips form one undo step");

    // The existing tempo-aware slip operation supplies source seconds.
    require(rig.controller.setTempoChange(240, 60), "add a tempo change under the slipped source range");
    rig.view.refresh();
    c.setSelection({audio});
    rig.drag(from, {to}, altLeft);
    require(rig.reg(audio).offset == 600 && std::abs(rig.reg(audio).audioOffset - .5) < 1e-9,
        "slip maps ticks to source seconds through tempo changes");
    rig.controller.undo(); rig.view.refresh();
    const auto tempoBase = rig.t();
    rig.controller.setExternalBusy(true);
    rig.drag(from, {to}, altLeft);
    rig.controller.setExternalBusy(false);
    require(rig.t() == tempoBase && !c.dragging(), "a busy controller refuses slip commit cleanly");

    // Audio edges retain Alt snap bypass instead of starting source slip.
    const auto edge = juce::Point<int>(juce::roundToInt(c.regionBounds(audio).getRight()) - 1, from.y);
    rig.drag(edge, {edge + juce::Point<int>(-10, 0)}, altLeft);
    require(rig.reg(audio).start == 0 && rig.reg(audio).length == 2 * bar - 600 && rig.reg(audio).offset == 0,
        "Alt-dragging an audio edge still trims freely");
    rig.controller.undo(); rig.view.refresh();
    const auto box = c.regionBounds(audio);
    const juce::Point<int> handle{juce::roundToInt(box.getRight()) - 1, static_cast<int>(box.getY()) + 4};
    rig.drag(handle, {handle + juce::Point<int>(-10, 0)}, altLeft);
    require(rig.reg(audio).fadeOut > 0 && rig.reg(audio).length == 2 * bar && rig.reg(audio).offset == 0,
        "Alt-dragging an audio fade handle still edits the fade");
    rig.controller.undo(); rig.view.refresh();
    c.mouseDown(mouse(c, from, leftButton));
    c.mouseDrag(mouse(c, to, altLeft, 1, true));
    c.mouseUp(mouse(c, to, altLeft, 1, true));
    require(rig.reg(audio).start == 600 && rig.reg(audio).offset == 0,
        "adding Alt after a normal audio move begins still bypasses snap without switching to slip");
    std::cout << "PASS: Alt-drag audio source slip, preview, selection, tempo, cancellation and history\n";
}

// ---- fades and crossfades -----------------------------------------------------------------------
static void fadeChecks() {
    Rig rig; rig.song();
    auto& c = rig.c; const auto& id = rig.id;
    constexpr int pixelsPerBar = 64;   // at the starting zoom: 60 ticks a pixel
    const auto before = rig.t();
    const auto box = c.regionBounds(id.au);
    const int top = static_cast<int>(box.getY()) + 4;
    // The handles sit in the top strip of an audio clip; below it the edges still trim.
    const juce::Point<int> inHandle{static_cast<int>(box.getX()) + 1, top}, outHandle{static_cast<int>(box.getRight()) - 1, top};
    require(c.hitTest(inHandle).fade == TimelineCanvas::Edge::left && c.hitTest(outHandle).fade == TimelineCanvas::Edge::right,
            "an audio clip's fade handles are in its top corners");
    require(c.hitTest({inHandle.x, static_cast<int>(box.getCentreY())}).fade == TimelineCanvas::Edge::none
            && c.hitTest({inHandle.x, static_cast<int>(box.getCentreY())}).edge == TimelineCanvas::Edge::left, "lower down the edge still trims");
    require(c.hitTest({static_cast<int>(c.regionBounds(id.a).getX()) + 1, static_cast<int>(c.regionBounds(id.a).getY()) + 4}).fade == TimelineCanvas::Edge::none,
            "a MIDI clip has no fade handles");
    rig.drag(inHandle, {inHandle + juce::Point<int>(pixelsPerBar / 2, 0)});
    require(rig.reg(id.au).fadeIn == tl::ticksPerBar / 2 && rig.reg(id.au).start == 0 && rig.reg(id.au).length == 2 * tl::ticksPerBar,
            "dragging the fade-in handle sets the fade without trimming the clip");
    const auto faded = rig.t();
    const juce::Point<int> movedIn{static_cast<int>(c.regionBounds(id.au).getX()) + pixelsPerBar / 2, top};
    require(c.hitTest(movedIn).fade == TimelineCanvas::Edge::left, "the handle follows the fade");
    rig.drag(outHandle, {outHandle + juce::Point<int>(-pixelsPerBar, 0)});
    require(rig.reg(id.au).fadeOut == tl::ticksPerBar && rig.reg(id.au).fadeIn == tl::ticksPerBar / 2, "dragging the fade-out handle sets the fade out");
    require(rig.oneUndoRestores(faded) && rig.controller.undo(), "each fade drag is one undo step");
    rig.view.refresh();
    require(rig.t() == before, "back to the song");

    // Crossfading the audio clip into a copy placed right after it.
    std::uint32_t next{};
    require(rig.controller.placeRegion(2, 0, 2 * tl::ticksPerBar, &next), "place a second audio clip touching the first");
    rig.view.refresh();
    const auto touching = rig.t();
    c.setSelection({next});
    rig.rightClick(rig.at(2 * tl::ticksPerBar + tl::ticksPerBar / 2, 2));
    const auto* item = entry(rig.menu.items(), TimelineCanvas::cmdCrossfade);
    require(item != nullptr && item->enabled, "one selected clip with a clip ending where it starts can be crossfaded");
    rig.menu.done(TimelineCanvas::cmdCrossfade);
    require(rig.reg(next).crossfade == tl::ticksPerBeat && rig.reg(id.au).end() == rig.reg(next).start, "the menu crosses them over a beat without moving them");
    const auto crossed = rig.t();
    const auto nextBox = c.regionBounds(next);
    const juce::Point<int> crossHandle{static_cast<int>(nextBox.getX()) + pixelsPerBar / 4, static_cast<int>(nextBox.getY()) + 4};
    require(c.hitTest(crossHandle).fade == TimelineCanvas::Edge::left, "the crossfade has a handle at its end");
    rig.drag(crossHandle, {crossHandle + juce::Point<int>(pixelsPerBar / 4, 0)});
    require(rig.reg(next).crossfade == tl::ticksPerBar / 2 && rig.reg(next).fadeIn == 0, "dragging it lengthens the crossfade");
    require(rig.oneUndoRestores(crossed), "one undo step");
    rig.rightClick(rig.at(2 * tl::ticksPerBar + tl::ticksPerBar / 2, 2));
    item = entry(rig.menu.items(), TimelineCanvas::cmdRemoveFades);
    require(item != nullptr && item->enabled, "a crossfaded clip offers Remove fades");
    rig.menu.done(TimelineCanvas::cmdRemoveFades);
    require(rig.reg(next).crossfade == 0 && rig.t() == touching, "which takes the crossfade away");
    c.setSelection({id.a});
    rig.rightClick(rig.at(tl::ticksPerBar / 2, 0));
    require(entry(rig.menu.items(), TimelineCanvas::cmdCrossfade) == nullptr, "a MIDI clip has no fade commands");
    rig.menu.reset();
    require(rig.valid(), "the timeline stays valid");
    std::cout << "PASS: fades and crossfades\n";
}

// ---- keys and commands ------------------------------------------------------------------------
static void commandChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    const auto& id = rig.id;
    const auto before = rig.t();

    require(rig.key(juce::KeyPress::spaceKey) && rig.toggles == 1, "Space plays and stops");
    // Copy and paste.
    c.setSelection({id.a});
    rig.messages.clear();
    require(rig.letter('c', ctrl) && !TimelineCanvas::clipboard().empty() && !rig.messages.empty(), "Ctrl+C copies");
    c.setCursor(4 * bar, false);
    rig.letter('v', ctrl);
    const auto* pasted = rig.t().regionAt(0, 4 * bar);
    require(pasted != nullptr && pasted->source == 0 && pasted->start == 4 * bar && rig.selectionIs({pasted->id}), "Ctrl+V pastes at the play position and selects the paste");
    require(rig.oneUndoRestores(before), "paste is one undo step");
    // A pattern pastes to the focused lane.
    c.setSelection({id.p}); rig.letter('c', ctrl);
    c.setFocusRow(tl::rackRowBase + 3); c.setCursor(8 * bar, false);
    rig.letter('v', ctrl);
    require(rig.t().regionAt(tl::rackRowBase + 3, 8 * bar) != nullptr, "pattern clips paste to the lane that has focus");
    rig.controller.undo(); rig.view.refresh();
    // Cut and delete.
    c.setSelection({id.a});
    rig.letter('x', ctrl);
    require(!rig.has(id.a) && !TimelineCanvas::clipboard().empty() && rig.selectionIs({}), "Ctrl+X removes and keeps");
    c.setCursor(0, false); rig.letter('v', ctrl);
    require(rig.t().regionAt(0, 0) != nullptr && rig.regionsOnRow(0) == 1, "and pastes back");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "cut and paste are two undo steps");
    c.setSelection({id.a, id.p});
    require(rig.key(juce::KeyPress::deleteKey) && !rig.has(id.a) && !rig.has(id.p) && rig.regionsOnRow(1) == 1, "Delete removes the selection");
    rig.controller.undo(); rig.view.refresh();
    c.clearSelection();
    require(!rig.key(juce::KeyPress::deleteKey) && rig.t() == before, "Delete with nothing selected does nothing");
    require(!rig.key(juce::KeyPress::backspaceKey) && rig.t() == before, "nor does Backspace");
    // Duplicate.
    c.setSelection({id.a, id.b});
    rig.letter('d', ctrl);
    require(rig.t().regions.size() == before.regions.size() + 2, "Ctrl+D duplicates");
    const auto* dupA = rig.t().regionAt(0, 3 * bar);
    const auto* dupB = rig.t().regionAt(1, 4 * bar);
    require(dupA != nullptr && dupB != nullptr && dupA->source == 0 && dupB->source == 1 && rig.selectionIs({dupA->id, dupB->id}), "the copies follow the selection as one unit and are selected");
    require(rig.oneUndoRestores(before), "duplicate is one undo step");
    // What was copied follows its tracks if they are renumbered, and skips them if they are gone.
    c.setSelection({id.au}); rig.letter('c', ctrl);
    require(rig.deleteTrack(1), "delete the second track");   // the tracks below move up: track 3 is now track 2
    rig.view.refresh();
    c.setCursor(10 * bar, false);
    rig.letter('v', ctrl);
    require(rig.t().regionAt(1, 10 * bar) != nullptr && rig.t().regionAt(1, 10 * bar)->source == 0 && rig.valid(), "a paste goes to the track it was copied from, even after tracks were renumbered");
    c.setSelection({id.au}); rig.letter('c', ctrl);
    require(rig.deleteTrack(1), "delete the track the copied clip was on");
    rig.view.refresh();
    const auto afterDelete = rig.t();
    rig.messages.clear();
    rig.letter('v', ctrl);
    require(rig.t() == afterDelete && !rig.messages.empty(), "and a paste of clips whose track is gone does nothing and says so");
    rig.controller.undo(); rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    // Group and ungroup.
    c.setSelection({id.a});
    rig.messages.clear();
    rig.letter('g', ctrl);
    require(rig.t() == before && !rig.messages.empty(), "grouping one clip explains itself");
    c.setSelection({id.a, id.b});
    rig.letter('g', ctrl);
    require(rig.reg(id.a).group != 0 && rig.reg(id.a).group == rig.reg(id.b).group && rig.reg(id.au).group == 0, "Ctrl+G groups the selection");
    rig.press(rig.at(1000, 0));
    require(rig.selectionIs({id.a, id.b}), "clicking one clip of a group selects the group");
    rig.drag(rig.at(1000, 0), {rig.at(1000, 0) + juce::Point<int>(64, 0)});
    require(rig.reg(id.a).start == bar && rig.reg(id.b).start == 2 * bar, "a group moves together");
    rig.controller.undo(); rig.view.refresh();
    rig.letter('g', ctrlShift);
    require(rig.reg(id.a).group == 0 && rig.reg(id.b).group == 0, "Ctrl+Shift+G ungroups");
    rig.press(rig.at(1000, 0));
    require(rig.selectionIs({id.a}), "after ungrouping a click selects one clip");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    // Split.
    c.setSelection({id.a});
    c.setCursor(1920, false);
    rig.letter('s');
    require(rig.regionsOnRow(0) == 2 && rig.reg(id.a).length == 1920, "S splits the selected clip at the play position");
    const auto* right = rig.t().regionAt(0, 1920);
    require(right != nullptr && right->offset == 1920 && right->length == 1920 && rig.selectionIs({id.a, right->id}), "the right half carries on from where the left stopped");
    rig.controller.undo(); rig.view.refresh();
    c.clearSelection();
    rig.letter('s');
    require(rig.regionsOnRow(0) == 2 && rig.regionsOnRow(2) == 2 && rig.regionsOnRow(1) == 1, "with nothing selected S splits every clip under the play position");
    rig.controller.undo(); rig.view.refresh();
    c.setCursor(0, false); c.setSelection({id.a});
    rig.messages.clear();
    rig.letter('s');
    require(rig.t() == before && !rig.messages.empty(), "S where no clip is crossed does nothing and says so");
    c.setCursor(1920, false); c.setSelection({id.au});
    rig.letter('e', ctrl);
    require(rig.regionsOnRow(2) == 2 && rig.regionsOnRow(0) == 1, "Ctrl+E splits only the selected clip");
    const auto* auRight = rig.t().regionAt(2, 1920);
    require(auRight != nullptr && auRight->audioOffset > 0.9 && auRight->audioOffset < 1.1, "an audio clip's right half starts a second into the file");
    rig.controller.undo(); rig.view.refresh();
    // Mute.
    c.setSelection({id.a, id.b});
    rig.letter('m');
    require(rig.reg(id.a).muted && rig.reg(id.b).muted && !rig.reg(id.au).muted, "M mutes the selection");
    rig.letter('m');
    require(!rig.reg(id.a).muted && !rig.reg(id.b).muted, "and M again unmutes it");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    // Loop.
    require(!rig.t().loopEnabled, "the loop starts off");
    rig.letter('l');
    require(rig.t().loopEnabled, "L turns the loop on");
    rig.letter('l');
    require(!rig.t().loopEnabled, "and off");
    c.setSelection({id.b});
    rig.letter('l', shift);
    require(rig.t().loopEnabled && rig.t().loopStart == bar && rig.t().loopEnd == 3 * bar, "Shift+L loops the selected clip");
    c.clearSelection();
    rig.letter('l', shift);
    require(rig.t().loopStart == 0 && rig.t().loopEnd == rig.t().length(), "with nothing selected, Shift+L loops the whole song");
    rig.controller.undo(); rig.controller.undo(); rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "back to the start");
    // Markers.
    c.setCursor(5 * bar, false);
    rig.letter('k');
    require(rig.t().markers.size() == 1 && rig.t().markers[0].tick == 5 * bar && rig.t().markers[0].name == "Marker 1" && c.selectedMarkerId() == rig.t().markers[0].id, "K adds a marker at the play position and selects it");
    c.setCursor(2 * bar, false);
    rig.letter('k');
    require(rig.t().markers.size() == 2 && rig.t().markers[0].tick == 2 * bar, "markers stay in order");
    c.setCursor(0, false);
    rig.key(juce::KeyPress::rightKey, ctrl);
    require(c.cursorTick() == 2 * bar, "Ctrl+Right jumps to the next marker");
    rig.key(juce::KeyPress::rightKey, ctrl);
    require(c.cursorTick() == 5 * bar, "and the one after");
    rig.messages.clear();
    rig.key(juce::KeyPress::rightKey, ctrl);
    require(c.cursorTick() == 5 * bar && !rig.messages.empty(), "past the last marker it says so");
    rig.key(juce::KeyPress::leftKey, ctrl);
    require(c.cursorTick() == 2 * bar, "Ctrl+Left goes back");
    rig.key(juce::KeyPress::deleteKey);
    require(rig.t().markers.size() == 1 && rig.t().markers[0].tick == 5 * bar, "Delete removes the selected marker");
    rig.controller.undo(); rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    // Nudge.
    c.setSelection({id.b});
    rig.key(juce::KeyPress::rightKey);
    require(rig.reg(id.b).start == bar + 240, "Right nudges a clip by the grid (a sixteenth here)");
    rig.key(juce::KeyPress::rightKey, shift);
    require(rig.reg(id.b).start == bar + 240 + bar, "Shift+Right nudges by a bar");
    rig.key(juce::KeyPress::leftKey);
    rig.key(juce::KeyPress::leftKey);
    require(rig.reg(id.b).start == 2 * bar - 240, "Left nudges back by the grid, twice");
    rig.controller.undo(); rig.view.refresh();
    require(rig.t() == before, "a run of nudges is a single undo step");
    c.setSelection({id.a});
    rig.key(juce::KeyPress::leftKey, shift);
    require(rig.reg(id.a).start == 0, "a clip cannot be nudged before the start");
    c.clearSelection();
    c.setCursor(2 * bar, false);
    rig.seeks.clear();
    rig.key(juce::KeyPress::rightKey);
    require(c.cursorTick() == 2 * bar + 240 && rig.seeks.back() == 2 * bar + 240, "with nothing selected Right moves the play position");
    rig.key(juce::KeyPress::homeKey);
    require(c.cursorTick() == 0, "Home goes to the start");
    rig.key(juce::KeyPress::endKey);
    require(c.cursorTick() == rig.t().length(), "End goes to the end of the song");
    // Up and down move a pattern between lanes, or the focus between rows.
    c.setSelection({id.p});
    rig.key(juce::KeyPress::downKey);
    require(rig.reg(id.p).row == tl::rackRowBase + 1, "Down moves a pattern to the next lane");
    rig.key(juce::KeyPress::upKey);
    require(rig.reg(id.p).row == tl::rackRowBase, "Up moves it back");
    rig.controller.undo(); rig.controller.undo(); rig.view.refresh();
    c.clearSelection(); c.setFocusRow(0);
    rig.key(juce::KeyPress::downKey);
    require(c.focusRow() == 1, "with nothing selected Down moves the focus");
    c.setSelection({id.a});
    const auto stay = rig.t();
    rig.key(juce::KeyPress::downKey);
    require(rig.t() == stay, "a track's clip does not change track");
    // Zoom.
    const double ppt = c.pixelsPerTick();
    rig.letter('=');
    require(c.pixelsPerTick() > ppt, "+ zooms in");
    rig.key('-', none, '-');
    require(std::abs(c.pixelsPerTick() - ppt) < 1e-9, "- zooms out by the same step");
    rig.key('0', ctrl, '0');
    require(c.scrollTicks() == 0.0 && c.visibleTicks() >= static_cast<double>(rig.t().contentEnd()), "Ctrl+0 fits the work in the window");
    // Keys that are not ours are left for the rest of the program.
    require(!rig.letter('a') && !rig.letter('w') && !rig.letter('j') && !rig.letter('z', ctrl) && !rig.letter('s', ctrl) && !rig.letter('o', ctrl),
            "typing-piano letters and the window's own shortcuts are not taken");
    require(!rig.key('s', alt, 's') && !rig.key(juce::KeyPress::upKey, ctrl), "Alt combinations are left alone");
    std::cout << "PASS: keyboard shortcuts and commands\n";
}

// ---- zoom and scroll ----------------------------------------------------------------------------------
static void zoomAndScrollChecks() {
    Rig rig(1200, 420);
    rig.song();
    auto& c = rig.c;
    // Ctrl+wheel zooms around the pointer.
    c.zoomBy(4.0);
    c.setScrollTicks(10 * bar);
    const auto pointer = juce::Point<int>(600, 200);
    const double under = c.tickAtX(pointer.x);
    rig.wheel(pointer, 0.0f, 1.0f, ctrl);
    require(c.pixelsPerTick() > 4 * 64.0 / 3840.0 && std::abs(c.tickAtX(pointer.x) - under) < 1.0 / c.pixelsPerTick() + 1.0, "Ctrl+wheel up zooms in and keeps the tick under the pointer");
    rig.wheel(pointer, 0.0f, -1.0f, ctrl);
    require(std::abs(c.pixelsPerTick() - 4 * 64.0 / 3840.0) < 1e-9 && std::abs(c.tickAtX(pointer.x) - under) < 1.0 / c.pixelsPerTick() + 1.0, "and wheel down zooms back out around the same point");
    const double before = c.pixelsPerTick();
    c.zoomBy(0.0); c.zoomBy(-3.0); c.zoomBy(std::nan("")); c.zoomBy(INFINITY);
    require(c.pixelsPerTick() == before, "nonsense zoom factors are ignored");
    for (int i = 0; i < 80; ++i) c.zoomBy(0.5);
    require(c.pixelsPerTick() == TimelineCanvas::minPixelsPerTick && c.scrollTicks() >= 0.0, "zooming out stops at the limit");
    require(c.visibleTicks() >= static_cast<double>(tl::maxLength), "the widest view shows the whole of the longest song (18 hours)");
    for (int i = 0; i < 80; ++i) c.zoomBy(2.0);
    require(c.pixelsPerTick() == TimelineCanvas::maxPixelsPerTick && c.scrollTicks() <= c.maxScrollTicks(), "zooming in stops at the limit");
    require(c.pixelsPerTick() * static_cast<double>(tl::ticksPerStep) > 40.0 && c.pixelsPerTick() * static_cast<double>(tl::ticksPerStep) < 400.0, "the closest view shows a sixteenth as a hand-sized block, not a screen-sized one");
    c.zoomToFit();
    require(c.scrollTicks() == 0.0 && c.visibleTicks() >= static_cast<double>(rig.t().contentEnd()) && c.visibleTicks() < rig.t().contentEnd() * 1.5, "Fit shows all the work and not much more");
    // Scrolling.
    c.zoomBy(8.0);
    c.setScrollTicks(0);
    rig.wheel({600, 200}, 0.0f, -1.0f, shift);
    require(c.scrollTicks() > 0.0, "Shift+wheel scrolls sideways");
    const double sideways = c.scrollTicks();
    rig.wheel({600, 200}, 1.0f, 0.0f);
    require(c.scrollTicks() < sideways, "a horizontal wheel (or trackpad) scrolls sideways too");
    c.setScrollTicks(1e12);
    require(c.scrollTicks() == c.maxScrollTicks(), "scrolling stops at the end");
    c.setScrollTicks(-5);
    require(c.scrollTicks() == 0.0, "and at the start");
    c.setScrollTicks(std::nan("")); c.setScrollTicks(INFINITY);
    require(c.scrollTicks() == 0.0, "nonsense is ignored");
    // Vertical scrolling needs more rows than fit: add tracks until they do.
    while (rig.controller.document().trackCount() < 8) rig.controller.addTrack();
    rig.view.refresh();
    require(c.maxScrollY() > 0 && c.scrollY() == 0, "with many tracks the lanes scroll vertically");
    rig.wheel({600, 200}, 0.0f, -1.0f);
    require(c.scrollY() > 0, "the wheel scrolls them");
    c.setScrollY(100000);
    require(c.scrollY() == c.maxScrollY(), "to the end and no further");
    c.setScrollY(-4);
    require(c.scrollY() == 0, "and back");
    c.scrollRowIntoView(tl::rackRowBase + 3);
    require(c.scrollY() > 0 && c.rowTop(c.displayRowOf(tl::rackRowBase + 3)) + TimelineCanvas::rowHeight <= c.getHeight(), "a row can be scrolled into view");
    c.scrollRowIntoView(0);
    require(c.scrollY() == 0, "and back to the top");
    // A clip at the visible edge is hit correctly after scrolling.
    c.zoomToFit();
    c.setScrollTicks(0);
    c.setScrollY(c.maxScrollY());
    c.clearSelection();
    require(c.scrollY() > 0 && c.scrollY() == c.maxScrollY(), "the lanes are scrolled to the bottom");
    const auto patternSpot = rig.at(2 * bar + 1000, tl::rackRowBase);
    require(patternSpot.y >= TimelineCanvas::topHeight && patternSpot.y < c.getHeight(), "the pattern lane is on screen");
    const auto scrolledHit = c.hitTest(patternSpot);
    require(scrolledHit.zone == Zone::region && scrolledHit.region == rig.id.p && scrolledHit.displayRow == c.displayRowOf(tl::rackRowBase), "hit testing follows the vertical scroll");
    rig.press(patternSpot);
    require(rig.selectionIs({rig.id.p}), "and so does clicking");
    const auto emptySpot = rig.at(2 * bar + 1000, tl::rackRowBase + 1);
    require(c.hitTest(emptySpot).zone == Zone::lane && c.hitTest(emptySpot).displayRow == c.displayRowOf(tl::rackRowBase + 1), "the lane below it is an empty lane");
    rig.press(emptySpot);
    require(rig.selectionIs({}), "so clicking there selects nothing");
    c.setScrollY(0);
    // The scroll bars mirror the view and drive it.
    auto* horizontal = scrollBar(rig.view, false);
    auto* vertical = scrollBar(rig.view, true);
    require(horizontal != nullptr && vertical != nullptr, "the screen has both scroll bars");
    c.zoomBy(8.0);
    c.setScrollTicks(1000);
    require(std::abs(horizontal->getCurrentRangeStart() - c.scrollTicks()) < 1.0 && std::abs(horizontal->getCurrentRangeSize() - c.visibleTicks()) < 1.0, "the bar shows the visible part of the song");
    horizontal->setCurrentRangeStart(5000.0, juce::sendNotificationSync);
    require(std::abs(c.scrollTicks() - 5000.0) < 1.0, "dragging the bar scrolls the song");
    vertical->setCurrentRangeStart(40.0, juce::sendNotificationSync);
    require(c.scrollY() == 40, "and the vertical bar scrolls the lanes");
    // The playhead is followed while playing, if asked.
    c.zoomToFit(); c.zoomBy(8.0); c.setScrollTicks(0);
    c.setFollow(true);
    c.setPlayhead(c.visibleTicks() * 0.5);
    require(c.scrollTicks() == 0.0, "a playhead in view does not scroll");
    c.setPlayhead(static_cast<std::int64_t>(c.visibleTicks()) + 5000);
    require(c.xForTick(static_cast<double>(c.playheadTick())) >= TimelineCanvas::headerWidth && c.xForTick(static_cast<double>(c.playheadTick())) <= c.getWidth(), "a playhead that runs off the right edge is brought back into view");
    const double followed = c.scrollTicks();
    c.setPlayhead(1000);
    require(c.scrollTicks() < followed && c.xForTick(1000.0) >= TimelineCanvas::headerWidth, "and one that jumps back is found again");
    c.setFollow(false);
    c.setScrollTicks(0);
    c.setPlayhead(40 * bar);
    require(c.scrollTicks() == 0.0, "with Follow off the view stays put");
    c.setPlayhead(-1);
    require(c.playheadTick() == -1, "the playhead can be hidden");
    auto* follow = childNamed<juce::ToggleButton>(rig.view, "Follow playhead");
    require(follow != nullptr && follow->getToggleState(), "the Follow button exists and starts on");
    clickButton(*follow);
    require(!follow->getToggleState() && !c.followsPlayhead(), "clicking it turns following off");
    clickButton(*follow);
    require(follow->getToggleState() && c.followsPlayhead(), "and on again");
    std::cout << "PASS: zoom and scroll\n";
}

// ---- the loop range ---------------------------------------------------------------------------------------
static void loopChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    const auto before = rig.t();
    // A click on the strip does nothing; a drag outside the range makes a new one and turns it on.
    rig.press(rig.loopStrip(6 * bar));
    require(rig.t() == before, "a click in the loop strip changes nothing");
    rig.drag(rig.loopStrip(6 * bar), {rig.loopStrip(7 * bar), rig.loopStrip(9 * bar)});
    require(rig.t().loopStart == 6 * bar && rig.t().loopEnd == 9 * bar && rig.t().loopEnabled, "dragging in the strip sets a loop range, snapped, and switches it on");
    require(rig.oneUndoRestores(before), "setting the loop is one undo step");
    rig.drag(rig.loopStrip(9 * bar), {rig.loopStrip(6 * bar)});
    require(rig.t().loopStart == 6 * bar && rig.t().loopEnd == 9 * bar, "it can be dragged backwards");
    // Ends and body.
    rig.drag(rig.loopStrip(9 * bar), {rig.loopStrip(12 * bar)});
    require(rig.t().loopEnd == 12 * bar && rig.t().loopStart == 6 * bar, "the right end drags");
    rig.drag(rig.loopStrip(6 * bar), {rig.loopStrip(4 * bar)});
    require(rig.t().loopStart == 4 * bar && rig.t().loopEnd == 12 * bar, "the left end drags");
    rig.drag(rig.loopStrip(8 * bar), {rig.loopStrip(9 * bar)});
    require(rig.t().loopStart == 5 * bar && rig.t().loopEnd == 13 * bar, "the body drags the whole range");
    rig.drag(rig.loopStrip(5 * bar), {rig.loopStrip(14 * bar)});
    require(rig.t().loopStart == 13 * bar - tl::ticksPerStep && rig.t().loopEnd == 13 * bar, "an end stops a step short of the other end instead of crossing it");
    require(rig.t().loopEnd - rig.t().loopStart >= tl::ticksPerStep && rig.valid(), "the range always stays valid");
    rig.drag(rig.loopStrip(rig.t().loopEnd), {rig.loopStrip(60 * bar)});
    require(rig.t().loopEnd <= rig.t().length() && rig.valid(), "the range cannot run past the end of the song");
    // Double-click loops the bar under the pointer.
    rig.doubleClick(rig.loopStrip(2 * bar + 1000));
    require(rig.t().loopStart == 2 * bar && rig.t().loopEnd == 3 * bar && rig.t().loopEnabled, "double-click in the loop strip loops that bar");
    // The menu and the button.
    rig.rightClick(rig.loopStrip(10 * bar));
    auto items = rig.menu.items();
    require(rig.menu.shown && entry(items, TimelineCanvas::cmdJump) != nullptr && entry(items, TimelineCanvas::cmdJump)->text == "Loop off", "the loop menu offers to turn the loop off");
    require(entry(items, TimelineCanvas::cmdLoopSelection) != nullptr && !entry(items, TimelineCanvas::cmdLoopSelection)->enabled, "looping the selection needs a selection");
    rig.menu.done(TimelineCanvas::cmdJump);
    require(!rig.t().loopEnabled, "and does");
    c.setSelection({rig.id.b});
    rig.rightClick(rig.loopStrip(10 * bar));
    rig.menu.done(TimelineCanvas::cmdLoopSelection);
    require(rig.t().loopStart == bar && rig.t().loopEnd == 3 * bar && rig.t().loopEnabled, "the loop can be set to the selected clips");
    auto* button = childNamed<juce::TextButton>(rig.view, "Loop");
    require(button != nullptr && button->getToggleState(), "the Loop button shows the loop is on");
    clickButton(*button);
    require(!rig.t().loopEnabled && !button->getToggleState(), "clicking it turns the loop off");
    clickButton(*button);
    require(rig.t().loopEnabled && button->getToggleState(), "and on");
    // The loop is part of the project and of undo.
    const auto saved = rig.t();
    const auto state = rig.controller.document();
    require(state.timeline.loopStart == saved.loopStart && state.timeline.loopEnd == saved.loopEnd && state.timeline.loopEnabled, "the document holds the loop");
    std::cout << "PASS: loop range\n";
}

// ---- markers --------------------------------------------------------------------------------------------------
// ---- tempo changes on the ruler -------------------------------------------------------------------------------------
static const juce::Label* labelStartingWith(juce::Component& root, const juce::String& prefix) {
    for (auto* child : root.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr && label->getText().startsWith(prefix)) return label;
        if (const auto* found = labelStartingWith(*child, prefix)) return found;
    }
    return nullptr;
}
static void tempoChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    const auto flag = [&](std::int64_t tick) { return rig.ruler(tick) + juce::Point<int>(8, 7); };
    const auto before = rig.t();
    // Double-click on the ruler asks for the tempo from there.
    rig.doubleClick(rig.ruler(4 * bar));
    require(rig.asked && rig.askTitle == "Tempo from Bar 5.1 (60-200 BPM)" && rig.askCurrent == "120", "double-click on the ruler asks for the tempo there");
    rig.answer("90");
    require(rig.t().tempoChanges.size() == 1 && rig.t().tempoChanges[0].tick == 4 * bar && rig.t().tempoChanges[0].bpm == 90.0,
        "and adds a tempo change, snapped");
    require(rig.oneUndoRestores(before, true), "adding a tempo change is one undo step");
    require(c.hitTest(flag(4 * bar)).zone == Zone::ruler && c.hitTest(flag(4 * bar)).tempo == 4 * bar
        && c.hitTest(rig.ruler(2 * bar)).tempo == -1, "the change is a flag on the ruler");
    // Bad answers change nothing and say why.
    for (const char* bad : {"fast", "500", "59", "", "-90"}) {
        rig.asked = false; rig.messages.clear();
        rig.doubleClick(flag(4 * bar));
        require(rig.asked && rig.askCurrent == "90", "double-click on a flag asks for its tempo");
        rig.answer(bad);
        require(rig.t().tempoChanges.size() == 1 && rig.t().tempoChanges[0].bpm == 90.0 && !rig.messages.empty(),
            "a tempo outside 60-200 BPM is refused with a message");
    }
    rig.asked = false;
    rig.doubleClick(flag(4 * bar));
    rig.answer("100.5 BPM");
    require(rig.t().tempoChanges.size() == 1 && rig.t().tempoChanges[0].bpm == 100.5, "a fractional tempo, with or without BPM after it");
    // The readout's time follows the tempo map: four bars at 120 BPM and two at 100.5 BPM.
    c.setCursor(6 * bar, false); rig.view.refresh();
    const double seconds = 8.0 + 2 * 4 * 60.0 / 100.5;
    const auto* readout = labelStartingWith(rig.view, "Bar 7.1");
    require(readout != nullptr && readout->getText().contains("0:" + juce::String(seconds, 2).paddedLeft('0', 5)), "the time readout follows the tempo map");
    // Clicking a flag plays from there; dragging moves it.
    rig.seeks.clear(); c.setCursor(0, false);
    rig.press(flag(4 * bar));
    require(!rig.seeks.empty() && rig.seeks.back() == 4 * bar && rig.t().tempoChanges[0].tick == 4 * bar, "clicking a tempo change plays from it");
    const auto placed = rig.t();
    rig.drag(flag(4 * bar), {flag(5 * bar), flag(6 * bar)});
    require(rig.t().tempoChanges.size() == 1 && rig.t().tempoChanges[0].tick == 6 * bar && rig.t().tempoChanges[0].bpm == 100.5,
        "dragging a tempo change moves it");
    require(rig.oneUndoRestores(placed, true), "moving it is one undo step");
    // The menus.
    rig.rightClick(flag(6 * bar));
    auto items = rig.menu.items();
    require(entry(items, TimelineCanvas::cmdJump) != nullptr && entry(items, TimelineCanvas::cmdTempo) != nullptr
        && entry(items, TimelineCanvas::cmdRemoveTempo) != nullptr, "the tempo change menu");
    rig.asked = false;
    rig.menu.done(TimelineCanvas::cmdTempo);
    require(rig.asked && rig.askCurrent == "100.50", "Change tempo asks for the tempo");
    rig.answer("140");
    require(rig.t().tempoChanges[0].bpm == 140.0, "and changes it");
    rig.rightClick(rig.ruler(2 * bar));
    items = rig.menu.items();
    require(entry(items, TimelineCanvas::cmdAddMarker) != nullptr && entry(items, TimelineCanvas::cmdTempo) != nullptr
        && entry(items, TimelineCanvas::cmdTempo)->enabled && entry(items, TimelineCanvas::cmdRemoveTempo)->enabled,
        "the ruler menu adds a tempo change, and removes them all when there are some");
    rig.asked = false;
    rig.menu.done(TimelineCanvas::cmdTempo);
    rig.answer("75");
    require(rig.t().tempoChanges.size() == 2 && rig.t().tempoChanges[0].tick == 2 * bar && rig.t().tempoChanges[0].bpm == 75.0, "a second change");
    rig.rightClick(flag(2 * bar));
    rig.menu.done(TimelineCanvas::cmdRemoveTempo);
    require(rig.t().tempoChanges.size() == 1 && rig.t().tempoChanges[0].tick == 6 * bar, "Delete tempo change removes that one");
    rig.rightClick(rig.ruler(2 * bar));
    rig.menu.done(TimelineCanvas::cmdRemoveTempo);
    require(rig.t().tempoChanges.empty() && rig.valid(), "Remove all tempo changes removes the rest");
    // The song's start belongs to the project tempo.
    rig.asked = false; rig.messages.clear();
    rig.doubleClick(rig.ruler(0));
    require(!rig.asked && !rig.messages.empty() && rig.t().tempoChanges.empty(), "no tempo change at the very start");
    // A clip placed after a slower tempo lasts more ticks: the audio clip is four seconds.
    require(rig.controller.setTempoChange(8 * bar, 60.0), "fixture: 60 BPM from bar 9");
    require(rig.controller.naturalLength(2, 0, 0) == 2 * bar && rig.controller.naturalLength(2, 0, 8 * bar) == bar,
        "an audio clip's length on the song follows the tempo where it is placed");
}

static void meterChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    const auto before = rig.t();
    // The ruler menu sets the time signature from the bar under the pointer.
    rig.rightClick(rig.ruler(4 * bar + 100));
    auto items = rig.menu.items();
    require(entry(items, TimelineCanvas::cmdMeter) != nullptr && entry(items, TimelineCanvas::cmdMeter)->enabled
        && entry(items, TimelineCanvas::cmdMeter)->text == "Time signature from bar 5..." && !entry(items, TimelineCanvas::cmdRemoveMeter)->enabled,
        "the ruler menu offers a time signature from the bar under the pointer");
    rig.asked = false;
    rig.menu.done(TimelineCanvas::cmdMeter);
    require(rig.asked && rig.askTitle == "Time signature from bar 5 (for example 3/4 or 6/8)" && rig.askCurrent == "4/4", "it asks, showing 4/4");
    rig.answer("3/4");
    require(rig.t().meterChanges.size() == 1 && rig.t().meterChanges[0].bar == 4 && rig.t().meterChanges[0].numerator == 3, "and sets 3/4 from bar 5");
    require(rig.oneUndoRestores(before, true), "one undo step");
    const auto threeFour = 3 * 960;
    // A label after the bar number; double-click it to change it.
    const auto label = c.meterBounds(rig.t().meterChanges[0]).getCentre().toInt();
    require(c.hitTest(label).zone == Zone::ruler && c.hitTest(label).meterBar == 4 && c.hitTest(label).tempo == -1, "the change is a label on the ruler");
    for (const char* bad : {"3/5", "three/4", "0/4", "40/4", "3", "/4", "3/"}) {
        rig.asked = false; rig.messages.clear();
        rig.doubleClick(label);
        require(rig.asked && rig.askCurrent == "3/4", "double-click on the label asks for its time signature");
        rig.answer(bad);
        require(rig.t().meterChanges[0].numerator == 3 && rig.t().meterChanges[0].denominator == 4 && !rig.messages.empty(),
            "an impossible time signature is refused with a message");
    }
    rig.asked = false;
    rig.doubleClick(label);
    rig.answer(" 6 / 8 ");
    require(rig.t().meterChanges[0].numerator == 6 && rig.t().meterChanges[0].denominator == 8, "6/8, spaces allowed");
    rig.doubleClick(label);
    rig.answer("3/4");
    // The grid's bar lines and beats follow the time signatures (bar 5 in 3/4, 6/8 from bar 7), at any zoom.
    require(rig.controller.editTimeline("6/8", [](tl::Timeline& t) { return t.setMeterChange(6, 6, 8); }), "fixture: 6/8 from bar 7");
    std::vector<std::int64_t> bars, beats;
    c.forEachGridLine(rig.t(), 3.5 * bar, 4 * bar + 4 * threeFour, 240, [&](std::int64_t tick, int kind, std::int64_t, std::int64_t) {
        if (kind == 2) bars.push_back(tick); else if (kind == 1) beats.push_back(tick);
    });
    require(bars == std::vector<std::int64_t>({3 * bar, 4 * bar, 4 * bar + threeFour, 4 * bar + 2 * threeFour, 4 * bar + 3 * threeFour, 4 * bar + 4 * threeFour}),
        "bar lines at every bar, each as long as its time signature");
    require(std::count_if(beats.begin(), beats.end(), [&](std::int64_t b) { return b > 4 * bar && b < 4 * bar + threeFour; }) == 2
        && std::count_if(beats.begin(), beats.end(), [&](std::int64_t b) { return b > 4 * bar + 2 * threeFour && b < 4 * bar + 3 * threeFour; }) == 5,
        "three beats in a 3/4 bar, six eighths in a 6/8 bar");
    std::vector<std::int64_t> sparse;
    c.forEachGridLine(rig.t(), 0, 12 * bar, 2 * bar, [&](std::int64_t tick, int kind, std::int64_t index, std::int64_t) { if (kind == 2) sparse.push_back(index); });
    require(sparse.size() >= 5 && std::find(sparse.begin(), sparse.end(), 4) != sparse.end() && std::find(sparse.begin(), sparse.end(), 6) != sparse.end(),
        "zoomed out, lines skip bars but never step over a time signature change");
    rig.controller.undo(); rig.view.refresh();
    // Snapping, the readout and looping a bar follow the time signature.
    require(c.snapTick(static_cast<double>(4 * bar + threeFour) - 300.0) == 4 * bar + threeFour, "bar snapping finds the 3/4 bar line");
    c.setSnap(Snap::beat);
    require(c.snapTick(static_cast<double>(4 * bar + threeFour + 1000)) == 4 * bar + threeFour + 960, "beat snapping counts the bar's beats");
    c.setSnap(Snap::bar);
    c.setCursor(4 * bar + threeFour + 960, false); rig.view.refresh();
    require(labelStartingWith(rig.view, "Bar 6.2  3/4") != nullptr, "the readout shows bar, beat and time signature");
    rig.doubleClick(rig.loopStrip(4 * bar + 100));
    require(rig.t().loopStart == 4 * bar && rig.t().loopEnd == 4 * bar + threeFour, "double-click on the loop strip loops a 3/4 bar");
    require(rig.key(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier) && c.cursorTick() == 4 * bar + 2 * threeFour + 960,
            "Shift+Right moves by the bar's own length");
    // The label menu.
    rig.rightClick(label);
    items = rig.menu.items();
    require(entry(items, TimelineCanvas::cmdJump) != nullptr && entry(items, TimelineCanvas::cmdMeter) != nullptr
        && entry(items, TimelineCanvas::cmdRemoveMeter) != nullptr, "the time signature menu");
    rig.seeks.clear();
    rig.menu.done(TimelineCanvas::cmdJump);
    require(!rig.seeks.empty() && rig.seeks.back() == 4 * bar, "Play from here starts at its bar");
    rig.rightClick(label);
    rig.menu.done(TimelineCanvas::cmdRemoveMeter);
    require(rig.t().meterChanges.empty() && rig.valid(), "Delete time signature change removes it");
    rig.controller.undo(); rig.view.refresh();
    rig.rightClick(rig.ruler(2 * bar));
    rig.menu.done(TimelineCanvas::cmdRemoveMeter);
    require(rig.t().meterChanges.empty(), "Remove all time signature changes");
    rig.messages.clear(); rig.asked = false;
    c.askMeter(rig.t().lengthBars);
    require(!rig.asked && !rig.messages.empty(), "no time signature past the end of the song");
}

static void markerChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    const auto before = rig.t();
    // Double-click on the strip adds a marker there.
    rig.doubleClick(rig.markerStrip(4 * bar));
    require(rig.t().markers.size() == 1 && rig.t().markers[0].tick == 4 * bar && rig.t().markers[0].name == "Marker 1", "double-click adds a marker at the pointer, snapped");
    const auto id = rig.t().markers[0].id;
    require(c.selectedMarkerId() == id && c.cursorTick() == 4 * bar, "it is selected and the play position goes there");
    require(rig.oneUndoRestores(before, true), "adding a marker is one undo step");
    // Clicking a marker jumps there.
    rig.seeks.clear();
    c.setCursor(0, false);
    rig.press(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    require(rig.seeks.size() == 1 && rig.seeks[0] == 4 * bar && c.cursorTick() == 4 * bar, "clicking a marker starts playback from it");
    // Dragging moves it.
    rig.drag(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0), {rig.markerStrip(6 * bar) + juce::Point<int>(4, 0)});
    require(rig.t().markers.size() == 1 && rig.t().markers[0].tick == 6 * bar && rig.t().markers[0].id == id, "dragging a marker moves it");
    rig.controller.undo(); rig.view.refresh();
    // Rename with the double-click or the menu.
    rig.doubleClick(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    require(rig.asked && rig.askTitle == "Rename marker" && rig.askCurrent == "Marker 1", "double-click on a marker asks for a new name");
    rig.answer("Chorus");
    require(rig.t().markers[0].name == "Chorus", "and renames it");
    rig.asked = false;
    rig.rightClick(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    auto items = rig.menu.items();
    require(entry(items, TimelineCanvas::cmdJump) != nullptr && entry(items, TimelineCanvas::cmdRename) != nullptr && entry(items, TimelineCanvas::cmdRemoveMarker) != nullptr, "the marker menu");
    int colours = 0;
    for (const auto& e : items) colours += e.id >= TimelineCanvas::cmdColourBase ? 1 : 0;
    (void)colours;
    rig.menu.done(TimelineCanvas::cmdRename);
    require(rig.asked && rig.askCurrent == "Chorus", "Rename in the menu asks too");
    rig.answer(juce::String("A name that is far longer than a marker can hold, to be cut"));
    require(rig.t().markers[0].name.size() <= static_cast<std::size_t>(tl::maxMarkerNameBytes) && rig.valid(), "a long name is shortened to fit");
    rig.answer("");
    require(rig.valid(), "an empty name leaves a valid marker");
    rig.rightClick(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    rig.menu.done(TimelineCanvas::cmdColourBase + 5);
    require(rig.t().markers[0].colour == 5, "a marker's colour can be chosen");
    rig.rightClick(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    rig.seeks.clear(); c.setCursor(0, false);
    rig.menu.done(TimelineCanvas::cmdJump);
    require(!rig.seeks.empty() && rig.seeks.back() == 4 * bar, "Play from here jumps to it");
    rig.rightClick(rig.markerStrip(4 * bar) + juce::Point<int>(4, 0));
    rig.menu.done(TimelineCanvas::cmdRemoveMarker);
    require(rig.t().markers.empty() && c.selectedMarkerId() == 0, "Delete marker removes it");
    // The ruler and strip menus add markers; the toolbar button does too.
    rig.rightClick(rig.ruler(7 * bar));
    require(rig.menu.shown && entry(rig.menu.items(), TimelineCanvas::cmdAddMarker) != nullptr, "right-click on the ruler offers a marker");
    rig.menu.done(TimelineCanvas::cmdAddMarker);
    require(rig.t().markers.size() == 1 && rig.t().markers[0].tick == 7 * bar, "and adds it at the pointer");
    c.setCursor(9 * bar, false);
    auto* add = childNamed<juce::TextButton>(rig.view, "Add marker");
    require(add != nullptr, "the toolbar has + Marker");
    clickButton(*add);
    require(rig.t().markers.size() == 2 && rig.t().markers[1].tick == 9 * bar, "at the play position");
    // Markers sit inside the song and there is a limit.
    for (int i = 0; i < 200; ++i) { c.setCursor((i % 60) * 240, false); c.addMarkerAtCursor(); }
    require(rig.t().markers.size() == static_cast<std::size_t>(tl::maxMarkers) && rig.valid(), "there is a limit to the markers and the song stays valid");
    rig.messages.clear();
    require(!c.addMarkerAtCursor() && !rig.messages.empty(), "past it, the user is told");
    std::cout << "PASS: markers\n";
}

// ---- playing from the timeline -------------------------------------------------------------------------------
static void playbackChecks() {
    Rig rig;
    rig.sources();
    auto& c = rig.c;
    c.setSnap(Snap::bar);
    // Place the audio clip with the mouse and play it from the middle.
    rig.doubleClick(rig.at(0, 2));
    require(rig.t().regions.size() == 1, "an audio clip is on the timeline");
    rig.seeks.clear();
    rig.press(rig.ruler(bar));
    require(rig.seeks.size() == 1 && rig.seeks[0] == bar && c.cursorTick() == bar, "a click on the ruler asks to play from there");
    rig.engine.playing.store(true);
    rig.render(9600);
    require(rig.engine.timelinePosition.load() >= bar, "playback starts at the clicked bar");
    require(rig.energy(0, 9600) > 0.5, "and the clip is heard from the middle");
    // Jump while playing, past the clip: silence.
    rig.press(rig.ruler(5 * bar));
    const auto mark = rig.rendered.size();
    rig.render(48000);
    require(rig.energy(mark + 4800, mark + 48000) < 1e-9, "jumping past the clip while playing falls silent");
    require(rig.engine.timelinePosition.load() >= 5 * bar, "and the playhead went there");
    // Back into the clip.
    rig.press(rig.ruler(0));
    const auto mark2 = rig.rendered.size();
    rig.render(9600);
    require(rig.energy(mark2 + 480, mark2 + 9600) > 0.5, "jumping back into it brings the sound back");
    // The scrub drag moves the play position as it goes.
    rig.seeks.clear();
    rig.drag(rig.ruler(0), {rig.ruler(bar), rig.ruler(2 * bar), rig.ruler(3 * bar)});
    require(rig.seeks.size() >= 3 && c.cursorTick() == 3 * bar, "dragging along the ruler scrubs");
    // Alt keeps the exact position; the end of the song clamps.
    rig.press(rig.ruler(1000), altLeft);
    require(std::abs(c.cursorTick() - 1000) <= 20, "Alt+click keeps the exact position");
    c.setScrollTicks(c.maxScrollTicks());
    rig.press(rig.ruler(static_cast<std::int64_t>(c.scrollTicks() + c.visibleTicks()) - 30));
    require(c.cursorTick() <= rig.t().length(), "a click past the end of the song lands at the end");
    // The marker strip scrubs too.
    c.setScrollTicks(0);
    rig.press(rig.markerStrip(7 * bar));
    require(c.cursorTick() == 7 * bar, "a click on an empty part of the marker strip sets the play position");
    // Clicks on the ruler land on the grid; Alt (tested above) keeps the exact spot.
    rig.seeks.clear();
    rig.press(rig.ruler(2 * bar + 1000));
    require(c.cursorTick() == 2 * bar && rig.seeks.size() == 1 && rig.seeks[0] == 2 * bar, "a click on the ruler snaps to the nearest bar and plays from there");
    rig.press(rig.ruler(2 * bar + 2900));
    require(c.cursorTick() == 3 * bar, "and to the next one when it is nearer");
    c.setSnap(Snap::sixteenth);
    rig.press(rig.ruler(bar + 1000));
    require(c.cursorTick() == bar + 960, "with a finer grid it lands on a sixteenth");
    c.setSnap(Snap::bar);
    // Export menu.
    auto* exportButton = childNamed<juce::TextButton>(rig.view, "Export WAV");
    require(exportButton != nullptr, "the toolbar has Export WAV");
    clickButton(*exportButton);
    require(rig.viewMenu.shown && rig.viewMenu.items().size() == 4, "it offers stereo and stem exports for song and loop");
    require(rig.viewMenu.items()[0].enabled && rig.viewMenu.items()[0].text.contains("16 bars"), "the whole song, with its length");
    rig.viewMenu.done(1);
    c.setLoopRange(2 * bar, 4 * bar);
    rig.viewMenu.reset();
    clickButton(*exportButton);
    require(rig.viewMenu.items()[1].text.contains("bars 3 to 4"), "the loop entry names its bars");
    rig.viewMenu.done(2);
    require(rig.exports.size() == 2 && rig.exports[0] == std::make_pair<std::int64_t, std::int64_t>(0, -1) && rig.exports[1] == std::make_pair(2 * bar, 4 * bar), "the choices reach the exporter");
    std::vector<std::pair<std::int64_t,std::int64_t>> stems;
    rig.view.onExportStems = [&](std::int64_t from, std::int64_t to) { stems.emplace_back(from,to); };
    rig.viewMenu.done(3); rig.viewMenu.done(4);
    require(stems.size()==2 && stems[0]==std::make_pair<std::int64_t,std::int64_t>(0,-1)
        && stems[1]==std::make_pair(2*bar,4*bar), "stem choices reach exporter with correct range");
    // With nothing on the timeline the export entries are off.
    Rig empty;
    auto* emptyExport = childNamed<juce::TextButton>(empty.view, "Export WAV");
    clickButton(*emptyExport);
    require(empty.viewMenu.shown && !empty.viewMenu.items()[0].enabled && !empty.viewMenu.items()[1].enabled, "an empty timeline cannot be exported");
    require(!empty.viewMenu.items()[2].enabled && !empty.viewMenu.items()[3].enabled, "empty song cannot export stems");
    std::cout << "PASS: playback from any position\n";
}

// ---- the song-length control and the readout ----------------------------------------------------------------
static void toolbarChecks() {
    Rig rig;
    rig.song();
    auto& c = rig.c;
    auto* length = childNamed<juce::Slider>(rig.view, "Song length");
    require(length != nullptr && static_cast<int>(length->getValue()) == rig.t().lengthBars, "the length control shows the song's length");
    length->setValue(32, juce::sendNotificationSync);
    require(rig.t().lengthBars == 32 && rig.valid(), "changing it lengthens the song");
    length->setValue(2, juce::sendNotificationSync);
    require(rig.t().lengthBars == 3 && rig.t().lengthBars == rig.t().minLengthBars(), "it cannot be shorter than the last clip (which ends at bar 3)");
    require(static_cast<int>(length->getValue()) == rig.t().lengthBars, "and the control shows the real length");
    require(rig.view.subtitle().contains("bars") && rig.view.subtitle().contains("4 clips") && rig.view.subtitle().contains("loop off"), "the screen's subtitle summarizes the song");
    // Zoom buttons.
    const double ppt = c.pixelsPerTick();
    clickButton(*childNamed<juce::TextButton>(rig.view, "Zoom in"));
    require(c.pixelsPerTick() > ppt, "the + button zooms in");
    clickButton(*childNamed<juce::TextButton>(rig.view, "Zoom out"));
    require(std::abs(c.pixelsPerTick() - ppt) < 1e-9, "the - button zooms out");
    clickButton(*childNamed<juce::TextButton>(rig.view, "Fit song"));
    require(c.visibleTicks() >= static_cast<double>(rig.t().contentEnd()), "Fit shows the whole song");
    // Changes made elsewhere (undo, opening a project) are picked up by refresh().
    c.setSelection({rig.id.a});
    rig.controller.newProject();
    rig.view.refresh();
    require(rig.t().regions.empty() && rig.selectionIs({}) && c.cursorTick() == 0, "a new project leaves nothing selected");
    std::cout << "PASS: toolbar\n";
}

// ---- robustness ------------------------------------------------------------------------------------------------------
static void robustnessChecks() {
    Rig rig;
    auto& c = rig.c;
    // Nothing to work on: every command is safe.
    require(!c.deleteSelection() && !c.copySelection() && !c.cutSelection() && !c.duplicateSelection() && !c.toggleMuteSelection() && !c.ungroupSelection(), "commands with no selection do nothing");
    rig.messages.clear();
    require(!c.paste() && !rig.messages.empty(), "paste with nothing copied says so");
    c.selectAll();
    require(c.selection().empty(), "select-all on an empty timeline selects nothing");
    c.jumpToMarker(true); c.jumpToMarker(false);
    rig.messages.clear();
    require(rig.letter('s') && rig.t().regions.empty() && !rig.messages.empty(), "S on an empty timeline changes nothing and says why");
    // Events arriving out of order.
    c.mouseUp(mouse(c, {400, 200}));
    c.mouseDrag(mouse(c, {500, 200}, leftButton, 1, true));
    c.mouseDoubleClick(mouse(c, {10, 10}, leftButton, 2));
    c.mouseDoubleClick(mouse(c, {5000, 5000}, leftButton, 2));
    c.mouseMove(mouse(c, {-5, -5}, none));
    rig.press({-20, -20});
    rig.press({c.getWidth() + 50, c.getHeight() + 50});
    rig.rightClick({-20, 300});
    rig.rightClick({10, c.rowTop(0) + 10});
    rig.rightClick({10, 5});
    require(!c.dragging(), "stray events leave no drag behind");
    // A drag released outside the window still ends cleanly.
    rig.song();
    c.setSnap(Snap::sixteenth);
    rig.drag(rig.at(1000, 0), {{c.getWidth() + 400, c.getHeight() + 400}});
    require(rig.valid() && !c.dragging(), "dragging out of the window leaves a valid song");
    rig.drag(rig.at(1000, 0), {{-500, -500}});
    require(rig.valid() && !c.dragging(), "and out the other way");
    // Deleting what the timeline refers to prunes it, and the clipboard follows.
    c.setSelection({rig.id.au}); c.copySelection();
    c.setSelection({rig.id.a, rig.id.au}); c.copySelection();
    require(rig.deleteTrack(2), "delete a track");
    rig.view.refresh();
    require(rig.valid() && !rig.has(rig.id.au), "deleting a track takes its clips off the timeline");
    c.setCursor(10 * bar, false);
    const auto count = rig.t().regions.size();
    c.paste();
    require(rig.valid() && rig.t().regions.size() == count + 1, "pasting what was copied skips clips whose track is gone");
    // Pattern numbers mean something else once patterns are added or removed: a copied pattern is not pasted after that.
    c.setSelection({rig.id.p});
    require(c.copySelection(), "copy the pattern clip");
    require(rig.controller.editRack("Add pattern", [](rack::Project& project) { return project.addPattern(); }), "add a pattern to the rack");
    rig.view.refresh();
    const auto regionsNow = rig.t().regions.size();
    rig.messages.clear();
    c.setCursor(14 * bar, false);
    require(!c.paste() && rig.t().regions.size() == regionsNow && !rig.messages.empty(), "a copied pattern is not pasted once the rack's patterns have changed");
    c.setSelection({rig.id.p});
    require(c.copySelection() && c.paste() && rig.t().regions.size() == regionsNow + 1 && rig.valid(), "copied again, it pastes");
    rig.controller.deleteRackPattern(0);
    rig.controller.addScene();
    rig.view.refresh();
    require(rig.valid(), "after deleting a pattern the view is consistent");
    // A very long song and extreme zoom paint without trouble and quickly.
    rig.controller.editTimeline("long", [](tl::Timeline& tlm) { return tlm.setLengthBars(tl::maxBars); });
    rig.view.refresh();
    require(c.place(0, 0, static_cast<std::int64_t>(tl::maxBars - 100) * bar) && rig.valid(), "a clip targetColour the end of an 18-hour song");
    rig.controller.editTimeline("big", [](tl::Timeline& tlm) { tl::Region r; r.row = 1; r.source = 1; r.start = 0; r.length = 500 * bar; return tlm.place(r); });
    rig.view.refresh();
    const auto begin = std::chrono::steady_clock::now();
    for (const double scale : {0.0001, 1.0, 100000.0}) {
        c.zoomBy(scale > 1.0 ? 1000.0 : scale < 1.0 ? 0.0001 : 1.0);
        for (const double scroll : {0.0, 1e5, 1e6}) {
            c.setScrollTicks(scroll);
            (void)c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
        }
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    require(seconds < 20.0, "painting a huge song at every zoom is quick");
    std::cout << "PASS: robustness\n";
}

// ---- how it looks ------------------------------------------------------------------------------------------------------
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
static int pixelsOfColour(const juce::Image& image, juce::Rectangle<int> area, juce::Colour targetColour, int tolerance) {
    int n = 0;
    for (int y = std::max(0, area.getY()); y < std::min(image.getHeight(), area.getBottom()); ++y)
        for (int x = std::max(0, area.getX()); x < std::min(image.getWidth(), area.getRight()); ++x) {
            const auto p = image.getPixelAt(x, y);
            n += std::abs(p.getRed() - targetColour.getRed()) + std::abs(p.getGreen() - targetColour.getGreen()) + std::abs(p.getBlue() - targetColour.getBlue()) <= tolerance ? 1 : 0;
        }
    return n;
}

static void snapshots() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const char* directory = std::getenv("ALIEN_SNAPSHOT_DIR");
    {
        Rig rig(1280, 640);
        rig.view.refresh();
        const auto empty = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
        require(hasDetail(empty), "an empty timeline draws its ruler, rows and toolbar");
        writePng(empty, directory, "timeline-empty.png");
    }
    {
        Rig rig(1280, 640);
        rig.song();
        rig.controller.editTimeline("more", [&](tl::Timeline& tlm) {
            std::uint32_t extra = 0;
            tl::Region r; r.row = 0; r.source = 0; r.start = 4 * bar; r.length = 3 * bar; tlm.place(r, &extra);
            r.row = tl::rackRowBase; r.source = 0; r.start = 5 * bar; r.length = 2 * bar; tlm.place(r, &extra);
            tlm.setMuted({extra}, true);
            tlm.addMarker(0, "Intro", 0); tlm.addMarker(4 * bar, "Verse", 2); tlm.addMarker(8 * bar, "Chorus", 5);
            tlm.setLoop(2 * bar, 6 * bar); tlm.setLoopEnabled(true);
            return true; });
        rig.view.refresh();
        rig.c.setCursor(3 * bar + 960, false);
        rig.c.setSelection({rig.id.b, rig.id.au});
        rig.view.setPlayhead(5 * bar + 100);
        const auto image = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
        require(hasDetail(image), "the song draws");
        // Each kind of clip draws something of its own inside its box (looked at on the canvas alone, at its own origin).
        const auto& c = rig.c;
        const auto canvasImage = rig.c.createComponentSnapshot(rig.c.getLocalBounds(), true, 1.0f);
        const auto region = [&](std::uint32_t id) { return c.regionBounds(id).toNearestInt(); };
        require(pixelsOfColour(canvasImage, region(rig.id.a), ui::trackColour(0), 90) > 20, "a MIDI clip shows its notes in the track's colour");
        require(pixelsOfColour(canvasImage, region(rig.id.au), ui::trackColour(2), 90) > 200, "an audio clip shows its waveform");
        require(pixelsOfColour(canvasImage, region(rig.id.p), ui::colours::warning, 120) > 5, "a pattern shows its steps");
        const auto emptyRow = juce::Rectangle<int>(TimelineCanvas::headerWidth + 5, c.rowTop(3) + 3, 600, TimelineCanvas::rowHeight - 6);
        require(pixelsOfColour(canvasImage, emptyRow, ui::trackColour(3), 90) == 0, "an empty track draws no clip");
        writePng(image, directory, "timeline-song.png");

        rig.c.zoomBy(6.0);
        rig.c.setScrollTicks(2.0 * bar);
        rig.c.setSelection({rig.id.a});
        const auto zoomed = rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f);
        require(hasDetail(zoomed), "a zoomed-in timeline draws");
        writePng(zoomed, directory, "timeline-zoomed.png");
        rig.c.zoomToFit();
        writePng(rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f), directory, "timeline-fit.png");
        // A drag in progress is drawn too.
        rig.c.zoomBy(0.2);
        rig.c.mouseDown(mouse(rig.c, rig.at(bar / 2, 0)));
        rig.c.mouseDrag(mouse(rig.c, rig.at(bar / 2, 0) + juce::Point<int>(180, 0), leftButton, 1, true));
        writePng(rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f), directory, "timeline-dragging.png");
        rig.c.mouseUp(mouse(rig.c, rig.at(bar / 2, 0) + juce::Point<int>(180, 0), leftButton, 1, true));
        rig.c.setSelection({});
        rig.c.mouseDown(mouse(rig.c, rig.at(30 * bar, 3)));
        rig.c.mouseDrag(mouse(rig.c, rig.at(2 * bar, 1), leftButton, 1, true));
        writePng(rig.view.createComponentSnapshot(rig.view.getLocalBounds(), true, 1.0f), directory, "timeline-marquee.png");
        rig.c.mouseUp(mouse(rig.c, rig.at(2 * bar, 1), leftButton, 1, true));
    }
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::cout << "PASS: the timeline renders\n";
}

static void takeCompChecks() {
    auto rig = std::make_unique<Rig>();
    rig->sources();
    require(rig->controller.editTimeline("Take fixture", [](tl::Timeline& t) {
        tl::Region take; take.row = 2; take.source = 0; take.length = 2 * bar;
        return t.recordTake(take);
    }), "fixture: recorded pass");
    rig->view.refresh();
    auto* toggle = childNamed<juce::TextButton>(rig->view, "Show recording takes");
    require(toggle != nullptr, "takes toggle available"); clickButton(*toggle);
    auto* panel = childNamed<RecordingTakesPanel>(rig->view, "Recording takes");
    require(panel && panel->isVisible(), "recording take panel shown");
    auto* range = childNamed<juce::TextButton>(*panel, "Comp loop range");
    auto* whole = childNamed<juce::TextButton>(*panel, "Comp whole take");
    require(range && whole && whole->isEnabled() && !range->isEnabled(), "range beyond take coverage disabled");
    require(rig->controller.editTimeline("Comp range", [](tl::Timeline& t) { return t.setLoop(960, 1920); }), "set comp range");
    rig->view.refresh(); require(range->isEnabled(), "covered range enables comp");
    clickButton(*range);
    require(rig->controller.undoLabel() == "Comp recording take" && rig->t().regions.size() == 3
        && rig->t().takes.size() == 1, "panel comps range and retains original");
    require(rig->controller.undo() && rig->t().regions.size() == 1, "panel comp undo");
    clickButton(*whole); require(rig->t().takes.size() == 1, "whole take comp retains source");
    auto* forget = childNamed<juce::TextButton>(*panel, "Forget recording take");
    const auto active = rig->t().regions;
    require(forget && forget->isEnabled(), "forget action available"); clickButton(*forget);
    require(rig->t().takes.empty() && rig->t().regions == active, "forget leaves active comp intact");
    require(rig->controller.undo() && rig->t().takes.size() == 1, "forget is undoable");
    rig->view.refresh();
    for (const int width : {960, 1200}) {
        rig->view.setSize(width, 650);
        require(rig->view.getLocalBounds().contains(panel->getBounds())
            && panel->getLocalBounds().contains(range->getBounds()), "take panel fits minimum layout");
    }
    writePng(rig->view.createComponentSnapshot(rig->view.getLocalBounds(), true, 1.0f),
        std::getenv("ALIEN_SNAPSHOT_DIR"), "recording-takes.png");
}

// ---- take lanes ------------------------------------------------------------------------------------------------------
static std::shared_ptr<SessionAudioClip> toneClip(int seconds, float level) {
    auto audio = std::make_shared<SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = static_cast<std::size_t>(seconds) * 8000;
    audio->left.resize(audio->frames);
    for (std::size_t i = 0; i < audio->left.size(); ++i) audio->left[i] = level * std::sin(static_cast<float>(i) * 0.2f);
    audio->buildWaveform();
    return audio;
}
static std::vector<std::pair<std::int64_t, std::int64_t>> plays(const Rig& rig, std::uint32_t take) {
    return rig.t().takePlays(take, rig.controller.songTempo());
}
using Parts = std::vector<std::pair<std::int64_t, std::int64_t>>;

static void takeLaneChecks() {
    auto rig = std::make_unique<Rig>();
    auto& c = rig->c;
    rig->sources();
    c.setSnap(Snap::sixteenth);
    constexpr int rh = TimelineCanvas::rowHeight, lh = TimelineCanvas::takeLaneHeight;
    const auto header = [&](int track) { return juce::Point<int>{40, c.rowTop(c.displayRowOf(track)) + 20}; };
    const auto lane = [&](std::int64_t tick, int track, int index) {
        return juce::Point<int>{juce::roundToInt(c.xForTick(static_cast<double>(tick))), c.takeLaneBounds(track, index).getCentreY()};
    };

    // A track without takes has no lanes, and cannot open them.
    const int plainHeight = c.contentHeight();
    require(c.takesOf(2).empty() && !c.toggleTakeLanes(2) && c.takeLaneCount(2) == 0 && c.blockHeight(2) == rh, "no takes: no lanes");
    rig->rightClick(header(2));
    auto headerItems = rig->menu.items();
    const auto* showItem = entry(headerItems, TimelineCanvas::cmdTakeLanes);
    require(showItem != nullptr && !showItem->enabled, "the track menu cannot show the lanes of a track without takes");

    // Two single passes on track 3, from two recorded files (scenes 0 and 1); the later one plays.
    require(rig->controller.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("timeline-view-take-2.wav"), 2, 1, toneClip(4, 0.8f)),
            "install a second take's audio");
    for (const int scene : {0, 1})
        require(rig->controller.editTimeline("Take fixture", [=](tl::Timeline& t) {
            tl::Region take; take.row = 2; take.source = scene; take.length = 2 * bar;
            return t.recordTake(take);
        }), "fixture: a recorded pass");
    rig->view.refresh();
    const auto first = rig->t().takes[0].id, second = rig->t().takes[1].id;
    require(plays(*rig, first).empty() && plays(*rig, second) == Parts{{0, 2 * bar}}, "only the last pass plays");
    require(c.takeLaneCount(2) == 0 && c.contentHeight() == plainHeight, "lanes start hidden");

    // The header button shows the lanes; it is a view setting, not an undo step.
    const auto document = rig->t();
    const auto undoBefore = rig->controller.undoLabel();
    const auto rowBelow = c.rowTop(3);
    rig->press(c.takeLanesButtonRect(2).getCentre());
    require(c.takeLanesShown(2) && c.takeLaneCount(2) == 2 && c.blockHeight(2) == rh + 2 * lh, "the header button opens one lane per take");
    require(rig->t() == document && rig->controller.undoLabel() == undoBefore, "showing lanes changes nothing in the song or the undo history");
    require(c.rowTop(3) == rowBelow + 2 * lh && c.contentHeight() == plainHeight + 2 * lh, "the rows below move down by the lanes");
    require(c.rowTop(2) + rh == c.takeLaneBounds(2, 0).getY() && c.takeLaneBounds(2, 1).getY() == c.takeLaneBounds(2, 0).getBottom(), "lanes sit under the row, in order");
    for (int y = c.rowTop(2); y < c.rowTop(3); ++y) require(c.displayRowAt(y) == 2, "every pixel of the row and its lanes belongs to it");
    require(c.displayRowAt(c.rowTop(3)) == 3 && c.displayRowAt(c.rowTop(1)) == 1, "the rows around it are where they were drawn");
    for (int d = 0; d + 1 < c.displayRowCount(); ++d) require(c.rowTop(d + 1) - c.rowTop(d) == c.blockHeight(d), "rows and lanes tile");
    tl::Region below; below.row = 3; below.start = 0; below.length = bar;
    require(static_cast<int>(c.regionBounds(below).getY()) == c.rowTop(3) + 3, "a clip below the lanes is drawn on its own row");

    // Hit-testing: the lanes and their labels are takes; the row itself is unchanged.
    auto hit = c.hitTest(lane(bar / 2, 2, 0));
    require(hit.zone == Zone::take && hit.take == first && hit.takeLane == 0 && hit.displayRow == 2, "the first lane is the first take");
    hit = c.hitTest(lane(bar / 2, 2, 1));
    require(hit.zone == Zone::take && hit.take == second && hit.takeLane == 1, "the second lane is the second take");
    hit = c.hitTest({30, c.takeLaneBounds(2, 1).getCentreY()});
    require(hit.zone == Zone::take && hit.take == second, "a lane's label in the header is its take");
    require(c.hitTest(rig->at(bar / 2, 2)).zone == Zone::region, "the clip on the row is still a clip");
    require(c.hitTest(rig->at(bar / 2, 3)).zone == Zone::lane && c.hitTest(rig->at(bar / 2, 3)).displayRow == 3, "the row below is still its own row");
    require(c.hitTest(header(3)).zone == Zone::header && c.hitTest(header(3)).displayRow == 3, "the header below is its own track");
    const auto box = c.takeBounds(first);
    require(std::abs(box.getX() - static_cast<float>(c.xForTick(0))) < 0.5f && std::abs(box.getRight() - static_cast<float>(c.xForTick(2.0 * bar))) < 0.5f
            && c.takeLaneBounds(2, 0).toFloat().contains(box), "a take is drawn at its song position, inside its lane");

    // A click picks a take (for the menu) and plays from there; nothing is edited.
    rig->press(lane(bar, 2, 0));
    require(c.selectedTakeId() == first && c.cursorTick() == bar && rig->t() == document, "a click on a lane picks its take");

    // A drag across a lane comps that range from the take, as one undo step, keeping the take.
    const auto takes = rig->t().takes;
    rig->drag(lane(bar / 2, 2, 0), {lane(bar, 2, 0), lane(3 * bar / 2, 2, 0)});
    require(rig->controller.undoLabel() == "Comp recording take" && rig->t().takes == takes, "a drag comps through the take path and keeps the takes");
    const auto* comped = rig->t().regionAt(2, bar);
    require(comped != nullptr && comped->source == 0 && comped->start == bar / 2 && comped->length == bar, "the dragged range now plays the first take");
    require(std::abs(comped->audioOffset - 1.0) < 1e-9, "the comp starts one second into the take's file (half a bar at 120 BPM)");
    require(comped->fadeIn > 0 && comped->fadeOut > 0, "the comp's edges get short fades");
    require(plays(*rig, first) == Parts{{bar / 2, 3 * bar / 2}} && plays(*rig, second) == Parts{{0, bar / 2}, {3 * bar / 2, 2 * bar}},
            "each lane knows which of its parts plays");
    require(rig->oneUndoRestores(document, true), "one undo takes the comp back");
    // Right to left, and past the take's end: the range is clamped to the take.
    const auto comped1 = rig->t();
    rig->drag(lane(3 * bar, 2, 0), {lane(5 * bar / 2, 2, 0), lane(7 * bar / 4, 2, 0)});
    require(plays(*rig, first) == Parts{{bar / 2, 3 * bar / 2}, {7 * bar / 4, 2 * bar}}, "a drag from past the end, leftwards, comps up to the take's end");
    require(rig->oneUndoRestores(comped1), "and is one undo step");

    // The playing part of a lane is drawn brighter than the rest.
    {
        ui::StudioLookAndFeel look;
        juce::LookAndFeel::setDefaultLookAndFeel(&look);
        const auto image = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
        const auto lit = c.takeBounds(first).toNearestInt().withX(juce::roundToInt(c.xForTick(bar / 2.0)) + 2).withWidth(juce::roundToInt(bar * c.pixelsPerTick()) - 4);
        const auto dim = lit.withX(juce::roundToInt(c.xForTick(1.5 * bar)) + 2).withWidth(juce::roundToInt(bar / 2.0 * c.pixelsPerTick()) - 4);
        const auto bright = ui::trackColour(rig->controller.document().tracks[2].colour);
        const int litPixels = pixelsOfColour(image, lit, bright, 90), dimPixels = pixelsOfColour(image, dim, bright, 90);
        require(litPixels > 40 && litPixels > 4 * dimPixels, "the part of a take that plays is highlighted");
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    // The lane menu.
    rig->rightClick(lane(bar, 2, 1));
    auto items = rig->menu.items();
    require(c.selectedTakeId() == second && entry(items, TimelineCanvas::cmdPlayTake) && entry(items, TimelineCanvas::cmdPlayTake)->enabled
            && entry(items, TimelineCanvas::cmdForgetTake) && entry(items, TimelineCanvas::cmdForgetTake)->enabled, "a lane offers its take");
    require(entry(items, TimelineCanvas::cmdPlayTakeLoop) && !entry(items, TimelineCanvas::cmdPlayTakeLoop)->enabled, "a loop range beyond the take cannot be comped");
    rig->menu.done(TimelineCanvas::cmdPlayTake);
    require(rig->controller.undoLabel() == "Comp recording take" && plays(*rig, second) == Parts{{0, 2 * bar}} && plays(*rig, first).empty(),
            "Play this take puts all of it back");
    require(rig->controller.editTimeline("Loop", [](tl::Timeline& t) { return t.setLoop(bar / 4, bar / 2); }), "set a loop inside the take");
    rig->rightClick(lane(bar, 2, 0));
    require(entry(rig->menu.items(), TimelineCanvas::cmdPlayTakeLoop)->enabled, "a loop range inside the take can be comped");
    rig->menu.done(TimelineCanvas::cmdPlayTakeLoop);
    require(plays(*rig, first) == Parts{{bar / 4, bar / 2}}, "the loop range plays the first take");
    const auto regions = rig->t().regions;
    rig->rightClick(lane(bar, 2, 0));
    rig->menu.done(TimelineCanvas::cmdForgetTake);
    require(rig->controller.undoLabel() == "Forget recording take" && rig->t().takes.size() == 1 && rig->t().regions == regions,
            "Forget take drops the take, not what plays");
    require(c.takeLaneCount(2) == 1 && c.contentHeight() == plainHeight + lh && c.hitTest(lane(bar, 2, 0)).take == second, "its lane goes; the next moves up");
    require(rig->controller.undo(), "undo the forget");
    rig->view.refresh();
    require(c.takeLaneCount(2) == 2, "undo brings its lane back");
    rig->rightClick(lane(bar, 2, 1));
    rig->menu.done(TimelineCanvas::cmdTakeLanes);
    require(!c.takeLanesShown(2) && c.contentHeight() == plainHeight, "Hide take lanes closes them");

    // The track menu shows and hides them too, ticked while shown.
    rig->rightClick(header(2));
    headerItems = rig->menu.items();
    showItem = entry(headerItems, TimelineCanvas::cmdTakeLanes);
    require(showItem != nullptr && showItem->enabled && showItem->text.contains("2"), "the track menu offers the lanes, with the take count");
    rig->menu.done(TimelineCanvas::cmdTakeLanes);
    require(c.takeLaneCount(2) == 2, "and opens them");
    auto* vertical = scrollBar(rig->view, true);
    require(vertical != nullptr && static_cast<int>(vertical->getMaximumRangeLimit()) == std::max(c.contentHeight(), c.laneAreaHeight()), "the scroll bar covers the lanes");
    c.scrollRowIntoView(tl::rackRowBase + 3);
    require(c.rowTop(c.displayRowOf(tl::rackRowBase + 3)) + rh <= c.getHeight(), "the last row can still be scrolled into view");
    c.setScrollY(0);

    // Lanes hide when the track has no takes left, and are gone with the track.
    require(c.forgetTake(first) && c.forgetTake(second), "forget both takes from their lanes");
    require(c.takeLaneCount(2) == 0 && c.blockHeight(2) == rh && c.contentHeight() == plainHeight, "a track without takes shows no lanes");
    require(c.hitTest({c.takeLanesButtonRect(2).getCentreX(), c.takeLanesButtonRect(2).getCentreY()}).zone == Zone::header, "and no lane button");
    require(rig->controller.undo() && rig->controller.undo(), "bring the takes back");
    rig->view.refresh();
    require(c.takeLaneCount(2) == 2, "the lanes come back with them");
    require(rig->deleteTrack(2), "delete the track");
    rig->view.refresh();
    require(rig->valid() && rig->t().takes.empty() && c.takeLaneCount(2) == 0 && c.contentHeight() == c.displayRowCount() * rh, "deleting a track takes its lanes");
    std::cout << "PASS: take lanes\n";
}

static void loopPassLaneChecks() {
    auto rig = std::make_unique<Rig>();
    auto& c = rig->c;
    rig->sources();
    c.setSnap(Snap::sixteenth);
    // One loop recording of three one-bar passes from one 6-second file; the last pass plays.
    require(rig->controller.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("timeline-view-loop-take.wav"), 1, 0, toneClip(6, 0.6f)),
            "install the loop recording");
    tl::Ids ids;
    require(rig->controller.editTimeline("Loop take fixture", [&](tl::Timeline& t) {
        std::vector<tl::Region> passes;
        for (int pass = 0; pass < 3; ++pass) {
            tl::Region r; r.row = 1; r.source = 0; r.start = bar; r.length = bar; r.audioOffset = 2.0 * pass;
            passes.push_back(r);
        }
        return t.recordTakes(passes, 2, &ids);
    }), "fixture: three loop passes");
    rig->view.refresh();
    require(ids.size() == 3 && c.toggleTakeLanes(1) && c.takeLaneCount(1) == 3, "a loop recording opens one lane per pass");
    for (int pass = 0; pass < 3; ++pass) {
        const auto hit = c.hitTest({juce::roundToInt(c.xForTick(1.5 * bar)), c.takeLaneBounds(1, pass).getCentreY()});
        require(hit.zone == Zone::take && hit.take == ids[static_cast<std::size_t>(pass)], "passes are in the order they were played");
        require(std::abs(c.takeBounds(ids[static_cast<std::size_t>(pass)]).getX() - static_cast<float>(c.xForTick(static_cast<double>(bar)))) < 0.5f,
                "every pass is drawn at the loop's position");
    }
    require(plays(*rig, ids[0]).empty() && plays(*rig, ids[1]).empty() && plays(*rig, ids[2]) == Parts{{bar, 2 * bar}}, "the last pass plays");
    // Comp the middle of the second pass: its audio comes from that pass's part of the file.
    const auto before = rig->t();
    const auto y = c.takeLaneBounds(1, 1).getCentreY();
    rig->drag({juce::roundToInt(c.xForTick(1.25 * bar)), y}, {{juce::roundToInt(c.xForTick(1.75 * bar)), y}});
    const auto* comped = rig->t().regionAt(1, 3 * bar / 2);
    require(comped != nullptr && comped->start == 5 * bar / 4 && comped->length == bar / 2 && std::abs(comped->audioOffset - 2.5) < 1e-9,
            "a comp from the second pass reads 2.5 s into the file");
    require(rig->t().takes == before.takes && plays(*rig, ids[1]) == Parts{{5 * bar / 4, 7 * bar / 4}}
            && plays(*rig, ids[2]) == Parts{{bar, 5 * bar / 4}, {7 * bar / 4, 2 * bar}}, "the passes are kept and show what plays");
    require(rig->oneUndoRestores(before), "the comp is one undo step");
    std::cout << "PASS: loop passes in take lanes\n";
}

static void freezeMenuChecks() {
    auto rig=std::make_unique<Rig>();rig->song();auto& c=rig->c;
    int frozen=-1,unfrozen=-1;
    rig->view.onFreezeTrack=[&](int track){
        frozen=track;const auto expected=rig->controller.timeline();
        auto audio=std::const_pointer_cast<SessionAudioClip>(rig->controller.document().clip(2,0).audio);
        require(rig->controller.installFrozenTrack(track,juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Freeze-UI.wav"),audio,expected),"UI installs a frozen track");
        rig->view.refresh();
    };
    rig->view.onUnfreezeTrack=[&](int track){unfrozen=track;require(rig->controller.unfreezeTrack(track),"UI unfreezes track");rig->view.refresh();};
    const auto header=juce::Point<int>{40,c.rowTop(c.displayRowOf(2))+20};
    rig->rightClick(header);require(entry(rig->menu.items(),TimelineCanvas::cmdFreeze)!=nullptr,"track header offers freeze");
    rig->menu.done(TimelineCanvas::cmdFreeze);
    require(frozen==2 && c.inputText(2)=="Frozen audio","freeze callback and header label");
    rig->rightClick(header);
    require(entry(rig->menu.items(),TimelineCanvas::cmdUnfreeze)!=nullptr
        && !entry(rig->menu.items(),TimelineCanvas::cmdArm)->enabled,"frozen header offers unfreeze and disables arming");
    writePng(rig->view.createComponentSnapshot(rig->view.getLocalBounds(),true,1.0f),std::getenv("ALIEN_SNAPSHOT_DIR"),"frozen-track.png");
    rig->menu.done(TimelineCanvas::cmdUnfreeze);
    require(unfrozen==2 && !rig->controller.isTrackFrozen(2) && c.inputText(2)!="Frozen audio","unfreeze restores editable track");
}

static void sendAutomationEditorChecks() {
    auto rig = std::make_unique<Rig>();
    auto* editor = childNamed<TrackAutomationEditor>(rig->view, "Track automation");
    require(editor != nullptr, "automation editor exists");
    editor->setSize(900, 180);
    auto* target = childNamed<juce::ComboBox>(*editor, "Automation parameter");
    auto* read = childNamed<juce::TextButton>(*editor, "Automation read");
    require(target != nullptr && target->getNumItems() == 6 && read != nullptr, "all send targets and Read are offered");
    for (int k = 0; k < 4; ++k) {
        target->setSelectedId(3 + k, juce::sendNotificationSync);
        // Top and bottom of the graph are unity and off for sends.
        editor->mouseDown(mouse(*editor, {100, 42}));
        editor->mouseDrag(mouse(*editor, {200, 42}, leftButton, 1, true));
        editor->mouseUp(mouse(*editor, {200, 42}));
        const auto& lanes = rig->controller.timeline().automationLanes;
        require(lanes.size() == static_cast<std::size_t>(k + 1) && lanes.back().parameter == static_cast<alien::automation::Parameter>(2 + k)
            && lanes.back().points.size() == 1 && lanes.back().points[0].value == 1, "send drag creates one unity point on chosen return");
        require(rig->controller.undo() && rig->controller.timeline().automationLanes.size() == static_cast<std::size_t>(k), "one undo removes whole gesture");
        require(rig->controller.redo(), "redo restores send curve");
    }
    clickButton(*read);
    require(!rig->controller.timeline().automationLanes.back().enabled, "Read disables selected send curve");
    require(rig->controller.undo() && rig->controller.timeline().automationLanes.back().enabled, "Read toggle is undoable");
    editor->mouseDown(mouse(*editor, {400, 171}));
    editor->mouseUp(mouse(*editor, {400, 171}));
    require(rig->controller.timeline().automationLanes.back().points.back().value < 0.01f, "send graph bottom means off, not negative gain");
}

// A placement clip (T1-03 B2b): a Song region with notes of its own and no Session cell.
static void placementClipChecks() {
    Rig rig; rig.song();
    std::vector<std::uint64_t> openedClips;
    rig.c.onOpenPlacement = [&](std::uint64_t clip) { openedClips.push_back(clip); };
    std::uint32_t second = 0;
    require(rig.controller.placeRegion(0, 0, 4 * bar, &second), "a second placement of clip a");
    rig.view.refresh();
    const auto scenes = rig.controller.document().sceneCount();
    require(rig.c.makePlacementUnique(second), "Make this placement unique");
    const auto clip = rig.reg(second).clip;
    require(clip != 0 && rig.reg(rig.id.a).playsCell() && rig.controller.document().sceneCount() == scenes,
            "it gets a clip of its own without taking a Session cell");
    rig.opened.clear();
    rig.doubleClick(rig.at(4 * bar + 100, 0));
    require(openedClips == std::vector<std::uint64_t>{clip} && rig.opened.empty(), "double-clicking it opens its own notes, not the cell");
    rig.rightClick(rig.at(4 * bar + 100, 0));
    require(rig.menu.shown, "its menu opens");
    rig.menu.done(TimelineCanvas::cmdOpen);
    require(openedClips.size() == 2 && openedClips[1] == clip, "Open in the menu does the same");
    // Cut and paste: the clip is dropped with its last region, and the paste brings it back with its notes.
    rig.c.setSelection({second});
    require(rig.c.cutSelection() && !rig.has(second)
            && rig.controller.document().library.clip(library::ClipId{clip}) == nullptr, "cutting the only region drops the clip");
    require(rig.c.paste(8 * bar) && rig.c.selection().size() == 1, "paste it back");
    const auto pasted = rig.reg(rig.c.selection()[0]);
    require(pasted.ownsClip() && pasted.start == 8 * bar && rig.controller.document().placementContent(pasted).notes().count == 3
            && rig.valid(), "the paste plays a clip with the copied notes");
    // A copy pasted while the clip is still there shares it.
    rig.c.setSelection({pasted.id});
    require(rig.c.copySelection() && rig.c.paste(12 * bar), "copy and paste again");
    int sharing = 0;
    for (const auto& r : rig.t().regions) sharing += r.ownsClip() && r.clip == pasted.clip ? 1 : 0;
    require(sharing == 2, "the two placements share one clip");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    freezeMenuChecks();
    placementClipChecks();
    sendAutomationEditorChecks();
    takeCompChecks();
    takeLaneChecks();
    loopPassLaneChecks();
    geometryChecks();
    snapChecks();
    placingChecks();
    newClipChecks();
    clipListChecks();
    dropChecks();
    selectionChecks();
    dragChecks();
    audioSlipDragChecks();
    fadeChecks();
    recordingSetupChecks();
    commandChecks();
    zoomAndScrollChecks();
    loopChecks();
    markerChecks();
    tempoChecks();
    meterChecks();
    playbackChecks();
    toolbarChecks();
    robustnessChecks();
    snapshots();
    if (failures != 0) { std::cerr << failures << " checks failed\n"; return 1; }
    std::cout << "All TimelineView tests passed\n";
    return 0;
}
