#pragma once
static void removeClipAudioChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate=8000; audio->channels=1; audio->frames=80;
    audio->left.assign(80,.2f); audio->buildWaveform();
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("remove-command.wav");
    require(c.installAudio(file,0,0,audio),"remove audio fixture"); c.setClipPattern(0,0,patternWith(65),false);
    require(c.copyCell(0,0) && c.pasteCell(1,0,true),"linked note/audio fixture"); c.markAsNewBaseline();
    const auto before=c.document(); const auto history=historyOf(c);
    const auto preview=commands.preview(cmd::RemoveClipAudio{0,0});
    require(preview.ok && preview.changed && !preview.after.clip(0,0).isAudioClip() && !preview.after.clip(1,0).isAudioClip()
        && preview.after.clip(0,0).notes().notes[0].pitch==65 && preview.after.cellClip(0,0)==before.cellClip(0,0),"Preview removes shared audio while retaining notes and identity");
    require(alien::sameContent(before,c.document()) && historyOf(c)==history,"remove Preview leaves live document/history untouched");
    engine->playing.store(true); engine->requestedPianoRollClip[0].store(0); engine->requestedPianoRollClip[1].store(2);
    alien::SessionClipLaunch launch; for(auto& mailbox:engine->pianoRollLaunches)mailbox.consume(launch);
    require(commands.execute(cmd::RemoveClipAudio{0,0}).changed && alien::sameContent(preview.after,c.document())
        && engine->playing.load() && engine->requestedPianoRollClip[1].load()==2,"live removal matches Preview and preserves unrelated pending launch");
    require(engine->pianoRollLaunches[0].consume(launch) && !launch.audio && launch.pattern.count==1
        && !engine->pianoRollLaunches[1].consume(launch),"only affected requested content is republished");
    require(c.undo() && !c.canUndo() && alien::sameContent(before,c.document()) && c.document().clip(1,0).audio==audio && c.redo(),"one-step Undo/Redo restores shared media and content");
    const auto noOp=historyOf(c); require(commands.execute(cmd::RemoveClipAudio{0,0}).ok && historyOf(c)==noOp,"missing audio is a no-op");
    require(c.undo(),"redo preservation fixture"); const auto redo=historyOf(c);
    require(!commands.batch({cmd::RemoveClipAudio{0,0},cmd::RemoveClipAudio{99,0}},"Bad removal").ok
        && alien::sameContent(before,c.document()) && historyOf(c)==redo,"invalid later removal refuses the full batch");
    auto frozen=before; frozen.tracks[1].settings.freezeScene=0;
    require(cmd::refusal(frozen,cmd::RemoveClipAudio{0,0}).isNotEmpty(),"frozen linked notes/audio cannot be mutated indirectly");
    auto offline=before; alien::SessionClip missing; missing.audioFilePath="missing-remove.wav"; offline.setClip(2,2,missing);
    require(cmd::applyTo(offline,cmd::RemoveClipAudio{2,2}).isEmpty() && !offline.cellClip(2,2),"offline audio removal clears default empty cell without file access");
    require(c.installAudio(file,2,0,audio) && c.copyCell(2,0) && c.pasteCell(3,0,true),"audio-only linked fixture");
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
