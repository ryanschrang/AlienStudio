#pragma once
// The Song length. TimelineViewTests drives the real length control; WarpMarkerTests has it refused while warped audio records.
static void songLengthCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    const int start = song.lengthBars;

    // Preview changes nothing live; execute does what Preview said, as one named Undo step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::SetSongLength{start + 8});
        require(preview.ok && preview.changed && preview.after.timeline.lengthBars == start + 8 && historyOf(c) == history && alien::sameContent(before, c.document()),
                "SetSongLength Preview changes no live state");
        require(commands.execute(cmd::SetSongLength{start + 8}).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Change song length",
                "SetSongLength does what Preview said");
        require(c.undo() && song.lengthBars == start && c.redo() && song.lengthBars == start + 8, "it undoes and redoes");
    }

    // A shorter song pulls the loop in with it, and Undo puts the loop back.
    {
        require(commands.execute(cmd::SetSongLoop{bar, 6 * bar, true}).changed, "loop fixture");
        require(commands.execute(cmd::SetSongLength{4}).changed && song.lengthBars == 4 && song.loopEnd == 4 * bar && song.loopStart == bar, "shortening the song shortens the loop");
        require(c.undo() && song.loopEnd == 6 * bar && song.lengthBars == start + 8, "and Undo restores both");
    }

    // Quick repeats under one key are one Undo step, as the length buttons are.
    {
        const auto steps = historyOf(c);
        require(commands.execute(cmd::SetSongLength{start + 9}, "song-length-test").changed && commands.execute(cmd::SetSongLength{start + 10}, "song-length-test").changed,
                "repeated length changes");
        require(c.undo() && song.lengthBars == start + 8 && historyOf(c) == steps, "repeats under one key undo as one step");
    }

    // An unchanged length adds no step and keeps Redo.
    {
        require(commands.execute(cmd::SetSongLength{start + 2}).changed && c.undo(), "Redo fixture");
        const auto redo = historyOf(c);
        const auto same = commands.execute(cmd::SetSongLength{song.lengthBars});
        require(same.ok && !same.changed && historyOf(c) == redo && c.redo() && song.lengthBars == start + 2, "an unchanged length keeps Redo");
    }

    // Refusals change nothing and say why: out of range, or shorter than what the song holds.
    {
        require(commands.execute(cmd::AddSongMarker{5 * bar + 1, "End", 0}).changed, "marker fixture");
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetSongLength{0}, "from 1 to");
        says(cmd::SetSongLength{tl::maxBars + 1}, "from 1 to");
        says(cmd::SetSongLength{5}, "shorter than 6 bars");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
        require(commands.execute(cmd::SetSongLength{6}).changed && song.lengthBars == 6, "the shortest length is allowed");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetSongLength{7});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && song.lengthBars == 6, "a busy project refuses it");
    }

    // With a frozen track, the song remembers the length the render was made for.
    {
        auto d = c.document();
        d.tracks[0].settings.freezeScene = 0;
        const auto length = d.timeline.lengthBars;
        require(cmd::applyTo(d, cmd::SetSongLength{length + 3}).isEmpty() && d.timeline.lengthBars == length + 3 && d.timeline.freezeBaseLengthBars == length + 3,
                "a frozen track's base length follows the new length");
    }

    // Script form.
    juce::String error;
    const auto parsed = cmd::parse("SetSongLength", juce::JSON::parse(R"({"bars": 12})"), error);
    require(parsed && std::get<cmd::SetSongLength>(*parsed).bars == 12 && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(cmd::SetSongLength{12})),
            "SetSongLength round-trips through JSON");
    require(cmd::describe(c.document(), cmd::SetSongLength{12}).contains("12 bars"), "it describes itself");
    error.clear();
    require(!cmd::parse("SetSongLength", juce::JSON::parse(R"({"bars": 1.5})"), error) && error.contains("bars"), "bars is a whole number");
    error.clear();
    require(!cmd::parse("SetSongLength", juce::JSON::parse(R"({})"), error) && error.contains("bars"), "SetSongLength needs its argument");
    while (c.canUndo()) c.undo();
}
