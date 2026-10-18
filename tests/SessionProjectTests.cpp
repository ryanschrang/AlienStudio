#include "SessionProject.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    alien::SessionProjectData source;
    source.launchQuantizationSteps = 4;
    source.selectedTrack = 2;
    source.selectedScene = 3;
    source.bpm = 137;
    source.synthPreset = 2;
    source.stepPattern = {true, false, true, false, false, true, false, false,
        true, false, false, true, false, false, true, false};
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
        && restored.stepPattern == source.stepPattern,
        "tempo, synth preset, sequencer steps and launch/editor settings round-trip");
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
    source.launchQuantizationSteps = 8;
    require(alien::serializeSessionProject(source).isEmpty(),
        "unsupported launch settings cannot be serialized");
    std::cout << "Session project serialization and validation passed\n";
}
