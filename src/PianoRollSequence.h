#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace alien {
struct PianoRollNote {
    std::uint32_t id{};
    int pitch{60};
    double startStep{}, lengthSteps{1.0};
    float velocity{0.8f};
    float probability{1.0f};
};

// Message-thread model. One time unit is a sixteenth note, divided into 240
// integer ticks (960 PPQ). Notes end at the clip boundary, never across it.
// Public times remain doubles for editor geometry and processing operations.
struct PianoRollPattern {
    static constexpr std::size_t maxNotes = 1024;
    static constexpr int defaultLoopSteps = 64, maxLoopSteps = 256, ticksPerStep = 240;
    static constexpr double timeQuantum = 1.0 / ticksPerStep, minLength = timeQuantum;
    static int toTicks(double steps) noexcept {
        if (!std::isfinite(steps)) return 0;
        return static_cast<int>(std::llround(std::clamp(steps, -static_cast<double>(maxLoopSteps),
            static_cast<double>(maxLoopSteps)) * ticksPerStep));
    }
    static constexpr double fromTicks(int ticks) noexcept { return static_cast<double>(ticks) / ticksPerStep; }
    std::array<PianoRollNote, maxNotes> notes{};
    std::size_t count{};
    int loopSteps{defaultLoopSteps};

    bool valid() const noexcept {
        if (count > maxNotes || loopSteps < 1 || loopSteps > maxLoopSteps) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& n = notes[i];
            if (n.id == 0 || n.pitch < 0 || n.pitch > 127 || !std::isfinite(n.startStep)
                || !std::isfinite(n.lengthSteps) || n.startStep < 0 || n.startStep >= loopSteps
                || n.lengthSteps < minLength || n.lengthSteps > loopSteps
                || !std::isfinite(n.velocity) || n.velocity <= 0 || n.velocity > 1
                || !std::isfinite(n.probability) || n.probability < 0 || n.probability > 1) return false;
            const int start = toTicks(n.startStep), length = toTicks(n.lengthSteps);
            if (std::abs(n.startStep * ticksPerStep - start) > 1e-7
                || std::abs(n.lengthSteps * ticksPerStep - length) > 1e-7
                || length < 1 || length > loopSteps * ticksPerStep - start) return false;
        }
        // IDs must be unique. Sorting a fixed-size copy keeps this O(n log n)
        // and allocation-free, so it is also safe on the audio thread.
        std::array<std::uint32_t, maxNotes> ids{};
        for (std::size_t i = 0; i < count; ++i) ids[i] = notes[i].id;
        std::sort(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count));
        return std::adjacent_find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count)) == ids.begin() + static_cast<std::ptrdiff_t>(count);
    }
    const PianoRollNote* find(std::uint32_t id) const noexcept {
        for (std::size_t i = 0; i < std::min(count, maxNotes); ++i)
            if (notes[i].id == id) return &notes[i];
        return nullptr;
    }
    PianoRollNote* find(std::uint32_t id) noexcept {
        return const_cast<PianoRollNote*>(static_cast<const PianoRollPattern&>(*this).find(id));
    }
    std::uint32_t add(int pitch, double start, double length, float velocity = 0.8f,
                      float probability = 1.0f) noexcept {
        if (count == maxNotes || !valid() || !std::isfinite(velocity) || !std::isfinite(probability)
            || !std::isfinite(start) || !std::isfinite(length)) return 0;
        // The bounded search also handles ID wrap without reusing a live ID.
        for (std::size_t tries = 0; tries <= maxNotes; ++tries) {
            if (++nextId == 0) ++nextId;
            if (!find(nextId)) break;
        }
        const int startTicks = std::clamp(toTicks(start), 0, loopSteps * ticksPerStep - 1);
        const int lengthTicks = std::clamp(toTicks(length), 1, loopSteps * ticksPerStep - startTicks);
        notes[count++] = {nextId, std::clamp(pitch, 0, 127), fromTicks(startTicks),
                         fromTicks(lengthTicks), boundedVelocity(velocity), boundedProbability(probability)};
        return nextId;
    }
    bool move(std::uint32_t id, int pitch, double start) noexcept {
        if (!std::isfinite(start) || !valid()) return false;
        if (auto* note = find(id)) {
            note->pitch = std::clamp(pitch, 0, 127);
            // Moving preserves duration; resizing is a separate operation.
            note->startStep = fromTicks(std::clamp(toTicks(start), 0,
                loopSteps * ticksPerStep - toTicks(note->lengthSteps)));
            return true;
        }
        return false;
    }
    bool resize(std::uint32_t id, double length) noexcept {
        if (!std::isfinite(length) || !valid()) return false;
        if (auto* note = find(id)) {
            note->lengthSteps = fromTicks(std::clamp(toTicks(length), 1,
                loopSteps * ticksPerStep - toTicks(note->startStep)));
            return true;
        }
        return false;
    }
    bool setVelocity(std::uint32_t id, float velocity) noexcept {
        if (!std::isfinite(velocity)) return false;
        if (auto* note = find(id)) { note->velocity = boundedVelocity(velocity); return true; }
        return false;
    }
    bool setProbability(std::uint32_t id, float probability) noexcept {
        if (!std::isfinite(probability)) return false;
        if (auto* note = find(id)) { note->probability = boundedProbability(probability); return true; }
        return false;
    }
    bool erase(std::uint32_t id) noexcept {
        for (std::size_t i = 0; i < count; ++i) if (notes[i].id == id) {
            for (std::size_t j = i + 1; j < count; ++j) notes[j - 1] = notes[j];
            notes[--count] = {};
            return true;
        }
        return false;
    }
    void clear() noexcept { notes = {}; count = 0; }
    void setLoopSteps(int length) noexcept {
        loopSteps = std::clamp(length, 1, maxLoopSteps);
        for (std::size_t i = 0; i < count; ++i) {
            auto& n = notes[i];
            const int start = std::min(toTicks(n.startStep), loopSteps * ticksPerStep - 1);
            n.startStep = fromTicks(start);
            n.lengthSteps = fromTicks(std::min(toTicks(n.lengthSteps), loopSteps * ticksPerStep - start));
        }
    }
