#pragma once
// SetRackChannelKeyZone and RemoveRackChannelKeyZone (T2-01 zone editor): a key zone's keys and root changed, or the
// zone taken away, on a channel with key zones. ChannelRackViewTests has the panel that runs them.
static void rackKeyZoneCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"snare"}).changed, "a channel with no key zones");
    const int plain = rack.channelCount - 1;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "key zone fixture channel");
    const int channel = rack.channelCount - 1;
    const auto level = [](float value) {
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = 8000; clip->channels = 1; clip->frames = 800;
        clip->left.assign(800, value);
        clip->buildWaveform();
        return clip;
    };
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("alien key zones");
    const std::vector<juce::File> files{folder.getChildFile("Keys_C3.wav"), folder.getChildFile("Keys_G3.wav"), folder.getChildFile("Keys_C4.wav")};
    require(cmd::loadDecodedRackMultisample(c, files, channel, {level(0.2f), level(0.4f), level(0.6f)}).changed, "a multisample of three files");
    c.markAsNewBaseline();
    const auto zones = [&] { return rack.info[static_cast<std::size_t>(channel)].zones; };
    const auto g3 = files[1].getFullPathName().toStdString();
    require(zones() != nullptr && zones()->size() == 3 && (*zones())[1] == alien::rack::KeyZone{0, 0, 52, 57, 55, g3}, "the files on C3, G3 and C4");

    // New keys and root for the middle zone: Preview changes nothing, execute does what it said, one Undo step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const cmd::SetRackChannelKeyZone edit{channel, 1, 53, 56, 54};
        const auto preview = commands.preview(cmd::Command{edit});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history,
                "SetRackChannelKeyZone Preview changes no live state");
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Edit key zone",
                "SetRackChannelKeyZone does what Preview said, as one Undo step");
        require((*zones())[1] == alien::rack::KeyZone{0, 0, 53, 56, 54, g3} && (*zones())[0].high == 51 && (*zones())[2].low == 58,
                "the zone keeps its file and the others keep their keys");
        const auto* map = c.rackChannelAudio(channel);
        require(map != nullptr && map->keyZones != nullptr && (*map->keyZones)[1].low == 53 && (*map->keyZones)[1].high == 56 && (*map->keyZones)[1].root == 54
                && engine->rackSamples[static_cast<std::size_t>(channel)].load() == map, "the engine plays the new keys");
        const auto again = commands.execute(edit);
        require(again.ok && !again.changed && c.undoLabel() == "Edit key zone", "the same keys again change nothing");
        require(commands.execute(cmd::SetRackChannelKeyZone{channel, 2, 57, 127, 72}).changed && (*zones())[2].root == 72, "a zone reaches down to the one below's edge");
        require(c.undo() && c.undo() && alien::sameContent(before, c.document()), "Undo puts each zone back");
        require(c.redo() && (*zones())[1].root == 54 && c.undo(), "Redo brings the edit back");
    }

    // Removing zones, down to none: the sample on every key again.
    {
        const auto before = c.document();
        const auto preview = commands.preview(cmd::Command{cmd::RemoveRackChannelKeyZone{channel, 0}});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()), "RemoveRackChannelKeyZone Preview changes no live state");
        const auto result = commands.execute(cmd::RemoveRackChannelKeyZone{channel, 0});
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Remove key zone"
                && zones()->size() == 2 && (*zones())[0].file == g3 && c.rackChannelAudio(channel)->keyZones->size() == 2,
                "RemoveRackChannelKeyZone takes the zone away as one Undo step; the rest keep their files");
        require(commands.execute(cmd::RemoveRackChannelKeyZone{channel, 1}).changed && commands.execute(cmd::RemoveRackChannelKeyZone{channel, 0}).changed
                && zones() == nullptr && c.rackChannelAudio(channel) != nullptr && c.rackChannelAudio(channel)->keyZones == nullptr,
                "with no zones left the channel plays its sample on every key");
        require(c.undo() && c.undo() && c.undo() && alien::sameContent(before, c.document()) && c.rackChannelAudio(channel)->keyZones->size() == 3,
                "three Undo steps bring the three zones back");
    }

    // Refusals change nothing and say why, in Preview too.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& command, const char* why) {
            const auto preview = commands.preview(command);
            const auto result = commands.execute(command);
            require(!preview.ok && !result.ok && !result.changed && result.error.contains(why), why);
        };
        says(cmd::SetRackChannelKeyZone{channel, 1, 50, 57, 55}, "would overlap zone 1");
        says(cmd::SetRackChannelKeyZone{channel, 1, 52, 58, 55}, "would overlap zone 3");
        says(cmd::SetRackChannelKeyZone{channel, 2, 58, 128, 60}, "notes 0 to 127");
        says(cmd::SetRackChannelKeyZone{channel, 0, -1, 51, 48}, "notes 0 to 127");
        says(cmd::SetRackChannelKeyZone{channel, 0, 0, 51, 130}, "notes 0 to 127");
        says(cmd::SetRackChannelKeyZone{channel, 1, 57, 52, 55}, "low key cannot be above");
        says(cmd::SetRackChannelKeyZone{channel, 3, 0, 1, 0}, "no key zone 4");
        says(cmd::RemoveRackChannelKeyZone{channel, 5}, "no key zone 6");
        says(cmd::SetRackChannelKeyZone{plain, 0, 60, 60, 60}, "has no key zones");
        says(cmd::RemoveRackChannelKeyZone{plain, 0}, "has no key zones");
        says(cmd::RemoveRackChannelKeyZone{99, 0}, "no rack channel");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
    }

    // Script form.
    juce::String error;
    const cmd::Command edit = cmd::SetRackChannelKeyZone{2, 1, 40, 47, 45};
    auto parsed = cmd::parse(cmd::name(edit), juce::JSON::parse(juce::JSON::toString(cmd::arguments(edit))), error);
    require(parsed && std::get<cmd::SetRackChannelKeyZone>(*parsed).channel == 2 && std::get<cmd::SetRackChannelKeyZone>(*parsed).zone == 1
            && std::get<cmd::SetRackChannelKeyZone>(*parsed).low == 40 && std::get<cmd::SetRackChannelKeyZone>(*parsed).high == 47
            && std::get<cmd::SetRackChannelKeyZone>(*parsed).root == 45, "SetRackChannelKeyZone round-trips through JSON");
    const cmd::Command remove = cmd::RemoveRackChannelKeyZone{2, 3};
    parsed = cmd::parse(cmd::name(remove), juce::JSON::parse(juce::JSON::toString(cmd::arguments(remove))), error);
    require(parsed && std::get<cmd::RemoveRackChannelKeyZone>(*parsed).channel == 2 && std::get<cmd::RemoveRackChannelKeyZone>(*parsed).zone == 3,
            "RemoveRackChannelKeyZone round-trips through JSON");
    error.clear();
    require(!cmd::parse("SetRackChannelKeyZone", juce::JSON::parse(R"({"channel": 0, "zone": 0, "low": 1, "high": 2})"), error) && error.contains("\"root\""),
            "a zone needs its root");
    require(cmd::describe(c.document(), cmd::SetRackChannelKeyZone{channel, 1, 53, 56, 54}).contains("key zone 2")
            && cmd::describe(c.document(), cmd::SetRackChannelKeyZone{channel, 1, 53, 56, 54}).contains("root F#3"), "SetRackChannelKeyZone describes itself");
    require(cmd::describe(c.document(), remove).contains("Remove key zone 4"), "RemoveRackChannelKeyZone describes itself");
}
