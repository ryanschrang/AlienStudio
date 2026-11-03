#pragma once
static void rackCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.setClipPattern(0,0,patternWith(60),false); require(c.copyCell(0,0), "rack command clipboard fixture");
    c.markAsNewBaseline(); const auto before = c.document(); const auto history = historyOf(c);
    const auto created = commands.preview(cmd::CreateRackPattern{});
    require(created.ok && created.after.channelRack.patternCount == before.channelRack.patternCount + 1
        && created.after.channelRack.selectedPattern == before.channelRack.patternCount, "Preview creates and selects empty rack pattern");
    const auto newId = created.after.channelRack.clips[static_cast<std::size_t>(before.channelRack.patternCount)];
    const std::vector<cmd::Command> edits{cmd::CreateRackPattern{},cmd::AssignRackClip{0,0,newId},cmd::AssignRackClip{1,1,newId}};
    const auto preview = commands.preview(edits);
    require(preview.ok && alien::sameContent(before,c.document()) && historyOf(c) == history, "rack Preview resolves new source IDs without live changes");
    require(commands.batch(edits,"Create and assign rack").changed && alien::sameContent(preview.after,c.document())
        && c.document().clip(0,0).rackClip == newId && c.document().clip(1,1).rackClip == newId, "sequential create/assign batch matches predicted identities");
    require(c.document().selectedTrack == before.selectedTrack && c.document().selectedScene == before.selectedScene
        && c.clipClipboard().source == before.cellClip(0,0), "rack commands preserve Session selection and clipboard");
    require(c.undo() && !c.canUndo() && alien::sameContent(before,c.document()) && c.redo(), "one Undo/Redo restores patterns and replaced cell content");
    const auto noOp = historyOf(c);
    require(commands.execute(cmd::AssignRackClip{0,0,newId}).ok && !commands.preview(cmd::AssignRackClip{0,0,newId}).changed
        && historyOf(c) == noOp, "same assignment is a no-op preserving history");
    require(c.undo(), "atomic rack refusal fixture"); const auto redo = historyOf(c);
    require(!commands.batch({cmd::CreateRackPattern{},cmd::AssignRackClip{99,0,newId}},"Bad rack").ok
        && alien::sameContent(before,c.document()) && historyOf(c) == redo, "invalid later assignment prevents prefix pattern creation");
    require(!commands.execute(cmd::AssignRackClip{0,0,newId}).ok, "deleted/undone rack source cannot resolve by another slot");
    const auto source = before.channelRack.clips[0];
    require(c.placeRegion(0,0,0), "Song reference fixture"); const auto song = c.document();
    require(!commands.execute(cmd::AssignRackClip{0,0,source}).ok && alien::sameContent(song,c.document()), "Song cell reference refuses assignment before mutation");
    auto frozen = before; frozen.tracks[0].settings.freezeScene = 0;
    require(cmd::refusal(frozen,cmd::AssignRackClip{0,0,source}).isNotEmpty(), "frozen assignment refuses");
    frozen.channelRack.channels[0].output = 0;
    require(cmd::refusal(frozen,cmd::CreateRackPattern{}).isNotEmpty(), "pattern creation honors frozen rack output routing");
    auto full = before; auto bank = full.channelRack; while (bank.addPattern()) {}
    require(full.setRack(std::move(bank)), "rack pattern limit fixture"); const auto fullBefore = full;
    require(cmd::applyTo(full,cmd::CreateRackPattern{}).isNotEmpty() && alien::sameContent(fullBefore,full), "full rack refuses without partial creation");
    c.setExternalBusy(true); require(!commands.execute(cmd::CreateRackPattern{}).ok, "busy rack create refuses"); c.setExternalBusy(false);
    for (const auto phase : {alien::RecordControls::countIn,alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        require(!commands.batch({cmd::RenameTrack{1,"Never"},cmd::CreateRackPattern{}},"Recording create").ok
            && !commands.execute(cmd::AssignRackClip{1,1,source}).ok && alien::sameContent(song,c.document()), "recording/count-in refuses create/assign before any mixed batch mutation");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    engine->playing.store(true); engine->requestedPianoRollClip[2].store(3);
    require(commands.execute(cmd::AssignRackClip{1,1,source}).changed && engine->playing.load()
        && engine->requestedPianoRollClip[2].load() == 3 && engine->requestedPianoRollClip[1].load() == -1, "assignment stops addressed ordinary track and preserves unrelated transport/queue");
    juce::String error;
    for (const cmd::Command& edit : std::vector<cmd::Command>{cmd::CreateRackPattern{},cmd::AssignRackClip{1,1,source},cmd::AssignRackClip{1,1,18446744073709551615ull}}) {
        const auto parsed = cmd::parse(cmd::name(edit),cmd::arguments(edit),error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "rack commands round-trip full 64-bit source identity without numeric precision loss");
    }
    for (const char* json : {"{}", "{\"track\":0,\"scene\":0,\"clip\":1}", "{\"track\":0,\"scene\":0,\"clip\":\"01\"}", "{\"track\":0,\"scene\":0,\"clip\":\"-1\"}", "{\"track\":0,\"scene\":0,\"clip\":\"18446744073709551616\"}"})
        require(!cmd::parse("AssignRackClip",juce::JSON::parse(json),error), "rack parser rejects missing, numeric, noncanonical and overflowing source IDs");
    require(!cmd::parse("CreateRackPattern",juce::JSON::parse("{\"extra\":1}"),error), "create pattern refuses unexpected arguments");
    std::cout << "PASS: rack creation/assignment, stable source IDs, Preview/Batch/Undo, no-ops and lifecycle guards\n";
}
