#pragma once
// ClearDrumPad, SetDrumPadSlice, LoadDrumSample, SliceDrumPad and SliceClipToDrumPads: a track's eight drum pads. The Drums page reaches
// clear directly; asynchronous/range methods use routeDrumPadEdits for command submission.
static void drumPadCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& d = c.document();
    const auto before = c.document();
    const auto kit = [&]() -> const alien::SessionTrack& { return d.tracks[0]; };
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 2; audio->frames = 83;
    for (int i = 0; i < 83; ++i) { audio->left.push_back((i + 1) / 100.f); audio->right.push_back(-(i + 1) / 100.f); }
    audio->buildWaveform();
    const juce::String path = "C:/kit/source.wav";

    // A decoded sample on a pad: the track plays its drum kit, and the audio is charged once.
    const auto bytes = c.availableAudioBytes();
    const cmd::LoadDrumSample loadSample{0, 0, path, audio};
    const auto preview = commands.preview(cmd::Command{loadSample});
    require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && c.availableAudioBytes() == bytes, "LoadDrumSample Preview changes no live state");
    const auto loaded = commands.execute(loadSample);
    require(loaded.ok && loaded.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Load drum sample", "LoadDrumSample does what Preview said");
    require(kit().drumAssets[0] == audio && kit().drumPaths[0] == path && kit().drumSlices[0].whole() && kit().settings.instrument == 1
            && engine->drumKits[0].samples[0].load() == audio.get() && c.availableAudioBytes() == bytes - audio->decodedBytes(), "the pad, the engine and the storage count have the sample");
    require(!commands.execute(cmd::LoadDrumSample{0, 1, path, nullptr}).ok && !commands.execute(cmd::LoadDrumSample{0, 8, path, audio}).ok && !commands.execute(cmd::LoadDrumSample{99, 0, path, audio}).ok,
            "no audio, no such pad and no such track are refused");
    const auto withSample = c.document();

    // A range of the sample: a view of the same audio, which costs no storage.
    const cmd::SetDrumPadSlice part{0, 0, 10, 40};
    const auto partPreview = commands.preview(cmd::Command{part});
    require(partPreview.ok && partPreview.changed && kit().drumSlices[0].whole(), "SetDrumPadSlice Preview changes no live state");
    const auto sliced = commands.execute(part);
    require(sliced.ok && sliced.changed && c.undoLabel() == "Edit drum slice" && kit().drumSlices[0] == alien::DrumSlice{10, 40} && partPreview.after.tracks[0].drumSlices[0] == kit().drumSlices[0],
            "SetDrumPadSlice does what Preview said");
    require(kit().drumAssets[0]->sliceSource == audio && kit().drumAssets[0]->frames == 30 && kit().drumAssets[0]->decodedBytes() == 0
            && engine->drumKits[0].samples[0].load() == kit().drumAssets[0].get() && c.availableAudioBytes() == bytes - audio->decodedBytes(), "the pad plays a view of the sample, which the engine has");
    const auto same = commands.execute(part);
    require(same.ok && !same.changed, "the same range again is a no-op");
    require(commands.execute(cmd::SetDrumPadSlice{0, 0, 0, 83}).changed && kit().drumSlices[0].whole() && kit().drumAssets[0] == audio, "the whole sample is one state, however it is written");
    require(c.undo() && kit().drumSlices[0] == alien::DrumSlice{10, 40} && c.undo() && alien::sameContent(withSample, c.document()), "each range is one Undo step");
    for (const cmd::Command bad : {cmd::Command{cmd::SetDrumPadSlice{0, 0, 40, 10}}, cmd::Command{cmd::SetDrumPadSlice{0, 0, 0, 84}}, cmd::Command{cmd::SetDrumPadSlice{0, 0, -1, 5}},
                                   cmd::Command{cmd::SetDrumPadSlice{0, 1, 0, 10}}, cmd::Command{cmd::SetDrumPadSlice{0, 8, 0, 10}}, cmd::Command{cmd::SetDrumPadSlice{99, 0, 0, 10}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok && alien::sameContent(withSample, c.document()), "a range outside the sample, an empty pad and a pad that is not there are refused");
    }

    // The sample cut to three pads: the other pads are cleared.
    const cmd::SliceDrumPad cut{0, 0, {0, 20, 50, 83}};
    const auto cutResult = commands.execute(cut);
    require(cutResult.ok && cutResult.changed && c.undoLabel() == "Slice sample to pads", "SliceDrumPad runs");
    require(kit().drumSlices[0] == alien::DrumSlice{0, 20} && kit().drumSlices[1] == alien::DrumSlice{20, 50} && kit().drumSlices[2] == alien::DrumSlice{50, 83}
            && kit().drumPaths[2] == path && kit().drumPaths[3].isEmpty() && !kit().drumAssets[3] && kit().drumAssets[1]->sliceSource == audio
            && engine->drumKits[0].samples[2].load() == kit().drumAssets[2].get() && engine->drumKits[0].samples[3].load() == nullptr, "three pads play the slices, and the rest are empty");
    require(c.undo() && alien::sameContent(withSample, c.document()) && engine->drumKits[0].samples[1].load() == nullptr && c.redo() && kit().drumSlices[1] == alien::DrumSlice{20, 50}, "it is one Undo step");
    const auto threePads = c.document();
    for (const cmd::Command bad : {cmd::Command{cmd::SliceDrumPad{0, 0, {0, 20}}}, cmd::Command{cmd::SliceDrumPad{0, 0, {0, 50, 20}}}, cmd::Command{cmd::SliceDrumPad{0, 0, {0, 20, 84}}},
                                   cmd::Command{cmd::SliceDrumPad{0, 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}}}, cmd::Command{cmd::SliceDrumPad{0, 5, {0, 20, 40}}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && alien::sameContent(threePads, c.document()), "bounds that make no slices, and an empty pad, are refused");
    }

    // Clearing a pad.
    const std::vector<cmd::Command> clearAndMix{cmd::ClearDrumPad{0,1},cmd::SetTrackPan{0,.27}};
    const auto clearPreview=commands.preview(clearAndMix);
    require(clearPreview.ok && clearPreview.changed && alien::sameContent(threePads,c.document())
        && engine->drumKits[0].samples[1].load()==threePads.tracks[0].drumAssets[1].get(),"clear/mixer Preview leaves live kit intact");
    const auto cleared = commands.batch(clearAndMix,"Clear pad and mix");
    require(cleared.ok && cleared.changed && c.undoLabel() == "Clear pad and mix" && !kit().drumAssets[1] && kit().drumPaths[1].isEmpty() && kit().drumSlices[1].whole()
            && kit().drumAssets[0] && engine->drumKits[0].samples[1].load() == nullptr && engine->tracks[0].pan.load()==.27f
            && alien::sameContent(clearPreview.after,c.document()), "ClearDrumPad/mixer batch matches Preview and publishes kit and mix");
    const auto again = commands.execute(cmd::ClearDrumPad{0, 1});
    require(again.ok && !again.changed && !commands.execute(cmd::ClearDrumPad{0, -1}).ok, "an empty pad is a no-op, and a pad that is not there is refused");
    require(c.undo() && alien::sameContent(threePads, c.document())
        && engine->drumKits[0].samples[1].load()==threePads.tracks[0].drumAssets[1].get()
        && engine->tracks[0].pan.load()==static_cast<float>(threePads.tracks[0].settings.pan), "one Undo brings the pad and live mix back");
    const auto redo=historyOf(c);
    require(commands.execute(cmd::ClearDrumPad{0,7}).ok && historyOf(c)==redo,"empty-pad clear preserves redo");
    const auto late=commands.batch({clearAndMix[0],clearAndMix[1],cmd::ClearDrumPad{0,99}},"Invalid clear");
    c.setExternalBusy(true); const auto busyBatch=commands.batch(clearAndMix,"Busy clear"); c.setExternalBusy(false);
    require(!late.ok && late.failedCommand==2 && !busyBatch.ok && historyOf(c)==redo && alien::sameContent(threePads,c.document())
        && engine->drumKits[0].samples[1].load()==threePads.tracks[0].drumAssets[1].get()
        && engine->tracks[0].pan.load()==static_cast<float>(threePads.tracks[0].settings.pan),"refused batches preserve redo, pad publication and live mix");
    require(c.redo() && alien::sameContent(clearPreview.after,c.document()) && engine->drumKits[0].samples[1].load()==nullptr
        && engine->tracks[0].pan.load()==.27f && c.undo(),"one Redo restores the pad/mixer transaction");
    for(const auto phase : {alien::RecordControls::countIn,alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        const auto allowed=commands.batch(clearAndMix,"Live clear");
        engine->recording.phase.store(alien::RecordControls::idle);
        require(allowed.changed && engine->drumKits[0].samples[1].load()==nullptr && engine->tracks[0].pan.load()==.27f
            && c.undo() && alien::sameContent(threePads,c.document()),"clear/mixer commands preserve count-in/recording compatibility and one-step Undo");
    }

    // A take and a busy project refuse.
    engine->recording.phase.store(alien::RecordControls::countIn);
    const auto recording = commands.execute(part);
    const auto recordingCut = commands.execute(cut);
    engine->recording.phase.store(alien::RecordControls::idle);
    require(!recording.ok && recording.error.contains("recording") && !recordingCut.ok, "slicing is refused during a count-in");
    c.setExternalBusy(true);
    const auto busy = commands.execute(cmd::ClearDrumPad{0, 0});
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy") && alien::sameContent(threePads, c.document()), "a busy project refuses, and refusals change nothing");

    // Without the command route, prepared work must not apply or commit directly.
    const auto unrouted=c.document(); const auto unroutedHistory=historyOf(c); const auto unroutedBytes=c.availableAudioBytes();
    alien::drum_pad::Edit missing; missing.kind=alien::drum_pad::Kind::clear; missing.track=0; missing.pad=1;
    require(c.commitDrumPad(missing,"Unrouted clear").isNotEmpty(),"missing drum command route refuses a prepared clear");
    auto freshAudio=std::make_shared<alien::SessionAudioClip>();
    freshAudio->sampleRate=8000; freshAudio->channels=1; freshAudio->frames=80; freshAudio->left.assign(80,.2f); freshAudio->buildWaveform();
    missing.kind=alien::drum_pad::Kind::load; missing.pad=7; missing.path="C:/kit/unrouted.wav"; missing.clip=freshAudio;
    require(c.commitDrumPad(missing,"Unrouted load").isNotEmpty() && alien::sameContent(unrouted,c.document())
        && historyOf(c)==unroutedHistory && c.availableAudioBytes()==unroutedBytes
        && engine->drumKits[0].samples[1].load()==unrouted.tracks[0].drumAssets[1].get()
        && engine->drumKits[0].samples[7].load()==nullptr,"missing route preserves document, redo, audio budget and engine kit");
    // The Drums page's controller adapters submit commands once routed.
    cmd::routeDrumPadEdits(c);
    require(c.sliceDrum(0, 1) && c.undoLabel() == "Slice sample to 8 pads" && kit().drumSlices[0] == alien::DrumSlice{20, 20 + 30 / 8} && kit().drumSlices[7].end == 50 && kit().drumPaths[7] == path,
            "the routed equal slice cuts the pad's own range to eight pads");
    require(c.editDrumSlice(c.drumSliceTarget(0, 0), {0, 5}) && c.undoLabel() == "Edit drum slice" && kit().drumSlices[0] == alien::DrumSlice{0, 5}, "the routed range edit keeps its Undo name");
    require(commands.execute(cmd::ClearDrumPad{0,7}).changed,"clear the routed slice through its named command");
    require(c.undoLabel() == "Clear drum pad" && !kit().drumAssets[7] && c.undo() && c.undo() && c.undo() && alien::sameContent(threePads, c.document()), "and the routed edits are one Undo step each");

    // A Session clip's audio cut to its track's eight pads: the kit is replaced and the clip stays.
    auto loop = std::make_shared<alien::SessionAudioClip>();
    loop->sampleRate = 8000; loop->channels = 1; loop->frames = 80;
    loop->left.assign(80, 0.2f); loop->buildWaveform();
    require(c.installAudio(juce::File("C:/kit/loop.wav"), 1, 0, loop), "fixture: a Session clip with audio");
    const auto withLoop = c.document();
    const auto loopBytes = c.availableAudioBytes();
    const cmd::SliceClipToDrumPads toPads{1, 0};
    const auto toPadsPreview = commands.preview(cmd::Command{toPads});
    require(toPadsPreview.ok && toPadsPreview.changed && alien::sameContent(withLoop, c.document()), "SliceClipToDrumPads Preview changes no live state");
    const auto kitMade = commands.execute(toPads);
    require(kitMade.ok && kitMade.changed && c.undoLabel() == "Slice sample to 8 pads" && toPadsPreview.after.tracks[1].drumSlices == d.tracks[1].drumSlices, "SliceClipToDrumPads does what Preview said");
    for (int p = 0; p < 8; ++p)
        require(d.tracks[1].drumSlices[p] == alien::DrumSlice{10 * p, 10 * (p + 1)} && d.tracks[1].drumAssets[p]->sliceSource == loop && d.tracks[1].drumPaths[p] == d.clip(1, 0).audioFilePath
                && engine->drumKits[1].samples[p].load() == d.tracks[1].drumAssets[p].get(), "each pad plays an eighth of the clip, which the engine has");
    require(d.tracks[1].settings.instrument == 1 && d.clip(1, 0).audio == loop && c.availableAudioBytes() == loopBytes, "the track plays its kit, the clip is kept and no storage is charged");
    require(c.undo() && alien::sameContent(withLoop, c.document()) && c.redo() && d.tracks[1].drumAssets[7] && c.undo(), "it is one Undo step");
    for (const cmd::Command bad : {cmd::Command{cmd::SliceClipToDrumPads{0, 3}}, cmd::Command{cmd::SliceClipToDrumPads{99, 0}}, cmd::Command{cmd::SliceClipToDrumPads{1, 99}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok && alien::sameContent(withLoop, c.document()), "a cell with no audio, and a cell that is not there, are refused");
    }
    engine->recording.phase.store(alien::RecordControls::countIn);
    const auto recordingKit = commands.execute(toPads);
    engine->recording.phase.store(alien::RecordControls::idle);
    require(!recordingKit.ok && recordingKit.error.contains("recording") && alien::sameContent(withLoop, c.document()), "slicing a clip is refused during a count-in");
    require(c.sliceSessionAudio(1, 0) && c.undoLabel() == "Slice sample to 8 pads" && d.tracks[1].drumAssets[7] && c.undo(), "the routed controller method runs the command");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::ClearDrumPad{2, 3}}, cmd::Command{part}, cmd::Command{cut}, cmd::Command{toPads}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the drum pad commands round-trip through JSON");
    }
    require(!cmd::parse("LoadDrumSample", juce::JSON::parse(juce::JSON::toString(cmd::arguments(cmd::Command{loadSample}))), error) && error.contains("decoded audio"), "LoadDrumSample has no script form");
    require(cmd::describe(c.document(), cmd::ClearDrumPad{0, 1}).contains("pad 2") && cmd::describe(c.document(), loadSample).contains("source.wav")
            && cmd::describe(c.document(), cut).contains("3 pads") && cmd::describe(c.document(), toPads).contains("scene 1") && cmd::describe(c.document(), part).contains("10 to 40"), "the descriptions name the pad, the file and the slices");
}
