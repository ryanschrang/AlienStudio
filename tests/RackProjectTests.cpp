// The Channel Rack inside the project: file format version 7, undo and redo,
// track deletes and moves, saving (plain and portable), reopening, missing
// samples and relinking. No audio device or window is needed.
#include "ProjectController.h"
#include "AudioFileWrite.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static void pump(int ms = 50) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
static void waitIdle(alien::ProjectController& c) {
    for (int i = 0; i < 600 && c.isBusy(); ++i) pump(10);
    require(!c.isBusy(), "controller finishes");
}

using namespace alien::rack;

// A project whose rack uses every feature the format stores.
static Project busyRack() {
    Project p = Project::createDefault();
    p.addChannel(std::string("T\xc3\xa4m \xe2\x80\x9c" "Tom\xe2\x80\x9d"), "builtin:tom");
    p.addChannel("My sample", "C:/Samples/Kicks/boom.wav");
    Channel c = p.channels[0]; c.volume = 0.65f; c.pan = -0.25f; c.output = 2; c.chokeGroup = 3;
    p.setChannel(0, c);
    c = p.channels[2]; c.mute = true; p.setChannel(2, c);
    c = p.channels[3]; c.solo = true; c.volume = 1.0f; p.setChannel(3, c);
    p.setSwing(0.375f);
    for (int s : {0, 4, 8, 12}) p.setStepOn(0, 0, s, true);
    p.setStepField(0, 0, 4, StepField::velocity, 33);
    p.setStepField(0, 0, 8, StepField::pan, -40);
    p.setStepField(0, 0, 12, StepField::pitch, 7);
    p.setStepField(0, 0, 12, StepField::probability, 25);
    p.setStepField(0, 1, 5, StepField::velocity, 77);        // drawn, but the step is off: kept
    p.setStepField(0, 0, 4, StepField::gate, 35);
    p.setStepField(0, 0, 8, StepField::nudge, 60);
    p.setStepField(0, 0, 12, StepField::ratchet, 3);
    p.setStepField(0, 1, 6, StepField::ratchet, 4);           // also kept on a step that is off
    p.setStepOn(0, 2, 15, true);
    p.addPattern(0);                                          // a copy, selected
    p.setPatternLength(1, 32);
    p.setStepOn(1, 3, 31, true);
    p.renamePattern(1, "Fill \xe2\x80\x93 B");
    p.addPattern();
    p.setPatternLength(2, 64);
    p.setStepOn(2, 4, 63, true);
    p.addPattern();
    p.setPatternLength(3, 128);
    p.setStepOn(3, 1, 127, true);
    p.selectPattern(1);
    p.setNameColumnWidth(201);
    return p;
}

