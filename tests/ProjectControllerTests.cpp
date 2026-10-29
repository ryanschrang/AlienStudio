#include "ProjectController.h"
#include "AudioFileWrite.h"
#include <algorithm>
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
    for (const auto& scene : d.cells) require(static_cast<int>(scene.size()) == d.trackCount(), "every scene has a clip per track");

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
    { auto clip = doc.clip(8, 10); clip.pattern = std::make_shared<const alien::PianoRollPattern>(patternWith(50)); doc.setClip(8, 10, clip); }
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

    // Clip notes are shared, never copied, on the way to and from the file.
    const auto data = alien::toProjectData(doc);
    require(data.clips[10][8].pattern.shared() == doc.clip(8, 10).pattern && data.clips[0][0].pattern.shared() == doc.clip(0, 0).pattern,
            "save data shares each clip's notes with the document");
    std::size_t kept = 0;
    for (const auto& scene : parsed.clips) for (const auto& clip : scene) kept += clip.pattern.shared() != nullptr ? 1u : 0u;
    require(kept == 1 && parsed.clips[10][8].pattern->count == 1 && parsed.clips[0][0].pattern->count == 0
            && parsed.clips[0][0].pattern->loopSteps == alien::PianoRollPattern::defaultLoopSteps,
            "a loaded project keeps notes only for the clip that has some; empty clips read as empty");
    require(back.clip(0, 0).pattern == alien::emptyPattern() && back.clip(3, 7).pattern == alien::emptyPattern(),
            "and the document's empty clips all share the one empty pattern");
    {   // An empty clip with its own loop length is kept: it is not the default.
        auto shortLoop = alien::ProjectDocument::createDefault(1, 1);
        alien::PianoRollPattern p; p.loopSteps = 32;
        { auto clip = shortLoop.clip(0, 0); clip.pattern = std::make_shared<const alien::PianoRollPattern>(p); shortLoop.setClip(0, 0, clip); }
        alien::SessionProjectData again;
        require(alien::parseSessionProject(alien::serializeSessionProject(alien::toProjectData(shortLoop)), again, error)
                && again.clips[0][0].pattern.shared() != nullptr && again.clips[0][0].pattern->loopSteps == 32,
                "an empty clip with a 32-step loop keeps its loop length");
    }

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

static void tempoMapControllerTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    constexpr auto bar = alien::timeline::ticksPerBar;
    const auto published = [&] { return engine.arrangementMailbox.publishedCount(); };
    const auto base = published();
    require(c.setTempoChange(2 * bar, 60.0) && c.undoLabel() == "Tempo change" && published() == base + 1,
        "a tempo change is one undo step and republishes the song");
    require(!c.setTempoChange(0, 90.0) && !c.setTempoChange(bar, 300.0) && !c.setTempoChange(2 * bar, 60.0),
        "tick 0, out-of-range tempos and repeats are refused");
    require(c.moveTempoChange(2 * bar, bar) && c.timeline().tempoAt(bar) == 60.0 && c.undoLabel() == "Move tempo change", "a change moves");
    require(c.undo() && c.timeline().tempoAt(bar) == 120.0 && c.timeline().tempoAt(2 * bar) == 60.0, "and the move undoes");
    require(c.removeTempoChange(2 * bar) && c.timeline().tempoChanges.empty(), "a change is removed");
    require(c.undo() && c.timeline().tempoChanges.size() == 1, "and the removal undoes");
    require(c.redo() && c.timeline().tempoChanges.empty() && c.undo(), "redo removes it again");
    c.setBpm(100); c.commit("Tempo");
    require(c.songTempo().tempoAt(0) == 100.0 && c.songTempo().tempoAt(2 * bar) == 60.0,
        "the project tempo plays until the first change");
    require(c.clearTempoChanges() && c.timeline().tempoChanges.empty() && !c.clearTempoChanges(), "all changes can be cleared");
    alien::SessionProjectData saved = alien::toProjectData(c.document());
    require(c.undo() && alien::toProjectData(c.document()).timeline.tempoChanges.size() == 1 && saved.timeline.tempoChanges.empty(),
        "the tempo map is part of the saved project");
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
        require(!c.deleteRackPattern(1) && c.timeline().find(p1) != nullptr,
                "a bank source with explicit Song uses is protected");
        require(c.editTimeline("Remove placement", [&](Timeline& t) { return t.remove({p1}); }) && c.deleteRackPattern(1)
                && sourceOf(p2) == 1 && sourceOf(p0) == 0, "after clearing its uses, deletion remaps other sources by identity");
        require(c.timeline().valid(c.document().trackCount(), c.document().sceneCount(), c.channelRack().patternCount), "the timeline is still valid for the project");
        require(!c.editRack("Remove a pattern", [](alien::rack::Project& p) { return p.deletePattern(1); }), "generic rack edits also protect used sources");
        require(c.editTimeline("Remove placement", [&](Timeline& t) { return t.remove({p2}); })
            && c.editRack("Remove a pattern", [](alien::rack::Project& p) { return p.deletePattern(1); }), "clear the placement before removing its bank source");
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
            require(live->clips[0][0].audio != nullptr && live->clips[1][1].pattern->count == 1, "the live song has the audio and the MIDI clip");
        }
        require(wav.deleteFile(), "the audio goes missing");
        ToneEngine engine; alien::ProjectController c(engine);
        c.openProject(song); waitIdle(c);
        require(c.document().clip(0, 0).audioOffline() && c.document().clip(0, 0).notes().count == 1, "the clip is offline and still holds its notes");
        const auto captured = c.captureArrangement();
        require(captured->clips[0][0].audio == nullptr && captured->clips[0][0].pattern->count == 0, "but the song plays nothing for it");
        require(captured->clips[1][1].pattern->count == 1, "an ordinary MIDI clip keeps its notes");
        require(captured->clips[1][1].pattern.shared() == c.document().clip(1, 1).pattern,
                "the song snapshot shares the clip's notes with the document instead of copying them");
        c.discardRecovery();
    }
    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: timeline in the controller\n";
}


