#pragma once
static void sectionProgressionChecks() {
    Rig rig;
    require(rig.controller.editTimeline("Set up progression", [](tl::Timeline& t) {
        return t.addSection(bar, 4 * bar, "Verse")
            && t.addChord(0, 3 * bar / 2, 0, alien::ChordType::major)
            && t.addChord(2 * bar, 5 * bar, 7, alien::ChordType::minor7);
    }), "prepare a section with two clipped chords and a gap");
    const auto point = juce::Point<int>{juce::roundToInt(rig.c.xForTick(2 * bar)), TimelineCanvas::sectionTop + 8};
    const auto destination = "1. " + rig.controller.document().tracks[0].name;
    const auto chooseTrack = [&] {
        for (const auto& item : rig.menu.items()) {
            if (item.text == destination && item.enabled) {
                auto done = rig.menu.done;
                done(item.id);
                return;
            }
        }
        require(false, "section menu offers the destination track");
    };
    rig.rightClick(point);
    chooseTrack();
    require(rig.t().regions.size() == 1, "progression creates one placement");
    const auto region = rig.t().regions[0];
    require(region.ownsClip() && region.row == 0 && region.start == bar && region.end() == 4 * bar,
        "progression placement covers the section");
    const auto notes = rig.controller.document().contentOf(alien::library::ClipId{region.clip}).pattern;
    require(notes->count == 7 && notes->notes[0].startStep == 0 && notes->notes[0].lengthSteps == 8
        && notes->notes[3].startStep == 16 && notes->notes[3].lengthSteps == 32
        && notes->notes[3].pitch == 67, "progression clips both ends and preserves the gap");
    rig.rightClick(point);
    chooseTrack();
    require(rig.t().regions.size() == 1 && rig.t().regions[0] == region, "occupied destination preserves the placement");
    rig.controller.undo(); rig.view.refresh();
    require(rig.t().regions.empty() && rig.t().chords.size() == 2, "one Undo removes only the generated clip");
    rig.controller.redo(); rig.view.refresh();
    require(rig.t().regions.size() == 1 && rig.t().regions[0] == region, "Redo restores the progression placement");
    rig.controller.undo(); rig.view.refresh();
    rig.rightClick(point);
    require(rig.controller.editTimeline("Change chord", [](tl::Timeline& t) {
        auto chord = t.chords[0];
        chord.root = 2;
        return t.setChord(chord);
    }), "change the progression while the menu is open");
    chooseTrack();
    require(rig.t().regions.empty(), "stale progression menu refuses changed chord context");
}

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
