#pragma once
#include "WindowLayout.h"
#include "DetachableView.h"

static void windowLayoutChecks() {
    namespace layout = alien::layout;
    layout::State state;
    state.mainWindow = {{-1800, 80, 1100, 700}, {-1920, 0, 1920, 1040}, true};
    state.selectedView = 8; state.devices = true; state.keyboard = true;
    state.panels[3] = {true, layout::WindowState{{150, 70, 1200, 600}, {0, 0, 1920, 1040}, false}};
    state.panels[5] = {false, layout::WindowState{{200, 100, 1000, 500}, {0, 0, 1920, 1040}, true}};
    const auto json = layout::serialize(state);
    layout::State parsed;
    require(layout::parse(json, parsed) && parsed == state, "layout round trip retains stable view keys, normal/maximized geometry and cached docked bounds");
    const auto damage = [&](auto edit) {
        auto value = juce::JSON::parse(json); edit(value);
        require(!layout::parse(juce::JSON::toString(value), parsed) && parsed == state, "damaged layout is refused without partially replacing state");
    };
    damage([](auto& v) { v.getDynamicObject()->setProperty("version", 2); });
    damage([](auto& v) { v.getDynamicObject()->setProperty("keyboard", 1); });
    damage([](auto& v) { v.getDynamicObject()->setProperty("selectedView", "unknown"); });
    damage([](auto& v) { v.getProperty("main", {}).getDynamicObject()->setProperty("maximized", "true"); });
    damage([](auto& v) { auto r = v.getProperty("main", {}).getProperty("bounds", {}); r.getArray()->set(2, 0); v.getProperty("main", {}).getDynamicObject()->setProperty("bounds", r); });
    damage([](auto& v) { auto r = v.getProperty("main", {}).getProperty("bounds", {}); r.getArray()->set(2, -20); v.getProperty("main", {}).getDynamicObject()->setProperty("bounds", r); });
    damage([](auto& v) { auto r = v.getProperty("main", {}).getProperty("bounds", {}); r.getArray()->set(0, 2000000); v.getProperty("main", {}).getDynamicObject()->setProperty("bounds", r); });
    damage([](auto& v) { auto r = v.getProperty("main", {}).getProperty("bounds", {}); r.getArray()->set(1, .5); v.getProperty("main", {}).getDynamicObject()->setProperty("bounds", r); });
    damage([](auto& v) { v.getProperty("panels", {}).getDynamicObject()->removeProperty("rack"); });
    damage([](auto& v) { v.getProperty("panels", {}).getProperty("mixer", {}).getDynamicObject()->removeProperty("window"); });
    const juce::Rectangle<int> left{-1920, 0, 1920, 1040}, right{0, 0, 1920, 1040};
    require(layout::fit(state.mainWindow, {right, left}, layout::mainLimits) == state.mainWindow.bounds, "unchanged negative-coordinate monitor keeps exact geometry");
    require(right.contains(layout::fit(state.mainWindow, {right}, layout::mainLimits)), "removed monitor brings the entire window and title bar onto primary");
    const juce::Rectangle<int> smallArea{0, 0, 800, 560};
    const auto fitted = layout::fit(state.mainWindow, {smallArea}, layout::mainLimits);
    require(smallArea.contains(fitted) && fitted.getWidth() == 800 && fitted.getHeight() == 560, "small logical display reduces minimum dimensions instead of stranding controls offscreen");
    layout::WindowState edge{{1400, 540, 1000, 700}, right, false};
    require(right.contains(layout::fit(edge, {right}, layout::mainLimits)), "partially offscreen settings are clamped to a reachable window");
    layout::WindowState scaled{{1920, 0, 1280, 800}, {1920, 0, 2560, 1400}, false};
    const juce::Rectangle<int> changed{1280, 0, 1600, 900};
    require(changed.contains(layout::fit(scaled, {changed}, layout::mainLimits)), "changed logical origin/resolution fits without double-applying a DPI scale");
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienLayoutTest", "", false);
    const auto file = directory.getChildFile("WindowLayout.json");
    require(layout::save(file, state) && layout::load(file) == state, "layout persists through an atomic temporary-file replacement and reload");
    auto invalid = state; invalid.selectedView = -1;
    require(!layout::save(file, invalid) && layout::load(file) == state, "invalid save leaves previous preference file intact");
    require(!layout::save(file.getChildFile("blocked.json"), state) && layout::load(file) == state, "write failure leaves earlier layout intact");
    require(file.replaceWithText("{broken") && layout::load(file) == layout::defaultState(), "corrupt preferences fall back to safe default layout");
    require(file.replaceWithText(juce::String::repeatedString("x", 65537)) && layout::load(file) == layout::defaultState(), "oversized settings are refused before loading JSON");
    require(directory.deleteRecursively(), "remove only the test-created preferences directory");
    std::cout << "PASS: saved layout round trip, strict damage refusal, monitor recovery and preference I/O\n";
}

