#include "SessionProject.h"
#include "PortableProject.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

static void portableProjectTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienPortableTest", "", false);
    const auto originals = root.getChildFile("originals");
    const auto projectDir = root.getChildFile("project");
    require(originals.createDirectory().wasOk() && projectDir.createDirectory().wasOk(), "create test directories");
    const auto first = originals.getChildFile("first/kick.wav"), second = originals.getChildFile("second/kick.wav");
    require(first.getParentDirectory().createDirectory().wasOk()
        && second.getParentDirectory().createDirectory().wasOk(), "create duplicate basename directories");
    require(first.replaceWithText("first audio bytes") && second.replaceWithText("second audio bytes"), "create source assets");
    alien::SessionProjectData source, saved;
    source.clips[0][0].audioFilePath = first.getFullPathName();
    source.clips[0][1].audioFilePath = second.getFullPathName();
    source.clips[1][0].audioFilePath = first.getFullPathName();
    source.drumPaths[0][0] = first.getFullPathName();
    juce::String error;
    const auto target = projectDir.getChildFile("Song.alienstudio");
    require(alien::writeSessionProjectFile(target, source, true, saved, error), "collect portable project");
    require(!juce::File::isAbsolutePath(saved.clips[0][0].audioFilePath)
        && saved.clips[0][0].audioFilePath == saved.clips[1][0].audioFilePath
        && saved.clips[0][0].audioFilePath != saved.clips[0][1].audioFilePath,
        "relative media paths deduplicate reused sources and preserve duplicate basenames");
    require(saved.drumPaths[0][0] == saved.clips[0][0].audioFilePath, "kit samples collected and deduplicated with clips");
    const auto priorJson = target.loadFileAsString();
    source.clips[1][1].audioFilePath = originals.getChildFile("missing.wav").getFullPathName();
    require(!alien::writeSessionProjectFile(target, source, true, saved, error)
        && target.loadFileAsString() == priorJson, "failed collection leaves existing project intact");
    require(projectDir.findChildFiles(juce::File::findDirectories, false).size() == 1,
        "failed collection cleans only its newly created media folder");
    require(originals.deleteRecursively(), "remove all original audio");
    const auto moved = root.getChildFile("moved");
    require(projectDir.moveFileTo(moved), "move whole project folder");
    const auto movedTarget = moved.getChildFile("Song.alienstudio");
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(movedTarget.loadFileAsString(), loaded, error), "open moved portable project");
    require(alien::resolveProjectAudio(movedTarget, loaded.clips[0][0].audioFilePath).loadFileAsString() == "first audio bytes"
        && alien::resolveProjectAudio(movedTarget, loaded.clips[0][1].audioFilePath).loadFileAsString() == "second audio bytes",
        "moved project resolves exact audio without original files");
    // The application stores resolved absolute paths in memory; a normal Save
    // must convert owned media back to relative paths to retain portability.
    for (auto& scene : loaded.clips) for (auto& clip : scene)
        if (clip.audioFilePath.isNotEmpty()) clip.audioFilePath = alien::resolveProjectAudio(movedTarget, clip.audioFilePath).getFullPathName();
    for (auto& kit : loaded.drumPaths) for (auto& path : kit)
        if (path.isNotEmpty()) path = alien::resolveProjectAudio(movedTarget, path).getFullPathName();
    require(alien::writeSessionProjectFile(movedTarget, loaded, false, saved, error)
        && !juce::File::isAbsolutePath(saved.clips[0][0].audioFilePath), "normal save retains portable references");
    require(alien::resolveProjectAudio(movedTarget, "../outside.wav") == juce::File{}, "reject escaping relative reference");
    require(root.deleteRecursively(), "remove owned test directory");
}

