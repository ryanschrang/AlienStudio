#include "PianoRollSequence.h"
#include "SynthEngine.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

static std::atomic<std::size_t> allocations{};
void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct Event { int sample{}, pitch{}; float velocity{}; bool on{}; };
struct Recorder {
    std::array<Event, 8192> events{};
    std::size_t count{};
    int sample{};
    void operator()(int pitch, float velocity, bool on) noexcept {
        require(count < events.size(), "event recorder overflow");
        events[count++] = {sample, pitch, velocity, on};
    }
};
using namespace alien;

static void modelTests() {
    PianoRollPattern pattern;
    require(pattern.valid() && pattern.count == 0 && pattern.loopSteps == 64, "empty four-bar clip");
    const auto first = pattern.add(-12, -9, 0, -2.0f);
    require(first != 0 && pattern.find(first)->pitch == 0 && pattern.find(first)->startStep == 0
        && pattern.find(first)->lengthSteps == 1 && pattern.find(first)->velocity > 0, "creation clamps lower bounds");
    const auto last = pattern.add(200, 100, 200, 2.0f);
    require(pattern.find(last)->pitch == 127 && pattern.find(last)->startStep == 63
        && pattern.find(last)->lengthSteps == 1 && pattern.find(last)->velocity == 1, "creation clamps clip and MIDI upper bounds");
    require(pattern.resize(first, 4) && pattern.move(first, 60, 64), "move and resize find stable ID");
    require(pattern.find(first)->startStep == 60 && pattern.find(first)->lengthSteps == 4, "move preserves duration at clip edge");
    require(pattern.resize(first, 100) && pattern.find(first)->lengthSteps == 4, "resize cannot extend beyond clip");
    require(pattern.resize(first, -10) && pattern.find(first)->lengthSteps == 1, "resize has one-step minimum");
    require(!pattern.setVelocity(first, std::numeric_limits<float>::quiet_NaN())
        && pattern.setVelocity(first, 0) && pattern.find(first)->velocity > 0, "velocity is finite and audible");
    require(pattern.add(60, 0, 1, std::numeric_limits<float>::infinity()) == 0, "invalid velocity rejected");
    require(pattern.erase(first) && !pattern.erase(first) && pattern.find(last), "erase preserves other IDs");
    require(!pattern.move(first, 60, 0) && !pattern.resize(first, 1), "stale selection cannot edit another note");
    pattern.setLoopSteps(16);
    require(pattern.find(last)->startStep == 15 && pattern.valid(), "shorter clip clamps existing notes");
    pattern.clear();
    for (std::size_t i = 0; i < PianoRollPattern::maxNotes; ++i)
        require(pattern.add(static_cast<int>(i), 0, 1) != 0, "fixed note capacity");
    require(pattern.valid() && pattern.add(0, 0, 1) == 0, "capacity cannot overflow storage");
    pattern.clear(); require(pattern.valid() && pattern.count == 0, "clear valid empty pattern");
}

static void mailboxTests() {
    PianoRollMailbox mailbox;
    PianoRollPattern pattern, received;
    const auto id = pattern.add(60, 0, 1);
    require(!mailbox.consume(received), "mailbox initially unchanged");
    for (int i = 0; i < 10000; ++i) {
        pattern.move(id, i % 128, i % 64);
        require(mailbox.publish(pattern), "publish accepts valid pattern");
    }
    require(mailbox.consume(received) && received.find(id)->pitch == 9999 % 128
        && received.find(id)->startStep == 9999 % 64, "latest snapshot survives producer overrun");
    require(!mailbox.consume(received), "snapshot consumed once");
    pattern.count = PianoRollPattern::maxNotes + 1;
    require(!mailbox.publish(pattern) && !mailbox.consume(received), "malformed snapshot never reaches audio");

    // Stress publication/consumption concurrently. Every note encodes the same
    // revision, detecting torn copies and buffer ownership errors.
    PianoRollMailbox concurrent;
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        PianoRollPattern value;
        for (int i = 0; i < 8; ++i) value.add(60, 0, 1);
        for (std::uint32_t revision = 1; revision <= 12000; ++revision) {
            for (std::size_t i = 0; i < value.count; ++i) {
                value.notes[i].id = revision * 16 + static_cast<std::uint32_t>(i);
                value.notes[i].pitch = static_cast<int>(revision % 128);
                value.notes[i].startStep = static_cast<int>(revision % 64);
            }
            require(concurrent.publish(value), "stress snapshot valid");
        }
        finished.store(true, std::memory_order_release);
    });
    std::uint32_t previous{};
    auto read = [&] {
        if (!concurrent.consume(received)) return;
        const auto revision = received.notes[0].id / 16;
        require(revision >= previous && received.count == 8 && received.valid(), "snapshots are coherent and monotonic");
        for (std::size_t i = 0; i < received.count; ++i)
            require(received.notes[i].id == revision * 16 + static_cast<std::uint32_t>(i)
                && received.notes[i].pitch == static_cast<int>(revision % 128)
                && received.notes[i].startStep == static_cast<int>(revision % 64), "snapshot not torn");
        previous = revision;
    };
    while (!finished.load(std::memory_order_acquire)) read();
    producer.join();
    read();
    require(previous == 12000, "final edit is eventually delivered without another producer action");
}

