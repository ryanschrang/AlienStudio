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
    // A track with nothing to place says so and changes nothing.
    rig.messages.clear();
    const auto count = rig.t().regions.size();
    rig.doubleClick(rig.at(0, 3));
    require(rig.t().regions.size() == count && !rig.messages.empty() && rig.messages.back().contains("no clips"), "an empty track places nothing and explains why");
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
    require(rig.viewMenu.shown && rig.viewMenu.items().size() == 2, "it offers the whole song or the loop");
    require(rig.viewMenu.items()[0].enabled && rig.viewMenu.items()[0].text.contains("16 bars"), "the whole song, with its length");
    rig.viewMenu.done(1);
    c.setLoopRange(2 * bar, 4 * bar);
    rig.viewMenu.reset();
    clickButton(*exportButton);
    require(rig.viewMenu.items()[1].text.contains("bars 3 to 4"), "the loop entry names its bars");
    rig.viewMenu.done(2);
    require(rig.exports.size() == 2 && rig.exports[0] == std::make_pair<std::int64_t, std::int64_t>(0, -1) && rig.exports[1] == std::make_pair(2 * bar, 4 * bar), "the choices reach the exporter");
    // With nothing on the timeline the export entries are off.
    Rig empty;
    auto* emptyExport = childNamed<juce::TextButton>(empty.view, "Export WAV");
    clickButton(*emptyExport);
    require(empty.viewMenu.shown && !empty.viewMenu.items()[0].enabled && !empty.viewMenu.items()[1].enabled, "an empty timeline cannot be exported");
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
    require(c.place(0, 0, static_cast<std::int64_t>(tl::maxBars - 100) * bar) && rig.valid(), "a clip near the end of an 18-hour song");
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
static int pixelsOfColour(const juce::Image& image, juce::Rectangle<int> area, juce::Colour near, int tolerance) {
    int n = 0;
    for (int y = std::max(0, area.getY()); y < std::min(image.getHeight(), area.getBottom()); ++y)
        for (int x = std::max(0, area.getX()); x < std::min(image.getWidth(), area.getRight()); ++x) {
            const auto p = image.getPixelAt(x, y);
            n += std::abs(p.getRed() - near.getRed()) + std::abs(p.getGreen() - near.getGreen()) + std::abs(p.getBlue() - near.getBlue()) <= tolerance ? 1 : 0;
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

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    geometryChecks();
    snapChecks();
    placingChecks();
    selectionChecks();
    dragChecks();
    commandChecks();
    zoomAndScrollChecks();
    loopChecks();
    markerChecks();
    playbackChecks();
    toolbarChecks();
    robustnessChecks();
    snapshots();
    if (failures != 0) { std::cerr << failures << " checks failed\n"; return 1; }
    std::cout << "All TimelineView tests passed\n";
    return 0;
}
