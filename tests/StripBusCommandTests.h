#pragma once
// Return strip volume, pan and mute, and bus strips and structure, as named commands.
static void returnStripCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.markAsNewBaseline();
    const auto baseline = c.document();

    const cmd::Command volume = cmd::SetReturnVolume{1, .5};
    const auto preview = commands.preview(volume);
    require(preview.ok && preview.changed && preview.after.returns[1].settings.gain == .5f && preview.changes[0].contains("Return B"), "Preview predicts the return volume");
    require(alien::sameContent(baseline, c.document()) && engine->returns[1].gain.load() == 1.0f && !c.canUndo(), "Preview leaves the project, engine and history alone");
    require(commands.execute(volume).changed && alien::sameContent(preview.after, c.document()) && engine->returns[1].gain.load() == .5f && c.undoLabel() == "Set return volume", "execute matches Preview and reaches the engine");
    require(c.undo() && alien::sameContent(baseline, c.document()) && engine->returns[1].gain.load() == 1.0f, "one Undo restores the document and engine");
    require(c.redo() && engine->returns[1].gain.load() == .5f, "Redo restores them");

    require(c.undo(), "redo fixture");
    const auto same = commands.execute(cmd::SetReturnVolume{1, 1.0});
    require(same.ok && !same.changed && c.canRedo(), "a no-op keeps Redo");
    require(c.redo(), "back to the edited return");

    require(commands.execute(cmd::SetReturnPan{1, -.25}).changed && engine->returns[1].pan.load() == -.25f && c.document().returns[1].settings.pan == -.25f && c.undoLabel() == "Set return pan", "pan reaches the engine");
    require(c.undo() && engine->returns[1].pan.load() == 0.0f, "Undo restores pan");
    require(commands.execute(cmd::SetReturnMute{1, true}).changed && engine->returns[1].mute.load() && c.document().returns[1].settings.mute && c.undoLabel() == "Set return mute", "mute reaches the engine");
    require(c.undo() && !engine->returns[1].mute.load(), "Undo restores mute");

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{
             cmd::SetReturnVolume{1, -.1}, cmd::SetReturnVolume{1, 2.1}, cmd::SetReturnVolume{1, nan}, cmd::SetReturnVolume{6, .5}, cmd::SetReturnVolume{-1, .5}, cmd::SetReturnVolume{alien::maxReturns, .5},
             cmd::SetReturnPan{1, -1.1}, cmd::SetReturnPan{1, 1.1}, cmd::SetReturnPan{1, nan}, cmd::SetReturnPan{6, 0.5},
             cmd::SetReturnMute{6, true}, cmd::SetReturnMute{-1, true}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()) && engine->returns[1].gain.load() == .5f, "bad return strip arguments refuse");

    // A drag is one step however long it takes.
    c.markAsNewBaseline(); const auto dragged = c.document();
    require(commands.execute(cmd::SetReturnVolume{0, .25}, "return-drag", true).changed, "drag start");
    juce::Thread::sleep(alien::DocumentHistory::coalesceMilliseconds + 20);
    require(commands.execute(cmd::SetReturnVolume{0, .75}, "return-drag", true).changed, "drag end");
    require(c.undo() && !c.canUndo() && alien::sameContent(dragged, c.document()), "one drag is one undo step");

    // Strips stay adjustable during recording, as the track fader does; busy work refuses.
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        require(commands.execute(cmd::SetReturnVolume{2, phase == alien::RecordControls::countIn ? .3 : .4}).changed, "a return can be mixed while recording");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    const auto idle = c.document();
    c.setExternalBusy(true);
    require(!commands.execute(cmd::SetReturnMute{2, true}).ok && alien::sameContent(idle, c.document()) && !engine->returns[2].mute.load(), "busy work refuses");
    c.setExternalBusy(false);

    // A batch sees earlier commands, is one step, and changes nothing if any part refuses.
    const auto beforeBatch = c.document();
    require(!commands.batch({cmd::SetReturnVolume{3, .1}, cmd::SetReturnPan{3, 5.0}}, "Refused").ok && alien::sameContent(beforeBatch, c.document()) && engine->returns[3].gain.load() == 1.0f, "a refused batch changes nothing");
    const auto batched = commands.batch({cmd::CreateReturn{}, cmd::SetReturnVolume{4, .5}, cmd::SetReturnPan{4, .5}, cmd::SetReturnMute{4, true}}, "New quiet return");
    require(batched.changed && engine->returns[4].gain.load() == .5f && engine->returns[4].pan.load() == .5f && engine->returns[4].mute.load() && c.undoLabel() == "New quiet return", "a new return can be mixed in one step");
    require(c.undo() && alien::sameContent(beforeBatch, c.document()) && !engine->returns[4].active.load(), "one Undo reverts it");

    juce::String error;
    for (const auto& edit : std::vector<cmd::Command>{cmd::SetReturnVolume{2, .5}, cmd::SetReturnPan{2, -.5}, cmd::SetReturnMute{2, true}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && parsed->index() == edit.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "return strip commands round trip");
    }
    require(!cmd::parse("SetReturnVolume", juce::JSON::parse(R"({"return":0,"gain":"loud"})"), error), "a text gain is refused");
    require(!cmd::parse("SetReturnPan", juce::JSON::parse(R"({"return":"A","pan":0})"), error), "a letter is not a slot");
    require(!cmd::parse("SetReturnMute", juce::JSON::parse(R"({"return":0,"muted":1})"), error), "a numeric mute is refused");
    require(!cmd::parse("SetReturnMute", juce::JSON::parse(R"({"return":0,"muted":true,"track":0})"), error) && error.contains("track"), "an unknown argument is refused");
    std::cout << "PASS: return strip commands preview, undo, grouping, refusal, batches and parsing\n";
}

