// The Session (clip launcher) page driven through its real controls: the FL Studio-style layout (a row per
// track with its header at the left, a column per scene with its launch button on top, Stop all in the
// corner), launching, the per-track Stop, the header's light/knobs/solo that are the Mixer's own controls,
// scrolling, what the tiles and headers look like (checked pixel by pixel), tooltips, and screenshots.
// No audio device or window is needed.
#include "SessionView.h"
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

static const juce::ModifierKeys leftButton(juce::ModifierKeys::leftButtonModifier);
static const juce::ModifierKeys rightButton(juce::ModifierKeys::rightButtonModifier);
static const juce::ModifierKeys shiftLeft(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);

static juce::MouseEvent mouse(juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1, bool dragged = false) {
    const auto now = juce::Time::getCurrentTime();
    const auto pf = p.toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pf, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pf, now, clicks, dragged);
}

static void pump(int ms = 30) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// Button::triggerClick() posts a message; running the message loop delivers it.
static void clickButton(juce::Button& button) { button.triggerClick(); pump(); }

static PianoRollPattern oneNote() {
    PianoRollPattern p;
    p.notes[0] = PianoRollNote{1, 60, 0.0, 1.0, 0.8f, 1.0f};
    p.count = 1;
    return p;
}

struct Rig {
    ToneEngine engine;
    ProjectController controller{engine};
    SessionView view{controller, engine};
    std::vector<std::pair<int, bool>> mixEdits;   // what the view reported: track, committed
    std::vector<int> mixCommands; // refresh notifications for edits already committed by commands

    explicit Rig(int width = 1280, int height = 720) {
        engine.prepare(48000);
        controller.pushDocumentToEngine();
        view.onTrackMixEdited = [this](int track, bool committed) { mixEdits.emplace_back(track, committed); };
        view.onTrackMixCommandExecuted = [this](int track) { mixCommands.push_back(track); };
        view.setSize(width, height);
        view.rebuildGrid();
    }
    // Makes a clip with one note in a cell (the tile then shows as a clip, not an empty slot).
    void fillClip(int track, int scene) { controller.setClipPattern(track, scene, oneNote(), false); view.rebuildGrid(); }
    SessionTrackStrip& strip() { return view.tracks(); }
    void press(juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1) {
        c.mouseDown(mouse(c, p, mods, clicks)); c.mouseUp(mouse(c, p, mods, clicks));
    }
    void click(juce::Point<int> p, juce::ModifierKeys mods = leftButton, int clicks = 1) { press(strip(), p, mods, clicks); }
    // Press, drag through the points, release (all in the strip's own coordinates).
    void drag(juce::Point<int> from, std::initializer_list<juce::Point<int>> via, juce::ModifierKeys mods = leftButton) {
        strip().mouseDown(mouse(strip(), from, mods));
        juce::Point<int> last = from;
        for (const auto& p : via) { strip().mouseDrag(mouse(strip(), p, mods, 1, true)); last = p; }
        strip().mouseUp(mouse(strip(), last, mods));
    }
    juce::Image snapshot() { return view.createComponentSnapshot(view.getLocalBounds(), true, 1.0f); }
    int requested(int track) const { return engine.requestedPianoRollClip[static_cast<std::size_t>(track)].load(); }
    float gain(int track) const { return engine.tracks[static_cast<std::size_t>(track)].gain.load(); }
    float pan(int track) const { return engine.tracks[static_cast<std::size_t>(track)].pan.load(); }
    bool muted(int track) const { return engine.tracks[static_cast<std::size_t>(track)].mute.load(); }
    bool soloed(int track) const { return engine.tracks[static_cast<std::size_t>(track)].solo.load(); }
};

static juce::Point<int> centre(juce::Rectangle<int> r) { return r.getCentre(); }

// ---- layout ---------------------------------------------------------------------------------
static void layoutChecks() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig(1280, 720);
    auto& v = rig.view;
    auto& strip = rig.strip();
    const auto& d = rig.controller.document();
    require(d.trackCount() == 4 && d.sceneCount() == 4, "the starting session is four tracks by four scenes");

    // A row per track: the header and the clips of one track sit on one line, the header to the left of its clips.
    for (int t = 0; t < d.trackCount(); ++t) {
        const auto header = strip.rowBounds(t).translated(strip.getX(), strip.getY());
        require(header.getHeight() == SessionView::rowHeight && header.getWidth() == SessionView::headerWidth, "a track header is one row high and the header width wide");
        for (int s = 0; s < d.sceneCount(); ++s) {
            const auto clip = v.clipArea(t, s);
            require(clip.getY() == header.getY() && clip.getHeight() == header.getHeight(), "a track's clips line up with its header");
            require(clip.getX() >= header.getRight(), "the clips start to the right of the track headers");
        }
    }
    // Rows follow each other from the top, one gap apart.
    require(strip.rowBounds(1).getY() - strip.rowBounds(0).getBottom() == SessionView::gap, "rows are one gap apart");

    // A column per scene: its launch button is on top of its clips and as wide as they are.
    for (int s = 0; s < d.sceneCount(); ++s) {
        const auto scene = v.sceneArea(s);
        const auto clip = v.clipArea(0, s);
        require(scene.getX() == clip.getX() && scene.getWidth() == clip.getWidth(), "a scene's launch button is as wide as its column and lines up with it");
        require(scene.getBottom() <= clip.getY() && scene.getHeight() == SessionView::sceneBarHeight, "the scene buttons are a row above the clips");
        if (s > 0) require(scene.getX() - v.sceneArea(s - 1).getRight() == SessionView::gap, "columns are one gap apart");
    }
    // Stop all sits in the corner, above the headers and left of the scene buttons.
    const auto stopAll = v.stopAllArea();
    require(stopAll.getX() == strip.getX() && stopAll.getWidth() == SessionView::headerWidth && stopAll.getBottom() <= strip.getY()
            && stopAll.getRight() <= v.sceneArea(0).getX() && stopAll.getY() == v.sceneArea(0).getY(), "Stop all is in the corner");

    // Everything that is drawn is where a click would land on it (not clipped away by the row or column it sits in).
    v.setVisible(true);   // (hit testing skips a component that is not visible)
    for (int s = 0; s < d.sceneCount(); ++s) {
        require(v.getComponentAt(centre(v.sceneArea(s))) == v.sceneLauncher(s), "every scene button can be clicked where it is drawn");
        for (int t = 0; t < d.trackCount(); ++t)
            require(v.getComponentAt(centre(v.clipArea(t, s))) == v.clipSlot(t, s), "every clip can be clicked where it is drawn");
    }

    // The columns are sized knowing whether the rows scroll, so a vertical bar never forces a horizontal one.
    {
        Rig squeezed(900, 700);
        int withBar = 0;
        for (int h = 300; h <= 700; ++h) {
            squeezed.view.setSize(900, h);
            auto& viewport = squeezed.view.clipViewport();
            if (viewport.getVerticalScrollBar().isVisible()) {
                ++withBar;
                require(!viewport.getHorizontalScrollBar().isVisible(), "four scenes fit beside the vertical scroll bar at every window height");
            }
        }
        require(withBar > 20, "the check ran over window heights that need the vertical bar");
    }

    // Columns take the window's width up to a limit, then stop growing.
    require(v.sceneWidth() == SessionView::maxSceneWidth, "four scenes in a wide window fill it up to the column limit");
    Rig wide(2600, 900);
    require(wide.view.sceneWidth() == SessionView::maxSceneWidth && wide.view.clipArea(0, 3).getRight() < 2600 - 32 - 100, "columns stop growing in a very wide window");
    Rig mid(1000, 700);
    require(mid.view.sceneWidth() < SessionView::maxSceneWidth && mid.view.sceneWidth() >= SessionView::minSceneWidth
            && mid.view.clipArea(0, 3).getRight() <= 1000 - 16, "columns shrink to fit a narrower window");

    // Many scenes scroll sideways; the scene buttons and the clips move together and the headers stay put.
    Rig many(1000, 700);
    for (int i = 0; i < 12; ++i) require(many.controller.addScene(), "scenes can be added");
    many.view.rebuildGrid();
    require(many.view.sceneWidth() == SessionView::minSceneWidth && many.view.clipViewport().getHorizontalScrollBar().isVisible(), "sixteen scenes scroll sideways at the minimum column width");
    const auto sceneBefore = many.view.sceneArea(5), clipBefore = many.view.clipArea(2, 5), headerBefore = many.strip().rowBounds(2);
    many.view.clipViewport().setViewPosition(150, 0);
    require(many.view.sceneArea(5).getX() == sceneBefore.getX() - 150 && many.view.clipArea(2, 5).getX() == clipBefore.getX() - 150, "scrolling sideways moves the scene buttons with the clips");
    require(many.strip().rowBounds(2) == headerBefore, "and leaves the track headers where they are");

    // Many tracks scroll up and down; the headers move with their rows and the scene row stays put.
    Rig tall(1280, 600);
    for (int i = 0; i < 12; ++i) require(tall.controller.addTrack(), "tracks can be added");
    tall.view.rebuildGrid();
    require(tall.controller.document().trackCount() == 16 && tall.view.clipViewport().getVerticalScrollBar().isVisible(), "sixteen tracks scroll vertically in a short window");
    const auto sceneRow = tall.view.sceneArea(0);
    tall.view.clipViewport().setViewPosition(0, 120);
    for (int t : {0, 5, 11}) {
        const auto header = tall.strip().rowBounds(t).translated(tall.strip().getX(), tall.strip().getY());
        require(header.getY() == tall.view.clipArea(t, 0).getY(), "after scrolling down a track's header is still level with its clips");
    }
    require(tall.strip().rowBounds(0).getY() == -120 && tall.view.sceneArea(0) == sceneRow, "scrolling down moves the headers and leaves the scene row");
    require(tall.strip().trackAt(-120 + SessionView::rowHeight / 2 + 2 * (SessionView::rowHeight + SessionView::gap)) == 2, "a click lands on the row that is drawn there");
    require(tall.strip().trackAt(-120 + SessionView::rowHeight + 2) == -1, "and the gap between rows is no row");
    // The mouse wheel over the headers scrolls the clips.
    tall.view.clipViewport().setViewPosition(0, 0);
    tall.view.mouseWheelMove(mouse(tall.view, {20, 300}), juce::MouseWheelDetails{0.0f, -0.5f, false, false, false});
    require(tall.view.clipViewport().getViewPositionY() > 0, "the wheel over the session scrolls the rows");
}

