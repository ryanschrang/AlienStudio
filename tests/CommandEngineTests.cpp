#include "CommandEngine.h"
#include <cstdlib>
#include <iostream>

// Command Engine: named track commands, execute/undo/redo, batches as one
// undo step, preview without side effects and argument checking. No audio
// device or window is needed.

namespace cmd = alien::command;

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

static alien::PianoRollPattern patternWith(int pitch) {
    alien::PianoRollPattern p;
    p.add(pitch, 0.0, 1.0);
    return p;
}

static std::vector<std::uint32_t> trackIds(const alien::ProjectDocument& d) {
    std::vector<std::uint32_t> ids;
    for (const auto& t : d.tracks) ids.push_back(t.id);
    return ids;
}

// What the undo history looks like from outside the controller.
struct HistoryView {
    bool canUndo{}, canRedo{}, dirty{};
    juce::String undoLabel, redoLabel;
    bool operator==(const HistoryView& o) const {
        return canUndo == o.canUndo && canRedo == o.canRedo && dirty == o.dirty && undoLabel == o.undoLabel && redoLabel == o.redoLabel;
    }
};
static HistoryView historyOf(const alien::ProjectController& c) {
    return {c.canUndo(), c.canRedo(), c.isDirty(), c.undoLabel(), c.redoLabel()};
}

static void nameTests() {
    require(cmd::name(cmd::CreateTrack{}) == "CreateTrack" && cmd::name(cmd::DeleteTrack{}) == "DeleteTrack"
            && cmd::name(cmd::MoveTrack{}) == "MoveTrack" && cmd::name(cmd::RenameTrack{}) == "RenameTrack"
            && cmd::name(cmd::ColorTrack{}) == "ColorTrack", "commands have stable names");
    require(cmd::label(cmd::DeleteTrack{}) == "Delete track" && cmd::label(cmd::RenameTrack{}) == "Rename track",
            "commands use the same undo labels as the Session view");
}

static void singleCommandTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    const auto& d = c.document();
    c.pushDocumentToEngine();

    // CreateTrack.
    auto r = commands.execute(cmd::CreateTrack{});
    require(r.ok && r.changed && d.trackCount() == 5 && d.tracks[4].name == "Track 5", "CreateTrack appends the next track");
    require(engine.activeTracks.load() == 5, "the engine follows a created track");
    require(c.undoLabel() == "Add track" && c.isDirty(), "CreateTrack is one named undo step");
    require(commands.undo() && d.trackCount() == 4 && engine.activeTracks.load() == 4, "undo CreateTrack");
    require(commands.redo() && d.trackCount() == 5 && d.tracks[4].name == "Track 5", "redo CreateTrack");

    // RenameTrack trims like the controller does.
    r = commands.execute(cmd::RenameTrack{1, "  Bass  "});
    require(r.ok && d.tracks[1].name == "Bass" && c.undoLabel() == "Rename track", "RenameTrack");
    require(commands.undo() && d.tracks[1].name == "Track 2", "undo RenameTrack");
    require(commands.redo() && d.tracks[1].name == "Bass", "redo RenameTrack");

    // ColorTrack.
    r = commands.execute(cmd::ColorTrack{1, 6});
    require(r.ok && d.tracks[1].colour == 6 && c.undoLabel() == "Colour track", "ColorTrack");
    require(commands.undo() && d.tracks[1].colour == 1, "undo ColorTrack");
    require(commands.redo() && d.tracks[1].colour == 6, "redo ColorTrack");

    // MoveTrack moves (not swaps) a track to its new index, as one undo step.
    c.setClipPattern(0, 0, patternWith(60), false);
    c.commit("Edit clip");
    const auto ids = trackIds(d);
    r = commands.execute(cmd::MoveTrack{0, 3});
    const std::vector<std::uint32_t> moved{ids[1], ids[2], ids[3], ids[0], ids[4]};
    require(r.ok && trackIds(d) == moved, "MoveTrack shifts the tracks in between");
    require(d.clip(3, 0).notes().count == 1 && d.clip(3, 0).notes().notes[0].pitch == 60, "clips move with their track");
    require(c.undoLabel() == "Move track", "a long move is one undo step");
    require(commands.undo() && trackIds(d) == ids && d.clip(0, 0).notes().notes[0].pitch == 60, "undo MoveTrack");
    require(c.undoLabel() == "Edit clip", "undo MoveTrack took exactly one step");
    require(commands.redo() && trackIds(d) == moved, "redo MoveTrack");
    r = commands.execute(cmd::MoveTrack{3, 0});
    require(r.ok && trackIds(d) == ids, "MoveTrack back to the start");
    const auto upward = trackIds(d);

    // DeleteTrack: later tracks shift and keep their content.
    r = commands.execute(cmd::DeleteTrack{0});
    require(r.ok && d.trackCount() == 4 && d.tracks[0].id == upward[1] && engine.activeTracks.load() == 4, "DeleteTrack");
    require(commands.undo() && trackIds(d) == upward && d.clip(0, 0).notes().notes[0].pitch == 60, "undo DeleteTrack restores the track and its clips");
    require(commands.redo() && d.trackCount() == 4 && d.tracks[0].id == upward[1], "redo DeleteTrack");

    // A command that changes nothing is accepted but records no undo step.
    const auto before = historyOf(c);
    r = commands.execute(cmd::RenameTrack{0, d.tracks[0].name});
    require(r.ok && !r.changed && historyOf(c) == before, "an unchanged rename adds no undo step");
    r = commands.execute(cmd::MoveTrack{2, 2});
    require(r.ok && !r.changed && historyOf(c) == before, "moving a track to where it is adds no undo step");
}