static void timingTests() {
    PianoRollPattern pattern;
    pattern.add(60, 0, 1, 0.4f);
    PianoRollMailbox mailbox;
    require(mailbox.publish(pattern), "timing pattern publish");
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 48000, 137, record);
    int ticks{};
    constexpr double interval = 48000.0 * 60 / (137 * 4);
    for (record.sample = 0; record.sample < 1100000; ++record.sample) {
        const int tick = scheduler.nextSample(record);
        if (tick >= 0) {
            const auto ideal = ticks * interval;
            require(record.sample >= ideal - 1e-6 && record.sample < ideal + 1.000001, "sample-accurate fractional tick without drift");
            require(tick == ticks % 64, "four-bar loop step order");
            ++ticks;
        }
    }
    require(ticks == 210 && record.count == 8, "four-bar loop restarts at expected tempo");
    for (std::size_t i = 0; i < record.count; ++i) {
        const double ideal = ((i / 2) * 64 + (i % 2)) * interval;
        const auto& event = record.events[i];
        require(event.sample >= ideal - 1e-6 && event.sample < ideal + 1.000001
            && event.pitch == 60 && event.on == (i % 2 == 0), "note-on and explicit duration note-off are sample accurate");
    }
    scheduler.stop(record);
    require(scheduler.currentStep() == -1 && scheduler.positionStep() == -1, "stop clears playhead");
    scheduler.beginBlock(mailbox, true, 48000, 120, record);
    require(scheduler.nextSample(record) == 0 && record.events[record.count - 1].on, "restart starts clip at step zero");

    PianoRollScheduler changingTempo;
    PianoRollMailbox empty;
    Recorder changes;
    changingTempo.beginBlock(empty, true, 800, 120, changes); // 100 samples/step
    for (int i = 0; i < 50; ++i) changingTempo.nextSample(changes);
    require(std::abs(changingTempo.positionStep() - 0.5) < 1e-9, "fractional GUI playhead");
    changingTempo.beginBlock(empty, true, 800, 60, changes); // next step takes 200 samples
    for (int i = 0; i < 50; ++i) require(changingTempo.nextSample(changes) == -1, "tempo change preserves pending boundary");
    require(changingTempo.nextSample(changes) == 1, "tempo change does not reset transport");
    for (int i = 1; i < 200; ++i) require(changingTempo.nextSample(changes) == -1, "new tempo duration");
    require(changingTempo.nextSample(changes) == 2, "tempo change takes effect at next step without accumulating drift");
}

static void overlapAndLoopTests() {
    PianoRollPattern pattern;
    pattern.setLoopSteps(4);
    pattern.add(60, 0, 2, 0.3f);
    pattern.add(60, 1, 2, 0.9f);
    pattern.add(64, 0, 1);
    pattern.add(67, 1, 1);
    pattern.add(72, 0, 1);
    pattern.add(72, 1, 1);
    pattern.add(127, 3, 1);
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    for (record.sample = 0; record.sample <= 400; ++record.sample) scheduler.nextSample(record);
    int sixtyOn{}, sixtyOff{};
    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& event = record.events[i];
        if (event.pitch == 60) {
            if (event.on) { ++sixtyOn; require(event.sample == 0 || event.sample == 400, "same-pitch overlap does not retrigger"); }
            else { ++sixtyOff; require(event.sample == 300, "same-pitch overlap held until final end"); }
        }
        if (event.sample == 100 && event.pitch == 72 && event.on) {
            bool sawOff = false;
            for (std::size_t j = 0; j < i; ++j)
                sawOff |= record.events[j].sample == 100 && record.events[j].pitch == 72 && !record.events[j].on;
            require(sawOff, "adjacent same-pitch notes send off before on");
        }
        if (event.sample == 400 && event.on) {
            bool sawEndOff = false;
            for (std::size_t j = 0; j < i; ++j)
                sawEndOff |= record.events[j].sample == 400 && record.events[j].pitch == 127 && !record.events[j].on;
            require(sawEndOff, "last-step note-off precedes new loop note-ons");
        }
    }
    require(sixtyOn == 2 && sixtyOff == 1, "overlap refcounts balanced");
    const auto beforeStop = record.count;
    scheduler.stop(record);
    require(record.count == beforeStop + 3, "stop explicitly releases every held pitch once");
    scheduler.stop(record);
    scheduler.nextSample(record);
    require(record.count == beforeStop + 3, "stop is idempotent and silences future events");
}

