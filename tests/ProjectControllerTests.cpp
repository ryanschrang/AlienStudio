#include "ProjectController.h"
#include "AudioFileWrite.h"
#include <cstdlib>
#include <iostream>

// Project model and controller: dynamic tracks/scenes, engine sync and the
// version 4 project format. No audio device or window is needed.

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

static alien::PianoRollPattern patternWith(int pitch) {
    alien::PianoRollPattern p;
    p.add(pitch, 0.0, 1.0);
    return p;
}

static void structureTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    require(d.trackCount() == 4 && d.sceneCount() == 4, "new project starts with 4 tracks and 4 scenes");
    c.pushDocumentToEngine();
    require(engine.activeTracks.load() == 4, "engine knows how many tracks are in use");

    for (int i = 0; i < 12; ++i) require(c.addTrack(), "tracks can be added up to the limit");
    require(d.trackCount() == alien::maxSessionTracks && !c.addTrack(), "track count is capped");
    require(engine.activeTracks.load() == alien::maxSessionTracks, "engine follows added tracks");
    require(d.tracks[4].name == "Track 5" && d.tracks[15].name == "Track 16", "new tracks get the next free name");
    for (const auto& scene : d.clips) require(static_cast<int>(scene.size()) == d.trackCount(), "every scene has a clip per track");

    // Delete a middle track: later tracks shift left and keep their content.
    c.setClipPattern(5, 0, patternWith(65), false);
    c.selectClip(5, 2);
    require(c.deleteTrack(2), "delete a middle track");
    require(d.trackCount() == 15 && d.tracks[4].name == "Track 6" && d.clip(4, 0).notes().count == 1
            && d.clip(4, 0).notes().notes[0].pitch == 65, "content moves with its track");
    require(d.selectedTrack == 4 && d.selectedScene == 2, "selection follows the moved track");
    while (d.trackCount() > 1) require(c.deleteTrack(0), "delete tracks");
    require(!c.deleteTrack(0) && d.trackCount() == 1, "the last track cannot be deleted");

    require(c.moveTrack(0, 1) == false, "cannot move beyond the ends");
    c.addTrack();
    c.renameTrack(0, "  Drums  ");
    require(c.moveTrack(0, 1) && d.tracks[1].name == "Drums", "tracks reorder");
    require(!c.renameTrack(1, "   "), "blank names are rejected");

    // Scenes.
    c.setClipPattern(0, 1, patternWith(72), false);
    require(c.duplicateScene(1) && d.sceneCount() == 5 && d.scenes[2].name == "Scene 2 copy"
            && d.clip(0, 2).notes().notes[0].pitch == 72, "duplicate scene copies its clips after it");
    // Shared immutable patterns: editing the copy must not change the original.
    c.setClipPattern(0, 2, patternWith(40), false);
    require(d.clip(0, 1).notes().notes[0].pitch == 72, "duplicated clips are independent");
    require(c.moveScene(2, -1) && d.scenes[1].name == "Scene 2 copy", "scenes reorder");
    require(c.deleteScene(1) && d.sceneCount() == 4, "delete scene");
    while (c.addScene()) {}
    require(d.sceneCount() == alien::maxSessionScenes, "scene count is capped");
    while (d.sceneCount() > 1) c.deleteScene(d.sceneCount() - 1);
    require(!c.deleteScene(0), "the last scene cannot be deleted");
    std::cout << "PASS: track and scene add/delete/move/rename/duplicate\n";
}

