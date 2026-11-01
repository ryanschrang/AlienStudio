#pragma once
// Standard MIDI file import (BROWSE-01). Reads a .mid with juce::MidiFile and turns it into notes on the
// piano roll's grid: one step is a sixteenth of 240 ticks, so a file's ticks are rescaled from its own
// pulses per quarter note to 960. Message thread only: it reads files and allocates.
//
// Timing is musical, not clock time: tempo and time-signature meta events are read only to report the
// file's tempo, never to stretch notes, so a groove recorded at 80 BPM plays in time at the project's tempo.
// Files timed in SMPTE frames have no beats to map onto and are refused.
//
// Note pairing never leaves a note open. A note-on with velocity 0 is a note-off. Each channel and key holds
// at most one note: a second note-on for a key that is already down ends the first one there (a retrigger),
// a note-off with nothing held is ignored, and a note still held at the end of its track ends at the last
// event of the file. A note whose on and off fall on the same tick (common for drum hits) is given a
// thirty-second note, cut short by the next note on its key so notes never overlap on one key. juce::MidiFile
// moves note-offs ahead of note-ons that share their tick, so such a hit arrives as a note-off with nothing
// held followed by its note-on: that note-off is kept until the tick moves on, and a note-on for its key on
// the same tick is the zero-length hit.
#include "PianoRollNoteInsert.h"
#include "PianoRollSequence.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace alien::midi_import {

inline constexpr int ticksPerQuarter = PianoRollPattern::ticksPerStep * 4;   // the piano roll's 960 PPQ
inline constexpr int zeroLengthTicks = PianoRollPattern::ticksPerStep / 2;   // a thirty-second note
inline constexpr int gmDrumChannel = 10;                                     // 1-based, as juce::MidiMessage counts

struct ImportedNote {
    int channel{1};          // 1..16
    int pitch{60};
    int startTicks{}, lengthTicks{1};   // project ticks (240 per sixteenth)
    float velocity{0.8f};    // 1/127..1
};

struct ImportedClip {
    std::vector<ImportedNote> notes;   // sorted by start, then pitch
    int lengthTicks{};                 // the clip rounded up to whole bars of 4/4
    int filePpq{};
    double fileTempo{};                // the first tempo in the file, in BPM, or 0 when it has none; never applied
    int retriggered{}, unterminated{}, zeroLength{}, strayOffs{};   // what pairing had to repair
    juce::String error;                // empty when the file was read
    bool ok() const noexcept { return error.isEmpty(); }
    double lengthSteps() const noexcept { return PianoRollPattern::fromTicks(lengthTicks); }
    bool drums() const noexcept {
        return !notes.empty() && std::all_of(notes.begin(), notes.end(), [](const ImportedNote& n) { return n.channel == gmDrumChannel; });
    }
};

// A file tick at `ppq` pulses per quarter note, in project ticks. Rounded to the nearest project tick.
inline int toProjectTicks(double fileTicks, int ppq) noexcept {
    if (ppq <= 0 || !std::isfinite(fileTicks)) return 0;
    const double ticks = std::round(fileTicks * ticksPerQuarter / ppq);
    return static_cast<int>(std::clamp(ticks, 0.0, 1.0e9));
}

