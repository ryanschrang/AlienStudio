#include "SessionProject.h"
#include "PortableProject.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>

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
    alien::timeline::Region archived; archived.row = 0; archived.source = 0;
    require(source.timeline.recordTake(archived), "portable fixture has original take");
    source.timeline.regions.clear(); // The original is not part of the active comp.
    source.clips[2][0].audioFilePath=first.getFullPathName();
    require(source.timeline.place(archived),"portable frozen original");
    source.timeline.freezeBaseLengthBars=source.timeline.lengthBars;
    require(source.timeline.freezeTrack(0,2,4000,2.0),"portable freeze cache");
    source.trackSettings[0].freezeScene=2;


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
    require(loaded.trackSettings[0].freezeScene==2 && loaded.timeline.frozenOriginals.size()==1
        && alien::resolveProjectAudio(movedTarget,loaded.clips[2][0].audioFilePath).existsAsFile(),"portable move retains frozen cache and original arrangement");

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

// Sampler-style plugins can legitimately save many megabytes of state. Anything
// that saves must reopen, so the per-plugin cap and the whole-file cap are
// checked together.
static void pluginStateLimitTests() {
    const juce::String description = "<PLUGIN name=\"Big Sampler\" format=\"VST3\" uniqueId=\"1\"/>";
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienPluginLimitTest", "", false);
    require(root.createDirectory().wasOk(), "create plugin limit directory");
    const auto target = root.getChildFile("Song.alienstudio");
    juce::String error;
    alien::SessionProjectData saved;

    // 8 Mi characters (6 MiB of state) exceeded the old 4 Mi limit and must now round-trip.
    const auto big = juce::String::repeatedString("QUJD", 2 * 1024 * 1024);
    alien::SessionProjectData source;
    source.plugins[0].instrument = {description, big, false};
    require(source.valid(), "multi-megabyte plugin state is accepted");
    require(alien::writeSessionProjectFile(target, source, false, saved, error), "save multi-megabyte plugin state");
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(target.loadFileAsString(), loaded, error)
        && loaded.plugins[0].instrument.stateBase64 == big, "multi-megabyte plugin state reopens intact");

    // One character over the per-plugin cap is rejected and the culprit is named.
    alien::PluginRackState rack;
    rack.effects.push_back({description, juce::String::repeatedString("A", alien::maxPluginStateChars + 1), false});
    require(!rack.valid() && alien::oversizedPluginName(rack) == "Big Sampler", "oversized plugin state is named");
    require(alien::oversizedPluginName(source.plugins[0]).isEmpty(), "in-limit plugin is not blamed");
    alien::SessionProjectData tooBig;
    tooBig.plugins[0] = rack;
    require(!tooBig.valid() && !alien::writeSessionProjectFile(target, tooBig, false, saved, error),
        "plugin state over the cap is refused");

    // Individually legal states can still add up to a file this version could not reopen.
    // Saving must refuse rather than write an unopenable project, and keep the old file.
    const auto before = target.loadFileAsString();
    const auto chunk = juce::String::repeatedString("A", 22 * 1024 * 1024);
    alien::SessionProjectData huge;
    for (int i = 0; i < 6; ++i) huge.plugins[0].effects.push_back({description, chunk, false});
    require(huge.valid(), "each state fits the per-plugin cap");
    error.clear();
    require(!alien::writeSessionProjectFile(target, huge, false, saved, error) && error.contains("128 MiB"),
        "a save larger than the open limit is refused with an explanation");
    require(target.loadFileAsString() == before, "refused save leaves the existing project untouched");
    require(root.deleteRecursively(), "remove plugin limit directory");
}

