#pragma once
// SetTrackSound (the Mixer strip's instrument box) and ImportSongAudio (a sample placed on the Song).
static void trackSoundCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& settings = c.document().tracks[0].settings;
    const auto& live = engine->tracks[0];

    const auto before = c.document();
    const int other = before.tracks[0].settings.preset == 2 ? 3 : 2;
    const cmd::SetTrackSound set{0, other, false};
    const auto preview = commands.preview(cmd::Command{set});
    require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && live.preset.load() == before.tracks[0].settings.preset,
            "SetTrackSound Preview changes no live state");
    const auto result = commands.execute(set);
    require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && settings.preset == other && settings.instrument == 0
            && live.preset.load() == other && live.instrument.load() == 0 && c.undoLabel() == "Change track sound", "SetTrackSound does what Preview said, and the engine plays the sound");
    require(!commands.execute(set).changed, "the same sound again is no edit");

    // The drum kit leaves the synth preset as it is, so going back to the synth finds it.
    const auto kit = commands.execute(cmd::SetTrackSound{0, 0, true});
    require(kit.ok && kit.changed && settings.instrument == 1 && settings.preset == other && live.instrument.load() == 1 && live.preset.load() == other, "the drum kit keeps the synth preset");
    require(c.undo() && settings.instrument == 0 && live.instrument.load() == 0 && c.undo() && alien::sameContent(before, c.document())
            && live.preset.load() == before.tracks[0].settings.preset && c.redo() && settings.preset == other && live.preset.load() == other && c.undo(), "each change is one Undo step");

    for (const cmd::Command bad : {cmd::Command{cmd::SetTrackSound{-1, 0, false}}, cmd::Command{cmd::SetTrackSound{99, 0, true}}, cmd::Command{cmd::SetTrackSound{0, -1, false}},
                                   cmd::Command{cmd::SetTrackSound{0, alien::presetCount, false}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok, "a track or preset that is not there is refused");
    }
    c.setExternalBusy(true);
    const auto busy = commands.execute(set);
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()) && live.preset.load() == before.tracks[0].settings.preset, "a busy project refuses, and refusals change nothing");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{cmd::SetTrackSound{1, 3, false}}, cmd::Command{cmd::SetTrackSound{1, 0, true}}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "SetTrackSound round-trips through JSON");
    }
    require(cmd::describe(c.document(), cmd::SetTrackSound{0, 0, false}).contains(alien::presetNames[0]) && cmd::describe(c.document(), cmd::SetTrackSound{0, 0, true}).contains("drum kit"),
            "SetTrackSound names the sound");
}

static void songAudioImportCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("alien-song-import.wav");   // only its name and path are used
    auto clip = std::make_shared<alien::SessionAudioClip>();
    clip->sampleRate = 8000; clip->channels = 1; clip->frames = 8000; clip->left.assign(clip->frames, .2f); clip->buildWaveform();

    const auto before = c.document();
    const cmd::ImportSongAudio set{{0, 3840, file, clip, false}};
    const auto preview = commands.preview(cmd::Command{set});
    require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && c.timeline().regions.empty(), "ImportSongAudio Preview changes no live state");
    const auto result = commands.execute(set);
    require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Import sample to Song", "ImportSongAudio does what Preview said");
    require(c.timeline().regions.size() == 1 && c.timeline().regions[0].row == 0 && c.timeline().regions[0].start == 3840 && c.timeline().takes.size() == 1, "the sample is one Song clip at the tick");
    const int scene = c.timeline().regions[0].source;
    require(c.document().clip(0, scene).isAudioClip() && c.document().clip(0, scene).audioFileName == file.getFileName() && c.document().selectedTrack == 0 && c.document().selectedScene == scene,
            "its audio is in a Session cell of the track, which is selected");
    require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && c.timeline().regions.size() == 1, "it is one Undo step");

    // A drop keeps the Song clips it would cover; a sample from the browser does not need a free range.
    const auto placed = c.document();
    const auto kept = commands.execute(cmd::ImportSongAudio{{0, 3000, file, clip, true}});
    require(!kept.ok && !kept.changed && kept.error.contains("free Song range") && alien::sameContent(placed, c.document()), "a drop onto a Song clip is refused whole");
    const auto second = commands.execute(cmd::ImportSongAudio{{1, 0, file, clip, true}});
    require(second.ok && second.changed && c.timeline().regions.size() == 2 && c.undo() && alien::sameContent(placed, c.document()), "a free range on another track takes it");

    for (const cmd::Command bad : {cmd::Command{cmd::ImportSongAudio{{0, 0, file, nullptr, false}}}, cmd::Command{cmd::ImportSongAudio{{-1, 0, file, clip, false}}},
                                   cmd::Command{cmd::ImportSongAudio{{99, 0, file, clip, false}}}, cmd::Command{cmd::ImportSongAudio{{0, -1, file, clip, false}}},
                                   cmd::Command{cmd::ImportSongAudio{{0, alien::timeline::maxLength, file, clip, false}}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok, "no audio, no such track or no room is refused");
    }
    require(alien::sameContent(placed, c.document()), "refusals change nothing");
    c.setExternalBusy(true);
    const auto busy = commands.execute(set);
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy"), "a busy project refuses");

    juce::String error;
    require(!cmd::parse("ImportSongAudio", cmd::arguments(cmd::Command{set}), error) && error.contains("decoded audio"), "ImportSongAudio has no script form");
    require(cmd::describe(c.document(), set).contains(file.getFileName()), "ImportSongAudio names the file");
}
