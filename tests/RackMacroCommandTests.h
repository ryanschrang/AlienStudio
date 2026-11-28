#pragma once
// A plugin rack's eight macros (RACK-01, T2-02) as named commands: move, rename, map and unmap, with Preview, Undo,
// saving (format 39), what a removed device does to its targets, refusals and script forms. PluginAutomationPlaybackTests
// checks that the host sets the mapped parameters of loaded plugins; PluginPanelTests the knobs.
static void rackMacroCommandChecks() {
    using alien::RackStage;
    // The model: each target maps the macro onto its own range, which may run backwards.
    {
        alien::MacroTarget up{7, "cutoff", 0.2f, 0.6f}, down{7, "mix", 1, 0};
        require(std::abs(up.valueAt(0.5f) - 0.4f) < 1e-6f && down.valueAt(0.25f) == 0.75f && up.valueAt(2) == 0.6f && up.valueAt(-1) == 0.2f,
                "a target maps the macro across its range, clamped to 0 to 1");
        require(up.valid() && !alien::MacroTarget{0, "cutoff"}.valid() && !alien::MacroTarget{7, ""}.valid() && !alien::MacroTarget{7, "x", -0.1f, 1}.valid()
                && !alien::MacroTarget{7, "x", 0, 1.5f}.valid() && !alien::MacroTarget{7, "x", 0, std::numeric_limits<float>::quiet_NaN()}.valid(),
                "a target needs a device, a parameter and a range inside 0 to 1");
        alien::PluginRackState rack;
        rack.effects = {deviceEffect("A")};
        const auto a = rack.effects[0].instanceId;
        rack.macros[0].targets = {{a, "cutoff"}, {a, "cutoff", 0, 0.5f}};
        require(!rack.valid(), "one macro names a parameter once");
        rack.macros[0].targets = {{a, "cutoff"}};
        rack.macros[1].targets = {{a, "cutoff"}};
        require(!rack.valid(), "one parameter follows one macro at most");
        rack.macros[1].targets = {{a, "mix"}, {99, "mix"}};
        rack.macros[2].value = 1.5f;
        require(!rack.valid(), "a macro's value is 0 to 1");
        rack.macros[2].value = 1;
        require(rack.valid() && rack.dropStaleMacroTargets() && rack.macros[1].targets.size() == 1 && !rack.dropStaleMacroTargets(),
                "targets on a device that is not in the rack can be dropped");
        require(rack.devicesOnly().macros == alien::RackMacros{} && rack.devicesOnly().effects == rack.effects, "the devices without the macros");
        require(alien::macroDisplayName(rack.macros[3], 3) == "Macro 4", "an unnamed macro is called by its number");
    }

    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    require(commands.execute(cmd::InsertDevice{RackStage::track, 0, 0, deviceEffect("Filter")}).changed
            && commands.execute(cmd::InsertDevice{RackStage::track, 0, 1, deviceEffect("Drive")}).changed
            && commands.execute(cmd::InsertDevice{RackStage::master, 0, 0, deviceEffect("Glue")}).changed, "macro fixture devices");
    const auto rack = [&]() -> const alien::PluginRackState& { return c.document().tracks[0].plugins; };
    const auto filter = rack().effects[0].instanceId, drive = rack().effects[1].instanceId, glue = c.document().masterPlugins.effects[0].instanceId;
    c.markAsNewBaseline();
    const auto before = c.document();

    // Map: Preview changes nothing, execute does what it said, one Undo step each.
    {
        const cmd::Command map = cmd::MapRackMacro{RackStage::track, 0, 2, filter, "cutoff", 0.1, 0.9};
        const auto preview = commands.preview(map);
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && !c.canUndo() && preview.changes[0].contains("Macro 3"),
                "MapRackMacro Preview changes no live state");
        require(commands.execute(map).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Map macro"
                && rack().macros[2].targets == std::vector<alien::MacroTarget>{{filter, "cutoff", 0.1f, 0.9f}}, "MapRackMacro does what Preview said");
        require(commands.execute(cmd::MapRackMacro{RackStage::track, 0, 2, drive, "drive", 1, 0}).changed && rack().macros[2].targets.size() == 2,
                "a macro moves several parameters, of several devices");
        require(commands.execute(cmd::MapRackMacro{RackStage::track, 0, 2, filter, "cutoff", 0.9, 0.1}).changed && rack().macros[2].targets.size() == 2
                && rack().macros[2].targets[0].from == 0.9f, "mapping a parameter again changes its range in place");
        require(commands.execute(cmd::SetRackMacro{RackStage::track, 0, 2, 0.5}).changed && rack().macros[2].value == 0.5f && c.undoLabel() == "Move macro",
                "SetRackMacro moves the knob");
        require(commands.execute(cmd::RenameRackMacro{RackStage::track, 0, 2, "  Brightness "}).changed && rack().macros[2].name == "Brightness"
                && c.undoLabel() == "Rename macro", "RenameRackMacro names it, trimmed");
        require(cmd::describe(c.document(), cmd::SetRackMacro{RackStage::track, 0, 2, 0.25}).contains("Brightness")
                && cmd::describe(c.document(), cmd::MapRackMacro{RackStage::track, 0, 2, drive, "drive", 0, 1}).contains("Drive"), "the commands describe the macro and device");
        require(alien::toProjectData(c.document()).valid(), "the project stays valid");
        require(commands.execute(cmd::UnmapRackMacro{RackStage::track, 0, 2, drive, "drive"}).changed && rack().macros[2].targets.size() == 1, "UnmapRackMacro lets a parameter go");
        require(commands.execute(cmd::MapRackMacro{RackStage::master, 0, 0, glue, "ratio", 0, 1}).changed && c.document().masterPlugins.macros[0].targets.size() == 1,
                "the master's rack has its own macros");
        for (int undo = 0; undo < 7; ++undo) require(c.undo(), "undo each macro step");
        require(alien::sameContent(before, c.document()), "every step undoes on its own, back to no macros");
        for (int redo = 0; redo < 7; ++redo) require(c.redo(), "redo each macro step");
    }

    // A drag is one Undo step.
    {
        const auto history = c.undoLabel();
        for (const double v : {0.6, 0.7, 0.8}) require(commands.execute(cmd::SetRackMacro{RackStage::track, 0, 2, v}, "macro-drag-test", true).ok, "drag the macro");
        require(rack().macros[2].value == 0.8f && c.undo() && rack().macros[2].value == 0.5f && c.redo(), "a drag of the knob undoes as one step");
    }

    // Saved and opened again.
    {
        const auto saved = alien::rackToVar(rack());
        alien::PluginRackState opened;
        require(alien::rackFromVar(saved, opened) && opened == rack(), "a rack's macros are saved");
        require(!alien::rackToVar(alien::PluginRackState{}).hasProperty("macros"), "an untouched rack writes no macros");
        auto broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("macros", {}).getArray()->getReference(2).getDynamicObject()->setProperty("value", 2.0);
        alien::PluginRackState rejected;
        require(!alien::rackFromVar(broken, rejected), "a macro value past 1 is refused when opened");
        broken = juce::JSON::parse(juce::JSON::toString(saved));
        broken.getProperty("macros", {}).getArray()->remove(7);
        require(!alien::rackFromVar(broken, rejected), "a rack saves all eight macros or none");
        const auto json = alien::serializeSessionProject(alien::toProjectData(c.document()));
        alien::SessionProjectData parsed; juce::String error;
        require(alien::parseSessionProject(json, parsed, error) && parsed.plugins[0].macros == rack().macros
                && parsed.masterPlugins.macros[0].targets.size() == 1, "the macros reopen with the project");
    }

    // Removing a device takes its targets with it; replacing an instrument does the same.
    {
        require(commands.execute(cmd::MapRackMacro{RackStage::track, 0, 4, drive, "drive", 0, 1}).changed, "map the second device");
        require(commands.execute(cmd::RemoveDevice{RackStage::track, 0, 1}).changed && rack().macros[4].targets.empty() && rack().macros[2].targets.size() == 1,
                "removing a device drops its macro targets, and only its");
        require(c.undo() && rack().macros[4].targets.size() == 1, "Undo gives them back with the device");
    }

    // Refusals change nothing and say why.
    {
        const auto now = c.document();
        const auto says = [&](const cmd::Command& command, const char* why) {
            const auto result = commands.execute(command);
            require(!result.ok && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetRackMacro{RackStage::track, 0, 8, 0.5}, "numbered 0 to 7");
        says(cmd::SetRackMacro{RackStage::track, 0, -1, 0.5}, "numbered 0 to 7");
        says(cmd::SetRackMacro{RackStage::track, 0, 0, 1.5}, "0 to 1");
        says(cmd::SetRackMacro{RackStage::track, 0, 0, std::numeric_limits<double>::quiet_NaN()}, "0 to 1");
        says(cmd::SetRackMacro{RackStage::track, 99, 0, 0.5}, "no plugin rack");
        says(cmd::MapRackMacro{RackStage::track, 0, 0, glue, "ratio", 0, 1}, "not in this rack");
        says(cmd::MapRackMacro{RackStage::track, 0, 0, filter, "cutoff", 0, 1}, "already follows Brightness");
        says(cmd::MapRackMacro{RackStage::track, 0, 0, filter, "", 0, 1}, "parameter ID");
        says(cmd::MapRackMacro{RackStage::track, 0, 0, filter, "q", 0, 2}, "range from 0 to 1");
        says(cmd::UnmapRackMacro{RackStage::track, 0, 0, filter, "q"}, "does not move");
        says(cmd::RenameRackMacro{RackStage::track, 0, 0, juce::String::repeatedString("x", 40)}, "at most 32");
        for (int k = 0; k < alien::maxMacroTargets; ++k)
            require(commands.execute(cmd::MapRackMacro{RackStage::track, 0, 6, filter, "p" + juce::String(k), 0, 1}).changed, "fill a macro");
        says(cmd::MapRackMacro{RackStage::track, 0, 6, filter, "one more", 0, 1}, "at most 16");
        for (int k = 0; k < alien::maxMacroTargets; ++k) require(c.undo(), "unfill it");
        require(alien::sameContent(now, c.document()), "refusals change nothing");
    }

    // Script forms.
    for (const cmd::Command command : {cmd::Command{cmd::SetRackMacro{RackStage::bus, 1, 3, 0.25}}, cmd::Command{cmd::RenameRackMacro{RackStage::returnTrack, 2, 1, "Space"}},
                                       cmd::Command{cmd::MapRackMacro{RackStage::master, 0, 7, 0x0123456789abcdefull, "gain", 0.75, 0.25}},
                                       cmd::Command{cmd::UnmapRackMacro{RackStage::track, 3, 0, 42, "mix"}}}) {
        juce::String error;
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed
                && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)), "a macro command round-trips through JSON");
    }
    juce::String error;
    require(!cmd::parse("MapRackMacro", juce::JSON::parse(R"({"stage": "track", "rack": 0, "macro": 0, "instance": "12", "parameter": "x", "from": 0, "to": 1})"), error)
            && error.contains("16 hex"), "an instance is 16 hex digits");
    std::puts("PASS: rack macro commands: map, move, rename, unmap, Undo, saving and refusals");
}
