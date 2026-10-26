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

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    nameTests();
    singleCommandTests();
    batchTests();
    previewTests();
    invalidArgumentTests();
    parseTests();
    std::cout << "CommandEngineTests passed\n";
    return 0;
}
