#pragma once
// NewSongClip (a new, empty clip on a Song lane) and MakeSongClipUnique (a Song clip gets its own copy of what it
// plays). TimelineViewTests drives the real double-click and menu item, which run these commands.
static void songPlacementCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    const auto& d = c.document();

    // A new clip takes a free cell of its track, which is selected.
    const auto before = c.document();
    const cmd::NewSongClip create{1, 2 * bar};
    const auto preview = commands.preview(cmd::Command{create});
    require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && song.regions.empty(), "NewSongClip Preview changes no live state");
    const auto made = commands.execute(create);
    require(made.ok && made.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "New clip", "NewSongClip does what Preview said");
    require(song.regions.size() == 1 && song.regions[0].row == 1 && song.regions[0].start == 2 * bar && song.regions[0].playsCell()
            && d.selectedTrack == 1 && d.selectedScene == song.regions[0].source, "the clip is on the lane at the tick, and its cell is selected");
    const int firstScene = song.regions[0].source;
    const auto again = commands.execute(cmd::NewSongClip{1, 8 * bar});
    require(again.ok && again.changed && song.regions.size() == 2 && song.regions[1].source != firstScene, "a second new clip is a clip of its own");
    require(c.undo() && c.undo() && alien::sameContent(before, c.document()) && c.redo() && song.regions.size() == 1 && c.undo(), "each is one Undo step");
    for (const cmd::Command bad : {cmd::Command{cmd::NewSongClip{-1, 0}}, cmd::Command{cmd::NewSongClip{99, 0}}, cmd::Command{cmd::NewSongClip{tl::rackRowBase, 0}},
                                   cmd::Command{cmd::NewSongClip{0, -1}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok && alien::sameContent(before, c.document()), "a lane or tick that takes no new clip is refused");
    }

    // A MIDI cell placed twice: one placement gets a placement clip, and the other keeps the cell.
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    std::uint32_t a = 0, b = 0;
    require(c.placeRegion(0, 0, 0, &a) && c.placeRegion(0, 0, 8 * bar, &b), "fixture: a cell placed twice");
    const auto shared = c.document();
    const cmd::MakeSongClipUnique unique{a};
    const auto uniquePreview = commands.preview(cmd::Command{unique});
    require(uniquePreview.ok && uniquePreview.changed && alien::sameContent(shared, c.document()), "MakeSongClipUnique Preview changes no live state");
    const auto own = commands.execute(unique);
    require(own.ok && own.changed && alien::sameContent(uniquePreview.after, c.document()) && c.undoLabel() == "Make placement unique", "MakeSongClipUnique does what Preview said");
    require(song.find(a)->ownsClip() && song.find(b)->playsCell() && song.find(b)->source == 0 && !c.placementShared(a) && !c.placementShared(b), "one placement owns a copy, and nothing is shared now");
    const auto twice = commands.execute(unique);
    require(!twice.ok && twice.error.contains("content of its own") && !commands.execute(cmd::MakeSongClipUnique{b}).ok, "a placement that shares nothing is refused");
    const auto gone = commands.execute(cmd::MakeSongClipUnique{9999});
    require(!gone.ok && gone.error.isNotEmpty(), "a Song clip that is not there is refused");
    require(c.undo() && alien::sameContent(shared, c.document()) && c.redo() && song.find(a)->ownsClip() && c.undo(), "it is one Undo step");

    // A pattern placement gets a rack pattern of its own, and the engine's rack follows.
    std::uint32_t p = 0;
    require(c.placeRegion(tl::rackRowBase, 0, 0, &p), "fixture: a pattern on the Song");
    const int patterns = c.channelRack().patternCount;
    const auto source = song.find(p)->rackClip;
    const auto forked = commands.execute(cmd::MakeSongClipUnique{p});
    require(forked.ok && forked.changed && c.channelRack().patternCount == patterns + 1 && song.find(p)->rackClip != source && song.find(p)->source == patterns, "a pattern placement plays a new pattern");
    require(c.undo() && c.channelRack().patternCount == patterns && song.find(p)->rackClip == source, "and Undo takes the pattern away");

    c.setExternalBusy(true);
    const auto busy = commands.execute(unique);
    const auto busyNew = commands.execute(create);
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy") && !busyNew.ok && song.find(a)->playsCell(), "a busy project refuses, and refusals change nothing");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::NewSongClip{2, 3 * bar}}, cmd::Command{cmd::MakeSongClipUnique{7}}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the Song placement commands round-trip through JSON");
    }
    require(cmd::describe(c.document(), cmd::NewSongClip{0, 0}).contains(d.tracks[0].name) && cmd::describe(c.document(), cmd::MakeSongClipUnique{a}).contains(juce::String(a)),
            "the descriptions name the track and the clip");
}
