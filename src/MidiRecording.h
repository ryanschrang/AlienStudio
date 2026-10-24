#pragma once
#include "PianoRollSequence.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace alien {

// Recording options saved with the project (optional "recording" object in the
// project file, so older projects open with these defaults).
struct RecordSettings {
    static constexpr int maxCountInBars = 2, maxLatencyOffsetMs = 100;
    bool metronome{false};         // click while playing; the count-in always clicks
    float metronomeVolume{0.5f};   // 0..1
    int countInBars{1};            // 0 = no count-in
    int latencyOffsetMs{0};        // extra shift on top of the device latency; positive = notes land earlier
    bool replace{false};           // false = overdub (layer onto existing notes), true = replace them
    // Song-view audio takes. A take moves earlier by the device's input and output latency
    // plus this offset (positive = earlier). With punch on, only the loop range is recorded.
    int audioLatencyOffsetMs{0};
    bool punch{false};

    bool valid() const noexcept {
        return std::isfinite(metronomeVolume) && metronomeVolume >= 0.0f && metronomeVolume <= 1.0f
            && countInBars >= 0 && countInBars <= maxCountInBars
            && latencyOffsetMs >= -maxLatencyOffsetMs && latencyOffsetMs <= maxLatencyOffsetMs
            && audioLatencyOffsetMs >= -maxLatencyOffsetMs && audioLatencyOffsetMs <= maxLatencyOffsetMs;
    }
    bool operator==(const RecordSettings&) const = default;
};

// The record-options menu offers a short list of choices instead of free-form
// numbers. Kept here, free of any UI code, so the mapping can be tested.
namespace recordmenu {
inline constexpr std::array<float, 3> volumeLevels{0.25f, 0.5f, 0.8f};
inline constexpr std::array<const char*, 3> volumeNames{"Low", "Medium", "High"};
inline constexpr std::array<int, 9> latencyChoices{-50, -25, -10, 0, 10, 25, 50, 75, 100};
inline constexpr int nudgeMs = 5;
// Menu item ids.
inline constexpr int countInBase = 100, volumeBase = 200, overdubId = 300, replaceId = 301, latencyBase = 400,
                     nudgeEarlierId = 500, nudgeLaterId = 501, audioLatencyBase = 600, audioNudgeEarlierId = 700,
                     audioNudgeLaterId = 701, punchId = 800;

inline int closestVolumeIndex(float volume) noexcept {
    int best = 0;
    for (int i = 1; i < static_cast<int>(volumeLevels.size()); ++i)
        if (std::fabs(volumeLevels[static_cast<std::size_t>(i)] - volume) < std::fabs(volumeLevels[static_cast<std::size_t>(best)] - volume))
            best = i;
    return best;
}

// The settings after the user picks `id`; unknown ids change nothing.
inline RecordSettings apply(RecordSettings settings, int id) noexcept {
    if (id >= countInBase && id <= countInBase + RecordSettings::maxCountInBars) settings.countInBars = id - countInBase;
    else if (id >= volumeBase && id < volumeBase + static_cast<int>(volumeLevels.size()))
        settings.metronomeVolume = volumeLevels[static_cast<std::size_t>(id - volumeBase)];
    else if (id == overdubId) settings.replace = false;
    else if (id == replaceId) settings.replace = true;
    else if (id >= latencyBase && id < latencyBase + static_cast<int>(latencyChoices.size()))
        settings.latencyOffsetMs = latencyChoices[static_cast<std::size_t>(id - latencyBase)];
    else if (id == nudgeEarlierId)
        settings.latencyOffsetMs = std::min(settings.latencyOffsetMs + nudgeMs, RecordSettings::maxLatencyOffsetMs);
    else if (id == nudgeLaterId)
        settings.latencyOffsetMs = std::max(settings.latencyOffsetMs - nudgeMs, -RecordSettings::maxLatencyOffsetMs);
    else if (id >= audioLatencyBase && id < audioLatencyBase + static_cast<int>(latencyChoices.size()))
        settings.audioLatencyOffsetMs = latencyChoices[static_cast<std::size_t>(id - audioLatencyBase)];
    else if (id == audioNudgeEarlierId)
        settings.audioLatencyOffsetMs = std::min(settings.audioLatencyOffsetMs + nudgeMs, RecordSettings::maxLatencyOffsetMs);
    else if (id == audioNudgeLaterId)
        settings.audioLatencyOffsetMs = std::max(settings.audioLatencyOffsetMs - nudgeMs, -RecordSettings::maxLatencyOffsetMs);
    else if (id == punchId) settings.punch = !settings.punch;
    return settings;
}
} // namespace recordmenu

