#pragma once
static void trackStructureBoundaryChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.setClipPattern(0,0,patternWith(64),false);
    require(commands.execute(cmd::SetRackChannelOutput{0,0}).ok, "route rack channel to track fixture");
    require(commands.execute(cmd::SetTrackVolume{0,.5}).ok, "set track control fixture");
    require(c.editTimeline("Place source", [](auto& timeline) {
        alien::timeline::Region region; region.row=0; region.source=0;
        region.length=alien::PianoRollPattern::ticksPerStep*4;
        return timeline.place(region);
    }), "place track move source");
    c.selectClip(0,0); c.markAsNewBaseline(); c.pushDocumentToEngine(); c.launchClip(0,0);
    engine->liveDrumTrack.store(0);
    const auto before=c.document();
    int captures=0;
    c.beforeStructureEdit=[&] {
        ++captures;
        require(alien::sameContent(before,c.document()), "track callback sees original order before any swaps");
    };
    const auto prediction=commands.preview(cmd::MoveTrack{0,3});
    require(prediction.ok && captures==0, "track Preview does not capture plugins");
    require(commands.execute(cmd::MoveTrack{0,3}).changed && captures==1
        && alien::sameContent(prediction.after,c.document()), "long track move reduces once and agrees with Preview");
    require(c.document().tracks[3].id==before.tracks[0].id && c.document().selectedTrack==3
        && c.document().cellClip(3,0)==before.cellClip(0,0) && c.timeline().regions.front().row==3
        && c.channelRack().channels[0].output==3 && engine->liveDrumTrack.load()==3
        && engine->tracks[3].gain.load()==.5f, "long move carries source, Song, rack, selection, live drum route and mix control");
    require(commands.undo() && alien::sameContent(before,c.document()), "long track move undoes once");
    require(commands.redo() && alien::sameContent(prediction.after,c.document()), "long track move redoes once");
    require(commands.undo(), "restore redo before direct refusals");
    c.launchClip(0,0); engine->liveDrumTrack.store(1);
    const auto queued=engine->requestedPianoRollClip[0].load(); const auto history=historyOf(c); captures=0;
    require(commands.execute(cmd::MoveTrack{0,0}).ok && c.moveTrack(0,0)
        && !c.moveTrack(-1,1) && !c.deleteTrack(999) && !c.renameTrack(0," ")
        && !c.setTrackColour(0,alien::ProjectController::trackColourCount), "track no-op and invalid adapters handled");
    require(captures==0 && alien::sameContent(before,c.document()) && historyOf(c)==history
        && engine->requestedPianoRollClip[0].load()==queued && engine->liveDrumTrack.load()==1,
        "refused/unchanged tracks preserve host, document, redo, launches and live route");
    auto racks=std::vector<alien::PluginRackState>(before.tracks.size()); auto master=before.masterPlugins;
    alien::PluginState effect; effect.descriptionXml="track-capture-fixture"; effect.stateBase64="captured";
    master.effects.push_back(effect);
    c.beforeStructureEdit=[&] { ++captures; c.setPluginRacks(racks,master); };
    require(commands.execute(cmd::MoveTrack{0,3}).changed && captures==1
        && c.document().masterPlugins==master && engine->liveDrumTrack.load()==0,
        "move retains captured plugin state and shifts an intervening live drum route");
    require(commands.undo() && c.document().masterPlugins==master && c.document().tracks[0].id==before.tracks[0].id,
        "track Undo keeps freshly captured plugin state");
    require(commands.redo() && c.document().masterPlugins==master && c.document().tracks[3].id==before.tracks[0].id,
        "track Redo keeps freshly captured state and identity");
    c.beforeStructureEdit={}; engine->liveDrumTrack.store(1);
    require(commands.execute(cmd::MoveTrack{3,0}).changed && engine->liveDrumTrack.load()==2,
        "upward move shifts an intervening live drum route");
    engine->liveDrumTrack.store(0);
    require(commands.execute(cmd::DeleteTrack{0}).changed && engine->liveDrumTrack.load()==-1,
        "removing live drum target clears route");
    while (c.document().trackCount()<alien::maxSessionTracks) require(c.addTrack(), "fill track capacity");
    const auto full=c.document(); const auto fullHistory=historyOf(c); captures=0;
    c.beforeStructureEdit=[&] { ++captures; };
    const auto identity=alien::newObjectId();
    require(!c.addTrack() && captures==0 && alien::newObjectId()==identity+1
        && alien::sameContent(full,c.document()) && historyOf(c)==fullHistory,
        "track capacity refuses before host capture, live IDs, document or history changes");
}