// Master volume is optional in the file so older projects keep opening unchanged.
static void masterVolumeFormatTests() {
    alien::SessionProjectData source;
    juce::String error;
    require(source.masterGain == 1.0f, "default master volume is unity");
    source.masterGain = 1.75f;
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(alien::serializeSessionProject(source), loaded, error) && loaded.masterGain == 1.75f,
        "master volume round-trips");
    source.masterGain = alien::maxMasterGain;
    require(alien::parseSessionProject(alien::serializeSessionProject(source), loaded, error) && loaded.masterGain == alien::maxMasterGain,
        "the +12 dB maximum round-trips");

    auto withField = [&](const juce::var& value, bool remove) {
        alien::SessionProjectData fresh;
        auto json = juce::JSON::parse(alien::serializeSessionProject(fresh));
        if (remove) json.getDynamicObject()->removeProperty("masterGain");
        else json.getDynamicObject()->setProperty("masterGain", value);
        alien::SessionProjectData parsed; juce::String message;
        const bool ok = alien::parseSessionProject(juce::JSON::toString(json), parsed, message);
        return std::make_pair(ok, parsed.masterGain);
    };
    require(withField({}, true) == std::make_pair(true, 1.0f), "a file without master volume opens at unity");
    require(withField(2, false) == std::make_pair(true, 2.0f), "an integer master volume is accepted");
    require(!withField(-0.5, false).first && !withField(4.5, false).first, "out-of-range master volume is rejected");
    require(!withField("loud", false).first, "non-numeric master volume is rejected");
    alien::SessionProjectData invalid;
    invalid.masterGain = std::numeric_limits<float>::quiet_NaN();
    require(!invalid.valid() && alien::serializeSessionProject(invalid).isEmpty(), "a non-finite master volume is never written");
}

// Recording settings are an optional object in the file, so older projects keep opening unchanged.
static void recordingSettingsFormatTests() {
    juce::String error;
    alien::SessionProjectData source;
    require(source.recording == alien::RecordSettings{} && source.recording.countInBars == 1 && !source.recording.metronome,
        "default recording settings");
    source.recording.metronome = true; source.recording.metronomeVolume = 0.8f; source.recording.countInBars = 2;
    source.recording.latencyOffsetMs = -35; source.recording.replace = true;
    source.recording.audioLatencyOffsetMs = 25; source.recording.punch = true;
    alien::SessionProjectData loaded;
    require(alien::parseSessionProject(alien::serializeSessionProject(source), loaded, error) && loaded.recording == source.recording,
        "recording settings round-trip");

    auto withRecording = [&](const std::function<void(juce::DynamicObject&)>& edit, bool remove) {
        alien::SessionProjectData fresh;
        fresh.recording = source.recording;
        auto json = juce::JSON::parse(alien::serializeSessionProject(fresh));
        if (remove) json.getDynamicObject()->removeProperty("recording");
        else edit(*json.getDynamicObject()->getProperty("recording").getDynamicObject());
        alien::SessionProjectData parsed; juce::String message;
        const bool ok = alien::parseSessionProject(juce::JSON::toString(json), parsed, message);
        return std::make_pair(ok, parsed.recording);
    };
    const auto none = withRecording({}, true);
    require(none.first && none.second == alien::RecordSettings{}, "a file without recording settings opens with the defaults");
    require(withRecording([](juce::DynamicObject& o) { o.setProperty("countInBars", 0); }, false).first, "count-in can be switched off");
    require(withRecording([](juce::DynamicObject& o) { o.setProperty("metronomeVolume", 1); }, false).first, "an integer volume is accepted");
    for (const auto& [name, value] : std::vector<std::pair<const char*, juce::var>>{
             {"countInBars", 3}, {"countInBars", -1}, {"countInBars", 1.5}, {"countInBars", "one"},
             {"latencyOffsetMs", 101}, {"latencyOffsetMs", -101}, {"latencyOffsetMs", 2.5},
             {"metronomeVolume", 1.5}, {"metronomeVolume", -0.1}, {"metronomeVolume", "loud"},
             {"metronome", 1}, {"metronome", "yes"}, {"replace", 0}, {"replace", "no"},
             {"audioLatencyOffsetMs", 101}, {"audioLatencyOffsetMs", 2.5}, {"audioLatencyOffsetMs", "x"}, {"punch", 1}, {"punch", "on"}}) {
        require(!withRecording([&](juce::DynamicObject& o) { o.setProperty(name, value); }, false).first,
            "recording settings that are out of range or of the wrong type are rejected");
    }
    require(!withRecording([](juce::DynamicObject& o) { o.removeProperty("replace"); }, false).first, "a recording object missing a field is rejected");
    {
        const auto older = withRecording([](juce::DynamicObject& o) { o.removeProperty("audioLatencyOffsetMs"); o.removeProperty("punch"); }, false);
        require(older.first && older.second.audioLatencyOffsetMs == 0 && !older.second.punch && older.second.countInBars == 2,
            "a project saved before audio takes opens with no audio offset and no punch");
    }
    {
        alien::SessionProjectData fresh;
        auto json = juce::JSON::parse(alien::serializeSessionProject(fresh));
        json.getDynamicObject()->setProperty("recording", 5);
        alien::SessionProjectData parsed; juce::String message;
        require(!alien::parseSessionProject(juce::JSON::toString(json), parsed, message), "a recording value that is not an object is rejected");
    }
    alien::SessionProjectData invalid;
    invalid.recording.countInBars = 7;
    require(!invalid.valid() && alien::serializeSessionProject(invalid).isEmpty(), "invalid recording settings are never written");
}


