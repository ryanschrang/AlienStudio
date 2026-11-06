#pragma once
// SetTempo, the Song's tempo changes and its time signature changes. Frozen tracks are in
// MixerRoutingTests and warped audio in WarpMarkerTests, which have those fixtures.
static void tempoCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto bar = alien::timeline::ticksPerBar;
    const int startBpm = c.document().bpm;

    // The project tempo.
    const auto before = c.document();
    const auto history = historyOf(c);
    const auto preview = commands.preview(cmd::SetTempo{133});
    require(preview.ok && preview.changed && preview.after.bpm == 133 && alien::sameContent(before, c.document())
        && engine->bpm.load() == startBpm && historyOf(c) == history, "SetTempo Preview changes no live state");
    require(commands.execute(cmd::SetTempo{133}).changed && alien::sameContent(preview.after, c.document()) && engine->bpm.load() == 133
        && c.undoLabel() == "Change tempo", "SetTempo does what Preview said and reaches the engine");
    require(c.undo() && c.document().bpm == startBpm && engine->bpm.load() == startBpm, "SetTempo undoes");
    require(c.redo() && c.document().bpm == 133 && engine->bpm.load() == 133 && c.undo(), "SetTempo redoes");
    const auto redo = historyOf(c);
    require(commands.execute(cmd::SetTempo{startBpm}).ok && historyOf(c) == redo, "an unchanged tempo preserves redo");
    for (const int bpm : {59, 201, -1, 0})
        require(!commands.execute(cmd::SetTempo{bpm}).ok && c.document().bpm == startBpm && engine->bpm.load() == startBpm && historyOf(c) == redo,
            "a tempo outside the range is refused, not clamped");

    // The transport's control: wheel steps are one Undo step, and a drag changes nothing until its release.
    c.markAsNewBaseline();
    for (const int bpm : {121, 122, 123}) require(cmd::setProjectTempo(c, bpm).changed, "a wheel step sets the tempo");
    require(c.document().bpm == 123 && c.undo() && !c.canUndo() && c.document().bpm == startBpm, "wheel steps are one Undo step");
    c.markAsNewBaseline();
    const auto dragHistory = historyOf(c);
    c.auditionTempo(150);
    require(engine->bpm.load() == 150 && c.document().bpm == startBpm && historyOf(c) == dragHistory, "a tempo drag is heard and is not an edit yet");
    c.auditionTempo(999);
    require(engine->bpm.load() == 200, "a drag stays in range");
    require(cmd::setProjectTempo(c, 150).changed && c.document().bpm == 150 && engine->bpm.load() == 150 && c.undo() && !c.canUndo() && c.document().bpm == startBpm,
        "its release is one Undo step");
    c.markAsNewBaseline();
    c.auditionTempo(150);
    const auto returned = historyOf(c);
    require(cmd::setProjectTempo(c, startBpm).ok && historyOf(c) == returned && engine->bpm.load() == startBpm, "a drag returning to its start leaves the engine on the project's tempo");
    c.auditionTempo(150);
    c.setExternalBusy(true);
    require(!cmd::setProjectTempo(c, 150).ok && c.document().bpm == startBpm && engine->bpm.load() == startBpm, "a refused release also leaves the engine on the project's tempo");
    c.auditionTempo(170);
    require(engine->bpm.load() == startBpm, "a busy project ignores a drag");
    c.setExternalBusy(false);

    // Tempo changes on the Song.
    c.markAsNewBaseline();
    const auto songBefore = c.document();
    const auto map = commands.preview(cmd::SetTempoChange{2 * bar, 90.0});
    require(map.ok && map.changed && map.after.timeline.tempoChanges.size() == 1 && c.timeline().tempoChanges.empty(), "SetTempoChange Preview changes no live state");
    require(commands.execute(cmd::SetTempoChange{2 * bar, 90.0}).changed && alien::sameContent(map.after, c.document()) && c.undoLabel() == "Tempo change", "SetTempoChange does what Preview said");
    const auto once = historyOf(c);
    require(commands.execute(cmd::SetTempoChange{2 * bar, 90.0}).ok && historyOf(c) == once, "the same tempo change adds no undo step");
    require(commands.execute(cmd::SetTempoChange{2 * bar, 95.5}).changed && c.timeline().tempoChanges[0].bpm == 95.5, "a change there is replaced");
    require(commands.execute(cmd::MoveTempoChange{2 * bar, bar}).changed && c.undoLabel() == "Move tempo change" && c.timeline().tempoChanges[0].tick == bar, "a change moves");
    const auto moved = historyOf(c);
    require(commands.execute(cmd::MoveTempoChange{bar, bar}).ok && historyOf(c) == moved, "moving a change to where it is adds no undo step");
    require(commands.execute(cmd::RemoveTempoChange{2 * bar}).ok && historyOf(c) == moved, "removing an absent change adds no undo step");
    require(commands.execute(cmd::RemoveTempoChange{bar}).changed && c.timeline().tempoChanges.empty() && c.undoLabel() == "Remove tempo change", "a change is removed");
    require(commands.batch({cmd::SetTempoChange{bar, 70.0}, cmd::SetTempoChange{2 * bar, 80.0}}, "Two changes").changed && c.timeline().tempoChanges.size() == 2, "a batch of changes");
    require(commands.execute(cmd::ClearTempoChanges{}).changed && c.timeline().tempoChanges.empty() && c.undoLabel() == "Remove tempo changes", "changes are cleared");
    require(c.undo() && c.timeline().tempoChanges.size() == 2, "clearing is one undo step");
    require(c.undo() && c.timeline().tempoChanges.empty() && c.undo(), "the batch is one undo step");
    const auto refusedHistory = historyOf(c);
    const auto refusedSong = c.timeline();
    const auto songEnd = c.timeline().length();
    for (const cmd::Command bad : std::vector<cmd::Command>{cmd::SetTempoChange{0, 90.0}, cmd::SetTempoChange{songEnd + 1, 90.0}, cmd::SetTempoChange{bar, 59.0}, cmd::SetTempoChange{bar, 201.0},
                                                            cmd::SetTempoChange{bar, std::numeric_limits<double>::quiet_NaN()}, cmd::MoveTempoChange{77, bar}, cmd::SetMeterChange{-1, 3, 4},
                                                            cmd::SetMeterChange{c.timeline().lengthBars, 3, 4}, cmd::SetMeterChange{0, 3, 5}, cmd::SetMeterChange{0, 0, 4}})
        require(!commands.execute(bad).ok && c.timeline() == refusedSong && historyOf(c) == refusedHistory, "a bad tempo or time signature edit changes nothing");
    require(!commands.batch({cmd::SetTempo{90}, cmd::SetTempoChange{bar, 70.0}, cmd::RenameTrack{99, "Never"}}, "Bad").ok
        && c.document().bpm == songBefore.bpm && engine->bpm.load() == songBefore.bpm && c.timeline() == refusedSong && historyOf(c) == refusedHistory,
        "a refused later command leaves no tempo behind");

    // Time signature changes.
    c.markAsNewBaseline();
    const auto meter = commands.preview(cmd::SetMeterChange{1, 3, 4});
    require(meter.ok && meter.changed && meter.after.timeline.meterChanges.size() == 1 && c.timeline().meterChanges.empty(), "SetMeterChange Preview changes no live state");
    require(commands.execute(cmd::SetMeterChange{1, 3, 4}).changed && alien::sameContent(meter.after, c.document()) && c.undoLabel() == "Time signature change", "SetMeterChange does what Preview said");
    const auto metered = historyOf(c);
    require(commands.execute(cmd::SetMeterChange{1, 3, 4}).ok && commands.execute(cmd::SetMeterChange{2, 3, 4}).ok && historyOf(c) == metered, "repeating the meter in force adds no undo step");
    require(commands.execute(cmd::RemoveMeterChange{5}).ok && historyOf(c) == metered, "removing an absent time signature change adds no undo step");
    require(commands.execute(cmd::RemoveMeterChange{1}).changed && c.timeline().meterChanges.empty() && c.undoLabel() == "Remove time signature change", "a time signature change is removed");
    require(commands.execute(cmd::SetMeterChange{0, 6, 8}).changed && commands.execute(cmd::ClearMeterChanges{}).changed && c.timeline().meterChanges.empty()
        && c.undoLabel() == "Remove time signature changes", "time signature changes are cleared");
    require(c.undo() && c.timeline().meterChanges.size() == 1, "clearing them is one undo step");
    require(commands.execute(cmd::ClearMeterChanges{}).ok && commands.execute(cmd::ClearMeterChanges{}).ok && commands.execute(cmd::ClearTempoChanges{}).ok && c.timeline().meterChanges.empty(), "clearing nothing is accepted");

    // Project state: busy refuses everything; while recording only the project tempo and the time signature stay editable.
    c.markAsNewBaseline();
    const auto guarded = historyOf(c);
    c.setExternalBusy(true);
    for (const cmd::Command edit : std::vector<cmd::Command>{cmd::SetTempo{90}, cmd::SetTempoChange{bar, 90.0}, cmd::SetMeterChange{0, 3, 4}})
        require(!commands.execute(edit).ok && historyOf(c) == guarded, "a busy project refuses");
    c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::recording);
    require(commands.execute(cmd::SetTempo{124}).changed && engine->bpm.load() == 124, "without warped audio the project tempo stays editable while recording, as before");
    engine->recording.phase.store(alien::RecordControls::idle);
    require(c.undo(), "recording fixture");
    juce::String error;
    for (const cmd::Command edit : std::vector<cmd::Command>{cmd::SetTempo{131}, cmd::SetTempoChange{2 * bar, 72.5}, cmd::MoveTempoChange{2 * bar, 3 * bar}, cmd::RemoveTempoChange{bar},
                                                             cmd::ClearTempoChanges{}, cmd::SetMeterChange{4, 6, 8}, cmd::RemoveMeterChange{4}, cmd::ClearMeterChanges{}}) {
        const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && cmd::name(*parsed) == cmd::name(edit) && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit))
            && cmd::describe(c.document(), edit).isNotEmpty(), "tempo commands round trip by name and arguments");
    }
    for (const auto& [commandName, json] : std::vector<std::pair<const char*, const char*>>{
             {"SetTempo", "{}"}, {"SetTempo", "{\"bpm\":120.5}"}, {"SetTempo", "{\"bpm\":\"120\"}"}, {"SetTempo", "{\"bpm\":120,\"tick\":1}"},
             {"SetTempoChange", "{\"tick\":1}"}, {"SetTempoChange", "{\"tick\":1.5,\"bpm\":90}"}, {"SetTempoChange", "{\"tick\":1,\"bpm\":true}"},
             {"MoveTempoChange", "{\"tick\":1}"}, {"RemoveTempoChange", "{}"}, {"ClearTempoChanges", "{\"tick\":1}"},
             {"SetMeterChange", "{\"bar\":1,\"numerator\":3}"}, {"RemoveMeterChange", "{}"}, {"ClearMeterChanges", "{\"bar\":1}"}})
        require(!cmd::parse(commandName, juce::JSON::parse(json), error), "tempo command parsing rejects missing, mistyped and extra arguments");
}