static void roundTripAndValidation() {
    alien::SessionProjectData data;
    data.channelRack = busyRack();
    require(data.valid(), "the busy rack is a valid project");
    const auto json = alien::serializeSessionProject(data);
    require(json.isNotEmpty(), "it serializes");
    alien::SessionProjectData back;
    juce::String error;
    require(alien::parseSessionProject(json, back, error), "and parses");
    require(back.channelRack == data.channelRack, "every channel, pattern, step value and the swing round-trip exactly");
    require(back.channelRack.pattern(0).steps[1][5].velocity == 77 && !back.channelRack.pattern(0).steps[1][5].on,
            "values drawn on a step that is off are kept");
    require(back.channelRack.patternNames[1] == "Fill \xe2\x80\x93 B" && back.channelRack.info[4].name == "T\xc3\xa4m \xe2\x80\x9c" "Tom\xe2\x80\x9d",
            "non-ASCII names survive");

    require(back.channelRack.nameColumnWidth == 201, "the name column's width round-trips");
    {
        const auto& steps = back.channelRack.pattern(0).steps;
        require(steps[0][4].gate == 35 && steps[0][8].nudge == 60 && steps[0][12].ratchet == 3 && steps[1][6].ratchet == 4 && !steps[1][6].on,
                "gate, nudge and ratchet round-trip, also on a step that is off");
        require(back.channelRack.pattern(3).length == 128 && back.channelRack.pattern(3).steps[1][127].on, "a 128-step pattern round-trips");
        require(juce::JSON::parse(json).getProperty("version", 0) == juce::var(12), "projects are written as version 12");
    }
    {   // Steps saved before version 12 have seven values: gate, nudge and ratchet take their defaults.
        auto copy = juce::JSON::parse(json);
        copy.getDynamicObject()->setProperty("version", 11);
        for (auto& pattern : *copy.getDynamicObject()->getProperty("channelRack").getDynamicObject()->getProperty("patterns").getArray())
            for (auto& cell : *pattern.getDynamicObject()->getProperty("steps").getArray()) cell.getArray()->removeRange(7, 3);
        alien::SessionProjectData older;
        require(alien::parseSessionProject(juce::JSON::toString(copy), older, error), "a version 11 rack with seven-value steps opens");
        auto expected = data.channelRack;
        for (int pat = 0; pat < expected.patternCount; ++pat)
            for (int c = 0; c < expected.channelCount; ++c)
                for (int st = 0; st < maxSteps; ++st) {
                    expected.setStepField(pat, c, st, StepField::gate, fullGate);
                    expected.setStepField(pat, c, st, StepField::nudge, 0);
                    expected.setStepField(pat, c, st, StepField::ratchet, 1);
                }
        require(older.channelRack == expected, "and its steps play the whole sample, on the grid, once");
    }
    {   // A full rack of 64 channels round-trips.
        alien::SessionProjectData full;
        while (full.channelRack.channelCount < maxChannels) full.channelRack.addChannel("Extra", "builtin:hat");
        full.channelRack.setStepOn(0, maxChannels - 1, maxSteps - 1, true);
        full.channelRack.setPatternLength(0, 128);
        alien::SessionProjectData again;
        require(alien::parseSessionProject(alien::serializeSessionProject(full), again, error) && again.channelRack == full.channelRack
                && again.channelRack.channelCount == 64 && again.channelRack.pattern(0).steps[63][127].on, "64 channels and step 128 round-trip");
    }
    {   // Projects saved before the name column could be resized open with the default width.
        auto copy = juce::JSON::parse(json);
        copy.getDynamicObject()->getProperty("channelRack").getDynamicObject()->removeProperty("nameColumnWidth");
        alien::SessionProjectData older;
        require(alien::parseSessionProject(juce::JSON::toString(copy), older, error) && older.channelRack.nameColumnWidth == defaultNameColumn,
                "a rack without a name column width opens with the default one");
    }

    // The default rack is what a project without one (any older file) opens with.
    auto old = juce::JSON::parse(json);   // (a var holds its object by reference: parse afresh for every variant)
    old.getDynamicObject()->removeProperty("channelRack");
    old.getDynamicObject()->setProperty("version", 6);
    alien::SessionProjectData legacy;
    require(alien::parseSessionProject(juce::JSON::toString(old), legacy, error), "a version 6 project (no rack) still opens");
    require(legacy.channelRack == Project::createDefault(), "and gets the starting rack");
    auto missing = juce::JSON::parse(json);
    missing.getDynamicObject()->removeProperty("channelRack");
    require(!alien::parseSessionProject(juce::JSON::toString(missing), legacy, error) && error.isNotEmpty(),
            "a project of the current version must carry its rack");
    auto future = juce::JSON::parse(json);
    future.getDynamicObject()->setProperty("version", alien::sessionProjectFormatVersion + 1);
    require(!alien::parseSessionProject(juce::JSON::toString(future), legacy, error), "newer versions are still refused");

    // Damaged racks are refused rather than opened as something else.
    auto damage = [&](const char* what, auto&& mutate) {
        auto copy = juce::JSON::parse(json);
        auto* rack = copy.getDynamicObject()->getProperty("channelRack").getDynamicObject();
        mutate(rack);
        alien::SessionProjectData out;
        juce::String message;
        if (alien::parseSessionProject(juce::JSON::toString(copy), out, message)) { std::cerr << "accepted: " << what << '\n'; require(false, "damaged rack must be refused"); }
        require(message.isNotEmpty(), "and a reason is given");
    };
    auto firstChannel = [](juce::DynamicObject* rack) { return rack->getProperty("channels").getArray()->getReference(0).getDynamicObject(); };
    auto firstPattern = [](juce::DynamicObject* rack) { return rack->getProperty("patterns").getArray()->getReference(0).getDynamicObject(); };
    damage("volume above 1", [&](auto* r) { firstChannel(r)->setProperty("volume", 1.5); });
    damage("pan below -1", [&](auto* r) { firstChannel(r)->setProperty("pan", -2.0); });
    damage("output beyond tracks", [&](auto* r) { firstChannel(r)->setProperty("output", 16); });
    damage("choke group 9", [&](auto* r) { firstChannel(r)->setProperty("chokeGroup", 9); });
    damage("text volume", [&](auto* r) { firstChannel(r)->setProperty("volume", "loud"); });
    damage("swing above 1", [&](auto* r) { r->setProperty("swing", 7.0); });
    damage("selected pattern beyond count", [&](auto* r) { r->setProperty("selectedPattern", 9); });
    damage("name column too narrow", [&](auto* r) { r->setProperty("nameColumnWidth", minNameColumn - 1); });
    damage("name column too wide", [&](auto* r) { r->setProperty("nameColumnWidth", maxNameColumn + 1); });
    damage("text name column width", [&](auto* r) { r->setProperty("nameColumnWidth", "wide"); });
    damage("pattern length 17", [&](auto* r) { firstPattern(r)->setProperty("length", 17); });
    damage("no patterns", [&](auto* r) { r->getProperty("patterns").getArray()->clear(); });
    damage("65 channels", [&](auto* r) { auto* a = r->getProperty("channels").getArray(); const auto first = a->getReference(0); while (a->size() < 65) a->add(first); });
    damage("pattern length 256", [&](auto* r) { firstPattern(r)->setProperty("length", 256); });
    damage("65 patterns", [&](auto* r) { auto* a = r->getProperty("patterns").getArray(); const auto first = a->getReference(0); while (a->size() < 65) a->add(first); });
    damage("step on a missing channel", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(0, 12); });
    damage("step 129", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(1, maxSteps); });
    auto damageCell = [&](const char* what, int index, juce::var value) {
        damage(what, [&](auto* r) { firstPattern(r)->getProperty("steps").getArray()->getReference(0).getArray()->set(index, value); });
    };
    damageCell("gate 0", 7, 0);
    damageCell("gate 101", 7, 101);
    damageCell("nudge 101", 8, 101);
    damageCell("negative nudge", 8, -1);
    damageCell("ratchet 0", 9, 0);
    damageCell("ratchet 5", 9, 5);
    damageCell("text ratchet", 9, "twice");
    damage("eight-value step record", [&](auto* r) {
        firstPattern(r)->getProperty("steps").getArray()->getReference(0).getArray()->removeRange(8, 2); });
    damage("velocity 0", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(3, 0); });
    damage("probability 101", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(6, 101); });
    damage("pitch 25", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(5, 25); });
    damage("short step record", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->remove(6); });
    damage("sample path too long", [&](auto* r) { firstChannel(r)->setProperty("sample", juce::String::repeatedString("x", 1025)); });
    damage("step 'on' flag of 5", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->set(2, 5); });
    damage("step record with an extra value", [&](auto* r) {
        auto* steps = firstPattern(r)->getProperty("steps").getArray(); steps->getReference(0).getArray()->add(juce::var(0)); });
    damage("numeric mute", [&](auto* r) { firstChannel(r)->setProperty("mute", 1); });
    damage("text solo", [&](auto* r) { firstChannel(r)->setProperty("solo", "yes"); });

    {   // A pattern with no usable name is called "Pattern N" rather than shown blank.
        auto copy = juce::JSON::parse(json);
        auto* patterns = copy.getDynamicObject()->getProperty("channelRack").getDynamicObject()->getProperty("patterns").getArray();
        patterns->getReference(0).getDynamicObject()->setProperty("name", "");
        patterns->getReference(2).getDynamicObject()->setProperty("name", "   ");
        alien::SessionProjectData named;
        juce::String why;
        require(alien::parseSessionProject(juce::JSON::toString(copy), named, why), "blank pattern names do not make a project unreadable");
        require(named.channelRack.patternNames[0] == "Pattern 1" && named.channelRack.patternNames[2] == "Pattern 3"
                && named.channelRack.patternNames[1] == "Fill \xe2\x80\x93 B", "blank names become 'Pattern N' and real names are kept");
    }

    // An invalid rack is never written.
    alien::SessionProjectData bad;
    bad.channelRack.channels[0].volume = 3.0f;
    require(alien::serializeSessionProject(bad).isEmpty(), "an invalid rack is not serialized");
    std::cout << "PASS: rack project format round-trips, upgrades older files and refuses damage\n";
}