// ---- the mouse wheel -------------------------------------------------------------------------
// A wheel turn over any part of the page scrolls the rows when they can scroll. When they cannot, it must go on to the
// parent once and stop there (it used to be handed back and forth between the page and its viewport without end).
struct WheelCatcher : juce::Component {
    int wheels = 0;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override { ++wheels; }
};
static void wheelEvents() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const juce::MouseWheelDetails down{0.0f, -0.5f, false, false, false};
    {
        Rig rig;
        WheelCatcher parent;
        parent.setSize(1280, 720);
        parent.addAndMakeVisible(rig.view);
        auto& v = rig.view;
        require(!v.clipViewport().canScrollVertically() && !v.clipViewport().canScrollHorizontally(), "a four by four session fits: nothing scrolls");
        auto* clip = v.clipSlot(1, 1);
        auto* scene = v.sceneLauncher(1);
        rig.strip().mouseWheelMove(mouse(rig.strip(), centre(rig.strip().nameRect(0))), down);
        require(parent.wheels == 1, "a wheel turn over a track name that cannot scroll anything goes on to the parent, once");
        clip->mouseWheelMove(mouse(*clip, {20, 10}), down);
        require(parent.wheels == 2, "and so does one over a clip");
        scene->mouseWheelMove(mouse(*scene, {20, 10}), down);
        require(parent.wheels == 3, "and one over a scene button");
        v.mouseWheelMove(mouse(v, {5, 5}), down);
        require(parent.wheels == 4, "and one over the page itself");
        parent.removeChildComponent(&v);
    }
    {
        Rig tall(1280, 560);
        for (int i = 0; i < 10; ++i) tall.controller.addTrack();
        tall.view.rebuildGrid();
        WheelCatcher parent;
        parent.setSize(1280, 560);
        parent.addAndMakeVisible(tall.view);
        auto& viewport = tall.view.clipViewport();
        require(viewport.canScrollVertically(), "fourteen tracks need scrolling in a short window");
        int last = viewport.getViewPositionY();
        auto scrolled = [&] { const bool moved = viewport.getViewPositionY() > last; last = viewport.getViewPositionY(); return moved; };
        tall.strip().mouseWheelMove(mouse(tall.strip(), centre(tall.strip().nameRect(0))), down);
        require(scrolled(), "the wheel over a track name scrolls the rows");
        auto* clip = tall.view.clipSlot(0, 0);
        clip->mouseWheelMove(mouse(*clip, {20, 10}), down);
        require(scrolled(), "the wheel over a clip scrolls the rows");
        auto* scene = tall.view.sceneLauncher(0);
        scene->mouseWheelMove(mouse(*scene, {20, 10}), down);
        require(scrolled(), "the wheel over a scene button scrolls the rows");
        require(parent.wheels == 0, "and the parent hears nothing of a wheel turn the page used");
        parent.removeChildComponent(&tall.view);
    }
}

// ---- launching and stopping ----------------------------------------------------------------
static void launching() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& v = rig.view;
    // A clip's launch button launches that clip on its track and selects it.
    clickButton(*v.clipSlot(2, 3));
    require(rig.requested(2) == 3 && rig.controller.document().selectedTrack == 2 && rig.controller.document().selectedScene == 3, "clicking a clip launches it and selects it");
    require(rig.requested(0) == -1 && rig.requested(1) == -1 && rig.requested(3) == -1, "and only on its own track");
    // A scene's launch button launches every track's clip in that scene.
    clickButton(*v.sceneLauncher(1));
    for (int t = 0; t < 4; ++t) require(rig.requested(t) == 1, "a scene button launches the whole scene");
    // The stop button of one track stops that track alone.
    int stopped = -1;
    rig.strip().onStop = [&](int track) { stopped = track; };
    rig.click(centre(rig.strip().stopRect(2)));
    require(rig.requested(2) == -1 && rig.requested(0) == 1 && rig.requested(1) == 1 && rig.requested(3) == 1, "a track's stop button stops that track and no other");
    require(stopped == 2, "and the owner is told");
    // Stop all stops every track.
    clickButton(*v.sceneLauncher(0));
    for (int t = 0; t < 4; ++t) require(rig.requested(t) == 0, "the scene launched again");
    // (the corner button is a child of the view; find it by what it is called)
    juce::Button* stopAll = nullptr;
    for (auto* child : v.getChildren())
        if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getName().startsWith("Stop all")) stopAll = b;
    require(stopAll != nullptr && stopAll->getBounds() == v.stopAllArea(), "Stop all is the corner button");
    clickButton(*stopAll);
    for (int t = 0; t < 4; ++t) require(rig.requested(t) == -1, "Stop all stops every track");
    // The controller refuses a track that does not exist.
    rig.controller.stopTrack(-1); rig.controller.stopTrack(99);
    clickButton(*v.sceneLauncher(2));
    rig.controller.stopTrack(99);
    require(rig.requested(0) == 2, "stopping a track that does not exist does nothing");
    // A right-click on a clip edits it instead of launching.
    rig.controller.stopAllTracks();
    int edited = -1;
    v.onEditClip = [&](int track, int scene) { edited = track * 10 + scene; };
    v.clipSlot(1, 2)->mouseUp(mouse(*v.clipSlot(1, 2), {10, 10}, rightButton));
    require(edited == 12 && rig.requested(1) == -1, "right-clicking a clip opens it for editing and does not launch it");
}

// Clicking a clip with the transport stopped makes it heard: the transport starts and the clip plays from the
// first boundary. With the Song playing instead, or the 16-step sequencer, the transport is left alone.
static void launchingIsHeard() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& e = rig.engine;
    rig.fillClip(0, 0);
    e.patternMode.store(1);
    std::vector<bool> reports;
    rig.view.onLaunched = [&](bool started) { reports.push_back(started); };
    require(!e.playing.load(), "the transport starts stopped");
    clickButton(*rig.view.clipSlot(0, 0));
    require(e.playing.load() && reports == std::vector<bool>{true}, "clicking a clip starts the stopped transport");
    std::array<float, 512> left{}, right{};
    float peak = 0;
    for (int b = 0; b < 8; ++b) {   // the first 85 ms
        float* outputs[]{left.data(), right.data()};
        e.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        for (const float v : left) peak = std::max(peak, std::abs(v));
    }
    require(peak > 0.01f, "and the clip is heard straight away");
    clickButton(*rig.view.sceneLauncher(0));
    require(e.playing.load() && reports.back() == false, "launching while playing leaves the transport running");
    e.playing.store(false);
    e.patternMode.store(2);   // Song mode: the Session view plays the Song
    clickButton(*rig.view.clipSlot(0, 0));
    require(!e.playing.load() && reports.back() == false && rig.requested(0) == 0, "with the Song playing the clip is queued but the transport stays as it was");
    e.patternMode.store(0);   // the 16-step sequencer
    clickButton(*rig.view.sceneLauncher(0));
    require(!e.playing.load(), "and so it does for the 16-step sequencer");
}