// One captured note-on/off, positioned inside the loop of the track being
// recorded. `cycle` counts loop passes since the clip started (it can be -1
// after latency compensation moves a note before the first downbeat).
enum class RecordEventKind : std::uint8_t { noteOn, noteOff, end, refused };
struct RecordEvent {
    RecordEventKind kind{RecordEventKind::end};
    std::uint8_t pitch{};
    float velocity{};
    std::int32_t cycle{};
    std::int32_t tick{}; // 0 .. loopTicks-1 (960 ticks per beat)
    std::uint32_t take{}; // which take asked for this event, so a late event of an old take is ignored
};

// Single audio-thread producer, single GUI consumer. Never blocks or allocates
// after construction. A full ring drops the event and counts it, so the take
// can tell the user instead of silently losing notes.
class RecordEventRing {
public:
    static constexpr std::size_t capacity = 8192;
    RecordEventRing() : events(std::make_unique<RecordEvent[]>(capacity)) {}
    bool push(const RecordEvent& event) noexcept {
        const auto w = write.load(std::memory_order_relaxed);
        const auto next = (w + 1) % capacity;
        if (next == read.load(std::memory_order_acquire)) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        events[w] = event;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(RecordEvent& event) noexcept {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        event = events[r];
        read.store((r + 1) % capacity, std::memory_order_release);
        return true;
    }
    std::uint64_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }
private:
    std::unique_ptr<RecordEvent[]> events;
    std::atomic<std::size_t> read{0}, write{0};
    std::atomic<std::uint64_t> dropped{0};
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
};

// Controls shared between the GUI and the audio thread. The audio thread owns
// `phase`; the GUI writes everything else.
struct RecordControls {
    enum Phase { idle = 0, countIn = 1, recording = 2 };
    std::atomic<bool> metronome{false};
    std::atomic<float> metronomeVolume{0.5f};
    std::atomic<int> countInBars{1};
    std::atomic<int> latencyOffsetMs{0};
    std::atomic<int> targetTrack{-1}, targetScene{-1};
    std::atomic<bool> startRequested{false}, stopRequested{false};
    std::atomic<int> phase{idle};
    std::atomic<std::uint32_t> take{0}; // set by the GUI before it asks to start
    // Set by the GUI before it asks to start: the take records audio from the armed tracks in
    // the Song view (see SongTakeCapture) instead of MIDI into a Session clip.
    std::atomic<bool> audioTake{false};
    RecordEventRing events;
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<int>::is_always_lock_free
                  && std::atomic<std::uint32_t>::is_always_lock_free);

    void apply(const RecordSettings& s) noexcept {
        metronome.store(s.metronome);
        metronomeVolume.store(s.metronomeVolume);
        countInBars.store(s.countInBars);
        latencyOffsetMs.store(s.latencyOffsetMs);
    }
    bool active() const noexcept { return phase.load(std::memory_order_relaxed) != idle; }
};

// Ticks the clock advances per audio sample (960 ticks per beat).
inline double ticksPerSample(double bpm, double sampleRate) noexcept {
    return sampleRate > 0.0 ? 16.0 * bpm / sampleRate : 0.0;
}

struct RecordPosition { std::int32_t cycle{}, tick{}; };

// Positions a played note inside the loop and moves it earlier by shiftTicks to
// make up for the delay between pressing a key and the playhead the engine
// reports. A shift can carry a note back across the loop start.
inline RecordPosition recordPosition(std::int64_t cycle, double positionTicks, std::int64_t shiftTicks,
                                     int loopTicks) noexcept {
    if (loopTicks < 1) return {};
    const std::int64_t total = cycle * loopTicks + std::llround(std::isfinite(positionTicks) ? positionTicks : 0.0) - shiftTicks;
    std::int64_t pass = total / loopTicks;
    std::int64_t tick = total - pass * loopTicks;
    if (tick < 0) { tick += loopTicks; --pass; }
    return {static_cast<std::int32_t>(pass), static_cast<std::int32_t>(tick)};
}

// GUI-side: turns the stream of note-on/off events into notes. Notes still held
// when recording ends are closed at the end position; a note that runs past the
// end of the loop is cut there; pressing a key that is already down restarts it.
class RecordingAssembler {
public:
    struct Note { int pitch{}; int startTick{}; int lengthTicks{}; float velocity{}; };
    static constexpr int minimumLengthTicks = 24; // about a tenth of a sixteenth note

    RecordingAssembler() { reset(PianoRollPattern::defaultLoopSteps); }
    explicit RecordingAssembler(int loopSteps) { reset(loopSteps); }

