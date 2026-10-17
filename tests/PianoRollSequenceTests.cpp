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
        && pattern.find(first)->lengthSteps == PianoRollPattern::minLength && pattern.find(first)->velocity > 0, "creation clamps lower bounds");
    const auto last = pattern.add(200, 100, 200, 2.0f);
    require(pattern.find(last)->pitch == 127 && PianoRollPattern::toTicks(pattern.find(last)->startStep) == 64 * 240 - 1
        && pattern.find(last)->lengthSteps == PianoRollPattern::minLength && pattern.find(last)->velocity == 1, "creation clamps clip and MIDI upper bounds");
    require(pattern.resize(first, 4) && pattern.move(first, 60, 64), "move and resize find stable ID");
    require(pattern.find(first)->startStep == 60 && pattern.find(first)->lengthSteps == 4, "move preserves duration at clip edge");
    require(pattern.resize(first, 100) && pattern.find(first)->lengthSteps == 4, "resize cannot extend beyond clip");
    require(pattern.resize(first, -10) && pattern.find(first)->lengthSteps == PianoRollPattern::minLength, "resize has one-tick minimum");
    require(!pattern.setVelocity(first, std::numeric_limits<float>::quiet_NaN())
        && pattern.setVelocity(first, 0) && pattern.find(first)->velocity > 0, "velocity is finite and audible");
    require(pattern.add(60, 0, 1, std::numeric_limits<float>::infinity()) == 0, "invalid velocity rejected");
    require(pattern.erase(first) && !pattern.erase(first) && pattern.find(last), "erase preserves other IDs");
    require(!pattern.move(first, 60, 0) && !pattern.resize(first, 1), "stale selection cannot edit another note");
    pattern.setLoopSteps(16);
    require(PianoRollPattern::toTicks(pattern.find(last)->startStep) == 16 * 240 - 1 && pattern.valid(), "shorter clip clamps existing notes");
    pattern.clear();
    for (std::size_t i = 0; i < PianoRollPattern::maxNotes; ++i)
        require(pattern.add(static_cast<int>(i), 0, 1) != 0, "fixed note capacity");
    require(pattern.valid() && pattern.add(0, 0, 1) == 0, "capacity cannot overflow storage");
    pattern.clear(); require(pattern.valid() && pattern.count == 0, "clear valid empty pattern");
}