static void fileFormatTests() {
    auto doc = alien::ProjectDocument::createDefault(9, 11);
    doc.tracks[8].name = "Vox";
    doc.tracks[8].colour = 5;
    doc.tracks[8].settings = {7, 0.5f, 0.25f, false, true};
    doc.scenes[10].name = "Outro";
    doc.clip(8, 10).pattern = std::make_shared<const alien::PianoRollPattern>(patternWith(50));
    doc.selectedTrack = 8;
    doc.selectedScene = 10;
    const auto json = alien::serializeSessionProject(alien::toProjectData(doc));
    require(json.isNotEmpty(), "9 x 11 project serializes");
    alien::SessionProjectData parsed;
    juce::String error;
    require(alien::parseSessionProject(json, parsed, error), "version 4 project parses");
    const auto back = alien::fromProjectData(parsed);
    require(back.trackCount() == 9 && back.sceneCount() == 11 && back.tracks[8].name == "Vox"
            && back.tracks[8].colour == 5 && back.tracks[8].settings.preset == 7 && back.tracks[8].settings.solo
            && back.scenes[10].name == "Outro" && back.clip(8, 10).notes().notes[0].pitch == 50
            && back.selectedTrack == 8 && back.selectedScene == 10,
            "names, colours, presets, clips and selection round-trip");

    // Inconsistent grids are rejected.
    auto broken = juce::JSON::parse(json);
    broken.getDynamicObject()->getProperty("scenes").getArray()->getReference(3).getArray()->remove(0);
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "ragged scenes are rejected");

    // 1,024 notes in one clip.
    alien::PianoRollPattern big;
    for (std::size_t i = 0; i < alien::PianoRollPattern::maxNotes; ++i)
        require(big.add(static_cast<int>(i % 128), static_cast<double>(i % 64), 0.5) != 0, "fill a clip to 1024 notes");
    require(big.count == 1024 && big.valid() && big.add(60, 0, 1) == 0, "clips hold exactly 1024 notes");
    std::cout << "PASS: version 4 project format and 1024-note clips\n";
}

static void pump(int ms = 50) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

static void undoTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    int restored = 0;
    c.onDocumentRestored = [&] { ++restored; };
    require(!c.canUndo() && !c.canRedo() && !c.isDirty(), "a new project is clean with no history");

    c.setStepOn(3, true);
    c.commit("Turn on step");
    require(c.canUndo() && c.undoLabel() == "Turn on step" && c.isDirty(), "an edit is undoable and unsaved");
    c.commit("Nothing changed");
    require(c.undoLabel() == "Turn on step", "commits that change nothing are ignored");

    c.setClipPattern(1, 2, patternWith(64), false);
    c.commit("Edit notes", 1, 2);
    c.selectClip(3, 3);
    require(c.addTrack(), "add track");
    c.commit("Add track");
    require(d.trackCount() == 5 && engine.activeTracks.load() == 5, "five tracks");

    require(c.undo() && d.trackCount() == 4 && engine.activeTracks.load() == 4, "undo add track restores the engine too");
    require(c.redoLabel() == "Add track" && c.canRedo(), "redo is available");
    require(c.undo() && d.clip(1, 2).notes().count == 0 && d.selectedTrack == 1 && d.selectedScene == 2,
            "undoing a clip edit restores the notes and shows that clip");
    require(c.undo() && !d.steps.on[3] && engine.steps[3].load() == false && !c.canUndo(),
            "undo the first edit; the engine step follows");
    require(!c.isDirty(), "undoing everything returns to the saved state");
    require(c.redo() && c.redo() && d.clip(1, 2).notes().notes[0].pitch == 64 && c.isDirty(), "redo re-applies edits");
    const auto unchanged = d.clip(1, 2).pattern;
    const auto copy = d.clip(1, 2).notes();
    c.setClipPattern(1, 2, copy, false);
    require(d.clip(1, 2).pattern == unchanged, "re-loading identical notes is not treated as an edit");
    c.setBpm(130);
    c.commit("Change tempo");
    require(!c.canRedo(), "a new edit clears redo");
    require(restored == 5, "every undo/redo notifies the views");

    // Rapid continuous edits merge into one step.
    for (int bpm = 131; bpm < 140; ++bpm) { c.setBpm(bpm); c.commit("Change tempo", -1, -1, "tempo"); }
    require(c.undo() && d.bpm == 130, "coalesced tempo changes undo in one step");

    // Undo of a reorder keeps playing slots consistent.
    c.renameTrack(0, "Kick");
    c.commit("Rename");
    require(c.moveTrack(0, 2), "move track");
    c.commit("Move track");
    require(c.undo() && d.tracks[0].name == "Kick", "undo move track");

    c.newProject();
    require(!c.canUndo() && !c.isDirty() && d.trackCount() == 4, "new project resets history");
    std::cout << "PASS: project undo/redo, merging, no-op skipping and unsaved state\n";
}