static void automationUndoTests() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController controller(*engine);
    controller.pushDocumentToEngine();
    require(controller.editTimeline("Automation point", [](alien::timeline::Timeline& t) {
        t.automationLanes.push_back({0, alien::automation::Parameter::volume, true, {{0, 0.25f}, {960, 1.0f}}});
        return true;
    }), "automation is an undoable timeline edit");
    require(controller.captureArrangement()->timeline.automationLanes.size() == 1, "automation reaches playback snapshot");
    require(controller.undo() && controller.timeline().automationLanes.empty(), "undo removes the automation lane");
    require(controller.redo() && controller.timeline().automationLanes.size() == 1, "redo restores the automation lane");
    const auto lanes = controller.timeline().automationLanes;
    const int tracks = controller.document().trackCount();
    require(controller.deleteTrack(0) && controller.timeline().automationLanes.empty(), "track deletion removes automation");
    // Structural controller methods mutate the document; their caller commits
    // the completed operation, unlike editTimeline which commits internally.
    controller.commit("Delete track");
    require(controller.undo() && controller.document().trackCount() == tracks
        && controller.timeline().automationLanes == lanes, "undo track deletion restores exact automation");
    require(controller.captureArrangement()->timeline.automationLanes == lanes, "restored automation reaches playback");
    require(controller.redo() && controller.document().trackCount() == tracks - 1
        && controller.timeline().automationLanes.empty(), "redo track deletion removes automation again");
}

static void parameterAutomationUndoTests() {
    auto engine = std::make_unique<ToneEngine>(); alien::ProjectController c(*engine);
    auto data = alien::toProjectData(c.document());
    data.plugins[0].effects = {{"<PLUGIN name=\"Effect\"/>", {}, false}};
    c.setPluginRacks(data.plugins, data.masterPlugins);
    const auto id = c.document().tracks[0].plugins.effects[0].instanceId;
    require(c.editTimeline("Parameter lane", [=](alien::timeline::Timeline& t) {
        t.parameterLanes.push_back({{id, "cutoff"}, true, {{0, 0.25f}, {960, 0.75f}}}); return true;
    }), "parameter lanes use document undo history");
    const auto lanes = c.timeline().parameterLanes;
    require(c.captureArrangement()->timeline.parameterLanes == lanes, "parameter lanes reach arrangement snapshots");
    require(c.undo() && c.timeline().parameterLanes.empty(), "undo removes parameter lane");
    require(c.redo() && c.timeline().parameterLanes == lanes, "redo restores parameter lane exactly");
    data.plugins[0].effects.clear();
    c.setPluginRacks(data.plugins, data.masterPlugins);
    require(c.timeline().parameterLanes == lanes, "removal keeps orphan automation");
    require(c.undo() && c.document().tracks[0].plugins.effects[0].instanceId == id
        && c.timeline().parameterLanes == lanes, "undo removal restores original plugin identity and lane");
}

// T1-03 increment B1: the clip library owns every Session cell's content.
static void clipLibraryOwnershipTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    // The only clip in a new project is the Channel Rack's empty pattern (T1-03 increment C).
    require(d.library.clipCount() == 1 && !d.cellClip(0, 0) && d.clip(0, 0).isEmpty(), "a new project has empty cells and no Session clips");
    // A cell gets a clip when it gets content; later edits are new revisions of the same clip.
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    const auto first = d.cellClip(0, 0);
    const auto pattern = d.library.clip(first)->pattern;
    require(first && d.library.clipCount() == 2 && d.clip(0, 0).notes().notes[0].pitch == 60, "the cell's notes live in the library");
    c.setClipPattern(0, 0, patternWith(62), false); c.commit("Notes", 0, 0);
    require(d.cellClip(0, 0) == first && d.library.clip(first)->pattern == pattern && d.library.pattern(pattern)->revision == 2
            && d.clip(0, 0).notes().notes[0].pitch == 62, "an edit is a new revision of the same clip, not a new clip");
    // Undo and redo restore the revision.
    require(c.undo() && d.clip(0, 0).notes().notes[0].pitch == 60 && d.library.pattern(pattern)->revision == 1, "undo restores the earlier revision");
    require(c.redo() && d.clip(0, 0).notes().notes[0].pitch == 62, "redo the later one");
    // Duplicating a scene copies its clips independently.
    c.setClipPattern(1, 0, patternWith(48), false); c.commit("Notes", 1, 0);
    require(c.duplicateScene(0), "duplicate the scene");
    c.commit("Duplicate scene");
    const auto copy = d.cellClip(0, 1);
    require(copy && copy != first && !d.library.linked(first, copy) && d.clip(0, 1).notes().notes[0].pitch == 62 && d.cellClip(2, 1) == alien::library::ClipId{},
            "the copied scene has clips of its own with the same notes, and empty cells stay empty");
    c.setClipPattern(0, 1, patternWith(70), false); c.commit("Notes", 0, 1);
    require(d.clip(0, 0).notes().notes[0].pitch == 62 && d.clip(0, 1).notes().notes[0].pitch == 70, "editing the copy leaves the original alone");
    // Moving scenes and tracks moves clips with their cells; deleting a scene drops its clips.
    require(c.moveScene(1, -1) && d.cellClip(0, 0) == copy && d.cellClip(0, 1) == first, "moving a scene moves its clips");
    c.commit("Move scene");
    require(c.moveTrack(0, 1) && d.cellClip(1, 1) == first && d.clip(1, 1).notes().notes[0].pitch == 62, "moving a track moves its clips");
    c.commit("Move track");
    const auto clips = d.library.clipCount();
    require(c.deleteScene(0) && d.library.clipCount() == clips - 2 && d.library.clip(copy) == nullptr && d.library.valid(),
            "deleting a scene drops the clips only it used");
    c.commit("Delete scene");
    require(c.undo() && d.library.clip(copy) != nullptr && d.cellClip(1, 0) == copy, "undo brings them back with the same ids");
    // Clearing a cell drops its clip; an empty clip with its own loop length is kept.
    alien::PianoRollPattern longLoop; longLoop.loopSteps = 32;
    c.setClipPattern(3, 0, longLoop, false);
    require(d.cellClip(3, 0) && d.clip(3, 0).notes().loopSteps == 32 && d.clip(3, 0).isEmpty(), "an empty 32-step clip is still a clip");
    c.setClipPattern(3, 0, alien::PianoRollPattern{}, false);
    require(!d.cellClip(3, 0) && d.library.valid(), "a default empty clip empties the cell");
    // Audio: one media entry per file a clip plays, dropped with the last clip that plays it.
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 800; audio->left.assign(800, 0.25f); audio->buildWaveform();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("clip-library-test.wav");
    require(c.installAudio(file, 2, 0, audio) && d.clip(2, 0).audio == audio && d.clip(2, 0).audioFilePath == file.getFullPathName()
            && d.media.size() == 1, "imported audio is a media entry its clip plays");
    require(c.removeAudio(2, 0) && d.media.empty() && !d.clip(2, 0).isAudioClip(), "removing it drops the media entry");
    // Saving writes each cell's content; opening gives every non-empty cell its own clip.
    c.setClipPattern(2, 1, patternWith(62), false);
    auto reopened = alien::fromProjectData(alien::toProjectData(d));
    require(alien::sameContent(reopened, d) || [&] {
                for (int s = 0; s < d.sceneCount(); ++s) for (int t = 0; t < d.trackCount(); ++t)
                    if (!alien::samePianoRollPattern(d.clip(t, s).notes(), reopened.clip(t, s).notes())) return false;
                return true; }(), "a save round trip keeps every cell's content");
    require(reopened.cellClip(1, 1) && reopened.cellClip(2, 1) && reopened.cellClip(1, 1) != reopened.cellClip(2, 1)
            && !reopened.library.linked(reopened.cellClip(1, 1), reopened.cellClip(2, 1)) && reopened.library.valid(),
            "cells with equal notes open as separate, unlinked clips");
    std::puts("PASS: the clip library owns Session cells");
}

