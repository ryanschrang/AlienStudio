#pragma once
// What a rack channel plays, and pasting copied steps. ChannelRackViewTests drives the real menu items.
static void rackSoundCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed && commands.execute(cmd::AddRackChannel{"snare"}).changed, "sound command fixture");
    const int first = rack.channelCount - 2, second = rack.channelCount - 1, pattern = rack.selectedPattern;
    const auto info = [&](int channel) -> const auto& { return rack.info[static_cast<std::size_t>(channel)]; };

    // A built-in sound: Preview touches nothing, execute does what it said, one Undo step, and the channel takes its name.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::SetRackChannelSound{first, "hat"});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history, "SetRackChannelSound Preview changes no live state");
        require(commands.execute(cmd::SetRackChannelSound{first, "hat"}).changed && alien::sameContent(preview.after, c.document()) && info(first).sample == "builtin:hat"
            && info(first).name == alien::rack::builtinDisplayName("hat") && c.undoLabel() == "Change channel sound", "SetRackChannelSound does what Preview said");
        require(c.undo() && info(first).sample == "builtin:kick" && c.redo() && info(first).sample == "builtin:hat", "and undoes and redoes");
        const auto redo = historyOf(c);
        require(commands.execute(cmd::SetRackChannelSound{first, "hat"}).ok && historyOf(c) == redo, "the same sound preserves history");
    }

    // Silence keeps the name; a synth takes the preset's name; one batch across a selection is one step.
    {
        const auto name = info(first).name;
        require(commands.execute(cmd::SilenceRackChannel{first}).changed && info(first).sample.empty() && info(first).name == name && c.undoLabel() == "Silence channel", "silence");
        require(commands.execute(cmd::SetRackChannelSynth{first, 2}).changed && rack.channels[static_cast<std::size_t>(first)].instrumentPreset == 2
            && info(first).name == alien::presetNames[2] && c.undoLabel() == "Change channel instrument", "synth");
        require(commands.execute(cmd::SilenceRackChannel{first}).changed && !rack.channels[static_cast<std::size_t>(first)].isInstrument(), "silencing a synth leaves no instrument");
        require(commands.batch({cmd::SetRackChannelSynth{first, 1}, cmd::SetRackChannelSynth{second, 1}}, "Change channel instrument").changed
            && rack.channels[static_cast<std::size_t>(first)].instrumentPreset == 1 && rack.channels[static_cast<std::size_t>(second)].instrumentPreset == 1, "a selection");
        require(c.undo() && rack.channels[static_cast<std::size_t>(second)].instrumentPreset == -1 && info(second).sample == "builtin:snare", "is one undo step");
    }

    // A plugin: Preview and execute agree on its identity, the host hears of it once, and the sound it replaces comes back on Undo. (Undo and Redo reach the host through the document restore.)
    {
        int told = 0;
        c.onRackPluginsChanged = [&] { ++told; };
        const cmd::SetRackChannelPlugin plugin{second, "<PLUGIN name=\"Test\"/>", "Test"};
        const auto preview = commands.preview(plugin);
        require(preview.ok && preview.changed && told == 0 && !info(second).hasPlugin(), "SetRackChannelPlugin Preview changes no live state and tells no host");
        require(commands.execute(plugin).changed && alien::sameContent(preview.after, c.document()) && info(second).pluginInstance() == plugin.instance && info(second).name == "Test"
            && info(second).sample.empty() && c.undoLabel() == "Change channel instrument", "SetRackChannelPlugin does what Preview said");
        require(told == 1, "the host rebuilds once");
        require(c.undo() && !info(second).hasPlugin() && info(second).sample == "builtin:snare", "Undo puts the sound back");
        require(c.redo() && info(second).pluginInstance() == plugin.instance, "Redo brings the same instance back");
        require(commands.execute(cmd::SetRackChannelSound{second, "snare"}).changed && !info(second).hasPlugin() && told == 2, "a built-in sound replaces the plugin, and the host hears of it");
        c.onRackPluginsChanged = nullptr;
        require(commands.execute(cmd::SetRackChannelPlugin{second, "<PLUGIN/>", ""}).changed && info(second).name == alien::rack::builtinDisplayName("snare"), "no name keeps the channel's name");
        require(c.undo(), "plugin fixture undoes");
    }

    // Pasting steps: only as far as the pattern's length, and a note channel stays as it is.
    {
        auto row = std::make_shared<alien::rack::Project::StepRow>();
        (*row)[2].on = true;
        const auto copied = std::shared_ptr<const alien::rack::Project::StepRow>(row);
        const auto preview = commands.preview(cmd::PasteRackSteps{pattern, second, copied});
        const auto stepOn = [&](int channel, int step) { return rack.pattern(pattern).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)].on; };
        require(preview.ok && preview.changed && !stepOn(second, 2), "PasteRackSteps Preview changes no live state");
        require(commands.execute(cmd::PasteRackSteps{pattern, second, copied}).changed && stepOn(second, 2) && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Paste steps", "PasteRackSteps does what Preview said");
        require(c.undo() && !stepOn(second, 2) && c.redo() && stepOn(second, 2), "and undoes and redoes");
        require(commands.execute(cmd::AddRackSynthChannel{0}).changed, "paste note channel fixture");
        const auto history = historyOf(c);
        require(commands.execute(cmd::PasteRackSteps{pattern, rack.channelCount - 1, copied}).ok && historyOf(c) == history, "a note channel has no steps to paste into");
        require(c.undo(), "paste fixture undoes");
    }

    // Refusals.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetRackChannelSound{99, "hat"}, "no rack channel");
        says(cmd::SetRackChannelSound{first, "cowbell-from-nowhere"}, "no built-in sound");
        says(cmd::SilenceRackChannel{99}, "no rack channel");
        says(cmd::SetRackChannelSynth{first, alien::presetCount}, "Synth presets");
        says(cmd::SetRackChannelSynth{first, -1}, "Synth presets");
        says(cmd::SetRackChannelPlugin{first, "", "Empty"}, "description");
        says(cmd::SetRackChannelPlugin{first, "<PLUGIN/>", "None", 0}, "identity");
        says(cmd::PasteRackSteps{pattern, first, nullptr}, "no copied steps");
        says(cmd::PasteRackSteps{99, first, std::make_shared<const alien::rack::Project::StepRow>()}, "no rack pattern");
        says(cmd::PasteRackSteps{pattern, 99, std::make_shared<const alien::rack::Project::StepRow>()}, "no rack channel");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");
        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetRackChannelSynth{first, 3});
        c.setExternalBusy(false);
        require(!busy.ok && historyOf(c) == history, "a busy project refuses them");
    }

    // The script form.
    juce::String error;
    for (const cmd::Command edit : std::vector<cmd::Command>{cmd::SetRackChannelSound{0, "hat"}, cmd::SilenceRackChannel{0}, cmd::SetRackChannelSynth{0, 1}, cmd::SetRackChannelPlugin{0, "<PLUGIN/>", "Name"}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && cmd::name(*parsed) == cmd::name(edit) && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "sound commands round-trip their script form");
    }
    const auto script = cmd::parse("SetRackChannelPlugin", juce::JSON::parse("{\"channel\":0,\"description\":\"<PLUGIN/>\",\"name\":\"\"}"), error);
    require(script && std::get<cmd::SetRackChannelPlugin>(*script).instance != 0, "a parsed plugin command gets its own identity");
    require(!cmd::parse("PasteRackSteps", juce::JSON::parse("{\"pattern\":0,\"channel\":0}"), error) && error.contains("cannot be written"), "copied steps have no script form");
    for (const auto& [commandName, json] : std::vector<std::pair<const char*, const char*>>{
             {"SetRackChannelSound", "{\"channel\":0}"}, {"SetRackChannelSound", "{\"channel\":0,\"sound\":1}"}, {"SilenceRackChannel", "{\"channel\":\"0\"}"}, {"SetRackChannelSynth", "{\"channel\":0}"},
             {"SetRackChannelPlugin", "{\"channel\":0,\"description\":\"x\"}"}, {"SetRackChannelPlugin", "{\"channel\":0,\"description\":\"x\",\"name\":\"y\",\"instance\":5}"}})
        require(!cmd::parse(commandName, juce::JSON::parse(json), error), "sound command parsing rejects missing, mistyped and extra arguments");
}
