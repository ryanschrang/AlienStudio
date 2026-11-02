#pragma once
static void moveClipCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.setClipPattern(0, 0, patternWith(64), false);
    require(c.copyCell(0, 0) && c.pasteCell(1, 0, true), "move link fixture");
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate = 8000; audio->channels = 1;
    audio->frames = 80; audio->left.assign(80, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("move-audio.wav"), 2, 0, audio), "move audio fixture");
    require(c.assignRackClip(3, 0, c.channelRack().clips[0]), "move rack fixture");
    c.selectClip(1, 3); c.markAsNewBaseline();
    const auto before = c.document(); const auto history = historyOf(c); const auto sourceId = before.cellClip(0, 0);
    alien::SessionClipLaunch launch; for (auto& mailbox : engine->pianoRollLaunches) mailbox.consume(launch);
    const std::vector<cmd::Command> edits{cmd::MoveClip{0, 0, 0, 1}, cmd::MoveClip{0, 1, 1, 1}, cmd::MoveClip{2, 0, 2, 1}, cmd::MoveClip{3, 0, 3, 1}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.changed && preview.after.cellClip(1, 1) == sourceId
        && preview.after.linkOf(1, 1) == before.linkOf(1, 0) && preview.after.clip(2, 1).audio == audio
        && preview.after.cellClip(3, 1) == before.cellClip(3, 0), "Move Preview preserves MIDI links, audio and rack identities across sequential moves");
    require(alien::sameContent(before, c.document()) && historyOf(c) == history && !engine->pianoRollLaunches[0].consume(launch), "Move Preview leaves document/history/engine untouched");
    require(commands.batch(edits, "Move clips").changed && alien::sameContent(preview.after, c.document())
        && c.document().channelRack == before.channelRack && c.document().selectedTrack == 3 && c.document().selectedScene == 1,
        "live moves match Preview, preserve rack sources and select final destination");
    require(c.undo() && !c.canUndo() && alien::sameContent(before, c.document()) && c.document().selectedTrack == 1 && c.document().selectedScene == 3,
        "one batch Undo restores exact cells, identities, media and original selection");
    require(c.redo() && c.document().selectedTrack == 3 && c.document().selectedScene == 1 && c.undo(), "Redo focuses final move destination");
    const auto redo = historyOf(c);
    auto offline = before; alien::SessionClip missing; missing.audioFilePath = "missing-move.wav";
    offline.setClip(2,2,missing); const auto offlineId = offline.cellClip(2,2);
    require(cmd::applyTo(offline,cmd::MoveClip{2,2,2,3}).isEmpty() && offline.cellClip(2,3) == offlineId && offline.clip(2,3).audioOffline(), "offline audio moves without decoding or a new identity");
    auto empty = before;
    require(cmd::applyTo(empty,cmd::CreateClip{3,2}).isEmpty(), "explicit empty move fixture");
    const auto emptyId = empty.cellClip(3,2);
    require(cmd::applyTo(empty,cmd::MoveClip{3,2,3,3}).isEmpty() && empty.cellClip(3,3) == emptyId, "explicit empty MIDI identity moves as a clip");
    require(commands.execute(cmd::MoveClip{0, 0, 0, 0}).ok && !commands.preview(cmd::MoveClip{0, 0, 0, 0}).changed
        && historyOf(c) == redo, "same-cell move is a no-op preserving redo");
    for (const cmd::Command& edit : std::vector<cmd::Command>{cmd::MoveClip{-1,0,0,1},cmd::MoveClip{0,0,99,1},cmd::MoveClip{0,3,0,1},cmd::MoveClip{0,0,1,0}})
        require(!commands.execute(edit).ok && alien::sameContent(before, c.document()) && historyOf(c) == redo, "invalid/missing/occupied moves refuse without mutation");
    require(!commands.batch({cmd::MoveClip{0,0,0,1},cmd::MoveClip{0,1,1,0}}, "Bad move").ok && alien::sameContent(before,c.document()), "invalid later move prevents a prefix move");
    auto frozen = before; frozen.tracks[0].settings.freezeScene = 0;
    require(cmd::refusal(frozen,cmd::MoveClip{0,0,1,1}).isNotEmpty() && cmd::refusal(frozen,cmd::MoveClip{1,0,0,1}).isNotEmpty(), "frozen source and destination tracks refuse");
    require(c.placeRegion(0,0,0), "Song-bound move fixture");
    const auto song = c.document();
    require(!commands.execute(cmd::MoveClip{0,0,0,1}).ok && alien::sameContent(song,c.document()), "Song cell source cannot move silently");
    auto destination = before; auto region = song.timeline.regions[0]; region.row = 0; region.source = 1;
    for (int kind = 0; kind < 3; ++kind) {
        auto doc = destination;
        if (kind == 0) doc.timeline.regions.push_back(region); else if (kind == 1) doc.timeline.takes.push_back(region); else doc.timeline.frozenOriginals.push_back(region);
        require(cmd::refusal(doc,cmd::MoveClip{0,0,0,1}).isNotEmpty(), "empty destination referenced by Song/take/frozen-original cannot acquire moved content");
    }
    c.setExternalBusy(true); require(!commands.execute(cmd::MoveClip{1,0,1,1}).ok, "busy move refuses"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0,"Never"},cmd::MoveClip{1,0,1,1}}, "Recording move").ok && alien::sameContent(song,c.document()), "count-in refuses mixed batch before live effects");
    engine->recording.phase.store(alien::RecordControls::idle);
    engine->playing.store(true); engine->activePianoRollClip[1].store(0); engine->requestedPianoRollClip[1].store(2);
    for (auto& mailbox : engine->pianoRollLaunches) mailbox.consume(launch);
    require(commands.execute(cmd::MoveClip{1,0,1,1}).changed && engine->playing.load() && engine->requestedPianoRollClip[1].load() == 2
        && !engine->pianoRollLaunches[1].consume(launch), "moving active source preserves another scene's queued request");
    engine->requestedPianoRollClip[1].store(1);
    require(commands.execute(cmd::MoveClip{1,1,1,2}).changed && engine->pianoRollLaunches[1].consume(launch) && launch.pattern.count == 0,
        "moving requested source publishes silence instead of leaving its old content");
    engine->requestedPianoRollClip[1].store(3);
    require(commands.execute(cmd::MoveClip{1,2,1,3}).changed && engine->pianoRollLaunches[1].consume(launch) && launch.pattern.count == 1,
        "already-queued destination receives moved content without being auto-launched elsewhere");
    juce::String error; const cmd::Command move = cmd::MoveClip{1,3,0,2}; const auto parsed = cmd::parse(cmd::name(move),cmd::arguments(move),error);
    require(parsed && cmd::name(*parsed) == "MoveClip" && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(move)), "move arguments round trip");
    for (const char* json : {"{}", "{\"track\":0,\"scene\":0,\"toTrack\":1,\"toScene\":1.5}", "{\"track\":0,\"scene\":0,\"toTrack\":true,\"toScene\":1}", "{\"track\":0,\"scene\":0,\"toTrack\":1,\"toScene\":1,\"extra\":1}"})
        require(!cmd::parse("MoveClip",juce::JSON::parse(json),error), "move parsing refuses missing, mistyped and unknown arguments");
    std::cout << "PASS: MoveClip identity, media/links/rack, Preview/Batch/Undo/Redo, guards and queued playback\n";
}