static double peakOf(ToneEngine& engine, int samples) {
    std::array<float, 256> left{}, right{};
    double peak = 0;
    for (int offset = 0; offset < samples; offset += 256) {
        float* outputs[]{left.data(), right.data()};
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 256, {});
        for (std::size_t i = 0; i < 256; ++i) peak = std::max(peak, static_cast<double>(std::abs(left[i])));
    }
    return peak;
}
static void startRackPlayback(ToneEngine& engine) {
    engine.patternMode.store(3); engine.bpm.store(120); engine.playing.store(true);
}
static void stopRackPlayback(ToneEngine& engine) {
    engine.playing.store(false); peakOf(engine, 4096);
}

static void controllerEditsAndUndo() {
    ToneEngine engine; engine.prepare(48000);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    int restored = 0;
    c.onDocumentRestored = [&] { ++restored; };

    require(c.channelRack() == Project::createDefault() && !c.canUndo(), "a new project has the starting rack and no history");
    require(c.editRack("Turn step on", [](Project& p) { return p.setStepOn(0, 0, 0, true); }), "an edit is applied");
    require(c.canUndo() && c.undoLabel() == "Turn step on" && c.isDirty(), "and is one undoable, unsaved step");
    startRackPlayback(engine);
    require(peakOf(engine, 3000) > 0.05, "the engine plays the edit with no further action");
    stopRackPlayback(engine);

    require(!c.editRack("Nothing", [](Project& p) { return p.setStepOn(0, 0, 0, true); }), "a change that changes nothing is not an edit");
    require(!c.editRack("Invalid", [](Project& p) { p.channels[0].volume = 9.0f; return true; }) && c.channelRack().channels[0].volume != 9.0f,
            "an edit that makes the rack invalid is refused");
    require(!c.editRack("Declined", [](Project&) { return false; }), "an edit function can decline");
    require(c.undoLabel() == "Turn step on", "none of those touched the history");

    // Choosing a pattern is a view setting: never an undo step.
    require(c.editRack("Add pattern", [](Project& p) { return p.addPattern(); }) && c.channelRack().selectedPattern == 1, "adding a pattern selects it");
    require(c.undoLabel() == "Add pattern", "adding a pattern is undoable");
    require(c.selectRackPattern(0) && c.channelRack().selectedPattern == 0 && c.undoLabel() == "Add pattern", "selecting a pattern is not an edit");
    require(!c.selectRackPattern(0) && !c.selectRackPattern(7), "selecting the same or a missing pattern does nothing");
    startRackPlayback(engine);
    require(peakOf(engine, 3000) > 0.05, "the engine plays the selected pattern (the one with a step)");
    stopRackPlayback(engine);
    c.selectRackPattern(1);
    startRackPlayback(engine);
    require(peakOf(engine, 3000) == 0.0, "and the empty one is silent once it is selected");
    stopRackPlayback(engine);

    // Undo goes back through the history without changing which pattern the user is looking at.
    c.selectRackPattern(1);
    require(c.undo() && c.channelRack().patternCount == 1 && c.channelRack().selectedPattern == 0, "undo removes the pattern; selection stays on a pattern that exists");
    require(c.redo() && c.channelRack().patternCount == 2, "redo brings it back");
    c.selectRackPattern(0);
    require(c.undo() && c.channelRack().patternCount == 1 && c.undo() && !c.channelRack().pattern(0).steps[0][0].on && !c.canUndo(),
            "undoing everything returns to the start");
    require(!c.isDirty(), "which is the saved state");
    startRackPlayback(engine);
    require(peakOf(engine, 3000) == 0.0, "and the engine follows the undo");
    stopRackPlayback(engine);
    require(restored >= 4, "views are told about every undo and redo");

    // Resizing the name column is a view setting too: clamped, never an undo step, and kept through undo and redo.
    require(c.editRack("Turn step on", [](Project& p) { return p.setStepOn(0, 0, 0, true); }), "an edit to undo later");
    require(c.setRackNameColumnWidth(180) && c.channelRack().nameColumnWidth == 180 && c.undoLabel() == "Turn step on",
            "resizing the name column is not an edit");
    require(!c.setRackNameColumnWidth(180), "the same width again changes nothing");
    require(c.setRackNameColumnWidth(5) && c.channelRack().nameColumnWidth == minNameColumn
            && c.setRackNameColumnWidth(5000) && c.channelRack().nameColumnWidth == maxNameColumn, "the width is held to its range");
    c.setRackNameColumnWidth(180);
    require(c.undo() && !c.channelRack().pattern(0).steps[0][0].on && c.channelRack().nameColumnWidth == 180, "undo keeps the width");
    require(c.redo() && c.channelRack().nameColumnWidth == 180, "and so does redo");
    require(c.undo() && !c.canUndo(), "back to the start");

    // A drag of the volume fader is one step.
    for (int i = 1; i <= 8; ++i)
        c.editRack("Change channel", [i](Project& p) { auto ch = p.channels[1]; ch.volume = 0.8f - 0.05f * static_cast<float>(i); return p.setChannel(1, ch); }, "rack-channel-1");
    require(c.undo() && c.channelRack().channels[1].volume == Channel{}.volume && !c.canUndo(), "coalesced edits undo together");

    // Clear history for the following checks.
    c.newProject();
    require(c.channelRack() == Project::createDefault() && !c.canUndo(), "a new project resets the rack and history");
    std::cout << "PASS: rack edits reach the engine and undo/redo correctly\n";
}

