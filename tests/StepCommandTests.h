#pragma once
// SetStepOn, SetStepPitch and SetStepVelocity: the 16-step sequencer's row for the live synth as commands. The
// sequencer's pads and the selected step's Pitch and Velocity sliders run them.
static void stepCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& d = c.document();
    const std::size_t s = 5;

    // A pad.
    {
        const auto before = c.document();
        const bool on = !before.steps.on[s];
        const cmd::SetStepOn set{5, on};
        const auto preview = commands.preview(cmd::Command{set});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && engine->steps[s].load() == before.steps.on[s],
                "SetStepOn Preview changes no live state");
        const auto result = commands.execute(set);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && d.steps.on[s] == on && engine->steps[s].load() == on
                && c.undoLabel() == "Toggle step", "SetStepOn does what Preview said, and the engine plays the step");
        require(!commands.execute(set).changed && c.undo() && alien::sameContent(before, c.document()) && engine->steps[s].load() == before.steps.on[s],
                "the same state again is no edit, and Undo brings the old one back");
        require(c.redo() && d.steps.on[s] == on && engine->steps[s].load() == on && c.undo(), "Redo plays it again");
        const auto named = commands.batch({cmd::Command{set}}, on ? "Turn on step" : "Turn off step");
        require(named.ok && named.changed && c.undoLabel() == (on ? "Turn on step" : "Turn off step") && c.undo(), "the pads name the Undo step by what they did");
    }

    // The selected step's pitch and velocity.
    {
        const auto before = c.document();
        const int pitch = before.steps.pitch[s] == 72 ? 71 : 72;
        const int velocity = before.steps.velocity[s] == 90 ? 91 : 90;
        const auto preview = commands.preview(std::vector<cmd::Command>{cmd::SetStepPitch{5, pitch}, cmd::SetStepVelocity{5, velocity}});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()), "Preview of pitch and velocity changes nothing");
        require(commands.execute(cmd::SetStepPitch{5, pitch}).changed && d.steps.pitch[s] == pitch && engine->stepPitches[s].load() == pitch
                && c.undoLabel() == "Change step pitch", "SetStepPitch sets the pitch the engine plays");
        require(commands.execute(cmd::SetStepVelocity{5, velocity}).changed && d.steps.velocity[s] == velocity && engine->stepVelocities[s].load() == velocity
                && c.undoLabel() == "Change step velocity" && alien::sameContent(preview.after, c.document()), "SetStepVelocity sets the velocity the engine plays");
        require(c.undo() && c.undo() && alien::sameContent(before, c.document()) && engine->stepPitches[s].load() == before.steps.pitch[s]
                && engine->stepVelocities[s].load() == before.steps.velocity[s], "each is one Undo step");

        // A slider drag: many values under one continuous key are one Undo step.
        for (const int value : {40, 41, 42, 43})
            require(commands.execute(cmd::SetStepPitch{5, value}, "step-drag-1", true).ok, "a drag value is accepted");
        require(d.steps.pitch[s] == 43 && engine->stepPitches[s].load() == 43 && c.undo() && alien::sameContent(before, c.document())
                && engine->stepPitches[s].load() == before.steps.pitch[s], "a drag is one Undo step");
    }

    // Refusals change nothing.
    {
        const auto before = c.document();
        for (const cmd::Command bad : {cmd::Command{cmd::SetStepOn{-1, true}}, cmd::Command{cmd::SetStepOn{16, true}}, cmd::Command{cmd::SetStepPitch{16, 60}},
                                       cmd::Command{cmd::SetStepVelocity{-1, 100}}}) {
            const auto refused = commands.execute(bad);
            require(!refused.ok && !refused.changed && refused.error.contains("0 to 15") && !commands.preview(bad).ok, "a step that is not there is refused");
        }
        for (const int bad : {-1, 128})
            require(commands.execute(cmd::SetStepPitch{5, bad}).error.contains("0 to 127"), "a pitch is a MIDI note");
        for (const int bad : {0, 128})
            require(commands.execute(cmd::SetStepVelocity{5, bad}).error.contains("1 to 127"), "a velocity is 1 to 127");
        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetStepOn{5, !before.steps.on[s]});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()) && engine->steps[s].load() == before.steps.on[s],
                "a busy project refuses, and refusals change nothing");
    }

    // Script form.
    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::SetStepOn{3, true}}, cmd::Command{cmd::SetStepPitch{3, 64}}, cmd::Command{cmd::SetStepVelocity{3, 80}}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the step commands round-trip through JSON");
    }
    require(!cmd::parse("SetStepPitch", juce::JSON::parse(R"({"step": 2, "pitch": 60, "extra": 1})"), error), "an argument that is not the command's is refused");
    require(cmd::describe(c.document(), cmd::SetStepOn{3, true}).contains("step 4") && cmd::describe(c.document(), cmd::SetStepPitch{0, 60}).contains("C")
            && cmd::describe(c.document(), cmd::SetStepVelocity{0, 80}).contains("80"), "the step commands say what they do");
}
