#pragma once
// A track's output (its bus, which is also its group) and an effect's sidechain key, as named commands.
static alien::PluginState routingEffect() { return alien::PluginState("<PLUGIN name=\"Key test\"/>", {}); }

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

static void sidechainCommandChecks() {
    using alien::KeySource; using alien::RackStage;
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const int returnSlot = alien::return_edit::firstFreeSlot(c.document());
    require(commands.execute(cmd::CreateBus{}).changed && commands.execute(cmd::CreateReturn{}).changed, "slot fixtures");
    std::vector<alien::PluginRackState> racks;
    for (const auto& track : c.document().tracks) racks.push_back(track.plugins);
    racks[0].effects.push_back(routingEffect());
    auto master = c.document().masterPlugins; master.effects.push_back(routingEffect());
    auto buses = c.busRacks(); buses[0].effects.push_back(routingEffect());
    auto returns = c.returnRacks(); returns[static_cast<std::size_t>(returnSlot)].effects.push_back(routingEffect());
    c.setPluginRacks(racks, master, returns, buses);
    c.markAsNewBaseline();
    const auto baseline = c.document();
    const auto keyOf = [](const alien::ProjectDocument& d) { return d.tracks[0].plugins.effects[0]; };

    const cmd::Command key = cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 2};
    const auto preview = commands.preview(key);
    require(preview.ok && preview.changed && keyOf(preview.after).sidechain == 2 && preview.changes[0].contains("FX 1"), "Preview predicts the key");
    require(alien::sameContent(baseline, c.document()) && !c.canUndo(), "Preview leaves the project and history alone");
    require(commands.execute(key).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Set sidechain", "execute matches Preview");
    require(alien::toProjectData(c.document()).valid(), "the keyed project stays valid");
    require(c.undo() && alien::sameContent(baseline, c.document()) && keyOf(c.document()).sidechain == -1, "one Undo removes the key");
    require(c.redo() && keyOf(c.document()).sidechain == 2, "Redo restores it");

    require(c.undo(), "redo fixture");
    const auto same = commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, KeySource::bus, -1});
    require(same.ok && !same.changed && c.canRedo(), "no key to no key is a no-op that keeps Redo");
    require(c.redo(), "keyed again");
    require(commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, KeySource::bus, -1}).changed, "-1 turns the key off");
    require(keyOf(c.document()).sidechain == -1 && keyOf(c.document()).sidechainFrom == KeySource::track && alien::toProjectData(c.document()).valid(), "off always saves as no key");
    require(c.undo(), "keyed fixture");

    // Each rack can take the keys the mix has already made by the time it runs.
    require(commands.execute(cmd::SetSidechain{RackStage::master, 0, 0, KeySource::returnTrack, returnSlot}).changed && c.document().masterPlugins.effects[0].sidechainFrom == KeySource::returnTrack, "the master can hear a return");
    require(commands.execute(cmd::SetSidechain{RackStage::bus, 0, 0, KeySource::trackPostFader, 1}).changed && c.document().buses[0].plugins.effects[0].sidechain == 1, "a bus can hear a track after its fader");
    require(commands.execute(cmd::SetSidechain{RackStage::returnTrack, returnSlot, 0, KeySource::bus, 0}).changed && c.document().returns[static_cast<std::size_t>(returnSlot)].plugins.effects[0].sidechainFrom == KeySource::bus, "a return can hear a bus");
    require(alien::toProjectData(c.document()).valid(), "all those keys save");

    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{
             cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 0},           // its own track
             cmd::SetSidechain{RackStage::track, 0, 0, KeySource::trackPostFader, 1},  // not made yet
             cmd::SetSidechain{RackStage::track, 0, 0, KeySource::bus, 0},             // not made yet
             cmd::SetSidechain{RackStage::bus, 0, 0, KeySource::bus, 0},               // not made yet
             cmd::SetSidechain{RackStage::returnTrack, returnSlot, 0, KeySource::returnTrack, returnSlot},
             cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 99},          // no such track
             cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, -2},
             cmd::SetSidechain{RackStage::master, 0, 0, KeySource::bus, 5},            // unused bus
             cmd::SetSidechain{RackStage::master, 0, 0, KeySource::returnTrack, alien::maxReturns},
             cmd::SetSidechain{RackStage::master, 0, 0, KeySource::bus, alien::maxBuses},
             cmd::SetSidechain{RackStage::track, 0, 1, KeySource::track, 2},           // past the rack
             cmd::SetSidechain{RackStage::track, 0, -1, KeySource::track, 2},
             cmd::SetSidechain{RackStage::track, 99, 0, KeySource::track, 2},
             cmd::SetSidechain{RackStage::master, 1, 0, KeySource::track, 2},
             cmd::SetSidechain{RackStage::bus, 5, 0, KeySource::track, 2},
             cmd::SetSidechain{RackStage::returnTrack, alien::maxReturns, 0, KeySource::track, 2}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()), "an impossible key refuses");

    // A batch sees earlier commands.
    require(!commands.preview(std::vector<cmd::Command>{cmd::DeleteBus{0}, cmd::SetSidechain{RackStage::master, 0, 0, KeySource::bus, 0}}).ok, "a key from a bus deleted earlier in the batch refuses");
    require(commands.preview(std::vector<cmd::Command>{cmd::CreateBus{}, cmd::SetSidechain{RackStage::master, 0, 0, KeySource::bus, 1}}).ok, "a key from a bus added earlier in the batch is accepted");

    // Changing a key rebuilds the plugin racks, so recording refuses, as bus structure does.
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        const auto r = commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 3});
        require(!r.ok && r.error.contains("Stop recording") && alien::sameContent(steady, c.document()), "recording refuses a key change");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    c.setExternalBusy(true);
    const auto busy = commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, KeySource::track, 3});
    require(!busy.ok && busy.error.contains("busy") && alien::sameContent(steady, c.document()), "busy work refuses");
    c.setExternalBusy(false);

    // A frozen track's own effects are locked, but other racks can still hear it.
    auto frozenDoc = c.document(); frozenDoc.tracks[0].settings.freezeScene = 0;
    require(alien::sidechain_edit::refusal(frozenDoc, {RackStage::track, 0, 0, KeySource::track, 3}).contains("Unfreeze"), "a frozen track's effects refuse");
    require(alien::sidechain_edit::refusal(frozenDoc, {RackStage::master, 0, 0, KeySource::track, 0}).isEmpty(), "and the master can still hear it");

    // A saved rack never holds an empty FX slot, because a key on one could not be saved.
    auto emptySlot = c.document(); emptySlot.tracks[0].plugins.effects.emplace_back();
    require(alien::sidechain_edit::refusal(emptySlot, {RackStage::track, 0, 1, KeySource::track, 2}).contains("no effect"), "an empty FX slot refuses");

    juce::String error;
    const cmd::SetSidechain original{RackStage::returnTrack, 2, 1, KeySource::trackPostFader, 3};
    const auto parsed = cmd::parse("SetSidechain", cmd::arguments(original), error);
    require(parsed.has_value(), "SetSidechain round-trips");
    const auto& back = std::get<cmd::SetSidechain>(*parsed);
    require(back.stage == original.stage && back.rack == 2 && back.effect == 1 && back.from == original.from && back.source == 3, "with every argument");
    require(!cmd::parse("SetSidechain", juce::JSON::parse(R"({"stage":"group","rack":0,"effect":0,"from":"track","source":1})"), error) && error.contains("stage"), "an unknown stage refuses");
    require(!cmd::parse("SetSidechain", juce::JSON::parse(R"({"stage":"track","rack":0,"effect":0,"from":"pre","source":1})"), error) && error.contains("from"), "an unknown source kind refuses");
    require(!cmd::parse("SetSidechain", juce::JSON::parse(R"({"stage":"track","rack":0,"effect":0,"from":"track"})"), error) && error.contains("source"), "the source is required");
    std::cout << "PASS: SetSidechain preview, undo, key stages, refusals, guards and parsing\n";
}
