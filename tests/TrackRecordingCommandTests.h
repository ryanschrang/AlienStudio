#pragma once
// SetTrackArmed, SetTrackInput and SetTrackMonitor: a track's recording setup (the Song track header).
static void trackRecordingCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& settings = c.document().tracks[0].settings;
    const auto& live = engine->tracks[0];
    const auto before = c.document();
    require(!settings.armed && settings.input == 0 && !settings.stereoInput && settings.monitor == 0, "fixture: a new track is not armed, on input 1, monitoring automatically");

    const cmd::SetTrackArmed arm{0, true};
    const auto preview = commands.preview(cmd::Command{arm});
    require(preview.ok && preview.changed && preview.after.tracks[0].settings.armed && alien::sameContent(before, c.document()) && !live.armed.load(), "SetTrackArmed Preview changes no live state");
    const auto armed = commands.execute(arm);
    require(armed.ok && armed.changed && alien::sameContent(preview.after, c.document()) && settings.armed && live.armed.load() && c.undoLabel() == "Arm track",
            "SetTrackArmed does what Preview said, and the engine hears it");
    require(!commands.execute(arm).changed, "arming an armed track is no edit");

    const auto input = commands.execute(cmd::SetTrackInput{0, 2, true});
    require(input.ok && input.changed && settings.input == 2 && settings.stereoInput && live.input.load() == 2 && live.stereoInput.load() && c.undoLabel() == "Change track input",
            "SetTrackInput sets the first input and stereo");
    const auto monitor = commands.execute(cmd::SetTrackMonitor{0, 2});
    require(monitor.ok && monitor.changed && settings.monitor == 2 && live.monitor.load() == 2 && c.undoLabel() == "Change input monitoring", "SetTrackMonitor sets the mode");
    require(c.undo() && settings.monitor == 0 && live.monitor.load() == 0 && c.undo() && settings.input == 0 && !settings.stereoInput && live.input.load() == 0 && !live.stereoInput.load()
            && c.undo() && alien::sameContent(before, c.document()) && !live.armed.load() && c.redo() && settings.armed && live.armed.load() && c.undo(), "each change is one Undo step");

    // The header's arm button names the track in Undo.
    const auto named = commands.batch({cmd::Command{arm}}, "Arm " + c.document().tracks[0].name);
    require(named.ok && named.changed && c.undoLabel() == "Arm " + c.document().tracks[0].name && c.undo() && alien::sameContent(before, c.document()), "a batch label names the Undo step");

    const int last = alien::maxTrackInputs - 1;
    require(commands.execute(cmd::SetTrackInput{0, last, false}).changed && settings.input == last && c.undo(), "the last input is allowed in mono");
    for (const cmd::Command bad : {cmd::Command{cmd::SetTrackArmed{-1, true}}, cmd::Command{cmd::SetTrackArmed{99, true}}, cmd::Command{cmd::SetTrackInput{0, -1, false}},
                                   cmd::Command{cmd::SetTrackInput{0, alien::maxTrackInputs, false}}, cmd::Command{cmd::SetTrackInput{0, last, true}}, cmd::Command{cmd::SetTrackInput{99, 0, false}},
                                   cmd::Command{cmd::SetTrackMonitor{0, -1}}, cmd::Command{cmd::SetTrackMonitor{0, 3}}, cmd::Command{cmd::SetTrackMonitor{-1, 0}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok, "a track, input or mode that is not there is refused");
    }
    c.setExternalBusy(true);
    const auto busy = commands.execute(arm);
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()) && !live.armed.load(), "a busy project refuses, and refusals change nothing");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::SetTrackArmed{1, true}}, cmd::Command{cmd::SetTrackInput{1, 3, true}}, cmd::Command{cmd::SetTrackMonitor{1, 1}}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the recording setup commands round-trip through JSON");
    }
    require(cmd::describe(c.document(), cmd::SetTrackArmed{0, true}).startsWith("Arm ") && cmd::describe(c.document(), cmd::SetTrackArmed{0, false}).startsWith("Disarm ")
            && cmd::describe(c.document(), cmd::SetTrackInput{0, 2, true}).contains("inputs 3 and 4") && cmd::describe(c.document(), cmd::SetTrackInput{0, 0, false}).contains("input 1")
            && cmd::describe(c.document(), cmd::SetTrackMonitor{0, 2}).endsWith("off"), "the descriptions count inputs from 1 and name the mode");
}
