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
    std::cout << "PASS: restored native geometry, maximize/normal state, cached docking and shared-editor reset\n";
}
