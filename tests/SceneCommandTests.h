#pragma once
static void sceneCommandChecks() {
    ToneEngine engine; alien::ProjectController c(engine); cmd::CommandEngine commands(c); c.pushDocumentToEngine();
    c.setClipPattern(0, 0, patternWith(64), false); c.commit("Fixture");
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate = 8000; audio->channels = 1; audio->frames = 80; audio->left.assign(80, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("scene-copy-fixture.wav"), 1, 0, audio), "audio scene fixture");
    const auto before = c.document(); const auto history = historyOf(c);
    const auto identity = alien::newObjectId();
    const std::vector<cmd::Command> edits{cmd::CreateScene{}, cmd::DuplicateScene{0}, cmd::RenameScene{1, "  Copy  "}, cmd::MoveScene{1, 4}, cmd::DeleteScene{2}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.after.sceneCount() == 5 && preview.changes.size() == 5, "scene batch previews sequential document changes");
    require(alien::newObjectId() == identity + 1 && alien::sameContent(before, c.document()) && historyOf(c) == history, "scene preview consumes no live object IDs and touches no document/history");
    // The probe above deliberately issued an ID. Re-preview immediately before
    // execution to compare actual object identities as well as content.
    const auto predicted = commands.preview(edits);
    require(commands.batch(edits, "Edit scenes").ok && alien::sameContent(predicted.after, c.document()), "scene preview equals live batch including scene identities");
    require(c.document().clip(1,3).audio == audio && c.document().cellClip(1,3) != c.document().cellClip(1,0), "duplicated audio retains immutable media with independent clip identity");
    require(c.undo() && alien::sameContent(c.document(), before), "scene batch undoes once");
    require(c.redo() && alien::sameContent(c.document(), predicted.after), "scene batch redoes once");
    require(c.undo(), "set up redo preservation");
    const auto redo = historyOf(c); const auto queued = engine.requestedPianoRollClip[0].load();
    const auto refused = commands.batch({cmd::RenameScene{0, "Never applied"}, cmd::DeleteScene{999}}, "Invalid scenes");
    require(!refused.ok && refused.failedCommand == 1 && historyOf(c) == redo && alien::sameContent(c.document(), before)
        && engine.requestedPianoRollClip[0].load() == queued, "invalid later scene command refuses without any writes or playback reset");
    require(commands.execute(cmd::RenameScene{0, c.document().scenes[0].name}).ok && historyOf(c) == redo, "unchanged scene rename preserves redo");
    c.setExternalBusy(true); require(!commands.execute(cmd::CreateScene{}).ok, "busy scene command refused"); c.setExternalBusy(false);
    engine.recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0,"No"}, cmd::CreateScene{}}, "Recording scenes").ok && alien::sameContent(c.document(), before), "count-in refuses mixed scene batch before earlier edits");
    engine.recording.phase.store(alien::RecordControls::idle);
    for (const auto& edit : edits) {
        juce::String error; const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && cmd::name(*parsed) == cmd::name(edit) && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "scene command arguments round trip");
    }
    for (const char* json : {"{\"scene\":1.5}", "{\"scene\":true}", "{\"scene\":0,\"unexpected\":1}"}) {
        juce::String error; require(!cmd::parse("DeleteScene", juce::JSON::parse(json), error), "strict scene argument parsing");
    }
    const auto createTrack = commands.preview(cmd::CreateTrack{});
    require(commands.execute(cmd::CreateTrack{}).ok && alien::sameContent(createTrack.after, c.document()), "track creation preview also preserves and predicts runtime identities");

    ToneEngine rackEngine; alien::ProjectController rackController(rackEngine); cmd::CommandEngine rackCommands(rackController);
    require(rackController.assignRackClip(0, 0, rackController.document().channelRack.clips[0]), "rack scene fixture");
    const auto rackPreview = rackCommands.preview(cmd::DuplicateScene{0});
    require(rackPreview.ok && rackCommands.execute(cmd::DuplicateScene{0}).ok && alien::sameContent(rackPreview.after, rackController.document()), "rack scene copy preview matches live independent content");
    require(rackController.document().cellClip(0,0) != rackController.document().cellClip(0,1), "scene duplication forks rack content");
    auto fullRackDocument = rackController.document(); auto bank = fullRackDocument.channelRack;
    while (bank.addPattern(0)) {}
    require(fullRackDocument.setRack(std::move(bank)), "full rack bank fixture");
    const auto fullRackBefore = fullRackDocument;
    require(cmd::applyTo(fullRackDocument, cmd::DuplicateScene{0}).isNotEmpty() && alien::sameContent(fullRackDocument, fullRackBefore), "rack capacity refusal leaves document unchanged");
    auto frozen = c.document(); frozen.tracks[0].settings.freezeScene = 0;
    require(cmd::refusal(frozen, cmd::DeleteScene{0}).isNotEmpty(), "frozen source scene deletion refused");
    auto single = alien::ProjectDocument::createDefault(1, 1);
    require(cmd::refusal(single, cmd::DeleteScene{0}).isNotEmpty(), "last scene deletion refused");
    const auto limitId = alien::newObjectId();
    std::vector<cmd::Command> full;
    for (int i = c.document().sceneCount(); i <= alien::maxSessionScenes; ++i) full.push_back(cmd::CreateScene{});
    require(!commands.preview(full).ok && alien::newObjectId() == limitId + 1, "scene-limit refusal consumes no live identities");
}
