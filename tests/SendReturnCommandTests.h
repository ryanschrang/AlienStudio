#pragma once
// Send levels and modes, and adding, renaming and removing returns, as named commands.
static void sendCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.markAsNewBaseline();
    const auto baseline = c.document();

    const cmd::Command level = cmd::SetTrackSend{1, 2, .5};
    const auto preview = commands.preview(level);
    require(preview.ok && preview.changed && preview.after.tracks[1].settings.send[2] == .5f && preview.changes[0].contains("send"), "Preview predicts the send level");
    require(alien::sameContent(baseline, c.document()) && engine->tracks[1].send[2].load() == 0.0f && !c.canUndo(), "Preview leaves the project, engine and history alone");
    require(commands.execute(level).changed && alien::sameContent(preview.after, c.document()) && engine->tracks[1].send[2].load() == .5f && c.undoLabel() == "Set send level", "execute matches Preview and reaches the engine");
    require(c.undo() && alien::sameContent(baseline, c.document()) && engine->tracks[1].send[2].load() == 0.0f, "one Undo restores the document and engine");
    require(c.redo() && engine->tracks[1].send[2].load() == .5f, "Redo restores them");

    // Setting what is already there records nothing, and keeps Redo.
    require(c.undo(), "redo fixture");
    const auto same = commands.execute(cmd::SetTrackSend{1, 2, 0.0});
    require(same.ok && !same.changed && c.canRedo(), "a no-op records nothing");
    require(c.redo(), "back to the edited send");

    const cmd::Command mode = cmd::SetTrackSendMode{1, 2, true};
    require(commands.execute(mode).changed && c.document().tracks[1].settings.sendPre[2] && engine->tracks[1].sendPre[2].load() && c.undoLabel() == "Set send mode", "pre-fader mode reaches the engine");
    require(c.undo() && !engine->tracks[1].sendPre[2].load(), "Undo restores post-fader");

    // Bad values refuse before anything changes.
    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{
             cmd::SetTrackSend{1, 2, -.1}, cmd::SetTrackSend{1, 2, 1.1}, cmd::SetTrackSend{1, 2, std::numeric_limits<double>::quiet_NaN()},
             cmd::SetTrackSend{9, 2, .5}, cmd::SetTrackSend{1, 6, .5}, cmd::SetTrackSend{1, -1, .5}, cmd::SetTrackSend{1, alien::maxReturns, .5},
             cmd::SetTrackSendMode{1, 6, true}, cmd::SetTrackSendMode{9, 2, true}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()), "bad send arguments refuse");

    // A drag is one step however long it takes.
    c.markAsNewBaseline(); const auto dragged = c.document();
    require(commands.execute(cmd::SetTrackSend{0, 0, .25}, "send-drag", true).changed, "drag start");
    juce::Thread::sleep(alien::DocumentHistory::coalesceMilliseconds + 20);
    require(commands.execute(cmd::SetTrackSend{0, 0, .75}, "send-drag", true).changed, "drag end");
    require(c.undo() && !c.canUndo() && alien::sameContent(dragged, c.document()), "one drag is one undo step");

    // Sends stay adjustable during recording, as the track fader does; busy work refuses.
    engine->recording.phase.store(alien::RecordControls::recording);
    require(commands.execute(cmd::SetTrackSend{0, 1, .5}).changed, "a send can be mixed while recording");
    engine->recording.phase.store(alien::RecordControls::idle);
    const auto idle = c.document();
    c.setExternalBusy(true);
    require(!commands.execute(cmd::SetTrackSend{0, 1, .1}).ok && alien::sameContent(idle, c.document()), "busy work refuses");
    c.setExternalBusy(false);

    // Names and arguments round trip; junk is refused.
    juce::String error;
    const auto parsed = cmd::parse(cmd::name(level), cmd::arguments(level), error);
    require(parsed && std::get<cmd::SetTrackSend>(*parsed).returnSlot == 2 && std::get<cmd::SetTrackSend>(*parsed).level == .5, "SetTrackSend round trips");
    const auto parsedMode = cmd::parse(cmd::name(mode), cmd::arguments(mode), error);
    require(parsedMode && std::get<cmd::SetTrackSendMode>(*parsedMode).preFader, "SetTrackSendMode round trips");
    require(!cmd::parse("SetTrackSend", juce::JSON::parse(R"({"track":0,"return":1,"level":"loud"})"), error), "a text level is refused");
    require(!cmd::parse("SetTrackSendMode", juce::JSON::parse(R"({"track":0,"return":1,"preFader":1})"), error), "a numeric mode is refused");
    std::cout << "PASS: SetTrackSend and SetTrackSendMode preview, undo, grouping, refusal and parsing\n";
}