static void automationFormatTests() {
    alien::SessionProjectData source;
    source.timeline.automationLanes = {
        {1, alien::automation::Parameter::volume, true, {{0, 0.25f}, {960, 1.0f}}},
        {2, alien::automation::Parameter::pan, false, {{240, -0.5f, alien::automation::Curve::hold}}},
        {1, alien::automation::Parameter::sendA, true, {{0, 0}, {960, 1}}},
        {1, alien::automation::Parameter::sendB, false, {{0, 0.3f}}},
        {1, alien::automation::Parameter::sendC, true, {{0, 0.4f, alien::automation::Curve::hold}}},
        {1, alien::automation::Parameter::sendD, true, {{0, 0.5f}}}
    };
    const auto json = alien::serializeSessionProject(source);
    alien::SessionProjectData loaded; juce::String error;
    require(alien::parseSessionProject(json, loaded, error)
        && loaded.timeline.automationLanes == source.timeline.automationLanes, "automation lanes and curves round-trip");
    const auto beforeDamage = loaded.timeline;
    auto badSend = juce::JSON::parse(json);
    auto* sendPoints = badSend.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("automation").getArray()->getReference(2).getDynamicObject()->getProperty("points").getArray();
    sendPoints->getReference(0).getDynamicObject()->setProperty("value", 1.5);
    require(!alien::parseSessionProject(juce::JSON::toString(badSend), loaded, error) && loaded.timeline == beforeDamage,
        "invalid send value rejected without replacing the loaded song");
    alien::SessionProjectData full;
    for (int t = 0; t < full.trackCount(); ++t) for (int p = 0; p < alien::automation::parametersPerTrack; ++p)
        full.timeline.automationLanes.push_back({t, static_cast<alien::automation::Parameter>(p), true, {{0, 0.5f}}});
    require(alien::parseSessionProject(alien::serializeSessionProject(full), loaded, error)
        && loaded.timeline.automationLanes == full.timeline.automationLanes, "all six lanes per track survive save and load");
    auto legacy = juce::JSON::parse(json);
    auto* timeline = legacy.getDynamicObject()->getProperty("timeline").getDynamicObject();
    timeline->removeProperty("automation");
    require(!alien::parseSessionProject(juce::JSON::toString(legacy), loaded, error),
        "format 10 requires automation metadata");
    legacy.getDynamicObject()->setProperty("version", 9);
    require(alien::parseSessionProject(juce::JSON::toString(legacy), loaded, error)
        && loaded.timeline.automationLanes.empty(), "format 9 migrates with no automation");
    auto damaged = juce::JSON::parse(json);
    auto* lanes = damaged.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("automation").getArray();
    lanes->getReference(0).getDynamicObject()->setProperty("parameter", 99);
    require(!alien::parseSessionProject(juce::JSON::toString(damaged), loaded, error), "unknown automation targets rejected");
}

