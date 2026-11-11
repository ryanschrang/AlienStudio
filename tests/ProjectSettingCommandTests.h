#pragma once
// SetLivePreset and SetLaunchQuantization: the live synth sound and the Session launch timing as commands. The
// Devices page and the Session header's Beat/Bar box run them.
static void projectSettingCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& d = c.document();

    // The live sound.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const int other = d.livePreset == 2 ? 3 : 2;
        const cmd::SetLivePreset set{other};
        const auto preview = commands.preview(cmd::Command{set});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history && engine->preset.load() == before.livePreset,
                "SetLivePreset Preview changes no live state");
        const auto result = commands.execute(set);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && d.livePreset == other && engine->preset.load() == other
                && c.undoLabel() == "Change live sound", "SetLivePreset does what Preview said, and the engine plays the sound");
        const auto same = commands.execute(set);
        require(same.ok && !same.changed && c.undoLabel() == "Change live sound" && c.undo() && alien::sameContent(before, c.document())
                && engine->preset.load() == before.livePreset, "the same sound again is no edit, and Undo brings the old one back");
        require(c.redo() && d.livePreset == other && engine->preset.load() == other && c.undo(), "Redo plays it again");
        for (const int bad : {-1, alien::presetCount}) {
            const auto refused = commands.execute(cmd::SetLivePreset{bad});
            require(!refused.ok && refused.error.contains("Synth presets are numbered") && !commands.preview(cmd::Command{cmd::SetLivePreset{bad}}).ok, "a preset that is not there is refused");
        }
        c.setExternalBusy(true);
        const auto busy = commands.execute(set);
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()), "a busy project refuses, and refusals change nothing");
    }

    // The Session launch timing: the next beat (4 steps) or the next bar (16).
    {
        const auto before = c.document();
        const int other = d.launchQuantizationSteps == 4 ? 16 : 4;
        const cmd::SetLaunchQuantization set{other};
        const auto preview = commands.preview(cmd::Command{set});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && engine->launchQuantizationSteps.load() == before.launchQuantizationSteps,
                "SetLaunchQuantization Preview changes no live state");
        const auto result = commands.execute(set);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && d.launchQuantizationSteps == other
                && engine->launchQuantizationSteps.load() == other && c.undoLabel() == "Change launch timing", "SetLaunchQuantization does what Preview said");
        require(!commands.execute(set).changed && c.undo() && alien::sameContent(before, c.document())
                && engine->launchQuantizationSteps.load() == before.launchQuantizationSteps && c.redo() && c.undo(), "it is one Undo step");
        for (const int bad : {0, 8, 32}) {
            const auto refused = commands.execute(cmd::SetLaunchQuantization{bad});
            require(!refused.ok && refused.error.contains("4 steps") && !refused.changed, "only a beat or a bar is a launch timing");
        }
        require(alien::sameContent(before, c.document()), "refusals change nothing");
    }

    // Script form.
    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::SetLivePreset{3}}, cmd::Command{cmd::SetLaunchQuantization{4}}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the project setting commands round-trip through JSON");
    }
    require(!cmd::parse("SetLivePreset", juce::JSON::parse(R"({"preset": 1.5})"), error) && error.contains("whole-number"), "a preset is a whole number");
    require(cmd::describe(c.document(), cmd::SetLivePreset{0}).contains(alien::presetNames[0]), "SetLivePreset names the sound");
    require(cmd::describe(c.document(), cmd::SetLaunchQuantization{4}).contains("beat") && cmd::describe(c.document(), cmd::SetLaunchQuantization{16}).contains("bar"),
            "SetLaunchQuantization says beat or bar");
}