static void trackStructureFollowsRoutes() {
    ToneEngine engine; engine.prepare(48000);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    require(c.editRack("Setup", [](Project& p) {
        auto ch = p.channels[0]; ch.output = 2; p.setChannel(0, ch);
        ch = p.channels[1]; ch.output = 3; p.setChannel(1, ch);
        ch = p.channels[2]; ch.output = 0; p.setChannel(2, ch);
        return p.setStepOn(0, 0, 0, true); }), "route channels to tracks");
    require(c.deleteTrack(0), "delete the first track");
    require(c.channelRack().channels[0].output == 1 && c.channelRack().channels[1].output == 2 && c.channelRack().channels[2].output == masterOutput,
            "routes shift down, and a channel whose track was deleted goes to the master");
    require(c.moveTrack(1, 1), "swap tracks 2 and 3");
    require(c.channelRack().channels[0].output == 2 && c.channelRack().channels[1].output == 1, "routes follow the tracks they point at");
    c.commit("Structure");
    require(c.undo() && c.document().trackCount() == 4 && c.channelRack().channels[0].output == 2 && c.channelRack().channels[1].output == 3
            && c.channelRack().channels[2].output == 0, "undo restores the routes together with the tracks");

    // End to end: a channel routed to a muted track is silent until that track is deleted.
    ToneEngine engine2; engine2.prepare(48000);
    alien::ProjectController d(engine2);
    d.pushDocumentToEngine();
    d.editRack("Setup", [](Project& p) { auto ch = p.channels[0]; ch.output = 2; p.setChannel(0, ch); return p.setStepOn(0, 0, 0, true); });
    engine2.tracks[2].mute.store(true); d.trackSettingsEdited(2); d.commit("Mute track 3");
    startRackPlayback(engine2);
    peakOf(engine2, 4096);    // let the mute settle
    stopRackPlayback(engine2);
    startRackPlayback(engine2);
    require(peakOf(engine2, 3000) == 0.0, "a channel routed to a muted track is silent");
    stopRackPlayback(engine2);
    require(d.deleteTrack(2), "delete that track");
    startRackPlayback(engine2);
    require(peakOf(engine2, 3000) > 0.05, "the channel now plays through the master");
    stopRackPlayback(engine2);
    std::cout << "PASS: deleting and moving tracks keeps channel routes correct\n";
}

