#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace alien {
// Audio-thread-owned MIDI gates. Repeated note-ons retrigger; one note-off
// releases the current gate. MIDI channels have independent sustain/ownership.
class MidiNoteState {
public:
    static bool supported(int status, int a, int b) noexcept {
        if (status < 0x80 || status > 0xef || a < 0 || a > 127 || b < 0 || b > 127) return false;
        return (status & 0xf0) == 0x80 || (status & 0xf0) == 0x90
            || ((status & 0xf0) == 0xb0 && (a == 64 || a == 120 || a == 121 || a == 123));
    }
    template<class Send> void handle(int status, int a, int b, Send&& send) noexcept {
        if (!supported(status, a, b)) return;
        const auto channel = static_cast<std::size_t>(status & 15);
        auto& c = channels[channel];
        if ((status & 0xf0) == 0xb0) {
            if (a == 64) {
                c.sustain = b >= 64;
                if (!c.sustain) for (int n = 0; n < 128; ++n)
                    if (!c.down[static_cast<std::size_t>(n)]) release(channel, n, send);
            } else if (a == 121) {
                c.sustain = false;
                for (int n = 0; n < 128; ++n) if (!c.down[static_cast<std::size_t>(n)]) release(channel, n, send);
            } else {
                for (int n = 0; n < 128; ++n) {
                    c.down[static_cast<std::size_t>(n)] = false;
                    if (a == 120 || !c.sustain) release(channel, n, send);
                }
                // Negative pitch is a source-local hard silence, used by CC120.
                if (a == 120) { c.sustain = false; send(-1, 0.0f, static_cast<int>(channel) + 1); }
            }
            return;
        }
        const auto note = static_cast<std::size_t>(a);
        if ((status & 0xf0) == 0x90 && b > 0) {
            release(channel, a, send);
            c.down[note] = c.sounding[note] = true;
            masks[note] |= static_cast<std::uint16_t>(1u << channel);
            send(a, static_cast<float>(b) / 127.0f, static_cast<int>(channel) + 1);
        } else {
            c.down[note] = false;
            if (!c.sustain) release(channel, a, send);
        }
    }
    template<class Send> void reset(Send&& send) noexcept {
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            for (int n = 0; n < 128; ++n) release(ch, n, send);
            channels[ch] = {};
        }
    }
    std::uint16_t pitchMask(std::size_t note) const noexcept { return note < masks.size() ? masks[note] : 0; }
private:
    template<class Send> void release(std::size_t ch, int note, Send&& send) noexcept {
        const auto n = static_cast<std::size_t>(note);
        if (!channels[ch].sounding[n]) return;
        channels[ch].sounding[n] = false;
        masks[n] &= static_cast<std::uint16_t>(~(1u << ch));
        send(note, 0.0f, static_cast<int>(ch) + 1);
    }
    struct Channel { std::array<bool, 128> down{}, sounding{}; bool sustain{}; };
    std::array<Channel, 16> channels{};
    std::array<std::uint16_t, 128> masks{};
};

// Bounded multiple-producer/single-audio-consumer pipe. Producers make ONE
// nonblocking attempt to claim the write side; contention/full storage requests
// a reset instead of losing a note-off. The audio side never claims this flag,
// waits, allocates, logs, or accesses device handles. Sessions reject late device
// callbacks; revisions discard events queued before a panic/reset.
class MidiInputPipe {
public:
    using Token = std::uint64_t;
    Token beginSession() noexcept {
        const auto token = session.fetch_add(1, std::memory_order_acq_rel) + 1;
        requestReset();
        return token;
    }
    void requestReset() noexcept { revision.fetch_add(1, std::memory_order_acq_rel); }
    bool push(Token token, int status, int a, int b) noexcept {
        if (token != session.load(std::memory_order_acquire) || token == 0
            || !MidiNoteState::supported(status, a, b)) return false;
        received.fetch_add(1, std::memory_order_relaxed);
        const auto eventRevision = revision.load(std::memory_order_acquire);
        if (writer.test_and_set(std::memory_order_acquire)) { overflow(); return false; }
        const auto w = write.load(std::memory_order_relaxed);
        const auto next = (w + 1) % events.size();
        if (next == read.load(std::memory_order_acquire)) {
            writer.clear(std::memory_order_release); overflow(); return false;
        }
        events[w] = {token, eventRevision, status, a, b};
        write.store(next, std::memory_order_release);
        writer.clear(std::memory_order_release);
        return true;
    }
    template<class Send> void process(Send&& send) noexcept {
        synchronize(send);
        // Bound consumption even while producers continue to send.
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto r = read.load(std::memory_order_relaxed);
            if (r == write.load(std::memory_order_acquire)) break;
            const auto e = events[r];
            read.store((r + 1) % events.size(), std::memory_order_release);
            if (e.token == session.load(std::memory_order_acquire) && e.revision == appliedRevision
                && e.revision == revision.load(std::memory_order_acquire))
                state.handle(e.status, e.a, e.b, send);
        }
        synchronize(send);
        publish();
    }
    template<class Send> void resetAudio(Send&& send) noexcept {
        requestReset(); synchronize(send); publish();
    }
    bool isNoteOn(int note) const noexcept {
        return note >= 0 && note < 128 && published[static_cast<std::size_t>(note)].load(std::memory_order_relaxed) != 0;
    }
    std::uint64_t activity() const noexcept { return received.load(std::memory_order_relaxed); }
    std::uint64_t overflowCount() const noexcept { return overflows.load(std::memory_order_relaxed); }
private:
    void overflow() noexcept { overflows.fetch_add(1, std::memory_order_relaxed); requestReset(); }
    template<class Send> void synchronize(Send&& send) noexcept {
        const auto latest = revision.load(std::memory_order_acquire);
        if (latest != appliedRevision) { state.reset(send); appliedRevision = latest; }
    }
    void publish() noexcept {
        for (std::size_t n = 0; n < published.size(); ++n) published[n].store(state.pitchMask(n), std::memory_order_relaxed);
    }
    struct Event { Token token{}, revision{}; int status{}, a{}, b{}; };
    std::array<Event, 512> events{};
    std::atomic<std::size_t> read{}, write{};
    std::atomic_flag writer = ATOMIC_FLAG_INIT;
    std::atomic<Token> session{}, revision{1}, received{}, overflows{};
    std::array<std::atomic<std::uint16_t>, 128> published{};
    MidiNoteState state;
    Token appliedRevision{};
    static_assert(std::atomic<Token>::is_always_lock_free && std::atomic<std::uint16_t>::is_always_lock_free
        && std::atomic<std::size_t>::is_always_lock_free);
};
}
