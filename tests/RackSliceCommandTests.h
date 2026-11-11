#pragma once
// SliceRackChannel (T2-01): slicing a Channel Rack sample into channels that each play a part of it, sharing its
// decoded audio. RackEngineTests plays a slice; ChannelRackViewTests has the menu.
static void rackSliceCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"snare"}).changed, "a built-in sound channel");
    const int builtin = rack.channelCount - 1;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "slice fixture channel");
    const int channel = rack.channelCount - 1;
    auto ramp = std::make_shared<alien::SessionAudioClip>();
    ramp->sampleRate = 8000; ramp->channels = 1; ramp->frames = 1000;
    for (int i = 0; i < 1000; ++i) ramp->left.push_back(static_cast<float>(i) / 1000.0f);
    ramp->buildWaveform();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rack slice ramp.wav");
    require(cmd::loadDecodedRackSample(c, file, channel, ramp).changed, "a decoded sample on the channel");
    require(commands.execute(cmd::SetRackStep{rack.selectedPattern, channel, 0, true}).changed, "a step on the sliced channel");
    c.markAsNewBaseline();
    const auto infoAt = [&](int ch) { return rack.info[static_cast<std::size_t>(ch)]; };
    const auto count = rack.channelCount;

    // Four equal slices: four new channels after it, each playing its part of the shared audio, in one Undo step.
    const auto bounds = alien::rack_channel::equalSlices(0, 1000, 4);
    require(bounds == std::vector<std::int64_t>{0, 250, 500, 750, 1000}, "equal slices split the frames evenly");
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const cmd::SliceRackChannel slice{channel, bounds};
        const auto preview = commands.preview(cmd::Command{slice});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history && rack.channelCount == count,
                "SliceRackChannel Preview changes no live state");
        const auto result = commands.execute(slice);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Slice channel" && rack.channelCount == count + 4,
                "SliceRackChannel does what Preview said");
        require(infoAt(channel).wholeSample() && infoAt(channel).sample == file.getFullPathName().toStdString(), "the sliced channel stays as it was");
        for (int k = 0; k < 4; ++k) {
            const auto info = infoAt(channel + 1 + k);
            const auto* clip = c.rackChannelAudio(channel + 1 + k);
            require(info.sample == infoAt(channel).sample && info.sliceStart == 250 * k && info.sliceEnd == 250 * (k + 1)
                    && info.name == infoAt(channel).name + " " + std::to_string(k + 1), "each new channel plays its slice of the file");
            require(clip != nullptr && clip->frames == 250 && clip->sliceSource.get() == ramp.get() && std::abs(clip->sampleAt(0, 0) - 0.25f * static_cast<float>(k)) < 1e-6f,
                    "and shares the file's decoded audio from the slice's first frame");
            require(engine->rackSamples[static_cast<std::size_t>(channel + 1 + k)].load() == clip, "the engine plays the slice");
            require(!rack.pattern(rack.selectedPattern).steps[static_cast<std::size_t>(channel + 1 + k)][0].on, "a new slice channel has no steps");
        }
        require(rack.pattern(rack.selectedPattern).steps[static_cast<std::size_t>(channel)][0].on, "the sliced channel keeps its steps");
        require(c.undo() && alien::sameContent(before, c.document()) && rack.channelCount == count, "Undo takes the slices away");
        require(c.redo() && rack.channelCount == count + 4 && c.rackChannelAudio(channel + 2)->frames == 250, "Redo brings them back, still playing");
    }

    // A slice can be sliced again, inside its own frames.
    {
        const int first = channel + 1;   // frames 0 to 250
        const auto inner = alien::rack_channel::equalSlices(0, 250, 2);
        require(commands.execute(cmd::SliceRackChannel{first, inner}).changed && infoAt(first + 1).sliceStart == 0 && infoAt(first + 2).sliceEnd == 250,
                "slicing a slice keeps inside it");
        require(c.undo(), "undo the inner slices");
    }

    // Saved and opened again with the project.
    {
        const auto saved = alien::rack::rackToVar(rack);
        alien::rack::Project opened; juce::String error;
        require(alien::rack::rackFromVar(saved, opened, error) && opened.info[static_cast<std::size_t>(channel + 3)].sliceStart == 500
                && opened.info[static_cast<std::size_t>(channel + 3)].sliceEnd == 750 && opened.info[static_cast<std::size_t>(channel)].wholeSample(), "slices are saved");
        auto broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("channels", {}).getArray()->getReference(channel + 1).getDynamicObject()->setProperty("sliceEnd", 0);
        alien::rack::Project rejected;
        require(!alien::rack::rackFromVar(broken, rejected, error) && error.contains("slice"), "a slice that ends before it starts is refused when opened");
        auto model = rack;
        require(model.setSample(channel + 1, "builtin:snare") && model.info[static_cast<std::size_t>(channel + 1)].wholeSample(), "a new sound plays whole");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::SliceRackChannel& slice, const char* why) {
            const auto preview = commands.preview(cmd::Command{slice});
            const auto result = commands.execute(slice);
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
        says({channel, {-1, 500, 1000}}, "Slice edges must rise");
        says({channel, {0, 500, 2000}}, "past the end");
        says({channel + 1, {0, 100, 300}}, "stay inside it");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
    }

    // Script form.
    juce::String error;
    const cmd::Command slice = cmd::SliceRackChannel{3, {0, 100, 200}};
    const auto parsed = cmd::parse(cmd::name(slice), juce::JSON::parse(juce::JSON::toString(cmd::arguments(slice))), error);
    require(parsed && std::get<cmd::SliceRackChannel>(*parsed).bounds == std::vector<std::int64_t>{0, 100, 200} && std::get<cmd::SliceRackChannel>(*parsed).channel == 3,
            "SliceRackChannel round-trips through JSON");
    error.clear();
    require(!cmd::parse("SliceRackChannel", juce::JSON::parse(R"({"channel": 0, "bounds": [0, 1.5]})"), error) && error.contains("whole frame"), "frames are whole");
    require(cmd::describe(c.document(), slice).contains("into 2 channels"), "SliceRackChannel describes itself");
}
