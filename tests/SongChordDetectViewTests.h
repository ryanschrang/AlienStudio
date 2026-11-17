#pragma once
// Song region menu > Detect chords into the chord track (CONV-03), through the real menu and project history.
static void chordDetectViewChecks() {
    Rig rig;
    const auto choose = [&](const juce::String& label) {
        int id = 0; for (const auto& item : rig.menu.items()) if (item.text == label && item.enabled) id = item.id;
        require(id && rig.menu.done, "the region menu offers chord detection"); auto done = rig.menu.done; done(id);
    };
    // Four bars on track 1, scene 1: C, A minor, F, G7, each with its root in the bass.
    PianoRollPattern progression;
    progression.loopSteps = 64;
    const auto hold = [&](double at, std::initializer_list<int> pitches) { for (const int p : pitches) progression.add(p, at, 16); };
    hold(0, {36, 60, 64, 67}); hold(16, {45, 60, 64, 69}); hold(32, {41, 60, 65, 69}); hold(48, {43, 59, 62, 65, 67});
    rig.controller.setClipPattern(0, 0, progression, false);
    std::uint32_t region = 0;
    require(rig.controller.placeRegion(0, 0, 0, &region), "place the progression on the Song");
    require(rig.reg(region).start == 0 && rig.reg(region).length == 4 * bar, "the placed region is as long as its four-bar clip");
    // Chords already on the track: E major inside the clip's span, D minor across its right edge.
    require(rig.controller.editTimeline("Chords", [](tl::Timeline& t) {
        return t.addChord(bar, 2 * bar, 4, alien::ChordType::major) != 0 && t.addChord(3 * bar, 6 * bar, 2, alien::ChordType::minor) != 0;
    }), "put chords on the chord track");
    rig.view.refresh();
    const auto before = rig.t().chords;
    const auto crossing = before[1];

    rig.rightClick(rig.at(bar / 2, 0)); choose("Detect chords into the chord track");
    const auto& chords = rig.t().chords;
    require(chords.size() == 5, "four detected chords and the cut D minor");
    require(chords[0].start == 0 && chords[0].end == bar && chords[0].root == 0 && chords[0].quality == alien::ChordType::major, "bar 1 is C major");
    require(chords[1].start == bar && chords[1].root == 9 && chords[1].quality == alien::ChordType::minor, "bar 2 is A minor, replacing E major");
    require(chords[2].root == 5 && chords[2].quality == alien::ChordType::major, "bar 3 is F major");
    require(chords[3].root == 7 && chords[3].quality == alien::ChordType::dominant7 && chords[3].end == 4 * bar, "bar 4 is G7");
    require(chords[4].id == crossing.id && chords[4].start == 4 * bar && chords[4].end == 6 * bar, "the D minor across the edge is cut there and keeps its id");
    require(!rig.messages.empty() && rig.messages.back() == "Detected 4 chords: C Major, A Minor, F Major, G Dominant 7.", "the status names the chords");

    rig.controller.undo(); rig.view.refresh();
    require(rig.t().chords == before, "one Undo restores the chord track exactly");
    rig.controller.redo(); rig.view.refresh();
    require(rig.t().chords.size() == 5 && rig.t().chords[0].root == 0, "Redo detects them again");

    const auto redo = rig.controller.redoLabel();
    const auto after = rig.t().chords;
    rig.rightClick(rig.at(bar / 2, 0)); choose("Detect chords into the chord track");
    require(rig.t().chords == after && rig.controller.redoLabel() == redo && rig.messages.back().contains("already has these chords"),
            "detecting again changes nothing and keeps redo");

    rig.controller.setExternalBusy(true);
    require(!rig.c.detectChords(region) && rig.t().chords == after, "chord detection waits while the project is busy");
    rig.controller.setExternalBusy(false);

    // An empty clip says why nothing happened.
    rig.controller.setClipPattern(0, 0, PianoRollPattern{}, false);
    rig.view.refresh();
    require(!rig.c.detectChords(region) && rig.messages.back().contains("no notes"), "an empty clip has no chords to detect");
}