static void saveAndAutosaveTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienStudioUndoTest", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");

    ToneEngine engine;
    alien::ProjectController c(engine);
    c.addTrack();
    c.renameTrack(4, "Bass");
    c.commit("Add track");
    require(c.isDirty() && !alien::ProjectController::hasRecoverableSession(), "dirty, nothing autosaved yet");

    c.autosaveIfNeeded();
    for (int i = 0; i < 100 && !alien::ProjectController::hasRecoverableSession(); ++i) pump();
    pump(100);
    require(alien::ProjectController::hasRecoverableSession(), "autosave writes a recovery file");
    alien::SessionProjectData recovered;
    juce::String error;
    require(alien::parseSessionProject(alien::ProjectController::recoveryFile().loadFileAsString(), recovered, error)
            && recovered.trackCount() == 5 && recovered.trackNames[4] == "Bass", "autosave contains the latest edits");

    // A second instance recovers it as unsaved work.
    ToneEngine engine2;
    alien::ProjectController c2(engine2);
    bool replaced = false;
    c2.onDocumentReplaced = [&] { replaced = true; };
    c2.recoverSession();
    for (int i = 0; i < 100 && !replaced; ++i) pump();
    require(replaced && c2.document().trackCount() == 5 && c2.isDirty() && c2.currentFile() == juce::File{},
            "recovered session is restored and still needs saving");

    // Saving writes the project, clears the unsaved state and removes the autosave.
    bool saved = false, finished = false;
    const auto target = root.getChildFile("Song.alienstudio");
    c2.writeProject(target, false, [&](bool ok) { saved = ok; finished = true; });
    for (int i = 0; i < 100 && !finished; ++i) pump();
    require(saved && target.existsAsFile() && !c2.isDirty() && c2.currentFile() == target,
            "save completes and marks the project clean");
    require(!alien::ProjectController::hasRecoverableSession(), "a clean save removes the autosave");

    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: autosave, crash recovery and save completion\n";
}

static void recoveryAndPortableRegressionTests() {
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienRegression","",false);
    root.createDirectory(); alien::ProjectController::recoveryDirectoryOverride()=root.getChildFile("Recovery");
    auto engine=std::make_unique<ToneEngine>(); alien::ProjectController c(*engine);
    c.setBpm(131);c.commit("Tempo");c.autosaveIfNeeded();
    // Deliberately discard immediately; the worker may be queued or writing.
    c.newProject();
    for(int i=0;i<20;++i)pump(10);
    require(!alien::ProjectController::hasRecoverableSession(),"New fences an in-flight autosave");
    c.setBpm(132);c.commit("Tempo");c.autosaveIfNeeded();c.discardRecovery();
    for(int i=0;i<20;++i)pump(10);
    require(!alien::ProjectController::hasRecoverableSession(),"Quit/discard fences an in-flight autosave");

    auto clip=std::make_shared<alien::SessionAudioClip>();clip->sampleRate=8000;clip->channels=1;clip->frames=80;clip->left.assign(80,0.1f);clip->buildWaveform();
    const auto original=root.getChildFile("Original.wav");std::atomic<bool> cancel{false};juce::String error;
    require(alien::writeClipWav(original,*clip,cancel,error),"create source WAV");
    require(c.installAudio(original,0,0,clip),"install source audio");
    c.setBpm(140);c.commit("Tempo before portable save");
    bool done=false,ok=false;const auto portable=root.getChildFile("Portable.alienstudio");
    c.writeProject(portable,true,[&](bool result){ok=result;done=true;});
    for(int i=0;i<100 && !done;++i)pump(10);
    require(done && ok,"portable save completes");
    const auto collected=c.document().clip(0,0).audioFilePath;
    require(collected!=original.getFullPathName() && juce::File(collected).existsAsFile(),"portable audio copied");
    original.deleteFile();
    require(c.undo() && c.document().clip(0,0).audioFilePath==collected,"undo retains collected audio path");
    require(c.redo() && c.document().clip(0,0).audioFilePath==collected,"redo retains collected audio path");

    // Both historical v4 dialects and v5 load into the unified v6 format.
    auto legacy=juce::JSON::parse(alien::serializeSessionProject(alien::toProjectData(c.document())));
    auto* object=legacy.getDynamicObject();object->setProperty("version",4);
    object->removeProperty("plugins");object->removeProperty("masterPlugins");object->removeProperty("drumPaths");object->removeProperty("arrangement");object->removeProperty("timeline");
    for(auto& value:*object->getProperty("trackSettings").getArray()) value.getDynamicObject()->removeProperty("instrument");
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(juce::JSON::toString(legacy),loaded,error),"redesign v4 with no drum fields migrates");
    auto combined=alien::toProjectData(c.document());combined.drumPaths[0][2]=collected;combined.trackSettings[0].instrument=1;
    // Before version 8 the song was whole bars of Session clips, saved as "arrangement".
    auto v5=juce::JSON::parse(alien::serializeSessionProject(combined));
    {
        auto* root5=v5.getDynamicObject();root5->setProperty("version",5);root5->removeProperty("timeline");
        auto* region=new juce::DynamicObject();region->setProperty("track",3);region->setProperty("scene",3);region->setProperty("startBar",0);region->setProperty("lengthBars",4);
        juce::Array<juce::var> regions;regions.add(juce::var(region));
        auto* arrangement=new juce::DynamicObject();arrangement->setProperty("lengthBars",8);arrangement->setProperty("regions",regions);
        root5->setProperty("arrangement",juce::var(arrangement));
    }
    require(alien::parseSessionProject(juce::JSON::toString(v5),loaded,error) && loaded.drumPaths[0][2]==collected && loaded.timeline.regions.size()==1,"drum/arrangement v5 migrates");
    require(loaded.timeline.lengthBars==8 && loaded.timeline.regions[0].row==3 && loaded.timeline.regions[0].source==3
        && loaded.timeline.regions[0].start==0 && loaded.timeline.regions[0].length==4*alien::timeline::ticksPerBar,"the old bars become the same place on the timeline");
    // Damaged old arrangements are refused, not quietly repaired.
    {
        auto* root5=v5.getDynamicObject();auto* arr=root5->getProperty("arrangement").getDynamicObject();
        auto* second=new juce::DynamicObject();second->setProperty("track",3);second->setProperty("scene",1);second->setProperty("startBar",2);second->setProperty("lengthBars",2);
        auto* regionList=arr->getProperty("regions").getArray();regionList->add(juce::var(second));
        alien::SessionProjectData refused;
        require(!alien::parseSessionProject(juce::JSON::toString(v5),refused,error) && error.isNotEmpty(),"overlapping legacy regions are refused");
    }
    c.discardRecovery();alien::ProjectController::recoveryDirectoryOverride()=juce::File{};root.deleteRecursively();
    std::cout<<"PASS: recovery fencing, portable undo/redo and both legacy schemas\n";
}

