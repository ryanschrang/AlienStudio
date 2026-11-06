#pragma once
// The Channel Rack's step, pattern and swing commands. ChannelRackViewTests drives the real grid, graph and pattern menu.
static void rackPatternCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "pattern command fixture");
    const int kick = rack.channelCount - 1, pattern = rack.selectedPattern;
    const auto step = [&](int s) { return rack.pattern(pattern).steps[static_cast<std::size_t>(kick)][static_cast<std::size_t>(s)]; };

    // A step: Preview touches nothing, execute does what it said, Undo and Redo are one step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::SetRackStep{pattern, kick, 3, true});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history && !step(3).on, "SetRackStep Preview changes no live state");
        require(commands.execute(cmd::SetRackStep{pattern, kick, 3, true}).changed && step(3).on && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Edit step", "SetRackStep does what Preview said");
        require(c.undo() && !step(3).on && c.redo() && step(3).on, "SetRackStep undoes and redoes");
        const auto redo = historyOf(c);
        require(commands.execute(cmd::SetRackStep{pattern, kick, 3, true}).ok && historyOf(c) == redo, "an unchanged step preserves history");
        require(commands.execute(cmd::SetRackStepValue{pattern, kick, 3, "velocity", 40}).changed && step(3).velocity == 40 && c.undoLabel() == "Edit step", "a step value");
        require(step(3).on, "and setting a value leaves the step on");
        require(commands.execute(cmd::SetRackStep{pattern, kick, 3, false}).changed && step(3).velocity == 40, "turning a step off keeps its values");
        require(commands.execute(cmd::SetRackStep{pattern, kick, 3, true}).changed && step(3).velocity == 40, "and turning it on finds them again");
    }

    // A paint stroke merges under its key into one undo step; a batch is one step.
    {
        const auto first = commands.batch({cmd::SetRackStep{pattern, kick, 5, true}}, "Add steps", "stroke");
        const auto second = commands.batch({cmd::SetRackStep{pattern, kick, 6, true}}, "Add steps", "stroke");
        require(first.changed && second.changed && c.undoLabel() == "Add steps", "a stroke paints");
        require(c.undo() && !step(5).on && !step(6).on, "and one Undo takes back the whole stroke");
        require(commands.batch({cmd::SetRackStep{pattern, kick, 5, true}, cmd::SetRackStep{pattern, kick, 6, true}}, "Two steps").changed && c.undo() && !step(5).on && !step(6).on, "a batch is one step");
        const auto history = historyOf(c);
        require(!commands.batch({cmd::SetRackStep{pattern, kick, 5, true}, cmd::SetRackStep{pattern, kick, 999, true}}, "Bad").ok && !step(5).on && historyOf(c) == history, "a refused batch changes nothing");
    }

    // Row edits work on the named pattern, not whichever is selected.
    {
        require(commands.execute(cmd::ClearRackSteps{pattern, kick}).changed && !step(3).on && c.undoLabel() == "Clear channel", "clear a channel's steps");
        require(commands.execute(cmd::FillRackSteps{pattern, kick, 4}).changed && step(0).on && step(4).on && !step(1).on && c.undoLabel() == "Edit channel steps", "fill");
        require(commands.execute(cmd::RotateRackSteps{pattern, kick, 1}).changed && step(1).on && !step(0).on, "rotate");
        require(commands.execute(cmd::ReverseRackSteps{pattern, kick}).changed, "reverse");
        require(commands.execute(cmd::ClearRackSteps{pattern, kick}).changed && commands.execute(cmd::SetRackStep{pattern, kick, 1, true}).changed, "repeat fixture");
        require(commands.execute(cmd::RepeatRackSteps{pattern, kick, 4}).changed && step(5).on && step(9).on, "repeat");
        const auto history = historyOf(c);
        require(commands.execute(cmd::RepeatRackSteps{pattern, kick, 999}).ok && historyOf(c) == history, "repeating over the whole pattern or more changes nothing");
    }

    // A note channel has no steps to edit: a quiet no-op, so a mixed selection still runs.
    {
        require(commands.execute(cmd::AddRackSynthChannel{0}).changed, "note channel fixture");
        const int synth = rack.channelCount - 1;
        const auto history = historyOf(c);
        require(commands.execute(cmd::SetRackStep{pattern, synth, 0, true}).ok && commands.execute(cmd::FillRackSteps{pattern, synth, 4}).ok && historyOf(c) == history, "a note channel's steps do not change");
        require(c.undo(), "note channel fixture undoes");
    }

    // Length, swing and naming.
    {
        require(commands.execute(cmd::SetRackPatternLength{pattern, 32}).changed && rack.pattern(pattern).length == 32 && c.undoLabel() == "Change pattern length", "length");
        require(c.undo() && rack.pattern(pattern).length == 16 && c.redo() && rack.pattern(pattern).length == 32, "length undoes and redoes");
        require(commands.execute(cmd::SetRackPatternLength{pattern, 16}).changed, "length back");
        const auto swing = commands.preview(cmd::SetRackSwing{.5});
        require(swing.ok && swing.after.channelRack.swing == .5f && rack.swing != .5f, "swing Preview changes no live state");
        require(commands.batch({cmd::SetRackSwing{.25}}, "Change swing", "swing-drag").changed && commands.batch({cmd::SetRackSwing{.5}}, "Change swing", "swing-drag").changed && rack.swing == .5f, "swing drag");
        require(c.undoLabel() == "Change swing" && c.undo() && rack.swing != .25f && rack.swing != .5f, "and one Undo takes it all back");
        require(commands.execute(cmd::RenameRackPattern{pattern, "Verse"}).changed && rack.patternNames[static_cast<std::size_t>(pattern)] == "Verse" && c.undoLabel() == "Rename pattern", "rename");
    }

    // Cloning selects the copy and gives it its own clip; clearing and deleting name their pattern.
    {
        const auto preview = commands.preview(cmd::CloneRackPattern{pattern});
        const int count = rack.patternCount;
        require(preview.ok && preview.after.channelRack.patternCount == count + 1 && rack.patternCount == count && rack.selectedPattern == pattern, "clone Preview changes no live state");
        require(commands.execute(cmd::CloneRackPattern{pattern}).changed && alien::sameContent(preview.after, c.document()) && rack.selectedPattern == count && c.undoLabel() == "Clone pattern", "clone");
        const auto copy = rack.clips[static_cast<std::size_t>(count)];
        require(copy != 0 && copy != rack.clips[static_cast<std::size_t>(pattern)], "the clone is an independent clip");
        require(commands.execute(cmd::ClearRackPattern{count}).changed && rack.pattern(count).empty() && !rack.pattern(pattern).empty() && c.undoLabel() == "Clear pattern", "clearing a named pattern leaves the others");
        require(commands.execute(cmd::DeleteRackPattern{count}).changed && rack.patternCount == count && c.undoLabel() == "Delete pattern", "delete");
        require(c.undo() && rack.patternCount == count + 1 && c.redo() && rack.patternCount == count, "delete undoes and redoes");
        // One in use is refused, and the refusal says why.
        require(commands.execute(cmd::CreateRackPattern{}).changed, "used pattern fixture");
        const int used = rack.patternCount - 1;
        require(commands.execute(cmd::AssignRackClip{0, 0, rack.clips[static_cast<std::size_t>(used)]}).changed, "used pattern is assigned");
        const auto history = historyOf(c);
        const auto refused = commands.execute(cmd::DeleteRackPattern{used});
        require(!refused.ok && refused.error.contains("Session") && rack.patternCount == used + 1 && historyOf(c) == history, "a pattern in use cannot be deleted");
    }

    // Refusals.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetRackStep{99, kick, 0, true}, "no rack pattern");
        says(cmd::SetRackStep{pattern, 99, 0, true}, "no rack channel");
        says(cmd::SetRackStep{pattern, kick, 999, true}, "no step");
        says(cmd::SetRackStepValue{pattern, kick, 0, "colour", 1}, "no \"colour\"");
        says(cmd::SetRackStepValue{pattern, kick, 0, "velocity", 9999}, "runs from");
        says(cmd::FillRackSteps{pattern, kick, 3}, "every 2, 4 or 8");
        says(cmd::RepeatRackSteps{pattern, kick, 0}, "at least 1");
        says(cmd::SetRackPatternLength{pattern, 17}, "16, 32, 64 or 128");
        says(cmd::SetRackSwing{1.5}, "from 0 to 1");
        says(cmd::SetRackSwing{std::numeric_limits<double>::quiet_NaN()}, "finite");
        says(cmd::RenameRackPattern{pattern, "   "}, "blank");
        says(cmd::CloneRackPattern{99}, "no rack pattern");
        says(cmd::DeleteRackPattern{99}, "no rack pattern");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
    }

    // A pattern must keep every note it holds, so a shortening that would cut one is refused.
    {
        require(commands.execute(cmd::AddRackSynthChannel{0}).changed && commands.execute(cmd::SetRackPatternLength{pattern, 32}).changed, "long note fixture");
        const int synth = rack.channelCount - 1;
        alien::PianoRollPattern notes;
        notes.loopSteps = 32;
        notes.add(60, 20.0, 4.0);
        require(c.setRackChannelNotes(pattern, synth, notes), "a note past step 16");
        const auto cut = commands.execute(cmd::SetRackPatternLength{pattern, 16});
        require(!cut.ok && cut.error.contains("shorten notes") && rack.pattern(pattern).length == 32, "a shortening that would cut a note is refused");
    }

    // Busy.
    {
        const auto history = historyOf(c);
        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetRackSwing{.75});
        c.setExternalBusy(false);
        require(!busy.ok && historyOf(c) == history && rack.swing != .75f, "a busy project refuses them");
    }

    // The script form.
    juce::String error;
    for (const cmd::Command edit : std::vector<cmd::Command>{cmd::SetRackStep{0, 1, 2, true}, cmd::SetRackStepValue{0, 1, 2, "pan", -5}, cmd::FillRackSteps{0, 1, 4}, cmd::RotateRackSteps{0, 1, -1}, cmd::ReverseRackSteps{0, 1},
                                                             cmd::RepeatRackSteps{0, 1, 8}, cmd::ClearRackSteps{0, 1}, cmd::SetRackPatternLength{0, 32}, cmd::SetRackSwing{.5}, cmd::CloneRackPattern{0},
                                                             cmd::ClearRackPattern{0}, cmd::RenameRackPattern{0, "Name"}, cmd::DeleteRackPattern{0}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && cmd::name(*parsed) == cmd::name(edit) && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "rack pattern commands round-trip their script form");
    }
    for (const auto& [commandName, json] : std::vector<std::pair<const char*, const char*>>{
             {"SetRackStep", "{\"pattern\":0,\"channel\":0,\"step\":0}"}, {"SetRackStepValue", "{\"pattern\":0,\"channel\":0,\"step\":0,\"field\":1,\"value\":1}"}, {"FillRackSteps", "{\"pattern\":0,\"channel\":0}"},
             {"RotateRackSteps", "{\"pattern\":0,\"channel\":0,\"distance\":1.5}"}, {"ReverseRackSteps", "{\"pattern\":0}"}, {"RepeatRackSteps", "{\"pattern\":0,\"channel\":0,\"period\":\"4\"}"},
             {"ClearRackSteps", "{\"channel\":0}"}, {"SetRackPatternLength", "{\"pattern\":0}"}, {"SetRackSwing", "{\"amount\":true}"}, {"CloneRackPattern", "{}"}, {"ClearRackPattern", "{\"pattern\":0,\"extra\":1}"},
             {"RenameRackPattern", "{\"pattern\":0}"}, {"DeleteRackPattern", "{\"pattern\":\"0\"}"}})
        require(!cmd::parse(commandName, juce::JSON::parse(json), error), "rack pattern parsing rejects missing, mistyped and extra arguments");
}