// T1-03 increment B2a: cells that share one clip's content, saved from format 22.
static void linkedClipTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    const auto pitch = [&](int t, int s) { return d.clip(t, s).notes().count != 0 ? d.clip(t, s).notes().notes[0].pitch : -1; };
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    require(!c.canPasteLinked() && !c.pasteCell(1, 1, true) && c.copyCell(0, 0) && c.canPasteLinked(), "Copy enables the pastes");
    // Paste linked: a new clip sharing the content.
    require(c.pasteCell(1, 1, true) && c.undoLabel() == "Paste linked clip", "Paste linked is one undo step");
    require(d.cellClip(1, 1) && d.cellClip(1, 1) != d.cellClip(0, 0) && d.linkOf(1, 1) == d.linkOf(0, 0) && d.linkedCells(0, 0) == 2 && pitch(1, 1) == 60,
            "the pasted cell has a clip of its own that shares the content");
    require(!c.pasteCell(1, 1, true), "pasting the same link again changes nothing");
    c.setClipPattern(1, 1, patternWith(64), false); c.commit("Notes", 1, 1);
    require(pitch(0, 0) == 64 && pitch(1, 1) == 64 && d.linkedCells(0, 0) == 2, "editing either cell edits both");
    // Paste a copy: independent.
    require(c.pasteCell(2, 0, false) && c.undoLabel() == "Paste clip" && pitch(2, 0) == 60 && d.linkOf(2, 0) != d.linkOf(0, 0),
            "Paste is an independent copy of what was copied (as it was when copied)");
    c.setClipPattern(2, 0, patternWith(48), false); c.commit("Notes", 2, 0);
    require(pitch(0, 0) == 64 && pitch(2, 0) == 48, "editing the copy leaves the original alone");
    // A third link, then Make unique forks just one cell.
    require(c.pasteCell(3, 3, true) && d.linkedCells(0, 0) == 3, "three cells linked");
    require(c.makeCellUnique(1, 1) && c.undoLabel() == "Make clip unique" && d.linkedCells(1, 1) == 1 && d.linkedCells(0, 0) == 2 && pitch(1, 1) == 64,
            "Make unique gives one cell its own copy; the others stay linked");
    require(!c.makeCellUnique(1, 1), "a cell that shares nothing is already unique");
    c.setClipPattern(1, 1, patternWith(72), false); c.commit("Notes", 1, 1);
    require(pitch(0, 0) == 64 && pitch(3, 3) == 64, "its edits are its own");
    // Clearing a linked cell leaves the others their content.
    require(c.clearCell(3, 3) && !d.cellClip(3, 3) && pitch(0, 0) == 64 && d.linkedCells(0, 0) == 1, "clearing one linked cell empties only that cell");
    c.commit("Clear clip", 3, 3);
    require(c.undo() && d.linkedCells(0, 0) == 2 && pitch(3, 3) == 64, "undo brings the link back");
    // Linking two cells that already hold equal notes is still an edit.
    c.setClipPattern(2, 3, patternWith(64), false); c.commit("Notes", 2, 3);
    require(c.copyCell(0, 0) && c.pasteCell(2, 3, true) && c.undoLabel() == "Paste linked clip" && d.linkedCells(0, 0) == 3,
            "linking equal content is an undoable edit");
    // Saving: links survive, written only where content is shared, from format 22.
    const auto data = alien::toProjectData(d);
    require(data.clips[0][0].link != 0 && data.clips[0][0].link == data.clips[3][3].link && data.clips[1][1].link == 0 && data.valid(),
            "linked cells save one link; unshared cells none");
    const auto json = alien::serializeSessionProject(data);
    alien::SessionProjectData parsed; juce::String error;
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 22 && alien::parseSessionProject(json, parsed, error), "written and read as format 22");
    auto reopened = alien::fromProjectData(parsed);
    require(reopened.linkedCells(0, 0) == 3 && reopened.linkOf(3, 3) == reopened.linkOf(0, 0) && reopened.linkedCells(1, 1) == 1 && reopened.library.valid(),
            "links survive a save and reopen");
    auto edited = reopened.clip(3, 3); edited.pattern = std::make_shared<const alien::PianoRollPattern>(patternWith(50)); reopened.setClip(3, 3, edited);
    require(reopened.clip(0, 0).notes().notes[0].pitch == 50 && reopened.clip(2, 3).notes().notes[0].pitch == 50, "and still edit together");
    // Damaged and older files.
    auto broken = juce::JSON::parse(json);
    auto* scenes = broken.getProperty("scenes", {}).getArray();
    scenes->getReference(3).getArray()->getReference(3).getDynamicObject()->setProperty("loopSteps", 32);
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "linked cells with different content are rejected");
    broken = juce::JSON::parse(json);
    broken.getProperty("scenes", {}).getArray()->getReference(0).getArray()->getReference(0).getDynamicObject()->setProperty("link", "zz");
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "a link that is not an id is rejected");
    auto old = juce::JSON::parse(json);
    old.getDynamicObject()->setProperty("version", 21);
    for (auto& scene : *old.getProperty("scenes", {}).getArray()) for (auto& cell : *scene.getArray()) cell.getDynamicObject()->removeProperty("link");
    require(alien::parseSessionProject(juce::JSON::toString(old), parsed, error) && alien::fromProjectData(parsed).linkedCells(0, 0) == 1,
            "a format 21 project opens with every cell its own");
    std::puts("PASS: linked Session clips");
}

