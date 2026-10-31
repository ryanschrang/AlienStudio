#pragma once
// Included after the TimelineViewTests Rig: real menus/drag handlers plus worker
// preparation, cache decoding and controller history, without an audio device.
static void waitForWarp(ProjectController& controller) {
    const auto end=juce::Time::getMillisecondCounter()+30000;
    while(controller.isBusy() && juce::Time::getMillisecondCounter()<end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    require(!controller.isBusy(),"warp background preparation completes");
}
static std::shared_ptr<SessionAudioClip> warpTestSine() {
    auto clip=std::make_shared<SessionAudioClip>();clip->sampleRate=8000;clip->channels=2;clip->frames=16000;
    clip->left.resize(clip->frames);clip->right.resize(clip->frames);
    for(std::size_t i=0;i<clip->frames;++i) {clip->left[i]=static_cast<float>(.4*std::sin(2*3.141592653589793*220*i/8000));clip->right[i]=-clip->left[i];}
    clip->buildWaveform();return clip;
}
static void warpRenderChecks() {
    auto source=warpTestSine();timeline::Region region;region.length=1920;region.warp={{0,0},{960,.5},{1920,2}};
    timeline::Timeline song;song.tempo=120;require(song.setTempoChange(960,60),"warp tempo fixture");
    std::atomic<bool> cancel{false};juce::String error;
    auto rendered=renderRegionWarp(*source,region,song,0,cancel,error);
    require(rendered && rendered->streamed() && rendered->frames==12000,"mapped cache duration integrates the tempo change inside the region");
    if(!rendered)return;
    auto reader=rendered->streamSource->open();std::vector<float> l(rendered->frames),r(rendered->frames);
    require(reader && reader->read(0,l.data(),r.data(),static_cast<int>(l.size())),"read rendered warp cache");
    for(std::size_t i=0;i<l.size();++i)require(std::isfinite(l[i]) && std::abs(l[i]+r[i])<3e-7f,"warp keeps antiphase stereo linked and finite");
    for(const auto span: {std::pair{800,3000},std::pair{5000,10000}}) {
        int crossings=0;for(int i=span.first+1;i<span.second;++i)if(l[static_cast<std::size_t>(i-1)]<=0 && l[static_cast<std::size_t>(i)]>0)++crossings;
        const double hz=crossings*8000.0/(span.second-span.first);
        require(hz>210 && hz<230,"stretch retains the input pitch before and after the tempo change");
    }
    // Reading a slice is sample-identical to a full read: seeks and callback sizes
    // cannot change the pre-rendered stretch or its stereo phase.
    std::array<float,311> slice{},right{};require(reader->read(4097,slice.data(),right.data(),311),"seek cache");
    require(std::equal(slice.begin(),slice.end(),l.begin()+4097),"cache seeking is deterministic");
    const auto cache=std::dynamic_pointer_cast<const WarpCacheFile>(rendered->streamSource);
    require(cache!=nullptr,"render cache owns its temporary file");
    cancel.store(true);require(!renderRegionWarp(*source,region,song,0,cancel,error),"cancelled render never publishes a cache");cancel.store(false);
    auto impulse=warpTestSine();std::fill(impulse->left.begin(),impulse->left.end(),0);std::fill(impulse->right.begin(),impulse->right.end(),0);
    for(int at:{2000,5600,12000})for(int n=0;n<60;++n){impulse->left[static_cast<std::size_t>(at+n)]=.8f;impulse->right[static_cast<std::size_t>(at+n)]=-.8f;}
    const std::vector<timeline::WarpMarker> map{{0,0},{3840,2}};
    const auto transients=detectWarpTransients(*impulse,map,cancel,error);
    require(transients.size()==5 && std::abs(transients[1].seconds-.25)<.033 && std::abs(transients[2].seconds-.7)<.033,
        "background onset analysis detects separated stereo attacks with bounded source blocks");
    auto anchored=region;anchored.warp={{0,0},{960,.25},{1440,.7},{1680,1.5},{1920,2}};
    auto attacks=renderRegionWarp(*impulse,anchored,song,0,cancel,error);require(attacks!=nullptr,"render non-linear transient anchors");
    auto attackReader=attacks->streamSource->open();std::vector<float> attackL(attacks->frames),attackR(attacks->frames);
    require(attackReader && attackReader->read(0,attackL.data(),attackR.data(),static_cast<int>(attackL.size())),"read warped attacks");
    // Original WSOLA uses 40 ms grains and a 6 ms alignment search. Verify the
    // map moves attacks to their musical neighbourhood, without claiming
    // sample-exact transient pinning from this algorithm.
    for(double seconds:{.5,1.0,1.25}) {
        double sum=0;const int centre=static_cast<int>(seconds*8000);
        for(int n=centre-480;n<centre+480;++n)sum+=attackL[static_cast<std::size_t>(n)]*attackL[static_cast<std::size_t>(n)];
        require(sum>.01,"non-linear source anchors move audible attacks with the tempo map");
    }
    auto loud=warpTestSine();for(auto& v:loud->left)v*=4;for(auto& v:loud->right)v*=4;
    auto loudCache=renderRegionWarp(*loud,region,song,0,cancel,error);
    require(loudCache!=nullptr,"render floating-point headroom fixture");
    auto loudReader=loudCache->streamSource->open();std::array<float,1024> loudL{},loudR{};
    require(loudReader && loudReader->read(800,loudL.data(),loudR.data(),1024) && *std::max_element(loudL.begin(),loudL.end())>1.4f,"float warp cache preserves source headroom above full scale");
    std::cout << "PASS: tempo-aware pitch-preserving warp render, stereo, seeks, cancellation and transient analysis\n";
}
static void warpEditorChecks() {
    auto rig=std::make_unique<Rig>();rig->song();auto& c=rig->controller;const auto id=rig->id.au;
    rig->rightClick(rig->at(bar,2));
    require(rig->menu.shown,"audio region offers warp actions");
    rig->menu.done(TimelineCanvas::cmdWarp);waitForWarp(c);
    require(c.timeline().find(id)->warp.size()==2 && c.undoLabel()=="Enable audio warp" && !rig->engine.arrangementPreparing.load(),"enable warp is one undoable background-prepared edit");
    rig->rightClick(rig->at(bar,2));rig->menu.done(TimelineCanvas::cmdAddWarp);waitForWarp(c);
    require(c.timeline().find(id)->warp.size()==3,"context menu adds a source anchor at musical cursor");
    const auto before=c.timeline();const auto sourceSeconds=before.find(id)->warp[1].seconds;
    const auto box=rig->c.regionBounds(*before.find(id));
    const juce::Point<int> handle{juce::roundToInt(rig->c.xForTick(static_cast<double>(bar))),juce::roundToInt(box.getBottom()-5)};
    require(rig->c.hitTest(handle).warp==1,"bottom marker handle has its own hit target");
    rig->drag(handle,{{handle.x+20,handle.y}});waitForWarp(c);
    require(c.timeline().find(id)->warp[1].seconds==sourceSeconds && c.timeline().find(id)->warp[1].tick!=bar && c.undoLabel()=="Move warp marker","drag moves musical position while retaining source time");
    require(c.undo(),"undo marker drag");waitForWarp(c);rig->view.refresh();require(c.timeline()==before,"one undo restores the complete marker drag");
    require(c.redo(),"redo marker drag");waitForWarp(c);
    const auto marked=c.timeline().find(id)->warp;
    require(c.setTempoChange(bar,90),"change Song tempo after warp");require(c.isBusy() && rig->engine.arrangementPreparing.load(),"tempo edits prepare a replacement cache before playback");waitForWarp(c);
    require(c.timeline().find(id)->warp==marked,"tempo edit preserves musical marker anchors");
    c.setExternalBusy(true);require(!c.enableRegionWarp(id),"busy warp edits refuse");c.setExternalBusy(false);
    require(c.editTimeline("Disable audio warp",[=](auto& t){return t.setWarp(id,{});}) && !c.isBusy(),"disabling warp restores ordinary playback without a render");
    juce::String lastStatus;c.onProjectStatus=[&](const auto& text,const auto&){lastStatus=text;};
    require(c.enableRegionWarp(id) && c.cancelWarpPreparation(),"cancel a pending warp job");waitForWarp(c);
    require(!rig->engine.arrangementPreparing.load() && lastStatus.containsIgnoreCase("cancelled") && !c.timeline().find(id)->warp.empty(),"cancellation keeps editable markers, reports its result and releases transport gate");
    require(c.retryWarpPreparation(),"retry cache preparation");waitForWarp(c);
    require(lastStatus=="Warped audio ready","rebuild recovers a cancelled cache");
    c.setBpm(130);require(rig->engine.arrangementPreparing.load(),"base tempo drag gates playback before its commit");
    c.setBpm(120);c.commit("Tempo drag returning to start");waitForWarp(c);
    require(!rig->engine.arrangementPreparing.load(),"returning a tempo drag to its starting value cannot strand transport preparation");
    c.setBpm(100);c.commit("Warp base tempo change");require(c.isBusy(),"base tempo commit invalidates prepared warp audio");waitForWarp(c);
    rig->engine.arrangementMailbox.consume();
    const auto* changed=rig->engine.arrangementMailbox.current();const auto* savedRegion=c.timeline().find(id);
    const timeline::TempoMap changedTempo{c.timeline().tempoChanges,100};
    const auto expectedFrames=static_cast<std::size_t>(std::ceil(changedTempo.secondsBetween(savedRegion->start,savedRegion->end())*8000));
    require(changed->clipFor(*changed->timeline.find(id),2).audio->frames==expectedFrames,"prepared cache duration uses changed project tempo before the first Song tempo event");
    c.setBpm(120);c.commit("Restore base tempo");waitForWarp(c);
    const auto publications=rig->engine.arrangementMailbox.publishedCount();
    require(c.retryWarpPreparation(),"prepare concurrent-edit fixture");
    auto notes=c.document().clip(0,0).notes();notes.add(77,12,1);c.setClipPattern(0,0,notes,false);c.commit("Concurrent note gesture");
    waitForWarp(c);rig->engine.arrangementMailbox.consume();
    require(rig->engine.arrangementMailbox.publishedCount()==publications+1 && rig->engine.arrangementMailbox.current()->clips[0][0].pattern->count==notes.count,
        "a stale worker result is discarded and only the latest committed song is published");
    rig->engine.recording.phase.store(RecordControls::recording);
    require(!c.editTimeline("Recording warp edit",[=](auto& t){return t.addWarpMarker(id,bar/2);}),"warp edits refuse during count-in or recording");
    rig->engine.recording.phase.store(RecordControls::idle);
    require(c.editTimeline("Snapshot warp anchor",[=](auto& t){return t.addWarpMarker(id,bar);}),"preview includes an interior warp handle");waitForWarp(c);rig->view.refresh();
    const auto* directory=std::getenv("ALIEN_SNAPSHOT_DIR");
    writePng(rig->view.createComponentSnapshot(rig->view.getLocalBounds()),directory,"timeline-warp.png");
    c.onProjectStatus={};
    std::cout << "PASS: warp menus, actual marker drag, undo/redo, tempo re-render and busy state\n";
}