static std::shared_ptr<alien::SessionAudioClip> toneClip(float level, int frames = 6000) {
    auto clip = std::make_shared<alien::SessionAudioClip>();
    clip->sampleRate = 8000; clip->channels = 1; clip->frames = static_cast<std::size_t>(frames);
    clip->left.assign(static_cast<std::size_t>(frames), level); clip->buildWaveform();
    return clip;
}

static void previewsAndPublishing() {
    ToneEngine engine; engine.prepare(48000);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    NoteQueue::Event event{};
    // Clicking a channel's name queues one preview; nothing is queued for a channel that does not exist.
    c.previewRackChannel(2, 0.5f);
    require(engine.rackAuditions.pop(event) && event.note == 2 && event.velocity == 0.5f, "a preview of channel 3 is queued at the given velocity");
    require(!engine.rackAuditions.pop(event), "and only one");
    c.previewRackChannel(-1);
    c.previewRackChannel(c.channelRack().channelCount);
    c.previewRackChannel(500);
    require(!engine.rackAuditions.pop(event) && !engine.rackAuditions.panic.load(), "previews of channels that do not exist are dropped");

    // The engine is handed a new snapshot only when the rack or its sounds changed.
    require(engine.rackMailbox.consume() != nullptr, "the first push publishes the rack");
    c.pushDocumentToEngine();
    require(engine.rackMailbox.consume() == nullptr, "pushing an unchanged project does not rebuild the snapshot");
    c.editRack("Turn step on", [](Project& p) { return p.setStepOn(0, 0, 0, true); });
    require(engine.rackMailbox.consume() != nullptr, "an edit to the rack publishes a new snapshot");
    require(engine.rackMailbox.consume() == nullptr, "...once");
    c.selectRackPattern(0);
    c.renameTrack(0, "Renamed");
    c.pushDocumentToEngine();
    require(engine.rackMailbox.consume() == nullptr, "an edit elsewhere in the project (a track name) leaves the rack's snapshot alone");
    c.editRack("Add pattern", [](Project& p) { return p.addPattern(); });
    engine.rackMailbox.consume();
    require(c.selectRackPattern(0) && engine.rackMailbox.consume() != nullptr, "choosing another pattern publishes too, since the engine plays the chosen one");
    std::cout << "PASS: rack previews and snapshots are only sent when they should be\n";
}