static void batchTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    const auto& d = c.document();
    c.pushDocumentToEngine();
    const auto start = d;

    const std::vector<cmd::Command> build{
        cmd::CreateTrack{}, cmd::RenameTrack{4, "Drums"}, cmd::ColorTrack{4, 3}, cmd::MoveTrack{4, 0}, cmd::DeleteTrack{2}};
    const auto r = commands.batch(build, "Set up drums");
    require(r.ok && r.changed && d.trackCount() == 4 && d.tracks[0].name == "Drums" && d.tracks[0].colour == 3, "batch applies every command in order");
    require(c.undoLabel() == "Set up drums", "a batch is one undo step with the batch's label");
    const auto built = d;
    require(commands.undo() && alien::sameContent(d, start) && !c.canUndo(), "one undo reverts the whole batch");
    require(commands.redo() && alien::sameContent(d, built) && !c.canRedo(), "one redo replays the whole batch");

    // A batch where a later command fails changes nothing, not even the redo stack.
    require(commands.undo() && c.canRedo(), "set up a redo step");
    const auto beforeDoc = d;
    const auto beforeHistory = historyOf(c);
    const int selectedTrack = d.selectedTrack, selectedScene = d.selectedScene;
    const std::vector<cmd::Command> broken{cmd::CreateTrack{}, cmd::RenameTrack{4, "Lead"}, cmd::DeleteTrack{9}, cmd::ColorTrack{0, 2}};
    const auto failed = commands.batch(broken, "Broken");
    require(!failed.ok && !failed.changed && failed.failedCommand == 2 && failed.error.contains("no track 10"), "the failing command is named");
    require(alien::sameContent(d, beforeDoc) && trackIds(d) == trackIds(beforeDoc), "a failed batch leaves the document untouched");
    require(d.selectedTrack == selectedTrack && d.selectedScene == selectedScene, "a failed batch leaves the selection untouched");
    require(historyOf(c) == beforeHistory && c.redoLabel() == "Set up drums", "a failed batch leaves the history untouched");
    require(engine.activeTracks.load() == d.trackCount(), "a failed batch leaves the engine untouched");

    // A command may depend on an earlier one in the same batch (track 5 only exists after CreateTrack).
    require(commands.batch({cmd::CreateTrack{}, cmd::RenameTrack{4, "Keys"}}, "Add keys").ok && d.tracks[4].name == "Keys",
            "later commands see the earlier commands' result");
    require(!commands.batch({}, "Nothing").ok, "an empty batch is refused");
}