// The song snapshot sent to the audio thread is large, and every republish used
// to restart arrangement playback. Edits that do not change clips or the
// timeline must not publish at all.
static void arrangementPublishTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    const auto published = [&] { return engine.arrangementMailbox.publishedCount(); };
    const auto base = published();
    require(base >= 1, "the initial song is published");
    c.pushDocumentToEngine();
    require(published() == base, "re-sending an unchanged project does not republish the song");

    c.renameTrack(0, "Bass"); c.commit("Rename track");
    c.renameScene(0, "Verse"); c.commit("Rename scene");
    c.setBpm(133); c.commit("Change tempo");
    c.trackSettingsEdited(0); c.commit("Change mix");
    require(published() == base, "renames, tempo and mixer edits leave the song snapshot alone");

    c.setClipPattern(0, 0, patternWith(65), false); c.commit("Edit notes", 0, 0);
    require(published() == base + 1, "changing clip contents publishes once");
    c.commit("No-op");
    require(published() == base + 1, "a commit with no change does not publish");

    require(c.placeRegion(0, 0, 0), "place a clip on the timeline");
    require(published() == base + 2, "changing the timeline publishes once");

    require(c.undo() && published() == base + 3, "undoing the timeline change republishes");
    require(c.undo() && published() == base + 4, "undoing the note edit republishes");
    while (c.undo()) {}
    require(published() == base + 4, "undoing renames, tempo and mix does not republish");
}