static void fractionalModelTests() {
    PianoRollPattern pattern;
    const auto id = pattern.add(60, 0.503, 0.127);
    require(id != 0 && PianoRollPattern::toTicks(pattern.find(id)->startStep) == 121
        && PianoRollPattern::toTicks(pattern.find(id)->lengthSteps) == 30,
        "fractional note geometry rounds to nearest 960-PPQ tick");
    require(pattern.move(id, 61, 1.0 / 240) && pattern.find(id)->startStep == PianoRollPattern::timeQuantum,
        "moving retains genuine sub-sixteenth timing");
    require(pattern.resize(id, 0.01) && PianoRollPattern::toTicks(pattern.find(id)->lengthSteps) == 2,
        "short chopped duration is represented by multiple ticks");
    require(!pattern.move(id, 70, std::numeric_limits<double>::quiet_NaN())
        && !pattern.resize(id, std::numeric_limits<double>::infinity())
        && pattern.add(60, std::numeric_limits<double>::infinity(), 1) == 0,
        "non-finite timing rejected without corrupting a valid clip");
    auto malformed = pattern;
    malformed.find(id)->startStep = 0.001;
    require(!malformed.valid(), "off-tick snapshot rejected");
    require(pattern.resize(id, -1) && pattern.find(id)->lengthSteps == PianoRollPattern::minLength,
        "minimum length is one tick rather than a whole sixteenth");
    require(pattern.move(id, 61, 1000)
        && PianoRollPattern::toTicks(pattern.find(id)->startStep) == 64 * 240 - 1,
        "fractional move respects final legal tick");
    pattern.setLoopSteps(4);
    require(pattern.valid() && PianoRollPattern::toTicks(pattern.find(id)->startStep) == 4 * 240 - 1,
        "clip-length change preserves final one-tick note without cropping its duration to zero");
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

static void fractionalTimingTests() {
    PianoRollPattern pattern;
    pattern.setLoopSteps(4);
    pattern.add(60, 0.125, 0.25);
    pattern.add(61, 0.5, PianoRollPattern::timeQuantum);
    pattern.add(62, 0.75, 0.125);
    pattern.add(63, 1.75, 0.5);
    pattern.add(64, 3.9, 0.1); // End exactly at the four-step loop boundary.
    pattern.add(67, 0.25, 0.25);
    pattern.add(67, 0.5, 0.25); // Adjacent same-pitch fractional gates.
    PianoRollMailbox mailbox;
    require(mailbox.publish(pattern), "fractional timing clip valid");
    PianoRollScheduler scheduler;
    Recorder record;
    constexpr double interval = 48000.0 * 60 / (137 * 4);
    const int sampleCount = static_cast<int>(std::ceil(12 * interval)) + 1;
    for (record.sample = 0; record.sample < sampleCount; ++record.sample) {
        if (record.sample % 113 == 0) scheduler.beginBlock(mailbox, true, 48000, 137, record);
        scheduler.nextSample(record);
    }
    require(record.count == pattern.count * 2 * 3, "all fractional notes release across three loop wraps");
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (std::size_t n = 0; n < pattern.count; ++n) {
            const auto& note = pattern.notes[n];
            for (int gate = 0; gate < 2; ++gate) {
                const double ideal = (cycle * 4 + note.startStep + (gate == 0 ? 0 : note.lengthSteps)) * interval;
                int matches = 0;
                for (std::size_t e = 0; e < record.count; ++e) {
                    const auto& event = record.events[e];
                    if (event.pitch == note.pitch && event.on == (gate == 0)
                        && event.sample >= ideal - 1e-6 && event.sample < ideal + 1.000001) ++matches;
                }
                require(matches == 1, "fractional start and duration release occur at first sample on or after ideal tick");
            }
        }
    }
    bool sawOn = false;
    int sample = -1;
    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& event = record.events[i];
        if (event.sample != sample) { sample = event.sample; sawOn = false; }
        require(event.on || !sawOn, "all same-sample fractional note-offs precede note-ons");
        sawOn |= event.on;
    }
    const auto beforeStop = record.count;
    scheduler.stop(record);
    require(record.count == beforeStop, "fractional final-loop releases leave no held pitches");

    PianoRollPattern tempoPattern;
    tempoPattern.setLoopSteps(2);
    tempoPattern.add(70, 0.75, 0.5);
    PianoRollMailbox tempoMailbox;
    tempoMailbox.publish(tempoPattern);
    PianoRollScheduler changingTempo;
    Recorder changes;
    changingTempo.beginBlock(tempoMailbox, true, 48000, 120, changes);
    for (changes.sample = 0; changes.sample < 10000; ++changes.sample) {
        if (changes.sample == 3000) changingTempo.beginBlock(tempoMailbox, true, 48000, 60, changes);
        changingTempo.nextSample(changes);
    }
    require(changes.count == 2 && changes.events[0].on && changes.events[0].sample == 4500
        && !changes.events[1].on && changes.events[1].sample == 9000,
        "substep start keeps pending tempo interval and note-off follows changed next-step tempo");
}

static void fractionalSnapshotTests() {
    PianoRollPattern pattern;
    const auto edited = pattern.add(60, 0.25, 0.5);
    const auto retained = pattern.add(64, 0, 2);
    const auto beforeEdit = pattern;
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 48000, 120, record);
    for (record.sample = 0; record.sample < 3000; ++record.sample) scheduler.nextSample(record);
    require(record.count == 2 && record.events[1].pitch == 60 && record.events[1].sample == 1500,
        "fractional clip plays before snapshot edit");
    pattern.resize(edited, 0.24); // Edited end now precedes the fractional cursor 0.5.
    pattern.add(67, 0.6, 0.2);
    pattern.add(69, 0.4, 0.2);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 48000, 120, record);
    require(record.count == 3 && !record.events[2].on && record.events[2].pitch == 60,
        "shortening a held note to before fractional cursor releases immediately");
    scheduler.nextSample(record);
    require(record.count == 4 && record.events[3].on && record.events[3].pitch == 69,
        "snapshot catches up only inserted notes covering the actual fractional cursor");
    for (record.sample = 3001; record.sample < 3600; ++record.sample) scheduler.nextSample(record);
    require(record.count == 4, "future fractional note does not start early after snapshot replacement");
    pattern.add(75, 0.6, 0.1);
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 48000, 120, record);
    scheduler.nextSample(record);
    require(record.count == 7 && !record.events[4].on && record.events[4].pitch == 69
        && record.events[5].on && record.events[5].pitch == 67
        && record.events[6].on && record.events[6].pitch == 75,
        "snapshot exactly at fractional boundary retains due off and starts each new note once");
    for (record.sample = 3601; record.sample < 3700; ++record.sample) scheduler.nextSample(record);
    mailbox.publish(beforeEdit); // Restoring a history snapshot keeps IDs stable.
    scheduler.beginBlock(mailbox, true, 48000, 120, record);
    scheduler.nextSample(record);
    require(record.count == 10 && !record.events[7].on && !record.events[8].on
        && record.events[9].on && record.events[9].pitch == 60,
        "undo-style snapshot releases removed fractional notes and restores currently covering note");
    int retainedEvents = 0;
    for (std::size_t i = 0; i < record.count; ++i) retainedEvents += record.events[i].pitch == beforeEdit.find(retained)->pitch;
    require(retainedEvents == 1, "unchanged held ID survives fractional edit and undo snapshots without retrigger");
    for (record.sample = 3701; record.sample <= 4500; ++record.sample) scheduler.nextSample(record);
    require(!record.events[record.count - 1].on && record.events[record.count - 1].pitch == 60
        && record.events[record.count - 1].sample == 4500, "restored fractional duration releases at original boundary");
    scheduler.stop(record);
}