static void placementUniqueTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    using alien::timeline::ticksPerBar;
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    std::uint32_t first = 0, second = 0;
    require(c.placeRegion(0, 0, 0, &first) && c.placeRegion(0, 0, 4 * ticksPerBar, &second), "two placements of one cell");
    require(c.placementShared(first) && c.placementShared(second), "they share what they play");
    const auto scenes = d.sceneCount();
    const auto clips = d.library.clipCount();
    require(c.makePlacementUnique(second) && c.undoLabel() == "Make placement unique", "Make placement unique is one undo step");
    const auto* own = d.timeline.find(second);
    require(own != nullptr && own->ownsClip() && own->source == -1 && d.sceneCount() == scenes && d.library.clipCount() == clips + 1
            && d.timeline.find(first)->playsCell(), "a MIDI placement gets a clip of its own, with no Session cell or new scene");
    require(d.placementContent(*own).notes().notes[0].pitch == 60 && d.placementName(own->clip) == d.scenes[0].name,
            "an independent copy of the same notes, named after the scene it came from");
    require(c.setPlacementPattern(own->clip, patternWith(70)), "the piano roll edits a placement clip");
    c.commit("Edit notes");
    require(d.clip(0, 0).notes().notes[0].pitch == 60 && d.placementContent(*d.timeline.find(second)).notes().notes[0].pitch == 70,
            "editing it leaves the cell and the other placement alone");
    require(!c.makePlacementUnique(second) && !c.placementShared(first) && !c.placementShared(second), "neither placement shares anything now");
    require(c.undo() && d.placementContent(*d.timeline.find(second)).notes().notes[0].pitch == 60, "undo takes the edit back");
    require(c.undo() && d.timeline.find(second)->playsCell() && d.timeline.find(second)->source == 0 && d.library.clipCount() == clips,
            "undo puts it back on the shared cell, and its clip is gone");
    require(c.redo() && d.timeline.find(second)->ownsClip(), "redo makes it unique again");
    std::puts("PASS: making one placement unique");
}

static void placementClipTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    using alien::timeline::ticksPerBar;
    c.setClipPattern(1, 0, patternWith(48), false); c.commit("Notes", 1, 0);
    std::uint32_t a = 0, b = 0;
    require(c.placeRegion(1, 0, 0, &a) && c.placeRegion(1, 0, 2 * ticksPerBar, &b) && c.makePlacementUnique(a), "a placement clip");
    const auto clip = d.timeline.find(a)->clip;
    const auto cell = d.cellClip(1, 0);
    require(!c.setPlacementPattern(0, patternWith(70)) && !c.setPlacementPattern(cell.value, patternWith(70))
            && d.clip(1, 0).notes().notes[0].pitch == 48, "placement edits refuse zero and Session clip ids without changing cells");
    // Copies of a placement share its clip, as copies of a cell's placement share the cell.
    require(c.editTimeline("Duplicate", [&](alien::timeline::Timeline& t) { return t.duplicate({a}); }), "duplicate the placement");
    std::uint32_t copy = 0;
    for (const auto& r : d.timeline.regions) if (r.id != a && r.clip == clip) copy = r.id;
    require(copy != 0 && c.placementShared(a) && c.placementShared(copy), "the duplicate shares the placement clip");
    require(c.setPlacementPattern(clip, patternWith(55)), "edit the shared clip");
    c.commit("Edit notes");
    require(d.placementContent(*d.timeline.find(copy)).notes().notes[0].pitch == 55 && d.clip(1, 0).notes().notes[0].pitch == 48,
            "both placements hear the edit; the cell does not");
    // The engine gets the placement clip with the song.
    const auto song = c.captureArrangement();
    require(alien::ArrangementMailbox::validSnapshot(*song) && song->placements.size() == 1
            && song->clipFor(*d.timeline.find(copy), 1).pattern->notes[0].pitch == 55
            && song->clipFor(*d.timeline.find(b), 1).pattern->notes[0].pitch == 48, "the song snapshot plays each region's own content");
    auto missing = *song; missing.placements.clear();
    require(!alien::ArrangementMailbox::validSnapshot(missing), "a snapshot without a region's placement clip is refused");
    // A placement clip belongs to no scene: deleting or moving scenes leaves it where it is.
    const auto regions = d.timeline.regions.size();
    require(c.deleteScene(0), "delete the scene the clip came from");
    c.commit("Delete scene");
    require(d.timeline.regions.size() == regions - 1 && d.timeline.find(a) != nullptr && d.timeline.find(copy) != nullptr && d.timeline.find(b) == nullptr,
            "the cell's placement goes with its scene; the placement clips stay");
    require(c.undo() && d.timeline.find(b) != nullptr, "undo brings the scene back");
    // Removing every region that plays it drops the clip; undo brings it back.
    const auto clips = d.library.clipCount();
    require(c.editTimeline("Delete", [&](alien::timeline::Timeline& t) { return t.remove({a, copy}); }) && d.library.clipCount() == clips - 1,
            "a placement clip nothing plays is dropped");
    require(c.undo() && d.library.clip(alien::library::ClipId{clip}) != nullptr && d.placementContent(*d.timeline.find(a)).notes().notes[0].pitch == 55,
            "undo restores it with its notes");
    // A paste can bring a cut clip back as a new one.
    const auto restored = c.restorePlacementClip(std::make_shared<const alien::PianoRollPattern>(patternWith(62)), "Scene 9");
    require(restored != 0 && restored != clip && d.placementName(restored) == "Scene 9", "a clip restored for a paste");
    require(!c.editTimeline("Bad", [&](alien::timeline::Timeline& t) { t.regions[0].clip = 0x7777; t.regions[0].source = -1; return true; }),
            "a region naming a clip the project does not have is refused");
    std::puts("PASS: placement clips (copies, engine, scenes, pruning)");
}