static void takeFormatTests() {
    alien::SessionProjectData source;
    alien::timeline::Region take; take.row = 0; take.source = 1; take.length = 3840;
    require(source.timeline.recordTake(take), "fixture recording pass");
    take.source = 2;
    require(source.timeline.recordTake(take), "fixture alternate pass");
    require(source.timeline.compTake(source.timeline.takes[0].id, 960, 1920), "fixture comp");
    source.clips[1][0].audioFilePath = "C:/takes/first.wav";
    source.clips[2][0].audioFilePath = "C:/takes/second.wav";
    alien::SessionProjectData loaded; juce::String error;
    const auto json = alien::serializeSessionProject(source);
    require(alien::parseSessionProject(json, loaded, error) && loaded.timeline.takes.size() == 2
        && loaded.timeline.regions.size() == 3, "takes and comp round-trip");
    require(loaded.timeline.takes[0].source == 1 && loaded.timeline.takes[1].source == 2
        && loaded.clips[1][0].audioFilePath == source.clips[1][0].audioFilePath, "original source references survive");
    require(loaded.timeline.compTake(loaded.timeline.takes[1].id, 0, 3840), "restored take can replace comp");
    auto broken = juce::JSON::parse(json);
    auto* regions = broken.getProperty("timeline", {}).getDynamicObject()->getProperty("regions").getArray();
    regions->getReference(regions->size() - 1).getDynamicObject()->setProperty("take", "wrong");
    const auto prior = loaded.timeline;
    require(!alien::parseSessionProject(juce::JSON::toString(broken), loaded, error) && loaded.timeline == prior,
        "malformed take is rejected without changing loaded project");
    alien::SessionProjectData oldSource;
    auto old = juce::JSON::parse(alien::serializeSessionProject(oldSource));
    old.getDynamicObject()->setProperty("version", 14);
    require(alien::parseSessionProject(juce::JSON::toString(old), loaded, error) && loaded.timeline.takes.empty(),
        "version 14 projects open with no recording archive");
}

static void freezeFormatTests() {
    alien::SessionProjectData source;alien::timeline::Region original;original.row=0;original.source=0;
    require(source.timeline.place(original),"freeze format original");source.timeline.freezeBaseLengthBars=source.timeline.lengthBars;
    require(source.timeline.freezeTrack(0,1,4000,2.0),"freeze format cache");
    source.trackSettings[0].freezeScene=1;source.clips[1][0].audioFilePath="C:/Freeze/cache.wav";
    source.plugins[0].effects.push_back({"<PLUGIN name=\"Original effect\"/>","YWJj",false});
    alien::SessionProjectData loaded;juce::String error;
    const auto json=alien::serializeSessionProject(source);
    require(json.isNotEmpty() && alien::parseSessionProject(json,loaded,error),"frozen project round trip");
    require(loaded.trackSettings[0].freezeScene==1 && loaded.timeline.frozenOriginals.size()==1
        && loaded.plugins==source.plugins && loaded.timeline.regions.front().gain==2.0,"cache, originals, gain and plugin states preserved");
    auto damaged=juce::JSON::parse(json);damaged.getProperty("trackSettings",{}).getArray()->getReference(0).getDynamicObject()->setProperty("freezeScene",30);
    const auto previous=loaded.timeline;
    require(!alien::parseSessionProject(juce::JSON::toString(damaged),loaded,error) && loaded.timeline==previous,"invalid cache reference rejected transactionally");
    alien::SessionProjectData oldSource;auto old=juce::JSON::parse(alien::serializeSessionProject(oldSource));old.getDynamicObject()->setProperty("version",15);
    for(auto& track:*old.getProperty("trackSettings",{}).getArray())track.getDynamicObject()->removeProperty("freezeScene");
    require(alien::parseSessionProject(juce::JSON::toString(old),loaded,error) && !loaded.trackSettings[0].frozen(),"version 15 defaults unfrozen");
}

