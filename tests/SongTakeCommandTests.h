#pragma once
// Recorded takes: comping part of one onto the arrangement, and forgetting one. TimelineViewTests drives the real
// takes panel, take lanes, comp drags and menus; SongRecordingTests records the takes.
static void songTakeCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 32000;   // 4 s: two bars at 120 BPM
    audio->left.assign(32000, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("take-command-fixture.wav"), 2, 0, audio), "audio take fixture");
    tl::Ids takeIds;
    require(c.editTimeline("Take fixture", [&](tl::Timeline& t) {
        tl::Region pass; pass.row = 2; pass.source = 0; pass.length = 2 * bar;
        auto later = pass; later.audioOffset = 0.5;   // a different stretch of the file, so the passes can be told apart
        return t.recordTakes({pass, later}, 1, &takeIds);
    }) && takeIds.size() == 2, "two recorded passes");
    c.markAsNewBaseline();
    const auto first = takeIds[0], second = takeIds[1];
    const auto regionsOn = [&](int row) { int n = 0; for (const auto& r : song.regions) n += r.row == row ? 1 : 0; return n; };

    // CompSongTake: Preview agrees with execute (the new clip's id included), one named step, and the takes are kept.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::CompSongTake{first, bar / 2, bar});
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), "CompSongTake Preview changes no live state");
        const auto result = commands.execute(cmd::CompSongTake{first, bar / 2, bar});
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Comp recording take", "CompSongTake does what Preview said");
        require(song.takes.size() == 2 && regionsOn(2) == 3, "the comp carves the placed pass and keeps both takes");
        const auto plays = song.takePlays(first, c.songTempo());
        require(plays.size() == 1 && plays[0].first == bar / 2 && plays[0].second == bar, "the arrangement plays the first take where it was comped");
        require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && regionsOn(2) == 3, "it undoes and redoes");
    }

    // ForgetSongTake: the take leaves the list; the comp stays.
    {
        const auto regions = song.regions;
        require(commands.execute(cmd::ForgetSongTake{first}).changed && c.undoLabel() == "Forget recording take"
                && song.takes.size() == 1 && song.takes[0].id == second && song.regions == regions, "ForgetSongTake keeps the comp");
        require(c.undo() && song.takes.size() == 2, "and Undo brings the take back");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::CompSongTake{99999999, 0, bar}, "no take");
        says(cmd::ForgetSongTake{99999999}, "no take");
        says(cmd::CompSongTake{first, bar, bar}, "inside the take");
        says(cmd::CompSongTake{first, bar, bar / 2}, "inside the take");
        says(cmd::CompSongTake{first, -1, bar}, "inside the take");
        says(cmd::CompSongTake{first, 0, 3 * bar}, "inside the take");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        engine->recording.phase.store(alien::RecordControls::recording);
        const auto comping = commands.execute(cmd::CompSongTake{first, 0, bar});
        const auto forgetting = commands.execute(cmd::ForgetSongTake{first});
        engine->recording.phase.store(alien::RecordControls::idle);
        require(!comping.ok && comping.error.contains("Stop recording") && !forgetting.ok && forgetting.error.contains("Stop recording")
                && alien::sameContent(before, c.document()), "takes wait while recording");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::ForgetSongTake{first});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && song.takes.size() == 2, "a busy project refuses them");
    }

    // A frozen track's takes cannot be comped; the pure reducer says why.
    {
        auto d = c.document();
        d.tracks[2].settings.freezeScene = 0;
        require(cmd::applyTo(d, cmd::CompSongTake{first, 0, bar}).contains("Unfreeze track 3"), "a frozen track refuses a comp");
    }

    // Script form.
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{cmd::CompSongTake{7, 960, 1920}, cmd::ForgetSongTake{7}}) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
                "take commands round-trip through JSON");
    }
    require(cmd::describe(c.document(), cmd::CompSongTake{7, 960, 1920}).contains("take 7"), "CompSongTake describes itself");
    error.clear();
    require(!cmd::parse("ForgetSongTake", juce::JSON::parse(R"({"take": 0})"), error) && error.contains("positive"), "a take id is positive");
    error.clear();
    require(!cmd::parse("CompSongTake", juce::JSON::parse(R"({"take": 3, "start": 0})"), error) && error.contains("end"), "CompSongTake needs all of its arguments");
}
