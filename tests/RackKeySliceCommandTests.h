#pragma once
// SetRackChannelKeySlices (T2-01): slices of a Channel Rack sample on the keys of one channel, sharing its decoded
// audio (key zones, format 38). RackEngineTests plays the zones; ChannelRackViewTests has the menu.
static void rackKeySliceCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"snare"}).changed, "a built-in sound channel");
    const int builtin = rack.channelCount - 1;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "key slice fixture channel");
    const int channel = rack.channelCount - 1;
    auto ramp = std::make_shared<alien::SessionAudioClip>();
    ramp->sampleRate = 8000; ramp->channels = 1; ramp->frames = 1000;
    for (int i = 0; i < 1000; ++i) ramp->left.push_back(static_cast<float>(i) / 1000.0f);
    ramp->buildWaveform();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rack key slice ramp.wav");
    require(cmd::loadDecodedRackSample(c, file, channel, ramp).changed, "a decoded sample on the channel");
    require(commands.execute(cmd::SetRackStep{rack.selectedPattern, channel, 0, true}).changed, "a step on the channel");
    c.markAsNewBaseline();
    const auto infoAt = [&](int ch) { return rack.info[static_cast<std::size_t>(ch)]; };
    const auto count = rack.channelCount;
    const auto bounds = alien::rack_channel::equalSlices(0, 1000, 4);

    // Four slices on four keys of the one channel: Preview changes nothing, execute does what it said, one Undo step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const cmd::SetRackChannelKeySlices keys{channel, bounds};
        const auto preview = commands.preview(cmd::Command{keys});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history && !infoAt(channel).hasZones(),
                "SetRackChannelKeySlices Preview changes no live state");
        const auto result = commands.execute(keys);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Slices on keys" && rack.channelCount == count,
                "SetRackChannelKeySlices does what Preview said and adds no channel");
        require(infoAt(channel).hasZones() && infoAt(channel).zones->size() == 4 && infoAt(channel).wholeSample(), "the channel holds four key zones");
        const auto* map = c.rackChannelAudio(channel);
        require(map != nullptr && map->keyZones != nullptr && map->keyZones->size() == 4 && map->frames == 1000 && map->left.empty(),
                "the channel plays a key map with no audio of its own");
        for (int k = 0; k < 4; ++k) {
            const auto& zone = (*infoAt(channel).zones)[static_cast<std::size_t>(k)];
            require(zone == alien::rack::KeyZone{250 * k, 250 * (k + 1), 60 + k, 60 + k, 60 + k}, "slice k sits alone on key 60 + k, at its own pitch");
            const auto& played = (*map->keyZones)[static_cast<std::size_t>(k)];
            require(played.low == 60 + k && played.high == 60 + k && played.root == 60 + k && played.clip->frames == 250
                    && played.clip->sliceSource.get() == ramp.get() && std::abs(played.clip->sampleAt(0, 0) - 0.25f * static_cast<float>(k)) < 1e-6f,
                    "and plays its part of the shared decoded audio");
        }
        require(engine->rackSamples[static_cast<std::size_t>(channel)].load() == map, "the engine plays the key map");
        require(rack.pattern(rack.selectedPattern).steps[static_cast<std::size_t>(channel)][0].on, "the channel keeps its steps");
        const auto again = commands.execute(keys);
        require(again.ok && !again.changed && c.rackChannelAudio(channel) == map, "the same slices again change nothing, and the map is reused");
        require(commands.execute(cmd::SetRackChannelKeySlices{channel, bounds, 36}).changed && (*infoAt(channel).zones)[0].low == 36
                && (*c.rackChannelAudio(channel)->keyZones)[3].root == 39, "the slices can start on another key");
        require(c.undo() && (*infoAt(channel).zones)[0].low == 60 && c.undo() && alien::sameContent(before, c.document()) && c.rackChannelAudio(channel) == ramp.get(),
                "Undo takes the keys away, and the whole sample plays again");
        require(c.redo() && infoAt(channel).hasZones() && engine->rackSamples[static_cast<std::size_t>(channel)].load() == map, "Redo brings them back, still playing");
    }

    // No bounds: the sample on every key again.
    {
        const auto zoned = c.document();
        require(commands.execute(cmd::SetRackChannelKeySlices{channel, {}}).changed && !infoAt(channel).hasZones() && c.rackChannelAudio(channel) == ramp.get(),
                "no bounds plays the whole sample on every key");
        require(!commands.execute(cmd::SetRackChannelKeySlices{channel, {}}).changed, "and is nothing to do twice");
        require(c.undo() && alien::sameContent(zoned, c.document()), "Undo puts the slices back on their keys");
    }

    // Saved and opened again with the project.
    {
        const auto saved = alien::rack::rackToVar(rack);
        alien::rack::Project opened; juce::String error;
        require(alien::rack::rackFromVar(saved, opened, error) && opened.info[static_cast<std::size_t>(channel)] == infoAt(channel)
                && !opened.info[static_cast<std::size_t>(builtin)].hasZones(), "key zones are saved");
        auto broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("channels", {}).getArray()->getReference(channel).getProperty("zones", {}).getArray()->getReference(1).getDynamicObject()->setProperty("high", 200);
        alien::rack::Project rejected;
        require(!alien::rack::rackFromVar(broken, rejected, error) && error.contains("key zones"), "a zone on a key that does not exist is refused when opened");
        broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("channels", {}).getArray()->getReference(channel).getProperty("zones", {}).getArray()->getReference(1).getDynamicObject()->setProperty("low", 60);
        require(!alien::rack::rackFromVar(broken, rejected, error) && error.contains("key zones"), "two zones on one key are refused when opened");
        auto model = rack;
        require(model.setSample(channel, "builtin:snare") && !model.info[static_cast<std::size_t>(channel)].hasZones() && model.valid(), "a new sound plays on every key");
        model = rack;
        require(model.setInstrumentPreset(channel, 0) && !model.info[static_cast<std::size_t>(channel)].hasZones() && model.valid(), "a synth has no key zones");
    }

    // Slicing a channel that has key zones still makes plain slice channels, and a slice channel can become a synth.
    {
        const auto before = c.document();
        require(commands.execute(cmd::SliceRackChannel{channel, bounds}).changed && infoAt(channel).hasZones() && !infoAt(channel + 1).hasZones()
                && infoAt(channel + 1).sliceEnd == 250 && c.rackChannelAudio(channel + 1)->keyZones == nullptr, "slice channels play their slice on every key");
        require(commands.execute(cmd::SetRackChannelKeySlices{channel + 4, {750, 800, 1000}}).changed && (*infoAt(channel + 4).zones)[1].end == 1000
                && c.rackChannelAudio(channel + 4)->frames == 250, "a slice channel's own slices go on its keys");
        const auto synth = commands.execute(cmd::SetRackChannelSynth{channel + 4, 0});
        require(synth.ok && synth.changed && infoAt(channel + 4).wholeSample() && !infoAt(channel + 4).hasZones(), "a slice channel can become a synth");
        require(c.undo() && c.undo() && c.undo() && alien::sameContent(before, c.document()), "all undone");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::SetRackChannelKeySlices& keys, const char* why) {
            const auto preview = commands.preview(cmd::Command{keys});
            const auto result = commands.execute(keys);
            require(!result.ok && result.error.contains(why) && !result.changed, why);
            require(preview.ok == false || juce::String(why) == "past the end", why);   // only the loaded audio knows the file's length
        };
        says({builtin, bounds}, "Only a channel playing an audio file");
        says({99, bounds}, "no rack channel");
        says({channel, {0, 1000}}, "2 to 16");
        std::vector<std::int64_t> seventeen;
        for (int k = 0; k <= 17; ++k) seventeen.push_back(k * 50);
        says({channel, seventeen}, "2 to 16");
        says({channel, {0, 500, 500, 1000}}, "Slice edges must rise");
        says({channel, {0, 500, 2000}}, "past the end");
        says({channel, bounds, 125}, "notes 0 to 127");
        says({channel, bounds, -1}, "notes 0 to 127");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
    }

    // Script form.
    juce::String error;
    const cmd::Command keys = cmd::SetRackChannelKeySlices{3, {0, 100, 200}, 48};
    const auto parsed = cmd::parse(cmd::name(keys), juce::JSON::parse(juce::JSON::toString(cmd::arguments(keys))), error);
    require(parsed && std::get<cmd::SetRackChannelKeySlices>(*parsed).bounds == std::vector<std::int64_t>{0, 100, 200}
            && std::get<cmd::SetRackChannelKeySlices>(*parsed).channel == 3 && std::get<cmd::SetRackChannelKeySlices>(*parsed).firstKey == 48,
            "SetRackChannelKeySlices round-trips through JSON");
    error.clear();
    require(!cmd::parse("SetRackChannelKeySlices", juce::JSON::parse(R"({"channel": 0, "bounds": [0, 1.5], "firstKey": 60})"), error) && error.contains("whole frame"),
            "frames are whole");
    require(cmd::describe(c.document(), keys).contains("2 slices") && cmd::describe(c.document(), keys).contains("from note 48"), "SetRackChannelKeySlices describes itself");
    require(cmd::describe(c.document(), cmd::SetRackChannelKeySlices{3, {}}).contains("on every key"), "and its clearing form");
}