// The real device callback rendered without a device: two tracks loop a clip, one is stopped from its header, and at the
// next bar that track is stopped while the other keeps playing.
static void renderBlocks(ToneEngine& engine, int samples) {
    std::array<float, 256> left{}, right{};
    while (samples > 0) {
        const int count = std::min(samples, static_cast<int>(left.size()));
        float* outputs[]{left.data(), right.data()};
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
        samples -= count;
    }
}
static void engineStopsOneTrack() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& e = rig.engine;
    auto active = [&](int track) { return e.activePianoRollClip[static_cast<std::size_t>(track)].load(); };
    rig.fillClip(0, 0); rig.fillClip(1, 0);
    e.patternMode.store(1);
    e.playing.store(true);
    clickButton(*rig.view.clipSlot(0, 0));
    clickButton(*rig.view.clipSlot(1, 0));
    renderBlocks(e, 512);
    require(active(0) == 0 && active(1) == 0, "both tracks are playing their clip");
    rig.click(centre(rig.strip().stopRect(0)));
    renderBlocks(e, 90000);   // a bar is 96000 samples at 120 BPM
    require(active(0) == 0 && active(1) == 0, "the stop waits for the launch boundary");
    renderBlocks(e, 8000);
    require(active(0) == -1, "at the bar the track whose stop button was pressed has stopped");
    require(active(1) == 0, "and the other track is still playing");
    renderBlocks(e, 96000 * 2);
    require(active(0) == -1 && active(1) == 0, "and stays that way");
    rig.view.refreshClips();
    const auto image = rig.snapshot();
    const auto at = [&](juce::Point<int> p) { return image.getPixelAt(rig.strip().getX() + p.x, rig.strip().getY() + p.y); };
    require(at(centre(rig.strip().stopRect(1))).getGreen() > 180 && at(centre(rig.strip().stopRect(0))).getBrightness() < 0.5f, "the headers show which track plays and which stopped");
}

// ---- the track header's controls ----------------------------------------------------------
static void headerControls() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& strip = rig.strip();
    int selected = -1, renamed = -1, menu = -1;
    strip.onSelect = [&](int t) { selected = t; };
    strip.onRename = [&](int t) { renamed = t; };
    strip.onMenu = [&](int t) { menu = t; };

    // The parts do not overlap and fit in the header.
    for (int t = 0; t < 4; ++t) {
        const std::vector<juce::Rectangle<int>> parts{strip.ledRect(t), strip.panRect(t), strip.volumeRect(t), strip.nameRect(t), strip.soloRect(t), strip.stopRect(t)};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            require(strip.rowBounds(t).contains(parts[i]), "every part of a header is inside its row");
            for (std::size_t j = i + 1; j < parts.size(); ++j) require(!parts[i].intersects(parts[j]), "header parts do not overlap");
        }
    }

    // The light mutes; a right-click on it soloes; the S button soloes. Commands report refresh-only notifications.
    rig.click(centre(strip.ledRect(1)));
    require(rig.muted(1) && !rig.muted(0), "the light mutes its track");
    require(rig.mixCommands == std::vector<int>{1} && rig.mixEdits.empty(), "mute reports refresh without a legacy edit commit");
    rig.click(centre(strip.ledRect(1)));
    require(!rig.muted(1), "clicking the light again unmutes");
    rig.click(centre(strip.ledRect(2)), rightButton);
    require(rig.soloed(2), "right-clicking the light soloes");
    require(rig.mixCommands.back() == 2 && rig.mixEdits.empty(), "solo reports refresh without a legacy edit commit");
    rig.mixEdits.clear();
    rig.mixCommands.clear();
    rig.click(centre(strip.soloRect(2)));
    require(!rig.soloed(2), "the S button toggles solo");
    require(rig.mixCommands == std::vector<int>{2} && rig.mixEdits.empty(), "the S button also uses the command notification");
    rig.click(centre(strip.soloRect(3)));
    require(rig.soloed(3) && !rig.soloed(2), "and soloes the track it is on");
    rig.click(centre(strip.soloRect(3)));
    rig.mixEdits.clear();

    // Dragging the volume knob up raises the track's gain, a drag down lowers it; the final value is reported once.
    const auto knob = centre(strip.volumeRect(0));
    require(std::abs(rig.gain(0) - 1.0f) < 1e-6f, "tracks start at unity gain");
    rig.drag(knob, {knob + juce::Point<int>(0, -10), knob + juce::Point<int>(0, -25)});
    require(rig.gain(0) > 1.25f && rig.gain(0) < 1.35f, "dragging the volume knob up raises the gain");
    int live = 0, finals = 0;
    for (const auto& edit : rig.mixEdits) { require(edit.first == 0, "only the dragged track is reported"); (edit.second ? finals : live)++; }
    require(live >= 2 && finals == 1 && rig.mixEdits.back().second, "a drag reports its live values and then one final value");
    rig.drag(knob, {knob + juce::Point<int>(0, 37)});
    require(rig.gain(0) > 0.84f && rig.gain(0) < 0.88f, "dragging down lowers it");
    const float stored = rig.gain(0);
    require(std::abs(stored * 100.0f - std::round(stored * 100.0f)) < 1e-3f, "volume is stored in whole percent, as the Mixer does");
    // Limits.
    rig.drag(knob, {knob + juce::Point<int>(0, -400)});
    require(rig.gain(0) == 2.0f, "volume stops at +6 dB (200%)");
    rig.drag(knob, {knob + juce::Point<int>(0, 400)});
    require(rig.gain(0) == 0.0f, "and at silence");
    // Near unity it snaps there; Shift drags finely; double-click resets.
    rig.engine.tracks[0].gain.store(1.0f);
    rig.drag(knob, {knob + juce::Point<int>(0, -1)});
    require(rig.gain(0) == 1.0f, "a nudge near unity snaps to unity");
    rig.drag(knob, {knob + juce::Point<int>(0, -10)}, shiftLeft);
    require(rig.gain(0) > 1.02f && rig.gain(0) < 1.05f, "Shift drags in fine steps");
    rig.mixEdits.clear();
    rig.click(knob, leftButton, 2);
    require(rig.gain(0) == 1.0f, "double-clicking the volume knob resets it to unity");
    require(!rig.mixEdits.empty() && rig.mixEdits.back() == std::pair<int, bool>{0, true}, "and reports it as a final value");

    // Pan: left of centre is negative, snaps to the centre, double-click centres it.
    const auto panKnob = centre(strip.panRect(0));
    rig.drag(panKnob, {panKnob + juce::Point<int>(0, 33)});
    require(rig.pan(0) < -0.38f && rig.pan(0) > -0.42f, "dragging the pan knob down pans left");
    require(std::abs(rig.pan(0) * 100.0f - std::round(rig.pan(0) * 100.0f)) < 1e-3f, "and pan is stored in whole percent too");
    rig.drag(panKnob, {panKnob + juce::Point<int>(0, -300)});
    require(rig.pan(0) == 1.0f, "and up pans right, up to the limit");
    rig.engine.tracks[0].pan.store(0.0f);
    rig.drag(panKnob, {panKnob + juce::Point<int>(0, -2)});
    require(rig.pan(0) == 0.0f, "a nudge near the centre snaps to the centre");
    rig.engine.tracks[0].pan.store(0.6f);
    rig.click(panKnob, leftButton, 2);
    require(rig.pan(0) == 0.0f, "double-clicking the pan knob centres it");
    // The mouse wheel over a knob turns it.
    const float before = rig.gain(1);
    strip.mouseWheelMove(mouse(strip, centre(strip.volumeRect(1))), juce::MouseWheelDetails{0.0f, 0.2f, false, false, false});
    require(rig.gain(1) > before, "the wheel over the volume knob raises it");
    // A knob drag does not change which track is selected or what it plays.
    require(selected == -1 && rig.requested(0) == -1, "using a control does not select the track or launch anything");

    // The name button selects the track; a double-click renames; a right-click opens the menu. Knobs and buttons do not.
    rig.click(centre(strip.nameRect(2)));
    require(selected == 2 && renamed == -1 && menu == -1, "clicking the name selects the track");
    rig.click(centre(strip.nameRect(3)), leftButton, 2);
    require(renamed == 3, "double-clicking the name renames it");
    rig.click(centre(strip.nameRect(1)), rightButton);
    require(menu == 1, "right-clicking the name opens the track menu");
    renamed = -1; menu = -1;
    rig.click(centre(strip.panRect(0)), leftButton, 2);
    rig.click(centre(strip.ledRect(0)), leftButton, 2);
    rig.click(centre(strip.ledRect(0)), leftButton, 1);   // restore the light
    require(renamed == -1, "double-clicking a knob or the light does not rename");
    rig.click(centre(strip.volumeRect(0)), rightButton);
    require(menu == 0, "right-clicking a knob opens the track menu");
    rig.click({SessionTrackStrip::width - 2, strip.rowBounds(0).getCentreY()}, rightButton);
    // Between rows nothing happens.
    selected = -1;
    rig.click({40, strip.rowBounds(0).getBottom() + 2});
    require(selected == -1, "the gap between rows is not a header");
}

