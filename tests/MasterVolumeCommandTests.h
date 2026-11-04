#pragma once
static void masterVolumeCommandChecks() {
    auto engine=std::make_unique<ToneEngine>();alien::ProjectController c(*engine);cmd::CommandEngine commands(c);c.pushDocumentToEngine();c.markAsNewBaseline();
    const auto before=c.document();const auto history=historyOf(c);engine->playing.store(true);engine->requestedPianoRollClip[0].store(2);
    const auto preview=commands.preview(cmd::SetMasterVolume{2.5});
    require(preview.ok && preview.after.masterGain==2.5f && alien::sameContent(before,c.document()) && engine->masterControls.gain.load()==1 && historyOf(c)==history,"master Preview changes no live document, engine or history");
    require(commands.execute(cmd::SetMasterVolume{2.5}).changed && alien::sameContent(preview.after,c.document()) && engine->masterControls.gain.load()==2.5f
        && c.undoLabel()=="Set master volume" && engine->playing.load() && engine->requestedPianoRollClip[0].load()==2,"master command publishes gain without disturbing transport/queued playback");
    require(c.undo() && !c.canUndo() && c.document().masterGain==1 && engine->masterControls.gain.load()==1 && c.redo(),"master command owns one Undo/Redo step");
    require(c.undo(),"master redo preservation fixture");const auto redo=historyOf(c);
    require(commands.execute(cmd::SetMasterVolume{1}).ok && historyOf(c)==redo,"master no-op preserves redo");
    for(double gain:{-.01,4.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        require(!commands.execute(cmd::SetMasterVolume{gain}).ok && c.document().masterGain==1 && engine->masterControls.gain.load()==1 && historyOf(c)==redo,"invalid master gain refuses without writes or lost redo");
    require(!commands.batch({cmd::SetMasterVolume{.4},cmd::RenameTrack{99,"Never"}},"Bad master batch").ok && c.document().masterGain==1
        && engine->masterControls.gain.load()==1 && historyOf(c)==redo,"invalid later command prevents master gain prefix publication");
    require(commands.batch({cmd::SetMasterVolume{0},cmd::SetMasterVolume{4}},"Master endpoints").changed && c.document().masterGain==4
        && engine->masterControls.gain.load()==4 && c.undo() && c.document().masterGain==1,"master endpoints and sequential batch undo");
    c.setExternalBusy(true);require(!commands.execute(cmd::SetMasterVolume{.3}).ok && engine->masterControls.gain.load()==1,"busy master refuses before engine write");c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::recording);
    require(commands.execute(cmd::SetMasterVolume{.6}).changed,"master mixing remains available while recording");engine->recording.phase.store(alien::RecordControls::idle);
    juce::String error;const cmd::Command edit=cmd::SetMasterVolume{.75};const auto parsed=cmd::parse(cmd::name(edit),cmd::arguments(edit),error);
    require(parsed && cmd::name(*parsed)=="SetMasterVolume" && juce::JSON::toString(cmd::arguments(*parsed))==juce::JSON::toString(cmd::arguments(edit)),"master name/arguments round trip");
    for(const char* json:{"{}","{\"gain\":true}","{\"gain\":\"1\"}","{\"gain\":1,\"track\":0}"})
        require(!cmd::parse("SetMasterVolume",juce::JSON::parse(json),error),"master parsing rejects missing, mistyped and extra arguments");
}