static void busStripCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    require(!commands.execute(cmd::SetBusVolume{0, .5}).ok, "a new project has no bus to mix");
    require(commands.execute(cmd::CreateBus{}).changed, "bus fixture");
    c.markAsNewBaseline();
    const auto baseline = c.document();

    const cmd::Command volume = cmd::SetBusVolume{0, .5};
    const auto preview = commands.preview(volume);
    require(preview.ok && preview.changed && preview.after.buses[0].settings.gain == .5f && preview.changes[0].contains("Bus 1"), "Preview predicts the bus volume");
    require(alien::sameContent(baseline, c.document()) && engine->buses[0].gain.load() == 1.0f && !c.canUndo(), "Preview leaves the project, engine and history alone");
    require(commands.execute(volume).changed && alien::sameContent(preview.after, c.document()) && engine->buses[0].gain.load() == .5f && c.undoLabel() == "Set bus volume", "execute matches Preview and reaches the engine");
    require(c.undo() && alien::sameContent(baseline, c.document()) && engine->buses[0].gain.load() == 1.0f, "one Undo restores the document and engine");
    require(c.redo() && engine->buses[0].gain.load() == .5f, "Redo restores them");

    require(c.undo(), "redo fixture");
    const auto same = commands.execute(cmd::SetBusVolume{0, 1.0});
    require(same.ok && !same.changed && c.canRedo(), "a no-op keeps Redo");
    require(c.redo(), "back to the edited bus");

    require(commands.execute(cmd::SetBusPan{0, .25}).changed && engine->buses[0].pan.load() == .25f && c.undoLabel() == "Set bus pan", "pan reaches the engine");
    require(c.undo() && engine->buses[0].pan.load() == 0.0f, "Undo restores pan");
    require(commands.execute(cmd::SetBusMute{0, true}).changed && engine->buses[0].mute.load() && c.undoLabel() == "Set bus mute", "mute reaches the engine");
    require(c.undo() && !engine->buses[0].mute.load(), "Undo restores mute");
    require(commands.execute(cmd::SetBusSolo{0, true}).changed && engine->buses[0].solo.load() && c.document().buses[0].settings.solo && c.undoLabel() == "Set bus solo", "solo reaches the engine");
    require(c.undo() && !engine->buses[0].solo.load(), "Undo restores solo");

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{
             cmd::SetBusVolume{0, -.1}, cmd::SetBusVolume{0, 2.1}, cmd::SetBusVolume{0, nan}, cmd::SetBusVolume{1, .5}, cmd::SetBusVolume{-1, .5}, cmd::SetBusVolume{alien::maxBuses, .5},
             cmd::SetBusPan{0, -1.1}, cmd::SetBusPan{0, 1.1}, cmd::SetBusPan{0, nan}, cmd::SetBusPan{1, 0.5},
             cmd::SetBusMute{1, true}, cmd::SetBusSolo{1, true}, cmd::SetBusMute{-1, true}, cmd::SetBusSolo{alien::maxBuses, true}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()), "bad bus strip arguments refuse");

    c.markAsNewBaseline(); const auto dragged = c.document();
    require(commands.execute(cmd::SetBusPan{0, -.25}, "bus-drag", true).changed, "drag start");
    juce::Thread::sleep(alien::DocumentHistory::coalesceMilliseconds + 20);
    require(commands.execute(cmd::SetBusPan{0, .75}, "bus-drag", true).changed, "drag end");
    require(c.undo() && !c.canUndo() && alien::sameContent(dragged, c.document()), "one drag is one undo step");

    engine->recording.phase.store(alien::RecordControls::recording);
    require(commands.execute(cmd::SetBusVolume{0, .3}).changed && commands.execute(cmd::SetBusSolo{0, true}).changed, "a bus can be mixed while recording");
    engine->recording.phase.store(alien::RecordControls::idle);
    const auto idle = c.document();
    c.setExternalBusy(true);
    require(!commands.execute(cmd::SetBusVolume{0, .9}).ok && alien::sameContent(idle, c.document()) && engine->buses[0].gain.load() == .3f, "busy work refuses");
    c.setExternalBusy(false);

    juce::String error;
    for (const auto& edit : std::vector<cmd::Command>{cmd::SetBusVolume{2, .5}, cmd::SetBusPan{2, -.5}, cmd::SetBusMute{2, true}, cmd::SetBusSolo{2, true}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && parsed->index() == edit.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "bus strip commands round trip");
    }
    require(!cmd::parse("SetBusVolume", juce::JSON::parse(R"({"bus":0,"gain":"loud"})"), error), "a text gain is refused");
    require(!cmd::parse("SetBusSolo", juce::JSON::parse(R"({"bus":0,"soloed":"yes"})"), error), "a text solo is refused");
    require(!cmd::parse("SetBusPan", juce::JSON::parse(R"({"return":0,"pan":0})"), error), "a bus command takes a bus");
    std::cout << "PASS: bus strip commands preview, undo, grouping, refusal and parsing\n";
}

