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

inline constexpr int maxChannels = 16;
inline constexpr int maxPatterns = 64;
inline constexpr int maxSteps = 64;
inline constexpr int defaultVelocity = 100;
inline constexpr int masterOutput = -1;     // Channel::output: straight to the master bus
inline constexpr int maxOutputTrack = 15;   // Channel::output: a Session track index
inline constexpr int maxChokeGroup = 8;
inline constexpr int maxNameBytes = 48;
inline constexpr int maxSampleBytes = 1024;
inline constexpr float defaultChannelVolume = 0.8f; // unity gain; the fader goes up to 1.0 (+1.9 dB)

inline bool validLength(int length) noexcept { return length == 16 || length == 32 || length == 64; }

// One cell of the grid. A step that is off keeps its values, so turning it back
// on restores what you had drawn in the graph editor.
struct Step {
    bool on{false};
    std::uint8_t velocity{defaultVelocity};  // 1..127
    std::int8_t pan{0};                      // -63..63 (hard left .. hard right)
    std::int8_t pitch{0};                    // -24..24 semitones
    std::uint8_t probability{100};           // 0..100 percent chance that the step plays
    bool operator==(const Step&) const = default;
    bool valid() const noexcept {
        return velocity >= 1 && velocity <= 127 && pan >= -63 && pan <= 63
            && pitch >= -24 && pitch <= 24 && probability <= 100;
    }
};

// The properties the graph editor can draw, in the order of its tabs.
enum class StepField { velocity, pan, pitch, probability };
inline constexpr int stepFieldCount = 4;
inline int fieldMinimum(StepField f) noexcept {
    switch (f) { case StepField::velocity: return 1; case StepField::pan: return -63;
                 case StepField::pitch: return -24; case StepField::probability: return 0; }
    return 0;
}
inline int fieldMaximum(StepField f) noexcept {
    switch (f) { case StepField::velocity: return 127; case StepField::pan: return 63;
                 case StepField::pitch: return 24; case StepField::probability: return 100; }
    return 0;
}
inline int fieldDefault(StepField f) noexcept {
    switch (f) { case StepField::velocity: return defaultVelocity; case StepField::pan: return 0;
                 case StepField::pitch: return 0; case StepField::probability: return 100; }
    return 0;
}
inline int getField(const Step& s, StepField f) noexcept {
    switch (f) { case StepField::velocity: return s.velocity; case StepField::pan: return s.pan;
                 case StepField::pitch: return s.pitch; case StepField::probability: return s.probability; }
    return 0;
}
inline void setField(Step& s, StepField f, int value) noexcept {
    value = std::clamp(value, fieldMinimum(f), fieldMaximum(f));
    switch (f) {
    case StepField::velocity: s.velocity = static_cast<std::uint8_t>(value); break;
    case StepField::pan: s.pan = static_cast<std::int8_t>(value); break;
    case StepField::pitch: s.pitch = static_cast<std::int8_t>(value); break;
    case StepField::probability: s.probability = static_cast<std::uint8_t>(value); break;
    }
}

// All channels' steps for one pattern. Steps past `length` are kept (switching
// 64 -> 16 -> 64 loses nothing) but are not played.
struct Pattern {
    std::array<std::array<Step, maxSteps>, maxChannels> steps{};
    int length{16};
    bool operator==(const Pattern&) const = default;
    bool valid() const noexcept {
        if (!validLength(length)) return false;
        for (const auto& row : steps) for (const auto& s : row) if (!s.valid()) return false;
        return true;
    }
    bool empty() const noexcept {
        for (const auto& row : steps) for (const auto& s : row) if (s.on) return false;
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
        edit(index, [&](Pattern& p) { p.length = length; });
        return true;
    }
    bool clearPattern(int index) {
        if (!validPattern(index) || pattern(index).empty()) return false;
        edit(index, [](Pattern& p) { for (auto& row : p.steps) for (auto& s : row) s = Step{}; });
        return true;
    }
    bool clearChannelSteps(int index, int channel) {
        if (!validPattern(index) || !validChannel(channel)) return false;
        const auto& row = pattern(index).steps[static_cast<std::size_t>(channel)];
        if (std::none_of(row.begin(), row.end(), [](const Step& s) { return s.on; })) return false;
        edit(index, [&](Pattern& p) { for (auto& s : p.steps[static_cast<std::size_t>(channel)]) s = Step{}; });
        return true;
    }

    // ---- step edits -----------------------------------------------------------
    bool setStep(int pat, int channel, int step, const Step& value) {
        if (!validStep(pat, channel, step) || !value.valid()) return false;
        if (pattern(pat).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)] == value) return false;
        edit(pat, [&](Pattern& p) { p.steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)] = value; });
        return true;
    }
    // Turning a step on or off leaves its velocity, pan, pitch and probability alone.
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
            for (int i = index; i + 1 < maxChannels; ++i) p.steps[static_cast<std::size_t>(i)] = p.steps[static_cast<std::size_t>(i + 1)];
            p.steps[maxChannels - 1] = {};
        });
        return true;
    }
    // Swaps the channel with its neighbour above (delta < 0) or below (delta > 0), steps included.
    bool moveChannel(int index, int delta) {
        const int other = index + (delta < 0 ? -1 : 1);
        if (delta == 0 || !validChannel(index) || !validChannel(other)) return false;
        std::swap(channels[static_cast<std::size_t>(index)], channels[static_cast<std::size_t>(other)]);
        std::swap(info[static_cast<std::size_t>(index)], info[static_cast<std::size_t>(other)]);
        forEachPattern([&](Pattern& p) { std::swap(p.steps[static_cast<std::size_t>(index)], p.steps[static_cast<std::size_t>(other)]); });
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
            || selectedPattern < 0 || selectedPattern >= patternCount || !std::isfinite(swing) || swing < 0.0f || swing > 1.0f)
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
            } else if (patterns[i] != nullptr || !patternNames[i].empty()) return false;
        }
        return true;
    }
    // Identity-cheap comparison: patterns are compared by pointer first, so two
    // projects that share their patterns compare in a few microseconds.
    bool operator==(const Project& o) const { return selectedPattern == o.selectedPattern && sameContent(o); }
    // Everything except which pattern is selected, which is a view setting like the
    // selected track or scene: choosing a different pattern is not an undoable edit.
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
