#pragma once
static void clipboardClipCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine(); c.setClipPattern(0, 0, patternWith(60), false);
    require(c.copyCell(0, 0), "clipboard snapshot fixture");
    c.setClipPattern(0, 0, patternWith(72), false); c.markAsNewBaseline();
    const auto before = c.document(); const auto history = historyOf(c);
    const std::vector<cmd::Command> edits{cmd::PasteClip{1, 0}, cmd::PasteLinkedClip{2, 0}, cmd::MakeClipUnique{2, 0}};
    const auto preview = commands.preview(edits);
    require(preview.ok && preview.changed && preview.after.clip(1, 0).notes().notes[0].pitch == 60
        && preview.after.clip(2, 0).notes().notes[0].pitch == 72, "copy uses captured notes; linked paste uses live source content");
    require(alien::sameContent(before, c.document()) && historyOf(c) == history, "clipboard Preview touches no live document or history");
    require(commands.batch(edits, "Paste and detach").changed && alien::sameContent(preview.after, c.document()), "sequential clipboard Preview equals execution with identical IDs");
    require(c.document().linkOf(2, 0) != c.document().linkOf(0, 0) && c.document().selectedTrack == before.selectedTrack,
        "independence detaches links and paste preserves selection");
    require(c.undo() && !c.canUndo() && alien::sameContent(before, c.document()) && c.redo(), "clipboard batch is exactly one undo/redo step");
    require(c.undo(), "redo preservation fixture"); const auto redo = historyOf(c);
    require(!commands.batch({cmd::PasteClip{1, 0}, cmd::PasteLinkedClip{99, 0}}, "Bad paste").ok
        && alien::sameContent(before, c.document()) && historyOf(c) == redo, "bad later command prevents all clipboard prefix edits");
    require(commands.batch({cmd::DeleteClip{0, 0}, cmd::PasteClip{1, 0}}, "Delete and paste").changed
        && c.document().clip(1, 0).notes().notes[0].pitch == 60, "independent copied snapshot survives source deletion within batch");
    require(c.undo(), "restore source");
    require(!commands.batch({cmd::DeleteClip{0, 0}, cmd::PasteLinkedClip{1, 0}}, "Delete and link").ok
        && alien::sameContent(before, c.document()), "linked paste refuses atomically if batch prunes its source");
    auto frozen = before; frozen.tracks[1].settings.freezeScene = 0;
    const auto copied = c.clipClipboard();
    require(cmd::refusal(frozen, cmd::PasteClip{1, 0}, &copied).isNotEmpty()
        && cmd::refusal(before, cmd::PasteClip{1, 0}).isNotEmpty(), "frozen destination and missing clipboard context refuse");
    auto offline = before; alien::SessionClip missing; missing.audioFilePath = "missing-clipboard.wav";
    const alien::clipboard_clip::Clipboard offlineCopy{{}, missing};
    require(cmd::applyTo(offline, cmd::PasteClip{3, 3}, &offlineCopy).isEmpty() && offline.clip(3, 3).audioOffline(),
        "offline audio snapshot paste needs no decoding or file access");
    require(commands.execute(cmd::PasteLinkedClip{1, 0}).changed, "linked no-op fixture");
    require(c.undo() && c.redo(), "restore linked paste");
    const auto linkedHistory = historyOf(c);
    require(commands.execute(cmd::PasteLinkedClip{1, 0}).ok && !commands.preview(cmd::PasteLinkedClip{1, 0}).changed
        && historyOf(c) == linkedHistory, "already-linked paste is a no-op");
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate = 8000; audio->channels = 1; audio->frames = 80;
    audio->left.assign(80, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("clipboard-audio.wav"), 0, 1, audio)
        && c.copyCell(0, 1), "audio clipboard fixture");
    auto audioPreview = commands.preview(std::vector<cmd::Command>{cmd::PasteClip{1, 1}, cmd::PasteLinkedClip{2, 1}, cmd::MakeClipUnique{2, 1}});
    require(audioPreview.ok && commands.batch({cmd::PasteClip{1, 1}, cmd::PasteLinkedClip{2, 1}, cmd::MakeClipUnique{2, 1}}, "Audio paste").changed
        && alien::sameContent(audioPreview.after, c.document()) && c.document().clip(1, 1).audio == audio
        && c.document().clip(2, 1).audio == audio && c.document().linkOf(0, 1) != c.document().linkOf(2, 1), "audio paste/independence shares immutable media but owns independent content");
    const auto source = c.channelRack().clips[0]; require(c.assignRackClip(0, 2, source) && c.copyCell(0, 2), "rack clipboard fixture");
    const auto rackBefore = c.document(); const auto rackHistory = historyOf(c);
    const std::vector<cmd::Command> rackEdits{cmd::PasteLinkedClip{1, 2}, cmd::PasteClip{2, 2}, cmd::MakeClipUnique{1, 2}};
    const auto rackPreview = commands.preview(rackEdits);
    require(rackPreview.ok && commands.batch(rackEdits, "Rack paste").changed && alien::sameContent(rackPreview.after, c.document())
        && c.document().clip(1, 2).rackClip != source && c.document().clip(2, 2).rackClip != source,
        "rack link/copy/independence Preview predicts source allocation without consuming live IDs");
    require(c.undo() && alien::sameContent(rackBefore, c.document()) && historyOf(c).undoLabel == rackHistory.undoLabel, "rack batch undo restores source bank and assignments");
    auto full = c.document(); auto bank = full.channelRack; while (bank.addPattern(0)) {}
    require(full.setRack(std::move(bank)), "rack capacity fixture"); const auto fullBefore = full; const auto clipboard = c.clipClipboard();
    require(cmd::applyTo(full, cmd::PasteClip{3, 2}, &clipboard).isNotEmpty() && alien::sameContent(full, fullBefore), "full rack copy refuses without partial source allocation");
    c.setClipPattern(3, 0, patternWith(50), false); require(c.placeRegion(3, 0, 0), "Song cell rack refusal fixture");
    const auto protectedCell = c.document();
    require(!commands.execute(cmd::PasteClip{3, 0}).ok && alien::sameContent(protectedCell, c.document()), "rack copy cannot replace a Song cell source and leaves no orphan pattern");
    c.setExternalBusy(true); require(!commands.execute(cmd::PasteClip{3, 2}).ok, "busy paste refuses"); c.setExternalBusy(false);
    engine->recording.phase.store(alien::RecordControls::countIn);
    require(!commands.batch({cmd::RenameTrack{0, "No"}, cmd::PasteClip{3, 2}}, "Count-in paste").ok && alien::sameContent(protectedCell, c.document()), "count-in rejects mixed batch before mutations");
    engine->recording.phase.store(alien::RecordControls::idle);
    for (const cmd::Command& edit : std::vector<cmd::Command>{cmd::PasteClip{1, 1}, cmd::PasteLinkedClip{2, 2}, cmd::MakeClipUnique{0, 1}}) {
        juce::String error; const auto parsed = cmd::parse(cmd::name(edit), cmd::arguments(edit), error);
        require(parsed && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(edit)), "clipboard commands round-trip strict address arguments");
    }
    juce::String error;
    require(!cmd::parse("PasteClip", juce::JSON::parse("{\"track\":0,\"scene\":true}"), error)
        && !cmd::parse("PasteLinkedClip", juce::JSON::parse("{\"track\":0,\"scene\":0,\"extra\":1}"), error), "clipboard parsing rejects mistyped/unknown arguments");
    std::cout << "PASS: clipboard commands, snapshots, linked/live content, all-type independence and atomic refusal\n";
}
