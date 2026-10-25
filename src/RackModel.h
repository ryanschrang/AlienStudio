#pragma once
// Channel Rack data model: a bank of step patterns shared by a set of sample
// channels, in the style of FL Studio's channel rack. No JUCE and no audio
// code, so every rule here is unit-testable in milliseconds.
//
// Threading: Project is a message-thread value type. Pattern contents are
// immutable once shared (copy-on-write), so copying a whole Project, which
// the undo history does after every edit, costs a few kilobytes. Snapshot is
// what the audio thread reads; it is built on the message thread and never
// changes afterwards.
#include "PianoRollSequence.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace alien::rack {

inline constexpr int maxChannels = 64;
inline constexpr int maxPatterns = 64;
inline constexpr int maxSteps = 128;
inline constexpr int defaultVelocity = 100;
// Per-step timing: gate is the percent of a step (or of one ratchet hit's share of it) a hit sounds for
// before it is released, 100 meaning the whole sample; nudge delays the step's hits by a percent of a
// step (FL's "shift"); ratchet plays the step 1-4 times, evenly spaced inside it.
inline constexpr int fullGate = 100, maxNudge = 100, maxRatchet = 4;
inline constexpr int masterOutput = -1;     // Channel::output: straight to the master bus
inline constexpr int maxOutputTrack = 15;   // Channel::output: a Session track index
inline constexpr int maxChokeGroup = 8;
inline constexpr int maxNameBytes = 48;
inline constexpr int maxSampleBytes = 1024;
// The width of the Channel Rack's name column, in pixels (a view setting saved with the project).
inline constexpr int defaultNameColumn = 118, minNameColumn = 64, maxNameColumn = 320;
inline constexpr float defaultChannelVolume = 0.8f; // unity gain; the fader goes up to 1.0 (+1.9 dB)

inline bool validLength(int length) noexcept { return length == 16 || length == 32 || length == 64 || length == 128; }

// One cell of the grid. A step that is off keeps its values, so turning it back
// on restores what you had drawn in the graph editor.
struct Step {
    bool on{false};
    std::uint8_t velocity{defaultVelocity};  // 1..127
    std::int8_t pan{0};                      // -63..63 (hard left .. hard right)
    std::int8_t pitch{0};                    // -24..24 semitones
    std::uint8_t probability{100};           // 0..100 percent chance that the step plays
    std::uint8_t gate{fullGate};             // 1..100 (see fullGate)
    std::uint8_t nudge{0};                   // 0..maxNudge percent of a step later
    std::uint8_t ratchet{1};                 // 1..maxRatchet hits
    bool operator==(const Step&) const = default;
    bool valid() const noexcept {
        return velocity >= 1 && velocity <= 127 && pan >= -63 && pan <= 63
            && pitch >= -24 && pitch <= 24 && probability <= 100
            && gate >= 1 && gate <= fullGate && nudge <= maxNudge && ratchet >= 1 && ratchet <= maxRatchet;
    }
};

