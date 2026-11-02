#pragma once
static void deleteClipCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.setClipPattern(0, 0, patternWith(65), false);
    require(c.copyCell(0, 0) && c.pasteCell(1, 0, true), "linked delete fixture");
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 80;
    audio->left.assign(80, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("delete-fixture.wav"), 2, 0, audio), "audio delete fixture");
    const auto rackId = c.document().channelRack.clips[0];
    require(c.assignRackClip(3, 0, rackId), "rack delete fixture");
    require(commands.execute(cmd::CreateClip{0, 1}).changed, "explicit empty MIDI fixture");
    require(c.placeRegion(0, 0, 0), "Song cell placement fixture");
    c.selectClip(2, 2); c.markAsNewBaseline();
    const auto before = c.document(); const auto history = historyOf(c);
    const auto audioId = before.cellClip(2, 0);
    engine->playing.store(true);
    engine->activePianoRollClip[0].store(0); engine->requestedPianoRollClip[0].store(0);
    engine->activePianoRollClip[1].store(0); engine->requestedPianoRollClip[1].store(0);
    alien::SessionClipLaunch launch;
    for (auto& mailbox : engine->pianoRollLaunches) mailbox.consume(launch);
    const std::vector<cmd::Command> edits{cmd::DeleteClip{0, 0}, cmd::DeleteClip{2, 0}, cmd::DeleteClip{3, 0}, cmd::DeleteClip{0, 1}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.changed && preview.changes.size() == 4
        && !preview.after.cellClip(0, 0) && !preview.after.cellClip(2, 0)
        && !preview.after.cellClip(3, 0) && !preview.after.cellClip(0, 1), "Preview removes MIDI/audio/rack/explicit empty cells");
    require(alien::sameContent(before, c.document()) && historyOf(c) == history
        && !engine->pianoRollLaunches[0].consume(launch), "delete Preview has no live document/history/engine effects");
    require(preview.after.clip(1, 0).notes().notes[0].pitch == 65
        && preview.after.channelRack == before.channelRack
        && preview.after.library.clip(alien::library::ClipId{rackId}) != nullptr,
        "delete preserves linked content and shared rack source");
    require(commands.batch(edits, "Delete selected clips").changed
        && alien::sameContent(c.document(), preview.after), "live delete batch matches Preview");
    require(c.document().selectedTrack == 2 && c.document().selectedScene == 2
        && engine->playing.load() && engine->requestedPianoRollClip[1].load() == 0,
        "delete preserves selection, transport and unrelated launches");
    require(engine->pianoRollLaunches[0].consume(launch) && launch.pattern.count == 0 && launch.audio == nullptr
        && !engine->pianoRollLaunches[1].consume(launch), "only the addressed live cell receives an empty launch");
    require(c.document().timeline == before.timeline && c.document().placementContent(c.document().timeline.regions[0]).isEmpty(),
        "Song cell placement remains in place and falls silent");
    require(c.undoLabel() == "Delete selected clips" && c.undo() && !c.canUndo()
        && alien::sameContent(before, c.document()) && c.document().cellClip(2, 0) == audioId
        && c.document().clip(2, 0).audio == audio, "single undo restores exact clip identities, links and audio ownership");
    require(c.redo() && alien::sameContent(preview.after, c.document()), "single redo removes the same cells");
    require(c.undo(), "prepare redo preservation");
    const auto redo = historyOf(c);
    require(commands.execute(cmd::DeleteClip{3, 3}).ok && !commands.preview(cmd::DeleteClip{3, 3}).changed
        && historyOf(c) == redo, "absent-cell no-op preserves redo");
    require(!commands.batch({cmd::DeleteClip{0, 0}, cmd::DeleteClip{99, 0}}, "Invalid delete").ok
        && alien::sameContent(before, c.document()) && historyOf(c) == redo, "invalid later delete refuses the entire batch");
    auto frozen = before; frozen.tracks[0].settings.freezeScene = 0;
    require(cmd::refusal(frozen, cmd::DeleteClip{0, 0}).isNotEmpty(), "frozen delete refuses before mutation");
    auto offline = before; alien::SessionClip missing; missing.audioFilePath = "missing-delete.wav";
    offline.setClip(2, 1, missing);
    require(offline.clip(2, 1).audioOffline() && cmd::applyTo(offline, cmd::DeleteClip{2, 1}).isEmpty()
        && !offline.cellClip(2, 1), "offline audio deletion needs no decoding or file access");
    c.setExternalBusy(true); require(!commands.execute(cmd::DeleteClip{0, 0}).ok, "busy delete refuses"); c.setExternalBusy(false);
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        require(!commands.batch({cmd::RenameTrack{0, "Never"}, cmd::DeleteClip{0, 0}}, "Recording delete").ok
            && alien::sameContent(before, c.document()) && historyOf(c) == redo, "recording/count-in refuses all commands before any prefix edit");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    juce::String error;
    const cmd::Command edit = cmd::DeleteClip{2, 0};
    const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
    require(parsed && cmd::name(*parsed) == "DeleteClip"
        && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "delete stable name and arguments round trip");
    for (const char* json : {"{}", "{\"track\":0,\"scene\":1.5}", "{\"track\":true,\"scene\":0}", "{\"track\":0,\"scene\":0,\"extra\":1}"})
        require(!cmd::parse("DeleteClip", juce::JSON::parse(json), error), "delete strictly validates arguments");
    require(commands.batch({cmd::DeleteClip{0, 0}, cmd::CreateClip{0, 0}, cmd::DeleteClip{0, 0}}, "Sequential delete").changed,
        "delete composes sequentially with MIDI creation");
    std::cout << "PASS: Session delete commands, links/media/rack, Preview, batch/undo and guarded no-ops\n";
}
