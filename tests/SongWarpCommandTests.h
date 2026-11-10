#pragma once
// Warp markers on a Song audio clip: turning warp on and off, and adding, moving and removing markers.
// WarpMarkerTests drives the real menu and marker drag, and checks the warped audio they render.
static void songWarpCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    // A warp edit prepares the warped audio in the background; nothing else edits until it is ready.
    const auto settle = [&] {
        const auto end = juce::Time::getMillisecondCounter() + 30000;
        while (c.isBusy() && juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(!c.isBusy(), "warped audio is prepared");
    };
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 32000;   // 4 s: two bars at 120 BPM
    audio->left.assign(32000, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("warp-command-fixture.wav"), 2, 0, audio), "audio warp fixture");
    require(c.editTimeline("Place clips", [](tl::Timeline& t) { return t.placeBars(2, 0, 0, 2) && t.placeBars(0, 0, 0, 1); }), "place an audio and a MIDI clip");
    c.markAsNewBaseline();
    std::uint32_t id = 0, midi = 0;
    for (const auto& r : song.regions) (r.row == 2 ? id : midi) = r.id;
    require(id != 0 && midi != 0, "fixture clips found");
    const auto warp = [&] { return song.find(id)->warp; };
    const auto fitted = c.fittedWarp(id);
    require(fitted && fitted->size() == 2 && (*fitted)[0] == tl::WarpMarker{0, 0.0} && (*fitted)[1] == tl::WarpMarker{2 * bar, 4.0}, "warp fits the source to the clip");
    require(!c.fittedWarp(midi), "a MIDI clip has no audio to fit");

    // Every edit: Preview changes nothing live, execute does what Preview said, one named Undo step, and Undo restores it.
    const auto step = [&](const cmd::Command& edit, const char* label, const std::function<bool()>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(edit);
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == label && happened(), what);
        settle();
        require(c.undo() && alien::sameContent(before, c.document()), what);
        settle();
        require(c.redo() && happened(), what);
        settle();
    };
    step(cmd::SetSongClipWarp{id, *fitted}, "Set warp markers", [&] { return warp() == *fitted; }, "SetSongClipWarp turns warp on");
    step(cmd::AddSongWarpMarker{id, bar}, "Add warp marker", [&] { return warp().size() == 3 && warp()[1] == tl::WarpMarker{bar, 2.0}; }, "AddSongWarpMarker keeps the source where it was");
    step(cmd::MoveSongWarpMarker{id, 1, bar + bar / 4}, "Move warp marker", [&] { return warp()[1] == tl::WarpMarker{bar + bar / 4, 2.0}; }, "MoveSongWarpMarker retimes the source");
    require(commands.execute(cmd::MoveSongWarpMarker{id, 1, bar + bar / 4}).ok && c.undoLabel() == "Move warp marker", "a move to where it is changes nothing");

    // Refusals change nothing and say why.
    {
        settle();
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetSongClipWarp{99999999, *fitted}, "no clip");
        says(cmd::SetSongClipWarp{midi, *fitted}, "audio clips");
        says(cmd::SetSongClipWarp{id, {{0, 0.0}}}, "2 to 256 markers");
        says(cmd::SetSongClipWarp{id, {{bar, 1.0}, {0, 2.0}}}, "2 to 256 markers");
        says(cmd::SetSongClipWarp{id, {{0, 2.0}, {bar, 1.0}}}, "2 to 256 markers");
        says(cmd::AddSongWarpMarker{midi, bar / 2}, "Turn on warp");
        says(cmd::AddSongWarpMarker{id, 0}, "between the clip's first and last");
        says(cmd::AddSongWarpMarker{id, 2 * bar}, "between the clip's first and last");
        says(cmd::MoveSongWarpMarker{id, 0, bar / 2}, "first and last");
        says(cmd::RemoveSongWarpMarker{id, 2}, "first and last");
        says(cmd::RemoveSongWarpMarker{id, 3}, "no warp marker 3");
        says(cmd::MoveSongWarpMarker{id, -1, bar}, "no warp marker -1");
        says(cmd::MoveSongWarpMarker{id, 1, 2 * bar}, "stays between");
        says(cmd::MoveSongWarpMarker{id, 1, 0}, "stays between");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        engine->recording.phase.store(alien::RecordControls::recording);
        const auto recording = commands.execute(cmd::AddSongWarpMarker{id, bar / 2});
        engine->recording.phase.store(alien::RecordControls::idle);
        require(!recording.ok && recording.error.contains("Stop recording") && alien::sameContent(before, c.document()), "warp waits while recording");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::RemoveSongWarpMarker{id, 1});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && warp().size() == 3, "a busy project refuses them");

        auto frozen = c.document();
        frozen.tracks[2].settings.freezeScene = 0;
        require(cmd::applyTo(frozen, cmd::AddSongWarpMarker{id, bar / 2}).contains("Unfreeze track 3"), "a frozen track's warp markers are refused");
    }

    step(cmd::RemoveSongWarpMarker{id, 1}, "Remove warp marker", [&] { return warp() == *fitted; }, "RemoveSongWarpMarker");
    step(cmd::SetSongClipWarp{id, {}}, "Set warp markers", [&] { return warp().empty(); }, "SetSongClipWarp with no markers turns warp off");
    require(commands.execute(cmd::SetSongClipWarp{midi, {}}).ok, "turning warp off where it is off changes nothing");

    // Script form.
    juce::String error;
    const std::vector<cmd::Command> scripted{cmd::SetSongClipWarp{7, {{0, 0.0}, {960, 1.25}, {1920, 2.5}}}, cmd::SetSongClipWarp{7, {}},
                                             cmd::AddSongWarpMarker{7, 960}, cmd::MoveSongWarpMarker{7, 1, 1200}, cmd::RemoveSongWarpMarker{7, 1}};
    for (const auto& command : scripted) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
                "warp commands round-trip through JSON");
    }
    const auto parsedWarp = cmd::parse("SetSongClipWarp", juce::JSON::parse(R"({"clip": 7, "markers": [{"tick": 0, "seconds": 0}, {"tick": 960, "seconds": 1.5}]})"), error);
    require(parsedWarp && std::get<cmd::SetSongClipWarp>(*parsedWarp).markers == std::vector<tl::WarpMarker>{{0, 0.0}, {960, 1.5}}, "markers parse from whole and fractional seconds");
    require(cmd::describe(c.document(), cmd::MoveSongWarpMarker{7, 1, 1200}).contains("warp marker 1"), "MoveSongWarpMarker describes itself");
    require(cmd::describe(c.document(), cmd::SetSongClipWarp{7, {}}).contains("Turn off warp"), "SetSongClipWarp with no markers says it turns warp off");
    error.clear();
    require(!cmd::parse("SetSongClipWarp", juce::JSON::parse(R"({"clip": 7})"), error) && error.contains("markers"), "SetSongClipWarp needs its markers");
    error.clear();
    require(!cmd::parse("SetSongClipWarp", juce::JSON::parse(R"({"clip": 7, "markers": [{"tick": 0}]})"), error) && error.contains("each marker"), "a marker needs a tick and seconds");
    error.clear();
    require(!cmd::parse("SetSongClipWarp", juce::JSON::parse(R"({"clip": 7, "markers": [{"tick": 0, "seconds": 0, "x": 1}]})"), error) && error.contains("each marker"), "a marker has nothing else");
    error.clear();
    require(!cmd::parse("AddSongWarpMarker", juce::JSON::parse(R"({"clip": 0, "tick": 3})"), error) && error.contains("positive"), "a clip id is positive");
    error.clear();
    require(!cmd::parse("MoveSongWarpMarker", juce::JSON::parse(R"({"clip": 7, "marker": 1})"), error) && error.contains("tick"), "MoveSongWarpMarker needs all of its arguments");
}