private:
    static float boundedVelocity(float value) noexcept { return std::clamp(value, 1.0f / 127.0f, 1.0f); }
    static float boundedProbability(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
    std::uint32_t nextId{};
};
static_assert(std::is_trivially_copyable_v<PianoRollPattern>);

// One GUI producer, one audio consumer. Each owns a buffer; the third is
// exchanged atomically. The producer can overwrite an unread snapshot, but the
// latest edit is always available, including a final deletion/clear. Neither
// side touches the other's buffer, retries, waits, allocates, or takes a lock.
class PianoRollMailbox {
public:
    bool publish(const PianoRollPattern& pattern) noexcept {
        if (!pattern.valid()) return false;
        buffers[back] = pattern;
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) & indexMask;
        return true;
    }
    bool consume(PianoRollPattern& destination) noexcept {
        if ((middle.load(std::memory_order_acquire) & dirty) == 0) return false;
        front = middle.exchange(front, std::memory_order_acq_rel) & indexMask;
        destination = buffers[front];
        return true;
    }
private:
    static constexpr unsigned dirty = 4, indexMask = 3;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::array<PianoRollPattern, 3> buffers{};
    std::atomic<unsigned> middle{1};
    unsigned front{}, back{2};
};

// Audio-thread owned. Call beginBlock once per callback and nextSample before
// rendering each sample. send(pitch, velocity, on) must route to a dedicated
// synth channel and must itself be real-time safe. Only this clip's notes are
// released. Same-pitch overlaps form one held gate until the last note ends.
// Each rendered sample dispatches releases before starts. A gate shorter than
// one sample that falls wholly between two samples is silent, never left held.
class PianoRollScheduler {
public:
    template<class Send>
    void beginBlock(PianoRollMailbox& mailbox, bool play, double rate, double tempo, Send&& send) noexcept {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        bpm = std::isfinite(tempo) && tempo > 0 ? std::clamp(tempo, 1.0, 1000.0) : 120.0;
        if (!play) stop(send);
        if (mailbox.consume(incoming)) replace(incoming, send);
        if (play && !running) {
            running = true; untilStep = 0; step = -1; nextStep = 0; nextEvent = 0; cycle = 0;
            activeClip = loadedClip;
        }
    }
    template<class Send>
    void beginBlock(bool play, double rate, double tempo, Send&& send) noexcept {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        bpm = std::isfinite(tempo) && tempo > 0 ? std::clamp(tempo, 1.0, 1000.0) : 120.0;
        if (!play) stop(send);
        if (play && !running) {
            running = true; untilStep = 0; step = -1; nextStep = 0; nextEvent = 0; cycle = 0;
            activeClip = loadedClip;
        }
    }
    bool queuePattern(const PianoRollPattern& replacement, int clipIndex, bool waitForSynchronizedLaunch = false,
                      int launchQuantizationSteps = 64) noexcept {
        if (!replacement.valid() || clipIndex < -1) return false;
        pendingPattern = replacement;
        pendingClip = clipIndex;
        pendingQuantizationSteps = launchQuantizationSteps > 0 ? launchQuantizationSteps : 64;
        pending = true;
        synchronizedPending = waitForSynchronizedLaunch;
        return true;
    }
    bool launchBoundaryDue(int quantizationSteps) const noexcept {
        return running && untilStep <= 0 && quantizationSteps > 0
            && (quantizationSteps >= 64 ? nextStep == 0 : quantizationPhase % quantizationSteps == 0);
    }
    template<class Send>
    void launchPendingNow(Send&& send) noexcept {
        if (pending) activatePending(send);
    }
    template<class Send>
    int nextSample(Send&& send) noexcept {
        if (!running) return -1;
        int tick = -1;
        if (untilStep <= 0) {
            if (nextStep == 0 && step >= 0) {
                // End the previous cycle before dispatching any new-cycle starts.
                dispatch(PianoRollPattern::fromTicks(pattern.loopSteps * PianoRollPattern::ticksPerStep), send);
                nextEvent = 0;
                ++cycle;
            }
            if (pending && !synchronizedPending && launchBoundaryDue(pendingQuantizationSteps))
                activatePending(send);
            step = nextStep;
            nextStep = (step + 1) % pattern.loopSteps;
            tick = step;
            // A tempo edit preserves the pending sixteenth boundary and takes
            // effect in the next interval, matching the existing step clock.
            intervalSamples = sampleRate * 60.0 / (bpm * 4.0);
            untilStep += intervalSamples;
            quantizationPhase = (quantizationPhase + 1) % 16;
        }
        const double position = samplePosition();
        dispatch(position, send);
        if (reconcile) {
            // Snapshot replacement seeks events at the actual fractional
            // playhead. Only notes covering this sample start immediately.
            for (std::size_t i = 0; i < pattern.count; ++i)
                if (!active[i] && covers(pattern.notes[i], position) && shouldPlay(pattern.notes[i])) press(i, send);
            reconcile = false;
        }
        untilStep -= 1.0;
        return tick;
    }
    template<class Send>
    void stop(Send&& send) noexcept {
        for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) release(i, send);
        running = false; reconcile = false; step = -1; nextStep = 0; nextEvent = 0; untilStep = 0;
        quantizationPhase = 0;
        activeClip = -1;
    }
    int currentStep() const noexcept { return step; }
    int activeClipIndex() const noexcept { return activeClip; }
    double positionStep() const noexcept { return step < 0 ? -1.0 : samplePosition(); }
