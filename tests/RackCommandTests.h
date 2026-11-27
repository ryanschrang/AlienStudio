#include "PlaceSongClipFixture.h"
#pragma once
static void rackCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.setClipPattern(0,0,patternWith(60),false); require(c.copyCell(0,0), "rack command clipboard fixture");
    c.markAsNewBaseline(); const auto before = c.document(); const auto history = historyOf(c);
    const auto created = commands.preview(cmd::CreateRackPattern{});
    require(created.ok && created.after.channelRack.patternCount == before.channelRack.patternCount + 1
        && created.after.channelRack.selectedPattern == before.channelRack.patternCount, "Preview creates and selects empty rack pattern");
    const auto newId = created.after.channelRack.clips[static_cast<std::size_t>(before.channelRack.patternCount)];
    const std::vector<cmd::Command> edits{cmd::CreateRackPattern{},cmd::AssignRackClip{0,0,newId},cmd::AssignRackClip{1,1,newId}};
    const auto preview = commands.preview(edits);
    require(preview.ok && alien::sameContent(before,c.document()) && historyOf(c) == history, "rack Preview resolves new source IDs without live changes");
    require(commands.batch(edits,"Create and assign rack").changed && alien::sameContent(preview.after,c.document())
        && c.document().clip(0,0).rackClip == newId && c.document().clip(1,1).rackClip == newId, "sequential create/assign batch matches predicted identities");
    require(c.document().selectedTrack == before.selectedTrack && c.document().selectedScene == before.selectedScene
        && c.clipClipboard().source == before.cellClip(0,0), "rack commands preserve Session selection and clipboard");
    require(c.undo() && !c.canUndo() && alien::sameContent(before,c.document()) && c.redo(), "one Undo/Redo restores patterns and replaced cell content");
    const auto noOp = historyOf(c);
    require(commands.execute(cmd::AssignRackClip{0,0,newId}).ok && !commands.preview(cmd::AssignRackClip{0,0,newId}).changed
        && historyOf(c) == noOp, "same assignment is a no-op preserving history");
    require(c.undo(), "atomic rack refusal fixture"); const auto redo = historyOf(c);
    require(!commands.batch({cmd::CreateRackPattern{},cmd::AssignRackClip{99,0,newId}},"Bad rack").ok
        && alien::sameContent(before,c.document()) && historyOf(c) == redo, "invalid later assignment prevents prefix pattern creation");
    require(!commands.execute(cmd::AssignRackClip{0,0,newId}).ok, "deleted/undone rack source cannot resolve by another slot");
    const auto source = before.channelRack.clips[0];
    require(alien::test::placeSongClip(c,0,0,0), "Song reference fixture"); const auto song = c.document();
    require(!commands.execute(cmd::AssignRackClip{0,0,source}).ok && alien::sameContent(song,c.document()), "Song cell reference refuses assignment before mutation");
    auto frozen = before; frozen.tracks[0].settings.freezeScene = 0;
    require(cmd::refusal(frozen,cmd::AssignRackClip{0,0,source}).isNotEmpty(), "frozen assignment refuses");
    frozen.channelRack.channels[0].output = 0;
    require(cmd::refusal(frozen,cmd::CreateRackPattern{}).isNotEmpty(), "pattern creation honors frozen rack output routing");
    auto full = before; auto bank = full.channelRack; while (bank.addPattern()) {}
    require(full.setRack(std::move(bank)), "rack pattern limit fixture"); const auto fullBefore = full;
    require(cmd::applyTo(full,cmd::CreateRackPattern{}).isNotEmpty() && alien::sameContent(fullBefore,full), "full rack refuses without partial creation");
    c.setExternalBusy(true); require(!commands.execute(cmd::CreateRackPattern{}).ok, "busy rack create refuses"); c.setExternalBusy(false);
    for (const auto phase : {alien::RecordControls::countIn,alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        require(!commands.batch({cmd::RenameTrack{1,"Never"},cmd::CreateRackPattern{}},"Recording create").ok
            && !commands.execute(cmd::AssignRackClip{1,1,source}).ok && alien::sameContent(song,c.document()), "recording/count-in refuses create/assign before any mixed batch mutation");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    engine->playing.store(true); engine->requestedPianoRollClip[2].store(3);
    require(commands.execute(cmd::AssignRackClip{1,1,source}).changed && engine->playing.load()
        && engine->requestedPianoRollClip[2].load() == 3 && engine->requestedPianoRollClip[1].load() == -1, "assignment stops addressed ordinary track and preserves unrelated transport/queue");
    juce::String error;
    for (const cmd::Command& edit : std::vector<cmd::Command>{cmd::CreateRackPattern{},cmd::AssignRackClip{1,1,source},cmd::AssignRackClip{1,1,18446744073709551615ull}}) {
        const auto parsed = cmd::parse(cmd::name(edit),cmd::arguments(edit),error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "rack commands round-trip full 64-bit source identity without numeric precision loss");
    }
    for (const char* json : {"{}", "{\"track\":0,\"scene\":0,\"clip\":1}", "{\"track\":0,\"scene\":0,\"clip\":\"01\"}", "{\"track\":0,\"scene\":0,\"clip\":\"-1\"}", "{\"track\":0,\"scene\":0,\"clip\":\"18446744073709551616\"}"})
        require(!cmd::parse("AssignRackClip",juce::JSON::parse(json),error), "rack parser rejects missing, numeric, noncanonical and overflowing source IDs");
    require(!cmd::parse("CreateRackPattern",juce::JSON::parse("{\"extra\":1}"),error), "create pattern refuses unexpected arguments");
    {
        c.markAsNewBaseline();
        const auto baseline = c.document();
        const auto history = historyOf(c);
        engine->requestedPianoRollClip[2].store(3); engine->requestedPianoRollClip[3].store(2);
        const std::vector<cmd::Command> edits{cmd::AssignRackClip{2,2,source}, cmd::SetTrackVolume{2,.41}, cmd::AssignRackClip{3,2,source}};
        const auto prediction = commands.preview(edits);
        require(prediction.ok && prediction.changed && alien::sameContent(baseline,c.document()) && historyOf(c) == history
            && engine->requestedPianoRollClip[2].load() == 3 && engine->requestedPianoRollClip[3].load() == 2,
            "assignment/mixer Preview leaves document, history and live queues untouched");
        require(commands.batch(edits,"Assign shared rack and mix").changed && alien::sameContent(prediction.after,c.document())
            && c.document().clip(2,2).rackClip == source && c.document().clip(3,2).rackClip == source
            && c.document().channelRack == baseline.channelRack && engine->tracks[2].gain.load() == .41f
            && engine->requestedPianoRollClip[2].load() == -1 && engine->requestedPianoRollClip[3].load() == -1,
            "assignment batch preserves the shared source and publishes stops only after acceptance");
        require(c.undo() && !c.canUndo() && alien::sameContent(baseline,c.document())
            && engine->tracks[2].gain.load() == static_cast<float>(baseline.tracks[2].settings.gain),
            "one Undo restores both assignments and the live mix");
        const auto redo = historyOf(c);
        engine->requestedPianoRollClip[2].store(3); engine->requestedPianoRollClip[3].store(2);
        const auto noop = commands.execute(cmd::AssignRackClip{1,1,source});
        require(noop.ok && !noop.changed && historyOf(c) == redo && engine->requestedPianoRollClip[2].load() == 3,
            "unchanged assignment preserves redo and unrelated launch requests");
        const auto late = commands.batch({edits[0],edits[1],cmd::AssignRackClip{3,2,0}},"Refused assignment");
        require(!late.ok && late.failedCommand == 2 && alien::sameContent(baseline,c.document()) && historyOf(c) == redo
            && engine->requestedPianoRollClip[2].load() == 3 && engine->requestedPianoRollClip[3].load() == 2
            && engine->tracks[2].gain.load() == static_cast<float>(baseline.tracks[2].settings.gain),
            "late assignment refusal leaves live queues, mix, shared content and redo unchanged");
        c.setExternalBusy(true);
        const auto busy = commands.batch(edits,"Busy assignment"); c.setExternalBusy(false);
        engine->recording.phase.store(alien::RecordControls::countIn);
        const auto counting = commands.batch(edits,"Count-in assignment");
        engine->recording.phase.store(alien::RecordControls::idle);
        require(!busy.ok && !counting.ok && alien::sameContent(baseline,c.document()) && historyOf(c) == redo
            && engine->requestedPianoRollClip[2].load() == 3 && engine->requestedPianoRollClip[3].load() == 2,
            "busy and count-in assignment batches refuse before live effects or redo loss");
        require(c.redo() && alien::sameContent(prediction.after,c.document()) && engine->tracks[2].gain.load() == .41f,
            "Redo restores the accepted shared assignments and live mix");
    }
    std::cout << "PASS: rack creation/assignment, stable source IDs, Preview/Batch/Undo, no-ops and lifecycle guards\n";
}
