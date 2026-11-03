#pragma once
static void allClipDuplicateChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.setClipPattern(0,0,patternWith(60),false); require(c.copyCell(0,0), "unrelated clipboard fixture");
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate = 8000; audio->channels = 1;
    audio->frames = 80; audio->left.assign(80,.2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("duplicate-audio.wav"),1,0,audio), "audio duplicate fixture");
    require(c.assignRackClip(2,0,c.channelRack().clips[0]), "rack duplicate fixture");
    c.selectClip(3,3); c.markAsNewBaseline(); const auto before = c.document(); const auto history = historyOf(c);
    const std::vector<cmd::Command> edits{cmd::DuplicateClip{1,0,1,1},cmd::DuplicateClip{1,1,1,2},cmd::DuplicateClip{2,0,2,1},cmd::DuplicateClip{2,1,2,2}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.changed && preview.after.clip(1,1).audio == audio && preview.after.clip(1,2).audio == audio
        && preview.after.cellClip(1,1) != before.cellClip(1,0) && preview.after.linkOf(1,1) != before.linkOf(1,0), "audio duplicates share immutable media but own independent clip content");
    require(alien::sameContent(before,c.document()) && historyOf(c) == history && c.clipClipboard().source == before.cellClip(0,0), "all-type Preview changes neither live IDs/document/history nor clipboard");
    require(commands.batch(edits,"Duplicate audio and rack").changed && alien::sameContent(preview.after,c.document())
        && c.document().channelRack.patternCount == before.channelRack.patternCount + 2
        && c.document().clip(2,1).rackClip != before.clip(2,0).rackClip
        && c.document().clip(2,2).rackClip != c.document().clip(2,1).rackClip, "rack duplicates allocate distinct independent sources and match sequential Preview IDs");
    require(c.document().selectedTrack == 2 && c.document().selectedScene == 2 && c.undo() && !c.canUndo()
        && alien::sameContent(before,c.document()) && c.document().selectedTrack == 3 && c.document().selectedScene == 3,
        "one batch Undo restores audio/rack identities, source bank and before-selection");
    require(c.redo() && c.document().selectedTrack == 2 && c.document().selectedScene == 2 && c.undo(), "Redo restores duplicate destination focus");
    require(commands.execute(cmd::PasteClip{3,3}).changed && c.document().clip(3,3).notes().notes[0].pitch == 60 && c.undo(), "duplication does not overwrite the user's clipboard");
    const auto redo = historyOf(c);
    require(!commands.batch({cmd::DuplicateClip{1,0,1,1},cmd::DuplicateClip{2,0,0,0}},"Bad duplicate").ok
        && alien::sameContent(before,c.document()) && historyOf(c) == redo, "invalid later duplicate refuses the complete batch");
    auto offline = before; alien::SessionClip missing; missing.audioFilePath = "missing-duplicate.wav"; offline.setClip(3,0,missing);
    require(cmd::applyTo(offline,cmd::DuplicateClip{3,0,3,1}).isEmpty() && offline.clip(3,1).audioOffline()
        && offline.cellClip(3,0) != offline.cellClip(3,1) && offline.linkOf(3,0) != offline.linkOf(3,1), "offline audio duplicates without decoding or linking its content");
    auto full = before; auto bank = full.channelRack; while (bank.addPattern(0)) {}
    require(full.setRack(std::move(bank)), "full rack fixture"); const auto fullBefore = full;
    require(cmd::applyTo(full,cmd::DuplicateClip{2,0,2,1}).isNotEmpty() && alien::sameContent(fullBefore,full), "rack capacity refusal leaves no prefix pattern or cell");
    auto almostFull = before; bank = almostFull.channelRack; while (bank.patternCount < alien::rack::maxPatterns-1) require(bank.addPattern(0), "one rack slot remaining fixture");
    require(almostFull.setRack(std::move(bank)) && cmd::applyTo(almostFull,cmd::DuplicateClip{2,0,2,1}).isEmpty()
        && cmd::applyTo(almostFull,cmd::DuplicateClip{2,1,2,2}).isNotEmpty(), "sequential rack copies account for remaining capacity");
    auto frozen = before; frozen.tracks[1].settings.freezeScene = 0;
    require(cmd::refusal(frozen,cmd::DuplicateClip{1,0,1,1}).isNotEmpty() && cmd::refusal(frozen,cmd::DuplicateClip{1,0,3,1}).isEmpty(), "frozen destination refuses while read-only source duplication is allowed");
    require(c.placeRegion(3,1,0), "empty Song cell reference fixture");
    const auto protectedDoc = c.document();
    require(!commands.execute(cmd::DuplicateClip{2,0,3,1}).ok && alien::sameContent(protectedDoc,c.document()), "rack copy refuses Song-bound destination before live mutation");
    c.setExternalBusy(true); require(!commands.execute(cmd::DuplicateClip{1,0,1,1}).ok, "busy all-type duplicate refuses"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0,"Never"},cmd::DuplicateClip{1,0,1,1}},"Count-in copy").ok
        && alien::sameContent(protectedDoc,c.document()), "count-in rejects entire all-type batch before mutation"); engine->recording.phase.store(alien::RecordControls::idle);
    engine->playing.store(true); engine->activePianoRollClip[1].store(1); engine->requestedPianoRollClip[1].store(3);
    alien::SessionClipLaunch launch; for (auto& mailbox : engine->pianoRollLaunches) mailbox.consume(launch);
    require(commands.execute(cmd::DuplicateClip{1,0,1,1}).changed && engine->playing.load() && engine->requestedPianoRollClip[1].load() == 3
        && !engine->pianoRollLaunches[1].consume(launch), "duplicate preserves another scene's pending launch");
    engine->requestedPianoRollClip[1].store(2);
    require(commands.execute(cmd::DuplicateClip{1,0,1,2}).changed && engine->pianoRollLaunches[1].consume(launch) && launch.audio == audio.get(), "already-requested audio destination receives the duplicate");
    std::cout << "PASS: all-type DuplicateClip, independent audio/rack content, clipboard isolation, Preview/Undo and capacity guards\n";
}