static void masterVolumeTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    require(c.document().masterGain == 1.0f && engine.masterControls.gain.load() == 1.0f, "new projects start at unity master volume");
    engine.masterControls.gain.store(2.0f); // the fader writes the engine directly
    c.masterGainEdited();
    c.commit("Change master volume", -1, -1, "master-volume");
    require(c.document().masterGain == 2.0f && c.canUndo() && c.undoLabel() == "Change master volume", "master volume is an undoable edit");
    require(alien::toProjectData(c.document()).masterGain == 2.0f, "master volume is saved with the project");
    require(c.undo() && c.document().masterGain == 1.0f && engine.masterControls.gain.load() == 1.0f, "undo restores the engine's master volume");
    require(c.redo() && engine.masterControls.gain.load() == 2.0f, "redo re-applies it");
    engine.masterControls.gain.store(99.0f);
    c.masterGainEdited();
    require(c.document().masterGain == alien::maxMasterGain, "an out-of-range value is clamped before it reaches the document");
    c.newProject();
    require(engine.masterControls.gain.load() == 1.0f && c.document().masterGain == 1.0f, "a new project resets the master volume");
}

static void mediaIndexTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienStudioMediaIndex", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    auto touch = [&](const char* relative) {
        const auto file = root.getChildFile(relative);
        require(file.getParentDirectory().createDirectory().wasOk() && file.replaceWithText("x"), "create index fixture");
    };
    touch("a/Kick.WAV"); touch("moved/samples/kick.wav"); touch("moved/kick.wav"); touch("moved/deep/er/snare.mp3"); touch("moved/notes.txt");
    alien::MediaIndex index(root);
    require(index.filesIndexed() == 4 && !index.wasTruncated(), "only wav/mp3 files are indexed");
    // The original lived in <something>/samples/kick.wav: the copy that keeps that folder name wins.
    auto found = index.candidates("C:\\Music\\Old Project\\samples\\KICK.wav");
    require(found.size() == 3 && found[0] == root.getChildFile("moved/samples/kick.wav"),
        "names match case-insensitively and a matching folder layout is preferred");
    require(index.candidates("/x/y/snare.mp3").size() == 1 && index.candidates("/x/y/hat.wav").empty(), "unknown names have no candidates");
    require(alien::MissingMedia::fileNameOf("C:\\a\\b\\c.wav") == "c.wav" && alien::MissingMedia::fileNameOf("rel/d.wav") == "d.wav"
        && alien::MissingMedia::fileNameOf("e.wav") == "e.wav", "file names are read from Windows and POSIX paths");
    // A file that does not decode is skipped in favour of one that does.
    auto good = std::make_shared<alien::SessionAudioClip>();
    good->sampleRate = 8000; good->channels = 1; good->frames = 80; good->left.assign(80, 0.1f); good->buildWaveform();
    std::atomic<bool> cancel{false}; juce::String error;
    require(alien::writeClipWav(root.getChildFile("moved/kick.wav"), *good, cancel, error), "write a real WAV");
    const auto decoded = alien::decodeFirstCandidate(index.candidates("samples/kick.wav"));
    require(decoded.clip != nullptr && decoded.file == root.getChildFile("moved/kick.wav"), "undecodable candidates are skipped");
    require(alien::decodeFirstCandidate({}).clip == nullptr, "no candidates, no audio");
    root.deleteRecursively();
}

