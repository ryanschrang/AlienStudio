#pragma once
// SetClipNotes and SetPlacementClipNotes: the piano roll's note edits to a Session MIDI clip and to a Song placement
// clip. MainComponent runs one per piano-roll change under the gesture's coalescing key; these checks run them the
// same way.
static void clipNotesCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& d = c.document();
    const auto notes = [](int pitch) { return std::make_shared<const alien::PianoRollPattern>(patternWith(pitch)); };
    const auto pitchAt = [&](int track, int scene) { return d.clip(track, scene).notes().count > 0 ? d.clip(track, scene).notes().notes[0].pitch : -1; };
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    require(c.copyCell(0, 0) && c.pasteCell(1, 0, true), "a linked copy of the clip");
    c.markAsNewBaseline();

    // One edit: Preview changes nothing live, execute does what Preview said under "Edit notes", and Undo and Redo work.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const cmd::SetClipNotes edit{0, 0, notes(64)};
        const auto preview = commands.preview(cmd::Command{edit});
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()) && pitchAt(0, 0) == 60,
                "SetClipNotes Preview changes no live state");
        const auto result = commands.batch({edit}, "Edit notes", "roll-notes-1");
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Edit notes" && pitchAt(0, 0) == 64,
                "SetClipNotes does what Preview said");
        require(pitchAt(1, 0) == 64, "a linked cell hears the new notes too");
        require(engine->requestedPianoRollClip[0].load() == 0, "the edited clip is queued to play, so the edit is heard");
        require(c.undo() && alien::sameContent(before, c.document()) && pitchAt(0, 0) == 60 && pitchAt(1, 0) == 60, "Undo puts the notes back");
        require(c.redo() && pitchAt(0, 0) == 64, "Redo does it again");
        require(c.undo(), "back to the baseline");
    }

    // A gesture: every change runs under one key, so the whole gesture is one Undo step; the next gesture is another.
    {
        const auto before = c.document();
        for (const int pitch : {61, 62, 63})
            require(commands.batch({cmd::SetClipNotes{0, 0, notes(pitch)}}, "Edit notes", "roll-notes-7").changed, "a change in the gesture");
        require(pitchAt(0, 0) == 63, "the gesture's last change is what the clip holds");
        require(commands.batch({cmd::SetClipNotes{0, 0, notes(65)}}, "Edit notes", "roll-notes-8").changed && pitchAt(0, 0) == 65, "the next gesture");
        require(c.undo() && pitchAt(0, 0) == 63, "the next gesture is a step of its own");
        require(c.undo() && alien::sameContent(before, c.document()), "a gesture of three changes is one Undo step");
    }

    // Unchanged notes change nothing and make no Undo step.
    {
        const auto history = historyOf(c);
        const auto same = commands.execute(cmd::SetClipNotes{0, 0, notes(60)});
        require(same.ok && !same.changed && historyOf(c) == history, "the notes the clip already has are no edit");
    }

    // Several clips edited together (multi-clip editing) are one batch and one Undo step.
    {
        c.setClipPattern(2, 0, patternWith(48), false); c.commit("Notes", 2, 0);
        const auto before = c.document();
        const auto both = commands.batch({cmd::SetClipNotes{0, 0, notes(67)}, cmd::SetClipNotes{2, 0, notes(50)}}, "Edit notes", "roll-notes-9");
        require(both.ok && both.changed && pitchAt(0, 0) == 67 && pitchAt(2, 0) == 50, "a multi-clip edit changes every clip");
        require(c.undo() && alien::sameContent(before, c.document()), "and is one Undo step");
    }

    // Taking every note out of a clip clears its cell, as the piano roll always has.
    {
        const auto blank = std::make_shared<const alien::PianoRollPattern>();
        require(commands.execute(cmd::SetClipNotes{2, 0, blank}).changed && d.clip(2, 0).isEmpty(), "no notes at the blank length clears the cell");
        require(c.undo() && pitchAt(2, 0) == 48, "Undo brings the clip back");
    }

    // A Song placement clip: every Song clip playing it hears the new notes.
    {
        std::uint32_t a = 0, b = 0;
        c.setClipPattern(2, 1, patternWith(55), false); c.commit("Notes", 2, 1);
        require(c.placeRegion(2, 1, 0, &a) && c.placeRegion(2, 1, 8 * alien::timeline::ticksPerBar, &b) && c.makePlacementUnique(a), "a placement clip");
        require(commands.execute(cmd::DuplicateSongClips{{a}}).changed, "a second Song clip playing it");
        const auto placement = d.timeline.find(a)->clip;
        std::vector<std::uint32_t> playing;
        for (const auto& r : d.timeline.regions) if (r.clip == placement) playing.push_back(r.id);
        require(playing.size() == 2, "two Song clips play the placement clip");
        const auto before = c.document();
        const cmd::SetPlacementClipNotes edit{placement, notes(70)};
        const auto preview = commands.preview(cmd::Command{edit});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()), "SetPlacementClipNotes Preview changes no live state");
        require(commands.batch({edit}, "Edit notes", "roll-notes-10").changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Edit notes",
                "SetPlacementClipNotes does what Preview said");
        for (const auto id : playing) require(d.placementContent(*d.timeline.find(id)).notes().notes[0].pitch == 70, "every Song clip playing it hears the notes");
        require(pitchAt(2, 1) == 55, "the Session cell it came from keeps its notes");
        require(c.undo() && alien::sameContent(before, c.document()), "Undo puts the placement clip's notes back");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& command, const char* why) {
            const auto preview = commands.preview(command);
            const auto result = commands.execute(command);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetClipNotes{0, 0, nullptr}, "not notes");
        says(cmd::SetClipNotes{99, 0, notes(60)}, "no Session cell");
        says(cmd::SetClipNotes{0, 99, notes(60)}, "no Session cell");
        says(cmd::SetPlacementClipNotes{0x7777, notes(60)}, "No Song clip plays");
        says(cmd::SetPlacementClipNotes{0x7777, nullptr}, "not notes");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        auto frozen = c.document();
        frozen.tracks[0].settings.freezeScene = 0;
        require(cmd::applyTo(frozen, cmd::SetClipNotes{0, 0, notes(72)}).contains("Unfreeze track 1"), "a frozen track's clip takes no note edit");
        require(cmd::applyTo(frozen, cmd::SetClipNotes{2, 0, notes(72)}).isEmpty(), "another track's clip still does");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetClipNotes{0, 0, notes(72)});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && pitchAt(0, 0) == 60, "a busy project refuses a note edit");

        require(c.assignRackClip(1, 1, c.channelRack().clips[0]), "a cell holding a rack clip");
        const auto rackBefore = c.document();
        says(cmd::SetClipNotes{1, 1, notes(60)}, "Channel Rack");
        require(alien::sameContent(rackBefore, c.document()), "the rack cell is left alone");
    }

    // No script form: the notes exist only as data.
    juce::String error;
    for (const auto* name : {"SetClipNotes", "SetPlacementClipNotes"}) {
        error.clear();
        require(!cmd::parse(name, juce::JSON::parse(R"({"track": 0, "scene": 0})"), error) && error.contains("cannot be written"), "note sequences have no script form");
    }
    require(cmd::describe(c.document(), cmd::SetClipNotes{0, 1, notes(60)}) == "Set the notes of the clip on track 1, scene 2 (1 note)", "SetClipNotes describes itself");
    require(cmd::describe(c.document(), cmd::SetPlacementClipNotes{9, notes(60)}).contains("placement clip 9"), "SetPlacementClipNotes describes itself");
    require(cmd::label(cmd::Command{cmd::SetClipNotes{}}) == "Edit notes" && cmd::label(cmd::Command{cmd::SetPlacementClipNotes{}}) == "Edit notes",
            "both keep the piano roll's Undo name");
}