// ---- selection, rename, menu and a changing number of tracks -------------------------------
static void selectionAndStructure() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& strip = rig.strip();
    int changed = 0;
    rig.view.onSelectionChanged = [&] { ++changed; };
    // Clicking a track's name selects the track and keeps the selected scene (the view's own wiring).
    rig.controller.selectClip(0, 2);
    rig.click(centre(strip.nameRect(3)));
    require(rig.controller.document().selectedTrack == 3 && rig.controller.document().selectedScene == 2 && changed == 1, "clicking a track's name selects it and keeps the scene");
    rig.click(centre(strip.panRect(1)));
    rig.click(centre(strip.volumeRect(1)));
    rig.click(centre(strip.soloRect(1)));
    rig.click(centre(strip.stopRect(1)));
    rig.click(centre(strip.ledRect(1)));
    require(rig.controller.document().selectedTrack == 3 && changed == 1, "using a track's controls does not select it");
    // A double-click on the name opens the rename box; a right-click opens the track menu.
    rig.click(centre(strip.nameRect(1)), leftButton, 2);
    pump();
    require(juce::ModalComponentManager::getInstance()->getNumModalComponents() == 1, "double-clicking a track's name opens the rename box");
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
    pump();
    require(juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0 && rig.controller.document().tracks[1].name == "Track 2", "and cancelling it leaves the name alone");
    int menuRequests = 0;
    std::function<void(int)> menuResult;
    rig.view.trackMenuLauncher = [&](juce::PopupMenu menu, std::function<void(int)> done) {
        ++menuRequests;
        bool rename = false, add = false, remove = false;
        juce::PopupMenu::MenuItemIterator items(menu, true);
        while (items.next()) {
            const auto& item = items.getItem();
            rename = rename || (item.itemID == 1 && item.text == "Rename..." && item.isEnabled);
            add = add || (item.itemID == 4 && item.text == "Add track" && item.isEnabled);
            remove = remove || (item.itemID == 5 && item.text == "Delete track" && item.isEnabled);
        }
        require(rename && add && remove, "the track context menu contains its enabled editing actions");
        menuResult = std::move(done);
    };
    rig.click(centre(strip.nameRect(1)), rightButton);
    require(menuRequests == 1 && static_cast<bool>(menuResult), "right-clicking a track's name requests a menu");
    menuResult(0);
    require(rig.controller.document().trackCount() == 4 && rig.controller.document().tracks[1].name == "Track 2",
        "cancelling the track menu leaves the document unchanged");
    // Command-backed menus own their commit, even when the host's legacy edit callback commits.
    int legacyEdits = 0, structureEdits = 0;
    rig.view.onEdited = [&](const juce::String& label) { ++legacyEdits; rig.controller.commit(label); };
    rig.view.onStructureChanged = [&] { ++structureEdits; };
    rig.controller.commit("Baseline");
    const auto baseline = rig.controller.document();
    menuResult(4);
    require(rig.controller.document().trackCount() == 5 && rig.controller.undoLabel() == "Add track",
        "the menu adds a track through a named command");
    require(structureEdits == 1 && legacyEdits == 0, "command menus refresh the host without a second commit");
    require(rig.controller.undo() && sameContent(rig.controller.document(), baseline), "one undo restores the menu edit");
    require(rig.controller.redo() && rig.controller.document().trackCount() == 5, "the menu edit can be redone");
    require(rig.controller.undo(), "restore baseline for move");
    rig.view.rebuildGrid();
    rig.click(centre(strip.nameRect(1)), rightButton);
    menuResult(2);
    require(rig.controller.document().tracks[0].name == "Track 2" && rig.controller.undoLabel() == "Move track",
        "the menu moves the selected track through a command");
    require(rig.controller.undo() && sameContent(rig.controller.document(), baseline), "one undo restores track order");
    rig.view.rebuildGrid();
    rig.click(centre(strip.nameRect(1)), rightButton);
    const int beforeBusy = structureEdits;
    rig.controller.setExternalBusy(true);
    menuResult(5);
    rig.controller.setExternalBusy(false);
    require(sameContent(rig.controller.document(), baseline) && structureEdits == beforeBusy && rig.controller.canRedo(),
        "a stale menu refuses edits while busy and preserves redo and UI notifications");
    menuResult(5);
    require(rig.controller.document().trackCount() == 3 && rig.controller.undoLabel() == "Delete track",
        "the menu deletes through a named command");
    require(rig.controller.undo() && sameContent(rig.controller.document(), baseline), "one undo restores the deleted track");
    rig.view.rebuildGrid();
    rig.click(centre(strip.nameRect(1)), leftButton, 2);
    auto* renameBox = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    require(renameBox != nullptr, "the track rename command uses the existing name dialog");
    renameBox->getTextEditor("name")->setText("Command bass");
    renameBox->exitModalState(1);
    pump();
    require(rig.controller.document().tracks[1].name == "Command bass" && rig.controller.undoLabel() == "Rename track",
        "accepting the dialog executes the named rename command");
    require(rig.controller.undo() && sameContent(rig.controller.document(), baseline), "one undo restores the old name");
    juce::Button* addButton = nullptr;
    for (auto* child : rig.view.getChildren())
        if (child->getName() == "Add track") addButton = dynamic_cast<juce::Button*>(child);
    require(addButton != nullptr, "the add-track toolbar button exists");
    clickButton(*addButton);
    require(rig.controller.document().trackCount() == 5 && rig.controller.undoLabel() == "Add track",
        "the toolbar also creates a track through a command");
    require(rig.controller.undo() && sameContent(rig.controller.document(), baseline) && legacyEdits == 0,
        "toolbar and dialog commands each commit once without legacy notifications");
    rig.view.onEdited = {};
    rig.view.onStructureChanged = {};
    rig.view.trackMenuLauncher = {};
    rig.view.rebuildGrid();

    // The rows follow the tracks: adding one adds a row, deleting one takes a row away.
    require(strip.trackAt(strip.rowBounds(4).getCentreY()) == -1, "there is no fifth row yet");
    require(rig.controller.addTrack(), "a track can be added");
    rig.view.rebuildGrid();
    require(strip.trackAt(strip.rowBounds(4).getCentreY()) == 4 && rig.view.clipSlot(4, 3) != nullptr, "a new track gets a row of clips and a header");
    require(rig.view.clipArea(4, 0).getY() == strip.rowBounds(4).translated(strip.getX(), strip.getY()).getY(), "lined up with each other");
    require(rig.controller.deleteTrack(0), "a track can be deleted");
    rig.view.rebuildGrid();
    require(strip.trackAt(strip.rowBounds(4).getCentreY()) == -1 && rig.view.clipSlot(4, 0) == nullptr, "and its row goes");
    // The header shows each track's own name, in the order they are in.
    require(rig.controller.moveTrack(0, 1), "a track can be moved");
    rig.view.rebuildGrid();
    rig.view.refreshFromDocument();
    const auto image = rig.snapshot();
    require(rig.controller.document().tracks[0].name == "Track 3" && image.getWidth() > 0, "tracks keep their order after a move");
}

