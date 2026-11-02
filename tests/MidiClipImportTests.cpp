// MIDI clip import (BROWSE-01): ticks at any PPQ land on the piano roll's 240-tick sixteenths, note-ons and
// note-offs pair without ever leaving a note held, tempo is reported but never stretches notes, the piano roll
// inserts a clip at its cursor as one undoable edit, General MIDI drum grooves become Channel Rack steps, and
// every Starter Pack clip imports.
#include "MidiClipImport.h"
#include "MidiClipDrag.h"
#include "SampleDrag.h"
#include "PianoRollEditor.h"
#include "RackGrooveImport.h"
#include "StarterPack.h"
#include <cstdlib>
#include <iostream>
#include <map>

using namespace alien;

static void require(bool ok, const juce::String& message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static const juce::File pack{juce::String(ALIEN_STARTER_PACK_DIR)};

// A file event: `tick` in the file's own PPQ. Velocity 0 on a note-on is a note-off written the running-status way.
struct Event { int track; double tick; juce::MidiMessage message; };
static juce::MidiMessage on(int key, int velocity = 100, int channel = 1) { return juce::MidiMessage::noteOn(channel, key, static_cast<juce::uint8>(velocity)); }
static juce::MidiMessage off(int key, int channel = 1) { return juce::MidiMessage::noteOff(channel, key); }

// Writes the events to a Standard MIDI file in memory and reads it back, so the real parser runs.
static midi_import::ImportedClip roundTrip(int ppq, const std::vector<Event>& events, double tempo = 0.0) {
    juce::MidiFile file;
    file.setTicksPerQuarterNote(ppq);
    int tracks = 1;
    for (const auto& e : events) tracks = std::max(tracks, e.track + 1);
    for (int t = 0; t < tracks; ++t) {
        juce::MidiMessageSequence sequence;
        if (t == 0 && tempo > 0.0) sequence.addEvent(juce::MidiMessage::tempoMetaEvent(juce::roundToInt(60.0e6 / tempo)), 0.0);
        for (const auto& e : events) if (e.track == t) sequence.addEvent(e.message, e.tick);
        file.addTrack(sequence);
    }
    juce::MemoryOutputStream out;
    require(file.writeTo(out), "the test file writes");
    juce::MemoryInputStream in(out.getData(), out.getDataSize(), false);
    return midi_import::read(in);
}

static void ticksAtAnyPpq() {
    for (const int ppq : {24, 96, 480, 960, 1920}) {
        // A quarter note on beat 2, then a sixteenth on beat 3's "e".
        const auto clip = roundTrip(ppq, {{0, ppq * 1.0, on(60)}, {0, ppq * 2.0, off(60)},
                                          {0, ppq * 2.25, on(62)}, {0, ppq * 2.5, off(62)}});
        require(clip.ok() && clip.filePpq == ppq && clip.notes.size() == 2, "reads at PPQ " + juce::String(ppq));
        require(clip.notes[0].startTicks == 960 && clip.notes[0].lengthTicks == 960, "a quarter note is 960 project ticks at PPQ " + juce::String(ppq));
        require(clip.notes[1].startTicks == 2160 && clip.notes[1].lengthTicks == 240, "a sixteenth is one 240-tick step at PPQ " + juce::String(ppq));
        require(clip.lengthTicks == 16 * 240, "the clip is one bar");
    }
    require(midi_import::toProjectTicks(1, 480) == 2 && midi_import::toProjectTicks(1, 1920) == 1 && midi_import::toProjectTicks(1, 3840) == 0,
            "a finer file tick rounds to the nearest project tick");
    require(midi_import::toProjectTicks(100, 0) == 0 && midi_import::toProjectTicks(-5, 480) == 0, "a bad PPQ or negative time is the start");
    const auto notes = midi_import::pianoRollNotes(roundTrip(480, {{0, 240, on(64, 64)}, {0, 600, off(64)}}));
    require(notes.size() == 1 && notes[0].startStep == 2.0 && notes[0].lengthSteps == 3.0 && std::abs(notes[0].velocity - 64.0f / 127.0f) < 1e-6f,
            "as piano-roll notes, times are sixteenths and velocity is a fraction");
    std::puts("PASS: ticks at 24 to 1920 PPQ land on the 960 PPQ grid");
}

static void notePairing() {
    // Velocity-0 note-on ends a note.
    auto clip = roundTrip(480, {{0, 0, on(60)}, {0, 480, on(60, 0)}});
    require(clip.notes.size() == 1 && clip.notes[0].lengthTicks == 960 && clip.strayOffs == 0, "a velocity-0 note-on is a note-off");
    // A second note-on on a held key retriggers: the first ends there, the off ends the second.
    clip = roundTrip(480, {{0, 0, on(60)}, {0, 240, on(60, 90)}, {0, 960, off(60)}, {0, 1200, off(60)}});
    require(clip.notes.size() == 2 && clip.notes[0].lengthTicks == 480 && clip.notes[1].startTicks == 480 && clip.notes[1].lengthTicks == 1440
            && clip.retriggered == 1 && clip.strayOffs == 1, "an overlapping same-key note-on retriggers, and the extra note-off is ignored");
    // Off then on at one tick is a repeat, not a retrigger.
    clip = roundTrip(480, {{0, 0, on(60)}, {0, 480, off(60)}, {0, 480, on(60)}, {0, 960, off(60)}});
    require(clip.notes.size() == 2 && clip.notes[0].lengthTicks == 960 && clip.notes[1].lengthTicks == 960 && clip.retriggered == 0, "back-to-back notes stay two notes");
    // On and off on one tick is a zero-length hit: a thirty-second, or less when the next hit on its key is closer.
    clip = roundTrip(480, {{0, 0, on(36)}, {0, 0, off(36)}, {0, 30, on(36)}, {0, 30, off(36)}, {0, 480, on(38)}, {0, 480, off(38)}});
    require(clip.notes.size() == 3 && clip.zeroLength == 3 && clip.notes[0].lengthTicks == 60 && clip.notes[1].lengthTicks == midi_import::zeroLengthTicks
            && clip.notes[2].lengthTicks == midi_import::zeroLengthTicks, "zero-length hits get a length that never overlaps the next on their key");
    // A note never released ends at the file's last event; one released by nothing at all still gets a length.
    clip = roundTrip(480, {{0, 0, on(60)}, {0, 0, on(64)}, {0, 1920, off(64)}});
    require(clip.notes.size() == 2 && clip.unterminated == 1 && clip.notes[0].lengthTicks == 3840, "an unterminated note ends at the last event");
    clip = roundTrip(480, {{0, 0, on(60)}});
    require(clip.notes.size() == 1 && clip.notes[0].lengthTicks == midi_import::zeroLengthTicks, "a lone note-on still ends");
    // The same key on two tracks: the earlier note ends where the later starts; two notes starting together are one.
    clip = roundTrip(480, {{0, 0, on(60)}, {0, 1920, off(60)}, {1, 480, on(60)}, {1, 960, off(60)}, {1, 0, on(60, 50)}, {1, 120, off(60)}});
    require(clip.notes.size() == 2 && clip.notes[0].startTicks == 0 && clip.notes[0].lengthTicks == 960 && clip.notes[1].startTicks == 960,
            "notes on one key never overlap, across tracks too");
    // Different channels and keys are independent.
    clip = roundTrip(480, {{0, 0, on(60, 100, 1)}, {0, 0, on(60, 100, 2)}, {0, 480, off(60, 2)}, {0, 960, off(60, 1)}});
    require(clip.notes.size() == 2 && clip.notes[0].channel == 1 && clip.notes[0].lengthTicks == 1920 && clip.notes[1].lengthTicks == 960, "channels pair separately");
    std::puts("PASS: note-ons and note-offs pair without leaving a note held");
}

// Plays `pattern` for two loops and stops: every key that goes down comes up, and none goes down twice.
static void playsWithoutStuckNotes(const PianoRollPattern& pattern, const juce::String& name) {
    require(pattern.valid(), "a valid pattern: " + name);
    for (std::size_t i = 0; i < pattern.count; ++i) for (std::size_t j = 0; j < pattern.count; ++j) if (i != j) {
        const auto& a = pattern.notes[i]; const auto& b = pattern.notes[j];
        require(a.pitch != b.pitch || a.startStep + a.lengthSteps <= b.startStep + 1e-9 || b.startStep + b.lengthSteps <= a.startStep + 1e-9,
                "no two notes overlap on one key: " + name);
    }
    auto scheduler = std::make_unique<PianoRollScheduler>();
    std::array<int, 128> down{};
    bool doubled = false;
    const auto send = [&](int pitch, float, bool isOn) {
        if (isOn && down[static_cast<std::size_t>(pitch)]++ != 0) doubled = true;
        if (!isOn) --down[static_cast<std::size_t>(pitch)];
    };
    auto mailbox = std::make_unique<PianoRollMailbox>();
    require(mailbox->publish(pattern), "publishes: " + name);
    const double rate = 8000.0, bpm = 120.0;
    const int samples = static_cast<int>(2.0 * pattern.loopSteps * rate * 60.0 / (bpm * 4.0)) + 64;
    scheduler->beginBlock(*mailbox, true, rate, bpm, send);
    for (int i = 0; i < samples; ++i) scheduler->nextSample(send);
    scheduler->beginBlock(*mailbox, false, rate, bpm, send);
    require(!doubled, "no key is pressed twice without a release: " + name);
    for (int pitch = 0; pitch < 128; ++pitch) require(down[static_cast<std::size_t>(pitch)] == 0, "no note is left held after stop: " + name);
}

static void noStuckNotes() {
    const auto clip = roundTrip(96, {{0, 0, on(60)}, {0, 24, on(60)}, {0, 24, on(64)}, {0, 24, off(64)}, {0, 48, off(60)},
                                     {0, 96, on(67)}, {1, 120, on(67)}, {1, 200, off(67)}, {0, 300, off(67)}, {0, 380, on(72)}});
    auto pattern = std::make_unique<PianoRollPattern>();
    pattern->loopSteps = 16;
    const auto notes = midi_import::pianoRollNotes(clip);
    const auto result = insertNotes(*pattern, notes, 0.0);
    require(result.added == static_cast<int>(notes.size()), "every repaired note is placed");
    playsWithoutStuckNotes(*pattern, "a file of retriggers, zero-length and unterminated notes");
    std::puts("PASS: an imported clip plays and stops with no stuck notes");
}

static void tempoIsReportedNotApplied() {
    const std::vector<Event> events{{0, 480, on(60)}, {0, 960, off(60)}};
    const auto slow = roundTrip(480, events, 60.0), fast = roundTrip(480, events, 180.0), none = roundTrip(480, events);
    require(std::abs(slow.fileTempo - 60.0) < 0.01 && std::abs(fast.fileTempo - 180.0) < 0.01 && none.fileTempo == 0.0, "the file's tempo is read");
    require(slow.notes[0].startTicks == fast.notes[0].startTicks && slow.notes[0].lengthTicks == fast.notes[0].lengthTicks
            && slow.notes[0].startTicks == none.notes[0].startTicks, "but notes are placed in beats whatever it is");
    NoteInsertResult result; result.added = 1;
    require(midi_import::describe(slow, result, "Slow").contains("60 BPM is not applied"), "and the status line says so");
    // SMPTE timing has no beats to map.
    juce::MidiFile smpte;
    smpte.setSmpteTimeFormat(25, 40);
    juce::MidiMessageSequence sequence; sequence.addEvent(on(60), 0); sequence.addEvent(off(60), 100);
    smpte.addTrack(sequence);
    require(!midi_import::fromMidiFile(smpte).ok(), "an SMPTE-timed file is refused");
    juce::MemoryInputStream junk("not a midi file", 15, false);
    require(!midi_import::read(junk).ok() && !midi_import::read(pack.getChildFile("missing.mid")).ok(), "junk and missing files are refused");
    require(!roundTrip(480, {}).ok(), "a file with no notes is refused");
    std::puts("PASS: tempo is reported, never applied; SMPTE and broken files are refused");
}

static void insertAtCursor() {
    auto pattern = std::make_unique<PianoRollPattern>();
    pattern->loopSteps = 16;
    pattern->add(48, 0, 1);
    const std::array<PianoRollNote, 3> notes{{{0, 60, 0.0, 2.0, 0.5f, 1.0f}, {0, 62, 3.0, 6.0, 0.5f, 1.0f}, {0, 64, 9.0, 1.0, 0.5f, 1.0f}}};
    auto result = insertNotes(*pattern, notes, 8.0);
    require(result.added == 2 && result.trimmed == 1 && result.skipped == 1 && pattern->count == 3 && pattern->valid(),
            "notes past the loop's end are left out and one crossing it is cut there");
    require(pattern->find(result.selection.ids[0])->startStep == 8.0 && pattern->find(result.selection.ids[1])->lengthSteps == 5.0,
            "placed at the cursor, the crossing note cut to fit");
    require(pattern->loopSteps == 16 && pattern->notes[0].pitch == 48, "the loop's length and its notes stay");
    result = insertNotes(*pattern, notes, 16.0);
    require(!result.changed() && result.skipped == 3, "nothing fits at the loop's end");
    auto full = std::make_unique<PianoRollPattern>();
    for (int i = 0; i < static_cast<int>(PianoRollPattern::maxNotes) - 1; ++i) full->add(i % 128, (i / 128) * 2.0, 1.0);
    result = insertNotes(*full, notes, 0.0);
    require(result.added == 1 && result.skipped == 2 && full->count == PianoRollPattern::maxNotes, "it stops at the note limit");
    std::puts("PASS: a clip inserts at the cursor without leaving the loop");
}

static void insertGrowsTheLoop() {
    const std::array<PianoRollNote, 3> notes{{{0, 60, 0.0, 2.0, 0.5f, 1.0f}, {0, 62, 3.0, 6.0, 0.5f, 1.0f}, {0, 64, 9.0, 1.0, 0.5f, 1.0f}}};
    const LoopGrowth fourFour{10.0};
    auto pattern = std::make_unique<PianoRollPattern>();
    pattern->loopSteps = 16;
    pattern->add(48, 15, 1);
    auto result = insertNotes(*pattern, notes, 8.0, &fourFour);
    require(result.added == 3 && result.trimmed == 0 && result.skipped == 0 && pattern->valid(), "with growth every note fits");
    require(pattern->loopSteps == 32 && result.grewLoop() && result.loopFrom == 16 && result.loopTo == 32,
            "the loop grows to the next whole bar after the block's end");
    require(pattern->find(result.selection.ids[1])->lengthSteps == 6.0 && pattern->notes[0].startStep == 15.0, "nothing is cut or moved");
    // Already long enough: no change to the loop.
    result = insertNotes(*pattern, notes, 0.0, &fourFour);
    require(result.added == 3 && !result.grewLoop() && pattern->loopSteps == 32, "a loop that holds the block keeps its length");
    // The cursor at the loop's very end still places the clip after it.
    auto atEnd = std::make_unique<PianoRollPattern>();
    atEnd->loopSteps = 16;
    result = insertNotes(*atEnd, notes, 16.0, &fourFour);
    require(result.added == 3 && atEnd->loopSteps == 32 && atEnd->find(result.selection.ids[0])->startStep == 16.0, "a cursor at the end appends");
    // Bars of 3/4 are twelve sixteenths.
    auto waltz = std::make_unique<PianoRollPattern>();
    waltz->loopSteps = 16;
    const LoopGrowth threeFour{20.0, 12 * PianoRollPattern::ticksPerStep};
    require(grownLoopSteps(*waltz, 0.0, threeFour) == 24, "two bars of 3/4 hold twenty sixteenths");
    // Never past the longest loop: the notes beyond it are cut as before.
    const std::array<PianoRollNote, 2> late{{{0, 60, 0.0, 1.0, 0.5f, 1.0f}, {0, 62, 250.0, 20.0, 0.5f, 1.0f}}};
    const LoopGrowth huge{270.0};
    auto capped = std::make_unique<PianoRollPattern>();
    result = insertNotes(*capped, late, 0.0, &huge);
    require(capped->loopSteps == PianoRollPattern::maxLoopSteps && result.trimmed == 1 && capped->valid(), "the loop stops at its longest");
    // Nothing added: the loop is left alone.
    auto full = std::make_unique<PianoRollPattern>();
    full->loopSteps = 16;
    for (int i = 0; i < static_cast<int>(PianoRollPattern::maxNotes); ++i) full->add(i % 128, (i / 128) * 2.0, 1.0);
    result = insertNotes(*full, notes, 8.0, &fourFour);
    require(!result.changed() && full->loopSteps == 16, "a full clip keeps its length");
    std::puts("PASS: an insert may lengthen the loop to whole bars, never shortening or cutting what is there");
}

static void timeSignatureSetsTheBar() {
    const auto signature = [](int beats, int unit) { return juce::MidiMessage::timeSignatureMetaEvent(beats, unit); };
    // Four quarter notes of 3/4: one bar and a beat, so two bars of twelve sixteenths.
    const auto waltz = roundTrip(480, {{0, 0, signature(3, 4)}, {0, 0, on(60)}, {0, 1920, off(60)}});
    require(waltz.ok() && waltz.beatsPerBar == 3 && waltz.beatUnit == 4 && waltz.barTicks == 12 * 240, "3/4 is read");
    require(waltz.lengthTicks == 24 * 240 && waltz.notes[0].startTicks == 0 && waltz.notes[0].lengthTicks == 3840,
            "the clip is whole bars of 3/4; the note keeps its length in beats");
    const auto sixEight = roundTrip(480, {{0, 0, signature(6, 8)}, {0, 0, on(60)}, {0, 240, off(60)}});
    require(sixEight.barTicks == 12 * 240 && sixEight.lengthTicks == 12 * 240, "a bar of 6/8 is twelve sixteenths");
    const auto plain = roundTrip(480, {{0, 0, on(60)}, {0, 480, off(60)}});
    require(plain.beatsPerBar == 4 && plain.beatUnit == 4 && plain.lengthTicks == 16 * 240, "no signature is 4/4");
    const auto changes = roundTrip(480, {{0, 0, signature(3, 4)}, {0, 1440, signature(4, 4)}, {0, 0, on(60)}, {0, 480, off(60)}});
    require(changes.beatsPerBar == 3, "the first signature is the clip's");
    require(midi_import::barTicksOf(5, 8) == 10 * 240 && midi_import::barTicksOf(0, 4) == 0 && midi_import::barTicksOf(4, 3) == 0
            && midi_import::barTicksOf(4, 128) == 0, "a bar is any count over a power-of-two beat, and nonsense is refused");
    // The status line says how far the clip grew, in bars of the file's signature.
    NoteInsertResult grew; grew.added = 1; grew.loopFrom = 12; grew.loopTo = 24;
    require(midi_import::describe(waltz, grew, "Waltz").contains("lengthened from 1 bar of 3/4 to 2 bars of 3/4"), "3/4 growth is described");
    grew.loopFrom = 64; grew.loopTo = 128;
    require(midi_import::describe(plain, grew, "Plain").contains("lengthened from 4 bars to 8 bars"), "4/4 growth is described");
    grew.loopFrom = 16;
    require(midi_import::describe(waltz, grew, "Waltz").contains("from 16 sixteenths"), "a length that is not whole bars is in sixteenths");
    std::puts("PASS: the file's time signature sets the bar; notes keep their places in beats");
}

static void editorImportIsOneUndoableEdit() {
    PianoRollEditor editor;
    editor.setSize(900, 500);
    int commits = 0, changes = 0;
    editor.onEditCommitted = [&] { ++commits; };
    editor.onChange = [&] { ++changes; };
    PianoRollPattern start;
    start.add(40, 0, 4);
    editor.setPattern(start);
    changes = 0;
    editor.setInsertionStep(16.0);
    const auto clip = roundTrip(480, {{0, 0, on(60)}, {0, 0, on(64)}, {0, 0, on(67)}, {0, 1920, off(60)}, {0, 1920, off(64)}, {0, 1920, off(67)}});
    const auto notes = midi_import::pianoRollNotes(clip);
    const auto result = editor.importNotes(notes);
    require(result.added == 3 && editor.getPattern().count == 4 && commits == 1 && changes >= 1, "the import is one committed edit");
    require(editor.getSelectedIds().size() == 3, "the imported notes are selected");
    for (const auto id : editor.getSelectedIds()) require(editor.getPattern().find(id)->startStep == 16.0, "at the cursor");
    editor.undo();
    require(editor.getPattern().count == 1 && editor.getPattern().notes[0].pitch == 40 && commits == 2, "one undo takes the whole clip back out");
    editor.redo();
    require(editor.getPattern().count == 4, "and redo puts it back");
    editor.setLocked(true);
    require(!editor.importNotes(notes).changed() && editor.getPattern().count == 4, "a locked editor refuses");
    editor.setLocked(false);
    editor.showStatus(midi_import::describe(clip, result, "Chords"));
    require(editor.getStatus().startsWith("Imported 3 notes from Chords"), "the status line says what was imported");
    // Eight bars into a four-bar clip: the loop grows in the same edit, and one undo restores both.
    PianoRollPattern fourBars;
    editor.setPattern(fourBars);
    commits = 0;
    std::vector<Event> eightBars;
    for (int bar = 0; bar < 8; ++bar) { eightBars.push_back({0, bar * 1920.0, on(60 + bar)}); eightBars.push_back({0, bar * 1920.0 + 1920, off(60 + bar)}); }
    const auto progression = roundTrip(480, eightBars);
    const auto growth = midi_import::loopGrowth(progression);
    const auto grown = editor.importNotes(midi_import::pianoRollNotes(progression), &growth);
    require(grown.added == 8 && grown.trimmed == 0 && grown.skipped == 0 && editor.getPattern().loopSteps == 128 && commits == 1,
            "an eight-bar clip lengthens a four-bar loop in one edit");
    editor.undo();
    require(editor.getPattern().loopSteps == 64 && editor.getPattern().count == 0, "one undo restores the length and removes the notes");
    editor.redo();
    require(editor.getPattern().loopSteps == 128 && editor.getPattern().count == 8, "and redo brings both back");
    std::puts("PASS: the piano roll imports at its cursor as one undoable edit");
}

// A MIDI clip dragged from the sample browser drops at the snapped step under the pointer and imports from there.
static void editorTakesDroppedClips() {
    PianoRollEditor editor;
    editor.setSize(900, 500);
    PianoRollPattern start;
    editor.setPattern(start);
    juce::Component source;
    using Details = juce::DragAndDropTarget::SourceDetails;
    const juce::File clip("C:/Clips/Chords.mid");
    const auto payload = mididrag::encode(clip);
    const Details nowhere(payload, &source, {450, 250});
    require(!editor.isInterestedInDragSource(nowhere), "without an importer the editor takes no drops");
    juce::File imported; double importedAt = -1.0; int imports = 0;
    editor.onImportMidiFile = [&](const juce::File& file) { imported = file; importedAt = editor.getInsertionStep(); ++imports; };
    require(editor.isInterestedInDragSource(nowhere), "with one, it takes a dragged MIDI clip");
    require(!editor.isInterestedInDragSource(Details(sampledrag::encode(juce::File("C:/kick.wav")), &source, {450, 250})), "but not a sample drag");
    // Halfway across the grid lands on the middle of the 64-step loop, snapped.
    const Details middle(payload, &source, {64 + (900 - 64 - 26) / 2, 250});
    editor.itemDragEnter(middle);
    const int target = editor.midiDropTarget();
    require(target >= 28 && target <= 32, "the drop marker follows the pointer: " + juce::String(target));
    editor.itemDropped(middle);
    require(imports == 1 && imported == clip && importedAt == target && editor.midiDropTarget() < 0, "dropping imports at the marked step");
    // Left of the grid is its first step; right of it its last snapped step.
    editor.itemDropped(Details(payload, &source, {0, 250}));
    require(imports == 2 && importedAt == 0.0, "a drop left of the grid starts at step 1");
    editor.itemDropped(Details(payload, &source, {899, 250}));
    require(imports == 3 && importedAt > 56.0 && importedAt < 64.0, "a drop right of the grid starts on its last snapped step");
    editor.itemDragExit(middle);
    require(editor.midiDropTarget() < 0, "leaving clears the marker");
    editor.setLocked(true);
    editor.itemDragMove(middle);
    require(editor.midiDropTarget() < 0, "a locked editor shows no marker");
    editor.itemDropped(middle);
    require(imports == 3 && editor.getStatus().startsWith("No changes"), "and refuses the drop, saying why");
    std::puts("PASS: the piano roll takes a dragged MIDI clip at the step under the pointer");
}

static rack_groove::Report importInto(rack::Project& p, const midi_import::ImportedClip& clip, const std::string& name) {
    rack_groove::Report report;
    require(rack_groove::importGroove(p, clip, name, &report) && p.valid(), "imports: " + juce::String(name));
    return report;
}

static void drumGrooveToRack() {
    using rack_groove::Role;
    require(rack_groove::roleOf(36) == Role::kick && rack_groove::roleOf(40) == Role::snare && rack_groove::roleOf(22) == Role::closedHat
            && rack_groove::roleOf(44) == Role::closedHat && rack_groove::roleOf(46) == Role::openHat && rack_groove::roleOf(26) == Role::openHat
            && rack_groove::roleOf(45) == Role::tom && rack_groove::roleOf(51) == Role::cymbal && rack_groove::roleOf(49) == Role::cymbal
            && rack_groove::roleOf(37) == Role::rim && rack_groove::roleOf(56) == Role::cowbell && rack_groove::roleOf(70) == Role::percussion,
            "General MIDI drum keys have roles");
    constexpr int d = midi_import::gmDrumChannel;
    // One bar at 480 PPQ: kick on 1 and 3, snare on 2 late by 48 ticks (a tenth of an eighth = 40% of a step),
    // hat on every eighth with one played 12 ticks early, an open hat, a crash, two hits on one step, and a piano note.
    std::vector<Event> events{{0, 0, on(36, 110, d)}, {0, 960, on(36, 100, d)}, {0, 528, on(38, 90, d)},
                              {0, 0, on(49, 120, d)}, {0, 1800, on(46, 80, d)}, {0, 1440, on(38, 40, d)}, {0, 1450, on(40, 70, d)},
                              {0, 100, on(72, 100, 1)}, {0, 200, off(72, 1)}};
    for (int i = 0; i < 8; ++i) events.push_back({0, i * 240.0 - (i == 3 ? 12.0 : 0.0), on(42, 60, d)});
    for (auto& e : std::vector<Event>(events)) if (e.message.isNoteOn() && e.message.getChannel() == d) events.push_back({0, e.tick + 30, off(e.message.getNoteNumber(), d)});
    const auto clip = roundTrip(480, events);
    require(clip.ok() && rack_groove::hasDrums(clip), "the groove reads");

    auto p = rack::Project::createDefault();   // Kick, Clap, Hat, Snare
    p.patterns[0] = std::make_shared<const rack::Pattern>();
    const auto first = p.pattern(0);
    const auto report = importInto(p, clip, "Test groove");
    const auto& pattern = p.pattern(report.pattern);
    const auto step = [&](int channel, int s) { return pattern.steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(s)]; };
    require(p.patternCount == 2 && report.pattern == 1 && p.selectedPattern == 1 && p.patternNames[1] == "Test groove" && pattern.length == 16,
            "a new selected pattern, named, as long as the groove");
    require(p.pattern(0) == first, "the pattern that was there is untouched");
    require(step(0, 0).on && step(0, 0).velocity == 110 && step(0, 8).on && step(0, 0).nudge == 0, "kicks on the Kick channel");
    require(step(3, 4).on && step(3, 4).nudge == 40 && step(3, 4).velocity == 90, "a late snare keeps its feel as nudge");
    require(step(3, 12).on && step(3, 12).velocity == 70 && report.merged == 1, "two hits on one step keep the louder");
    for (int i = 0; i < 8; ++i) require(step(2, i * 2).on && step(2, i * 2).nudge == 0, "hats on the Hat channel, an early one on its beat");
    require(!step(1, 0).on, "nothing on the Clap channel");
    require(p.channelCount == 6 && report.added.size() == 2, "an open hat and a cymbal channel are added");
    const int openHat = report.channelOf[static_cast<std::size_t>(Role::openHat)], cymbal = report.channelOf[static_cast<std::size_t>(Role::cymbal)];
    require(p.info[static_cast<std::size_t>(openHat)].sample == "builtin:openhat" && step(openHat, 15).on, "the open hat on a new built-in open hat");
    require(p.info[static_cast<std::size_t>(cymbal)].name == "Cymbal" && step(cymbal, 0).on && step(cymbal, 0).velocity == 120, "the crash on a Cymbal channel");
    require(report.otherChannels == 1, "the piano note on channel 1 is left out");
    require(rack_groove::describe(p, report, "Test groove", clip).contains("2 channels added"), "the hint says which channels were added");

    // Imported again, it reuses the channels it made.
    const auto again = importInto(p, clip, "Again");
    require(p.channelCount == 6 && again.added.empty() && again.channelOf[static_cast<std::size_t>(Role::cymbal)] == cymbal, "a second import adds nothing");

    // Sample channels match by their Starter Pack folder, then their name.
    auto samples = rack::Project::createDefault();
    samples.setSample(0, pack.getChildFile("Drums/Kicks/Kick Snares On.wav").getFullPathName().toStdString());
    samples.renameChannel(0, "Kick Snares On");
    samples.setSample(1, pack.getChildFile("Drums/Hats/Hat Open.wav").getFullPathName().toStdString());
    samples.setSample(2, "C:/samples/My Ride 01.wav");
    samples.renameChannel(2, "My Ride 01");
    samples.setInstrumentPreset(3, 0);
    require(rack_groove::classify(samples, 0) == static_cast<int>(Role::kick) && rack_groove::classify(samples, 1) == static_cast<int>(Role::openHat)
            && rack_groove::classify(samples, 2) == static_cast<int>(Role::cymbal) && rack_groove::classify(samples, 3) == -1,
            "a Starter Pack kick named 'Snares On' is a kick, a ride file is a cymbal, a synth is nothing");

    // No drum channel notes, a full bank: refused.
    require(!rack_groove::importGroove(p, roundTrip(480, {{0, 0, on(60)}, {0, 480, off(60)}}), "Piano"), "a clip with no drum notes is refused");
    auto full = rack::Project::createDefault();
    while (full.addPattern()) {}
    require(!rack_groove::importGroove(full, clip, "Full"), "a full pattern bank is refused");
    std::puts("PASS: a General MIDI groove becomes Channel Rack steps on matching channels");
}