// The properties the graph editor can draw, in the order of its tabs.
enum class StepField { velocity, pan, pitch, probability, gate, nudge, ratchet };
inline constexpr int stepFieldCount = 7;
inline int fieldMinimum(StepField f) noexcept {
    switch (f) { case StepField::velocity: return 1; case StepField::pan: return -63;
                 case StepField::pitch: return -24; case StepField::probability: return 0;
                 case StepField::gate: return 1; case StepField::nudge: return 0; case StepField::ratchet: return 1; }
    return 0;
}
inline int fieldMaximum(StepField f) noexcept {
    switch (f) { case StepField::velocity: return 127; case StepField::pan: return 63;
                 case StepField::pitch: return 24; case StepField::probability: return 100;
                 case StepField::gate: return fullGate; case StepField::nudge: return maxNudge; case StepField::ratchet: return maxRatchet; }
    return 0;
}
inline int fieldDefault(StepField f) noexcept {
    switch (f) { case StepField::velocity: return defaultVelocity; case StepField::pan: return 0;
                 case StepField::pitch: return 0; case StepField::probability: return 100;
                 case StepField::gate: return fullGate; case StepField::nudge: return 0; case StepField::ratchet: return 1; }
    return 0;
}
inline int getField(const Step& s, StepField f) noexcept {
    switch (f) { case StepField::velocity: return s.velocity; case StepField::pan: return s.pan;
                 case StepField::pitch: return s.pitch; case StepField::probability: return s.probability;
                 case StepField::gate: return s.gate; case StepField::nudge: return s.nudge; case StepField::ratchet: return s.ratchet; }
    return 0;
}
inline void setField(Step& s, StepField f, int value) noexcept {
    value = std::clamp(value, fieldMinimum(f), fieldMaximum(f));
    switch (f) {
    case StepField::velocity: s.velocity = static_cast<std::uint8_t>(value); break;
    case StepField::pan: s.pan = static_cast<std::int8_t>(value); break;
    case StepField::pitch: s.pitch = static_cast<std::int8_t>(value); break;
    case StepField::probability: s.probability = static_cast<std::uint8_t>(value); break;
    case StepField::gate: s.gate = static_cast<std::uint8_t>(value); break;
    case StepField::nudge: s.nudge = static_cast<std::uint8_t>(value); break;
    case StepField::ratchet: s.ratchet = static_cast<std::uint8_t>(value); break;
    }
}