static void returnCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.markAsNewBaseline();
    const auto baseline = c.document();
    require(baseline.returns[3].used && !baseline.returns[4].used, "a new project has returns A to D");

    // Add takes the first free slot.
    const auto preview = commands.preview(cmd::CreateReturn{});
    require(preview.ok && preview.after.returns[4].used && preview.after.returns[4].name == "Return E" && preview.changes[0].contains("Return E"), "Preview names the new return");
    require(alien::sameContent(baseline, c.document()) && !engine->returns[4].active.load(), "Preview adds nothing");
    require(commands.execute(cmd::CreateReturn{}).changed && alien::sameContent(preview.after, c.document()) && engine->returns[4].active.load() && c.undoLabel() == "Add return", "execute matches Preview");
    require(c.undo() && alien::sameContent(baseline, c.document()) && !engine->returns[4].active.load(), "one Undo removes it");
    require(c.redo() && engine->returns[4].active.load(), "Redo brings it back");

    // Rename.
    require(commands.execute(cmd::RenameReturn{4, "  Hall  "}).changed && c.document().returns[4].name == "Hall" && c.undoLabel() == "Rename return", "renaming trims the name");
    const auto unchanged = commands.execute(cmd::RenameReturn{4, "Hall"});
    require(unchanged.ok && !unchanged.changed, "the same name is a no-op");
    require(!commands.execute(cmd::RenameReturn{4, "   "}).ok && !commands.execute(cmd::RenameReturn{6, "X"}).ok && !commands.execute(cmd::RenameReturn{-1, "X"}).ok, "blank names and unused slots refuse");

    // A same-name rename inside a batch does not undo the rest of the batch.
    const auto mixed = commands.batch({cmd::RenameReturn{4, "Hall"}, cmd::SetTrackSend{0, 4, .25}}, "Hall send");
    require(mixed.changed && c.document().tracks[0].settings.send[4] == .25f && c.undoLabel() == "Hall send", "a no-op rename can share a batch");
    require(c.undo(), "batch undo");

    // Removing a return takes its sends and automation, and leaves the others in their slots.
    require(commands.execute(cmd::SetTrackSend{0, 1, .5}).changed && commands.execute(cmd::SetTrackSendMode{0, 1, true}).changed && commands.execute(cmd::SetTrackSend{0, 2, .25}).changed, "send fixtures");
    alien::automation::Lane lane; lane.track = 0; lane.parameter = alien::automation::Parameter::sendB;
    auto withLane = c.document(); withLane.timeline.automationLanes.push_back(lane);
    c.revertTo(withLane); c.commit("Lane fixture");
    const auto full = c.document();
    const auto removal = commands.preview(cmd::DeleteReturn{1});
    require(removal.ok && !removal.after.returns[1].used && removal.after.tracks[0].settings.send[1] == 0.0f && !removal.after.tracks[0].settings.sendPre[1]
        && removal.after.timeline.automationLanes.empty() && removal.after.tracks[0].settings.send[2] == .25f, "Preview shows what removal takes");
    require(alien::sameContent(full, c.document()) && engine->returns[1].active.load(), "Preview removes nothing");
    require(commands.execute(cmd::DeleteReturn{1}).changed && alien::sameContent(removal.after, c.document()) && !engine->returns[1].active.load() && engine->tracks[0].send[1].load() == 0.0f && c.undoLabel() == "Remove return", "execute matches Preview");
    require(alien::toProjectData(c.document()).valid(), "the project stays valid");
    require(c.undo() && alien::sameContent(full, c.document()) && engine->returns[1].active.load() && engine->tracks[0].send[1].load() == .5f && engine->tracks[0].sendPre[1].load(), "one Undo restores the return, send and automation");
    require(!commands.execute(cmd::SetTrackSend{0, 6, .5}).ok, "a send to an unused slot refuses");

    // A batch sees earlier commands, is one step, and changes nothing if any part refuses.
    const auto beforeBatch = c.document();
    require(!commands.batch({cmd::CreateReturn{}, cmd::DeleteReturn{7}}, "Refused").ok && alien::sameContent(beforeBatch, c.document()) && !engine->returns[5].active.load(), "a refused batch adds nothing");
    const auto batched = commands.batch({cmd::CreateReturn{}, cmd::RenameReturn{5, "Echo"}, cmd::SetTrackSend{1, 5, .5}}, "Add echo");
    require(batched.changed && c.document().returns[5].name == "Echo" && engine->tracks[1].send[5].load() == .5f && c.undoLabel() == "Add echo", "a new return can be named and sent to in one step");
    require(c.undo() && alien::sameContent(beforeBatch, c.document()) && !engine->returns[5].active.load(), "one Undo reverts it");

    // A slot reused within a batch gets a fresh identity in Preview and live alike.
    const auto oldId = c.document().trackId(alien::TrackRole::returnTrack, 0);
    const std::vector<cmd::Command> reuse{cmd::DeleteReturn{0}, cmd::CreateReturn{}};
    const auto reused = commands.batch(reuse, "Replace return A");
    require(reused.changed && c.document().returns[0].used && c.document().trackId(alien::TrackRole::returnTrack, 0) != oldId, "a reused slot has a fresh identity");
    require(c.undo() && c.document().trackId(alien::TrackRole::returnTrack, 0) == oldId, "Undo restores the old identity");

    // Structure is locked while recording, and when every slot is taken.
    const auto steady = c.document();
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        for (const auto& edit : std::vector<cmd::Command>{cmd::CreateReturn{}, cmd::RenameReturn{0, "X"}, cmd::DeleteReturn{0}}) {
            const auto r = commands.execute(edit);
            require(!r.ok && r.error.contains("Stop recording") && alien::sameContent(steady, c.document()), "recording refuses return structure edits");
        }
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    c.setExternalBusy(true);
    require(!commands.execute(cmd::CreateReturn{}).ok && alien::sameContent(steady, c.document()), "busy work refuses");
    c.setExternalBusy(false);
    while (c.document().returns[alien::maxReturns - 1].used == false) require(commands.execute(cmd::CreateReturn{}).changed, "fill the slots");
    const auto fullSlots = commands.execute(cmd::CreateReturn{});
    require(!fullSlots.ok && fullSlots.error.contains("most returns"), "the ninth return refuses");

    // Parsing.
    juce::String error;
    require(cmd::parse("CreateReturn", {}, error).has_value(), "CreateReturn takes no arguments");
    require(!cmd::parse("CreateReturn", juce::JSON::parse(R"({"name":"X"})"), error) && error.contains("name"), "and refuses any");
    const auto rename = cmd::parse("RenameReturn", juce::JSON::parse(R"({"return":2,"name":"Plate"})"), error);
    require(rename && std::get<cmd::RenameReturn>(*rename).returnSlot == 2 && std::get<cmd::RenameReturn>(*rename).name == "Plate", "RenameReturn parses");
    const auto remove = cmd::parse("DeleteReturn", juce::JSON::parse(R"({"return":3})"), error);
    require(remove && std::get<cmd::DeleteReturn>(*remove).returnSlot == 3, "DeleteReturn parses");
    require(!cmd::parse("DeleteReturn", juce::JSON::parse(R"({"return":"A"})"), error), "a letter is not a slot");
    std::cout << "PASS: CreateReturn, RenameReturn and DeleteReturn preview, undo, batches, identities, guards and parsing\n";
}
