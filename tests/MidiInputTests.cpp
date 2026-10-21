#include "MidiInputDeviceManager.h"
#include "SynthEngine.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static std::atomic<std::size_t> allocations{};
void* operator new(std::size_t n) { allocations.fetch_add(1); if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc{}; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void require(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
struct Recorder {
    struct Event { int note; float velocity; int channel; };
    std::array<Event, 10000> events{};
    std::array<std::array<bool, 128>, 16> gates{};
    std::size_t count{};
    void operator()(int n, float v, int ch) noexcept {
        require(count < events.size(), "MIDI recorder capacity");
        events[count++] = {n, v, ch};
        if (n >= 0) gates[static_cast<std::size_t>(ch - 1)][static_cast<std::size_t>(n)] = v > 0;
        else gates[static_cast<std::size_t>(ch - 1)] = {};
    }
    bool silent() const { for (const auto& ch : gates) for (bool n : ch) if (n) return false; return true; }
};
using namespace alien;
static void translationTests() {
    MidiNoteState state;
    Recorder r;
    state.handle(0x90, 60, 1, r); state.handle(0x90, 64, 127, r);
    require(r.count == 2 && r.events[0].velocity == 1.0f / 127.0f && r.events[1].velocity == 1,
        "MIDI chord velocity endpoints translate exactly");
    state.handle(0x90, 60, 100, r);
    require(r.count == 4 && r.events[2].velocity == 0 && r.events[3].velocity > 0,
        "repeated note-on releases prior gate then retriggers");
    state.handle(0x90, 60, 0, r); state.handle(0x80, 64, 77, r);
    require(r.silent(), "velocity-zero note-on and explicit note-off release notes");
    state.handle(0x90, 60, 100, r); state.handle(0x91, 60, 90, r);
    state.handle(0xb0, 64, 64, r); state.handle(0xb1, 64, 63, r);
    state.handle(0x80, 60, 0, r); state.handle(0x81, 60, 0, r);
    require(r.gates[0][60] && !r.gates[1][60] && state.pitchMask(60) == 1,
        "sustain threshold and same-pitch gates are independent per MIDI channel");
    state.handle(0x90, 60, 90, r); state.handle(0x80, 60, 0, r);
    state.handle(0xb0, 64, 0, r);
    require(r.silent(), "pedal-up releases a retriggered, sustained note exactly once");
    state.handle(0x9f, 127, 120, r); state.handle(0xbf, 64, 127, r);
    state.handle(0x8f, 127, 0, r); state.handle(0xbf, 121, 0, r);
    require(r.silent(), "reset controllers releases pedal-held notes on channel 16");
    state.handle(0x90, 0, 100, r); state.handle(0xb0, 64, 127, r);
    state.handle(0xb0, 123, 0, r);
    require(r.gates[0][0], "CC123 honors sustain");
    state.handle(0xb0, 120, 0, r);
    require(r.silent() && r.events[r.count - 1].note == -1, "CC120 hard-silences its source");
    const auto count = r.count;
    for (const auto message : std::array<std::array<int, 3>, 8>{{{0xf8, 0, 0}, {0xe0, 60, 2}, {0x90, 128, 1},
        {0x90, -1, 1}, {0x90, 60, 128}, {0x70, 60, 1}, {0xb0, 7, 127}, {0x100, 0, 0}}})
        state.handle(message[0], message[1], message[2], r);
    require(r.count == count, "unsupported/malformed messages never affect synth state");
    state.handle(0x90, 60, 127, r); state.reset(r); state.reset(r);
    require(r.silent() && state.pitchMask(60) == 0, "reset clears every held gate and pedal idempotently");
}
static void queueTests() {
    MidiInputPipe pipe;
    Recorder r;
    const auto token = pipe.beginSession();
    require(pipe.push(token, 0x90, 60, 127), "active MIDI session accepts events");
    pipe.process(r); require(pipe.isNoteOn(60), "audio publishes MIDI highlights");
    int mappedChannel = -1, mappedCC = -1, mappedValue = -1;
    pipe.push(token, 0xb2, 20, 96);
    pipe.process(r, [&](int channel, int cc, int value) {
        mappedChannel = channel; mappedCC = cc; mappedValue = value;
    });
    require(mappedChannel == 2 && mappedCC == 20 && mappedValue == 96,
        "general MIDI CCs pass through the bounded audio-thread control callback");
    pipe.push(token,0xe2,0,127);
    pipe.process(r,[&](int channel,int control,int value) {
        mappedChannel=channel;mappedCC=control;mappedValue=value;
    });
    require(mappedChannel==2&&mappedCC==128&&mappedValue==16256,
        "14-bit pitch bend passes through without becoming a note");
    require(pipe.isNoteOn(60), "unmapped MIDI controls do not alter active note gates");
    pipe.push(token, 0xb0, 64, 127); pipe.push(token, 0x80, 60, 0); pipe.process(r);
    require(pipe.isNoteOn(60), "highlight remains during sustain");
    pipe.requestReset(); pipe.process(r);
    require(r.silent() && !pipe.isNoteOn(60), "panic clears held notes and published state");
    pipe.push(token, 0x90, 64, 100); pipe.requestReset(); pipe.process(r);
    require(r.silent(), "panic discards pre-reset queued note-ons");
    const auto fresh = pipe.beginSession();
    require(!pipe.push(token, 0x90, 60, 127), "old device callbacks are rejected after session change");
    pipe.push(fresh, 0x90, 64, 127); pipe.process(r);
    require(pipe.isNoteOn(64), "replacement device works without restarting audio");
    for (int i = 0; i < 1000; ++i) pipe.push(fresh, 0x90, i % 128, 100);
    pipe.process(r);
    require(pipe.overflowCount() > 0 && r.silent(), "queue overload discards stale events and releases notes");
    pipe.push(fresh, 0x90, 60, 100); pipe.push(fresh, 0x80, 60, 0); pipe.process(r);
    require(r.silent(), "fresh input recovers after overflow");

    // MIDI state, queue consumption, reset and synth dispatch allocate nothing.
    alien::SynthEngine synth; synth.prepare(48000);
    auto send = [&](int n, float v, int ch) noexcept {
        if (n < 0) synth.allSoundOff(ch + 2);
        else if (v > 0) synth.noteOn(n, v, ch + 2); else synth.noteOff(n, ch + 2);
    };
    const auto before = allocations.load();
    for (int i = 0; i < 100; ++i) {
        pipe.push(fresh, 0x90, i % 128, 100); pipe.process(send);
        synth.nextSample(); pipe.resetAudio(send);
    }
    require(allocations.load() == before, "MIDI callback delivery, audio processing and cleanup never allocate");
}
struct FakeBackend final : MidiInputBackend {
    struct Connection final : MidiInputConnection {
        FakeBackend& owner; MidiInputPipe& pipe; MidiInputPipe::Token token;
        Connection(FakeBackend& o, MidiInputPipe& p, MidiInputPipe::Token t) : owner(o), pipe(p), token(t) {}
        void start() override { ++owner.starts; }
        void stop() override { ++owner.stops; owner.lateAccepted = pipe.push(token, 0x90, 99, 127); }
    };
    std::vector<MidiInputDevice> devices{{"one", "Keyboard"}, {"two", "Keyboard"}};
    int opens{}, starts{}, stops{}; bool fail{}, lateAccepted{}; MidiInputPipe::Token token{};
    std::vector<MidiInputDevice> available() override { return devices; }
    std::unique_ptr<MidiInputConnection> open(const std::string&, MidiInputPipe& pipe, MidiInputPipe::Token t) override {
        ++opens; token = t; if (fail) return {};
        return std::make_unique<Connection>(*this, pipe, t);
    }
};
static void lifecycleTests() {
    FakeBackend backend; MidiInputPipe pipe; Recorder r;
    {
        MidiInputDeviceManager manager(backend, pipe);
        require(manager.getState() == MidiInputDeviceManager::State::off && backend.opens == 0,
            "enumeration does not automatically open controllers");
        manager.selectDevice("one");
        require(backend.starts == 1 && manager.selectedIdentifier() == "one", "friendly device selected by stable ID");
        pipe.push(backend.token, 0x90, 60, 127); pipe.process(r);
        backend.devices.erase(backend.devices.begin()); manager.refresh(); pipe.process(r);
        require(manager.getState() == MidiInputDeviceManager::State::disconnected && r.silent()
            && !backend.lateAccepted, "unplug invalidates callbacks and releases held notes");
        backend.devices.push_back({"one", "Keyboard"}); manager.refresh();
        pipe.push(backend.token, 0x90, 64, 100); pipe.process(r);
        require(pipe.isNoteOn(64) && backend.starts == 2, "same ID reconnects automatically after enumeration reorders");
        manager.selectDevice("two"); pipe.process(r);
        require(r.silent() && manager.selectedIdentifier() == "two", "switching equal-name devices releases old notes");
        backend.fail = true; manager.selectDevice("one");
        require(manager.getState() == MidiInputDeviceManager::State::error, "open failure is exposed");
        const int attempts = backend.opens; manager.refresh();
        require(backend.opens == attempts, "failed opens are not retried continuously");
        backend.fail = false; manager.refresh(true);
        require(manager.getState() == MidiInputDeviceManager::State::connected, "explicit retry reconnects after open failure");
        const auto beforeRefresh = backend.token;
        manager.refresh(true);
        require(backend.token != beforeRefresh && !pipe.push(beforeRefresh, 0x90, 60, 127),
            "Refresh replaces stale connected handles and rejects their callbacks");
        manager.selectTest(); manager.sendTest(0xb0, 64, 127); manager.sendTest(0x90, 60, 100);
        manager.sendTest(0x80, 60, 0); pipe.process(r);
        require(pipe.isNoteOn(60), "hardware-free test input exercises sustain through the real pipe");
        manager.selectOff(); pipe.process(r);
        require(r.silent() && !manager.sendTest(0x90, 60, 100), "None disables input and clears sustain");
        manager.selectDevice("one"); pipe.push(backend.token, 0x90, 60, 127); pipe.process(r);
    }
    pipe.process(r); require(r.silent(), "manager shutdown closes input and releases all held notes");
}
static void concurrentTests() {
    MidiInputPipe pipe; const auto token = pipe.beginSession();
    std::atomic<int> finished{};
    auto producer = [&](int channel) {
        for (int i = 0; i < 10000; ++i) {
            pipe.push(token, 0x90 | channel, i % 128, 100);
            pipe.push(token, 0x80 | channel, i % 128, 0);
        }
        finished.fetch_add(1);
    };
    Recorder r;
    auto send = [&](int n, float v, int ch) noexcept {
        if (n >= 0) r.gates[static_cast<std::size_t>(ch - 1)][static_cast<std::size_t>(n)] = v > 0;
    };
    std::thread first(producer, 0), second(producer, 1);
    while (finished.load() != 2) pipe.process(send);
    first.join(); second.join(); pipe.requestReset(); pipe.process(send);
    require(r.silent(), "concurrent producers, contention and overflow cannot leave held notes after reset");
}
int main() {
    translationTests(); queueTests(); lifecycleTests(); concurrentTests();
    std::cout << "MIDI translation, sustain, bounded queue, device lifecycle, concurrency and allocation passed\n";
}
