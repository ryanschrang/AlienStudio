#pragma once
// Moving a Song section with the clips inside it. TimelineViewTests drives the section menu item that runs it.
static void songSectionMoveCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    const auto run = [&](const cmd::Command& edit, const char* what) { require(commands.execute(edit).changed, what); };

    // A two-bar Verse at bars 3 and 4 holding a clip on track 1, a two-bar clip on track 2 and a rack pattern that ends
    // exactly at its end; an Intro in bar 1, an empty Outro in bar 9, a chord under the Verse, and three clips outside
    // it: one on track 2 that starts exactly at the Verse's end, one on track 1 in bar 7 and one in bar 1.
    run(cmd::SetSongLength{32}, "fixture: song length");
    run(cmd::PlaceSongClip{0, 0, 2 * bar, bar}, "fixture: clip on track 1");
    run(cmd::PlaceSongClip{1, 0, 2 * bar, 2 * bar}, "fixture: clip on track 2");
    run(cmd::PlaceSongClip{tl::rackRowBase, 0, 3 * bar, bar}, "fixture: rack pattern");
    run(cmd::PlaceSongClip{1, 0, 4 * bar, bar}, "fixture: clip after the section");
    run(cmd::PlaceSongClip{0, 0, 6 * bar, bar}, "fixture: clip outside");
    run(cmd::PlaceSongClip{0, 0, 0, bar}, "fixture: clip in the intro");
    const cmd::AddSongSection intro{0, bar, "Intro", 0}, verse{2 * bar, 4 * bar, "Verse", 1}, outro{8 * bar, 9 * bar, "Outro", 2};
    run(intro, "fixture: Intro"); run(verse, "fixture: Verse"); run(outro, "fixture: Outro");
    const cmd::AddSongChord chord{2 * bar, 3 * bar, 0, alien::ChordType::major};
    run(chord, "fixture: chord");
    c.markAsNewBaseline();

    const auto idAt = [&](int row, std::int64_t start) {
        for (const auto& r : song.regions) if (r.row == row && r.start == start) return r.id;
        return std::uint32_t{};
    };
    const auto a = idAt(0, 2 * bar), b = idAt(1, 2 * bar), p = idAt(tl::rackRowBase, 3 * bar);
    const auto after = idAt(1, 4 * bar), outside = idAt(0, 6 * bar), first = idAt(0, 0);
    require(a != 0 && b != 0 && p != 0 && after != 0 && outside != 0 && first != 0 && song.regions.size() == 6, "fixture clips found");
    require(song.find(a)->group == 0, "fixture clips are not grouped");
    const auto base = c.document();
    const auto was = [&](std::uint32_t id) { return *base.timeline.find(id); };
    // The clip is what it was, `delta` ticks later: same lane, length, source and everything else.
    const auto shifted = [&](std::uint32_t id, std::int64_t delta) {
        auto expected = was(id);
        expected.start += delta;
        const auto* now = song.find(id);
        return now != nullptr && *now == expected;
    };
    const auto sectionAt = [&](std::uint32_t id, std::int64_t start, std::int64_t end) {
        const auto* s = song.findSection(id);
        return s != nullptr && s->start == start && s->end == end;
    };
    const auto restAsBefore = [&] {
        return shifted(after, 0) && shifted(outside, 0) && shifted(first, 0) && song.regions.size() == 6 && song.chords == base.timeline.chords
            && song.markers == base.timeline.markers && song.loopStart == base.timeline.loopStart && song.loopEnd == base.timeline.loopEnd
            && song.lengthBars == base.timeline.lengthBars && sectionAt(intro.section, 0, bar);
    };

    // Every edit: Preview changes nothing live, execute does what Preview said, one named Undo step, and Undo restores it.
    const auto step = [&](const cmd::Command& edit, const std::function<bool()>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(edit);
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Move section and clips" && happened(), what);
        require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && happened(), what);
        require(c.undo() && alien::sameContent(base, c.document()), what);
    };

    // The section and its three clips move together; the clip that starts at its end, the others, the chord and the loop stay.
    step(cmd::MoveSongSection{verse.section, 10 * bar}, [&] {
        return sectionAt(verse.section, 10 * bar, 12 * bar) && shifted(a, 8 * bar) && shifted(b, 8 * bar) && shifted(p, 8 * bar)
            && sectionAt(outro.section, 8 * bar, 9 * bar) && restAsBefore();
    }, "MoveSongSection moves the section and the clips inside it");
    // A short move lands on where the section was, right after the adjacent Intro.
    step(cmd::MoveSongSection{verse.section, bar}, [&] {
        return sectionAt(verse.section, bar, 3 * bar) && shifted(a, -bar) && shifted(b, -bar) && shifted(p, -bar) && restAsBefore();
    }, "a move that overlaps the section's old place");
    // An empty section moves alone.
    step(cmd::MoveSongSection{outro.section, 12 * bar}, [&] {
        return sectionAt(outro.section, 12 * bar, 13 * bar) && shifted(a, 0) && shifted(b, 0) && shifted(p, 0) && restAsBefore();
    }, "an empty section moves on its own");

    // An unchanged start adds no step and keeps Redo.
    {
        require(commands.execute(cmd::MoveSongSection{outro.section, 12 * bar}).changed && c.undo(), "Redo fixture");
        const auto redo = historyOf(c);
        const auto same = commands.execute(cmd::MoveSongSection{verse.section, 2 * bar});
        require(same.ok && !same.changed && historyOf(c) == redo, "a move to where the section is adds no step and keeps Redo");
    }

    // Refusals change nothing and say why.
    {
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto before = c.document();
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed
                    && alien::sameContent(before, c.document()), why);
        };
        says(cmd::MoveSongSection{99999999, bar}, "no section");
        says(cmd::MoveSongSection{verse.section, -1}, "does not fit");
        says(cmd::MoveSongSection{verse.section, song.length() - bar}, "does not fit");
        says(cmd::MoveSongSection{verse.section, bar / 2}, "Another section");
        says(cmd::MoveSongSection{verse.section, 7 * bar}, "Another section");
        says(cmd::MoveSongSection{verse.section, 3 * bar}, "in the way");       // track 2's clip would land on the one after it
        says(cmd::MoveSongSection{verse.section, 6 * bar}, "in the way");       // track 1's clip would land on bar 7's
        require(historyOf(c) == history, "refusals keep the history");

        // A clip that crosses either edge.
        run(cmd::MoveSongClips{{outside}, -3 * bar + bar / 2}, "fixture: a clip across the section's end");
        says(cmd::MoveSongSection{verse.section, 10 * bar}, "crosses");
        require(c.undo(), "undo the crossing fixture");
        run(cmd::MoveSongClips{{first}, bar + bar / 2}, "fixture: a clip across the section's start");
        says(cmd::MoveSongSection{verse.section, 10 * bar}, "crosses");
        require(c.undo(), "undo the crossing fixture");
        // A group that reaches outside the section.
        run(cmd::GroupSongClips{{a, outside}}, "fixture: a group across the section");
        says(cmd::MoveSongSection{verse.section, 10 * bar}, "grouped");
        require(c.undo() && alien::sameContent(base, c.document()), "undo the group fixture");
        // A group wholly inside it moves.
        run(cmd::GroupSongClips{{a, b}}, "fixture: a group inside the section");
        require(commands.execute(cmd::MoveSongSection{verse.section, 10 * bar}).changed && song.find(a)->start == 10 * bar && song.find(a)->group != 0
                && song.find(a)->group == song.find(b)->group, "a group inside the section moves with it");
        require(c.undo() && c.undo() && alien::sameContent(base, c.document()), "undo the inside group");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::MoveSongSection{verse.section, 10 * bar});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && alien::sameContent(base, c.document()), "a busy project refuses it");

        engine->recording.phase.store(alien::RecordControls::countIn);
        const auto recording = commands.execute(cmd::MoveSongSection{verse.section, 10 * bar});
        engine->recording.phase.store(alien::RecordControls::idle);
        require(!recording.ok && recording.error.contains("recording") && alien::sameContent(base, c.document()), "it is refused during a count-in");

        // A frozen track plays its render, so a section holding one of its clips stays (the reducer, on a frozen copy).
        auto frozen = c.document();
        frozen.tracks[1].settings.freezeScene = 0;
        require(alien::song_clip::refusal(frozen, *cmd::songClipEdit(cmd::MoveSongSection{verse.section, 10 * bar})).contains("Unfreeze track 2"), "a frozen track's clips are refused");
        require(alien::song_clip::refusal(frozen, *cmd::songClipEdit(cmd::MoveSongSection{outro.section, 12 * bar})).isEmpty(), "a section with none of them is not");
    }

    // Script form and description.
    juce::String error;
    const cmd::Command command = cmd::MoveSongSection{verse.section, 10 * bar};
    const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
    require(parsed.has_value() && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
            "MoveSongSection round-trips through its script form");
    require(cmd::describe(c.document(), command).contains("\"Verse\"") && cmd::describe(c.document(), command).contains(juce::String(10 * bar)),
            "MoveSongSection describes the section and where it goes");
}
