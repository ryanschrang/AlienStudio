#pragma once
// A Channel Rack channel shown as notes in the piano roll (FL Studio's "channel piano roll").
// Each lit step is a note one step long: the step's pitch is the note's distance from
// channelRootPitch, its velocity and chance are the note's. Editing the notes rewrites the steps:
// a note sits on the step it starts nearest to, keeps the step's pan, and is held to the
// steps' pitch range. A step holds one hit, so of several notes on one step the last one placed
// wins, and note lengths are not kept (steps have no length yet).
#include "PianoRollSequence.h"
#include "RackModel.h"
#include <algorithm>
#include <cmath>

namespace alien::rack {

inline constexpr int channelRootPitch = 60;   // middle C: a step pitched 0
inline constexpr int minStepPitch = -24, maxStepPitch = 24;

inline PianoRollPattern channelNotes(const Pattern& pattern, int channel) {
    PianoRollPattern notes;
    notes.loopSteps = pattern.length;
    if (channel < 0 || channel >= maxChannels) return notes;
    const auto& row = pattern.steps[static_cast<std::size_t>(channel)];
    for (int s = 0; s < pattern.length; ++s) {
        const auto& step = row[static_cast<std::size_t>(s)];
        if (step.on)
            notes.add(channelRootPitch + step.pitch, s, 1.0, static_cast<float>(step.velocity) / 127.0f,
                      static_cast<float>(step.probability) / 100.0f);
    }
    return notes;
}

// What happened to the notes when they became steps.
struct ChannelNotesFit {
    bool moved{};      // a note was not on a step, or not one step long
    bool merged{};     // two or more notes fell on one step; the last one placed is kept
    bool clamped{};    // a note was outside the steps' pitch range, or past the pattern's end
};

// The channel's steps as the notes say. Steps past the pattern's length are left as they are.
inline Project::StepRow stepsFromNotes(const PianoRollPattern& notes, const Pattern& pattern, int channel,
                                       ChannelNotesFit* fit = nullptr) {
    ChannelNotesFit result;
    auto row = pattern.steps[static_cast<std::size_t>(std::clamp(channel, 0, maxChannels - 1))];
    const auto original = row;
    for (int s = 0; s < pattern.length; ++s) row[static_cast<std::size_t>(s)].on = false;
    std::array<std::uint32_t, maxSteps> placedBy{};
    for (std::size_t i = 0; i < notes.count; ++i) {
        const auto& note = notes.notes[i];
        const int s = static_cast<int>(std::lround(note.startStep));
        if (s < 0 || s >= pattern.length) { result.clamped = true; continue; }
        if (std::abs(note.startStep - s) > 1.0e-9 || std::abs(note.lengthSteps - 1.0) > 1.0e-9) result.moved = true;
        auto& step = row[static_cast<std::size_t>(s)];
        // Ids grow as notes are added, so the newest note on a step is the one with the largest id.
        if (step.on) { result.merged = true; if (note.id < placedBy[static_cast<std::size_t>(s)]) continue; }
        const int pitch = note.pitch - channelRootPitch;
        if (pitch < minStepPitch || pitch > maxStepPitch) result.clamped = true;
        step.on = true;
        step.pitch = static_cast<std::int8_t>(std::clamp(pitch, minStepPitch, maxStepPitch));
        step.velocity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(note.velocity * 127.0f)), 1, 127));
        step.probability = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(note.probability * 100.0f)), 0, 100));
        step.pan = original[static_cast<std::size_t>(s)].pan;
        placedBy[static_cast<std::size_t>(s)] = note.id;
    }
    if (fit != nullptr) *fit = result;
    return row;
}

} // namespace alien::rack
