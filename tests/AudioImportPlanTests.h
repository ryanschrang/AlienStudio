#pragma once
static void audioImportPlanChecks() {
    auto engine=std::make_unique<ToneEngine>();alien::ProjectController c(*engine);c.pushDocumentToEngine();
    auto audio=std::make_shared<alien::SessionAudioClip>();audio->sampleRate=8000;audio->channels=1;audio->frames=80;audio->left.assign(80,.2f);audio->buildWaveform();
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("prepared-import",".wav",false);
    c.setClipPattern(0,0,patternWith(60),false);c.markAsNewBaseline();
    const auto plan=c.prepareAudioImport(file,0,0);require(plan.has_value(),"prepare captures editable destination without decoding");
    const alien::session_audio_import::Prepared prepared{*plan,{audio,{}}};const auto before=c.document();const auto available=c.availableAudioBytes();
    auto preview=before;
    require(alien::session_audio_import::apply(preview,prepared).isEmpty() && preview.clip(0,0).audio==audio
        && preview.clip(0,0).notes().notes[0].pitch==60 && alien::sameContent(before,c.document()) && c.availableAudioBytes()==available && !c.canUndo(),"pure prepared result preview changes no live retention/history or document");
    require(c.completeAudioImport(prepared) && alien::sameContent(preview,c.document()) && c.undoLabel()=="Import audio", "validated prepared installation matches pure candidate");
    require(c.undo() && !c.canUndo() && alien::sameContent(before,c.document()) && c.redo() && c.document().clip(0,0).audio==audio,"prepared import owns exactly one Undo/Redo step");
    require(c.undo(),"stale prepared fixture");
    c.setClipPattern(0,0,patternWith(72),false);c.commit("Changed while decoding");const auto changed=c.document();const auto bytes=c.availableAudioBytes();const auto label=c.undoLabel();
    require(!c.completeAudioImport(prepared) && alien::sameContent(changed,c.document()) && c.availableAudioBytes()==bytes && c.undoLabel()==label,"changed destination content refuses before retention/history");
    const auto follows=c.prepareAudioImport(file,0,0);require(follows.has_value() && c.moveTrack(0,1) && c.moveScene(0,1),"reordered destination fixture");c.markAsNewBaseline();c.selectClip(3,3);
    require(c.completeAudioImport({*follows,{audio,{}}}) && c.document().clip(1,1).audio==audio && !c.document().clip(0,0).audio
        && c.document().selectedTrack==3 && c.document().selectedScene==3,"stable identities follow reordered destination without redirecting to current selection");
    const auto replaced=c.prepareAudioImport(file,2,2);require(replaced.has_value(),"project replacement fixture");c.newProject();const auto fresh=c.document();const auto freshBytes=c.availableAudioBytes();
    require(!c.completeAudioImport({*replaced,{audio,{}}}) && alien::sameContent(fresh,c.document()) && c.availableAudioBytes()==freshBytes && !c.canUndo(),"project replacement invalidates old plan without retaining media or changing history");
    const auto missing=c.prepareAudioImport(file,2,2);require(missing.has_value() && c.deleteTrack(2),"deleted target fixture");const auto deleted=c.document();
    require(!c.completeAudioImport({*missing,{audio,{}}}) && alien::sameContent(deleted,c.document()),"deleted track identity cannot alias replacement row");
    const auto valid=c.prepareAudioImport(file,0,0);require(valid.has_value(),"lifecycle fixture");
    for(const auto phase:{alien::RecordControls::countIn,alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);require(!c.prepareAudioImport(file,0,0) && !c.completeAudioImport({*valid,{audio,{}}}),"recording/count-in refuses planning and installation");
    }
    engine->recording.phase.store(alien::RecordControls::idle);c.setExternalBusy(true);
    require(!c.prepareAudioImport(file,0,0) && !c.completeAudioImport({*valid,{audio,{}}}),"busy work refuses planning and installation");c.setExternalBusy(false);
    const auto rejected=c.document();const auto rejectedBytes=c.availableAudioBytes();auto invalid=std::make_shared<alien::SessionAudioClip>();
    require(!c.completeAudioImport({*valid,{invalid,{}}}) && !c.completeAudioImport({*valid,{{},"decode failed"}})
        && alien::sameContent(rejected,c.document()) && c.availableAudioBytes()==rejectedBytes,"invalid/decode-failed results refuse before retention/document changes");
    auto frozen=c.document();frozen.tracks[0].settings.freezeScene=0;
    require(!alien::session_audio_import::prepare(frozen,file,0,0),"frozen destination refuses before decoding");
    require(c.assignRackClip(0,0,c.channelRack().clips[0]) && !c.prepareAudioImport(file,0,0),"rack assignments refuse rather than reporting a lost audio import as successful");
    c.setClipPattern(1,0,patternWith(65),false);require(c.copyCell(1,0) && c.pasteCell(2,0,true),"frozen alias fixture");frozen=c.document();frozen.tracks[2].settings.freezeScene=0;
    require(!alien::session_audio_import::prepare(frozen,file,1,0),"frozen linked content refuses planning");
    // Real worker completion: selection changes while decode runs do not redirect it.
    std::atomic<bool> cancel{false};juce::String error;require(alien::writeClipWav(file,*audio,cancel,error),"write async import fixture");
    c.importAudio(file,1,2);require(c.isBusy(),"real decoder starts asynchronously");c.selectClip(0,3);
    for(int i=0;i<400 && c.isBusy();++i)pump(10);
    require(!c.isBusy() && c.document().clip(1,2).hasAudio() && !c.document().clip(0,3).hasAudio(),"real decode installs captured destination after selection change");
    file.deleteFile();
    std::cout<<"PASS: prepared audio import identities, stale refusal, pure candidate, retention/history and real async decode\n";
}