inline ImportedClip fromMidiFile(const juce::MidiFile& file) {
    ImportedClip clip;
    const int ppq = file.getTimeFormat();
    if (ppq <= 0) { clip.error = "This MIDI file is timed in SMPTE frames, not beats, so it cannot be placed on the grid."; return clip; }
    clip.filePpq = ppq;
    struct Open { int start; float velocity; };
    int lastTick = 0;
    for (int t = 0; t < file.getNumTracks(); ++t) {
        const auto& track = *file.getTrack(t);
        std::map<int, Open> held;   // channel * 128 + key
        std::map<int, int> loneOffs;   // key -> the tick of a note-off that had nothing to end
        for (const auto* event : track) {
            const auto& m = event->message;
            const int tick = toProjectTicks(m.getTimeStamp(), ppq);
            lastTick = std::max(lastTick, tick);
            if (m.isTempoMetaEvent() && clip.fileTempo == 0.0) {
                const double seconds = m.getTempoSecondsPerQuarterNote();
                if (seconds > 0.0) clip.fileTempo = 60.0 / seconds;
            }
            if (!m.isNoteOnOrOff()) continue;
            const int key = m.getChannel() * 128 + m.getNoteNumber();
            const auto end = [&](const Open& open) {
                ImportedNote note{m.getChannel(), m.getNoteNumber(), open.start, tick - open.start, open.velocity};
                if (note.lengthTicks <= 0) { note.lengthTicks = 0; ++clip.zeroLength; }   // given a length below
                clip.notes.push_back(note);
            };
            const float velocity = std::clamp(m.getFloatVelocity(), 1.0f / 127.0f, 1.0f);
            if (m.isNoteOn()) {
                if (const auto lone = loneOffs.find(key); lone != loneOffs.end()) {
                    const bool sameTick = lone->second == tick;
                    loneOffs.erase(lone);
                    if (sameTick) { end({tick, velocity}); continue; }   // the off that JUCE moved ahead of its on
                    ++clip.strayOffs;
                }
                if (const auto found = held.find(key); found != held.end()) { end(found->second); ++clip.retriggered; held.erase(found); }
                held[key] = {tick, velocity};
            } else if (const auto found = held.find(key); found != held.end()) {
                end(found->second);
                held.erase(found);
            } else if (const auto [lone, added] = loneOffs.try_emplace(key, tick); !added) {
                ++clip.strayOffs;
                lone->second = tick;
            }
        }
        clip.strayOffs += static_cast<int>(loneOffs.size());
        for (const auto& [key, open] : held) {
            clip.notes.push_back({key / 128, key % 128, open.start, -1, open.velocity});   // closed at the file's end below
            ++clip.unterminated;
        }
    }
    for (auto& note : clip.notes) if (note.lengthTicks < 0) note.lengthTicks = std::max(0, lastTick - note.startTicks);
    // Two notes starting together on one key are one note: the longer one, which sorts first.
    std::sort(clip.notes.begin(), clip.notes.end(), [](const ImportedNote& a, const ImportedNote& b) {
        if (a.startTicks != b.startTicks) return a.startTicks < b.startTicks;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.lengthTicks > b.lengthTicks;
    });
    clip.notes.erase(std::unique(clip.notes.begin(), clip.notes.end(), [](const ImportedNote& a, const ImportedNote& b) {
        return a.startTicks == b.startTicks && a.pitch == b.pitch && a.channel == b.channel;
    }), clip.notes.end());
    // One note per key at a time, across tracks too: each note ends no later than the next one on its key
    // starts, and a zero-length note gets a thirty-second.
    std::map<int, int> nextStart;   // key -> the start of the next note on it
    for (std::size_t i = clip.notes.size(); i-- > 0;) {
        auto& note = clip.notes[i];
        const int key = note.channel * 128 + note.pitch;
        if (note.lengthTicks == 0) note.lengthTicks = zeroLengthTicks;
        if (const auto found = nextStart.find(key); found != nextStart.end())
            note.lengthTicks = std::min(note.lengthTicks, found->second - note.startTicks);
        nextStart[key] = note.startTicks;
    }
    // Whole bars that hold every note: one that starts in a bar is in it, and a note held less than a sixteenth
    // past a bar line (a loose release) does not make another bar.
    constexpr int bar = PianoRollPattern::ticksPerStep * 16;
    int bars = 1;
    for (const auto& note : clip.notes)
        bars = std::max({bars, note.startTicks / bar + 1,
                         (note.startTicks + note.lengthTicks - PianoRollPattern::ticksPerStep + bar - 1) / bar});
    clip.lengthTicks = bars * bar;
    if (clip.notes.empty()) clip.error = "This MIDI file has no notes.";
    return clip;
}

inline ImportedClip read(juce::InputStream& in) {
    juce::MidiFile file;
    // Our own pairing below closes notes; JUCE inventing note-offs would hide what the file says.
    if (!file.readFrom(in, false)) { ImportedClip clip; clip.error = "This is not a Standard MIDI file Alien Studio can read."; return clip; }
    return fromMidiFile(file);
}

inline ImportedClip read(const juce::File& path) {
    juce::FileInputStream in(path);
    if (!in.openedOk()) { ImportedClip clip; clip.error = "Could not open " + path.getFileName() + "."; return clip; }
    return read(in);
}

// The clip as piano-roll notes, timed from its start (ids are left for the pattern to give).
inline std::vector<PianoRollNote> pianoRollNotes(const ImportedClip& clip) {
    std::vector<PianoRollNote> notes;
    notes.reserve(clip.notes.size());
    for (const auto& n : clip.notes)
        notes.push_back({0, n.pitch, PianoRollPattern::fromTicks(n.startTicks), PianoRollPattern::fromTicks(n.lengthTicks), n.velocity, 1.0f});
    return notes;
}

// One line for the editor's status bar: what was added, what was left out, and the tempo note.
inline juce::String describe(const ImportedClip& clip, const NoteInsertResult& result, const juce::String& name) {
    if (!clip.ok()) return "No changes: " + clip.error;
    if (!result.changed()) return "No changes: none of " + name + "'s notes fit after the cursor. Move the cursor earlier.";
    juce::String text = "Imported " + juce::String(result.added) + " notes from " + name;
    if (result.skipped != 0) text << "; " << result.skipped << " past the clip's end were left out";
    if (result.trimmed != 0) text << "; " << result.trimmed << " were cut at the clip's end";
    if (clip.fileTempo > 0.0) text << ". The file's " << juce::roundToInt(clip.fileTempo) << " BPM is not applied; notes follow the project tempo";
    return text + ".";
}

} // namespace alien::midi_import
