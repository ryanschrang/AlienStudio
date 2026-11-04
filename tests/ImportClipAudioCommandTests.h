#pragma once
#include "AudioFileWrite.h"
// ImportClipAudio is the one route for decoded audio into a Session cell.
static std::shared_ptr<alien::SessionAudioClip> importFixtureClip(std::size_t frames = 80, float level = .2f) {
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = frames; audio->left.assign(frames, level); audio->buildWaveform();
    return audio;
}
static std::shared_ptr<const alien::session_audio_import::Prepared> preparedImport(const alien::ProjectController& c, const juce::File& file, int track, int scene,
                                                                                    std::shared_ptr<alien::SessionAudioClip> audio, bool emptyOnly = false) {
    const auto plan = c.prepareAudioImport(file, track, scene, emptyOnly);
    require(plan.has_value(), "import plan fixture");
    return std::make_shared<const alien::session_audio_import::Prepared>(alien::session_audio_import::Prepared{*plan, {std::move(audio), {}}});
}

static void importClipAudioNameChecks() {
    juce::String error;
    require(cmd::name(cmd::ImportClipAudio{}) == "ImportClipAudio" && cmd::label(cmd::ImportClipAudio{}) == "Import audio", "stable name and undo label");
    require(!cmd::parse("ImportClipAudio", juce::JSON::parse(R"({"file":"C:/a.wav"})"), error) && error.contains("decoded audio"), "a decoded clip cannot come from arguments, and parse says so");
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    const auto nothing = commands.preview(cmd::ImportClipAudio{});
    require(!nothing.ok && nothing.error.contains("no decoded audio") && !commands.execute(cmd::ImportClipAudio{}).ok, "a command without a result is refused");
}

