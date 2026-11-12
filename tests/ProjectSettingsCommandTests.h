#pragma once
// SetProjectScale (the project's key and scale) and SetRecordSettings (the recording options). The piano roll's
// project-scale menu and the top bar's metronome and recording options run these commands.
static void projectSettingsCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& d = c.document();
    const auto before = c.document();

    // The key: D Dorian.
    int notified = 0;
    c.onMusicalScaleChanged = [&] { ++notified; };
    const cmd::SetProjectScale key{2, static_cast<int>(alien::ScaleType::dorian)};
    const auto preview = commands.preview(cmd::Command{key});
    require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && notified == 0, "SetProjectScale Preview changes no live state");
    const auto set = commands.execute(key);
    require(set.ok && set.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Set project key and scale", "SetProjectScale does what Preview said");
    require(d.musicalScale.defined && d.musicalScale.rootPitchClass == 2 && d.musicalScale.type == alien::ScaleType::dorian && notified == 1, "the project has the key, and the views are told once");
    const auto same = commands.execute(key);
    require(same.ok && !same.changed && notified == 1, "the same key again is a no-op");
    require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && d.musicalScale.rootPitchClass == 2 && c.undo(), "it is one Undo step");
    for (const cmd::Command bad : {cmd::Command{cmd::SetProjectScale{-1, 0}}, cmd::Command{cmd::SetProjectScale{12, 0}}, cmd::Command{cmd::SetProjectScale{0, -1}}, cmd::Command{cmd::SetProjectScale{0, 10}}}) {
        const auto refused = commands.execute(bad);
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && !commands.preview(bad).ok && alien::sameContent(before, c.document()), "a key that is out of range is refused");
    }

    // The recording options reach the engine, and repeated changes share one Undo step.
    auto options = d.recording;
    options.metronome = !options.metronome; options.countInBars = 2; options.replace = true; options.metronomeVolume = 0.25f; options.audioLatencyOffsetMs = -12; options.punch = true;
    const cmd::SetRecordSettings record{options};
    const auto recordPreview = commands.preview(cmd::Command{record});
    require(recordPreview.ok && recordPreview.changed && alien::sameContent(before, c.document()) && engine->recording.metronome.load() == before.recording.metronome, "SetRecordSettings Preview changes no live state");
    const auto recorded = commands.execute(record, "recordSettings");
    require(recorded.ok && recorded.changed && alien::sameContent(recordPreview.after, c.document()) && c.undoLabel() == "Change recording settings", "SetRecordSettings does what Preview said");
    require(d.recording == options && engine->recording.metronome.load() == options.metronome, "the project and the engine have the options");
    auto more = options; more.metronomeVolume = 0.75f;
    require(commands.execute(cmd::SetRecordSettings{more}, "recordSettings").changed && d.recording == more, "a second change applies");
    require(c.undo() && alien::sameContent(before, c.document()) && engine->recording.metronome.load() == before.recording.metronome && !c.canUndo(), "both changes are one Undo step, and Undo reaches the engine");
    require(c.redo() && d.recording == more && c.undo(), "Redo brings them back");
    auto loud = options; loud.metronomeVolume = 1.5f;
    auto longCount = options; longCount.countInBars = alien::RecordSettings::maxCountInBars + 1;
    auto late = options; late.latencyOffsetMs = alien::RecordSettings::maxLatencyOffsetMs + 1;
    for (const auto& bad : {loud, longCount, late}) {
        const auto refused = commands.execute(cmd::SetRecordSettings{bad});
        require(!refused.ok && !refused.changed && refused.error.isNotEmpty() && alien::sameContent(before, c.document()), "recording options that are out of range are refused");
    }

    c.setExternalBusy(true);
    const auto busyKey = commands.execute(key);
    const auto busyRecord = commands.execute(record);
    c.setExternalBusy(false);
    require(!busyKey.ok && busyKey.error.contains("busy") && !busyRecord.ok && alien::sameContent(before, c.document()) && notified == 1, "a busy project refuses, and refusals change nothing");

    juce::String error;
    for (const cmd::Command command : {cmd::Command{key}, cmd::Command{record}}) {
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)) && parsed->index() == command.index(), "the project settings commands round-trip through JSON");
    }
    const auto parsedRecord = cmd::parse("SetRecordSettings", juce::JSON::parse(juce::JSON::toString(cmd::arguments(cmd::Command{record}))), error);
    require(parsedRecord && std::get<cmd::SetRecordSettings>(*parsedRecord).settings == options, "and the parsed recording options are the same options");
    require(cmd::describe(c.document(), key).contains("D Dorian") && cmd::describe(c.document(), record).contains("2 bars") && cmd::describe(c.document(), record).contains("replacing"),
            "the descriptions name the key and the options");
}
