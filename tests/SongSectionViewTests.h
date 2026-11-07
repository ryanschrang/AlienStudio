#pragma once
static void sectionViewChecks() {
    Rig rig; auto& c = rig.c;
    const auto at = [&](std::int64_t tick) { return juce::Point<int>{juce::roundToInt(c.xForTick(tick)), TimelineCanvas::sectionTop + 8}; };
    const auto choose = [&](const juce::String& label) {
        const auto list = rig.menu.items(); int id = 0;
        for (const auto& item : list) if (item.text == label && item.enabled) id = item.id;
        require(id != 0 && rig.menu.done != nullptr, "section menu exposes enabled action");
        auto done = rig.menu.done; done(id);
    };
    require(rig.controller.editTimeline("Set loop", [](tl::Timeline& t) { return t.setLoop(bar, 3 * bar); }), "prepare section loop range");
    rig.rightClick(at(0)); choose("Create section from loop");
    require(rig.t().sections.size() == 1 && rig.t().sections[0].start == bar && rig.t().sections[0].end == 3 * bar, "real section menu creates named loop range");
    const auto original = rig.t().sections[0];
    require(c.hitTest(at(2 * bar)).section == original.id && c.sectionBounds(original).getY() >= TimelineCanvas::sectionTop
        && c.sectionBounds(original).getBottom() <= TimelineCanvas::topHeight, "section strip draws and hit-tests independently of ruler");
    rig.controller.undo(); rig.view.refresh(); require(rig.t().sections.empty(), "one Undo removes section");
    rig.controller.redo(); rig.view.refresh(); require(rig.t().sections[0] == original, "Redo preserves section identity and range");
    rig.press(at(2 * bar)); require(!rig.seeks.empty() && rig.seeks.back() == bar, "section click seeks to start");
    rig.rightClick(at(2 * bar)); choose("Rename section..."); require(rig.asked, "section rename dialog opens");
    rig.answer("Chorus"); require(rig.t().sections[0].name == "Chorus", "section rename applies");
    rig.rightClick(at(2 * bar)); choose("Colour 6"); require(rig.t().sections[0].colour == 5, "section palette applies colour");
    rig.rightClick(at(2 * bar)); choose("Loop this section"); require(rig.t().loopEnabled && rig.t().loopStart == bar && rig.t().loopEnd == 3 * bar, "loop section enables exact exclusive range");
    rig.controller.undo(); rig.view.refresh(); require(!rig.t().loopEnabled, "loop section is one undo step");
    require(rig.controller.editTimeline("Move loop", [](tl::Timeline& t) { return t.setLoop(4 * bar, 6 * bar); }), "prepare section range edit");
    rig.rightClick(at(2 * bar)); choose("Use current loop as section range");
    require(rig.t().sections[0].start == 4 * bar && rig.t().sections[0].end == 6 * bar && rig.t().sections[0].id == original.id, "section range edit follows loop without changing identity");
    rig.rightClick(at(5 * bar)); choose("Delete section"); require(rig.t().sections.empty(), "section menu deletes range");
    rig.controller.undo(); rig.view.refresh(); const auto restored = rig.t().sections[0];
    const auto redo = rig.controller.redoLabel();
    require(!c.addSectionFromLoop("Overlap") && rig.controller.redoLabel() == redo, "overlap refusal preserves redo");
    rig.controller.setExternalBusy(true); require(!c.removeSection(restored, rig.controller.document().identityScope), "busy section deletion refuses"); rig.controller.setExternalBusy(false);
    const auto messageCount = rig.messages.size();
    rig.rightClick(at(5 * bar)); choose("Rename section..."); rig.answer("  ");
    require(rig.t().sections[0] == restored && rig.controller.redoLabel() == redo && rig.messages.size() > messageCount,
        "blank section rename reports refusal and preserves content and redo");
    rig.rightClick(at(5 * bar)); rig.asked = false;
    require(rig.controller.editTimeline("External section edit", [=](tl::Timeline& t) { auto changed = restored; changed.name = "Changed"; return t.setSection(changed); }), "change target while menu open");
    choose("Rename section..."); require(!rig.asked && rig.t().sections[0].name == "Changed", "stale section menu refuses changed content");
    rig.rightClick(at(5 * bar)); choose("Rename section..."); const auto answer = rig.answer;
    rig.controller.newProject(); rig.view.refresh(); answer("Late rename");
    require(rig.t().sections.empty(), "rename dialog refuses replaced project");
    rig.rightClick(at(0)); const auto pending = rig.menu.done;
    int add = 0; for (const auto& item : rig.menu.items()) if (item.text == "Create section from loop") add = item.id;
    require(rig.controller.editTimeline("New loop", [](tl::Timeline& t) { return t.setLoop(bar, 2 * bar); }), "change loop while create menu open");
    pending(add); require(rig.t().sections.empty(), "create menu refuses stale loop range");
}
