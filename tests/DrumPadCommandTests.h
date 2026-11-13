#pragma once
// ClearDrumPad, SetDrumPadSlice, LoadDrumSample and SliceDrumPad: a track's eight drum pads. The Drums page reaches
// them through ProjectController's drum methods, which routeDrumPadEdits sends through the command engine.
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
    const auto cleared = commands.execute(cmd::ClearDrumPad{0, 1});
    require(cleared.ok && cleared.changed && c.undoLabel() == "Clear drum pad" && !kit().drumAssets[1] && kit().drumPaths[1].isEmpty() && kit().drumSlices[1].whole()
            && kit().drumAssets[0] && engine->drumKits[0].samples[1].load() == nullptr, "ClearDrumPad empties one pad");
    const auto again = commands.execute(cmd::ClearDrumPad{0, 1});
    require(again.ok && !again.changed && !commands.execute(cmd::ClearDrumPad{0, -1}).ok, "an empty pad is a no-op, and a pad that is not there is refused");
    require(c.undo() && alien::sameContent(threePads, c.document()), "and Undo brings the pad back");

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

    // The controller's own drum methods, which the Drums page calls, run the commands once they are routed.
    cmd::routeDrumPadEdits(c);
    require(c.sliceDrum(0, 1) && c.undoLabel() == "Slice sample to 8 pads" && kit().drumSlices[0] == alien::DrumSlice{20, 20 + 30 / 8} && kit().drumSlices[7].end == 50 && kit().drumPaths[7] == path,
            "the routed equal slice cuts the pad's own range to eight pads");
    require(c.editDrumSlice(c.drumSliceTarget(0, 0), {0, 5}) && c.undoLabel() == "Edit drum slice" && kit().drumSlices[0] == alien::DrumSlice{0, 5}, "the routed range edit keeps its Undo name");
    c.clearDrum(0, 7);
    require(c.undoLabel() == "Clear drum pad" && !kit().drumAssets[7] && c.undo() && c.undo() && c.undo() && alien::sameContent(threePads, c.document()), "and the routed edits are one Undo step each");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::ClearDrumPad{2, 3}}, cmd::Command{part}, cmd::Command{cut}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the drum pad commands round-trip through JSON");
    }
    require(!cmd::parse("LoadDrumSample", juce::JSON::parse(juce::JSON::toString(cmd::arguments(cmd::Command{loadSample}))), error) && error.contains("decoded audio"), "LoadDrumSample has no script form");
    require(cmd::describe(c.document(), cmd::ClearDrumPad{0, 1}).contains("pad 2") && cmd::describe(c.document(), loadSample).contains("source.wav")
            && cmd::describe(c.document(), cut).contains("3 pads") && cmd::describe(c.document(), part).contains("10 to 40"), "the descriptions name the pad, the file and the slices");
}
