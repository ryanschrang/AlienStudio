#pragma once
static void removeClipAudioChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate=8000; audio->channels=1; audio->frames=80;
    audio->left.assign(80,.2f); audio->buildWaveform();
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("remove-command.wav");
    require(alien::command::installDecodedAudio(c,file,0,0,audio).ok,"remove audio fixture"); c.setClipPattern(0,0,patternWith(65),false);
    require(c.copyCell(0,0) && alien::command::CommandEngine(c).execute(alien::command::PasteLinkedClip{1, 0}).changed,"linked note/audio fixture"); c.markAsNewBaseline();
    const auto before=c.document(); const auto history=historyOf(c);
    const std::vector<cmd::Command> edits{cmd::RemoveClipAudio{0,0},cmd::SetTrackPan{0,.27}};
    const auto preview=commands.preview(edits);
    require(preview.ok && preview.changed && !preview.after.clip(0,0).isAudioClip() && !preview.after.clip(1,0).isAudioClip()
        && preview.after.clip(0,0).notes().notes[0].pitch==65 && preview.after.cellClip(0,0)==before.cellClip(0,0),"Preview removes shared audio while retaining notes and identity");
    require(alien::sameContent(before,c.document()) && historyOf(c)==history,"remove Preview leaves live document/history untouched");
    engine->playing.store(true); engine->requestedPianoRollClip[0].store(0); engine->requestedPianoRollClip[1].store(2);
    alien::SessionClipLaunch launch; for (std::size_t t = 0; t < engine->pianoRollLaunches.size(); ++t) engine->pianoRollLaunches[t].consume(launch);
    require(commands.batch(edits,"Remove audio and mix").changed && alien::sameContent(preview.after,c.document()) && engine->tracks[0].pan.load()==.27f
        && engine->playing.load() && engine->requestedPianoRollClip[1].load()==2,"live removal matches Preview and preserves unrelated pending launch");
    require(engine->pianoRollLaunches[0].consume(launch) && !launch.audio && launch.pattern.count==1
        && !engine->pianoRollLaunches[1].consume(launch),"only affected requested content is republished");
    require(c.undo() && !c.canUndo() && alien::sameContent(before,c.document()) && c.document().clip(1,0).audio==audio
        && engine->tracks[0].pan.load()==static_cast<float>(before.tracks[0].settings.pan)
        && c.redo() && alien::sameContent(preview.after,c.document()) && engine->tracks[0].pan.load()==.27f,"one-step Undo/Redo restores shared media, content and live mix");
    const auto noOp=historyOf(c); require(commands.execute(cmd::RemoveClipAudio{0,0}).ok && historyOf(c)==noOp,"missing audio is a no-op");
    require(c.undo(),"redo preservation fixture"); const auto redo=historyOf(c);
    for (std::size_t t = 0; t < engine->pianoRollLaunches.size(); ++t) engine->pianoRollLaunches[t].consume(launch);
    const auto late=commands.batch({edits[0],edits[1],cmd::RemoveClipAudio{99,0}},"Bad removal");
    c.setExternalBusy(true); const auto busy=commands.batch(edits,"Busy removal"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    const auto counting=commands.batch(edits,"Count-in removal"); engine->recording.phase.store(alien::RecordControls::idle);
    require(!late.ok && late.failedCommand==2 && !busy.ok && !counting.ok
        && alien::sameContent(before,c.document()) && historyOf(c)==redo
        && engine->tracks[0].pan.load()==static_cast<float>(before.tracks[0].settings.pan)
        && !engine->pianoRollLaunches[0].consume(launch) && !engine->pianoRollLaunches[1].consume(launch),
        "late, busy and count-in removal refuse before live mix/publication or redo changes");
    const auto empty=commands.execute(cmd::RemoveClipAudio{3,3});
    require(empty.ok && !empty.changed && historyOf(c)==redo,"empty-cell removal preserves redo");
    auto frozen=before; frozen.tracks[1].settings.freezeScene=0;
    require(cmd::refusal(frozen,cmd::RemoveClipAudio{0,0}).isNotEmpty(),"frozen linked notes/audio cannot be mutated indirectly");
    auto offline=before; alien::SessionClip missing; missing.audioFilePath="missing-remove.wav"; offline.setClip(2,2,missing);
    require(cmd::applyTo(offline,cmd::RemoveClipAudio{2,2}).isEmpty() && !offline.cellClip(2,2),"offline audio removal clears default empty cell without file access");
    require(alien::command::installDecodedAudio(c,file,2,0,audio).ok && c.copyCell(2,0) && alien::command::CommandEngine(c).execute(alien::command::PasteLinkedClip{3, 0}).changed,"audio-only linked fixture");
    require(commands.execute(cmd::RemoveClipAudio{2,0}).changed && !c.document().cellClip(2,0) && c.document().clip(3,0).audio==audio,
        "default empty remainder clears only selected cell, preserving linked audio");
    c.setExternalBusy(true); require(!commands.execute(cmd::RemoveClipAudio{3,0}).ok,"busy removal refuses"); c.setExternalBusy(false);
    const auto guarded=c.document(); engine->recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0,"Never"},cmd::RemoveClipAudio{3,0}},"Count-in removal").ok && alien::sameContent(guarded,c.document()),"count-in refuses mixed batch before edits");
    engine->recording.phase.store(alien::RecordControls::idle);
    juce::String error; const cmd::Command edit=cmd::RemoveClipAudio{3,0}; const auto parsed=cmd::parse(cmd::name(edit),cmd::arguments(edit),error);
    require(parsed && juce::JSON::toString(cmd::arguments(*parsed))==juce::JSON::toString(cmd::arguments(edit)),"remove audio arguments round-trip");
    require(!cmd::parse("RemoveClipAudio",juce::JSON::parse("{\"track\":0,\"scene\":true}"),error),"remove audio refuses mistyped coordinates");
}