static void muteSoloCommandHistory() {
    Rig rig;
    auto& c = rig.controller;
    auto& strip = rig.strip();
    c.selectClip(3, 2);
    require(c.setTrackVolume(0, .5) && c.setTrackPan(0, -.25) && c.setTrackSolo(2, true), "prepare mix settings");
    c.markAsNewBaseline();
    const auto baseline = c.document();
    rig.engine.requestedPianoRollClip[0].store(1);
    int legacyCommits = 0;
    rig.view.onTrackMixEdited = [&](int track, bool committed) {
        ++legacyCommits;
        c.trackSettingsEdited(track);
        if (committed) c.commit("Legacy mix");
    };
    rig.click(centre(strip.ledRect(0)));
    require(c.document().tracks[0].settings.mute && rig.muted(0) && c.undoLabel() == "Set track mute"
        && c.isDirty() && rig.mixCommands == std::vector<int>{0} && legacyCommits == 0,
        "mute click uses the named command, commits once and refreshes without the legacy callback");
    require(c.undo() && !c.canUndo() && !c.isDirty() && sameContent(c.document(), baseline) && !rig.muted(0),
        "one undo restores document, live mute and clean baseline");
    require(c.redo() && rig.muted(0) && c.document().tracks[0].settings.mute, "redo restores mute");
    rig.click(centre(strip.ledRect(0)));
    require(!rig.muted(0) && !c.document().tracks[0].settings.mute && c.undoLabel() == "Set track mute", "second mute click unmutes through a command");
    require(c.undo() && rig.muted(0), "undo unmute restores mute");
    require(c.undo() && sameContent(c.document(), baseline), "restore baseline and redo for refusal checks");
    const auto redoLabel = c.redoLabel();
    const auto notifications = rig.mixCommands.size();
    c.setExternalBusy(true);
    rig.click(centre(strip.ledRect(0)));
    rig.click(centre(strip.ledRect(1)), rightButton);
    rig.click(centre(strip.soloRect(1)));
    c.setExternalBusy(false);
    require(sameContent(c.document(), baseline) && !rig.muted(0) && !rig.soloed(1) && rig.soloed(2)
        && c.canRedo() && c.redoLabel() == redoLabel && rig.mixCommands.size() == notifications && legacyCommits == 0,
        "all three busy controls refuse without live writes, notifications or lost redo");
    require(c.redo() && rig.muted(0) && c.undo(), "redo still works after busy refusal");
    rig.click(centre(strip.ledRect(1)), rightButton);
    require(rig.soloed(1) && rig.soloed(2) && c.document().tracks[1].settings.solo && c.undoLabel() == "Set track solo",
        "right-click light solo is a named additive command");
    require(c.undo() && !rig.soloed(1) && rig.soloed(2) && sameContent(c.document(), baseline), "one undo restores the previous solo set");
    require(c.redo() && rig.soloed(1) && rig.soloed(2), "redo restores additive solo");
    rig.click(centre(strip.soloRect(1)));
    require(!rig.soloed(1) && rig.soloed(2) && !c.document().tracks[1].settings.solo && c.undoLabel() == "Set track solo",
        "the S button unsolos through the same command without clearing another solo");
    require(c.undo() && rig.soloed(1) && c.redo() && !rig.soloed(1), "S button command undo and redo restore live solo");
    require(c.document().selectedTrack == 3 && c.document().selectedScene == 2 && rig.requested(0) == 1
        && rig.gain(0) == .5f && rig.pan(0) == -.25f && legacyCommits == 0,
        "mute/solo preserve selection, queued launch and unrelated mix settings");
}

// The light is small, so its click zone reaches a few pixels past the drawn circle; further out is something else.
static void lightHitArea() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& strip = rig.strip();
    const auto led = strip.ledRect(1);
    rig.click({led.getRight() + 2, led.getCentreY()});
    require(rig.muted(1), "a click just right of the light mutes");
    rig.click({led.getX() - 2, led.getCentreY()});
    require(!rig.muted(1), "and one just left of it unmutes");
    rig.click({led.getCentreX(), led.getBottom() + 2});
    require(rig.muted(1), "and one just below it mutes");
    rig.click({led.getCentreX(), led.getY() - 2});
    require(!rig.muted(1), "and one just above it unmutes");
    rig.click({led.getRight() + 6, led.getCentreY()});
    require(!rig.muted(1) && rig.controller.document().selectedTrack == 1, "six pixels away it is the row, not the light: the track is selected and nothing is muted");
}

// While a knob is dragged a bubble says what its value is.
static void knobReadouts() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& strip = rig.strip();
    require(strip.knobReadout().isEmpty(), "no bubble when no knob is being dragged");
    const auto pan = centre(strip.panRect(0));
    strip.mouseDown(mouse(strip, pan));
    strip.mouseDrag(mouse(strip, {pan.x, pan.y - 25}, leftButton, 1, true));
    require(strip.knobReadout() == "Pan R 30", "dragging the pan knob up 25 px reads 'Pan R 30'");
    strip.mouseDrag(mouse(strip, {pan.x, pan.y + 25}, leftButton, 1, true));
    require(strip.knobReadout() == "Pan L 30", "and down 25 px reads 'Pan L 30'");
    strip.mouseDrag(mouse(strip, pan, leftButton, 1, true));
    require(strip.knobReadout() == "Pan centre", "and back where it started reads 'Pan centre'");
    strip.mouseUp(mouse(strip, pan));
    require(strip.knobReadout().isEmpty(), "the bubble goes when the knob is let go");
    const auto volume = centre(strip.volumeRect(0));
    strip.mouseDown(mouse(strip, volume));
    strip.mouseDrag(mouse(strip, {volume.x, volume.y - 25}, leftButton, 1, true));
    require(strip.knobReadout() == "Volume " + mixer_text::gainToText(130.0) && strip.knobReadout().endsWith("dB"), "dragging the volume knob up 25 px reads its level in dB");
    strip.mouseUp(mouse(strip, volume));
}

// ---- tooltips ---------------------------------------------------------------------------------
static void tooltips() {
    Rig rig;
    auto& strip = rig.strip();
    auto tip = [&](juce::Point<int> p) { strip.mouseMove(mouse(strip, p)); return strip.getTooltip(); };
    require(tip(centre(strip.ledRect(0))).containsIgnoreCase("mute") && tip(centre(strip.ledRect(0))).containsIgnoreCase("solo"), "the light's tooltip says it mutes and soloes");
    require(tip({strip.ledRect(0).getRight() + 2, strip.ledRect(0).getCentreY()}).containsIgnoreCase("mute"), "and so does the margin around it that a click counts for");
    require(tip(centre(strip.soloRect(0))).containsIgnoreCase("solo"), "the S button says it soloes");
    require(tip(centre(strip.stopRect(0))).containsIgnoreCase("stop"), "the stop button says it stops the track");
    require(tip(centre(strip.panRect(0))).containsIgnoreCase("pan") && tip(centre(strip.volumeRect(0))).containsIgnoreCase("volume"), "the knobs say what they are");
    const auto name = tip(centre(strip.nameRect(1)));
    require(name.contains(rig.controller.document().tracks[1].name) && name.containsIgnoreCase("double-click to rename"), "the name button's tooltip names the track and what a click does");
    strip.mouseExit(mouse(strip, {0, 0}));
    require(strip.getTooltip().isEmpty(), "no tooltip when the mouse is away");
    require(tip({40, strip.rowBounds(0).getBottom() + 2}).isEmpty(), "none between rows either");
}

// ---- the header follows the Mixer's values ---------------------------------------------------
static void followsMixer() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    auto& strip = rig.strip();
    // The Mixer writes the same atomics; the header shows them without being told (it repaints from them).
    rig.engine.tracks[1].mute.store(true);
    rig.engine.tracks[2].gain.store(0.0f);
    rig.engine.tracks[3].pan.store(-1.0f);
    strip.refresh();
    const auto image = rig.snapshot();
    const auto origin = strip.getPosition();
    auto at = [&](juce::Point<int> p) { return image.getPixelAt(origin.x + p.x, origin.y + p.y); };
    require(at(centre(strip.ledRect(0))).getBrightness() > 0.6f && at(centre(strip.ledRect(1))).getBrightness() < 0.35f, "a track muted in the Mixer shows a dark light here");
}

// ---- what it looks like ------------------------------------------------------------------------
// A session with something in it: clips, a muted track, a soloed one, knobs off centre, two clips playing.
static void dress(Rig& rig);