int main() {
    freezeFormatTests();
    takeFormatTests();
    automationFormatTests();
    recordingSettingsFormatTests();
    masterVolumeFormatTests();
    pluginStateLimitTests();
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
    source.trackSettings[1].armed = true; source.trackSettings[1].input = 2; source.trackSettings[1].stereoInput = true;
    source.trackSettings[1].monitor = static_cast<int>(alien::InputMonitor::off);
    source.stepPattern = {true, false, true, false, false, true, false, false,
        true, false, false, true, false, false, true, false};
    source.stepPitches = {36, 48, 60, 72, 84, 96, 108, 120,
        57, 59, 61, 63, 65, 67, 69, 71};
    source.stepVelocities = {1, 8, 16, 24, 32, 40, 48, 56,
        64, 72, 80, 88, 96, 104, 112, 127};
    auto& midi = source.clips[1][2];
    alien::PianoRollPattern midiNotes;
    midiNotes.loopSteps = 128;
    require(midiNotes.add(67, 7.25, 2.5, 0.63f, 0.42f) != 0,
        "test pattern contains a valid note");
    midi.pattern = midiNotes;
    auto& audio = source.clips[3][1];
    alien::PianoRollPattern audioNotes;
    audioNotes.add(48, 0.0, 4.0);
    audio.pattern = audioNotes;
    audio.audioFilePath = "C:\\Music\\Ambient \"take\".mp3";
    alien::timeline::Region edited;
    edited.row = 1; edited.source = 3; edited.start = 0; edited.length = alien::timeline::ticksPerBar;
    edited.audioOffset = 0.25; edited.gain = 1.5; edited.fadeIn = 120; edited.fadeOut = 240;
    edited.loopSource = true; edited.reversed = true; edited.crossfade = 360;
    require(source.timeline.place(edited), "test project contains a non-destructively edited audio region");

    const auto json = alien::serializeSessionProject(source);
    require(json.isNotEmpty(), "valid project serializes to JSON");
    alien::SessionProjectData restored;
    juce::String error;
    require(alien::parseSessionProject(json, restored, error), "serialized project parses");
    require(error.isEmpty() && restored.valid(), "loaded project passes model validation");
    const auto& restoredEdit = restored.timeline.regions.front();
    require(restoredEdit.audioOffset == 0.25 && restoredEdit.gain == 1.5 && restoredEdit.fadeIn == 120
        && restoredEdit.fadeOut == 240 && restoredEdit.loopSource && restoredEdit.reversed && restoredEdit.crossfade == 360,
        "non-destructive audio edits round-trip in the current project format");
    auto missingEdit = juce::JSON::parse(json);
    auto* missingRegion = missingEdit.getDynamicObject()->getProperty("timeline").getDynamicObject()
        ->getProperty("regions").getArray()->getReference(0).getDynamicObject();
    missingRegion->setProperty("crossfade", "long");
    require(!alien::parseSessionProject(juce::JSON::toString(missingEdit), restored, error),
        "a crossfade that is not a whole number of ticks is rejected");
    missingRegion->setProperty("crossfade", 2 * alien::timeline::ticksPerBar);
    require(!alien::parseSessionProject(juce::JSON::toString(missingEdit), restored, error),
        "a crossfade longer than its region is rejected");
    missingRegion->removeProperty("crossfade");
    missingEdit.getDynamicObject()->setProperty("version", 10);
    require(alien::parseSessionProject(juce::JSON::toString(missingEdit), restored, error)
        && restored.timeline.regions.front().crossfade == 0 && restored.timeline.regions.front().fadeIn == 120,
        "version 10 regions, saved before crossfades, load without one");
    missingEdit.getDynamicObject()->setProperty("version", alien::sessionProjectFormatVersion);
    missingRegion->removeProperty("fadeOut");
    require(!alien::parseSessionProject(juce::JSON::toString(missingEdit), restored, error),
        "a current project missing audio edit metadata is rejected");
    missingRegion->removeProperty("gain"); missingRegion->removeProperty("fadeIn");
    missingRegion->removeProperty("loopSource"); missingRegion->removeProperty("reversed");
    missingEdit.getDynamicObject()->setProperty("version", 8);
    require(alien::parseSessionProject(juce::JSON::toString(missingEdit), restored, error)
        && restored.timeline.regions.front().gain == 1.0 && restored.timeline.regions.front().fadeOut == 0,
        "version 8 regions without audio edit metadata migrate to safe defaults");
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
    {
        alien::SessionProjectData reloaded;
        require(alien::parseSessionProject(json, reloaded, error) && reloaded.trackSettings[1].armed && reloaded.trackSettings[1].input == 2
            && reloaded.trackSettings[1].stereoInput && reloaded.trackSettings[1].monitor == static_cast<int>(alien::InputMonitor::off)
            && !reloaded.trackSettings[0].armed && reloaded.trackSettings[0].monitor == static_cast<int>(alien::InputMonitor::automatic),
            "track arm, input and monitoring round-trip");
        auto older = juce::JSON::parse(json);
        auto* olderTrack = older.getDynamicObject()->getProperty("trackSettings").getArray()->getReference(1).getDynamicObject();
        for (const auto* key : {"armed", "input", "stereoInput", "monitor"}) olderTrack->removeProperty(key);
        require(alien::parseSessionProject(juce::JSON::toString(older), reloaded, error) && !reloaded.trackSettings[1].armed
            && reloaded.trackSettings[1].input == 0 && !reloaded.trackSettings[1].stereoInput
            && reloaded.trackSettings[1].monitor == static_cast<int>(alien::InputMonitor::automatic),
            "a project saved before track recording settings opens with the track disarmed on input 1, monitoring automatic");
        olderTrack->setProperty("monitor", 7);
        require(!alien::parseSessionProject(juce::JSON::toString(older), reloaded, error), "an unknown monitoring mode is rejected");
        olderTrack->setProperty("monitor", 0); olderTrack->setProperty("armed", "yes");
        require(!alien::parseSessionProject(juce::JSON::toString(older), reloaded, error), "a malformed arm state is rejected");
        olderTrack->setProperty("armed", true); olderTrack->setProperty("input", alien::maxTrackInputs);
        require(!alien::parseSessionProject(juce::JSON::toString(older), reloaded, error), "an input past the last one is rejected");
    }
    const alien::PianoRollPattern& restoredMidi = restored.clips[1][2].pattern;
    require(restoredMidi.loopSteps == 128 && restoredMidi.count == 1
        && restoredMidi.notes[0].pitch == 67 && restoredMidi.notes[0].startStep == 7.25
        && restoredMidi.notes[0].lengthSteps == 2.5
        && std::abs(restoredMidi.notes[0].velocity - 0.63f) < 1.0e-6f
        && std::abs(restoredMidi.notes[0].probability - 0.42f) < 1.0e-6f,
        "MIDI timing, pitch, velocity, probability and loop length round-trip");
    require(restored.clips[3][1].audioFilePath == audio.audioFilePath
        && restored.clips[3][1].pattern->count == 1,
        "audio source reference and the clip's underlying MIDI pattern round-trip");

    const auto projectFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienStudioProjectTest", ".alienstudio", false);
    require(projectFile.replaceWithText(json), "project JSON writes to a session file");
    alien::SessionProjectData fromDisk;
    require(alien::parseSessionProject(projectFile.loadFileAsString(), fromDisk, error)
        && fromDisk.clips[3][1].audioFilePath == audio.audioFilePath,
        "saved project file can be reopened with audio references intact");
    projectFile.deleteFile();

    const auto unchanged = restored.clips[1][2].pattern->notes[0].pitch;
    require(!alien::parseSessionProject("{ broken", restored, error)
        && error.isNotEmpty() && restored.clips[1][2].pattern->notes[0].pitch == unchanged,
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