static void placementClipFileTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    using alien::timeline::ticksPerBar;
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    std::uint32_t a = 0, b = 0;
    require(c.placeRegion(0, 0, 0, &a) && c.placeRegion(0, 0, ticksPerBar, &b) && c.makePlacementUnique(a), "a placement clip");
    require(c.editTimeline("Duplicate", [&](alien::timeline::Timeline& t) { return t.duplicate({a}); }), "and a copy of it");
    require(c.setPlacementPattern(d.timeline.find(a)->clip, patternWith(66)), "with notes of its own"); c.commit("Edit notes");
    require(c.deleteReturn(1) && c.addReturn("Song echo") == 1 && c.addReturn("Extra return") == 4,
            "placement project supports renamed and extra returns");
    require(c.deleteReturn(2), "placement project supports a removed return");
    const auto data = alien::toProjectData(d);
    require(data.valid() && data.placementClips.size() == 1, "saved once, however many regions play it");
    const auto json = alien::serializeSessionProject(data);
    alien::SessionProjectData parsed; juce::String error;
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 24 && alien::parseSessionProject(json, parsed, error), "written and read as format 24 or later");
    const auto reopened = alien::fromProjectData(parsed);
    int own = 0; std::uint64_t shared = 0; bool same = true;
    for (const auto& r : reopened.timeline.regions) if (r.ownsClip()) { ++own; same = same && (shared == 0 || shared == r.clip); shared = r.clip; }
    require(own == 2 && same && reopened.placementContent(*std::find_if(reopened.timeline.regions.begin(), reopened.timeline.regions.end(),
            [](const auto& r) { return r.ownsClip(); })).notes().notes[0].pitch == 66 && reopened.placementName(shared) == "Scene 1",
            "both regions reopen sharing one clip with its notes and name");
    require(reopened.library.valid() && alien::toProjectData(reopened).valid(), "the reopened library is consistent");
    require(reopened.returns[1].used && reopened.returns[1].name == "Song echo" && !reopened.returns[2].used
            && reopened.returns[4].used && reopened.returns[4].name == "Extra return",
            "format 24 preserves renamed, removed and extra return slots alongside placement clips");
    // Damaged and older files.
    auto broken = juce::JSON::parse(json);
    broken.getProperty("placementClips", {})[0].getDynamicObject()->setProperty("id", "0000000000000000");
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "a Song clip with id 0 is rejected");
    broken = juce::JSON::parse(json);
    broken.getDynamicObject()->removeProperty("placementClips");
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "a region naming a missing Song clip is rejected");
    broken = juce::JSON::parse(json);
    broken.getProperty("placementClips", {})[0].getDynamicObject()->setProperty("audioFilePath", "C:/x.wav");
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "a Song clip with audio is rejected");
    broken = juce::JSON::parse(json);
    broken.getDynamicObject()->setProperty("version", 22);
    require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error), "Song clips in an older format are rejected");
    std::puts("PASS: placement clips save and reopen (format 24)");
}

// T1-03 increment C: the Channel Rack's patterns live in the clip library; the rack is their view.
static void rackLibraryTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    using alien::library::ClipId;
    namespace rk = alien::rack;
    const auto clipOf = [&](int slot) { return ClipId{d.channelRack.clips[static_cast<std::size_t>(slot)]}; };
    const auto revisionOf = [&](int slot) { return d.library.pattern(d.library.clip(clipOf(slot))->pattern)->revision; };
    require(clipOf(0) && d.library.clip(clipOf(0)) != nullptr && d.rackMatchesLibrary(), "a new project's pattern is a library clip");
    require(d.library.clip(clipOf(0))->info.name == "Pattern 1", "named after the pattern");

    // A step edit is a new revision of the same clip.
    const auto first = clipOf(0);
    const auto revision = revisionOf(0);
    require(c.editRack("Step", [](rk::Project& p) { return p.setStepOn(0, 0, 0, true); }), "turn a step on");
    require(clipOf(0) == first && revisionOf(0) == revision + 1 && d.rackMatchesLibrary(), "a step edit is a new revision of the pattern's clip");
    // Notes on a channel are kept as notes, with the dormant steps.
    alien::PianoRollPattern notes; notes.loopSteps = 16; notes.add(62, 1, 3, 0.7f); notes.add(65, 1, 3, 0.6f);
    require(c.setRackChannelNotes(0, 1, notes), "notes on channel 2");
    const auto content = d.library.content(first);
    require(content->lanes.size() == 2 && std::get<alien::library::StepLane>(content->lanes[1].content).notes != nullptr && d.rackMatchesLibrary(),
            "the note row is in the clip as notes");

    // A copied pattern is an independent clip.
    require(c.editRack("Copy pattern", [](rk::Project& p) { return p.addPattern(0); }), "copy the pattern");
    require(clipOf(1) && clipOf(1) != first && !d.library.linked(first, clipOf(1)) && d.library.clip(clipOf(1))->info.name == "Pattern 2",
            "the copy is a clip of its own");
    require(c.editRack("Step", [](rk::Project& p) { return p.setStepOn(1, 0, 4, true); }), "edit the copy");
    require(!d.channelRack.pattern(0).steps[0][4].on && d.rackMatchesLibrary(), "the original keeps its steps");

    // Moving a channel changes no content; removing one is an edit of every pattern that used it.
    const auto before = revisionOf(0);
    require(c.editRack("Move channel", [](rk::Project& p) { return p.moveChannel(0, 1); }), "move channel 1 down");
    require(revisionOf(0) == before && d.channelRack.pattern(0).steps[1][0].on && d.rackMatchesLibrary(), "a moved channel keeps its row and the clip its revision");
    require(c.editRack("Remove channel", [](rk::Project& p) { return p.removeChannel(1); }), "remove that channel");
    require(revisionOf(0) == before + 1 && d.library.content(first)->lanes.size() == 1 && d.rackMatchesLibrary(), "its lane is gone from the content");
    require(c.editRack("Add channel", [](rk::Project& p) { return p.addChannel("New", "builtin:kick"); }), "add a channel");
    require(!d.channelRack.pattern(0).steps[3][0].on && d.rackMatchesLibrary(), "a new channel does not inherit the removed row");

    // Deleting a pattern drops its clip; undo brings it back.
    const auto copy = clipOf(1);
    const auto copied = d.channelRack.patterns[1];
    require(c.deleteRackPattern(1) && d.library.clip(copy) == nullptr && d.channelRack.patternCount == 1, "deleting a pattern drops its clip");
    require(c.undo() && d.library.clip(copy) != nullptr && d.channelRack.pattern(1) == *copied && d.rackMatchesLibrary(), "undo restores it");

    // Save and reopen: the content comes from the library, and every pattern is its own clip again.
    const auto data = alien::toProjectData(d);
    require(data.valid() && data.channelRack.sameContent(d.channelRack), "a save writes the library's content");
    const auto json = alien::serializeSessionProject(data);
    alien::SessionProjectData parsed; juce::String error;
    require(alien::parseSessionProject(json, parsed, error), "reads back");
    const auto reopened = alien::fromProjectData(parsed);
    require(reopened.rackMatchesLibrary() && reopened.channelRack.sameContent(d.channelRack)
            && reopened.channelRack.clips[0] != 0 && reopened.channelRack.clips[0] != reopened.channelRack.clips[1], "reopens as independent library clips");

    // Ids from another document never name a clip here.
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    auto withCell = c.document();
    auto stray = withCell.channelRack; stray.clips[0] = withCell.cellClip(0, 0).value;
    stray.clips[1] = 0;
    require(withCell.setRack(stray) && withCell.clip(0, 0).notes().notes[0].pitch == 60 && withCell.rackMatchesLibrary(),
            "a rack naming a Session cell's clip gets a clip of its own, and the cell keeps its notes");
    // Nothing changes when the rack is invalid.
    auto broken = withCell.channelRack; broken.lanes[0] = 0;
    const auto clips = withCell.library.clipCount();
    require(!withCell.setRack(broken) && withCell.library.clipCount() == clips && withCell.rackMatchesLibrary(), "an invalid rack is refused");
    std::puts("PASS: Channel Rack patterns are library clips");
}