static void restoredDetachedLayoutChecks() {
    namespace layout = alien::layout;
    juce::Component editor; editor.getProperties().set("shared", 73);
    alien::DetachableView host("Mixer", editor); host.setSize(1100, 550);
    int opened = 0, closed = 0;
    host.onWindowOpened = [&](auto&) { ++opened; };
    host.onWindowClosed = [&](auto&) { ++closed; };
    host.detach();
    auto* first = host.detachedWindow();
    layout::apply(*first, {{30, 40, 1050, 500}, layout::displays().front(), false}, layout::panelLimits);
    if (const auto frame = first->getPeer()->getFrameSizeIfPresent())
        require(layout::areaFor(first->getBounds()).contains(frame->addedTo(first->getBounds())), "native title bar and borders fit the usable display area");
    const auto saved = host.layoutState();
    require(saved.detached && saved.window && saved.window->bounds == first->getBounds(), "detached layout captures the native window geometry");
    first->setFullScreen(true);
    const auto maximized = host.layoutState();
    require(maximized.window->maximized && maximized.window->bounds == saved.window->bounds, "maximized capture retains normal bounds");
    first->setFullScreen(false);
    host.dock();
    require(!host.layoutState().detached && host.layoutState().window, "docking retains the last floating bounds for the next detach");
    host.detach();
    require(host.detachedWindow()->getBounds() == layout::fit(*saved.window, layout::displays(), layout::panelLimits), "detaching again reuses the remembered normal window");
    host.restoreLayout(saved);
    require(host.isDetached() && editor.getTopLevelComponent() == host.detachedWindow()
        && static_cast<int>(editor.getProperties()["shared"]) == 73, "saved layout restores the same editor and its state without duplicating project ownership");
    host.restoreLayout(maximized);
    require(host.detachedWindow()->isFullScreen() && host.layoutState().window->bounds == maximized.window->bounds,
        "saved maximized layout restores with the original normal geometry");
    host.detachedWindow()->setFullScreen(false);
    require(host.detachedWindow()->getBounds() == layout::fit(*saved.window, layout::displays(), layout::panelLimits),
        "unmaximizing a restored window returns to its saved normal position and size");
    host.restoreLayout({});
    require(!host.layoutState().window, "reset clears remembered floating geometry");
    require(!host.isDetached() && editor.getParentComponent() == &host && opened == closed,
        "default-layout reset docks the editor and balances native-window listeners");
    {
        class RecoveryWindow final : public juce::DocumentWindow {
        public:
            RecoveryWindow() : DocumentWindow("Display recovery test", juce::Colours::black, allButtons) {
                setUsingNativeTitleBar(true); setResizable(true, true);
            }
            void closeButtonPressed() override {}
        } live;
        juce::Component content;
        content.getProperties().set("shared", 91);
        live.setContentNonOwned(&content, false);
        layout::Tracker tracker(live, layout::panelLimits);
        const auto screens = layout::displays();
        const auto area = screens.front();
        auto expanded = screens;
        expanded.push_back({area.getRight() + 100, area.getY(), 1920, 1080});
        int offscreenX = area.getRight();
        for (const auto screen : screens) offscreenX = juce::jmax(offscreenX, screen.getRight());
        offscreenX += 8192;
        layout::apply(live, {{area.getX() + 30, area.getY() + 40, 1000, 500}, area, false}, layout::panelLimits);
        live.setVisible(true);
        const auto initial = live.getBounds();
        require(!tracker.refreshDisplays(expanded) && live.getBounds() == initial,
            "adding an unrelated monitor does not move an already reachable window");
        live.setTopLeftPosition(offscreenX, area.getY() + 50);
        require(tracker.refreshDisplays(screens), "removing a display recovers an open offscreen native window");
        const auto recovered = live.getBounds();
        if (const auto frame = live.getPeer()->getFrameSizeIfPresent())
            require(layout::areaFor(recovered).contains(frame->addedTo(recovered)), "live recovery includes the native title bar and borders");
        require(!tracker.refreshDisplays(screens) && live.getBounds() == recovered,
            "unchanged topology leaves window geometry alone");
        tracker.refreshDisplays(expanded);
        live.setMinimised(true);
        require(!tracker.refreshDisplays(screens) && live.isMinimised(), "display recovery never unminimizes a window");
        live.setMinimised(false);
        live.setTopLeftPosition(offscreenX, area.getY() + 50);
        require(tracker.refreshDisplays(screens), "a deferred topology change is retried after unminimizing");
        tracker.refreshDisplays(expanded);
        live.setVisible(false);
        live.setTopLeftPosition(offscreenX, area.getY() + 50);
        require(!tracker.refreshDisplays(screens) && !live.isVisible(), "hidden windows are left hidden");
        live.setVisible(true);
        require(tracker.refreshDisplays(screens), "hidden recovery is retried after showing the window");
        live.setFullScreen(true);
        const auto normal = tracker.capture().bounds;
        const auto maximizedBounds = live.getBounds();
        auto reduced = area.withTrimmedBottom(80);
        require(tracker.refreshDisplays({reduced}) && live.isFullScreen(), "work-area changes preserve maximized state");
        require(live.getBounds() == maximizedBounds && reduced.contains(live.getPeer()->getNonFullScreenBounds()),
            "recovery updates only the restore bounds while the OS manages the maximized frame");
        const auto restoredBounds=live.getPeer()->getNonFullScreenBounds();
        live.setFullScreen(false);
        require(tracker.capture().bounds==restoredBounds,"capture preserves recovered bounds during the native restore transition");
        live.setVisible(false); const auto hiddenBounds=live.getBounds();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        require(!live.isVisible() && live.getBounds()==hiddenBounds,"pending restore recovery does not move or show a hidden window");
        live.setVisible(true);
        for (int i=0;i<40 && live.getBounds()!=restoredBounds;++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        // Showing can already restore the right rectangle. Dispatch the queued
        // correction as well before testing a subsequent independent user move.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        require(reduced.contains(live.getBounds()) && tracker.capture().bounds.getWidth() <= normal.getWidth(),
            "unmaximizing returns to the recovered normal bounds");
        require(live.getBounds()==restoredBounds,"normal restoration uses the recovered client rectangle");
        // The client rectangle excludes the native frame. On a 1024-wide
        // desktop the restored client can be only eight pixels from that edge;
        // blindly moving left by sixteen asks for an offscreen frame, which the
        // next display refresh is supposed to recover. Choose a reachable move
        // so this checks stale restore work, not offscreen recovery or DPI rounding.
        auto clientArea=reduced;
        if (const auto frame=live.getPeer()->getFrameSizeIfPresent()) clientArea=frame->subtractedFrom(reduced);
        const int dx=restoredBounds.getX()-clientArea.getX()>=8 ? -8
            : clientArea.getRight()-restoredBounds.getRight()>=8 ? 8 : 0;
        const int dy=restoredBounds.getY()-clientArea.getY()>=16 ? -16
            : clientArea.getBottom()-restoredBounds.getBottom()>=16 ? 16 : 0;
        const auto moved=restoredBounds.translated(dx,dy);
        require(moved!=restoredBounds && clientArea.contains(moved),
            "manual-move fixture leaves the complete native frame reachable");
        live.setBounds(moved); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        const auto actual=live.getBounds();
        const bool movedAsRequested=std::abs(actual.getX()-moved.getX())<=1 && std::abs(actual.getY()-moved.getY())<=1
            && actual.getWidth()==moved.getWidth() && actual.getHeight()==moved.getHeight();
        if (!movedAsRequested || tracker.capture().bounds!=actual)
            std::cerr << "recovery move expected=" << moved.toString() << " actual=" << live.getBounds().toString()
                      << " captured=" << tracker.capture().bounds.toString() << '\n';
        require(movedAsRequested && tracker.capture().bounds==actual,"completed recovery does not override a later manual move");
        require(live.getContentComponent() == &content && static_cast<int>(content.getProperties()["shared"]) == 91,
            "display recovery preserves the editor and its state");
        live.clearContentComponent();
    }
    std::cout << "PASS: restored native geometry, maximize/normal state, cached docking and shared-editor reset\n";
}