static void headerLook() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    {
        Rig rig;
        dress(rig);
        auto& strip = rig.strip();
        const auto image = rig.snapshot();
        const auto origin = strip.getPosition();
        auto at = [&](juce::Point<int> p) { return image.getPixelAt(origin.x + p.x, origin.y + p.y); };
        const auto trackColour = [&](int t) { return ui::trackColour(rig.controller.document().tracks[static_cast<std::size_t>(t)].colour); };
        // Track 3 is the soloed one, so it is the only audible track: its light is lit in its colour, the others are dark, and the muted track is ringed.
        const auto lit = at(centre(strip.ledRect(2)));
        require(lit.getRed() > 200 && lit.getBlue() < 140 && lit.getBrightness() > 0.7f, "the light of an audible track glows in the track's colour");
        require(at(centre(strip.ledRect(0))).getBrightness() < 0.3f && at(centre(strip.ledRect(3))).getBrightness() < 0.3f, "tracks silenced by another track's solo have a dark light");
        auto ringPixel = [&](int t) {   // the mute ring is 8.5 px from the light's centre
            const auto c = centre(strip.ledRect(t));
            for (const int dx : {-9, -8, 8, 9}) { const auto p = at({c.x + dx, c.y}); if (p.getRed() > 190 && p.getGreen() > 140 && p.getBlue() < 120) return true; }
            return false;
        };
        require(ringPixel(1) && !ringPixel(0) && !ringPixel(2), "a muted track's light is ringed and no other is");
        // The solo and stop buttons light when they are on / the track is playing.
        auto solo = [&](int t) { return at({strip.soloRect(t).getX() + 3, strip.soloRect(t).getCentreY()}); };
        require(solo(2).getBlue() > 200 && solo(2).getRed() < 120 && solo(0).getBlue() < 120, "a soloed track's S button is lit blue and the others are not");
        auto stop = [&](int t) { return at(centre(strip.stopRect(t))); };
        require(stop(0).getGreen() > 180 && stop(0).getRed() < 120 && stop(2).getGreen() > 180, "the stop button of a playing track shows a lit square");
        require(std::abs(stop(1).getRed() - stop(1).getGreen()) < 20 && stop(1).getBrightness() < 0.5f, "and a track that is not playing shows a grey one");
        // The selected track's row has a bar in the track's colour at its left edge.
        const auto bar = at({1, strip.rowBounds(2).getCentreY()});
        require(bar.getRed() > 200 && bar.getBlue() < 140, "the selected track has a bar in its colour at the left edge");
        require(at({1, strip.rowBounds(1).getCentreY()}).getBrightness() < 0.25f, "and no other track does");
        // The name button is raised and has grip lines at its right end; the selected track's is lighter.
        const auto name = strip.nameRect(0);
        require(at({name.getX() + 30, name.getY() + 2}).getBrightness() > at({name.getX() + 30, name.getBottom() - 3}).getBrightness(), "the name button is raised: lighter at the top than the bottom");
        const int gripX = name.getRight() - 15;
        for (int line = 0; line < 4; ++line)
            require(at({gripX + 3 * line, name.getCentreY()}).getBrightness() + 0.03f < at({gripX + 3 * line + 2, name.getCentreY()}).getBrightness(), "the name button has four grip lines");
        require(at({name.getX() + 40, name.getY() + 4}).getBrightness() < at({strip.nameRect(2).getX() + 40, strip.nameRect(2).getY() + 4}).getBrightness(), "the selected track's name button is lighter");
        (void) trackColour;
    }
    {   // The knobs' pointers follow their values.
        Rig rig;
        rig.engine.tracks[0].gain.store(0.0f);  rig.engine.tracks[0].pan.store(-1.0f);
        rig.engine.tracks[1].gain.store(2.0f);  rig.engine.tracks[1].pan.store(1.0f);
        rig.strip().refresh();
        auto& strip = rig.strip();
        const auto image = rig.snapshot();
        const auto origin = strip.getPosition();
        auto at = [&](juce::Point<int> p) { return image.getPixelAt(origin.x + p.x, origin.y + p.y); };
        for (const auto& knob : {std::pair<int, bool>{0, true}, {0, false}}) {   // (track, isPan): value at the lower end
            const auto c = centre(knob.second ? strip.panRect(knob.first) : strip.volumeRect(knob.first));
            require(at({c.x - 4, c.y + 3}).getBrightness() > 0.7f && at({c.x + 3, c.y + 3}).getBrightness() < 0.5f, "a knob at its lowest points to the lower left");
        }
        for (const auto& knob : {std::pair<int, bool>{1, true}, {1, false}}) {
            const auto c = centre(knob.second ? strip.panRect(knob.first) : strip.volumeRect(knob.first));
            require(at({c.x + 3, c.y + 3}).getBrightness() > 0.7f && at({c.x - 4, c.y + 3}).getBrightness() < 0.5f, "a knob at its highest points to the lower right");
        }
        const auto c = centre(strip.volumeRect(2));   // untouched: straight up
        require(at({c.x, c.y - 5}).getBrightness() > 0.7f && at({c.x - 4, c.y + 3}).getBrightness() < 0.5f && at({c.x + 3, c.y + 3}).getBrightness() < 0.5f, "a knob at its default points straight up");
        // A knob off its default has a dot in the track's colour at the tip of its pointer; one at its default does not.
        const auto moved = centre(strip.volumeRect(0)), rest = centre(strip.volumeRect(2));
        const auto dot = at({moved.x - 6, moved.y + 6});
        require(dot.getGreen() > 190 && dot.getRed() < 140 && dot.getBlue() > 140, "a knob off its default shows a dot in its track's colour (track 1 is teal)");
        const auto panDot = at({centre(strip.panRect(0)).x - 6, centre(strip.panRect(0)).y + 6});
        require(panDot.getSaturation() > 0.4f && at({centre(strip.panRect(2)).x, centre(strip.panRect(2)).y - 8}).getSaturation() < 0.2f, "the pan knob shows the dot too when it is off centre");
        const auto none = at({rest.x, rest.y - 8});
        require(none.getSaturation() < 0.2f && dot.getSaturation() > 0.4f, "and a knob at its default shows none (the end of its pointer stays grey)");
    }
}

