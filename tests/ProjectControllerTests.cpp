#include "ProjectController.h"
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

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    structureTests();
    fileFormatTests();
    return 0;
}
