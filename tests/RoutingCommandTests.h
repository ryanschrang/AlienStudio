#pragma once
// A track's output (its bus, which is also its group) as a named command.

static void trackOutputCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    require(commands.execute(cmd::CreateBus{}).changed && commands.execute(cmd::RenameBus{0, "Drums"}).changed, "bus fixture");
    c.markAsNewBaseline();
    const auto baseline = c.document();

    const cmd::Command route = cmd::SetTrackOutput{1, 0};
    const auto preview = commands.preview(route);
    require(preview.ok && preview.changed && preview.after.tracks[1].settings.output == 0 && preview.changes[0].contains("Drums"), "Preview predicts the output");
    require(alien::sameContent(baseline, c.document()) && engine->tracks[1].output.load() == -1 && !c.canUndo(), "Preview leaves the project, engine and history alone");
    require(commands.execute(route).changed && alien::sameContent(preview.after, c.document()) && engine->tracks[1].output.load() == 0 && c.undoLabel() == "Set track output", "execute matches Preview and reaches the engine");
    require(c.undo() && alien::sameContent(baseline, c.document()) && engine->tracks[1].output.load() == -1, "one Undo restores the document and engine");
    require(c.redo() && engine->tracks[1].output.load() == 0, "Redo restores them");

    require(c.undo(), "redo fixture");
    const auto same = commands.execute(cmd::SetTrackOutput{1, -1});
    require(same.ok && !same.changed && c.canRedo(), "a no-op keeps Redo");
    require(c.redo(), "back to the routed track");
    require(commands.execute(cmd::SetTrackOutput{1, -1}).changed && engine->tracks[1].output.load() == -1, "-1 sends it back to the master");
    require(c.undo(), "routed again");

    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{cmd::SetTrackOutput{1, 1}, cmd::SetTrackOutput{1, -2}, cmd::SetTrackOutput{1, alien::maxBuses}, cmd::SetTrackOutput{-1, 0}, cmd::SetTrackOutput{99, 0}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()) && engine->tracks[1].output.load() == 0, "a bad output or track refuses");

    // A batch sees earlier commands, and a no-op inside it does not undo the rest.
    const auto add = commands.batch({cmd::CreateBus{}, cmd::SetTrackOutput{2, 1}, cmd::SetTrackOutput{1, 0}}, "Group keys");
    require(add.changed && engine->tracks[2].output.load() == 1 && c.undoLabel() == "Group keys", "a new bus can take a track in one step");
    require(c.undo() && alien::sameContent(steady, c.document()) && engine->tracks[2].output.load() == -1, "one Undo reverts it");
    const auto refused = commands.batch({cmd::SetTrackOutput{2, 0}, cmd::SetTrackOutput{3, 7}}, "Refused");
    require(!refused.ok && refused.failedCommand == 1 && alien::sameContent(steady, c.document()) && engine->tracks[2].output.load() == -1, "a refused batch changes nothing");
    require(!commands.preview(std::vector<cmd::Command>{cmd::DeleteBus{0}, cmd::SetTrackOutput{2, 0}}).ok, "a bus deleted earlier in the batch cannot be chosen");

    // Rerouting is a single atomic store, so it stays live while recording, like a fader.
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        require(commands.execute(cmd::SetTrackOutput{2, 0}).changed && engine->tracks[2].output.load() == 0, "recording leaves the output adjustable");
        require(c.undo(), "output undo during recording");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    c.setExternalBusy(true);
    const auto busy = commands.execute(cmd::SetTrackOutput{2, 0});
    require(!busy.ok && busy.error.contains("busy") && engine->tracks[2].output.load() == -1, "busy work refuses");
    c.setExternalBusy(false);

    juce::String error;
    const auto parsed = cmd::parse("SetTrackOutput", cmd::arguments(cmd::SetTrackOutput{2, -1}), error);
    require(parsed && std::get<cmd::SetTrackOutput>(*parsed).track == 2 && std::get<cmd::SetTrackOutput>(*parsed).bus == -1, "SetTrackOutput round-trips");
    require(cmd::name(*parsed) == "SetTrackOutput", "the stable name");
    require(!cmd::parse("SetTrackOutput", juce::JSON::parse(R"({"track":1})"), error) && error.contains("bus"), "the bus is required");
    require(!cmd::parse("SetTrackOutput", juce::JSON::parse(R"({"track":1,"bus":"Drums"})"), error), "a name is not a slot");
    require(!cmd::parse("SetTrackOutput", juce::JSON::parse(R"({"track":1,"bus":0,"group":1})"), error) && error.contains("group"), "unknown arguments refuse");
    std::cout << "PASS: SetTrackOutput preview, undo, batches, guards and parsing\n";
}
