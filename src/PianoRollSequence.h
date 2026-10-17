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
    int pitch{60}, startStep{}, lengthSteps{1};
    float velocity{0.8f};
};

// Message-thread model. Times and lengths are quantized sixteenth notes. Notes
// end at the clip boundary, never across it; MIDI pitches include 0 through 127.
struct PianoRollPattern {
    static constexpr std::size_t maxNotes = 128;
    static constexpr int defaultLoopSteps = 64, maxLoopSteps = 256;
    std::array<PianoRollNote, maxNotes> notes{};
    std::size_t count{};
    int loopSteps{defaultLoopSteps};

    bool valid() const noexcept {
        if (count > maxNotes || loopSteps < 1 || loopSteps > maxLoopSteps) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& n = notes[i];
            if (n.id == 0 || n.pitch < 0 || n.pitch > 127 || n.startStep < 0
                || n.startStep >= loopSteps || n.lengthSteps < 1
                || n.lengthSteps > loopSteps - n.startStep || !std::isfinite(n.velocity)
                || n.velocity <= 0 || n.velocity > 1) return false;
            for (std::size_t j = 0; j < i; ++j) if (notes[j].id == n.id) return false;
        }
        return true;
    }
    const PianoRollNote* find(std::uint32_t id) const noexcept {
        for (std::size_t i = 0; i < std::min(count, maxNotes); ++i)
            if (notes[i].id == id) return &notes[i];
        return nullptr;
    }
    PianoRollNote* find(std::uint32_t id) noexcept {
        return const_cast<PianoRollNote*>(static_cast<const PianoRollPattern&>(*this).find(id));
    }
    std::uint32_t add(int pitch, int start, int length, float velocity = 0.8f) noexcept {
        if (count == maxNotes || !valid() || !std::isfinite(velocity)) return 0;
        // The bounded search also handles ID wrap without reusing a live ID.
        for (std::size_t tries = 0; tries <= maxNotes; ++tries) {
            if (++nextId == 0) ++nextId;
            if (!find(nextId)) break;
        }
        start = std::clamp(start, 0, loopSteps - 1);
        notes[count++] = {nextId, std::clamp(pitch, 0, 127), start,
                         std::clamp(length, 1, loopSteps - start), boundedVelocity(velocity)};
        return nextId;
    }
    bool move(std::uint32_t id, int pitch, int start) noexcept {
        if (auto* note = find(id)) {
            note->pitch = std::clamp(pitch, 0, 127);
            // Moving preserves duration; resizing is a separate operation.
            note->startStep = std::clamp(start, 0, loopSteps - note->lengthSteps);
            return true;
        }
        return false;
    }
    bool resize(std::uint32_t id, int length) noexcept {
        if (auto* note = find(id)) {
            note->lengthSteps = std::clamp(length, 1, loopSteps - note->startStep);
            return true;
        }
        return false;
    }
    bool setVelocity(std::uint32_t id, float velocity) noexcept {
        if (!std::isfinite(velocity)) return false;
        if (auto* note = find(id)) { note->velocity = boundedVelocity(velocity); return true; }
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
            n.startStep = std::min(n.startStep, loopSteps - 1);
            n.lengthSteps = std::min(n.lengthSteps, loopSteps - n.startStep);
        }
    }
private:
    static float boundedVelocity(float value) noexcept { return std::clamp(value, 1.0f / 127.0f, 1.0f); }
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
// released. Same-pitch overlaps form one held gate until the last note ends;
// adjacent notes explicitly send off before on at their shared boundary.
class PianoRollScheduler {
public:
    template<class Send>
    void beginBlock(PianoRollMailbox& mailbox, bool play, double rate, double tempo, Send&& send) noexcept {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        bpm = std::isfinite(tempo) && tempo > 0 ? std::clamp(tempo, 1.0, 1000.0) : 120.0;
        if (!play) stop(send);
        PianoRollPattern updated;
        if (mailbox.consume(updated)) replace(updated, send);
        if (play && !running) { running = true; untilStep = 0; step = -1; nextStep = 0; }
    }
    template<class Send>
    int nextSample(Send&& send) noexcept {
        if (!running) return -1;
        int tick = -1;
        if (untilStep <= 0) {
            step = nextStep;
            nextStep = (step + 1) % pattern.loopSteps;
            tick = step;
            // Complete all releases before starting notes at this sample.
            for (std::size_t i = 0; i < pattern.count; ++i)
                if (active[i] && (step == 0 || pattern.notes[i].startStep + pattern.notes[i].lengthSteps <= step))
                    release(i, send);
            for (std::size_t i = 0; i < pattern.count; ++i)
                if (!active[i] && (pattern.notes[i].startStep == step || (reconcile && covers(pattern.notes[i], step))))
                    press(i, send);
            reconcile = false;
            intervalSamples = sampleRate * 60.0 / (bpm * 4.0);
            untilStep += intervalSamples;
        } else if (reconcile) {
            // An edit received mid-step starts new/relocated notes that cover
            // the current position; unchanged held notes are not retriggered.
            for (std::size_t i = 0; i < pattern.count; ++i)
                if (!active[i] && covers(pattern.notes[i], step)) press(i, send);
            reconcile = false;
        }
        untilStep -= 1.0;
        return tick;
    }
    template<class Send>
    void stop(Send&& send) noexcept {
        for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) release(i, send);
        running = false; reconcile = false; step = -1; nextStep = 0; untilStep = 0;
    }
    int currentStep() const noexcept { return step; }
    double positionStep() const noexcept {
        return step < 0 ? -1.0 : static_cast<double>(step)
            + std::clamp(1.0 - untilStep / intervalSamples, 0.0, 1.0);
    }
private:
    static bool covers(const PianoRollNote& n, int position) noexcept {
        return n.startStep <= position && position < n.startStep + n.lengthSteps;
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
    void replace(const PianoRollPattern& replacement, Send&& send) noexcept {
        std::array<bool, PianoRollPattern::maxNotes> retained{};
        if (replacement.loopSteps == pattern.loopSteps) {
            for (std::size_t i = 0; i < pattern.count; ++i) if (active[i]) {
                const auto& old = pattern.notes[i];
                bool keep = false;
                for (std::size_t j = 0; j < replacement.count; ++j) {
                    const auto& n = replacement.notes[j];
                    if (n.id == old.id && n.pitch == old.pitch && n.startStep == old.startStep
                        && n.lengthSteps == old.lengthSteps) { retained[j] = true; keep = true; break; }
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
    }
    PianoRollPattern pattern;
    std::array<bool, PianoRollPattern::maxNotes> active{};
    std::array<unsigned, 128> held{};
    double sampleRate{44100.0}, bpm{120.0}, untilStep{}, intervalSamples{1.0};
    int step{-1}, nextStep{};
    bool running{}, reconcile{};
};
}