static void previewTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    const auto& d = c.document();
    c.pushDocumentToEngine();
    require(commands.execute(cmd::RenameTrack{0, "Kick"}).ok, "set up an undo step");
    require(commands.undo() && c.canRedo(), "set up a redo step");

    const auto beforeDoc = d;
    const auto beforeHistory = historyOf(c);
    const auto beforeIds = trackIds(d);
    const std::vector<cmd::Command> plan{cmd::CreateTrack{}, cmd::MoveTrack{4, 1}, cmd::RenameTrack{1, "Pad"}, cmd::ColorTrack{1, 7}, cmd::DeleteTrack{0}};
    const auto p = commands.preview(plan);
    require(p.ok && p.changed && p.changes.size() == 5, "preview reports one change per command");
    require(p.changes[0] == "Add \"Track 5\" as track 5" && p.changes[4] == "Delete track 1 \"Track 1\"", "preview describes each change");
    require(p.after.trackCount() == 4 && p.after.tracks[0].name == "Pad" && p.after.tracks[0].colour == 7, "preview shows the resulting document");
    require(alien::sameContent(d, beforeDoc) && trackIds(d) == beforeIds, "preview does not change the document");
    require(historyOf(c) == beforeHistory, "preview does not change the history");
    require(engine.activeTracks.load() == 4, "preview does not touch the engine");

    const auto refused = commands.preview({cmd::CreateTrack{}, cmd::ColorTrack{0, 99}});
    require(!refused.ok && refused.failedCommand == 1 && refused.changes.isEmpty() && alien::sameContent(refused.after, d),
            "a refused preview reports the failing command");
    require(alien::sameContent(d, beforeDoc) && historyOf(c) == beforeHistory, "a refused preview changes nothing either");

    // Preview matches what execute does. Except for a created track's new id, the documents are identical.
    for (const auto& command : std::vector<cmd::Command>{cmd::MoveTrack{3, 0}, cmd::RenameTrack{2, " Vox "}, cmd::ColorTrack{1, 5}, cmd::DeleteTrack{1}}) {
        const auto predicted = commands.preview(command);
        require(predicted.ok && commands.execute(command).ok, "preview and execute both accept the command");
        require(alien::sameContent(predicted.after, d) && predicted.after.selectedTrack == d.selectedTrack, "preview predicts execute exactly");
    }
    const auto created = commands.preview(cmd::CreateTrack{});
    require(commands.execute(cmd::CreateTrack{}).ok && created.after.trackCount() == d.trackCount()
            && created.after.tracks.back().name == d.tracks.back().name && created.after.tracks.back().colour == d.tracks.back().colour,
            "preview predicts the created track");
    require(created.after.trackId(alien::TrackRole::session, created.after.trackCount() - 1)
            == d.trackId(alien::TrackRole::session, d.trackCount() - 1)
        && alien::toProjectData(created.after).valid(), "preview allocates the same persistent identity and a valid owner registry");
}

static void invalidArgumentTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    const auto& d = c.document();
    const auto beforeDoc = d;
    const auto beforeHistory = historyOf(c);
    for (const auto& command : std::vector<cmd::Command>{
             cmd::DeleteTrack{-1}, cmd::DeleteTrack{4}, cmd::MoveTrack{0, 4}, cmd::MoveTrack{-1, 0}, cmd::RenameTrack{7, "X"},
             cmd::RenameTrack{0, "   "}, cmd::ColorTrack{0, -1}, cmd::ColorTrack{0, alien::ProjectController::trackColourCount},
             cmd::ColorTrack{9, 0}}) {
        const auto r = commands.execute(command);
        require(!r.ok && r.error.isNotEmpty() && r.failedCommand == 0, "invalid arguments are refused with a reason");
    }
    require(alien::sameContent(d, beforeDoc) && historyOf(c) == beforeHistory, "refused commands change nothing");

    while (d.trackCount() > 1) require(commands.execute(cmd::DeleteTrack{0}).ok, "delete down to one track");
    require(!commands.execute(cmd::DeleteTrack{0}).ok && d.trackCount() == 1, "the last track cannot be deleted");
    while (d.canAddTrack()) require(commands.execute(cmd::CreateTrack{}).ok, "create up to the limit");
    require(!commands.execute(cmd::CreateTrack{}).ok && d.trackCount() == alien::maxSessionTracks, "the track limit is enforced");

    c.setExternalBusy(true);
    const auto busy = historyOf(c);
    require(!commands.execute(cmd::RenameTrack{0, "Busy"}).ok && d.tracks[0].name != "Busy" && historyOf(c) == busy,
            "commands are refused while the project is busy");
    c.setExternalBusy(false);
}

