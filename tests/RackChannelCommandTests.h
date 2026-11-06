#pragma once
// The Channel Rack's channel commands. ChannelRackViewTests drives the real buttons and menus.
static void rackChannelCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& rack = c.document().channelRack;
    const int start = rack.channelCount;

    // Adding: Preview touches nothing, execute does what it said, Undo and Redo are one step.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::AddRackChannel{"kick"});
        require(preview.ok && preview.changed && preview.after.channelRack.channelCount == start + 1 && alien::sameContent(before, c.document()) && historyOf(c) == history,
            "AddRackChannel Preview changes no live state");
        require(commands.execute(cmd::AddRackChannel{"kick"}).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Add channel"
            && rack.info[static_cast<std::size_t>(start)].sample == "builtin:kick", "AddRackChannel does what Preview said");
        require(c.undo() && rack.channelCount == start && c.redo() && rack.channelCount == start + 1, "AddRackChannel undoes and redoes");
        require(commands.execute(cmd::AddRackChannel{}).changed && rack.info[static_cast<std::size_t>(start + 1)].name == "Sampler" && rack.info[static_cast<std::size_t>(start + 1)].sample.empty(),
            "no sound is an empty sampler");
        require(commands.execute(cmd::AddRackSynthChannel{2}).changed && c.undoLabel() == "Add synth channel" && rack.channels[static_cast<std::size_t>(start + 2)].instrumentPreset == 2
            && rack.isNoteChannel(rack.selectedPattern, start + 2), "a synth channel plays notes in this pattern");
    }
    const int kick = start, sampler = start + 1, synth = start + 2;

    // The mix.
    {
        const auto preview = commands.preview(cmd::SetRackChannelVolume{kick, .25});
        require(preview.ok && preview.after.channelRack.channels[static_cast<std::size_t>(kick)].volume == .25f && rack.channels[static_cast<std::size_t>(kick)].volume != .25f, "volume Preview changes no live state");
        require(commands.execute(cmd::SetRackChannelVolume{kick, .25}).changed && rack.channels[static_cast<std::size_t>(kick)].volume == .25f && c.undoLabel() == "Change channel volume", "volume");
        require(commands.execute(cmd::SetRackChannelPan{kick, -.5}).changed && rack.channels[static_cast<std::size_t>(kick)].pan == -.5f && c.undoLabel() == "Change channel pan", "pan");
        require(commands.execute(cmd::SetRackChannelMute{kick, true}).changed && rack.channels[static_cast<std::size_t>(kick)].mute && c.undoLabel() == "Mute channel", "mute");
        require(commands.execute(cmd::SetRackChannelSolo{kick, true}).changed && rack.channels[static_cast<std::size_t>(kick)].solo && c.undoLabel() == "Solo channel", "solo");
        require(commands.execute(cmd::SetRackChannelOutput{kick, 0}).changed && rack.channels[static_cast<std::size_t>(kick)].output == 0 && c.undoLabel() == "Route channel", "route to a track");
        require(commands.execute(cmd::SetRackChannelOutput{kick, -1}).changed && rack.channels[static_cast<std::size_t>(kick)].output == -1, "route to the master");
        require(commands.execute(cmd::SetRackChannelChoke{kick, 3}).changed && rack.channels[static_cast<std::size_t>(kick)].chokeGroup == 3 && c.undoLabel() == "Change choke group", "choke group");
        require(c.undo() && rack.channels[static_cast<std::size_t>(kick)].chokeGroup == 0, "each is one undo step");
        const auto redo = historyOf(c);
        for (const cmd::Command same : std::vector<cmd::Command>{cmd::SetRackChannelVolume{kick, .25}, cmd::SetRackChannelPan{kick, -.5}, cmd::SetRackChannelMute{kick, true}, cmd::SetRackChannelSolo{kick, true},
                                                                 cmd::SetRackChannelOutput{kick, -1}, cmd::SetRackChannelChoke{kick, 0}, cmd::RenameRackChannel{kick, juce::String::fromUTF8(rack.info[static_cast<std::size_t>(kick)].name.c_str())},
                                                                 cmd::MoveRackChannel{kick, kick}})
            require(commands.execute(same).ok && historyOf(c) == redo, "an unchanged channel setting preserves redo");
        require(c.redo(), "redo after no-ops");
    }

    // Grouping: a gesture's commands merge under one key, and a batch is one step.
    {
        c.markAsNewBaseline();
        for (const double level : {.3, .4, .5}) require(commands.batch({cmd::SetRackChannelVolume{kick, level}, cmd::SetRackChannelVolume{sampler, level}}, "Change channel volumes", "knob-1").changed, "knob step");
        require(rack.channels[static_cast<std::size_t>(sampler)].volume == .5f && c.undoLabel() == "Change channel volumes" && c.undo() && !c.canUndo(), "a knob gesture is one undo step");
        c.markAsNewBaseline();
    }

    // Structure.
    {
        const auto kickName = rack.info[static_cast<std::size_t>(kick)].name;
        require(commands.execute(cmd::RenameRackChannel{kick, "Boom"}).changed && rack.info[static_cast<std::size_t>(kick)].name == "Boom" && c.undoLabel() == "Rename channel", "rename");
        require(c.undo() && rack.info[static_cast<std::size_t>(kick)].name == kickName && c.redo(), "rename undoes");
        const auto lane = rack.lanes[static_cast<std::size_t>(kick)];
        require(commands.execute(cmd::MoveRackChannel{kick, synth}).changed && rack.lanes[static_cast<std::size_t>(synth)] == lane && rack.info[static_cast<std::size_t>(synth)].name == "Boom" && c.undoLabel() == "Move channel",
            "a channel moves any distance and keeps its identity");
        require(c.undo() && rack.lanes[static_cast<std::size_t>(kick)] == lane, "moving is one undo step");
        const auto count = rack.channelCount;
        const auto duplicated = commands.preview(cmd::DuplicateRackChannel{kick, 77});
        require(commands.execute(cmd::DuplicateRackChannel{kick, 77}).changed && rack.channelCount == count + 1 && rack.info[static_cast<std::size_t>(kick + 1)].name == "Boom copy"
            && alien::sameContent(duplicated.after, c.document()) && c.undoLabel() == "Duplicate channel", "DuplicateRackChannel does what Preview said");
        require(commands.execute(cmd::RemoveRackChannel{kick + 1}).changed && rack.channelCount == count && c.undoLabel() == "Delete channel", "delete");
        require(c.undo() && rack.channelCount == count + 1 && c.undo() && rack.channelCount == count, "delete undoes");
    }

    // A plugin channel's copy gets the identity the command carries, so Preview and execute agree.
    {
        c.markAsNewBaseline();
        require(c.setRackPlugin(synth, "<PLUGIN name=\"Test\"/>", "Test") && rack.info[static_cast<std::size_t>(synth)].hasPlugin(), "plugin channel fixture");
        const auto original = rack.info[static_cast<std::size_t>(synth)].pluginInstance();
        require(!commands.execute(cmd::DuplicateRackChannel{synth, 0}).ok, "a plugin copy needs an identity");
        const cmd::DuplicateRackChannel copy{synth};
        const auto preview = commands.preview(copy);
        require(preview.ok && preview.after.channelRack.info[static_cast<std::size_t>(synth + 1)].pluginInstance() == copy.instance && copy.instance != original, "Preview shows the carried identity");
        require(commands.execute(copy).changed && rack.info[static_cast<std::size_t>(synth + 1)].pluginInstance() == copy.instance && alien::sameContent(preview.after, c.document()), "execute uses it");
        require(rack.info[static_cast<std::size_t>(synth)].pluginInstance() == original, "the original keeps its identity");
        require(c.undo(), "plugin copy undoes");
    }

    // Refusals change nothing.
    {
        c.markAsNewBaseline();
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const int outside = rack.channelCount;
        for (const cmd::Command bad : std::vector<cmd::Command>{
                 cmd::AddRackChannel{"nothing-like-this"}, cmd::AddRackSynthChannel{-1}, cmd::AddRackSynthChannel{alien::presetCount},
                 cmd::RemoveRackChannel{outside}, cmd::RemoveRackChannel{-1}, cmd::MoveRackChannel{0, outside}, cmd::MoveRackChannel{outside, 0}, cmd::DuplicateRackChannel{outside},
                 cmd::RenameRackChannel{0, "   "}, cmd::RenameRackChannel{outside, "Nope"},
                 cmd::SetRackChannelVolume{0, -.01}, cmd::SetRackChannelVolume{0, 1.01}, cmd::SetRackChannelVolume{0, nan}, cmd::SetRackChannelPan{0, -1.01}, cmd::SetRackChannelPan{0, nan}, cmd::SetRackChannelVolume{outside, .5},
                 cmd::SetRackChannelMute{outside, true}, cmd::SetRackChannelSolo{outside, true},
                 cmd::SetRackChannelOutput{0, -2}, cmd::SetRackChannelOutput{0, c.document().trackCount()}, cmd::SetRackChannelOutput{outside, -1},
                 cmd::SetRackChannelChoke{0, -1}, cmd::SetRackChannelChoke{0, alien::rack::maxChokeGroup + 1}})
            require(!commands.execute(bad).ok && alien::sameContent(before, c.document()) && historyOf(c) == history, "a bad rack channel edit changes nothing");
        require(!commands.batch({cmd::AddRackChannel{"snare"}, cmd::SetRackChannelMute{0, true}, cmd::RenameTrack{99, "Never"}}, "Bad").ok && alien::sameContent(before, c.document()) && historyOf(c) == history,
            "a refused later command leaves no channel behind");
        c.setExternalBusy(true);
        require(!commands.execute(cmd::AddRackChannel{"snare"}).ok && !commands.execute(cmd::SetRackChannelMute{0, true}).ok && alien::sameContent(before, c.document()), "a busy project refuses");
        c.setExternalBusy(false);
        // The most channels a rack holds.
        std::vector<cmd::Command> fill(static_cast<std::size_t>(alien::rack::maxChannels - rack.channelCount), cmd::AddRackChannel{"snare"});
        if (!fill.empty()) require(commands.batch(fill, "Fill").changed && rack.channelCount == alien::rack::maxChannels, "a sequential batch fills the rack");
        require(!commands.execute(cmd::AddRackChannel{"snare"}).ok && !commands.execute(cmd::AddRackSynthChannel{0}).ok && !commands.execute(cmd::DuplicateRackChannel{0}).ok, "a full rack refuses another channel");
        if (!fill.empty()) require(c.undo() && alien::sameContent(before, c.document()), "the fill is one undo step");
    }

    // Script form.
    juce::String error;
    for (const cmd::Command edit : std::vector<cmd::Command>{cmd::AddRackChannel{"kick"}, cmd::AddRackChannel{}, cmd::AddRackSynthChannel{1}, cmd::RemoveRackChannel{0}, cmd::MoveRackChannel{0, 1}, cmd::DuplicateRackChannel{0},
                                                             cmd::RenameRackChannel{0, "Name"}, cmd::SetRackChannelVolume{0, .5}, cmd::SetRackChannelPan{0, -.5}, cmd::SetRackChannelMute{0, true}, cmd::SetRackChannelSolo{0, false},
                                                             cmd::SetRackChannelOutput{0, -1}, cmd::SetRackChannelChoke{0, 2}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && cmd::name(*parsed) == cmd::name(edit) && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit))
            && cmd::describe(c.document(), edit).isNotEmpty(), "rack channel commands round trip by name and arguments");
    }
    for (const auto& [commandName, json] : std::vector<std::pair<const char*, const char*>>{
             {"AddRackChannel", "{}"}, {"AddRackChannel", "{\"sound\":1}"}, {"AddRackSynthChannel", "{}"}, {"RemoveRackChannel", "{\"channel\":\"1\"}"}, {"MoveRackChannel", "{\"channel\":0}"},
             {"DuplicateRackChannel", "{\"channel\":0,\"instance\":5}"}, {"RenameRackChannel", "{\"channel\":0}"}, {"SetRackChannelVolume", "{\"channel\":0,\"volume\":true}"}, {"SetRackChannelPan", "{\"channel\":0}"},
             {"SetRackChannelMute", "{\"channel\":0,\"muted\":1}"}, {"SetRackChannelSolo", "{\"channel\":0}"}, {"SetRackChannelOutput", "{\"channel\":0,\"output\":1.5}"}, {"SetRackChannelChoke", "{\"channel\":0,\"group\":1,\"extra\":1}"}})
        require(!cmd::parse(commandName, juce::JSON::parse(json), error), "rack channel parsing rejects missing, mistyped and extra arguments");
}