static void reloadAndRelinkEdgeCases() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioRackReload", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");
    std::atomic<bool> cancel{false}; juce::String error;
    const auto file = root.getChildFile("loud.wav");
    require(alien::writeClipWav(file, *toneClip(0.5f), cancel, error), "write the sample");

    {   // Loading the same file again picks up what is in it now.
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        c.loadRackSample(file, 0); waitIdle(c);
        c.editRack("Step", [](Project& p) { return p.setStepOn(0, 0, 0, true); });
        startRackPlayback(engine);
        const double before = peakOf(engine, 3000);
        stopRackPlayback(engine);
        const auto* oldClip = engine.rackSamples[0].load();
        require(before > 0.1 && oldClip != nullptr, "the first decode plays");
        require(file.deleteFile() && alien::writeClipWav(file, *toneClip(0.2f), cancel, error), "rewrite the file with quieter audio");
        const auto history = c.undoLabel();
        c.loadRackSample(file, 0); waitIdle(c);
        require(engine.rackSamples[0].load() != oldClip && c.undoLabel() == history, "loading the same path again swaps the sound without making another undo step");
        startRackPlayback(engine);
        const double after = peakOf(engine, 3000);
        stopRackPlayback(engine);
        require(after > 0.02 && after < 0.6 * before, "and the engine plays the new audio, not the old decode");
        c.discardRecovery();
    }

    {   // One missing file used by several channels is relinked in every one of them.
        const auto gone = root.getChildFile("gone"), found = root.getChildFile("found");
        require(found.createDirectory().wasOk(), "folder to search");
        const auto shared = found.getChildFile("shared.wav");
        require(alien::writeClipWav(shared, *toneClip(0.3f), cancel, error), "write the shared sample");
        const auto sharedOld = gone.getChildFile("shared.wav").getFullPathName().toStdString();
        const auto otherOld = gone.getChildFile("other.wav").getFullPathName().toStdString();
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        c.editRack("Three", [&](Project& p) { p.addChannel("A", sharedOld); p.addChannel("B", sharedOld); p.addChannel("C", otherOld); return true; });
        require(c.missingMedia().size() == 2, "two files are missing");
        c.relinkMissingAudio(found); waitIdle(c);
        const auto newPath = shared.getFullPathName().toStdString();
        require(c.channelRack().info[4].sample == newPath && c.channelRack().info[5].sample == newPath, "both channels that used the file now point at the found copy");
        require(c.channelRack().info[6].sample == otherOld && c.rackChannelOffline(6) && !c.rackChannelOffline(4) && !c.rackChannelOffline(5),
                "and the channel whose file was not found keeps its reference and stays offline");
        require(c.missingMedia().size() == 1 && c.missingMedia()[0].fileName() == "other.wav", "only the unfound file is still listed");
        c.discardRecovery();
    }

    {   // A channel the user changes while the search runs is left as they changed it.
        const auto gone = root.getChildFile("gone2"), found = root.getChildFile("found2");
        require(found.createDirectory().wasOk(), "folder to search");
        require(alien::writeClipWav(found.getChildFile("late.wav"), *toneClip(0.3f), cancel, error), "write the sample");
        const auto oldPath = gone.getChildFile("late.wav").getFullPathName().toStdString();
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        juce::String lastStatus;
        c.onProjectStatus = [&](const juce::String& text, const juce::String&) { lastStatus = text; };
        c.editRack("Offline", [&](Project& p) { return p.setSample(0, oldPath); });
        require(c.rackChannelOffline(0), "the channel is offline");
        c.relinkMissingAudio(found);                // the search starts...
        require(c.editRack("Back to a built-in", [](Project& p) { return p.setSample(0, "builtin:snare"); }), "...and the user picks another sound before it finishes");
        waitIdle(c);
        require(c.channelRack().info[0].sample == "builtin:snare" && c.undoLabel() == "Back to a built-in", "the finished search does not overwrite the user's choice or add an undo step");
        require(lastStatus.contains("Relinked 0 of 1"), "and nothing is reported as relinked");
        c.discardRecovery();
    }
    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: reloading a sample and relinking handle shared files and edits made mid-search\n";
}