static void parseTests() {
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{
             cmd::CreateTrack{}, cmd::DeleteTrack{2}, cmd::MoveTrack{1, 3}, cmd::RenameTrack{0, "Bass"}, cmd::ColorTrack{3, 5}}) {
        const auto parsed = cmd::parse(cmd::name(command), cmd::arguments(command), error);
        require(parsed.has_value() && parsed->index() == command.index()
                && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
                "a command survives name + arguments round trip");
    }
    const auto rename = cmd::parse("RenameTrack", juce::JSON::parse(R"({"track": 1, "name": "Lead"})"), error);
    require(rename && std::get<cmd::RenameTrack>(*rename).track == 1 && std::get<cmd::RenameTrack>(*rename).name == "Lead", "parse from JSON");
    require(cmd::parse("CreateTrack", {}, error).has_value(), "CreateTrack takes no arguments");

    auto refuses = [&](const juce::String& commandName, const char* json, const char* expect) {
        error.clear();
        return !cmd::parse(commandName, juce::JSON::parse(json), error).has_value() && error.contains(expect);
    };
    require(refuses("SplitTrack", "{}", "Unknown command"), "unknown command names are refused");
    require(refuses("DeleteTrack", "{}", "\"track\""), "a missing argument is refused");
    require(refuses("DeleteTrack", R"({"track": "two"})", "whole-number"), "a mistyped number is refused");
    require(refuses("DeleteTrack", R"({"track": 1.5})", "whole-number"), "a fractional index is refused");
    require(refuses("DeleteTrack", R"({"track": 99999999999})", "whole-number"), "an out-of-range number is refused");
    require(refuses("RenameTrack", R"({"track": 1, "name": 5})", "text"), "a mistyped text argument is refused");
    require(refuses("MoveTrack", R"({"track": 1, "to": 2, "by": 1})", "no \"by\""), "unknown argument names are refused");
    require(refuses("ColorTrack", "[1, 2]", "object"), "arguments must be an object");
}

