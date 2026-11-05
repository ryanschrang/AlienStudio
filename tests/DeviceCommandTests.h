#pragma once
// The devices in a plugin rack, as named commands: insert, remove, move and bypass.
static alien::PluginState deviceEffect(const juce::String& name = "Test FX") { return alien::PluginState("<PLUGIN name=\"" + name + "\"/>", {}); }
static alien::PluginState deviceInstrument() { return alien::PluginState("<PLUGIN name=\"Test synth\" isInstrument=\"1\"/>", {}); }

static void deviceCommandChecks() {
    using alien::RackStage;
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto names = [](const alien::PluginRackState& rack) {
        juce::StringArray list; for (const auto& effect : rack.effects) list.add(alien::pluginDisplayName(effect));
        return list.joinIntoString(",");
    };
    const auto trackRack = [&]() -> const alien::PluginRackState& { return c.document().tracks[0].plugins; };
    c.markAsNewBaseline();
    const auto empty = c.document();

    // Insert: Preview and execute agree because the command carries the device's identity.
    const cmd::Command insert = cmd::InsertDevice{RackStage::track, 0, 0, deviceEffect("A")};
    const auto preview = commands.preview(insert);
    require(preview.ok && preview.changed && names(preview.after.tracks[0].plugins) == "A" && preview.changes[0].contains("Insert A"), "Preview predicts the insert");
    require(alien::sameContent(empty, c.document()) && !c.canUndo(), "Preview leaves the project and history alone");
    require(commands.execute(insert).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Insert device", "execute matches Preview");
    require(alien::toProjectData(c.document()).valid(), "the project stays valid");
    require(c.undo() && alien::sameContent(empty, c.document()), "one Undo removes it");
    require(c.redo() && names(trackRack()) == "A", "Redo restores it");
    require(!commands.execute(insert).ok && names(trackRack()) == "A", "the same device cannot go in twice");

    require(commands.execute(cmd::InsertDevice{RackStage::track, 0, 1, deviceEffect("C")}).changed && commands.execute(cmd::InsertDevice{RackStage::track, 0, 1, deviceEffect("B")}).changed, "fixtures");
    require(names(trackRack()) == "A,B,C", "an insert lands at the slot asked for");
    const auto idA = trackRack().effects[0].instanceId;

    // Move ends with the device in the slot asked for, identity intact.
    require(commands.execute(cmd::MoveDevice{RackStage::track, 0, 0, 2}).changed && names(trackRack()) == "B,C,A" && trackRack().effects[2].instanceId == idA && c.undoLabel() == "Move device", "a move lands at its slot");
    require(c.undo() && names(trackRack()) == "A,B,C", "one Undo puts it back");
    require(c.redo() && c.undo(), "redo fixture");
    const auto sameMove = commands.execute(cmd::MoveDevice{RackStage::track, 0, 1, 1});
    require(sameMove.ok && !sameMove.changed && c.canRedo(), "a move to its own slot is a no-op that keeps Redo");

    // Bypass sets the state; the same state again is a no-op.
    require(commands.execute(cmd::SetDeviceBypass{RackStage::track, 0, 1, true}).changed && trackRack().effects[1].bypass && c.undoLabel() == "Set device bypass", "bypass sets the state");
    const auto sameBypass = commands.execute(cmd::SetDeviceBypass{RackStage::track, 0, 1, true});
    require(sameBypass.ok && !sameBypass.changed, "the same state is a no-op");
    require(c.undo() && !trackRack().effects[1].bypass, "one Undo enables it");

    // A key on a device stays with it through a move.
    require(commands.execute(cmd::SetSidechain{RackStage::track, 0, 0, alien::KeySource::track, 2}).changed, "key fixture");
    require(commands.execute(cmd::MoveDevice{RackStage::track, 0, 0, 1}).changed && trackRack().effects[1].sidechain == 2 && trackRack().effects[0].sidechain == -1, "a key moves with its effect");
    require(c.undo() && c.undo(), "back to no key");

    // Remove.
    const auto beforeRemove = c.document();
    require(commands.execute(cmd::RemoveDevice{RackStage::track, 0, 1}).changed && names(trackRack()) == "A,C" && c.undoLabel() == "Remove device", "remove takes the slot out");
    require(c.undo() && alien::sameContent(beforeRemove, c.document()), "one Undo restores it");

    // The instrument slot: only a track has one, it takes instruments, and a new one replaces the old.
    const auto synth = deviceInstrument();
    require(commands.execute(cmd::InsertDevice{RackStage::track, 0, -1, synth}).changed && trackRack().instrument.instanceId == synth.instanceId, "an instrument goes in its slot");
    const auto second = deviceInstrument();
    require(commands.execute(cmd::InsertDevice{RackStage::track, 0, -1, second}).changed && trackRack().instrument.instanceId == second.instanceId, "and replaces the one there");
    require(c.undo() && trackRack().instrument.instanceId == synth.instanceId, "Undo brings the old one back");
    require(commands.execute(cmd::SetDeviceBypass{RackStage::track, 0, -1, true}).changed && trackRack().instrument.bypass, "an instrument can be bypassed");
    require(commands.execute(cmd::RemoveDevice{RackStage::track, 0, -1}).changed && trackRack().instrument.descriptionXml.isEmpty() && names(trackRack()) == "A,B,C", "and removed, leaving the FX");
    require(alien::toProjectData(c.document()).valid(), "an empty instrument slot saves");
    require(c.undo() && c.undo(), "instrument back, enabled");

    // Other racks.
    const int returnSlot = alien::return_edit::firstFreeSlot(c.document());
    require(commands.execute(cmd::CreateBus{}).changed && commands.execute(cmd::CreateReturn{}).changed, "slot fixtures");
    require(commands.execute(cmd::InsertDevice{RackStage::master, 0, 0, deviceEffect("M")}).changed && names(c.document().masterPlugins) == "M", "the master takes effects");
    require(commands.execute(cmd::InsertDevice{RackStage::bus, 0, 0, deviceEffect("B0")}).changed && names(c.document().buses[0].plugins) == "B0", "a bus takes effects");
    require(commands.execute(cmd::InsertDevice{RackStage::returnTrack, returnSlot, 0, deviceEffect("R")}).changed && names(c.document().returns[static_cast<std::size_t>(returnSlot)].plugins) == "R", "a return takes effects");
    require(alien::toProjectData(c.document()).valid(), "all of those save");

    const auto steady = c.document();
    for (const auto& bad : std::vector<cmd::Command>{
             cmd::InsertDevice{RackStage::master, 0, -1, deviceInstrument()},                 // no instrument slot
             cmd::InsertDevice{RackStage::bus, 0, -1, deviceInstrument()},
             cmd::InsertDevice{RackStage::track, 1, -1, deviceEffect()},                      // an effect in the instrument slot
             cmd::InsertDevice{RackStage::track, 1, 0, deviceInstrument()},                   // an instrument in an FX slot
             cmd::InsertDevice{RackStage::track, 0, 4, deviceEffect()},                       // past the end
             cmd::InsertDevice{RackStage::track, 0, -2, deviceEffect()},
             cmd::InsertDevice{RackStage::track, 99, 0, deviceEffect()},                      // no such rack
             cmd::InsertDevice{RackStage::master, 1, 0, deviceEffect()},
             cmd::InsertDevice{RackStage::bus, 5, 0, deviceEffect()},
             cmd::InsertDevice{RackStage::returnTrack, alien::maxReturns, 0, deviceEffect()},
             cmd::InsertDevice{RackStage::track, 0, 0, alien::PluginState{}},                 // nothing to insert
             cmd::InsertDevice{RackStage::track, 0, 0, alien::PluginState("not xml", {})},
             cmd::InsertDevice{RackStage::track, 0, 0, alien::PluginState("<OTHER/>", {})},
             cmd::RemoveDevice{RackStage::track, 0, 3},                                       // past the rack
             cmd::RemoveDevice{RackStage::track, 1, -1},                                      // no instrument
             cmd::RemoveDevice{RackStage::master, 0, -1},
             cmd::MoveDevice{RackStage::track, 0, -1, 0},                                     // an instrument does not move
             cmd::MoveDevice{RackStage::track, 0, 0, 3},
             cmd::MoveDevice{RackStage::track, 0, 0, -1},
             cmd::MoveDevice{RackStage::track, 0, 3, 0},
             cmd::SetDeviceBypass{RackStage::track, 0, 3, true},
             cmd::SetDeviceBypass{RackStage::track, 1, -1, true}})
        require(!commands.execute(bad).ok && alien::sameContent(steady, c.document()), "an impossible device edit refuses");

    // Eight FX is the limit.
    for (int i = 3; i < alien::device_edit::maxEffects; ++i) require(commands.execute(cmd::InsertDevice{RackStage::track, 0, i, deviceEffect()}).changed, "filling the rack");
    const auto full = commands.execute(cmd::InsertDevice{RackStage::track, 0, 8, deviceEffect()});
    require(!full.ok && full.error.contains("eight"), "a ninth effect refuses");
    require(alien::toProjectData(c.document()).valid(), "a full rack saves");
    for (int i = 3; i < alien::device_edit::maxEffects; ++i) require(c.undo(), "emptying the rack");

    // A batch sees earlier commands and is one Undo step.
    const auto beforeBatch = c.document();
    const auto batch = commands.batch({cmd::InsertDevice{RackStage::track, 1, 0, deviceEffect("X")}, cmd::InsertDevice{RackStage::track, 1, 1, deviceEffect("Y")}, cmd::MoveDevice{RackStage::track, 1, 1, 0}, cmd::SetDeviceBypass{RackStage::track, 1, 0, true}}, "Build chain");
    require(batch.changed && names(c.document().tracks[1].plugins) == "Y,X" && c.document().tracks[1].plugins.effects[0].bypass && c.undoLabel() == "Build chain", "a batch builds a chain");
    require(c.undo() && alien::sameContent(beforeBatch, c.document()), "one Undo reverts it");
    const auto refused = commands.batch({cmd::InsertDevice{RackStage::track, 1, 0, deviceEffect()}, cmd::RemoveDevice{RackStage::track, 1, 7}}, "Refused");
    require(!refused.ok && refused.failedCommand == 1 && alien::sameContent(beforeBatch, c.document()), "a refused batch changes nothing");
    const auto twice = deviceEffect();
    require(!commands.preview(std::vector<cmd::Command>{cmd::InsertDevice{RackStage::track, 1, 0, twice}, cmd::InsertDevice{RackStage::track, 1, 1, twice}}).ok, "one device cannot go in twice in a batch");

    // What the Plugins panel's buttons ask for.
    const auto& d = c.document();
    auto panel = [&](int op, int slot) { return cmd::pluginPanelCommand(d, RackStage::track, 0, op, slot); };
    const auto bypass = panel(1, 1); require(bypass && std::get<cmd::SetDeviceBypass>(*bypass).bypassed, "the bypass button enables a bypass");
    require(commands.execute(*bypass).changed && std::get<cmd::SetDeviceBypass>(*panel(1, 1)).bypassed == false, "and toggles back");
    require(c.undo(), "bypass undone");
    require(std::get<cmd::RemoveDevice>(*panel(2, 0)).slot == 0, "the remove button asks to remove that slot");
    require(std::get<cmd::MoveDevice>(*panel(3, 1)).to == 0 && std::get<cmd::MoveDevice>(*panel(4, 1)).to == 2, "the move buttons swap with a neighbour");
    require(!panel(3, 0) && !panel(4, 2) && !panel(3, -1) && !panel(4, -1) && !panel(0, 0), "a move with nowhere to go, or Open editor, is not a command");
    require(!commands.execute(*panel(1, 9)).ok && !commands.execute(*panel(2, 9)).ok, "an empty slot gets a refusal to show");

    // A frozen track's rack, recording and busy all refuse.
    c.markAsNewBaseline();
    {
        auto frozen = c.document(); frozen.tracks[1].settings.freezeScene = 0;
        const auto why = alien::device_edit::refusal(frozen, {alien::device_edit::Kind::insert, RackStage::track, 1, 0, -1, false, deviceEffect()});
        require(why.contains("Unfreeze"), "a frozen track refuses");
    }
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        for (const auto& edit : std::vector<cmd::Command>{cmd::InsertDevice{RackStage::track, 1, 0, deviceEffect()}, cmd::RemoveDevice{RackStage::track, 0, 0}, cmd::MoveDevice{RackStage::track, 0, 0, 1}, cmd::SetDeviceBypass{RackStage::track, 0, 0, true}}) {
            const auto during = commands.execute(edit);
            require(!during.ok && during.error.contains("Stop recording"), "device edits refuse during recording and count-in");
        }
        engine->recording.phase.store(alien::RecordControls::idle);
    }

    // Strict arguments, and a round trip through the script form.
    juce::String error;
    const auto moved = cmd::parse("MoveDevice", cmd::arguments(cmd::MoveDevice{RackStage::bus, 2, 3, 1}), error);
    require(moved && cmd::name(*moved) == "MoveDevice" && std::get<cmd::MoveDevice>(*moved).stage == RackStage::bus && std::get<cmd::MoveDevice>(*moved).rack == 2 && std::get<cmd::MoveDevice>(*moved).slot == 3 && std::get<cmd::MoveDevice>(*moved).to == 1, "MoveDevice round-trips");
    const auto bypassed = cmd::parse("SetDeviceBypass", cmd::arguments(cmd::SetDeviceBypass{RackStage::returnTrack, 1, -1, true}), error);
    require(bypassed && std::get<cmd::SetDeviceBypass>(*bypassed).bypassed && std::get<cmd::SetDeviceBypass>(*bypassed).slot == -1 && std::get<cmd::SetDeviceBypass>(*bypassed).stage == RackStage::returnTrack, "SetDeviceBypass round-trips");
    const auto removed = cmd::parse("RemoveDevice", cmd::arguments(cmd::RemoveDevice{RackStage::master, 0, 4}), error);
    require(removed && std::get<cmd::RemoveDevice>(*removed).slot == 4 && std::get<cmd::RemoveDevice>(*removed).stage == RackStage::master, "RemoveDevice round-trips");
    const auto original = deviceEffect("Z");
    const auto inserted = cmd::parse("InsertDevice", cmd::arguments(cmd::InsertDevice{RackStage::track, 1, 0, original}), error);
    require(inserted && std::get<cmd::InsertDevice>(*inserted).device.descriptionXml == original.descriptionXml && std::get<cmd::InsertDevice>(*inserted).device.instanceId != 0 && std::get<cmd::InsertDevice>(*inserted).device.instanceId != original.instanceId, "InsertDevice round-trips with a fresh identity");
    for (const auto& [commandName, text] : std::vector<std::pair<const char*, const char*>>{
             {"RemoveDevice", R"({"stage":"nowhere","rack":0,"slot":0})"}, {"RemoveDevice", R"({"stage":"track","rack":0})"}, {"RemoveDevice", R"({"stage":"track","rack":0,"slot":0,"extra":1})"},
             {"MoveDevice", R"({"stage":"track","rack":0,"slot":0})"}, {"SetDeviceBypass", R"({"stage":"track","rack":0,"slot":0,"bypassed":1})"}, {"InsertDevice", R"({"stage":"track","rack":0,"slot":0})"}})
        require(!cmd::parse(commandName, juce::JSON::parse(text), error), "malformed device arguments refuse");
    std::cout << "PASS: device commands\n";
}