static void overlapEditAndRestartTests() {
    PianoRollPattern pattern;
    const auto first = pattern.add(60, 0, 8);
    const auto second = pattern.add(60, 0, 8);
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.count == 1 && record.events[0].on, "overlapping unison starts one gate");
    pattern.erase(first);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.count == 1, "deleting one overlapping note retains surviving unison gate");
    scheduler.stop(record);
    require(record.count == 2 && !record.events[1].on, "stop drains overlapping pitch references");
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.count == 3 && record.events[2].on, "restart cannot inherit stale held pitch references");
    pattern.resize(second, 1);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.count == 5 && !record.events[3].on && record.events[4].on,
        "resizing active note explicitly replaces its held gate");
    for (int i = 2; i <= 100; ++i) scheduler.nextSample(record);
    require(record.count == 6 && !record.events[5].on, "resized note ends at edited boundary");
}

static void editingAndAllocationTests() {
    PianoRollPattern pattern;
    const auto changed = pattern.add(60, 0, 8);
    const auto unchanged = pattern.add(64, 0, 8);
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    for (record.sample = 0; record.sample < 50; ++record.sample) scheduler.nextSample(record);
    require(record.count == 2, "initial chord");
    pattern.move(changed, 67, 0);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    require(record.count == 3 && !record.events[2].on && record.events[2].pitch == 60, "moving held note releases original pitch at block boundary");
    scheduler.nextSample(record);
    require(record.count == 4 && record.events[3].on && record.events[3].pitch == 67, "edited note starts at current position");
    pattern.setVelocity(unchanged, 0.2f);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.count == 4, "unmodified held note and velocity edits avoid retriggering");
    pattern.erase(changed);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    require(record.count == 5 && !record.events[4].on && record.events[4].pitch == 67, "deleting active note releases immediately");
    for (int i = 0; i < 1000; ++i) { pattern.move(unchanged, i % 128, 0); mailbox.publish(pattern); }
    pattern.clear();
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    require(record.count == 6 && !record.events[5].on && record.events[5].pitch == 64, "final clear cannot be lost when producer outruns audio");
    scheduler.nextSample(record);
    require(record.count == 6, "cleared snapshot has no stale note-ons");

    pattern.add(0, 0, 64);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    scheduler.nextSample(record);
    require(record.events[record.count - 1].pitch == 0 && record.events[record.count - 1].on, "MIDI pitch zero supported");
    pattern.setLoopSteps(16);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    require(!record.events[record.count - 1].on && scheduler.nextSample(record) == 0, "loop-length edit releases and restarts cleanly");
    scheduler.stop(record);

    // Exercise snapshot replacement, stop/restart, all notes, and rendering
    // under allocation accounting. The synth's dedicated channel keeps live
    // keyboard voices outside the clip lifecycle.
    SynthEngine synth;
    synth.prepare(48000);
    synth.setPatch({Waveform::sine, 0, 0, 1, 0.001, 8000});
    synth.noteOn(60, 1, 0);
    auto send = [&](int pitch, float velocity, bool on) noexcept {
        if (on) synth.noteOn(pitch, velocity, 2); else synth.noteOff(pitch, 2);
    };
    const auto before = allocations.load();
    pattern.clear();
    pattern.add(60, 0, 1);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 48000, 120, send);
    scheduler.nextSample(send);
    require(synth.activeVoices() == 2, "clip and computer keyboard can hold same pitch on distinct channels");
    scheduler.stop(send);
    for (int i = 0; i < 100; ++i) synth.nextSample();
    require(synth.activeVoices() == 1, "clip stop leaves computer keyboard note held");
    for (int iteration = 0; iteration < 100; ++iteration) {
        pattern.clear();
        for (int n = 0; n < 16; ++n) pattern.add(n + 48, n % 4, 2);
        mailbox.publish(pattern);
        scheduler.beginBlock(mailbox, true, 48000, 137, send);
        for (int sample = 0; sample < 1000; ++sample) {
            scheduler.nextSample(send);
            synth.nextSample();
        }
        scheduler.stop(send);
    }
    require(allocations.load() == before, "audio scheduler, snapshot delivery and DSP never allocate");
}

int main() {
    modelTests();
    mailboxTests();
    timingTests();
    overlapAndLoopTests();
    overlapEditAndRestartTests();
    editingAndAllocationTests();
    std::cout << "Piano-roll edits, mailbox concurrency, timing, overlap, transport and allocation passed\n";
}