static void parameterCommandTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    const auto& d = c.document();
    c.pushDocumentToEngine();
    engine.tracks[0].mute.store(true);
    c.trackSettingsEdited(0); c.markAsNewBaseline();
    const auto before = d;
    const auto history = historyOf(c);
    const std::vector<cmd::Command> mix{cmd::SetTrackVolume{0, .5}, cmd::SetTrackPan{0, -.25}};
    const auto preview = commands.preview(mix);
    require(preview.ok && preview.changed && preview.changes.size() == 2
        && preview.after.tracks[0].settings.gain == .5f && preview.after.tracks[0].settings.pan == -.25f
        && preview.changes[0].contains("linear gain") && preview.changes[1].contains("Track 1"),
        "parameter preview describes and predicts both values");
    require(alien::sameContent(d, before) && historyOf(c) == history
        && engine.tracks[0].gain.load() == 1 && engine.tracks[0].pan.load() == 0,
        "parameter preview touches neither document, history nor live controls");
    require(commands.batch(mix, "Balance track").ok && alien::sameContent(d, preview.after)
        && engine.tracks[0].gain.load() == .5f && engine.tracks[0].pan.load() == -.25f
        && d.tracks[0].settings.mute && engine.tracks[0].mute.load(),
        "parameter execution matches preview, publishes controls and preserves unrelated settings");
    require(c.undoLabel() == "Balance track" && commands.undo() && !c.canUndo()
        && alien::sameContent(d, before) && engine.tracks[0].gain.load() == 1 && engine.tracks[0].pan.load() == 0,
        "one undo restores both document and live controls");
    require(commands.redo() && engine.tracks[0].gain.load() == .5f && engine.tracks[0].pan.load() == -.25f,
        "one redo restores both parameters");
    const auto mixed = d;
    const auto mixedHistory = historyOf(c);
    const auto failed = commands.batch({cmd::SetTrackVolume{0, .75}, cmd::SetTrackPan{0, 1.01}}, "Invalid mix");
    require(!failed.ok && failed.failedCommand == 1 && alien::sameContent(d, mixed)
        && historyOf(c) == mixedHistory && engine.tracks[0].gain.load() == .5f && engine.tracks[0].pan.load() == -.25f,
        "a later invalid parameter prevents every earlier write in the batch");
    for (const auto& value : std::vector<cmd::Command>{cmd::SetTrackVolume{0, .5}, cmd::SetTrackPan{0, -.25},
             cmd::SetTrackVolume{0, std::nextafter(.5, 1.0)}}) {
        const auto result = commands.execute(value);
        require(result.ok && !result.changed && historyOf(c) == mixedHistory,
            "unchanged values, including values equal at stored float precision, create no history");
    }
    require(commands.undo(), "prepare redo history");
    const auto redoHistory = historyOf(c);
    for (const auto& value : std::vector<cmd::Command>{cmd::SetTrackVolume{-1, 1}, cmd::SetTrackPan{99, 0},
             cmd::SetTrackVolume{0, -.001}, cmd::SetTrackVolume{0, 2.000000000001}, cmd::SetTrackPan{0, -1.000000000001},
             cmd::SetTrackVolume{0, std::numeric_limits<double>::infinity()},
             cmd::SetTrackPan{0, std::numeric_limits<double>::quiet_NaN()}}) {
        const auto result = commands.execute(value);
        require(!result.ok && result.error.isNotEmpty() && result.failedCommand == 0
            && historyOf(c) == redoHistory && alien::sameContent(d, before)
            && engine.tracks[0].gain.load() == 1 && engine.tracks[0].pan.load() == 0,
            "invalid targets/ranges/non-finite values preserve redo, document and engine");
    }
    c.setExternalBusy(true);
    require(!commands.execute(cmd::SetTrackVolume{0, .3}).ok && !c.setTrackVolume(0, .3)
        && !c.setTrackPan(0, .3) && historyOf(c) == redoHistory
        && engine.tracks[0].gain.load() == 1 && engine.tracks[0].pan.load() == 0, "busy project refuses parameter writes");
    c.setExternalBusy(false);
    require(!c.setTrackVolume(0, std::numeric_limits<double>::quiet_NaN()) && !c.setTrackPan(0, 2)
        && !c.setTrackVolume(-1, .5) && alien::sameContent(d, before)
        && engine.tracks[0].gain.load() == 1 && engine.tracks[0].pan.load() == 0, "controller setters also validate direct callers");
    require(commands.batch({cmd::SetTrackVolume{0, 0}, cmd::SetTrackPan{0, -1}}, "Left silence").ok
        && d.tracks[0].settings.gain == 0 && d.tracks[0].settings.pan == -1,
        "lower endpoints are valid");
    require(commands.execute(cmd::SetTrackVolume{0, 2}).ok && c.undoLabel() == "Set track volume"
        && commands.execute(cmd::SetTrackPan{0, 1}).ok && c.undoLabel() == "Set track pan",
        "upper endpoints are valid and individual commands have named undo steps");
    const auto structureBase = d;
    require(commands.batch({cmd::CreateTrack{}, cmd::SetTrackVolume{4, .25}, cmd::MoveTrack{4, 0},
             cmd::SetTrackPan{0, .5}, cmd::SetTrackVolume{0, .25}}, "Create mixed track").ok
        && d.tracks[0].settings.gain == .25f && d.tracks[0].settings.pan == .5f
        && engine.tracks[0].gain.load() == .25f && engine.tracks[0].pan.load() == .5f,
        "parameters follow sequential create/reorder operations, including an unchanged command");
    require(commands.undo() && alien::sameContent(d, structureBase), "mixed structural/parameter batch is one undo step");
}

static void parameterParseTests() {
    juce::String error;
    require(cmd::name(cmd::SetTrackVolume{}) == "SetTrackVolume" && cmd::name(cmd::SetTrackPan{}) == "SetTrackPan",
        "parameter commands have stable external names");
    for (const auto& command : std::vector<cmd::Command>{cmd::SetTrackVolume{2, .125}, cmd::SetTrackPan{1, -.75}}) {
        const auto parsed = cmd::parse(cmd::name(command), cmd::arguments(command), error);
        require(parsed && parsed->index() == command.index()
            && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
            "parameter commands round-trip by stable name and arguments");
    }
    for (const char* json : {R"({"track":0,"gain":0})", R"({"track":0,"gain":1.5})"})
        require(cmd::parse("SetTrackVolume", juce::JSON::parse(json), error).has_value(), "integer and decimal gains parse");
    require(cmd::parse("SetTrackPan", juce::JSON::parse(R"({"track":0,"pan":-0.25})"), error).has_value(),
        "signed pan values parse by their external name");
    for (const char* json : {R"({"track":0})", R"({"track":0,"gain":true})", R"({"track":0,"gain":"0.5"})",
                             R"({"track":0,"gain":null})", R"({"track":0,"gain":1,"extra":0})",
                             R"({"track":0.5,"gain":1})"})
        require(!cmd::parse("SetTrackVolume", juce::JSON::parse(json), error) && error.isNotEmpty(),
            "missing, mistyped, fractional-index and unknown arguments are refused");
    const auto valid = cmd::arguments(cmd::SetTrackPan{0, .5});
    for (const double value : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        auto args = valid.clone(); args.getDynamicObject()->setProperty("pan", value);
        require(!cmd::parse("SetTrackPan", args, error) && error.contains("finite-number"), "non-finite numeric variants are refused");
    }
}