static void snapshotCrossedEndTests() {
    PianoRollPattern pattern;
    const auto id = pattern.add(60, 0, PianoRollPattern::fromTicks(119));
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    for (record.sample = 0; record.sample < 50; ++record.sample) scheduler.nextSample(record);
    require(record.count == 1 && std::abs(scheduler.positionStep() - 0.5) < 1e-9,
        "last rendered sample precedes fractional end while upcoming sample follows it");
    pattern.setVelocity(id, 0.2f); // Geometry and ID unchanged in this snapshot.
    pattern.add(64, 0.6, 0.1); // Unrelated edit also must not lose a pending off.
    mailbox.publish(pattern);
    scheduler.beginBlock(mailbox, true, 800, 120, record);
    require(record.count == 2 && !record.events[1].on && record.events[1].pitch == 60
        && record.events[1].sample == 50,
        "snapshot seek releases geometrically unchanged held note whose end crossed between rendered samples");
    scheduler.nextSample(record);
    require(record.count == 2, "crossed-end note cannot restart during snapshot reconciliation");
    for (record.sample = 51; record.sample <= 70; ++record.sample) scheduler.nextSample(record);
    require(record.count == 4 && record.events[2].on && record.events[2].pitch == 64
        && record.events[2].sample == 60 && !record.events[3].on && record.events[3].sample == 70,
        "unrelated future fractional events survive crossed-end cleanup");
    scheduler.stop(record);
    require(record.count == 4, "snapshot cannot strand a held pitch until loop wrap");
}

static void subSampleGateTests() {
    PianoRollPattern pattern;
    pattern.setLoopSteps(1);
    pattern.add(60, 0, PianoRollPattern::timeQuantum);
    pattern.add(61, PianoRollPattern::fromTicks(1), PianoRollPattern::timeQuantum);
    pattern.add(62, PianoRollPattern::fromTicks(2), PianoRollPattern::fromTicks(2));
    pattern.add(63, PianoRollPattern::fromTicks(3), PianoRollPattern::fromTicks(3));
    pattern.add(64, PianoRollPattern::fromTicks(239), PianoRollPattern::timeQuantum);
    PianoRollMailbox mailbox;
    mailbox.publish(pattern);
    PianoRollScheduler scheduler;
    Recorder record;
    scheduler.beginBlock(mailbox, true, 800, 120, record); // One tick is less than one sample here.
    for (record.sample = 0; record.sample < 105; ++record.sample) scheduler.nextSample(record);
    require(record.count == 12, "two low-rate cycles contain only gates covering rendered samples");
    for (std::size_t i = 0; i < record.count; ++i)
        require(record.events[i].pitch != 61 && record.events[i].pitch != 64,
            "gates falling wholly between samples are silent and never left held");
    for (int cycle = 0; cycle < 2; ++cycle) {
        const auto first = static_cast<std::size_t>(cycle * 6);
        require(record.events[first].on && record.events[first].pitch == 60 && record.events[first].sample == cycle * 100
            && !record.events[first + 1].on && record.events[first + 1].sample == cycle * 100 + 1
            && record.events[first + 2].on && record.events[first + 2].pitch == 62 && record.events[first + 2].sample == cycle * 100 + 1
            && !record.events[first + 3].on && record.events[first + 3].sample == cycle * 100 + 2
            && record.events[first + 4].on && record.events[first + 4].pitch == 63 && record.events[first + 4].sample == cycle * 100 + 2
            && !record.events[first + 5].on && record.events[first + 5].sample == cycle * 100 + 3,
            "different tick times rounded to same sample release before starting notes");
    }
    const auto beforeStop = record.count;
    scheduler.stop(record);
    require(record.count == beforeStop, "subsample and loop-end gates do not leave stuck voices");
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
        for (int n = 0; n < 16; ++n) pattern.add(n + 48, n % 4 + (n % 3) * PianoRollPattern::timeQuantum,
                0.5 + (n % 5) * PianoRollPattern::timeQuantum);
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
    fractionalModelTests();
    mailboxTests();
    timingTests();
    fractionalTimingTests();
    fractionalSnapshotTests();
    snapshotCrossedEndTests();
    subSampleGateTests();
    overlapAndLoopTests();
    overlapEditAndRestartTests();
    editingAndAllocationTests();
    std::cout << "Piano-roll ticks, fractional timing/edits, mailbox concurrency, overlap, transport and allocation passed\n";
}
