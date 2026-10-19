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
    object->removeProperty("plugins");object->removeProperty("masterPlugins");object->removeProperty("drumPaths");object->removeProperty("arrangement");
    for(auto& value:*object->getProperty("trackSettings").getArray()) value.getDynamicObject()->removeProperty("instrument");
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(juce::JSON::toString(legacy),loaded,error),"redesign v4 with no drum fields migrates");
    auto combined=alien::toProjectData(c.document());combined.drumPaths[0][2]=collected;combined.trackSettings[0].instrument=1;
    require(combined.arrangement.add({3,3,0,4}),"add legacy region");
    auto v5=juce::JSON::parse(alien::serializeSessionProject(combined));v5.getDynamicObject()->setProperty("version",5);
    require(alien::parseSessionProject(juce::JSON::toString(v5),loaded,error) && loaded.drumPaths[0][2]==collected && loaded.arrangement.count==1,"drum/arrangement v5 migrates");
    c.discardRecovery();alien::ProjectController::recoveryDirectoryOverride()={};root.deleteRecursively();
    std::cout<<"PASS: recovery fencing, portable undo/redo and both legacy schemas\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    structureTests();
    fileFormatTests();
    undoTests();
    saveAndAutosaveTests();
    recoveryAndPortableRegressionTests();
    return 0;
}