static void muteSoloTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    require(c.setTrackVolume(0, .5) && c.setTrackPan(0, -.25) && c.setTrackSolo(2, true), "prepare unrelated settings");
    c.markAsNewBaseline();
    const auto before = c.document();
    const auto history = historyOf(c);
    const std::vector<cmd::Command> edits{cmd::SetTrackMute{0, true}, cmd::SetTrackSolo{1, true}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.changed && preview.changes.size() == 2
        && preview.changes[0].contains("Mute") && preview.changes[1].contains("Solo")
        && preview.after.tracks[0].settings.mute && preview.after.tracks[1].settings.solo,
        "mute/solo preview predicts and describes the requested states");
    require(alien::sameContent(before, c.document()) && historyOf(c) == history
        && !engine.tracks[0].mute.load() && !engine.tracks[1].solo.load(), "preview has no live or history side effects");
    require(commands.batch(edits, "Audition tracks").ok && alien::sameContent(preview.after, c.document())
        && engine.tracks[0].mute.load() && engine.tracks[1].solo.load() && engine.tracks[2].solo.load()
        && engine.tracks[0].gain.load() == .5f && engine.tracks[0].pan.load() == -.25f,
        "execution matches preview, preserves mix values and does not exclusively solo");
    require(c.undoLabel() == "Audition tracks" && commands.undo() && !c.canUndo()
        && alien::sameContent(before, c.document()) && !engine.tracks[0].mute.load()
        && !engine.tracks[1].solo.load() && engine.tracks[2].solo.load(), "one undo restores document and live states");
    const auto redoHistory = historyOf(c);
    for (const auto& edit : std::vector<cmd::Command>{cmd::SetTrackMute{0, false}, cmd::SetTrackSolo{1, false}}) {
        const auto result = commands.execute(edit);
        require(result.ok && !result.changed && historyOf(c) == redoHistory, "unchanged booleans preserve redo and create no history");
    }
    const auto invalid = commands.batch({cmd::SetTrackMute{0, true}, cmd::SetTrackSolo{99, true}}, "Invalid audition");
    require(!invalid.ok && invalid.failedCommand == 1 && historyOf(c) == redoHistory
        && alien::sameContent(before, c.document()) && !engine.tracks[0].mute.load(), "invalid later target prevents every batch write");
    require(!commands.execute(cmd::SetTrackMute{-1, true}).ok && !c.setTrackMute(-1, true)
        && !c.setTrackSolo(c.document().trackCount(), true), "negative and out-of-range targets are refused");
    c.setExternalBusy(true);
    require(!commands.execute(cmd::SetTrackSolo{0, true}).ok && !c.setTrackMute(0, true) && !c.setTrackSolo(0, true),
        "busy commands and direct setters refuse writes");
    c.setExternalBusy(false);
    require(historyOf(c) == redoHistory && alien::sameContent(before, c.document())
        && !engine.tracks[0].mute.load() && !engine.tracks[0].solo.load(), "refusals preserve document, live controls and history");
    require(commands.redo() && engine.tracks[0].mute.load() && engine.tracks[1].solo.load(), "redo restores the complete audition");
    require(commands.execute(cmd::SetTrackMute{0, false}).ok && c.undoLabel() == "Set track mute"
        && !engine.tracks[0].mute.load(), "explicit unmute uses a named undo step");
    require(commands.execute(cmd::SetTrackSolo{1, false}).ok && c.undoLabel() == "Set track solo"
        && !engine.tracks[1].solo.load() && engine.tracks[2].solo.load(), "explicit unsolo preserves other solo states");
    const auto structureBase = c.document();
    const std::vector<cmd::Command> structure{cmd::CreateTrack{}, cmd::SetTrackMute{4, true}, cmd::MoveTrack{4, 0},
        cmd::SetTrackSolo{0, true}, cmd::SetTrackMute{0, true}};
    const auto structurePreview = commands.preview(structure);
    require(structurePreview.ok && commands.batch(structure, "Create audition track").ok
        && structurePreview.after.trackCount() == c.document().trackCount()
        && structurePreview.after.tracks[0].settings.mute == c.document().tracks[0].settings.mute
        && structurePreview.after.tracks[0].settings.solo == c.document().tracks[0].settings.solo
        && engine.tracks[0].mute.load() && engine.tracks[0].solo.load(),
        "boolean commands follow create/reorder in a batch, including a repeated state");
    require(commands.undo() && alien::sameContent(structureBase, c.document()), "structural/boolean batch is one undo step");
}