static void tileLook() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const auto teal = juce::Colour(0xff4fe0b6);
    auto draw = [&](SessionClipButton::Visual v, int width = 200) {
        SessionClipButton b;
        b.setSize(width, SessionView::rowHeight);
        b.setVisual(v);
        return b.createComponentSnapshot(b.getLocalBounds(), true, 1.0f);
    };
    SessionClipButton::Visual idle;
    idle.kind = SessionClipButton::Kind::midi; idle.colour = teal; idle.label = "4 notes";
    auto playing = idle; playing.playing = true;
    SessionClipButton::Visual empty;
    const auto imIdle = draw(idle), imPlaying = draw(playing), imEmpty = draw(empty);
    const int w = 200, h = SessionView::rowHeight;
    const juce::Point<int> face{w - 14, h / 2};                         // clear of the launch button and the label
    const juce::Point<int> triangle{static_cast<int>(SessionClipButton::launchWidth / 2), h / 2};

    // A clip is a tile in its track's colour, dimmer until it plays.
    const auto idleFace = imIdle.getPixelAt(face.x, face.y), playingFace = imPlaying.getPixelAt(face.x, face.y);
    require(idleFace.getGreen() > idleFace.getRed() + 15 && idleFace.getBrightness() > 0.35f && idleFace.getBrightness() < 0.75f, "an idle clip is a tile in its track's colour, dimmed");
    require(playingFace.getGreen() > 180 && playingFace.getRed() < 150 && playingFace.getBrightness() > idleFace.getBrightness() + 0.1f, "a playing clip is brighter");
    // The clip's label is drawn.
    int labelPixels = 0;
    for (int y = 8; y < h - 8; ++y) for (int x = static_cast<int>(SessionClipButton::launchWidth) + 6; x < 90; ++x) if (imIdle.getPixelAt(x, y).getBrightness() > 0.85f) ++labelPixels;
    require(labelPixels > 15, "a clip shows its label");
    // Raised: lighter along the top than the bottom.
    require(imIdle.getPixelAt(face.x, 4).getBrightness() > imIdle.getPixelAt(face.x, h - 5).getBrightness(), "clip tiles are raised: lighter at the top than the bottom");
    // The launch button: a darker square with a white triangle on it.
    require(imIdle.getPixelAt(triangle.x, triangle.y).getBrightness() > 0.8f && imIdle.getPixelAt(4, h / 2).getBrightness() < idleFace.getBrightness(), "an idle clip has a white launch triangle on a darker button");
    require(imPlaying.getPixelAt(triangle.x, triangle.y).getBrightness() < 0.2f, "a playing clip's triangle is dark on the bright tile");
    // An empty slot is a dark tile with a dim triangle (a little brighter than the tile), also raised.
    const auto emptyFace = imEmpty.getPixelAt(face.x, face.y);
    require(emptyFace.getBrightness() < 0.3f && imEmpty.getPixelAt(triangle.x, triangle.y).getBrightness() > emptyFace.getBrightness() + 0.05f, "an empty slot is dark with a dim launch triangle");
    require(imEmpty.getPixelAt(face.x, 4).getBrightness() > imEmpty.getPixelAt(face.x, h - 5).getBrightness(), "and raised");
    auto emptyPlaying = empty; emptyPlaying.playing = true;
    require(draw(emptyPlaying).getPixelAt(triangle.x, triangle.y).getBrightness() > imEmpty.getPixelAt(triangle.x, triangle.y).getBrightness() + 0.15f, "an empty slot the track is sitting on has a lit triangle");
    // Hover and press. (The button is painted directly with the state a mouse would give it.)
    auto drawState = [&](SessionClipButton::Visual v, bool over, bool down) {
        SessionClipButton b;
        b.setSize(w, h);
        b.setVisual(v);
        juce::Image image(juce::Image::ARGB, w, h, true);
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101114));
        b.paintButton(g, over, down);
        return image;
    };
    const auto hovered = drawState(idle, true, false), pressed = drawState(idle, true, true);
    require(hovered.getPixelAt(face.x, face.y).getBrightness() > idleFace.getBrightness() + 0.01f, "a clip under the mouse is a little lighter");
    require(pressed.getPixelAt(face.x, face.y).getBrightness() > hovered.getPixelAt(face.x, face.y).getBrightness() + 0.01f, "and one being pressed is lighter again");
    auto labelOf = [&](const juce::Image& image) {
        int n = 0;
        for (int y = 8; y < h - 8; ++y) for (int x = static_cast<int>(SessionClipButton::launchWidth) + 6; x < 90; ++x) if (image.getPixelAt(x, y).getBrightness() > 0.45f) ++n;
        return n;
    };
    require(labelOf(drawState(empty, true, false)) > 10 && labelOf(imEmpty) == 0, "an empty slot under the mouse says 'Empty'; at rest it says nothing");
    require(drawState(empty, true, false).getPixelAt(face.x, face.y).getBrightness() > emptyFace.getBrightness() + 0.01f, "and is a little lighter");
    // Progress along the bottom of a playing MIDI clip.
    auto progressing = playing; progressing.progress = 0.5f;
    const auto imProgress = draw(progressing);
    const int barY = h - 4;
    require(imProgress.getPixelAt(40, barY).getBrightness() + 0.08f < imProgress.getPixelAt(w - 30, barY).getBrightness(), "a playing clip shows how far through it is");
    require(std::abs(imPlaying.getPixelAt(40, barY).getBrightness() - imPlaying.getPixelAt(w - 30, barY).getBrightness()) < 0.05f, "and none when its position is unknown");
    // Offline audio is red and cannot be mistaken for a clip; audio has its waveform badge.
    auto offline = idle; offline.kind = SessionClipButton::Kind::offline; offline.label = "missing.wav";
    const auto imOffline = draw(offline);
    require(imOffline.getPixelAt(face.x - 14, face.y).getRed() > imOffline.getPixelAt(face.x - 14, face.y).getGreen() + 15, "an offline clip is red");
    auto audio = idle; audio.kind = SessionClipButton::Kind::audio; audio.label = "loop.wav";
    const auto imAudio = draw(audio);
    int badge = 0;
    for (int y = 10; y < h - 10; ++y) for (int x = w - 21; x < w - 7; ++x) if (imAudio.getPixelAt(x, y).getBrightness() > 0.75f) ++badge;
    int noBadge = 0;
    for (int y = 10; y < h - 10; ++y) for (int x = w - 21; x < w - 7; ++x) if (imIdle.getPixelAt(x, y).getBrightness() > 0.75f) ++noBadge;
    require(badge > noBadge + 5, "an audio clip has a waveform badge");
    // Selected: a white outline. Queued: an amber outline that blinks (so check it differs from none in either phase).
    auto selected = idle; selected.selected = true;
    require(draw(selected).getPixelAt(1, h / 2).getBrightness() > 0.8f && imIdle.getPixelAt(1, h / 2).getBrightness() + 0.2f < draw(selected).getPixelAt(1, h / 2).getBrightness(), "the selected clip has a white outline");
    auto queued = idle; queued.queued = true;
    const auto q = draw(queued).getPixelAt(1, h / 2), n = imIdle.getPixelAt(1, h / 2);
    require(std::abs(q.getRed() - n.getRed()) + std::abs(q.getGreen() - n.getGreen()) + std::abs(q.getBlue() - n.getBlue()) > 40, "a queued clip has an amber outline");
}

static void sceneAndTrackLook() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    Rig rig;
    dress(rig);
    const auto image = rig.snapshot();
    auto at = [&](juce::Point<int> p) { return image.getPixelAt(p.x, p.y); };
    // Scene buttons are raised; a scene that is playing is tinted green.
    const auto scene = rig.view.sceneArea(1);
    require(at({scene.getRight() - 12, scene.getY() + 3}).getBrightness() > at({scene.getRight() - 12, scene.getBottom() - 4}).getBrightness(), "scene buttons are raised: lighter at the top than the bottom");
    const auto playingScene = at({rig.view.sceneArea(0).getRight() - 12, rig.view.sceneArea(0).getCentreY()});
    require(playingScene.getGreen() > playingScene.getRed() + 12, "a scene with a clip playing is tinted green");
    require(at({scene.getRight() - 12, scene.getCentreY()}).getGreen() <= at({scene.getRight() - 12, scene.getCentreY()}).getRed() + 12, "and one with none is not");
    // The corner's Stop all is raised too.
    const auto stopAll = rig.view.stopAllArea();
    require(at({stopAll.getRight() - 12, stopAll.getY() + 3}).getBrightness() > at({stopAll.getRight() - 12, stopAll.getBottom() - 4}).getBrightness(), "Stop all is raised");
    // The tiles in place: a clip playing in the grid is lit; the selected one is outlined; the queued one blinks an outline.
    const auto playingClip = rig.view.clipArea(0, 0), idleClip = rig.view.clipArea(0, 3);
    require(at({playingClip.getRight() - 14, playingClip.getCentreY()}).getBrightness() > at({idleClip.getRight() - 14, idleClip.getCentreY()}).getBrightness() + 0.1f, "in the grid, the playing clip is brighter than an idle one on the same track");
    const auto selectedClip = rig.view.clipArea(2, 1);
    require(at({selectedClip.getX() + 1, selectedClip.getCentreY()}).getBrightness() > 0.8f, "the selected clip is outlined in white");
    const auto queuedClip = rig.view.clipArea(3, 3);
    const auto queuedBefore = at({queuedClip.getX() + 1, queuedClip.getCentreY()});
    rig.engine.requestedPianoRollClip[3].store(-1);
    rig.view.refreshClips();
    const auto queuedAfter = rig.snapshot().getPixelAt(queuedClip.getX() + 1, queuedClip.getCentreY());
    require(std::abs(queuedBefore.getRed() - queuedAfter.getRed()) + std::abs(queuedBefore.getGreen() - queuedAfter.getGreen()) + std::abs(queuedBefore.getBlue() - queuedAfter.getBlue()) > 40, "a queued clip in the grid has an outline that goes when it is not queued");
}

