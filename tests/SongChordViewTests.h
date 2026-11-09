#pragma once
static void chordViewChecks() {
    Rig rig; auto& canvas = rig.c;
    const auto at = [&](std::int64_t tick) { return juce::Point<int>{juce::roundToInt(canvas.xForTick(tick)), TimelineCanvas::chordTop + 8}; };
    const auto choose = [&](const juce::String& label) {
        int id = 0; for (const auto& item : rig.menu.items()) if (item.text == label && item.enabled) id = item.id;
        require(id && rig.menu.done, "chord menu exposes requested enabled action"); auto done = rig.menu.done; done(id);
    };
    require(rig.controller.setProjectScale(2, alien::ScaleType::naturalMinor, rig.controller.document().identityScope), "prepare project key for chord root seed");
    require(rig.controller.editTimeline("Set loop", [](tl::Timeline& t) { return t.setLoop(bar, 3 * bar); }), "prepare chord loop range");
    rig.rightClick(at(0)); choose("Create D Major chord from loop");
    require(rig.t().chords.size() == 1 && rig.t().chords[0].root == 2 && rig.t().chords[0].quality == alien::ChordType::major
        && rig.t().chords[0].start == bar && rig.t().chords[0].end == 3 * bar, "chord menu creates explicit major quality with project-seeded root");
    const auto original = rig.t().chords[0];
    const auto track = rig.controller.document().trackId(alien::TrackRole::session, 0);
    const auto scope = rig.controller.document().identityScope;
    rig.rightClick(at(2 * bar));
    choose("1. " + rig.controller.document().tracks[0].name);
    require(rig.t().regions.size() == 1, "chord menu creates one MIDI placement");
    const auto region = rig.t().regions[0];
    require(region.ownsClip() && region.row == 0 && region.start == bar && region.end() == 3 * bar,
        "MIDI placement covers the exact chord range on the chosen track");
    const auto notes = rig.controller.document().contentOf(alien::library::ClipId{region.clip}).pattern;
    require(notes->count == 3 && notes->notes[0].pitch == 62 && notes->notes[1].pitch == 66
        && notes->notes[2].pitch == 69 && notes->notes[0].lengthSteps == 32,
        "placement voices D major for the chord duration");
    std::uint32_t refused = 0;
    require(rig.controller.placeChordMidi(original, track, scope, refused).isNotEmpty() && !refused
        && rig.t().regions.size() == 1, "occupied chord destination refuses without replacing the clip");
    rig.controller.undo(); rig.view.refresh();
    require(rig.t().regions.empty() && rig.t().chords[0] == original, "one Undo removes MIDI and keeps its source chord");
    rig.controller.redo(); rig.view.refresh();
    require(rig.t().regions[0] == region, "Redo restores the same MIDI placement");
    rig.controller.undo(); rig.view.refresh();
    require(canvas.hitTest(at(2 * bar)).chord == original.id && canvas.chordBounds(original).getY() >= TimelineCanvas::chordTop
        && canvas.chordBounds(original).getBottom() <= TimelineCanvas::topHeight, "chord strip has independent drawing and hit geometry");
    rig.controller.undo(); rig.view.refresh(); require(rig.t().chords.empty(), "one Undo removes chord range");
    rig.controller.redo(); rig.view.refresh(); require(rig.t().chords[0] == original, "Redo restores exact chord identity/context");
    rig.press(at(2 * bar)); require(!rig.seeks.empty() && rig.seeks.back() == bar, "chord click seeks to its start");
    rig.rightClick(at(2 * bar)); choose("C#");
    require(rig.t().chords[0].root == 1, "root submenu edits pitch class");
    rig.rightClick(at(2 * bar)); choose("Minor 7");
    require(rig.t().chordAt(2 * bar)->quality == alien::ChordType::minor7 && rig.t().chords[0].id == original.id, "quality submenu updates shared harmonic context without replacing identity");
    rig.rightClick(at(2 * bar)); choose("Loop this chord");
    require(rig.t().loopEnabled && rig.t().loopStart == bar && rig.t().loopEnd == 3 * bar, "loop chord sets and enables exact range");
    rig.controller.undo(); rig.view.refresh(); require(!rig.t().loopEnabled, "loop chord is one undo step");
    require(rig.controller.editTimeline("Move loop", [](tl::Timeline& t) { return t.setLoop(4 * bar, 6 * bar); }), "prepare chord resize range");
    rig.rightClick(at(2 * bar)); choose("Use current loop as chord range");
    require(rig.t().chords[0].start == 4 * bar && rig.t().chords[0].end == 6 * bar, "chord range edit follows current loop");
    rig.rightClick(at(5 * bar)); choose("Delete chord"); require(rig.t().chords.empty(), "chord menu deletes context");
    rig.controller.undo(); rig.view.refresh(); const auto restored = rig.t().chords[0]; const auto redo = rig.controller.redoLabel();
    rig.rightClick(at(5 * bar)); choose("C#");
    require(rig.t().chords[0] == restored && rig.controller.redoLabel() == redo, "no-op root choice preserves redo");
    require(!canvas.addChordFromLoop(7, alien::ChordType::major) && rig.controller.redoLabel() == redo, "overlap refusal preserves chord content and redo");
    rig.controller.setExternalBusy(true); require(!canvas.removeChord(restored, rig.controller.document().identityScope), "busy chord deletion refuses"); rig.controller.setExternalBusy(false);
    rig.rightClick(at(5 * bar));
    require(rig.controller.editTimeline("External chord edit", [=](tl::Timeline& t) { auto next = restored; next.root = 7; return t.setChord(next); }), "change chord while menu is open");
    choose("Major"); require(rig.t().chords[0].root == 7 && rig.t().chords[0].quality == alien::ChordType::minor7, "stale quality menu refuses changed target");
    rig.rightClick(at(5 * bar)); auto pendingDelete = rig.menu.done;
    int remove = 0; for (const auto& item : rig.menu.items()) if (item.text == "Delete chord") remove = item.id;
    rig.controller.newProject(); rig.view.refresh(); pendingDelete(remove); require(rig.t().chords.empty(), "stale chord menu refuses replaced project");
    rig.rightClick(at(0)); auto pendingAdd = rig.menu.done;
    int add = 0; for (const auto& item : rig.menu.items()) if (item.text == "Create C Major chord from loop") add = item.id;
    require(add && rig.controller.editTimeline("Change loop", [](tl::Timeline& t) { return t.setLoop(bar, 2 * bar); }), "change loop while chord creation menu is open");
    pendingAdd(add); require(rig.t().chords.empty(), "chord creation refuses changed loop target");
}