// Message-thread identity for cancellation of queued notes after edits. Never
// generated by the audio callback, never persisted, and unaffected by allocator reuse.
inline std::uint64_t nextNoteRevision() noexcept {
    static std::atomic<std::uint64_t> revision{0};
    return revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

// All channels' steps for one pattern. Steps past `length` are kept (switching
// 64 -> 16 -> 64 loses nothing) but are not played. About 64 KiB, so patterns are
// only ever made with make_shared (see Project::edit), never on a stack.
struct Pattern {
    std::array<std::array<Step, maxSteps>, maxChannels> steps{};
    // Null means drum steps. A non-null immutable sequence is authoritative for
    // this row; the old steps remain dormant until the row is explicitly cleared.
    std::array<std::shared_ptr<const PianoRollPattern>, maxChannels> notes{};
    std::array<std::uint64_t, maxChannels> noteRevisions{};
    int length{16};
    static bool sameNotes(const PianoRollPattern& a, const PianoRollPattern& b) noexcept {
        if (a.count != b.count || a.loopSteps != b.loopSteps || a.count > PianoRollPattern::maxNotes) return false;
        for (std::size_t i = 0; i < a.count; ++i) {
            const auto& x = a.notes[i]; const auto& y = b.notes[i];
            if (x.id != y.id || x.pitch != y.pitch || x.startStep != y.startStep
                || x.lengthSteps != y.lengthSteps || x.velocity != y.velocity || x.probability != y.probability) return false;
        }
        return true;
    }
    bool operator==(const Pattern& other) const {
        if (length != other.length || steps != other.steps) return false;
        for (std::size_t c = 0; c < notes.size(); ++c)
            if (notes[c] != other.notes[c] && (!notes[c] || !other.notes[c] || !sameNotes(*notes[c], *other.notes[c]))) return false;
        return true;
    }
    bool valid() const noexcept {
        if (!validLength(length)) return false;
        for (const auto& sequence : notes)
            if (sequence && (sequence->loopSteps != length || !sequence->valid())) return false;
        for (const auto& row : steps) for (const auto& s : row) if (!s.valid()) return false;
        return true;
    }
    bool empty() const noexcept {
        for (std::size_t c = 0; c < steps.size(); ++c) {
            if (notes[c]) { if (notes[c]->count != 0) return false; }
            else for (const auto& s : steps[c]) if (s.on) return false;
        }
        return true;
    }
};

struct Channel {
    float volume{defaultChannelVolume};  // 0..1
    float pan{0.0f};                     // -1..1
    bool mute{false}, solo{false};
    std::int8_t output{masterOutput};    // masterOutput or a Session track index
    std::uint8_t chokeGroup{0};          // 0: none. Hits in a group cut off every other sound of the group.
    bool operator==(const Channel&) const = default;
    bool valid() const noexcept {
        return std::isfinite(volume) && volume >= 0.0f && volume <= 1.0f
            && std::isfinite(pan) && pan >= -1.0f && pan <= 1.0f
            && output >= masterOutput && output <= maxOutputTrack && chokeGroup <= maxChokeGroup;
    }
};

// Where a channel's sound comes from. Message thread only.
struct ChannelInfo {
    std::string name;
    // "" (silent), "builtin:<id>" (a synthesized drum sound), or the path of an audio file.
    std::string sample;
    bool operator==(const ChannelInfo&) const = default;
    bool isBuiltin() const { return sample.rfind("builtin:", 0) == 0; }
    bool isFile() const { return !sample.empty() && !isBuiltin(); }
    std::string builtinId() const { return isBuiltin() ? sample.substr(8) : std::string{}; }
};

// The immutable view of a project that the audio thread plays from.
struct Snapshot {
    int channelCount{};
    std::array<Channel, maxChannels> channels{};
    int selectedPattern{};
    float swing{};
    std::array<std::shared_ptr<const Pattern>, maxPatterns> patterns{};
    bool anySolo{};
    const Pattern* pattern(int index) const noexcept {
        return index >= 0 && index < maxPatterns ? patterns[static_cast<std::size_t>(index)].get() : nullptr;
    }
    // The pattern that plays now, or nullptr when there is none.
    const Pattern* selected() const noexcept { return pattern(selectedPattern); }
    // Mute and solo applied: does this channel sound when the pattern plays?
    bool audible(int channel) const noexcept {
        if (channel < 0 || channel >= channelCount) return false;
        const auto& c = channels[static_cast<std::size_t>(channel)];
        return !c.mute && (!anySolo || c.solo);
    }
};

struct Project {
    int channelCount{};
    std::array<Channel, maxChannels> channels{};
    std::array<ChannelInfo, maxChannels> info{};
    int patternCount{1};
    std::array<std::shared_ptr<const Pattern>, maxPatterns> patterns{};
    std::array<std::string, maxPatterns> patternNames{};
    int selectedPattern{};
    float swing{};  // 0..1: how late every second sixteenth lands (1 = three quarters of the way to the next)
    int nameColumnWidth{defaultNameColumn};   // a view setting like selectedPattern: not an undoable edit

    static const std::shared_ptr<const Pattern>& emptyPattern() {
        static const auto empty = std::make_shared<const Pattern>();
        return empty;
    }

    // FL's starting rack: four channels and one empty pattern.
    static Project createDefault() {
        Project p;
        p.patterns[0] = emptyPattern();
        p.patternNames[0] = "Pattern 1";
        p.addChannel("Kick", "builtin:kick");
        p.addChannel("Clap", "builtin:clap");
        p.addChannel("Hat", "builtin:hat");
        p.addChannel("Snare", "builtin:snare");
        return p;
    }

    // ---- queries -------------------------------------------------------------
    const Pattern& pattern(int index) const { return *patterns[static_cast<std::size_t>(index)]; }
    bool validPattern(int index) const noexcept { return index >= 0 && index < patternCount; }
    bool validChannel(int index) const noexcept { return index >= 0 && index < channelCount; }
    bool validStep(int pattern, int channel, int step) const noexcept {
        return validPattern(pattern) && validChannel(channel) && step >= 0 && step < maxSteps;
    }
    bool anySolo() const noexcept {
        for (int c = 0; c < channelCount; ++c) if (channels[static_cast<std::size_t>(c)].solo) return true;
        return false;
    }
    // Would the channel sound if the pattern played now (mute and solo applied)?
    bool audible(int channel) const noexcept {
        if (!validChannel(channel)) return false;
        const auto& c = channels[static_cast<std::size_t>(channel)];
        return !c.mute && (!anySolo() || c.solo);
    }

    // ---- pattern edits (each returns true when something changed) --------------
    bool selectPattern(int index) {
        if (!validPattern(index) || index == selectedPattern) return false;
        selectedPattern = index; return true;
    }
    bool setNameColumnWidth(int width) {
        width = std::clamp(width, minNameColumn, maxNameColumn);
        if (width == nameColumnWidth) return false;
        nameColumnWidth = width; return true;
    }
    // Adds an empty pattern, or a copy of `copyFrom`. The new pattern becomes the selected one.
    bool addPattern(int copyFrom = -1) {
        if (patternCount >= maxPatterns) return false;
        const int slot = patternCount++;
        patterns[static_cast<std::size_t>(slot)] = validPattern(copyFrom) && copyFrom != slot
            ? patterns[static_cast<std::size_t>(copyFrom)] : emptyPattern();
        patternNames[static_cast<std::size_t>(slot)] = freshPatternName();
        selectedPattern = slot;
        return true;
    }
    bool deletePattern(int index) {
        if (!validPattern(index) || patternCount <= 1) return false;
        for (int i = index; i + 1 < patternCount; ++i) {
            patterns[static_cast<std::size_t>(i)] = patterns[static_cast<std::size_t>(i + 1)];
            patternNames[static_cast<std::size_t>(i)] = patternNames[static_cast<std::size_t>(i + 1)];
        }
        --patternCount;
        patterns[static_cast<std::size_t>(patternCount)].reset();
        patternNames[static_cast<std::size_t>(patternCount)].clear();
        if (selectedPattern > index || selectedPattern >= patternCount) selectedPattern = std::max(0, selectedPattern - 1);
        return true;
    }
    bool renamePattern(int index, std::string name) {
        name = clean(std::move(name));
        if (!validPattern(index) || name.empty() || patternNames[static_cast<std::size_t>(index)] == name) return false;
        patternNames[static_cast<std::size_t>(index)] = std::move(name); return true;
    }
    bool setPatternLength(int index, int length) {
        if (!validPattern(index) || !validLength(length) || pattern(index).length == length) return false;
        // Refuse a destructive shortening rather than silently truncating notes.
        for (const auto& sequence : pattern(index).notes) if (sequence)
            for (std::size_t n = 0; n < sequence->count; ++n)
                if (PianoRollPattern::toTicks(sequence->notes[n].startStep) + PianoRollPattern::toTicks(sequence->notes[n].lengthSteps)
                    > length * PianoRollPattern::ticksPerStep) return false;
        edit(index, [&](Pattern& p) {
            p.length = length;
            for (auto& sequence : p.notes) if (sequence) {
                auto copy = std::make_shared<PianoRollPattern>(*sequence);
                copy->loopSteps = length; sequence = std::move(copy);
            }
            for (std::size_t c = 0; c < p.notes.size(); ++c) if (p.notes[c]) p.noteRevisions[c] = nextNoteRevision();
        });
        return true;
    }
    bool clearPattern(int index) {
        if (!validPattern(index) || (pattern(index).empty()
            && std::none_of(pattern(index).notes.begin(), pattern(index).notes.end(), [](const auto& row) { return row != nullptr; }))) return false;
        edit(index, [](Pattern& p) { p.notes = {}; p.noteRevisions = {}; for (auto& row : p.steps) for (auto& s : row) s = Step{}; });
        return true;
    }
    bool clearChannelSteps(int index, int channel) {
        if (!validPattern(index) || !validChannel(channel)) return false;
        const auto& row = pattern(index).steps[static_cast<std::size_t>(channel)];
        if (!pattern(index).notes[static_cast<std::size_t>(channel)]
            && std::none_of(row.begin(), row.end(), [](const Step& s) { return s.on; })) return false;
        edit(index, [&](Pattern& p) { p.notes[static_cast<std::size_t>(channel)].reset(); p.noteRevisions[static_cast<std::size_t>(channel)] = 0; for (auto& s : p.steps[static_cast<std::size_t>(channel)]) s = Step{}; });
        return true;
    }


    bool setChannelNotes(int pat, int channel, const PianoRollPattern& sequence) {
        if (!validPattern(pat) || !validChannel(channel) || !sequence.valid()
            || sequence.loopSteps != pattern(pat).length) return false;
        const auto& previous = pattern(pat).notes[static_cast<std::size_t>(channel)];
        if (previous && Pattern::sameNotes(*previous, sequence)) return false;
        auto stored = std::make_shared<const PianoRollPattern>(sequence);
        edit(pat, [&](Pattern& p) { p.notes[static_cast<std::size_t>(channel)] = std::move(stored); p.noteRevisions[static_cast<std::size_t>(channel)] = nextNoteRevision(); });
        return true;
    }
    bool isNoteChannel(int pat, int channel) const noexcept {
        return validPattern(pat) && validChannel(channel) && pattern(pat).notes[static_cast<std::size_t>(channel)] != nullptr;
    }

    // Row tools affect only the audible pattern length. Hidden steps survive.
    using StepRow = std::array<Step, maxSteps>;
    bool pasteRow(int pat, int channel, const StepRow& source) {
        if (!validPattern(pat) || !validChannel(channel) || isNoteChannel(pat, channel)
            || std::any_of(source.begin(), source.end(), [](const Step& s) { return !s.valid(); })) return false;
        auto row = pattern(pat).steps[static_cast<std::size_t>(channel)];
        std::copy_n(source.begin(), pattern(pat).length, row.begin());
        if (row == pattern(pat).steps[static_cast<std::size_t>(channel)]) return false;
        edit(pat, [&](Pattern& p) { p.steps[static_cast<std::size_t>(channel)] = row; }); return true;
    }
    bool fillEvery(int pat, int channel, int interval) {
        if (!validPattern(pat) || !validChannel(channel) || (interval != 2 && interval != 4 && interval != 8)) return false;
        auto row = pattern(pat).steps[static_cast<std::size_t>(channel)];
        for (int s = 0; s < pattern(pat).length; ++s) row[static_cast<std::size_t>(s)].on = s % interval == 0;
        return pasteRow(pat, channel, row);
    }
    bool rotateRow(int pat, int channel, int distance) {
        if (!validPattern(pat) || !validChannel(channel)) return false;
        const int len = pattern(pat).length, shift = ((distance % len) + len) % len;
        if (shift == 0) return false;
        auto row = pattern(pat).steps[static_cast<std::size_t>(channel)];
        std::rotate(row.begin(), row.begin() + len - shift, row.begin() + len);
        return pasteRow(pat, channel, row);
    }
    bool reverseRow(int pat, int channel) {
        if (!validPattern(pat) || !validChannel(channel)) return false;
        auto row = pattern(pat).steps[static_cast<std::size_t>(channel)];
        std::reverse(row.begin(), row.begin() + pattern(pat).length);
        return pasteRow(pat, channel, row);
    }
    bool repeatRow(int pat, int channel, int period) {
        if (!validPattern(pat) || !validChannel(channel) || period < 1 || period >= pattern(pat).length) return false;
        auto row = pattern(pat).steps[static_cast<std::size_t>(channel)];
        for (int s = period; s < pattern(pat).length; ++s) row[static_cast<std::size_t>(s)] = row[static_cast<std::size_t>(s % period)];
        return pasteRow(pat, channel, row);
    }
    // Insert a full channel clone directly below its source, in every pattern.
    bool duplicateChannel(int index) {
        if (!validChannel(index) || channelCount >= maxChannels) return false;
        for (int i = channelCount; i > index + 1; --i) {
            channels[static_cast<std::size_t>(i)] = channels[static_cast<std::size_t>(i - 1)];
            info[static_cast<std::size_t>(i)] = info[static_cast<std::size_t>(i - 1)];
        }
        channels[static_cast<std::size_t>(index + 1)] = channels[static_cast<std::size_t>(index)];
        info[static_cast<std::size_t>(index + 1)] = info[static_cast<std::size_t>(index)];
        info[static_cast<std::size_t>(index + 1)].name = clean(info[static_cast<std::size_t>(index)].name + " copy");
        ++channelCount;
        forEachPattern([&](Pattern& p) {
            for (int i = channelCount - 1; i > index + 1; --i) {
                p.steps[static_cast<std::size_t>(i)] = p.steps[static_cast<std::size_t>(i - 1)];
                p.notes[static_cast<std::size_t>(i)] = p.notes[static_cast<std::size_t>(i - 1)];
                p.noteRevisions[static_cast<std::size_t>(i)] = p.noteRevisions[static_cast<std::size_t>(i - 1)];
            }
            p.steps[static_cast<std::size_t>(index + 1)] = p.steps[static_cast<std::size_t>(index)];
            p.notes[static_cast<std::size_t>(index + 1)] = p.notes[static_cast<std::size_t>(index)];
            p.noteRevisions[static_cast<std::size_t>(index + 1)] = p.noteRevisions[static_cast<std::size_t>(index)];
        });
        return true;
    }

    // ---- step edits -----------------------------------------------------------
    bool setStep(int pat, int channel, int step, const Step& value) {
        if (!validStep(pat, channel, step) || isNoteChannel(pat, channel) || !value.valid()) return false;
        if (pattern(pat).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)] == value) return false;
        edit(pat, [&](Pattern& p) { p.steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)] = value; });
        return true;
    }
    // Turning a step on or off leaves its velocity, pan, pitch, probability, gate, nudge and ratchet alone.
    bool setStepOn(int pat, int channel, int step, bool on) {
        if (!validStep(pat, channel, step)) return false;
        auto s = pattern(pat).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)];
        if (s.on == on) return false;
        s.on = on;
        return setStep(pat, channel, step, s);
    }
    bool setStepField(int pat, int channel, int step, StepField field, int value) {
        if (!validStep(pat, channel, step)) return false;
        auto s = pattern(pat).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)];
        setField(s, field, value);
        return setStep(pat, channel, step, s);
    }

    // ---- channel edits --------------------------------------------------------
    bool addChannel(std::string name, std::string sample) {
        if (channelCount >= maxChannels) return false;
        const auto slot = static_cast<std::size_t>(channelCount++);
        channels[slot] = Channel{};
        info[slot] = {clean(std::move(name)), std::move(sample)};
        if (info[slot].name.empty()) info[slot].name = "Channel " + std::to_string(channelCount);
        return true;
    }
    // Removes the channel and its steps in every pattern; the channels after it move up.
    bool removeChannel(int index) {
        if (!validChannel(index)) return false;
        for (int i = index; i + 1 < channelCount; ++i) {
            channels[static_cast<std::size_t>(i)] = channels[static_cast<std::size_t>(i + 1)];
            info[static_cast<std::size_t>(i)] = std::move(info[static_cast<std::size_t>(i + 1)]);
        }
        --channelCount;
        channels[static_cast<std::size_t>(channelCount)] = Channel{};
        info[static_cast<std::size_t>(channelCount)] = {};
        forEachPattern([&](Pattern& p) {
            for (int i = index; i + 1 < maxChannels; ++i) {
                p.steps[static_cast<std::size_t>(i)] = p.steps[static_cast<std::size_t>(i + 1)];
                p.notes[static_cast<std::size_t>(i)] = p.notes[static_cast<std::size_t>(i + 1)];
                p.noteRevisions[static_cast<std::size_t>(i)] = p.noteRevisions[static_cast<std::size_t>(i + 1)];
            }
            p.steps[maxChannels - 1] = {}; p.notes[maxChannels - 1].reset(); p.noteRevisions[maxChannels - 1] = 0;
        });
        return true;
    }
    // Swaps the channel with its neighbour above (delta < 0) or below (delta > 0), steps included.
    bool moveChannel(int index, int delta) {
        const int other = index + (delta < 0 ? -1 : 1);
        if (delta == 0 || !validChannel(index) || !validChannel(other)) return false;
        std::swap(channels[static_cast<std::size_t>(index)], channels[static_cast<std::size_t>(other)]);
        std::swap(info[static_cast<std::size_t>(index)], info[static_cast<std::size_t>(other)]);
        forEachPattern([&](Pattern& p) { std::swap(p.steps[static_cast<std::size_t>(index)], p.steps[static_cast<std::size_t>(other)]); std::swap(p.notes[static_cast<std::size_t>(index)], p.notes[static_cast<std::size_t>(other)]); std::swap(p.noteRevisions[static_cast<std::size_t>(index)], p.noteRevisions[static_cast<std::size_t>(other)]); });
        return true;
    }
    bool setChannel(int index, const Channel& value) {
        if (!validChannel(index) || !value.valid() || channels[static_cast<std::size_t>(index)] == value) return false;
        channels[static_cast<std::size_t>(index)] = value; return true;
    }
    bool renameChannel(int index, std::string name) {
        name = clean(std::move(name));
        if (!validChannel(index) || name.empty() || info[static_cast<std::size_t>(index)].name == name) return false;
        info[static_cast<std::size_t>(index)].name = std::move(name); return true;
    }
    bool setSample(int index, std::string sample) {
        if (!validChannel(index) || sample.size() > maxSampleBytes || info[static_cast<std::size_t>(index)].sample == sample) return false;
        info[static_cast<std::size_t>(index)].sample = std::move(sample); return true;
    }
    bool setSwing(float value) {
        if (!std::isfinite(value)) return false;
        value = std::clamp(value, 0.0f, 1.0f);
        if (value == swing) return false;
        swing = value; return true;
    }

    // ---- Session track bookkeeping ------------------------------------------
    // A channel routed to a track that no longer exists falls back to the master bus.
    void trackDeleted(int track) {
        for (int c = 0; c < channelCount; ++c) {
            auto& out = channels[static_cast<std::size_t>(c)].output;
            if (out == track) out = masterOutput;
            else if (out > track) --out;
        }
    }
    void tracksSwapped(int a, int b) {
        for (int c = 0; c < channelCount; ++c) {
            auto& out = channels[static_cast<std::size_t>(c)].output;
            if (out == a) out = static_cast<std::int8_t>(b);
            else if (out == b) out = static_cast<std::int8_t>(a);
        }
    }

    // ---- whole-project rules --------------------------------------------------
    bool valid() const noexcept {
        if (channelCount < 0 || channelCount > maxChannels || patternCount < 1 || patternCount > maxPatterns
            || selectedPattern < 0 || selectedPattern >= patternCount || !std::isfinite(swing) || swing < 0.0f || swing > 1.0f
            || nameColumnWidth < minNameColumn || nameColumnWidth > maxNameColumn)
            return false;
        for (int c = 0; c < maxChannels; ++c) {
            const auto i = static_cast<std::size_t>(c);
            if (!channels[i].valid() || info[i].name.size() > maxNameBytes || info[i].sample.size() > maxSampleBytes) return false;
            if (c >= channelCount && !(channels[i] == Channel{} && info[i] == ChannelInfo{})) return false;
        }
        for (int p = 0; p < maxPatterns; ++p) {
            const auto i = static_cast<std::size_t>(p);
            if (p < patternCount) {
                if (patterns[i] == nullptr || !patterns[i]->valid() || patternNames[i].size() > maxNameBytes) return false;
                for (int c = channelCount; c < maxChannels; ++c) if (patterns[i]->notes[static_cast<std::size_t>(c)]) return false;
            } else if (patterns[i] != nullptr || !patternNames[i].empty()) return false;
        }
        return true;
    }
    // Identity-cheap comparison: patterns are compared by pointer first, so two
    // projects that share their patterns compare in a few microseconds.
    bool operator==(const Project& o) const {
        return selectedPattern == o.selectedPattern && nameColumnWidth == o.nameColumnWidth && sameContent(o);
    }
    // Everything except which pattern is selected and how wide the name column is, which are view settings like
    // the selected track or scene: choosing a different pattern or resizing the column is not an undoable edit.
    bool sameContent(const Project& o) const {
        if (channelCount != o.channelCount || patternCount != o.patternCount
            || swing != o.swing || channels != o.channels || info != o.info || patternNames != o.patternNames)
            return false;
        for (int p = 0; p < patternCount; ++p) {
            const auto& a = patterns[static_cast<std::size_t>(p)];
            const auto& b = o.patterns[static_cast<std::size_t>(p)];
            if (a != b && !(*a == *b)) return false;
        }
        return true;
    }

    std::shared_ptr<const Snapshot> snapshot() const {
        auto s = std::make_shared<Snapshot>();
        s->channelCount = channelCount; s->channels = channels; s->selectedPattern = selectedPattern; s->swing = swing;
        s->patterns = patterns; s->anySolo = anySolo();
        return s;
    }

    static std::string clean(std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        if (text.size() > maxNameBytes) {
            std::size_t cut = maxNameBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut; // never split a UTF-8 character
            text.resize(cut);
        }
        return text;
    }

