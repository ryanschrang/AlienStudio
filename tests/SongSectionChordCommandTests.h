#pragma once
// Song sections and chords. TimelineViewTests drives the real section and chord strips and their menus.
static void songSectionChordCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    require(song.length() >= 4 * bar, "the default song is at least four bars long");

    // Every edit: Preview changes nothing live, execute does what Preview said, one named Undo step, and Undo restores it.
    const auto step = [&](const cmd::Command& edit, const char* label, const std::function<bool()>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(edit);
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == label && happened(), what);
        require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && happened(), what);
    };

    // Sections. An added section carries its id, so Preview and execute agree.
    const cmd::AddSongSection verse{0, bar, "  Verse  ", 2};
    step(verse, "Add Song section", [&] { const auto* s = song.findSection(verse.section); return s && s->name == "Verse" && s->end == bar && s->colour == 2; }, "AddSongSection");
    step(cmd::EditSongSection{verse.section, bar, 2 * bar, "Chorus", 5}, "Edit Song section",
         [&] { const auto* s = song.findSection(verse.section); return s && s->start == bar && s->end == 2 * bar && s->name == "Chorus" && s->colour == 5; }, "EditSongSection");
    const cmd::AddSongSection intro{0, bar, "Intro", 0};
    step(intro, "Add Song section", [&] { return song.sections.size() == 2 && song.sections.front().id == intro.section; }, "a second section keeps the order");
    step(cmd::RemoveSongSection{intro.section}, "Delete Song section", [&] { return song.findSection(intro.section) == nullptr && song.sections.size() == 1; }, "RemoveSongSection");

    // Chords, the same way.
    const cmd::AddSongChord cMajor{0, bar, 0, alien::ChordType::major};
    step(cMajor, "Add Song chord", [&] { const auto* ch = song.findChord(cMajor.chord); return ch && ch->root == 0 && ch->quality == alien::ChordType::major; }, "AddSongChord");
    step(cmd::EditSongChord{cMajor.chord, 0, 2 * bar, 9, alien::ChordType::minor}, "Edit Song chord",
         [&] { const auto* ch = song.findChord(cMajor.chord); return ch && ch->root == 9 && ch->quality == alien::ChordType::minor && ch->end == 2 * bar; }, "EditSongChord");
    const cmd::AddSongChord g{2 * bar, 3 * bar, 7, alien::ChordType::major};
    step(g, "Add Song chord", [&] { return song.chords.size() == 2; }, "a second chord");
    step(cmd::RemoveSongChord{g.chord}, "Delete Song chord", [&] { return song.findChord(g.chord) == nullptr && song.chords.size() == 1; }, "RemoveSongChord");

    // The Loop menu items reuse SetSongLoop under their own names.
    require(commands.batch({cmd::SetSongLoop{bar, 2 * bar, true}}, "Loop Song section").changed && c.undoLabel() == "Loop Song section" && song.loopEnabled
            && song.loopStart == bar, "Loop Song section is a named SetSongLoop");
    require(c.undo(), "loop fixture undoes");

    // Unchanged edits add no step and keep Redo.
    {
        require(commands.execute(cmd::EditSongSection{verse.section, bar, 2 * bar, "Bridge", 5}).changed && c.undo(), "Redo fixture");
        const auto redo = historyOf(c);
        for (const auto& same : std::vector<cmd::Command>{cmd::EditSongSection{verse.section, bar, 2 * bar, " Chorus ", 5}, cmd::EditSongChord{cMajor.chord, 0, 2 * bar, 9, alien::ChordType::minor}}) {
            const auto r = commands.execute(same);
            require(r.ok && !r.changed && historyOf(c) == redo, "an unchanged section or chord edit adds no step and keeps Redo");
        }
        require(c.redo() && song.findSection(verse.section)->name == "Bridge" && c.undo(), "so Redo still works");
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
        says(cmd::AddSongSection{-1, bar, "Early", 0}, "inside the song");
        says(cmd::AddSongSection{0, song.length() + 1, "Long", 0}, "inside the song");
        says(cmd::AddSongSection{0, 1, "Short", 0}, "one step");
        says(cmd::AddSongSection{0, bar, "   ", 0}, "needs a name");
        says(cmd::AddSongSection{0, bar, "Odd", 8}, "colour");
        says(cmd::AddSongSection{bar + bar / 2, 3 * bar, "Over", 0}, "overlap");
        says(cmd::AddSongSection{0, bar, "Taken", 0, verse.section}, "already");
        says(cmd::AddSongSection{0, bar, "Zero", 0, 0}, "already");
        says(cmd::EditSongSection{99999999, 0, bar, "Gone", 0}, "no section");
        says(cmd::EditSongSection{verse.section, bar, 2 * bar, "", 0}, "needs a name");
        says(cmd::RemoveSongSection{99999999}, "no section");
        says(cmd::AddSongChord{bar, 3 * bar, 0, alien::ChordType::major}, "overlap");
        says(cmd::AddSongChord{2 * bar, 3 * bar, 12, alien::ChordType::major}, "root");
        says(cmd::AddSongChord{2 * bar, 3 * bar, 0, static_cast<alien::ChordType>(9)}, "quality");
        says(cmd::AddSongChord{2 * bar, 2 * bar, 0, alien::ChordType::major}, "one step");
        says(cmd::AddSongChord{2 * bar, 3 * bar, 0, alien::ChordType::major, cMajor.chord}, "already");
        says(cmd::EditSongChord{99999999, 0, bar, 0, alien::ChordType::major}, "no chord");
        says(cmd::EditSongChord{cMajor.chord, 0, bar, -1, alien::ChordType::major}, "root");
        says(cmd::RemoveSongChord{99999999}, "no chord");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::RemoveSongSection{verse.section});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && historyOf(c) == history, "a busy project refuses them");
    }

    // The section limit.
    {
        c.markAsNewBaseline();
        require(song.length() >= 2 * bar + static_cast<std::int64_t>(tl::maxSections) * tl::ticksPerStep, "room for every section");
        while (song.sections.size() < tl::maxSections) {
            const auto at = static_cast<std::int64_t>(song.sections.size()) * tl::ticksPerStep + 2 * bar;
            require(commands.execute(cmd::AddSongSection{at, at + tl::ticksPerStep, "S", 0}).changed, "sections up to the limit");
        }
        const auto full = commands.execute(cmd::AddSongSection{0, tl::ticksPerStep, "One more", 0});
        require(!full.ok && full.error.contains("most"), "past the limit a section is refused");
        while (c.canUndo()) c.undo();
        require(song.sections.size() == 1, "section limit fixture undoes");
    }

    // Script form: ids, ranges, roots and qualities round-trip; an added section or chord gets a fresh id when it is read.
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{cmd::AddSongSection{0, 3840, "Intro", 3}, cmd::EditSongSection{7, 960, 1920, "Outro", 1}, cmd::RemoveSongSection{7},
                                                         cmd::AddSongChord{0, 960, 4, alien::ChordType::minor}, cmd::EditSongChord{7, 0, 1920, 11, alien::ChordType::diminished}, cmd::RemoveSongChord{7}}) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed.has_value() && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
            "Song section and chord commands round-trip through JSON");
    }
    const auto first = cmd::parse("AddSongChord", juce::JSON::parse(R"({"start": 0, "end": 960, "root": 0, "quality": 0})"), error);
    const auto second = cmd::parse("AddSongChord", juce::JSON::parse(R"({"start": 0, "end": 960, "root": 0, "quality": 0})"), error);
    require(first && second && std::get<cmd::AddSongChord>(*first).chord != std::get<cmd::AddSongChord>(*second).chord, "each read AddSongChord carries its own id");
    error.clear();
    require(!cmd::parse("AddSongChord", juce::JSON::parse(R"({"start": 0, "end": 960, "root": 0, "quality": 9})"), error) && error.contains("quality"), "a chord quality is 0 to 8");
    error.clear();
    require(!cmd::parse("RemoveSongSection", juce::JSON::parse(R"({"section": 0})"), error) && error.contains("positive"), "a section id is positive");
    error.clear();
    require(!cmd::parse("EditSongSection", juce::JSON::parse(R"({"section": 3, "start": 0, "end": 960, "name": "A"})"), error) && error.contains("colour"), "EditSongSection needs all of its arguments");
}
