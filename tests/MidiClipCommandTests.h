#pragma once
static void midiClipCommandChecks() {
    auto engine=std::make_unique<ToneEngine>();alien::ProjectController c(*engine);cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();c.selectClip(3,2);c.markAsNewBaseline();
    const auto before=c.document();const auto history=historyOf(c);
    const auto preview=commands.preview(std::vector<cmd::Command>{cmd::CreateClip{0,0},cmd::DuplicateClip{0,0,1,1}});
    require(preview.ok && preview.changed && preview.after.cellClip(0,0) && preview.after.cellClip(1,1)
        && preview.after.linkOf(0,0)!=preview.after.linkOf(1,1),"Preview creates and independently duplicates an empty MIDI clip");
    require(alien::sameContent(c.document(),before) && historyOf(c)==history,"clip Preview changes neither live IDs/document nor history");
    const auto result=commands.batch({cmd::CreateClip{0,0},cmd::DuplicateClip{0,0,1,1}},"Create two clips");
    require(result.ok && result.changed && alien::sameContent(c.document(),preview.after) && c.document().selectedTrack==1 && c.document().selectedScene==1,
        "live clip batch matches Preview, including deterministic library IDs and destination selection");
    require(c.undoLabel()=="Create two clips" && c.undo() && !c.canUndo() && alien::sameContent(c.document(),before)
        && c.document().selectedTrack==3 && c.document().selectedScene==2,"one batch undo restores cells and selection");
    require(c.redo() && c.document().cellClip(0,0) && c.document().selectedTrack==1 && c.document().selectedScene==1,"redo restores explicit empty clips and destination focus");
    const auto data=alien::toProjectData(c.document());alien::SessionProjectData loaded;juce::String error;
    require(alien::parseSessionProject(alien::serializeSessionProject(data),loaded,error),"explicit empty MIDI clips serialize");
    const auto reopened=alien::fromProjectData(loaded);
    require(reopened.cellClip(0,0) && reopened.cellClip(1,1) && reopened.linkOf(0,0)!=reopened.linkOf(1,1),"empty clip identity and independent copies survive save/reopen");
    require(c.copyCell(0,0) && c.pasteCell(3,3,false) && c.document().cellClip(3,3)
        && c.document().linkOf(3,3)!=c.document().linkOf(0,0),"existing clipboard paste preserves explicitly empty MIDI clips as independent copies");
    require(c.undo() && !c.document().cellClip(3,3),"empty clipboard paste remains one undo step");
    c.setClipPattern(0,0,patternWith(60),false);c.commit("Source notes");
    require(commands.execute(cmd::DuplicateClip{0,0,2,1}).changed,"duplicate populated MIDI clip");
    c.setClipPattern(2,1,patternWith(77),false);c.commit("Copy notes");
    require(c.document().clip(0,0).notes().notes[0].pitch==60 && c.document().clip(2,1).notes().notes[0].pitch==77,"editing duplicate leaves source unchanged");
    require(c.copyCell(0,0) && c.pasteCell(1,2,true),"linked fixture");c.markAsNewBaseline();
    const auto linked=c.document();engine->activePianoRollClip[1].store(2);engine->requestedPianoRollClip[1].store(2);engine->playing.store(true);
    require(commands.execute(cmd::MakeClipUnique{1,2}).changed && c.document().linkOf(1,2)!=c.document().linkOf(0,0)
        && c.undoLabel()=="Make clip independent","make independent uses its named command");
    require(engine->playing.load() && engine->activePianoRollClip[1].load()==2 && engine->requestedPianoRollClip[1].load()==2,"independence preserves transport and queued playback");
    require(c.undo() && alien::sameContent(c.document(),linked) && !c.canUndo(),"one undo restores clip sharing");
    require(c.redo(),"redo independence");
    c.setClipPattern(1,2,patternWith(79),false);c.commit("Independent edit");
    require(c.document().clip(0,0).notes().notes[0].pitch==60,"unique clip edits do not modify its former link");
    require(c.undo(),"prepare no-op redo fixture");const auto noOpHistory=historyOf(c);
    require(commands.execute(cmd::MakeClipUnique{1,2}).ok && !commands.preview(cmd::MakeClipUnique{1,2}).changed
        && historyOf(c)==noOpHistory,"already-independent no-op preserves redo");
    const auto unchanged=c.document();const auto rejectedHistory=historyOf(c);
    for(const cmd::Command& edit:std::vector<cmd::Command>{cmd::CreateClip{0,0},cmd::CreateClip{-1,0},cmd::DuplicateClip{3,3,3,1},cmd::MakeClipUnique{3,3}})
        require(!commands.execute(edit).ok && alien::sameContent(c.document(),unchanged) && historyOf(c)==rejectedHistory,"invalid/occupied/missing cells refuse atomically");
    require(!commands.batch({cmd::CreateClip{3,3},cmd::DuplicateClip{3,3,0,0}},"Invalid batch").ok
        && !c.document().cellClip(3,3) && historyOf(c)==rejectedHistory,"failed batch leaves no prefix clip or consumed live identity");
    auto frozen=unchanged;frozen.tracks[3].settings.freezeScene=0;
    require(!cmd::refusal(frozen,cmd::CreateClip{3,3}).isEmpty(),"frozen destination refuses");
    auto offline=unchanged;alien::SessionClip audio;audio.audioFilePath="missing.wav";offline.setClip(3,3,audio);
    require(cmd::refusal(offline,cmd::DuplicateClip{3,3,3,2}).isEmpty() && cmd::refusal(offline,cmd::MakeClipUnique{3,3}).isEmpty(),"offline audio supports duplication and independence");
    auto rack=unchanged;require(rack.bindRackClip(3,3,rack.channelRack.clips[0]),"rack refusal fixture");
    require(cmd::refusal(rack,cmd::DuplicateClip{3,3,3,2}).isEmpty(),"rack clips support named duplication");
    c.setExternalBusy(true);require(!commands.execute(cmd::CreateClip{3,3}).ok,"busy clip command refuses");c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0,"Wrong"},cmd::CreateClip{3,3}},"Recording batch").ok
        && alien::sameContent(c.document(),unchanged),"count-in refuses the entire mixed batch before mutation");engine->recording.phase.store(alien::RecordControls::idle);
    for(const cmd::Command& edit:std::vector<cmd::Command>{cmd::CreateClip{3,3},cmd::DuplicateClip{0,0,3,3},cmd::MakeClipUnique{1,2}}) {
        auto parsed=cmd::parse(cmd::name(edit),cmd::arguments(edit),error);
        require(parsed && cmd::arguments(*parsed).getDynamicObject()->getProperties()==cmd::arguments(edit).getDynamicObject()->getProperties(),"clip command arguments round-trip");
    }
    auto bad=cmd::arguments(cmd::DuplicateClip{0,0,3,3});bad.getDynamicObject()->setProperty("toScene",1.5);
    require(!cmd::parse("DuplicateClip",bad,error),"fractional destination refused");
    bad=cmd::arguments(cmd::CreateClip{3,3});bad.getDynamicObject()->setProperty("extra",true);
    require(!cmd::parse("CreateClip",bad,error),"unknown clip arguments refused");
    std::cout<<"PASS: Session MIDI clip commands, pure Preview, batch/undo, independent links, identity persistence and refusal\n";
}