static void busStructureCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.markAsNewBaseline();
    const auto baseline = c.document();

    // Add takes the first free slot and the lowest free name.
    const auto preview = commands.preview(cmd::CreateBus{});
    require(preview.ok && preview.after.buses[0].used && preview.after.buses[0].name == "Bus 1" && preview.changes[0].contains("Bus 1"), "Preview names the new bus");
    require(alien::sameContent(baseline, c.document()) && !engine->buses[0].active.load(), "Preview adds nothing");
    require(commands.execute(cmd::CreateBus{}).changed && alien::sameContent(preview.after, c.document()) && engine->buses[0].active.load() && c.undoLabel() == "Add bus", "execute matches Preview");
    require(preview.after.trackId(alien::TrackRole::bus, 0) == c.document().trackId(alien::TrackRole::bus, 0), "and predicts the identity");
    require(c.undo() && alien::sameContent(baseline, c.document()) && !engine->buses[0].active.load(), "one Undo removes it");
    require(c.redo() && engine->buses[0].active.load(), "Redo brings it back");

    // Rename.
    require(commands.execute(cmd::RenameBus{0, "  Drums  "}).changed && c.document().buses[0].name == "Drums" && c.undoLabel() == "Rename bus", "renaming trims the name");
    const auto unchanged = commands.execute(cmd::RenameBus{0, "Drums"});
    require(unchanged.ok && !unchanged.changed && c.undoLabel() == "Rename bus", "the same name is a no-op");
    require(!commands.execute(cmd::RenameBus{0, "   "}).ok && !commands.execute(cmd::RenameBus{5, "X"}).ok && !commands.execute(cmd::RenameBus{-1, "X"}).ok, "blank names and unused slots refuse");

    // A same-name rename inside a batch does not undo the rest of the batch.
    const auto mixed = commands.batch({cmd::RenameBus{0, "Drums"}, cmd::SetBusVolume{0, .25}}, "Drum level");
    require(mixed.changed && c.document().buses[0].settings.gain == .25f && c.undoLabel() == "Drum level", "a no-op rename can share a batch");
    require(c.undo(), "batch undo");

    // Deleting a bus sends its tracks to the master and leaves other buses in their slots.
    require(commands.execute(cmd::CreateBus{}).changed && c.setTrackOutput(1, 0) && c.setTrackOutput(2, 1), "route fixtures");
    c.commit("Route fixture");
    const auto full = c.document();
    const auto removal = commands.preview(cmd::DeleteBus{0});
    require(removal.ok && !removal.after.buses[0].used && removal.after.buses[1].used && removal.after.tracks[1].settings.output == -1 && removal.after.tracks[2].settings.output == 1, "Preview shows what deletion takes");
    require(alien::sameContent(full, c.document()) && engine->buses[0].active.load() && engine->tracks[1].output.load() == 0, "Preview removes nothing");
    require(commands.execute(cmd::DeleteBus{0}).changed && alien::sameContent(removal.after, c.document()) && !engine->buses[0].active.load() && engine->tracks[1].output.load() == -1 && engine->tracks[2].output.load() == 1 && c.undoLabel() == "Delete bus", "execute matches Preview");
    require(alien::toProjectData(c.document()).valid(), "the project stays valid");
    require(c.undo() && alien::sameContent(full, c.document()) && engine->buses[0].active.load() && engine->tracks[1].output.load() == 0, "one Undo restores the bus and its routing");

    // A batch sees earlier commands, is one step, and changes nothing if any part refuses.
    const auto beforeBatch = c.document();
    require(!commands.batch({cmd::CreateBus{}, cmd::DeleteBus{7}}, "Refused").ok && alien::sameContent(beforeBatch, c.document()) && !engine->buses[2].active.load(), "a refused batch adds nothing");
    const auto batched = commands.batch({cmd::CreateBus{}, cmd::RenameBus{2, "Keys"}, cmd::SetBusVolume{2, .5}}, "Add keys");
    require(batched.changed && c.document().buses[2].name == "Keys" && engine->buses[2].gain.load() == .5f && c.undoLabel() == "Add keys", "a new bus can be named and mixed in one step");
    require(c.undo() && alien::sameContent(beforeBatch, c.document()) && !engine->buses[2].active.load(), "one Undo reverts it");

    // A slot reused within a batch gets a fresh identity in Preview and live alike.
    const auto oldId = c.document().trackId(alien::TrackRole::bus, 0);
    const std::vector<cmd::Command> reuse{cmd::DeleteBus{0}, cmd::CreateBus{}};
    const auto reusedPreview = commands.preview(reuse);
    require(reusedPreview.ok && reusedPreview.after.buses[0].used && reusedPreview.after.trackId(alien::TrackRole::bus, 0) != oldId, "Preview gives a reused slot a fresh identity");
    const auto reused = commands.batch(reuse, "Replace bus");
    require(reused.changed && c.document().buses[0].used && c.document().trackId(alien::TrackRole::bus, 0) != oldId, "a reused slot has a fresh identity");
    require(c.undo() && c.document().trackId(alien::TrackRole::bus, 0) == oldId, "Undo restores the old identity");

    // Structure is locked while recording, and when every slot is taken.
    const auto steady = c.document();
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        for (const auto& edit : std::vector<cmd::Command>{cmd::CreateBus{}, cmd::RenameBus{0, "X"}, cmd::DeleteBus{0}}) {
            const auto r = commands.execute(edit);
            require(!r.ok && r.error.contains("Stop recording") && alien::sameContent(steady, c.document()), "recording refuses bus structure edits");
        }
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    c.setExternalBusy(true);
    require(!commands.execute(cmd::CreateBus{}).ok && alien::sameContent(steady, c.document()), "busy work refuses");
    c.setExternalBusy(false);
    while (!c.document().buses[alien::maxBuses - 1].used) require(commands.execute(cmd::CreateBus{}).changed, "fill the slots");
    const auto fullSlots = commands.execute(cmd::CreateBus{});
    require(!fullSlots.ok && fullSlots.error.contains("most buses"), "the ninth bus refuses");

    // Parsing.
    juce::String error;
    require(cmd::parse("CreateBus", {}, error).has_value(), "CreateBus takes no arguments");
    require(!cmd::parse("CreateBus", juce::JSON::parse(R"({"name":"X"})"), error) && error.contains("name"), "and refuses any");
    const auto rename = cmd::parse("RenameBus", juce::JSON::parse(R"({"bus":2,"name":"Keys"})"), error);
    require(rename && std::get<cmd::RenameBus>(*rename).bus == 2 && std::get<cmd::RenameBus>(*rename).name == "Keys", "RenameBus parses");
    const auto remove = cmd::parse("DeleteBus", juce::JSON::parse(R"({"bus":3})"), error);
    require(remove && std::get<cmd::DeleteBus>(*remove).bus == 3, "DeleteBus parses");
    require(!cmd::parse("DeleteBus", juce::JSON::parse(R"({"bus":"A"})"), error), "a letter is not a slot");
    std::cout << "PASS: CreateBus, RenameBus and DeleteBus preview, undo, batches, identities, guards and parsing\n";
}