private:
    std::string freshPatternName() const {
        for (int n = patternCount;; ++n) {
            const auto candidate = "Pattern " + std::to_string(n);
            bool used = false;
            for (int p = 0; p < patternCount - 1; ++p) used = used || patternNames[static_cast<std::size_t>(p)] == candidate;
            if (!used) return candidate;
        }
    }
    template <class Fn> void edit(int index, Fn&& fn) {
        auto copy = std::make_shared<Pattern>(*patterns[static_cast<std::size_t>(index)]);
        fn(*copy);
        patterns[static_cast<std::size_t>(index)] = std::move(copy);
    }
    template <class Fn> void forEachPattern(Fn&& fn) {
        for (int p = 0; p < patternCount; ++p) edit(p, fn);
    }
};

// Hands complete, immutable snapshots from the message thread to the audio
// thread. Only a pointer ever crosses threads: the audio callback swaps a slot
// index and reads through a raw pointer, and never copies, allocates, or frees
// a snapshot. The three slots hold shared_ptrs that only the publishing side
// assigns, so an old snapshot is released on the message thread, and only
// after the audio thread has moved on.
class SnapshotMailbox {
public:
    SnapshotMailbox() {
        const auto initial = Project::createDefault().snapshot();
        for (auto& slot : slots) slot = initial;
    }
    // Message thread (producers are serialised by a mutex the audio thread never takes).
    bool publish(std::shared_ptr<const Snapshot> value) {
        if (value == nullptr) return false;
        std::lock_guard<std::mutex> guard(producerMutex);
        slots[back] = std::move(value);
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & 3;
        return true;
    }
    // Audio thread. The newest snapshot if one arrived since the last call, else nullptr.
    // The pointer stays valid until the next consume() that returns non-null.
    const Snapshot* consume() noexcept {
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return nullptr;
        front = middle.exchange(front, std::memory_order_acq_rel) & 3;
        return slots[front].get();
    }
    const Snapshot* current() const noexcept { return slots[front].get(); }
private:
    static constexpr unsigned dirty = 4;
    std::array<std::shared_ptr<const Snapshot>, 3> slots;
    std::atomic<unsigned> middle{1};
    std::mutex producerMutex;
    unsigned front{}, back{2};
};

} // namespace alien::rack