int main() {
    portableProjectTests();
    alien::SessionProjectData source;
    source.launchQuantizationSteps = 4;
    source.selectedTrack = 2;
    source.selectedScene = 3;
    source.bpm = 137;
    source.synthPreset = 2;
    source.drumPaths[2][7] = "C:/Drums/snare.wav";
    source.trackSettings[0] = {1, 0.5f, -0.75f, true, false};
    source.trackSettings[3] = {2, 1.5f, 0.5f, false, true, 1};
    source.stepPattern = {true, false, true, false, false, true, false, false,
        true, false, false, true, false, false, true, false};
    source.stepPitches = {36, 48, 60, 72, 84, 96, 108, 120,
        57, 59, 61, 63, 65, 67, 69, 71};
    source.stepVelocities = {1, 8, 16, 24, 32, 40, 48, 56,
        64, 72, 80, 88, 96, 104, 112, 127};
    auto& midi = source.clips[1][2];
    midi.pattern.loopSteps = 128;
    require(midi.pattern.add(67, 7.25, 2.5, 0.63f, 0.42f) != 0,
        "test pattern contains a valid note");
    auto& audio = source.clips[3][1];
    audio.pattern.add(48, 0.0, 4.0);
    audio.audioFilePath = "C:\\Music\\Ambient \"take\".mp3";

    const auto json = alien::serializeSessionProject(source);
    require(json.isNotEmpty(), "valid project serializes to JSON");
    alien::SessionProjectData restored;
    juce::String error;
    require(alien::parseSessionProject(json, restored, error), "serialized project parses");
    require(error.isEmpty() && restored.valid(), "loaded project passes model validation");
    require(restored.launchQuantizationSteps == 4 && restored.selectedTrack == 2
        && restored.selectedScene == 3 && restored.bpm == 137 && restored.synthPreset == 2
        && restored.stepPattern == source.stepPattern
        && restored.stepPitches == source.stepPitches
        && restored.stepVelocities == source.stepVelocities,
        "tempo, synth preset, sequencer steps, pitch, velocity and launch/editor settings round-trip");
    require(restored.trackSettings[0].preset == 1 && restored.trackSettings[0].gain == 0.5f
        && restored.trackSettings[0].pan == -0.75f && restored.trackSettings[0].mute
        && restored.trackSettings[3].preset == 2 && restored.trackSettings[3].solo,
        "independent track sound and mixer settings round-trip");
    auto invalidTracks = juce::JSON::parse(json);
    auto* tracks = invalidTracks.getDynamicObject()->getProperty("trackSettings").getArray();
    (*tracks)[0].getDynamicObject()->setProperty("gain", -1.0);
    require(!alien::parseSessionProject(juce::JSON::toString(invalidTracks), restored, error)
        && restored.trackSettings[0].gain == 0.5f, "bad mixer data leaves destination unchanged");
    require(restored.trackSettings[3].instrument == 1 && restored.drumPaths[2][7] == source.drumPaths[2][7], "drum instrument and pads round-trip");
    const auto& restoredMidi = restored.clips[1][2].pattern;
    require(restoredMidi.loopSteps == 128 && restoredMidi.count == 1
        && restoredMidi.notes[0].pitch == 67 && restoredMidi.notes[0].startStep == 7.25
        && restoredMidi.notes[0].lengthSteps == 2.5
        && std::abs(restoredMidi.notes[0].velocity - 0.63f) < 1.0e-6f
        && std::abs(restoredMidi.notes[0].probability - 0.42f) < 1.0e-6f,
        "MIDI timing, pitch, velocity, probability and loop length round-trip");
    require(restored.clips[3][1].audioFilePath == audio.audioFilePath
        && restored.clips[3][1].pattern.count == 1,
        "audio source reference and the clip's underlying MIDI pattern round-trip");

    const auto projectFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienStudioProjectTest", ".alienstudio", false);
    require(projectFile.replaceWithText(json), "project JSON writes to a session file");
    alien::SessionProjectData fromDisk;
    require(alien::parseSessionProject(projectFile.loadFileAsString(), fromDisk, error)
        && fromDisk.clips[3][1].audioFilePath == audio.audioFilePath,
        "saved project file can be reopened with audio references intact");
    projectFile.deleteFile();

    const auto unchanged = restored.clips[1][2].pattern.notes[0].pitch;
    require(!alien::parseSessionProject("{ broken", restored, error)
        && error.isNotEmpty() && restored.clips[1][2].pattern.notes[0].pitch == unchanged,
        "malformed JSON fails without partially mutating the current project");
    require(!alien::parseSessionProject("{\"format\":\"AlienStudioSession\",\"version\":99}",
        restored, error) && error.contains("version"), "unknown project versions are rejected");
    auto legacy = juce::JSON::parse(json);
    auto* legacyObject = legacy.getDynamicObject();
    legacyObject->setProperty("version", 1);
    legacyObject->removeProperty("stepPitches");
    legacyObject->removeProperty("stepVelocities");
    require(alien::parseSessionProject(juce::JSON::toString(legacy), restored, error)
        && restored.stepPitches[0] == 57 && restored.stepVelocities[0] == 102,
        "version 1 projects load with the original pitch and velocity defaults");
    for (const int version : {1, 2}) {
        auto old = juce::JSON::parse(json);
        old.getDynamicObject()->setProperty("version", version);
        old.getDynamicObject()->removeProperty("trackSettings");
        require(alien::parseSessionProject(juce::JSON::toString(old), restored, error), "legacy project loads");
        for (const auto& track : restored.trackSettings)
            require(track.preset == source.synthPreset && track.gain == 1 && track.pan == 0
                && !track.mute && !track.solo, "legacy global preset migrates to every track");
    }
    source.stepPitches[0] = 128;
    require(alien::serializeSessionProject(source).isEmpty(),
        "out-of-range step pitch cannot be saved");
    source.stepPitches[0] = 57;
    source.stepVelocities[0] = 0;
    require(alien::serializeSessionProject(source).isEmpty(),
        "out-of-range step velocity cannot be saved");
    source.launchQuantizationSteps = 8;
    require(alien::serializeSessionProject(source).isEmpty(),
        "unsupported launch settings cannot be serialized");
    std::cout << "Session project serialization and validation passed\n";
}