static void everyStarterPackClipImports() {
    const auto clips = starterPack::midiClips(pack);
    require(clips.size() == 38, "the pack lists 20 progressions and 18 grooves");
    require(starterPack::midiClips(pack.getChildFile("missing")).empty(), "a missing pack lists none");
    for (std::size_t i = 1; i < clips.size(); ++i)
        require(clips[i - 1].category.compareNatural(clips[i].category) < 0
                || (clips[i - 1].category == clips[i].category && clips[i - 1].file.getFileName().compareNatural(clips[i].file.getFileName()) < 0),
                "sorted by category, then name");
    int grooves = 0;
    for (const auto& entry : clips) {
        const auto name = entry.file.getFileNameWithoutExtension();
        const auto clip = midi_import::read(entry.file);
        require(clip.ok() && !clip.notes.empty(), "reads: " + name);
        require(clip.unterminated == 0 && clip.strayOffs == 0, "pairs cleanly: " + name);
        auto pattern = std::make_unique<PianoRollPattern>();
        pattern->loopSteps = PianoRollPattern::maxLoopSteps;
        const auto notes = midi_import::pianoRollNotes(clip);
        const auto placed = insertNotes(*pattern, notes, 0.0);
        require(placed.added == static_cast<int>(notes.size()) && placed.trimmed == 0, "every note fits a long clip: " + name);
        pattern->setLoopSteps(static_cast<int>(clip.lengthSteps()));
        playsWithoutStuckNotes(*pattern, name);
        if (entry.isDrumGroove()) {
            ++grooves;
            require(clip.drums() && clip.lengthSteps() == 64, "a groove is four bars of drums: " + name);
            auto rackProject = rack::Project::createDefault();
            const auto report = importInto(rackProject, clip, name.toStdString());
            require(rackProject.pattern(report.pattern).length == 64 && report.hits > 0 && report.dropped == 0 && report.otherChannels == 0,
                    "imports onto the rack as a four-bar pattern: " + name);
        } else {
            require(!clip.drums() && (clip.lengthSteps() == 64 || clip.lengthSteps() == 128), "a progression is four or eight bars: " + name);
            auto fourBars = std::make_unique<PianoRollPattern>();   // the default clip length
            const auto result = insertNotes(*fourBars, notes, 0.0);
            require(result.changed() && (clip.lengthSteps() > 64 || result.skipped == 0), "a four-bar progression fits a new clip: " + name);
            auto growing = std::make_unique<PianoRollPattern>();
            const auto growth = midi_import::loopGrowth(clip);
            const auto grown = insertNotes(*growing, notes, 0.0, &growth);
            require(grown.added == static_cast<int>(notes.size()) && grown.trimmed == 0 && growing->loopSteps == static_cast<int>(clip.lengthSteps()),
                    "a new Session clip grows to hold the whole progression: " + name);
        }
    }
    require(grooves == 18, "every groove was imported");
    std::puts("PASS: every Starter Pack clip imports into the piano roll, and every groove into the Channel Rack");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    require(pack.isDirectory(), "the source Starter Pack folder exists");
    ticksAtAnyPpq();
    notePairing();
    noStuckNotes();
    tempoIsReportedNotApplied();
    insertAtCursor();
    insertGrowsTheLoop();
    timeSignatureSetsTheBar();
    editorImportIsOneUndoableEdit();
    editorTakesDroppedClips();
    drumGrooveToRack();
    everyStarterPackClipImports();
}