private:
    struct Event { int ticks{}; std::size_t note{}; bool on{}; };
    static constexpr double tickTolerance = 1e-9;
    static bool covers(const PianoRollNote& n, double position) noexcept {
        const double ticks = position * PianoRollPattern::ticksPerStep;
        const int start = PianoRollPattern::toTicks(n.startStep);
        const int end = start + PianoRollPattern::toTicks(n.lengthSteps);
        return start <= ticks + tickTolerance && ticks < end - tickTolerance;
    }
    bool shouldPlay(const PianoRollNote& note) const noexcept {
        if (note.probability >= 1.0f) return true;
        if (note.probability <= 0.0f) return false;
        std::uint32_t value = note.id ^ (cycle * 0x9e3779b9u + 0x85ebca6bu);
        value ^= value >> 16; value *= 0x7feb352du;
        value ^= value >> 15; value *= 0x846ca68bu; value ^= value >> 16;
        return static_cast<double>(value) / 4294967296.0 < note.probability;
    }
    double samplePosition() const noexcept {
        return static_cast<double>(step) + std::clamp(1.0 - untilStep / intervalSamples, 0.0, 1.0);
    }
    template<class Send>
    void activatePending(Send&& send) noexcept {
        for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) release(i, send);
        pattern = pendingPattern;
        active.fill(false);
        held.fill(0);
        loadedClip = pendingClip;
        activeClip = pendingClip;
        pending = false;
        synchronizedPending = false;
        reconcile = false;
        // Rebuild from the new clip's beginning rather than seeking from the
        // previous clip's final step.
        step = -1;
        nextStep = 0;
        untilStep = 0;
        nextEvent = 0;
        rebuildEvents();
    }
    template<class Send>
    void press(std::size_t index, Send&& send) noexcept {
        const auto& n = pattern.notes[index];
        active[index] = true;
        if (held[static_cast<std::size_t>(n.pitch)]++ == 0) send(n.pitch, n.velocity, true);
    }
    template<class Send>
    void release(std::size_t index, Send&& send) noexcept {
        const auto& n = pattern.notes[index];
        active[index] = false;
        if (--held[static_cast<std::size_t>(n.pitch)] == 0) send(n.pitch, 0.0f, false);
    }
    template<class Send>
    void dispatch(double position, Send&& send) noexcept {
        const double ticks = position * PianoRollPattern::ticksPerStep;
        std::size_t end = nextEvent;
        while (end < eventCount && events[end].ticks <= ticks + tickTolerance) ++end;
        // Batch all due releases first, including different tick times rounded
        // to the same sample. Never start a note whose end is already due.
        for (std::size_t i = nextEvent; i < end; ++i)
            if (!events[i].on && active[events[i].note]) release(events[i].note, send);
        for (std::size_t i = nextEvent; i < end; ++i)
            if (events[i].on && !active[events[i].note] && covers(pattern.notes[events[i].note], position)
                && shouldPlay(pattern.notes[events[i].note]))
                press(events[i].note, send);
        nextEvent = end;
    }
    void rebuildEvents() noexcept {
        eventCount = 0;
        for (std::size_t i = 0; i < pattern.count; ++i) {
            const auto& n = pattern.notes[i];
            const int start = PianoRollPattern::toTicks(n.startStep);
            events[eventCount++] = {start, i, true};
            events[eventCount++] = {start + PianoRollPattern::toTicks(n.lengthSteps), i, false};
        }
        auto less = [](const Event& a, const Event& b) {
            if (a.ticks != b.ticks) return a.ticks < b.ticks;
            if (a.on != b.on) return !a.on;
            return a.note < b.note;
        };
        // At most 2 * maxNotes entries, sorted in place in O(n log n) using only
        // this fixed storage (no allocation on the audio thread).
        std::sort(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(eventCount), less);
        nextEvent = 0;
        if (step >= 0) {
            const double ticks = samplePosition() * PianoRollPattern::ticksPerStep;
            // Events exactly at the next sample position must still dispatch.
            while (nextEvent < eventCount && events[nextEvent].ticks < ticks - tickTolerance) ++nextEvent;
        }
    }
    template<class Send>
    void replace(const PianoRollPattern& replacement, Send&& send) noexcept {
        std::array<bool, PianoRollPattern::maxNotes> retained{};
        if (replacement.loopSteps == pattern.loopSteps) {
            for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) {
                const auto& old = pattern.notes[i];
                bool keep = false;
                for (std::size_t j = 0; j < replacement.count; ++j) {
                    const auto& n = replacement.notes[j];
                    if (n.id == old.id && n.pitch == old.pitch && n.startStep == old.startStep
                        && n.lengthSteps == old.lengthSteps && covers(n, samplePosition())) {
                        retained[j] = true; keep = true; break;
                    }
                }
                if (!keep) release(i, send);
            }
        } else {
            // Length changes restart at step zero to keep notes in range.
            for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) release(i, send);
            step = -1; nextStep = 0; untilStep = 0;
        }
        pattern = replacement;
        active = retained;
        reconcile = running && step >= 0;
        rebuildEvents();
    }
    PianoRollPattern pattern;
    PianoRollPattern pendingPattern;
    PianoRollPattern incoming; // Mailbox scratch: kept off the audio thread's stack.
    std::array<bool, PianoRollPattern::maxNotes> active{};
    std::array<unsigned, 128> held{};
    std::array<Event, PianoRollPattern::maxNotes * 2> events{};
    std::size_t eventCount{}, nextEvent{};
    double sampleRate{44100.0}, bpm{120.0}, untilStep{}, intervalSamples{1.0};
    int step{-1}, nextStep{};
    std::uint32_t cycle{};
    int loadedClip{-1}, activeClip{-1}, pendingClip{-1}, pendingQuantizationSteps{64}, quantizationPhase{};
    bool running{}, reconcile{}, pending{}, synchronizedPending{};
};
}