    void reset(int loopSteps) {
        loopTicks = std::clamp(loopSteps, 1, PianoRollPattern::maxLoopSteps) * PianoRollPattern::ticksPerStep;
        recorded.clear();
        open = {};
        overflow = finished = false;
    }

    void consume(const RecordEvent& event) {
        const auto absolute = static_cast<std::int64_t>(event.cycle) * loopTicks + event.tick;
        switch (event.kind) {
        case RecordEventKind::noteOn:
            if (event.pitch > 127) return;
            close(event.pitch, absolute);
            open[event.pitch] = {true, absolute, event.velocity};
            break;
        case RecordEventKind::noteOff:
            if (event.pitch <= 127) close(event.pitch, absolute);
            break;
        case RecordEventKind::end:
            for (int pitch = 0; pitch < 128; ++pitch) close(pitch, absolute);
            finished = true;
            break;
        case RecordEventKind::refused: // the take never started; nothing to assemble
            finished = true;
            break;
        }
    }

    const std::vector<Note>& notes() const noexcept { return recorded; }
    bool noteLimitReached() const noexcept { return overflow; }
    bool isFinished() const noexcept { return finished; }
    int loopLengthTicks() const noexcept { return loopTicks; }

    // base + recorded notes (or only the recorded notes when replacing). Notes
    // that no longer fit are dropped and `truncated` says so.
    PianoRollPattern mergeInto(const PianoRollPattern& base, bool replace, bool& truncated) const {
        truncated = overflow;
        PianoRollPattern result = base;
        if (replace) result.clear();
        const int limit = result.loopSteps * PianoRollPattern::ticksPerStep;
        for (const auto& note : recorded) {
            if (note.startTick >= limit) continue;
            const int length = std::min(note.lengthTicks, limit - note.startTick);
            if (result.add(note.pitch, PianoRollPattern::fromTicks(note.startTick), PianoRollPattern::fromTicks(length),
                           note.velocity) == 0) {
                truncated = true;
                break;
            }
        }
        return result;
    }

private:
    struct Open { bool down{}; std::int64_t start{}; float velocity{}; };
    void close(int pitch, std::int64_t absoluteEnd) {
        auto& held = open[static_cast<std::size_t>(pitch)];
        if (!held.down) return;
        held.down = false;
        if (recorded.size() >= PianoRollPattern::maxNotes) { overflow = true; return; }
        std::int64_t startTick = held.start % loopTicks;
        if (startTick < 0) startTick += loopTicks;
        const std::int64_t played = std::max<std::int64_t>(absoluteEnd - held.start, minimumLengthTicks);
        const auto length = std::clamp<std::int64_t>(played, 1, loopTicks - startTick);
        recorded.push_back({pitch, static_cast<int>(startTick), static_cast<int>(length),
                            std::clamp(held.velocity, 1.0f / 127.0f, 1.0f)});
    }

    int loopTicks{PianoRollPattern::defaultLoopSteps * PianoRollPattern::ticksPerStep};
    std::vector<Note> recorded;
    std::array<Open, 128> open{};
    bool overflow{}, finished{};
};

// A short decaying sine "tick": higher and louder on the first beat of a bar.
// Audio-thread safe: no allocation, one sine per sample while a click rings.
class MetronomeClick {
public:
    static constexpr double accentHz = 1568.0, beatHz = 1046.5, lengthSeconds = 0.04;
    void prepare(double rate) noexcept {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        total = std::max(1, static_cast<int>(sampleRate * lengthSeconds));
        decay = static_cast<float>(std::pow(0.001, 1.0 / total));
        stop();
    }
    void trigger(bool accent, float volume) noexcept {
        amplitude = 0.6f * std::clamp(std::isfinite(volume) ? volume : 0.0f, 0.0f, 1.0f) * (accent ? 1.0f : 0.75f);
        step = static_cast<float>(2.0 * 3.14159265358979323846 * (accent ? accentHz : beatHz) / sampleRate);
        phase = 0.0f; envelope = 1.0f; remaining = total;
    }
    void stop() noexcept { remaining = 0; }
    bool ringing() const noexcept { return remaining > 0; }
    float next() noexcept {
        if (remaining <= 0) return 0.0f;
        const float out = amplitude * envelope * std::sin(phase);
        phase += step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
        envelope *= decay;
        --remaining;
        return out;
    }
private:
    double sampleRate{44100.0};
    int total{1764}, remaining{};
    float decay{0.996f}, amplitude{}, step{}, phase{}, envelope{};
};

} // namespace alien