static void saveOpenAndRelink() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioRackProject", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");
    std::atomic<bool> cancel{false}; juce::String error;
    const auto boom = root.getChildFile("samples/sub/boom.wav"), snap = root.getChildFile("samples/snap.wav");
    require(boom.getParentDirectory().createDirectory().wasOk(), "sample folders");
    require(alien::writeClipWav(boom, *toneClip(0.5f), cancel, error) && alien::writeClipWav(snap, *toneClip(0.4f), cancel, error), "write samples");
    const auto song = root.getChildFile("Song.alienstudio");
    const auto portable = root.getChildFile("Portable.alienstudio");

    Project saved;
    {
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        c.loadRackSample(boom, 0); waitIdle(c);
        require(c.channelRack().info[0].sample == boom.getFullPathName().toStdString() && c.channelRack().info[0].name == "boom",
                "loading a sample sets the channel's file and names the channel after it");
        require(!c.rackChannelOffline(0) && c.missingMedia().empty(), "a loaded file is not offline");
        require(c.undoLabel() == "Load channel sample", "loading is one undoable step");
        c.editRack("Add", [&](Project& p) { return p.addChannel("Snap", snap.getFullPathName().toStdString()); });
        // The file was never loaded, only named: it is offline right away.
        require(c.rackChannelOffline(4) && c.missingMedia().size() == 1 && c.missingMedia()[0].rackChannel, "a file the rack names but never loaded is offline");
        c.loadRackSample(snap, 4); waitIdle(c);
        require(!c.rackChannelOffline(4) && c.missingMedia().empty(), "loading it brings it online");
        c.editRack("Steps", [](Project& p) { bool a = p.setStepOn(0, 0, 0, true); bool b = p.setStepOn(0, 4, 4, true); bool s = p.setSwing(0.25f); return a || b || s; });
        saved = c.channelRack();
        startRackPlayback(engine);
        require(peakOf(engine, 3000) > 0.05, "the loaded sample plays");
        stopRackPlayback(engine);
        bool done = false, ok = false;
        c.writeProject(song, false, [&](bool r) { ok = r; done = true; });
        for (int i = 0; i < 300 && !done; ++i) pump(10);
        require(done && ok, "plain save");
        done = false; ok = false;
        c.writeProject(portable, true, [&](bool r) { ok = r; done = true; });
        for (int i = 0; i < 300 && !done; ++i) pump(10);
        require(done && ok, "portable save");
    }
    {   // The files are plain JSON with the rack in it, and the portable one points at collected copies.
        alien::SessionProjectData plain, collected;
        require(alien::parseSessionProject(song.loadFileAsString(), plain, error) && alien::parseSessionProject(portable.loadFileAsString(), collected, error), "both files parse");
        require(plain.channelRack.info[0].sample.find("boom.wav") != std::string::npos && plain.channelRack.info[0].sample.find("_media_") == std::string::npos,
                "a plain save references the sample where it is");
        require(collected.channelRack.info[0].sample.find("_media_") != std::string::npos && collected.channelRack.info[0].sample.find("boom.wav") != std::string::npos
                && !juce::File::isAbsolutePath(juce::String(collected.channelRack.info[0].sample)),
                "a portable save collects it next to the project with a relative path");
        require(collected.channelRack.info[0].sample != collected.channelRack.info[4].sample, "different samples get different copies");
        require(collected.channelRack.info[1].sample == "builtin:clap", "built-in sounds are saved by name, not as files");
    }

    // Reopen the plain save: nothing is lost, and it plays.
    {
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        c.openProject(song); waitIdle(c);
        require(c.channelRack() == saved, "reopening restores the identical rack");
        require(c.missingMedia().empty() && !c.rackChannelOffline(0), "its samples are online");
        require(!c.canUndo() && !c.isDirty(), "a freshly opened project is clean");
        startRackPlayback(engine);
        require(peakOf(engine, 3000) > 0.05, "and it plays");
        stopRackPlayback(engine);
    }
    // The portable copy keeps working when its folder moves somewhere else entirely.
    {
        const auto relocated = root.getChildFile("elsewhere");
        require(relocated.createDirectory().wasOk(), "new home");
        for (const auto& entry : juce::RangedDirectoryIterator(root, false, "*", juce::File::findFilesAndDirectories))
            if (entry.getFile().getFileName().startsWith("Portable")) {
                const auto from = entry.getFile(), to = relocated.getChildFile(from.getFileName());
                require(from.isDirectory() ? from.copyDirectoryTo(to) : from.copyFileTo(to), "copy portable files");
            }
        ToneEngine engine; engine.prepare(48000);
        alien::ProjectController c(engine);
        c.pushDocumentToEngine();
        c.openProject(relocated.getChildFile("Portable.alienstudio")); waitIdle(c);
        require(c.missingMedia().empty() && c.channelRack().pattern(0).steps[4][4].on, "a portable project opens complete after being moved");
    }

    // Move the samples away: the project still opens, with the channel offline.
    const auto moved = root.getChildFile("moved");
    require(root.getChildFile("samples").moveFileTo(moved), "move samples away");
    const auto empty = root.getChildFile("empty");
    require(empty.createDirectory().wasOk(), "empty folder");
    ToneEngine engine; engine.prepare(48000);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    juce::String lastStatus, lastTip;
    c.onProjectStatus = [&](const juce::String& text, const juce::String& tip) { lastStatus = text; lastTip = tip; };
    c.openProject(song); waitIdle(c);
    require(c.channelRack().sameContent(saved) && c.channelRack().info[0].sample == saved.info[0].sample, "the rack opens intact even though its samples are gone");
    require(c.rackChannelOffline(0) && c.rackChannelOffline(4) && !c.rackChannelOffline(1), "the two file channels are offline; built-ins are not");
    const auto missing = c.missingMedia();
    require(missing.size() == 2 && missing[0].rackChannel && missing[0].track == 0 && missing[0].fileName() == "boom.wav"
            && missing[1].rackChannel && missing[1].track == 4 && missing[1].fileName() == "snap.wav", "both are listed with their channel");
    require(lastStatus.contains("offline") && lastTip.contains("Channel Rack, channel 1") && lastTip.contains("boom.wav"), "the user is told which channels");
    startRackPlayback(engine);
    require(peakOf(engine, 3000) == 0.0, "offline channels are silent and nothing breaks");
    stopRackPlayback(engine);
    // Saving keeps the references; a portable save says what is missing.
    bool done = false, ok = false;
    const auto resaved = root.getChildFile("Resaved.alienstudio");
    c.writeProject(resaved, false, [&](bool r) { ok = r; done = true; });
    for (int i = 0; i < 300 && !done; ++i) pump(10);
    alien::SessionProjectData reread;
    require(done && ok && alien::parseSessionProject(resaved.loadFileAsString(), reread, error) && reread.channelRack.info[0].sample.find("boom.wav") != std::string::npos,
            "saving with offline samples keeps the references");
    done = false;
    c.writeProject(root.getChildFile("Portable2.alienstudio"), true, [&](bool r) { ok = r; done = true; });
    for (int i = 0; i < 300 && !done; ++i) pump(10);
    require(done && !ok && lastStatus.contains("Missing audio"), "a portable save reports the missing sample");

    c.relinkMissingAudio(empty); waitIdle(c);
    require(c.missingMedia().size() == 2 && lastStatus.contains("Relinked 0 of 2"), "the wrong folder relinks nothing");
    const auto marker = c.undoLabel();
    c.relinkMissingAudio(moved); waitIdle(c);
    require(c.missingMedia().empty() && lastStatus.contains("Relinked 2 of 2"), "searching the right folder finds both");
    require(c.channelRack().info[0].sample == moved.getChildFile("sub/boom.wav").getFullPathName().toStdString()
            && c.channelRack().info[4].sample == moved.getChildFile("snap.wav").getFullPathName().toStdString(), "channels point at the found files");
    require(c.undoLabel() == "Relink audio" && c.isDirty(), "relinking is one undoable, unsaved step");
    startRackPlayback(engine);
    require(peakOf(engine, 3000) > 0.05, "and the relinked sample plays");
    stopRackPlayback(engine);
    require(c.undo() && c.missingMedia().size() == 2 && c.undoLabel() == marker, "undo returns both to offline");
    require(c.redo() && c.missingMedia().empty(), "redo relinks again");

    // A sample used by two channels is one missing file and one relink.
    ToneEngine engine2; engine2.prepare(48000);
    alien::ProjectController d(engine2);
    d.pushDocumentToEngine();
    d.editRack("Two", [&](Project& p) {
        p.addChannel("A", "C:/nowhere/shared.wav"); p.addChannel("B", "C:/nowhere/shared.wav"); p.addChannel("C", "C:/nowhere/other.wav"); return true; });
    require(d.missingMedia().size() == 2, "two channels with one missing file are listed once");

    c.discardRecovery();
    alien::ProjectController::recoveryDirectoryOverride() = juce::File{};
    root.deleteRecursively();
    std::cout << "PASS: rack samples save, collect, reopen offline, and relink\n";
}