static void savedRackBindingTests() {
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    require(c.editRack("Rack source", [](alien::rack::Project& p) {
        p.setStepOn(0, 0, 0, true);
        p.setStepField(0, 0, 100, alien::rack::StepField::ratchet, 3); // dormant, beyond the loop
        alien::PianoRollPattern chord; chord.loopSteps = 16; chord.add(60, 0.5, 2); chord.add(64, 0.5, 2);
        p.setChannelNotes(0, 1, chord);
        return p.addPattern(0);
    }), "two independent rack sources");
    auto source = c.channelRack().clips[1];
    require(c.assignRackClip(0, 0, source) && c.assignRackClip(0, 1, source), "two saved assignments share one rack source");
    const auto unchanged = d;
    require(c.assignRackClip(1, 0, c.channelRack().clips[0]) && c.undo() && alien::sameContent(unchanged, d),
            "a scene can bind different sources on independent rows, with one-step undo");
    engine.patternMode.store(1); c.launchClip(3, 2);
    require(engine.requestedTrackRackClip[3].load() == alien::rack::RackPlayer::noLaunchRequest,
        "an ordinary row with no rack occurrence does not queue a spurious rack stop");
    c.launchClip(0, 0);
    require(engine.requestedTrackRackClip[0].load() == source && engine.playing.load(), "a bound Session cell launches its composite source");
    c.stopTrack(0); require(engine.requestedTrackRackClip[0].load() == 0, "the anchor row's stop controls its rack launch");
    c.launchScene(0); require(engine.sceneTrackRackClip[0].load() == source, "a scene queues its composite source with the synchronized launch");
    c.launchClip(1, 2); require(engine.sceneTrackRackClip[0].load() == source && engine.requestedRackClip.load() == alien::rack::RackPlayer::noLaunchRequest,
        "ordinary launch on an unrelated row leaves the rack alone");
    c.launchClip(0, 2); require(engine.requestedTrackRackClip[0].load() == 0, "ordinary launch on the rack's anchor row stops that instance");
    c.launchScene(2); require(engine.sceneTrackRackClip[0].load() == 0, "a scene without a rack assignment queues synchronized rack stop");
    std::uint32_t region{};
    require(c.placeRegion(0, 0, 0, &region) && d.timeline.find(region)->isPattern() && d.timeline.find(region)->rackClip == source,
            "placing a rack cell creates an explicit rack-lane Song reference");
    require(c.editTimeline("Edit with stale rack cache", [&](auto& song) {
        auto* r = song.find(region); r->source = -1; r->gain = 0.7f; return true;
    }) && d.timeline.find(region)->source == 1 && d.timeline.find(region)->rackClip == source,
        "timeline edits derive the source cache from identity, even when the cache is out of range");
    const auto beforeInvalidReference = d;
    require(!c.editTimeline("Unknown rack identity", [&](auto& song) {
        song.find(region)->rackClip = 0xffffffffffffffffULL; return true;
    }) && alien::sameContent(beforeInvalidReference, d), "unknown rack identity refuses the whole edit without index fallback");
    require(!c.editTimeline("Missing rack identity", [&](auto& song) {
        song.find(region)->rackClip = 0; return true;
    }) && alien::sameContent(beforeInvalidReference, d), "existing placements cannot discard ownership and rebind by cache");
    require(c.undo() && c.redo(), "identity-normalized timeline edits support undo and redo");
    {
        auto malformed = d;
        malformed.timeline.find(region)->rackClip = 0;
        const auto before = malformed;
        require(!malformed.setRack(malformed.channelRack) && alien::sameContent(before, malformed),
            "document rack edits cannot recover missing ownership from an index cache");
    }
    require(c.deleteRackPattern(0) && d.timeline.find(region)->source == 0 && d.timeline.find(region)->rackClip == source,
            "deleting an unused preceding slot remaps the Song cache by identity");
    require(!c.deleteRackPattern(0), "a source with Session/Song uses cannot be deleted");
    require(c.editRack("Shared edit", [](alien::rack::Project& p) { return p.setStepOn(0, 0, 1, true); }), "edit shared rack source");
    require(d.clip(0, 0).rackClip == d.clip(0, 1).rackClip && d.timeline.find(region)->rackClip == source && d.rackMatchesLibrary(),
            "Create, Session and Song retain one content owner");
    require(c.copyCell(0, 0) && c.pasteCell(1, 2, false), "independent rack paste");
    const auto independent = d.clip(1, 2).rackClip;
    require(independent != source && c.editRack("Independent edit", [](alien::rack::Project& p) { return p.setStepOn(1, 0, 1, false); })
        && c.channelRack().pattern(0).steps[0][1].on && !c.channelRack().pattern(1).steps[0][1].on,
        "an independent copy changes only its own content");
    c.setClipPattern(2, 0, patternWith(66), false); c.commit("Ordinary notes");
    require(c.duplicateScene(0), "duplicate scene with a composite assignment"); c.commit("Duplicate scene");
    const auto sceneCopy = d.clip(0, 1).rackClip;
    require(sceneCopy != source && c.rackPatternForClip(sceneCopy) >= 0 && d.clip(2, 1).notes().count == 1
        && d.clip(2, 1).notes().notes[0].pitch == 66 && c.undo() && c.redo(),
            "scene copies get independent rack sources, with undo and redo");
    require(c.makePlacementUnique(region) && d.timeline.find(region)->rackClip != source && c.undo()
        && d.timeline.find(region)->rackClip == source, "Song Make unique forks the composite source in one undo step");
    const auto data = alien::toProjectData(d);
    const auto json = alien::serializeSessionProject(data);
    alien::SessionProjectData parsed; juce::String error;
    require(data.valid() && !json.isEmpty() && alien::parseSessionProject(json, parsed, error), "format 26 rack bindings round trip");
    require(static_cast<int>(juce::JSON::parse(json).getProperty("version", 0)) >= 26, "rack bindings use format 26 or later");
    const auto reopened = alien::fromProjectData(parsed);
    const auto reopenedSource = reopened.clip(0, 0).rackClip;
    require(reopenedSource != 0 && reopened.clip(0, 2).rackClip == reopenedSource
        && reopened.timeline.regions[0].rackClip == reopenedSource && reopened.clip(1, 3).rackClip != reopenedSource
        && reopened.rackMatchesLibrary() && alien::toProjectData(reopened).valid(), "saved references rebind together; copies stay independent");
    require(reopened.channelRack.lanes == data.channelRack.lanes && reopened.channelRack.pattern(0).steps[0][100].ratchet == 3
        && reopened.channelRack.pattern(0).notes[1]->count == 2, "lane identities, dormant steps and full note rows survive");
    const auto beforeDamage = alien::serializeSessionProject(parsed);
    const auto damage = [&](const auto& edit, const char* message) {
        auto broken = juce::JSON::parse(json); edit(broken);
        require(!alien::parseSessionProject(juce::JSON::toString(broken), parsed, error)
            && alien::serializeSessionProject(parsed) == beforeDamage, message);
    };
    damage([](juce::var& v) { v.getProperty("scenes", {})[0][0].getDynamicObject()->setProperty("rackClip", "ffffffffffffffff"); }, "missing rack source refused atomically");
    damage([](juce::var& v) { v.getProperty("channelRack", {}).getProperty("patterns", {})[1].getDynamicObject()->setProperty("clip",
        v.getProperty("channelRack", {}).getProperty("patterns", {})[0].getProperty("clip", {})); }, "duplicate rack clip ids refused");
    damage([](juce::var& v) { v.getProperty("channelRack", {}).getProperty("channels", {})[0].getDynamicObject()->setProperty("lane", "0000000000000000"); }, "zero lane ids refused");
    damage([](juce::var& v) { v.getProperty("timeline", {}).getProperty("regions", {})[0].getDynamicObject()->setProperty("rackClip",
        v.getProperty("channelRack", {}).getProperty("patterns", {})[1].getProperty("clip", {})); }, "mismatched Song source cache refused");
    damage([](juce::var& v) { v.getDynamicObject()->setProperty("version", 25); }, "bindings hidden in an older format refused");
    auto legacy = juce::JSON::parse(json); legacy.getDynamicObject()->setProperty("version", 25);
    for (auto& scene : *legacy.getProperty("scenes", {}).getArray()) for (auto& cell : *scene.getArray()) cell.getDynamicObject()->removeProperty("rackClip");
    for (auto& r : *legacy.getProperty("timeline", {}).getProperty("regions", {}).getArray()) r.getDynamicObject()->removeProperty("rackClip");
    require(alien::parseSessionProject(juce::JSON::toString(legacy), parsed, error)
        && alien::fromProjectData(parsed).rackMatchesLibrary(), "format 25 migrates independent legacy bank slots and Song placements");
    c.newProject(); require(!c.canPasteLinked() && !c.hasCopiedCell(), "clipboard ids cannot alias a new document's sources");
    std::puts("PASS: saved rack bindings, linked/independent uses, migration and strict validation (format 26)");
}

