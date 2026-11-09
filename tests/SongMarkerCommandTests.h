#pragma once
// Song markers and the loop range. TimelineViewTests drives the real marker strip, loop strip, keys and menus;
// WarpMarkerTests has the loop refused while warped audio records.
static void songMarkerCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto bar = alien::timeline::ticksPerBar;
    const auto& song = c.timeline();
    const auto markerAt = [&](std::uint32_t id) { return song.findMarker(id); };

    // AddSongMarker: Preview agrees with execute because the command carries its marker's id.
    const cmd::AddSongMarker verse{bar, "  Verse  ", 3};
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(verse);
        require(preview.ok && preview.changed && song.markers.empty() && historyOf(c) == history && alien::sameContent(before, c.document()), "AddSongMarker Preview changes no live state");
        require(preview.after.timeline.findMarker(verse.marker) != nullptr, "Preview shows the marker under the id the command carries");
        require(commands.execute(verse).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Add marker", "AddSongMarker does what Preview said");
        const auto* m = markerAt(verse.marker);
        require(m != nullptr && m->tick == bar && m->name == "Verse" && m->colour == 3, "the marker is stored trimmed, where and as asked");
        const auto again = commands.execute(verse);
        require(!again.ok && again.error.contains("already") && song.markers.size() == 1, "the same marker cannot be added twice");
        require(c.undo() && song.markers.empty() && c.redo() && markerAt(verse.marker) != nullptr, "and it undoes and redoes with its id");
    }

    // Move, rename, colour and remove name the marker by id; each is one Undo step and an unchanged edit keeps Redo.
    {
        require(commands.execute(cmd::MoveSongMarker{verse.marker, 2 * bar}).changed && markerAt(verse.marker)->tick == 2 * bar && c.undoLabel() == "Move marker", "MoveSongMarker");
        require(commands.execute(cmd::RenameSongMarker{verse.marker, "Chorus"}).changed && markerAt(verse.marker)->name == "Chorus" && c.undoLabel() == "Rename marker", "RenameSongMarker");
        require(commands.execute(cmd::ColourSongMarker{verse.marker, 6}).changed && markerAt(verse.marker)->colour == 6 && c.undoLabel() == "Colour marker", "ColourSongMarker");
        require(c.undo() && markerAt(verse.marker)->colour == 3, "colour undoes");
        const auto redo = historyOf(c);
        for (const auto& same : std::vector<cmd::Command>{cmd::MoveSongMarker{verse.marker, 2 * bar}, cmd::RenameSongMarker{verse.marker, " Chorus "}, cmd::ColourSongMarker{verse.marker, 3}}) {
            const auto r = commands.execute(same);
            require(r.ok && !r.changed && historyOf(c) == redo, "an unchanged marker edit adds no step and keeps Redo");
        }
        require(c.redo() && markerAt(verse.marker)->colour == 6, "so Redo still works");
        const auto removed = commands.preview(cmd::RemoveSongMarker{verse.marker});
        require(removed.ok && removed.changed && markerAt(verse.marker) != nullptr && removed.after.timeline.findMarker(verse.marker) == nullptr, "RemoveSongMarker Preview changes no live state");
        require(commands.execute(cmd::RemoveSongMarker{verse.marker}).changed && song.markers.empty() && c.undoLabel() == "Delete marker", "RemoveSongMarker");
        require(c.undo() && markerAt(verse.marker) != nullptr && markerAt(verse.marker)->name == "Chorus", "and Undo brings the marker back as it was");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::AddSongMarker{-1, "Early", 0}, "inside the song");
        says(cmd::AddSongMarker{song.length() + 1, "Late", 0}, "inside the song");
        says(cmd::AddSongMarker{0, "   ", 0}, "needs a name");
        says(cmd::AddSongMarker{0, "Odd", 8}, "colour");
        says(cmd::AddSongMarker{0, "Zero", 0, 0}, "already");
        says(cmd::MoveSongMarker{99999999, 0}, "no marker");
        says(cmd::MoveSongMarker{verse.marker, song.length() + 1}, "inside the song");
        says(cmd::RenameSongMarker{verse.marker, ""}, "needs a name");
        says(cmd::ColourSongMarker{verse.marker, -1}, "colour");
        says(cmd::RemoveSongMarker{99999999}, "no marker");
        says(cmd::SetSongLoop{-1, bar, true}, "inside the song");
        says(cmd::SetSongLoop{0, song.length() + 1, true}, "inside the song");
        says(cmd::SetSongLoop{bar, bar, true}, "one step");
        says(cmd::SetSongLoop{2 * bar, bar, true}, "one step");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::AddSongMarker{0, "Busy", 0});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && historyOf(c) == history, "a busy project refuses them");
    }

    // The marker limit.
    {
        c.markAsNewBaseline();
        while (song.markers.size() < alien::timeline::maxMarkers) require(commands.execute(cmd::AddSongMarker{0, "M", 0}).changed, "markers up to the limit");
        const auto full = commands.execute(cmd::AddSongMarker{0, "One more", 0});
        require(!full.ok && full.error.contains("most") && song.markers.size() == alien::timeline::maxMarkers, "past the limit a marker is refused");
        while (c.canUndo()) c.undo();
        require(song.markers.size() == 1, "marker limit fixture undoes");
    }

    // SetSongLoop: range and on/off in one step, heard by the engine when it commits.
    {
        c.markAsNewBaseline();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::SetSongLoop{bar, 3 * bar, true});
        require(preview.ok && preview.changed && !song.loopEnabled && historyOf(c) == history && preview.after.timeline.loopStart == bar && preview.after.timeline.loopEnabled,
            "SetSongLoop Preview changes no live state");
        require(commands.execute(cmd::SetSongLoop{bar, 3 * bar, true}).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Set loop",
            "SetSongLoop does what Preview said");
        engine->arrangementMailbox.consume();
        const auto* heard = engine->arrangementMailbox.current();
        require(heard != nullptr && heard->timeline.loopEnabled && heard->timeline.loopStart == bar && heard->timeline.loopEnd == 3 * bar, "the engine plays the new loop");
        require(commands.batch({cmd::SetSongLoop{bar, 3 * bar, false}}, "Loop off").changed && !song.loopEnabled && c.undoLabel() == "Loop off", "a caller can name the step, as the Loop button does");
        const auto redo = historyOf(c);
        require(commands.execute(cmd::SetSongLoop{bar, 3 * bar, false}).ok && historyOf(c) == redo, "an unchanged loop keeps Redo");
        require(c.undo() && song.loopEnabled && c.undo() && !song.loopEnabled && song.loopStart == 0, "and each loop step undoes");
    }

    // Script form: marker ids and ticks round-trip; a new marker gets a fresh id when it is read.
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{cmd::AddSongMarker{960, "Intro", 2}, cmd::MoveSongMarker{7, 1920}, cmd::RenameSongMarker{7, "Bridge"},
                                                         cmd::ColourSongMarker{7, 4}, cmd::RemoveSongMarker{7}, cmd::SetSongLoop{0, 3840, false}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed.has_value() && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
            "Song marker and loop commands round-trip through JSON");
    }
    const auto first = cmd::parse("AddSongMarker", juce::JSON::parse(R"({"tick": 0, "name": "A", "colour": 0})"), error);
    const auto second = cmd::parse("AddSongMarker", juce::JSON::parse(R"({"tick": 0, "name": "A", "colour": 0})"), error);
    require(first && second && std::get<cmd::AddSongMarker>(*first).marker != std::get<cmd::AddSongMarker>(*second).marker, "each read AddSongMarker carries its own id");
    for (const auto* bad : {R"({"marker": 0, "tick": 0})", R"({"marker": -3, "tick": 0})", R"({"marker": 4294967296, "tick": 0})"}) {
        error.clear();
        require(!cmd::parse("MoveSongMarker", juce::JSON::parse(bad), error) && error.contains("positive"), "a marker id must be positive and fit 32 bits");
    }
    error.clear();
    require(!cmd::parse("SetSongLoop", juce::JSON::parse(R"({"start": 0, "end": 960})"), error) && error.contains("enabled"), "SetSongLoop needs all of its arguments");
}