static void muteSoloParseTests() {
    juce::String error;
    require(cmd::name(cmd::SetTrackMute{}) == "SetTrackMute" && cmd::name(cmd::SetTrackSolo{}) == "SetTrackSolo", "stable mute/solo names");
    for (const auto& edit : std::vector<cmd::Command>{cmd::SetTrackMute{0, true}, cmd::SetTrackMute{1, false},
                                                   cmd::SetTrackSolo{2, true}, cmd::SetTrackSolo{3, false}}) {
        const auto parsed = cmd::parse(cmd::name(edit), juce::JSON::parse(juce::JSON::toString(cmd::arguments(edit))), error);
        require(parsed && parsed->index() == edit.index()
            && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "boolean commands round-trip through JSON");
    }
    for (const auto* key : {"muted", "soloed"}) {
        const juce::String command = juce::String(key) == "muted" ? "SetTrackMute" : "SetTrackSolo";
        for (const auto* value : {"0", "1", "0.0", "\"true\"", "null", "[]", "{}"}) {
            const auto json = "{\"track\":0,\"" + juce::String(key) + "\":" + value + "}";
            require(!cmd::parse(command, juce::JSON::parse(json), error) && error.contains("boolean"), "boolean arguments reject truthy numbers, strings, null and containers");
        }
        require(!cmd::parse(command, juce::JSON::parse(R"({"track":0})"), error), "missing boolean is refused");
        const auto args = juce::JSON::parse("{\"track\":true,\"" + juce::String(key) + "\":true}");
        require(!cmd::parse(command, args, error) && error.contains("whole-number"), "boolean track indices are refused");
        require(!cmd::parse(command, juce::JSON::parse("{\"track\":0,\"" + juce::String(key) + "\":true,\"toggle\":true}"), error),
            "unknown boolean command arguments are refused");
    }
}

static void continuousCommandTests() {
    ToneEngine engine; alien::ProjectController c(engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); const auto baseline = c.document();
    require(commands.execute(cmd::SetTrackVolume{0, .8}, "drag-1", true).ok
        && commands.execute(cmd::SetTrackVolume{0, .6}, "drag-1", true).ok, "continuous commands accept live updates");
    require(c.undo() && !c.canUndo() && alien::sameContent(c.document(), baseline) && c.redo() && engine.tracks[0].gain.load() == .6f,
        "continuous command updates are one undo step with final-value redo");
    require(commands.execute(cmd::SetTrackVolume{0, .4}, "drag-2", true).ok && c.undo() && engine.tracks[0].gain.load() == .6f,
        "a new gesture key creates a separate undo step");
    const auto history = historyOf(c);
    require(commands.execute(cmd::SetTrackVolume{0, .6}, "drag-3", true).ok && historyOf(c) == history,
        "no-op gestures preserve redo and history");
    require(!commands.execute(cmd::SetTrackPan{0, 2}, "invalid", true).ok && historyOf(c) == history,
        "continuous editing retains strict command validation");
    require(commands.execute(cmd::SetTrackVolume{0, .5}, "interrupted", true).ok
        && commands.execute(cmd::SetTrackMute{1, true}).ok
        && commands.execute(cmd::SetTrackVolume{0, .3}, "interrupted", true).ok, "prepare an interleaved edit");
    require(c.undo() && engine.tracks[0].gain.load() == .5f && engine.tracks[1].mute.load(), "grouping never absorbs an unrelated intervening command");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    continuousCommandTests();
    nameTests();
    singleCommandTests();
    batchTests();
    previewTests();
    invalidArgumentTests();
    parseTests();
    parameterCommandTests();
    parameterParseTests();
    muteSoloTests();
    muteSoloParseTests();
    std::cout << "CommandEngineTests passed\n";
    return 0;
}
