#pragma once
// Automation lanes: a track's volume, pan or send lane and a plugin parameter's lane, through Read and points.
// TimelineViewTests and PluginAutomationPlaybackTests drive the real automation editor.
static void automationCommandChecks() {
    namespace tl = alien::timeline;
    namespace au = alien::automation;
    using Lane = alien::automation_edit::LaneRef;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& song = c.timeline();
    const auto bar = tl::ticksPerBar;
    if (!c.document().returns[0].used) require(commands.execute(cmd::CreateReturn{}).ok && c.document().returns[0].used, "return A for a send lane");
    c.markAsNewBaseline();

    Lane volume; volume.track = 1; volume.parameter = au::Parameter::volume;
    Lane send = volume; send.parameter = au::Parameter::sendA;
    Lane plugin; plugin.plugin = {42, "cutoff"};
    const auto points = [&](const Lane& lane) -> std::vector<au::Point> {
        const auto* p = alien::automation_edit::pointsOf(song, lane);
        return p != nullptr ? *p : std::vector<au::Point>{};
    };

    // Every edit: Preview changes nothing live, execute does what Preview said, one named Undo step, and Undo restores it.
    const auto step = [&](const cmd::Command& edit, const char* label, const std::function<bool()>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(edit);
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == label && happened(), what);
        require(c.undo() && alien::sameContent(before, c.document()), what);
        require(c.redo() && happened(), what);
    };
    step(cmd::AddAutomationPoint{volume, bar, 1.5f, au::Curve::linear}, "Add automation point",
         [&] { return song.automationLanes.size() == 1 && points(volume) == std::vector<au::Point>{{bar, 1.5f, au::Curve::linear}}; },
         "AddAutomationPoint makes the lane with its first point");
    step(cmd::AddAutomationPoint{volume, 2 * bar, 0.5f, au::Curve::hold}, "Add automation point",
         [&] { return points(volume).size() == 2 && points(volume)[1] == au::Point{2 * bar, 0.5f, au::Curve::hold}; }, "a second point goes after the first");
    step(cmd::AddAutomationPoint{volume, bar, 1.0f, au::Curve::linear}, "Add automation point",
         [&] { return points(volume).size() == 2 && points(volume)[0].value == 1.0f; }, "a point at a tick that has one replaces it");
    step(cmd::MoveAutomationPoint{volume, bar, bar / 2, 0.25f, au::Curve::hold}, "Move automation point",
         [&] { return points(volume) == std::vector<au::Point>{{bar / 2, 0.25f, au::Curve::hold}, {2 * bar, 0.5f, au::Curve::hold}}; },
         "MoveAutomationPoint moves a point to its new tick and value");
    step(cmd::MoveAutomationPoint{volume, bar / 2, 2 * bar, 2.0f, au::Curve::linear}, "Move automation point",
         [&] { return points(volume) == std::vector<au::Point>{{2 * bar, 2.0f, au::Curve::linear}}; }, "a point moved onto another replaces it");
    step(cmd::SetAutomationRead{volume, false}, "Set automation read", [&] { return !song.automationLanes[0].enabled; }, "SetAutomationRead turns a lane off");
    step(cmd::SetAutomationRead{volume, true}, "Set automation read", [&] { return song.automationLanes[0].enabled; }, "and on");
    step(cmd::RemoveAutomationPoint{volume, 2 * bar}, "Remove automation point",
         [&] { return song.automationLanes.size() == 1 && points(volume).empty(); }, "removing the last point keeps the lane, empty");
    step(cmd::AddAutomationPoint{send, 0, 1.0f, au::Curve::linear}, "Add automation point",
         [&] { return song.automationLanes.size() == 2 && song.automationLanes[1].parameter == au::Parameter::sendA; }, "a send lane for a return that exists");
    step(cmd::AddAutomationPoint{plugin, bar, 0.75f, au::Curve::linear}, "Add automation point",
         [&] { return song.parameterLanes.size() == 1 && song.parameterLanes[0].target == plugin.plugin && points(plugin).size() == 1; },
         "a plugin parameter point makes a plugin lane");
    step(cmd::SetAutomationRead{plugin, false}, "Set automation read", [&] { return !song.parameterLanes[0].enabled; }, "Read on a plugin lane");
    {
        const auto history = historyOf(c);
        const auto off = commands.execute(cmd::SetAutomationRead{plugin, false});
        require(off.ok && !off.changed && historyOf(c) == history, "turning Read off where it is off changes nothing");
        const auto same = commands.execute(cmd::AddAutomationPoint{plugin, bar, 0.75f, au::Curve::linear});
        require(same.ok && !same.changed && historyOf(c) == history, "putting a point that is already there records nothing");
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
        Lane missingTrack = volume; missingTrack.track = 99;
        Lane missingReturn = volume; missingReturn.parameter = au::Parameter::sendH;
        Lane badPlugin; badPlugin.plugin = {7, ""};
        Lane pan = volume; pan.parameter = au::Parameter::pan;
        says(cmd::AddAutomationPoint{missingTrack, 0, 1.0f, au::Curve::linear}, "no track 99");
        says(cmd::AddAutomationPoint{missingReturn, 0, 1.0f, au::Curve::linear}, "needs return H");
        says(cmd::AddAutomationPoint{badPlugin, 0, 0.5f, au::Curve::linear}, "plugin instance and a parameter id");
        says(cmd::SetAutomationRead{pan, false}, "no automation yet");
        says(cmd::AddAutomationPoint{volume, -1, 1.0f, au::Curve::linear}, "inside the song");
        says(cmd::AddAutomationPoint{volume, song.length() + 1, 1.0f, au::Curve::linear}, "inside the song");
        says(cmd::AddAutomationPoint{volume, 0, 2.5f, au::Curve::linear}, "from 0.0 to 2.0");
        says(cmd::AddAutomationPoint{pan, 0, -1.5f, au::Curve::linear}, "from -1.0 to 1.0");
        says(cmd::AddAutomationPoint{plugin, 0, 1.5f, au::Curve::linear}, "from 0 to 1");
        says(cmd::AddAutomationPoint{volume, 0, 1.0f, static_cast<au::Curve>(5)}, "linear (0) or hold (1)");
        says(cmd::MoveAutomationPoint{volume, 3 * bar, bar, 1.0f, au::Curve::linear}, "no automation point at tick");
        says(cmd::RemoveAutomationPoint{plugin, 0}, "no automation point at tick 0");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        auto full = c.document();
        auto& lane = full.timeline.automationLanes[0];
        for (std::size_t i = 0; i < au::maxPoints; ++i) lane.points.push_back({static_cast<std::int64_t>(i), 1.0f, au::Curve::linear});
        require(cmd::applyTo(full, cmd::AddAutomationPoint{volume, 2 * bar, 1.0f, au::Curve::linear}).contains("at most 4096"), "a lane holds at most 4096 points");
        require(cmd::applyTo(full, cmd::AddAutomationPoint{volume, 5, 0.5f, au::Curve::linear}).isEmpty(), "a full lane can still replace a point");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::RemoveAutomationPoint{plugin, bar});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && points(plugin).size() == 1, "a busy project refuses them");
    }

    // While warped audio records, automation waits like the loop does.
    {
        auto audio = std::make_shared<alien::SessionAudioClip>();
        audio->sampleRate = 8000; audio->channels = 1; audio->frames = 32000;
        audio->left.assign(32000, .2f); audio->buildWaveform();
        require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("automation-command-fixture.wav"), 2, 0, audio), "audio fixture");
        require(c.editTimeline("Place clip", [](tl::Timeline& t) { return t.placeBars(2, 0, 0, 2); }), "place an audio clip");
        std::uint32_t id = 0;
        for (const auto& r : song.regions) if (r.row == 2) id = r.id;
        require(commands.execute(cmd::SetSongClipWarp{id, *c.fittedWarp(id)}).ok, "warp the clip");
        const auto end = juce::Time::getMillisecondCounter() + 30000;
        while (c.isBusy() && juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(!c.isBusy(), "warped audio is prepared");
        const auto before = c.document();
        engine->recording.phase.store(alien::RecordControls::recording);
        const auto recording = commands.execute(cmd::AddAutomationPoint{volume, 0, 1.0f, au::Curve::linear});
        const auto unchanged = commands.execute(cmd::AddAutomationPoint{plugin, bar, 0.75f, au::Curve::linear});
        engine->recording.phase.store(alien::RecordControls::idle);
        require(!recording.ok && recording.error.contains("Stop recording") && alien::sameContent(before, c.document()), "automation waits while warped audio records");
        require(unchanged.ok && !unchanged.changed, "an edit that changes nothing is not refused");
    }

    // Script form.
    juce::String error;
    const std::vector<cmd::Command> scripted{cmd::SetAutomationRead{volume, false}, cmd::SetAutomationRead{plugin, true},
                                             cmd::AddAutomationPoint{send, 960, 0.5f, au::Curve::hold}, cmd::AddAutomationPoint{plugin, 0, 0.25f, au::Curve::linear},
                                             cmd::MoveAutomationPoint{volume, 960, 1920, 1.25f, au::Curve::linear}, cmd::RemoveAutomationPoint{plugin, 480}};
    for (const auto& command : scripted) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
                "automation commands round-trip through JSON");
    }
    const auto parsedPlugin = cmd::parse("AddAutomationPoint", juce::JSON::parse(R"({"instance": "18446744073709551615", "parameterId": "mix", "tick": 0, "value": 1, "curve": 1})"), error);
    require(parsedPlugin && std::get<cmd::AddAutomationPoint>(*parsedPlugin).lane.plugin == au::ParameterTarget{18446744073709551615ull, "mix"}
            && std::get<cmd::AddAutomationPoint>(*parsedPlugin).curve == au::Curve::hold, "a plugin instance parses from its full 64-bit decimal string");
    const auto parsedSend = cmd::parse("RemoveAutomationPoint", juce::JSON::parse(R"({"track": 0, "parameter": "sendC", "tick": 5})"), error);
    require(parsedSend && std::get<cmd::RemoveAutomationPoint>(*parsedSend).lane.parameter == au::Parameter::sendC, "a send lane parses by name");
    require(cmd::describe(c.document(), cmd::AddAutomationPoint{volume, 960, 0.5f, au::Curve::hold}).contains("volume automation"), "AddAutomationPoint describes its lane");
    require(cmd::describe(c.document(), cmd::SetAutomationRead{plugin, false}).contains("plugin 42's parameter \"cutoff\""), "a plugin lane is described by its target");
    const auto refuses = [&](const char* name, const char* json, const char* why) {
        error.clear();
        require(!cmd::parse(name, juce::JSON::parse(json), error) && error.contains(why), why);
    };
    refuses("SetAutomationRead", R"({"track": 0, "parameter": "gain", "enabled": true})", "volume, pan or sendA");
    refuses("SetAutomationRead", R"({"track": 0, "parameter": "pan", "instance": "4", "parameterId": "x", "enabled": true})", "not both");
    refuses("SetAutomationRead", R"({"instance": "04", "parameterId": "x", "enabled": true})", "canonical decimal-string");
    refuses("SetAutomationRead", R"({"instance": 4, "parameterId": "x", "enabled": true})", "text \"instance\"");
    refuses("AddAutomationPoint", R"({"track": 0, "parameter": "pan", "tick": 0, "value": 0, "curve": 2})", "0 (linear) or 1 (hold)");
    refuses("AddAutomationPoint", R"({"track": 0, "parameter": "pan", "tick": 0, "value": 1e300, "curve": 0})", "value");
    refuses("MoveAutomationPoint", R"({"track": 0, "parameter": "pan", "tick": 0, "value": 0, "curve": 0})", "\"from\"");
    refuses("RemoveAutomationPoint", R"({"track": 0, "parameter": "pan", "tick": 0, "value": 0})", "no \"value\" argument");
}