// ---- screenshots -------------------------------------------------------------------------------
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
static void dress(Rig& rig) {
    for (int t = 0; t < 4; ++t) for (int s = 0; s < 4; ++s) if ((t + s) % 3 != 1) rig.fillClip(t, s);
    rig.engine.tracks[1].mute.store(true);
    rig.engine.tracks[2].solo.store(true);
    rig.engine.tracks[0].gain.store(0.62f);
    rig.engine.tracks[3].pan.store(-0.45f);
    rig.engine.tracks[2].gain.store(1.4f);
    rig.engine.activePianoRollClip[0].store(0);
    rig.engine.sessionTrackPositions[0].store(21000);
    rig.engine.activePianoRollClip[2].store(2);
    rig.engine.requestedPianoRollClip[3].store(3);
    rig.controller.selectClip(2, 1);
    rig.view.refreshFromDocument();
    rig.view.refreshClips();
}
static void snapshots() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const char* directory = std::getenv("ALIEN_SNAPSHOT_DIR");
    {
        Rig rig(1280, 720);
        dress(rig);
        const auto image = rig.snapshot();
        require(image.getWidth() == rig.view.getWidth() && image.getHeight() == rig.view.getHeight() && hasDetail(image), "the session page draws real content");
        writePng(image, directory, "session-4x4.png");
    }
    {
        Rig rig(1280, 720);
        for (int i = 0; i < 10; ++i) rig.controller.addTrack();
        for (int i = 0; i < 4; ++i) rig.controller.addScene();
        rig.view.rebuildGrid();
        for (int t = 0; t < 14; t += 2) for (int s = 0; s < 8; s += 3) rig.fillClip(t, s);
        rig.view.refreshClips();
        rig.controller.selectClip(5, 2);
        rig.view.refreshFromDocument();
        writePng(rig.snapshot(), directory, "session-14x8.png");
    }
    {
        Rig rig(900, 640);
        dress(rig);
        writePng(rig.snapshot(), directory, "session-narrow.png");
    }
}

// The Copy / link menu for the selected cell (T1-03 B2a).
static void linkMenu() {
    Rig rig;
    auto& c = rig.controller;
    rig.fillClip(0, 0);
    juce::PopupMenu shown; std::function<void(int)> choose;
    rig.view.linkMenuLauncher = [&](juce::PopupMenu m, std::function<void(int)> done) { shown = std::move(m); choose = std::move(done); };
    int changed = 0;
    rig.view.onCellContentChanged = [&] { ++changed; };
    const auto enabled = [&](int id) {
        for (juce::PopupMenu::MenuItemIterator it(shown); it.next();) if (it.getItem().itemID == id) return it.getItem().isEnabled;
        return false;
    };
    c.selectClip(0, 0); rig.view.showLinkMenu();
    require(enabled(1) && !enabled(2) && !enabled(3) && !enabled(4), "a filled cell can be copied; nothing to paste or unlink yet");
    choose(1);
    c.selectClip(1, 2); rig.view.showLinkMenu();
    require(enabled(2) && enabled(3) && !enabled(1), "after Copy, an empty cell offers both pastes");
    choose(3);
    require(changed == 1 && c.document().linkedCells(1, 2) == 2 && c.undoLabel() == "Paste linked clip", "Paste linked here links the cell");
    rig.view.refreshClips();
    c.selectClip(0, 0); rig.view.showLinkMenu();
    bool namesOther = false;
    for (juce::PopupMenu::MenuItemIterator it(shown); it.next();) namesOther = namesOther || it.getItem().text == "Make unique (linked with 1 other cell)";
    require(enabled(4) && namesOther, "Make unique says how many cells it is linked with");
    choose(4);
    require(changed == 2 && c.document().linkedCells(0, 0) == 1 && c.document().linkedCells(1, 2) == 1, "and makes the cell its own again");
    std::cout << "PASS: the Copy / link menu\n";
}

static void rackLaunchMenu() {
    Rig rig;
    auto& c = rig.controller;
    require(c.editRack("Second pattern", [](rack::Project& p) { return p.addPattern(0); }), "second rack source");
    c.selectRackPattern(0); c.markAsNewBaseline();
    const auto before = c.document();
    const auto second = c.channelRack().clips[1];
    std::function<void(int)> choose;
    rig.view.rackMenuLauncher = [&](juce::PopupMenu, std::function<void(int)> done) { choose = std::move(done); };
    rig.view.showRackClipMenu(); choose(2);
    require(rig.engine.requestedRackClip.load() == second && rig.engine.playing.load()
        && c.channelRack().selectedPattern == 0 && sameContent(before, c.document()) && !c.canUndo() && !c.isDirty(),
        "Perform launch addresses the shared source without selection, edit, dirty state or undo");
    std::uint64_t opened{};
    rig.view.onEditRackClip = [&](std::uint64_t id) { opened = id; };
    rig.view.showRackClipMenu(); choose(1002);
    require(opened == second && rig.engine.requestedRackClip.load() == second, "Edit opens the same source without launching another clip");
    rig.view.showRackClipMenu(); choose(2000);
    require(rig.engine.requestedRackClip.load() == 0, "menu offers quantized rack stop");
    rig.view.showRackClipMenu();
    require(c.deleteRackPattern(1), "remove a source while popup is open"); choose(2);
    require(rig.engine.requestedRackClip.load() == 0, "a stale menu cannot launch whichever slot replaced its source");
    c.setExternalBusy(true); require(!c.launchRackClip(c.channelRack().clips[0]), "busy launches refused"); c.setExternalBusy(false);
    rig.engine.patternMode.store(2); require(!c.launchRackClip(c.channelRack().clips[0]), "Song playback is not silently replaced");
    c.newProject();
    require(rig.engine.requestedRackClip.load() == rack::RackPlayer::noLaunchRequest && !rig.engine.rackPerformMode.load(),
            "new document clears pending rack launches and Perform mode");
    rig.engine.patternMode.store(1);
    require(c.editRack("New pattern", [](rack::Project& p) { return p.addPattern(); }), "new source for undo");
    const auto removed = c.channelRack().clips[1];
    require(c.launchRackClip(removed) && c.undo(), "undo creation with launch still pending");
    require(c.editRack("Another pattern", [](rack::Project& p) { return p.addPattern(); }), "a different source after undo");
    require(c.channelRack().clips[1] != removed && !c.launchRackClip(removed),
            "undo followed by creation cannot redirect an old launch to a different clip");
}

static void savedRackAssignmentMenu() {
    Rig rig;
    auto& c = rig.controller;
    const auto source = c.channelRack().clips[0];
    std::function<void(int)> choose;
    rig.view.linkMenuLauncher = [&](juce::PopupMenu, std::function<void(int)> done) { choose = std::move(done); };
    c.selectClip(0, 0); rig.view.showLinkMenu(); choose(1000);
    require(c.document().clip(0, 0).rackClip == source && rig.view.clipSlot(0, 0)->getButtonText().contains("Rack"),
            "the assignment submenu creates a labelled saved rack cell");
    clickButton(*rig.view.clipSlot(0, 0));
    require(rig.engine.requestedTrackRackClip[0].load() == source, "the actual tile launches the composite source");
    rig.view.showLinkMenu(); choose(5);
    require(c.timeline().regions.size() == 1 && c.timeline().regions[0].isPattern() && c.timeline().regions[0].rackClip == source,
            "the placement menu places that same shared rack source in Song");
    require(c.copyCell(0, 0), "copy bound rack cell"); c.selectClip(1, 1);
    rig.view.showLinkMenu(); choose(2);
    require(c.document().clip(1, 1).rackClip != 0 && c.document().clip(1, 1).rackClip != source,
            "Paste a copy creates an independent composite source");
    require(c.undo() && c.document().clip(1, 1).isEmpty() && c.redo(), "assignment copies have one-step undo and redo");
    const auto other = c.document().clip(1, 1).rackClip;
    require(c.assignRackClip(1, 0, other), "a scene accepts a different source on another row");
    rig.engine.activeTrackRackClip[0].store(source); rig.engine.activeTrackRackClip[1].store(other);
    rig.view.refreshClips();
    require(rig.view.clipSlot(0, 0)->getButtonText().contains("Playing") && rig.view.clipSlot(1, 0)->getButtonText().contains("Playing"),
        "overlapping rack tiles report their own row's playing state");
    rig.engine.activeTrackRackClip[0].store(0); rig.view.refreshClips();
    require(!rig.view.clipSlot(0, 0)->getButtonText().contains("Playing") && rig.view.clipSlot(1, 0)->getButtonText().contains("Playing"),
        "stopping one row leaves the other tile's playing state visible");
    std::cout << "PASS: saved rack assignment, tile launch, independent paste and Song placement menu\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    layoutChecks();
    wheelEvents();
    launching();
    launchingIsHeard();
    engineStopsOneTrack();
    headerControls();
    muteSoloCommandHistory();
    lightHitArea();
    knobReadouts();
    selectionAndStructure();
    tooltips();
    followsMixer();
    headerLook();
    tileLook();
    sceneAndTrackLook();
    linkMenu();
    rackLaunchMenu();
    savedRackAssignmentMenu();
    snapshots();
    std::cout << "SessionViewTests passed\n";
    return 0;
}