static void universalTrackIdentityTests() {
    using alien::TrackRole;
    ToneEngine engine;
    alien::ProjectController c(engine);
    const auto& d = c.document();
    const auto session = d.trackId(TrackRole::session, 1), rack = d.trackId(TrackRole::rack, 0);
    const auto master = d.trackId(TrackRole::master), ret = d.trackId(TrackRole::returnTrack, 0);
    require(session && rack && master && ret && session != rack && rack != master && master != ret,
            "one registry supplies distinct typed identities for every document stage");
    auto channel = d.channelRack.channels[0]; channel.output = 1;
    require(c.editRack("Route", [=](auto& p) { return p.setChannel(0, channel); }), "route rack stage through existing Session fader");
    c.commit("Initial routes");
    require(c.moveTrack(1, 1), "reorder Session track"); c.commit("Move identity");
    require(d.trackId(TrackRole::session, 2) == session && d.trackSlot(session, TrackRole::session) == 2
            && d.channelRack.channels[0].output == 2, "track identity and rack output follow Session reorder");
    require(c.editRack("Move rack", [](auto& p) { return p.moveChannel(0, 1); }), "reorder rack row");
    require(d.trackId(TrackRole::rack, 1) == rack, "rack device identity follows its stable lane");
    require(c.editRack("Duplicate rack", [](auto& p) { return p.duplicateChannel(1); }), "duplicate rack channel");
    const auto copy = d.trackId(TrackRole::rack, 2);
    require(copy && copy != rack, "duplicate rack stage gets its own identity without copying ownership");
    const int bus = c.addBus("Group"); require(bus >= 0, "add universal bus stage");
    const auto busId = d.trackId(TrackRole::bus, bus);
    require(c.setTrackOutput(2, bus), "route Session stage through group"); c.commit("Route group");
    c.setClipPattern(2, 0, patternWith(64), false);
    require(c.placeRegion(2, 0, 0), "Song region retains its universal track owner");
    c.commit("Prepare identity fixture");
    const auto json = alien::serializeSessionProject(alien::toProjectData(d));
    alien::SessionProjectData parsed; juce::String error;
    require(json.isNotEmpty() && alien::parseSessionProject(json, parsed, error), "format 29 saves and parses shared track registry");
    const auto reopened = alien::fromProjectData(parsed);
    require(reopened.trackId(TrackRole::session, 2) == session && reopened.trackId(TrackRole::rack, 1) == rack
        && reopened.trackId(TrackRole::rack, 2) == copy && reopened.trackId(TrackRole::bus, bus) == busId
        && reopened.trackId(TrackRole::master) == master && reopened.trackId(TrackRole::returnTrack, 0) == ret,
        "save/reopen preserves identities for all stage roles");
    require(reopened.channelRack.channels[1] == d.channelRack.channels[1] && reopened.tracks[2].settings.output == bus
        && reopened.rackMatchesLibrary(), "identity adapters preserve devices, both faders, routes and rack content");
    const auto before = alien::serializeSessionProject(parsed);
    const auto damage = [&](auto mutate, const char* message) {
        auto value = juce::JSON::parse(json); mutate(value);
        const bool accepted = alien::parseSessionProject(juce::JSON::toString(value), parsed, error);
        if (accepted) std::cerr << "Unexpected accepted fixture " << message << ": "
            << juce::JSON::toString(value.getProperty("trackReferences", {}), true) << '\n';
        require(!accepted && alien::serializeSessionProject(parsed) == before, message);
    };
    damage([](auto& v) { auto& entries = *v.getProperty("trackRegistry", {}).getProperty("entries", {}).getArray();
        entries[1].getDynamicObject()->setProperty("id", entries[0].getProperty("id", {})); }, "duplicate TrackIds fail atomically");
    damage([](auto& v) { v.getDynamicObject()->removeProperty("trackRegistry"); }, "format 29 requires registry");
    damage([](auto& v) { v.getProperty("trackRegistry", {}).getDynamicObject()->setProperty("next", "0000000000000001"); }, "allocator cannot alias existing identities");
    damage([](auto& v) { v.getProperty("trackRegistry", {}).getProperty("entries", {})[0].getDynamicObject()->setProperty("id", "0000000000000000"); }, "zero identity refused");
    damage([](auto& v) { auto refs = v.getProperty("trackReferences", {}); auto outputs = refs.getProperty("rackOutputs", {});
        outputs.getArray()->set(1, juce::var("000000000000dead")); refs.getDynamicObject()->setProperty("rackOutputs", outputs); }, "unknown saved route identity refused");
    damage([](auto& v) { v.getProperty("channelRack", {}).getProperty("channels", {})[1].getDynamicObject()->setProperty("output", 0); }, "route cache cannot redirect a persistent reference");
    damage([](auto& v) { v.getProperty("trackRegistry", {}).getProperty("entries", {})[0].getDynamicObject()->setProperty("role", 4); }, "wrong role or missing owner refused");
    auto legacy = juce::JSON::parse(json); legacy.getDynamicObject()->setProperty("version", 28);
    legacy.getDynamicObject()->removeProperty("trackRegistry"); legacy.getDynamicObject()->removeProperty("trackReferences");
    require(alien::parseSessionProject(juce::JSON::toString(legacy), parsed, error), "format 28 migrates to one registry");
    const auto migrated = alien::fromProjectData(parsed);
    const auto migratedJson = alien::serializeSessionProject(alien::toProjectData(migrated));
    require(alien::parseSessionProject(migratedJson, parsed, error)
        && alien::fromProjectData(parsed).trackId(TrackRole::rack, 1) == migrated.trackId(TrackRole::rack, 1),
        "legacy identities are allocated once and retained on next save");
    require(c.deleteTrack(2), "delete routed identity"); c.commit("Delete identity");
    require(d.trackSlot(session, TrackRole::session) == -1 && d.channelRack.channels[1].output == -1,
        "deleted track is retired and legacy rack route falls back to master");
    require(c.undo() && d.trackId(TrackRole::session, 2) == session && d.channelRack.channels[1].output == 2,
        "undo restores exact identity and output binding");
    require(c.redo() && d.trackSlot(session, TrackRole::session) == -1, "redo retires identity");
    require(c.addTrack() && d.trackId(TrackRole::session, d.trackCount() - 1) != session, "new track cannot inherit deleted identity");
    c.commit("Add new identity"); const auto issued = d.trackId(TrackRole::session, d.trackCount() - 1);
    require(c.undo() && c.addTrack() && d.trackId(TrackRole::session, d.trackCount() - 1) != issued,
        "branching after undo never reissues an identity");
    require(c.deleteReturn(0) && c.addReturn() == 0 && d.trackId(TrackRole::returnTrack, 0) != ret,
        "a reused return slot has a fresh identity");
    require(c.deleteBus(bus) && c.addBus() == bus && d.trackId(TrackRole::bus, bus) != busId,
        "a reused bus slot has a fresh identity");
    auto invalid = parsed;
    invalid.channelRack.channels[0].output = alien::maxSessionTracks - 1;
    require(alien::serializeSessionProject(invalid).isEmpty(), "a rack output cannot name an absent Session owner");
    invalid = parsed;
    invalid.channelRack.channelCount = std::numeric_limits<int>::max();
    require(alien::serializeSessionProject(invalid).isEmpty(), "invalid in-memory capacity is refused before building owner adapters");
    alien::TrackRegistry registry;
    require(!registry.synchronize({{TrackRole::master, 1}, {TrackRole::master, 1}}) && registry.entries().empty(),
        "duplicate owner adapter fails without partial mutation");
    require(registry.restore({{alien::TrackId{1}, {TrackRole::master, 1}}}, std::numeric_limits<std::uint64_t>::max()), "exhausted allocator fixture");
    require(!registry.synchronize({{TrackRole::master, 1}, {TrackRole::session, 1}}) && registry.entries().size() == 1,
        "identity exhaustion refuses a new owner atomically");
    std::puts("PASS: persistent universal track identities, migration, references and undo lifecycle");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    universalTrackIdentityTests();
    automationUndoTests();
    parameterAutomationUndoTests();
    masterVolumeTests();
    mediaIndexTests();
    missingAudioTests();
    timelineIntegrationTests();
    arrangementPublishTests();
    tempoMapControllerTests();
    clipLibraryOwnershipTests();
    linkedClipTests();
    placementUniqueTests();
    placementClipTests();
    placementClipFileTests();
    rackLibraryTests();
    savedRackBindingTests();
    structureTests();
    fileFormatTests();
    undoTests();
    saveAndAutosaveTests();
    recoveryAndPortableRegressionTests();
    return 0;
}