static void importClipAudioCommandChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("import-command.wav");
    const auto audio = importFixtureClip();
    c.setClipPattern(0, 0, patternWith(60), false); c.markAsNewBaseline();

    // Preview is pure: no retention, publication or history.
    const auto before = c.document(); const auto history = historyOf(c); const auto bytes = c.availableAudioBytes();
    const auto import = cmd::ImportClipAudio{preparedImport(c, file, 0, 0, audio)};
    const auto preview = commands.preview(import);
    require(preview.ok && preview.changed && preview.after.clip(0, 0).audio == audio && preview.after.clip(0, 0).notes().notes[0].pitch == 60
        && preview.changes[0].contains("import-command.wav"), "Preview shows the audio installed beside the notes");
    require(alien::sameContent(before, c.document()) && historyOf(c) == history && c.availableAudioBytes() == bytes, "Preview leaves the project, storage and history alone");

    // Execute matches Preview as one undo step, and leaves the selection where it was.
    c.selectClip(3, 3); const auto selected = c.document();
    require(commands.execute(import).changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Import audio", "execute matches Preview");
    require(c.document().selectedTrack == 3 && c.document().selectedScene == 3 && c.availableAudioBytes() == bytes - audio->decodedBytes(), "the selection stays and the audio counts against storage");
    require(c.undo() && alien::sameContent(selected, c.document()) && c.redo() && c.document().clip(0, 0).audio == audio, "one Undo and one Redo");

    // Replacing audio with another decode of the same file is still an edit.
    const auto again = importFixtureClip(160, .4f);
    require(commands.execute(cmd::ImportClipAudio{preparedImport(c, file, 0, 0, again)}).changed && c.document().clip(0, 0).audio == again, "a new decode of the same file replaces the old one");

    // A result captured before an edit is stale.
    const auto stale = cmd::ImportClipAudio{preparedImport(c, file, 1, 0, audio)};
    c.setClipPattern(1, 0, patternWith(64), false); c.commit("Changed while decoding");
    const auto changed = c.document(); const auto label = c.undoLabel(); const auto free = c.availableAudioBytes();
    const auto refused = commands.execute(stale);
    require(!refused.ok && refused.error.contains("changed while") && alien::sameContent(changed, c.document()) && c.undoLabel() == label && c.availableAudioBytes() == free, "a stale result changes nothing");

    // Moved tracks and scenes do not redirect it; deleted ones refuse it.
    const auto follows = cmd::ImportClipAudio{preparedImport(c, file, 2, 2, audio)};
    require(c.moveTrack(2, 1) && c.moveScene(2, -1), "reorder fixture");
    require(commands.execute(follows).changed && c.document().clip(3, 1).audio == audio, "the destination follows its track and scene identities");
    const auto gone = cmd::ImportClipAudio{preparedImport(c, file, 0, 3, audio)};
    require(c.deleteScene(3), "delete fixture");
    require(!commands.execute(gone).ok, "a deleted destination refuses");

    // An empty-only plan refuses once something lands in the cell.
    const auto empty = cmd::ImportClipAudio{preparedImport(c, file, 2, 0, audio, true)};
    require(commands.execute(cmd::CreateClip{2, 0}).changed, "fill the cell");
    const auto notEmpty = commands.execute(empty);
    require(!notEmpty.ok && notEmpty.error == "Destination is no longer empty." && !c.document().clip(2, 0).isAudioClip(), "a drop never replaces what arrived while decoding");
    require(!c.prepareAudioImport(file, 2, 0, true), "and is not planned onto an occupied cell");

    // Recording, count-in and busy work refuse before any live change.
    const auto waiting = cmd::ImportClipAudio{preparedImport(c, file, 2, 1, audio)};
    const auto steady = c.document(); const auto steadyBytes = c.availableAudioBytes();
    for (const auto phase : {alien::RecordControls::countIn, alien::RecordControls::recording}) {
        engine->recording.phase.store(phase);
        const auto r = commands.execute(waiting);
        require(!r.ok && r.error.contains("Stop recording") && alien::sameContent(steady, c.document()) && c.availableAudioBytes() == steadyBytes, "recording refuses");
    }
    engine->recording.phase.store(alien::RecordControls::idle);
    c.setExternalBusy(true);
    require(!commands.execute(waiting).ok && alien::sameContent(steady, c.document()), "busy work refuses");
    c.setExternalBusy(false);

    // A linked cell shares the audio, and is one undo step.
    c.setClipPattern(1, 2, patternWith(67), false); c.commit("Linked source");
    require(c.copyCell(1, 2) && c.pasteCell(2, 2, true), "linked fixture");
    require(commands.execute(cmd::ImportClipAudio{preparedImport(c, file, 1, 2, audio)}).changed && c.document().clip(2, 2).audio == audio, "linked cells share the audio");
    require(c.undo() && !c.document().clip(2, 2).isAudioClip() && !c.document().clip(1, 2).isAudioClip(), "and one Undo takes it from both");

    // A batch is all or nothing, and runs against the document as it goes.
    const auto batchBefore = c.document(); const auto batchBytes = c.availableAudioBytes();
    const auto first = cmd::ImportClipAudio{preparedImport(c, file, 1, 1, audio)};
    require(!commands.batch({first, first}, "Twice").ok && alien::sameContent(batchBefore, c.document()) && c.availableAudioBytes() == batchBytes, "the second import into one cell refuses both");
    const auto renamed = commands.batch({first, cmd::RenameTrack{1, "Vocals"}}, "Import vocals");
    require(renamed.changed && c.document().clip(1, 1).audio == audio && c.document().tracks[1].name == "Vocals" && c.undoLabel() == "Import vocals", "import and rename are one step");
    require(c.undo() && alien::sameContent(batchBefore, c.document()), "one Undo reverts the batch");
    std::cout << "PASS: ImportClipAudio preview, one undo step, stale refusal, stable destination, empty-only, recording, linked cells and batches\n";
}

// Storage is checked before anything is retained, and says so.
static void importClipAudioStorageChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); cmd::CommandEngine commands(c);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("import-full.wav");
    const auto big = importFixtureClip((std::min<std::size_t>)(alien::SessionAudioClip::maxDecodedBytes, 300u * 1024u * 1024u) / sizeof(float), 0.f);
    const auto cells = static_cast<int>(alien::ProjectController::maxSessionAudioBytes / big->decodedBytes()) + 1;
    const auto tracks = c.document().trackCount();
    require(cells <= tracks * c.document().sceneCount(), "the storage fixture fits the default grid");
    std::vector<cmd::Command> batch;
    for (int i = 0; i < cells; ++i) batch.push_back(cmd::ImportClipAudio{preparedImport(c, file, i % tracks, i / tracks, big)});
    const auto before = c.document(); const auto free = c.availableAudioBytes(); const auto label = c.undoLabel();
    const auto refused = commands.batch(batch, "Too much");
    require(!refused.ok && refused.error.contains("storage is full") && alien::sameContent(before, c.document()) && c.availableAudioBytes() == free && c.undoLabel() == label, "too much audio names the limit and changes nothing");
    std::cout << "PASS: ImportClipAudio refuses audio over the storage limit before retaining any\n";
}