// A moved or deleted audio file used to make the whole project refuse to open.
static void missingAudioTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienStudioMissingAudio", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");
    std::atomic<bool> cancel{false}; juce::String error;
    auto makeClip = [](float level) {
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = 8000; clip->channels = 1; clip->frames = 80; clip->left.assign(80, level); clip->buildWaveform();
        return clip;
    };
    const auto loop = root.getChildFile("samples/sub/loop.wav"), kick = root.getChildFile("samples/kick.wav");
    require(loop.getParentDirectory().createDirectory().wasOk(), "sample folders");
    require(alien::writeClipWav(loop, *makeClip(0.2f), cancel, error) && alien::writeClipWav(kick, *makeClip(0.3f), cancel, error), "write samples");
    const auto song = root.getChildFile("Song.alienstudio");
    auto waitIdle = [](alien::ProjectController& c) { for (int i = 0; i < 400 && c.isBusy(); ++i) pump(10); require(!c.isBusy(), "controller finishes"); };

    {
        ToneEngine engine; alien::ProjectController c(engine);
        require(c.installAudio(loop, 1, 2, makeClip(0.2f)), "install clip");
        c.loadDrum(kick, 2, 3); waitIdle(c);
        require(c.document().tracks[2].drumAssets[3] != nullptr, "drum pad loaded");
        c.setClipPattern(0, 0, patternWith(65), false); c.setBpm(133); c.commit("Edit");
        bool done = false, ok = false;
        c.writeProject(song, false, [&](bool r) { ok = r; done = true; });
        for (int i = 0; i < 200 && !done; ++i) pump(10);
        require(done && ok, "initial save");
    }
    // The sample folder is moved away after the project was saved.
    const auto moved = root.getChildFile("moved");
    require(root.getChildFile("samples").moveFileTo(moved), "move samples away");
    const auto elsewhere = root.getChildFile("empty");
    require(elsewhere.createDirectory().wasOk(), "empty search folder");

    ToneEngine engine; alien::ProjectController c(engine);
    juce::String lastStatus, lastTip;
    c.onProjectStatus = [&](const juce::String& text, const juce::String& tip) { lastStatus = text; lastTip = tip; };
    c.openProject(song); waitIdle(c);
    require(c.currentFile() == song && c.document().bpm == 133 && c.document().clip(0, 0).notes().count == 1,
        "the project opens even though audio is missing, with everything else intact");
    const auto missing = c.missingMedia();
    require(missing.size() == 2 && !missing[0].drumPad && missing[0].track == 1 && missing[0].slot == 2
        && missing[0].fileName() == "loop.wav" && missing[1].drumPad && missing[1].track == 2 && missing[1].slot == 3,
        "missing clip and drum pad are listed");
    const auto& offline = c.document().clip(1, 2);
    require(offline.audioOffline() && !offline.hasAudio() && offline.isAudioClip() && !offline.isEmpty() && offline.audioFileName == "loop.wav",
        "the clip is offline but keeps its reference and is not treated as empty or MIDI");
    require(lastStatus.contains("offline") && lastTip.contains("loop.wav") && lastTip.contains("kick.wav"), "the user is told what is offline");
    require(c.removeAudio(0, 0) == false, "an ordinary MIDI clip has no audio to remove");

    // Saving keeps the references so nothing is lost; a portable save cannot copy what is missing and says so.
    bool done = false, ok = false;
    const auto resaved = root.getChildFile("Resaved.alienstudio");
    c.writeProject(resaved, false, [&](bool r) { ok = r; done = true; });
    for (int i = 0; i < 200 && !done; ++i) pump(10);
    alien::SessionProjectData reread;
    require(done && ok && alien::parseSessionProject(resaved.loadFileAsString(), reread, error)
        && reread.clips[2][1].audioFilePath.endsWith("loop.wav") && reread.drumPaths[2][3].endsWith("kick.wav"),
        "saving a project with offline audio keeps the file references");
    done = false;
    c.writeProject(root.getChildFile("Portable.alienstudio"), true, [&](bool r) { ok = r; done = true; });
    for (int i = 0; i < 200 && !done; ++i) pump(10);
    require(done && !ok && lastStatus.contains("Missing audio"), "a portable save reports the missing file instead of dropping it");

    // Searching the wrong folder changes nothing.
    c.relinkMissingAudio(elsewhere); waitIdle(c);
    require(c.missingMedia().size() == 2 && lastStatus.contains("Relinked 0 of 2"), "nothing is relinked from a folder without the files");
    const auto undoDepthMarker = c.undoLabel();

    // Searching the folder that now holds them restores both in one undoable step.
    c.relinkMissingAudio(moved); waitIdle(c);
    require(c.missingMedia().empty() && lastStatus.contains("Relinked 2 of 2"), "both files are found by name");
    require(c.document().clip(1, 2).hasAudio() && !c.document().clip(1, 2).audioOffline()
        && c.document().clip(1, 2).audioFilePath == moved.getChildFile("sub/loop.wav").getFullPathName()
        && c.document().tracks[2].drumAssets[3] != nullptr
        && c.document().tracks[2].drumPaths[3] == moved.getChildFile("kick.wav").getFullPathName(), "clip and drum pad point at the found files");
    require(engine.drumKits[2].samples[3].load() == c.document().tracks[2].drumAssets[3].get(), "the drum sampler is given the relinked sample");
    require(c.undoLabel() == "Relink audio" && c.isDirty(), "relinking is one undoable, unsaved edit");
    require(c.undo() && c.missingMedia().size() == 2 && c.undoLabel() == undoDepthMarker, "undo returns both to offline");
    require(c.redo() && c.missingMedia().empty(), "redo brings them back");
    c.relinkMissingAudio(moved); waitIdle(c);
    require(lastStatus == "No audio is missing." || c.missingMedia().empty(), "relinking with nothing missing is harmless");

    c.discardRecovery();
    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: missing audio opens offline, keeps references, and relinks by name\n";
}

