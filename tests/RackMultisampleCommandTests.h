#pragma once
// LoadRackMultisample (T2-01): several files on one Channel Rack channel's keys (key zones with files of their own,
// format 39). MemoryPeakTests opens, relinks and collects a multisample's files; RackEngineTests plays key zones.
static void rackMultisampleCommandChecks() {
    namespace rc = alien::rack_channel;
    // A file's name says the note it was recorded on, as the app names notes (C4 is note 60).
    {
        const auto note = [](const char* stem) { return rc::noteInFileName(stem); };
        require(note("Piano_C4") == 60 && note("Strings Eb2 soft") == 39 && note("pad_F#3") == 54 && note("b3") == 59
                && note("Piano_C4_v2") == 60 && note("C-1") == 0 && note("G9") == 127 && note("Choir A3 C4") == 60,
                "a note letter, accidental and octave in a file name give its note");
        require(!note("Bass2") && !note("Kick 808") && !note("sub2") && !note("A10") && !note("Snare") && !note("G#9") && !note("Cb-2"),
                "a letter inside a word, a longer number or a note past 127 is no note");
    }
    // The layout: by note when every name says one, the keys between split halfway; otherwise one key each from C4.
    {
        auto layout = rc::multisampleLayout({"Piano_C4", "Piano_C3", "Piano_G3"});
        require(layout.error.isEmpty() && layout.fromNames && layout.order == std::vector<int>{1, 2, 0}, "named files go lowest note first");
        require(layout.zones.size() == 3 && layout.zones[0] == alien::rack::KeyZone{0, 0, 0, 51, 48}
                && layout.zones[1] == alien::rack::KeyZone{0, 0, 52, 57, 55} && layout.zones[2] == alien::rack::KeyZone{0, 0, 58, 127, 60}
                && alien::rack::validKeyZones(layout.zones), "each file plays at its own pitch on its note, the keys between split halfway, 0 to 127 covered");
        layout = rc::multisampleLayout({"Kick", "Snare", "Hat 2", "Hat 10"});
        require(layout.error.isEmpty() && !layout.fromNames && layout.order == std::vector<int>{2, 3, 0, 1} && layout.zones.size() == 4 && layout.zones[0] == alien::rack::KeyZone{0, 0, 60, 60, 60}
                && layout.zones[3] == alien::rack::KeyZone{0, 0, 63, 63, 63}, "unnamed files get one key each from C4, in natural name order");
        layout = rc::multisampleLayout({"Piano_C4", "Kick"});
        require(!layout.fromNames && layout.zones[0].low == 60, "one name without a note lays them all out as a kit");
        layout = rc::multisampleLayout({"Piano_C4", "Rhodes C4"});
        require(layout.error.contains("both on C4") && layout.zones.empty(), "two files on one note are refused");
        require(rc::multisampleLayout({"Piano_C4"}).error.contains("2 to 16"), "one file is no multisample");
        std::vector<juce::String> seventeen;
        for (int k = 0; k < 17; ++k) seventeen.push_back("Kit " + juce::String(k));
        require(rc::multisampleLayout(seventeen).error.contains("2 to 16"), "seventeen files are too many");
        require(rc::multisampleName({"Piano_C3", "Piano_C4"}) == "Piano" && rc::multisampleName({"Strings A2", "Strings A3"}) == "Strings"
                && rc::multisampleName({"Pad1", "Pad2"}) == "Pad" && rc::multisampleName({"Kick", "Snare"}) == "Multisample",
                "the channel is named for what the files share");
    }

    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "multisample fixture channel");
    const int channel = rack.channelCount - 1;
    const auto level = [](double rate, float value) {
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = rate; clip->channels = 1; clip->frames = 800;
        clip->left.assign(800, value);
        clip->buildWaveform();
        return clip;
    };
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("alien multisample");
    const std::vector<juce::File> files{folder.getChildFile("Keys_C4.wav"), folder.getChildFile("Keys_C3.wav"), folder.getChildFile("Keys_G3.wav")};
    const std::vector<std::shared_ptr<alien::SessionAudioClip>> clips{level(8000, 0.6f), level(16000, 0.2f), level(44100, 0.4f)};
    const auto path = [&](std::size_t k) { return files[k].getFullPathName().toStdString(); };
    const auto infoAt = [&](int ch) { return rack.info[static_cast<std::size_t>(ch)]; };
    c.markAsNewBaseline();

    // Three files on one channel's keys: Preview changes nothing, execute does what it said, one Undo step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        std::vector<juce::String> paths;
        for (const auto& file : files) paths.push_back(file.getFullPathName());
        const cmd::LoadRackMultisample load{channel, paths, "Keys", clips};
        const auto preview = commands.preview(cmd::Command{load});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history, "LoadRackMultisample Preview changes no live state");
        const auto free = c.availableAudioBytes();
        const auto result = cmd::loadDecodedRackMultisample(c, files, channel, clips);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Load multisample",
                "LoadRackMultisample does what Preview said, as one Undo step");
        require(free - c.availableAudioBytes() == 3 * 800 * sizeof(float), "every file's audio is counted");
        const auto info = infoAt(channel);
        require(info.name == "Keys" && info.sample == path(1) && info.wholeSample() && info.hasZones() && info.zones->size() == 3,
                "the channel is named for its files and plays the lowest one as its sample");
        require((*info.zones)[0] == alien::rack::KeyZone{0, 0, 0, 51, 48} && (*info.zones)[1] == alien::rack::KeyZone{0, 0, 52, 57, 55, path(2)}
                && (*info.zones)[2] == alien::rack::KeyZone{0, 0, 58, 127, 60, path(0)}, "the other zones name their own files");
        require(info.files() == std::vector<std::string>{path(1), path(2), path(0)}, "the channel names its three files");
        const auto* map = c.rackChannelAudio(channel);
        require(map != nullptr && map->keyZones && map->keyZones->size() == 3 && map->left.empty() && engine->rackSamples[static_cast<std::size_t>(channel)].load() == map,
                "the engine plays a key map");
        require((*map->keyZones)[0].clip == clips[1] && (*map->keyZones)[1].clip == clips[2] && (*map->keyZones)[2].clip == clips[0]
                && (*map->keyZones)[1].clip->sampleRate == 44100, "each zone plays its own file's decoded audio, at its own rate");
        require(!c.rackChannelOffline(channel) && c.missingMedia().empty(), "nothing is missing");
        const auto again = cmd::loadDecodedRackMultisample(c, files, channel, clips);
        require(again.ok && !again.changed && c.rackChannelAudio(channel) == map, "the same files again change nothing, and the map is reused");
        require(c.undo() && alien::sameContent(before, c.document()) && !infoAt(channel).hasZones() && infoAt(channel).isBuiltin(), "Undo gives the channel its sound back");
        require(c.redo() && infoAt(channel) == info && c.rackChannelAudio(channel) == map, "Redo plays the multisample again");
        require(cmd::describe(c.document(), cmd::Command{load}).contains("multisample of 3 files"), "LoadRackMultisample describes itself");
    }

    // Saved and opened again: the channel's own sample has no file on its zone, the others do.
    {
        const auto saved = alien::rack::rackToVar(rack);
        const auto zones = saved.getProperty("channels", {}).getArray()->getReference(channel).getProperty("zones", {});
        require(!zones[0].hasProperty("file") && zones[1].getProperty("file", {}).toString().toStdString() == path(2), "only a zone's own file is written");
        alien::rack::Project opened; juce::String error;
        require(alien::rack::rackFromVar(saved, opened, error) && opened.info[static_cast<std::size_t>(channel)] == infoAt(channel), "a multisample is saved");
        auto broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("channels", {}).getArray()->getReference(channel).getProperty("zones", {}).getArray()->getReference(1).getDynamicObject()->setProperty("file", "");
        alien::rack::Project rejected;
        require(!alien::rack::rackFromVar(broken, rejected, error) && error.contains("key zones"), "an empty zone file is refused when opened");
        broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("channels", {}).getArray()->getReference(channel).getProperty("zones", {}).getArray()->getReference(1).getDynamicObject()->setProperty("file", "builtin:kick");
        require(!alien::rack::rackFromVar(broken, rejected, error) && error.contains("key zones"), "a zone cannot name a built-in sound");
    }

    // A file moved (relinked, collected or resolved on open) keeps the channel's slices and zones.
    {
        auto model = rack;
        require(model.moveFile(path(2), "D:/moved/Keys_G3.wav") && (*model.info[static_cast<std::size_t>(channel)].zones)[1].file == "D:/moved/Keys_G3.wav"
                && model.info[static_cast<std::size_t>(channel)].zones->size() == 3 && model.valid(), "a zone's file moves alone");
        require(model.moveFile(path(1), "D:/moved/Keys_C3.wav") && model.info[static_cast<std::size_t>(channel)].sample == "D:/moved/Keys_C3.wav"
                && model.info[static_cast<std::size_t>(channel)].zones->size() == 3, "the channel's own sample moves and keeps its zones");
        require(model.moveFile(path(0), "D:/moved/Keys_C3.wav") && (*model.info[static_cast<std::size_t>(channel)].zones)[2].file.empty()
                && model.valid(), "a zone moved onto the channel's own sample names it by no file");
        require(!model.moveFile("D:/nothing.wav", "D:/else.wav"), "a file no channel names moves nothing");
        auto sliced = rack;
        require(sliced.setSample(0, path(1)) && sliced.sliceChannel(0, {0, 400, 800}) && sliced.moveFile(path(1), "D:/moved/Keys_C3.wav")
                && sliced.info[1].sliceEnd == 400 && sliced.info[1].sample == "D:/moved/Keys_C3.wav", "a slice channel's file moves and keeps its slice");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](cmd::LoadRackMultisample load, const char* why) {
            const auto result = commands.execute(load);
            require(!result.ok && result.error.contains(why) && !result.changed, why);
        };
        const std::vector<juce::String> two{files[0].getFullPathName(), files[1].getFullPathName()};
        says({channel, two, "Keys", {clips[0]}}, "for every file");
        says({channel, two, "Keys", {clips[0], nullptr}}, "for every file");
        says({channel, {two[0]}, "Keys", {clips[0]}}, "2 to 16");
        says({channel, {two[0], two[0]}, "Keys", {clips[0], clips[1]}}, "each file once");
        says({channel, {two[0], "Keys_C3.wav"}, "Keys", {clips[0], clips[1]}}, "full path");
        says({channel, {two[0], folder.getChildFile("Rhodes C4.wav").getFullPathName()}, "Keys", {clips[0], clips[1]}}, "both on C4");
        says({99, two, "Keys", {clips[0], clips[1]}}, "no rack channel");
        says({channel, two, " ", {clips[0], clips[1]}}, "cannot be blank");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
    }

    // No script form: it carries decoded audio.
    juce::String error;
    require(!cmd::parse("LoadRackMultisample", juce::JSON::parse(R"({"channel": 0, "paths": ["a.wav", "b.wav"]})"), error)
            && error.contains("decoded audio"), "LoadRackMultisample has no script form");
}