// What an arrangement export is handed: the sound for each channel, kept alive while it renders.
static void exportHandOff() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioRackExport", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    alien::ProjectController::recoveryDirectoryOverride() = root.getChildFile("Recovery");
    ToneEngine engine; engine.prepare(48000);
    alien::ProjectController c(engine);
    c.pushDocumentToEngine();
    const int count = c.channelRack().channelCount;
    require(count == 4, "the default rack has four channels");

    std::vector<std::shared_ptr<const alien::SessionAudioClip>> keep;
    auto clips = c.rackClipsForExport(keep);
    for (int i = 0; i < count; ++i)
        require(clips[static_cast<std::size_t>(i)] != nullptr && clips[static_cast<std::size_t>(i)] == engine.rackSamples[static_cast<std::size_t>(i)].load(),
                "an export plays each built-in channel's sound, the very one the engine plays");
    for (int i = count; i < maxChannels; ++i) require(clips[static_cast<std::size_t>(i)] == nullptr, "channels the rack does not have are silent");
    require(keep.empty(), "built-in sounds live for the whole program and need no keep-alive");

    // A loaded file replaces the built-in sound of its channel, and is held for the length of the render.
    std::atomic<bool> cancel{false}; juce::String error;
    const auto file = root.getChildFile("export-tone.wav");
    require(alien::writeClipWav(file, *toneClip(0.5f), cancel, error), "write a sample");
    c.loadRackSample(file, 1); waitIdle(c);
    keep.clear();
    clips = c.rackClipsForExport(keep);
    require(clips[1] != nullptr && clips[1] == engine.rackSamples[1].load() && clips[1]->frames == 6000, "a loaded file is what channel 2 exports");
    require(keep.size() == 1 && keep[0].get() == clips[1], "and the export keeps that audio alive");
    require(clips[0] == engine.rackSamples[0].load() && clips[2] == engine.rackSamples[2].load() && clips[3] == engine.rackSamples[3].load(), "the other channels are unchanged");

    // A channel whose file is not loaded (missing) exports silence, and keeps nothing alive.
    require(c.editRack("Add a missing sample", [](Project& p) { return p.addChannel("Ghost", "C:/Nowhere/ghost.wav"); }), "add a channel whose file does not exist");
    keep.clear();
    clips = c.rackClipsForExport(keep);
    require(c.channelRack().channelCount == 5 && clips[4] == nullptr && keep.size() == 1, "a missing sample exports silence and is not kept");
    require(clips[1] != nullptr, "a missing file elsewhere does not disturb the loaded one");
    c.discardRecovery();
    root.deleteRecursively();
    std::cout << "PASS: the export is handed the same sounds the engine plays\n";
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    roundTripAndValidation();
    controllerEditsAndUndo();
    previewsAndPublishing();
    trackStructureFollowsRoutes();
    saveOpenAndRelink();
    reloadAndRelinkEdgeCases();
    exportHandOff();
    return 0;
}