// The timeline inside the controller: refused while busy, checked against the real project, lengths
// that follow the tempo, and clips that stay attached to the right track, scene and pattern.
static void timelineIntegrationTests() {
    using namespace alien::timeline;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioTimelineCtl", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");
    std::atomic<bool> cancel{false}; juce::String error;
    auto makeClip = [](int rate, std::size_t frames) {
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = rate; clip->channels = 1; clip->frames = frames; clip->left.assign(frames, 0.25f); clip->buildWaveform();
        return clip;
    };
    auto waitIdle = [](alien::ProjectController& c) { for (int i = 0; i < 400 && c.isBusy(); ++i) pump(10); require(!c.isBusy(), "controller finishes"); };
    auto closeTo = [](double a, double b, const char* message) { require(std::abs(a - b) < 1.0e-9, message); };

    {   // Nothing on the timeline changes while a file is loading.
        const auto wav = root.getChildFile("busy.wav");
        require(alien::writeClipWav(wav, *makeClip(8000, 80), cancel, error), "write a sample");
        ToneEngine engine; alien::ProjectController c(engine); c.pushDocumentToEngine();
        c.importAudio(wav, 0, 0);
        require(c.isBusy(), "an import is under way");
        require(!c.placeRegion(0, 0, 0) && c.timeline().regions.empty(), "the timeline cannot be edited while a file is loading");
        waitIdle(c);
        require(c.placeRegion(0, 0, 0) && c.timeline().regions.size() == 1, "and can be once it has finished");
    }
    {   // Only what the project has can be placed, and an edit that changes nothing is not an edit.
        ToneEngine engine; alien::ProjectController c(engine); c.pushDocumentToEngine();
        const auto label = c.undoLabel();
        require(c.document().trackCount() == 4 && c.document().sceneCount() == 4 && c.channelRack().patternCount == 1, "a new project's size");
        require(!c.placeRegion(5, 0, 0), "no clip on a track the project does not have");
        require(!c.placeRegion(0, 9, 0), "no clip from a scene the project does not have");
        require(!c.placeRegion(rackRowBase, 3, 0), "no pattern the rack does not have");
        require(c.timeline().regions.empty() && c.undoLabel() == label, "and nothing was recorded");
        require(!c.editTimeline("Nothing", [](Timeline&) { return true; }), "an edit that leaves the timeline as it was reports no change");
    }
    {   // Audio lasts as long as the project tempo says, rounded up to the next sixteenth, and trims by that tempo too.
        const auto wav = root.getChildFile("tempo.wav");
        require(alien::writeClipWav(wav, *makeClip(8000, 10400), cancel, error), "write a 1.3 second sample");
        ToneEngine engine; alien::ProjectController c(engine); c.pushDocumentToEngine();
        require(c.installAudio(wav, 1, 0, makeClip(8000, 10400)), "install the audio");
        c.setBpm(100); c.commit("Tempo");
        require(c.naturalLength(1, 0) == 9 * ticksPerStep, "1.3 seconds at 100 BPM is 2080 ticks, rounded up to 9 sixteenths");
        std::uint32_t id = 0;
        require(c.placeRegion(1, 0, 0, &id) && c.timeline().find(id) != nullptr && c.timeline().find(id)->length == 9 * ticksPerStep, "a placed audio clip has that length");
        require(c.editTimeline("Trim", [&](Timeline& t) { return t.trimStart(id, 480); }), "trim the front");
        closeTo(c.timeline().find(id)->audioOffset, 0.3, "480 ticks at 100 BPM is 0.3 seconds of the audio");
        require(c.naturalLength(rackRowBase, 0) == 16 * ticksPerStep, "a rack pattern lasts its length in steps");
    }
    {   // Clips stay attached to their track, scene and pattern as those are moved, added and deleted.
        ToneEngine engine; alien::ProjectController c(engine); c.pushDocumentToEngine();
        std::uint32_t a = 0, b = 0, p0 = 0, p1 = 0, p2 = 0;
        require(c.placeRegion(2, 1, 0, &a) && c.placeRegion(3, 2, ticksPerBar, &b), "two clips");
        require(c.editRack("Two more patterns", [](alien::rack::Project& p) { return p.addPattern() && p.addPattern(); }), "three patterns");
        require(c.placeRegion(rackRowBase, 0, 0, &p0) && c.placeRegion(rackRowBase + 1, 1, 0, &p1) && c.placeRegion(rackRowBase + 2, 2, 0, &p2), "three patterns on three lanes");
        auto rowOf = [&](std::uint32_t id) { const auto* r = c.timeline().find(id); return r != nullptr ? r->row : -1; };
        auto sourceOf = [&](std::uint32_t id) { const auto* r = c.timeline().find(id); return r != nullptr ? r->source : -1; };

        require(c.moveTrack(2, 1) && rowOf(a) == 3 && rowOf(b) == 2, "clips follow their tracks when two tracks swap places");
        require(c.duplicateScene(0) && sourceOf(a) == 2 && sourceOf(b) == 3, "a scene added in front pushes later clips along");
        require(c.moveScene(3, -1) && sourceOf(a) == 3 && sourceOf(b) == 2, "clips follow their scenes when two scenes swap places");
        require(c.deleteScene(2) && c.timeline().find(b) == nullptr && sourceOf(a) == 2, "deleting a scene removes its clips and renumbers the rest");
        require(c.deleteTrack(0) && rowOf(a) == 2, "deleting a track renumbers the tracks after it");
        require(c.deleteTrack(2) && c.timeline().find(a) == nullptr, "and removes the clips of the track that went");
        require(c.deleteRackPattern(1) && c.timeline().find(p1) == nullptr && sourceOf(p2) == 1 && sourceOf(p0) == 0,
                "deleting a pattern removes its clips and renumbers the patterns after it");
        require(c.timeline().valid(c.document().trackCount(), c.document().sceneCount(), c.channelRack().patternCount), "the timeline is still valid for the project");
        require(c.editRack("Remove a pattern", [](alien::rack::Project& p) { return p.deletePattern(1); }), "the rack loses another pattern");
        require(c.timeline().find(p2) == nullptr && c.timeline().find(p0) != nullptr
                && c.timeline().valid(c.document().trackCount(), c.document().sceneCount(), c.channelRack().patternCount),
                "a clip of a pattern that no longer exists is dropped, not left pointing at nothing");
    }
    {   // A clip whose audio file is missing plays nothing, not the notes it also happens to hold.
        const auto wav = root.getChildFile("offline.wav"), song = root.getChildFile("Offline.alienstudio");
        require(alien::writeClipWav(wav, *makeClip(8000, 80), cancel, error), "write a sample");
        {
            ToneEngine engine; alien::ProjectController c(engine);
            c.setClipPattern(0, 0, patternWith(65), false);
            c.setClipPattern(1, 1, patternWith(67), false);
            require(c.installAudio(wav, 0, 0, makeClip(8000, 80)), "install the audio on a clip that has notes");
            c.commit("Edit");
            bool done = false, ok = false;
            c.writeProject(song, false, [&](bool r) { ok = r; done = true; });
            for (int i = 0; i < 200 && !done; ++i) pump(10);
            require(done && ok, "save");
            const auto live = c.captureArrangement();
            require(live->clips[0][0].audio != nullptr && live->clips[1][1].pattern.count == 1, "the live song has the audio and the MIDI clip");
        }
        require(wav.deleteFile(), "the audio goes missing");
        ToneEngine engine; alien::ProjectController c(engine);
        c.openProject(song); waitIdle(c);
        require(c.document().clip(0, 0).audioOffline() && c.document().clip(0, 0).notes().count == 1, "the clip is offline and still holds its notes");
        const auto captured = c.captureArrangement();
        require(captured->clips[0][0].audio == nullptr && captured->clips[0][0].pattern.count == 0, "but the song plays nothing for it");
        require(captured->clips[1][1].pattern.count == 1, "an ordinary MIDI clip keeps its notes");
        c.discardRecovery();
    }
    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: timeline in the controller\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    masterVolumeTests();
    mediaIndexTests();
    missingAudioTests();
    timelineIntegrationTests();
    arrangementPublishTests();
    structureTests();
    fileFormatTests();
    undoTests();
    saveAndAutosaveTests();
    recoveryAndPortableRegressionTests();
    return 0;
}