// The entry points the views call, through a real decode where there is one.
static void importEntryPointChecks() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine); c.pushDocumentToEngine();
    juce::String shown, error; int changedTrack = -1, changedScene = -1;
    c.onImportStatus = [&](const juce::String& text, const juce::String&) { shown = text; };
    c.onClipContentChanged = [&](int t, int s) { changedTrack = t; changedScene = s; };
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("import-entry", "", false);
    require(root.createDirectory().wasOk(), "entry fixture folder");
    const auto wav = root.getChildFile("entry point.wav");
    std::atomic<bool> cancel{false};
    require(alien::writeClipWav(wav, *importFixtureClip(), cancel, error), "entry fixture file");
    auto finish = [&] {
        for (int i = 0; i < 400 && c.isBusy(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(!c.isBusy(), "the decode finishes");
    };

    // The Import audio button replaces audio in place, and may hold notes beside it.
    c.setClipPattern(1, 1, patternWith(60), false);
    cmd::importAudioFile(c, wav, 1, 1);
    require(c.isBusy() && shown.startsWith("Loading"), "the decode runs in the background and says so");
    c.selectClip(0, 3);
    finish();
    require(c.document().clip(1, 1).isAudioClip() && c.document().clip(1, 1).notes().notes[0].pitch == 60 && !c.document().clip(0, 3).isAudioClip(), "it lands where it was aimed, not at the new selection");
    require(c.undoLabel() == "Import audio" && shown == "Imported entry point.wav" && changedTrack == 1 && changedScene == 1, "one named step, a message and an editor refresh");

    // A browser or Explorer drop only takes an empty cell.
    const auto label = c.undoLabel();
    cmd::importSampleToCell(c, wav, 1, 1);
    require(!c.isBusy() && c.undoLabel() == label && shown.contains("empty"), "an occupied cell is refused before decoding");
    cmd::importSampleToCell(c, wav, 2, 2);
    require(c.isBusy(), "a drop decodes in the background");
    finish();
    require(c.document().clip(2, 2).isAudioClip() && c.undoLabel() == "Import audio", "a drop is the same command");
    require(c.undo() && !c.document().cellClip(2, 2), "and one Undo removes it");

    // A missing file is reported and changes nothing.
    const auto steady = c.document();
    cmd::importSampleToCell(c, root.getChildFile("missing.wav"), 3, 3);
    finish();
    require(alien::sameContent(steady, c.document()) && !shown.startsWith("Imported"), "a failed decode changes nothing and says so");

    // A finished take or tempo render is the same command, so it honours the same refusals.
    const auto take = importFixtureClip();
    const auto installed = cmd::installDecodedAudio(c, wav, 0, 0, take);
    require(installed.ok && installed.changed && c.document().clip(0, 0).audio == take && c.undoLabel() == "Import audio", "a decoded take installs as one step");
    require(c.assignRackClip(0, 1, c.channelRack().clips[0]), "rack fixture");
    const auto rackBefore = c.document();
    require(!cmd::installDecodedAudio(c, wav, 0, 1, take).ok && alien::sameContent(rackBefore, c.document()), "a rack cell refuses rather than losing the take silently");
    engine->recording.phase.store(alien::RecordControls::recording);
    require(!cmd::installDecodedAudio(c, wav, 3, 0, take).ok && !c.document().clip(3, 0).isAudioClip(), "recording refuses");
    engine->recording.phase.store(alien::RecordControls::idle);
    require(root.deleteRecursively(), "remove entry fixtures");
    std::cout << "PASS: the Import audio button, browser and Explorer drops, and decoded takes all run ImportClipAudio\n";
}
