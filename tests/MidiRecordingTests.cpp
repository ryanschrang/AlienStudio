#include "ProjectController.h"
#include "MidiRecording.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

// MIDI recording, overdub, count-in and metronome: the pure pieces, the audio
// engine (rendered offline through the real device callback) and the project
// controller that turns a take into one undo step. No audio device, MIDI
// hardware or window is needed.

// Count heap allocation on the calling thread, including real audio callbacks.
static thread_local std::size_t allocationCount = 0;
void* operator new(std::size_t size) {
    ++allocationCount;
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

using alien::PianoRollNote;
using alien::PianoRollPattern;
using alien::RecordEvent;
using alien::RecordEventKind;
using alien::RecordControls;
using alien::RecordSettings;

namespace {

constexpr int loopTicks = 64 * PianoRollPattern::ticksPerStep; // the default 64-step loop

RecordEvent event(RecordEventKind kind, int pitch, float velocity, int cycle, int tick, std::uint32_t take = 0) {
    RecordEvent e;
    e.kind = kind; e.pitch = static_cast<std::uint8_t>(pitch); e.velocity = velocity;
    e.cycle = cycle; e.tick = tick; e.take = take;
    return e;
}
RecordEvent on(int pitch, int cycle, int tick, float velocity = 0.8f) { return event(RecordEventKind::noteOn, pitch, velocity, cycle, tick); }
RecordEvent off(int pitch, int cycle, int tick) { return event(RecordEventKind::noteOff, pitch, 0.0f, cycle, tick); }
RecordEvent endAt(int cycle, int tick) { return event(RecordEventKind::end, 0, 0.0f, cycle, tick); }

bool closeTo(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

// --- Pure pieces -------------------------------------------------------------

void settingsTests() {
    RecordSettings s;
    require(s.valid() && !s.metronome && s.countInBars == 1 && s.latencyOffsetMs == 0 && !s.replace,
        "default recording settings: one bar count-in, metronome off, overdub");
    auto bad = [&](auto change) { RecordSettings t; change(t); return !t.valid(); };
    require(bad([](RecordSettings& t) { t.countInBars = 3; }) && bad([](RecordSettings& t) { t.countInBars = -1; }),
        "count-in is 0 to 2 bars");
    require(bad([](RecordSettings& t) { t.latencyOffsetMs = 101; }) && bad([](RecordSettings& t) { t.latencyOffsetMs = -101; }),
        "latency offset stays within 100 ms");
    require(bad([](RecordSettings& t) { t.metronomeVolume = 1.01f; }) && bad([](RecordSettings& t) { t.metronomeVolume = -0.1f; })
            && bad([](RecordSettings& t) { t.metronomeVolume = std::numeric_limits<float>::quiet_NaN(); }),
        "metronome volume is a finite 0..1");
    RecordSettings a, b;
    require(a == b, "equal settings compare equal");
    b.replace = true;
    require(!(a == b), "different settings compare different");
    std::cout << "PASS: recording settings\n";
}

void ringTests() {
    alien::RecordEventRing ring;
    RecordEvent e;
    require(!ring.pop(e), "an empty ring has nothing to pop");
    for (int i = 0; i < 5; ++i) require(ring.push(on(60 + i, i, i)), "push");
    for (int i = 0; i < 5; ++i) require(ring.pop(e) && e.pitch == 60 + i && e.cycle == i, "events come out in order");
    // Wrap around the end several times.
    for (std::size_t round = 0; round < 3 * alien::RecordEventRing::capacity; ++round) {
        require(ring.push(on(1, static_cast<int>(round), 0)) && ring.pop(e) && e.cycle == static_cast<int>(round), "wraparound keeps order");
    }
    // A full ring drops the event and counts it instead of blocking or overwriting.
    std::size_t accepted = 0;
    while (ring.push(on(2, static_cast<int>(accepted), 0))) ++accepted;
    require(accepted == alien::RecordEventRing::capacity - 1 && ring.droppedCount() == 1, "a full ring refuses and counts the drop");
    require(ring.pop(e) && e.cycle == 0 && ring.push(on(3, 0, 0)), "popping frees room again");
    // One producer and one consumer on separate threads never lose or reorder events that were accepted.
    alien::RecordEventRing shared;
    constexpr int total = 300000;
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (int i = 0; i < total; ++i) shared.push(on(0, i, 0));
        producerDone.store(true);
    });
    int received = 0, last = -1;
    bool ordered = true;
    for (;;) {
        RecordEvent got;
        if (shared.pop(got)) { ordered = ordered && got.cycle > last; last = got.cycle; ++received; }
        else if (producerDone.load()) { if (!shared.pop(got)) break; ordered = ordered && got.cycle > last; last = got.cycle; ++received; }
        else std::this_thread::yield();
    }
    producer.join();
    require(ordered && static_cast<std::uint64_t>(received) + shared.droppedCount() == total,
        "across threads every event is either received in order or counted as dropped");
    std::cout << "PASS: record event ring\n";
}

void positionTests() {
    auto p = alien::recordPosition(0, 960.0, 0, loopTicks);
    require(p.cycle == 0 && p.tick == 960, "no compensation leaves the position alone");
    p = alien::recordPosition(0, 960.0, 38, loopTicks);
    require(p.cycle == 0 && p.tick == 922, "compensation moves a note earlier");
    p = alien::recordPosition(2, 10.0, 38, loopTicks);
    require(p.cycle == 1 && p.tick == loopTicks - 28, "compensation across the loop start goes back one pass");
    p = alien::recordPosition(0, 10.0, 38, loopTicks);
    require(p.cycle == -1 && p.tick == loopTicks - 28, "before the first downbeat is pass -1, never a negative tick");
    p = alien::recordPosition(0, 960.0, -100, loopTicks);
    require(p.cycle == 0 && p.tick == 1060, "a negative offset moves a note later");
    p = alien::recordPosition(0, loopTicks - 50.0, -100, loopTicks);
    require(p.cycle == 1 && p.tick == 50, "moving later across the loop end goes forward one pass");
    p = alien::recordPosition(3, static_cast<double>(loopTicks), 0, loopTicks);
    require(p.cycle == 4 && p.tick == 0, "a rounded position at the loop end is the next pass");
    p = alien::recordPosition(1, std::numeric_limits<double>::quiet_NaN(), 0, loopTicks);
    require(p.cycle == 1 && p.tick == 0, "a non-finite position is treated as the loop start");
    p = alien::recordPosition(1, 5.0, 0, 0);
    require(p.cycle == 0 && p.tick == 0, "an empty loop gives a harmless position");
    require(closeTo(alien::ticksPerSample(120.0, 48000.0), 0.04, 1e-12) && alien::ticksPerSample(120.0, 0.0) == 0.0,
        "ticks per sample follow tempo and sample rate");
    std::cout << "PASS: record position and latency compensation\n";
}

void assemblerTests() {
    using Assembler = alien::RecordingAssembler;
    {
        Assembler a;
        a.consume(on(60, 0, 960, 0.8f)); a.consume(off(60, 0, 1440));
        require(a.notes().size() == 1 && a.notes()[0].pitch == 60 && a.notes()[0].startTick == 960
                && a.notes()[0].lengthTicks == 480 && closeTo(a.notes()[0].velocity, 0.8, 1e-6), "a played note becomes one note");
        require(!a.isFinished(), "a take is not finished until its end event");
        a.consume(endAt(0, 2000));
        require(a.isFinished(), "the end event finishes the take");
    }
    {
        Assembler a;
        a.consume(on(60, 0, 1000)); a.consume(off(60, 0, 1005));
        require(a.notes().size() == 1 && a.notes()[0].lengthTicks == Assembler::minimumLengthTicks, "a very short note is made long enough to hear");
    }
    {
        Assembler a; // pressing a key that is already down restarts it
        a.consume(on(60, 0, 100)); a.consume(on(60, 0, 300)); a.consume(off(60, 0, 500));
        require(a.notes().size() == 2 && a.notes()[0].startTick == 100 && a.notes()[0].lengthTicks == 200
                && a.notes()[1].startTick == 300 && a.notes()[1].lengthTicks == 200, "a retriggered key closes the earlier note");
    }
    {
        Assembler a;
        a.consume(off(60, 0, 100));
        a.consume(event(RecordEventKind::noteOn, 200, 0.5f, 0, 100)); // not a MIDI note
        a.consume(endAt(0, 200));
        require(a.notes().empty(), "a release without a press and an out-of-range pitch are ignored");
    }
    {
        Assembler a; // held when the take ends
        a.consume(on(64, 0, 15000)); a.consume(on(67, 0, 15100));
        a.consume(endAt(0, 15200));
        require(a.notes().size() == 2 && a.notes()[0].lengthTicks == 200 && a.notes()[1].lengthTicks == 100, "held notes are closed at the end of the take");
    }
    {
        Assembler a; // runs past the end of the loop: cut there
        a.consume(on(60, 0, 15000)); a.consume(off(60, 1, 100));
        require(a.notes().size() == 1 && a.notes()[0].startTick == 15000 && a.notes()[0].lengthTicks == loopTicks - 15000,
            "a note that runs past the loop end is cut at the loop end");
    }
    {
        Assembler a; // later passes land on the same loop position
        a.consume(on(60, 2, 500)); a.consume(off(60, 2, 700));
        require(a.notes()[0].startTick == 500 && a.notes()[0].lengthTicks == 200, "a note from a later pass is placed by its position in the loop");
    }
    {
        Assembler a; // compensation moved it before the downbeat
        a.consume(on(60, -1, loopTicks - 28)); a.consume(off(60, 0, 100));
        require(a.notes().size() == 1 && a.notes()[0].startTick == loopTicks - 28 && a.notes()[0].lengthTicks == 28,
            "a note moved before the downbeat sits at the end of the loop");
    }
    {
        Assembler a;
        a.consume(on(60, 0, 100, 5.0f)); a.consume(off(60, 0, 200));
        a.consume(on(61, 0, 300, 0.0001f)); a.consume(off(61, 0, 400));
        require(a.notes()[0].velocity == 1.0f && closeTo(a.notes()[1].velocity, 1.0 / 127.0, 1e-6), "velocity is kept in the range a note can have");
    }
    {
        Assembler a(16); // loop length decides where notes wrap
        require(a.loopLengthTicks() == 16 * PianoRollPattern::ticksPerStep, "the loop length follows the clip");
        a.consume(on(60, 0, 3800)); a.consume(off(60, 1, 20));
        require(a.notes()[0].lengthTicks == 16 * PianoRollPattern::ticksPerStep - 3800, "the cut uses this clip's loop length");
    }
    {
        Assembler a; // a full clip
        for (int i = 0; i < 1030; ++i) { a.consume(on(i % 100, 0, 10)); a.consume(off(i % 100, 0, 100)); }
        require(a.notes().size() == PianoRollPattern::maxNotes && a.noteLimitReached(), "at most 1024 notes are kept and the limit is reported");
    }
    {
        PianoRollPattern base;
        base.add(60, 0.0, 1.0); base.add(62, 2.0, 1.0);
        Assembler a;
        a.consume(on(64, 0, 960)); a.consume(off(64, 0, 1440));
        bool truncated = true;
        auto overdub = a.mergeInto(base, false, truncated);
        require(!truncated && overdub.count == 3 && overdub.valid() && overdub.find(base.notes[0].id) != nullptr,
            "overdub keeps the old notes and adds the new ones");
        require(closeTo(overdub.notes[2].startStep, 4.0, 1e-9) && closeTo(overdub.notes[2].lengthSteps, 2.0, 1e-9), "recorded notes land on the right steps");
        auto replaced = a.mergeInto(base, true, truncated);
        require(replaced.count == 1 && replaced.valid() && replaced.notes[0].pitch == 64 && replaced.loopSteps == base.loopSteps,
            "replace keeps only the new notes and the clip's loop length");
        PianoRollPattern full;
        for (int i = 0; i < 1024; ++i) full.add(60 + i % 12, i % 64, 0.5);
        require(full.count == 1024, "fixture: a full clip");
        auto limited = a.mergeInto(full, false, truncated);
        require(truncated && limited.count == 1024 && limited.valid(), "notes that do not fit are left out and the take says so");
        PianoRollPattern shortLoop;
        shortLoop.setLoopSteps(8);
        Assembler longer(128);
        longer.consume(on(60, 0, 100 * PianoRollPattern::ticksPerStep)); longer.consume(off(60, 0, 101 * PianoRollPattern::ticksPerStep));
        auto skipped = longer.mergeInto(shortLoop, false, truncated);
        require(skipped.count == 0 && skipped.valid(), "a note beyond a shorter clip's loop is skipped, never invalid");
    }
    std::cout << "PASS: take assembly, overdub and replace merging\n";
}

void metronomeClickTests() {
    alien::MetronomeClick click;
    click.prepare(48000.0);
    require(!click.ringing() && click.next() == 0.0f, "a click is silent until triggered");
    auto render = [&](bool accent, float volume) {
        click.trigger(accent, volume);
        std::vector<float> out;
        while (click.ringing()) out.push_back(click.next());
        return out;
    };
    const auto accent = render(true, 1.0f), beat = render(false, 1.0f);
    require(accent.size() == 1920 && beat.size() == 1920, "a click lasts 40 ms");
    auto peak = [](const std::vector<float>& v) { float m = 0; for (float x : v) m = std::max(m, std::abs(x)); return m; };
    require(peak(accent) > 0.5f && peak(accent) <= 0.6001f, "an accented click at full volume peaks near 0.6");
    require(closeTo(peak(beat) / peak(accent), 0.75, 0.03), "ordinary beats are quieter than the first beat of a bar");
    require(std::abs(accent.back()) < 0.001f, "the click dies away");
    auto crossings = [](const std::vector<float>& v) {
        int n = 0;
        for (std::size_t i = 1; i < v.size(); ++i) n += (v[i - 1] <= 0.0f && v[i] > 0.0f);
        return n;
    };
    require(crossings(accent) > crossings(beat) && crossings(accent) > 55 && crossings(accent) < 70, "the first beat is higher in pitch");
    require(click.next() == 0.0f, "nothing rings after the click ends");
    const auto quiet = render(true, 0.0f), junk = render(true, std::numeric_limits<float>::quiet_NaN());
    require(peak(quiet) == 0.0f && peak(junk) == 0.0f, "volume zero or not a number is silent");
    const auto half = render(true, 0.5f);
    require(closeTo(peak(half) / peak(accent), 0.5, 0.02), "volume scales the click");
    click.trigger(true, 1.0f); click.stop();
    require(!click.ringing() && click.next() == 0.0f, "a click can be stopped");
    click.prepare(0.0);
    click.trigger(false, 1.0f);
    require(click.ringing() && std::isfinite(click.next()), "an invalid sample rate falls back to a usable one");
    std::cout << "PASS: metronome click\n";
}

void menuTests() {
    namespace rm = alien::recordmenu;
    RecordSettings s;
    require(rm::apply(s, rm::countInBase + 0).countInBars == 0 && rm::apply(s, rm::countInBase + 2).countInBars == 2
            && rm::apply(s, rm::countInBase + 3).countInBars == s.countInBars, "count-in choices");
    require(rm::apply(s, rm::volumeBase + 2).metronomeVolume == rm::volumeLevels[2] && rm::apply(s, rm::volumeBase + 3).metronomeVolume == s.metronomeVolume,
        "metronome volume choices");
    require(rm::apply(s, rm::replaceId).replace && !rm::apply(rm::apply(s, rm::replaceId), rm::overdubId).replace, "overdub and replace choices");
    require(rm::apply(s, rm::latencyBase + 8).latencyOffsetMs == 100 && rm::apply(s, rm::latencyBase + 0).latencyOffsetMs == -50, "latency choices");
    auto nudged = rm::apply(s, rm::nudgeEarlierId);
    require(nudged.latencyOffsetMs == 5 && rm::apply(nudged, rm::nudgeLaterId).latencyOffsetMs == 0, "nudges move 5 ms");
    s.latencyOffsetMs = 98;
    require(rm::apply(s, rm::nudgeEarlierId).latencyOffsetMs == 100, "nudging stops at the limit");
    s.latencyOffsetMs = -98;
    require(rm::apply(s, rm::nudgeLaterId).latencyOffsetMs == -100, "nudging stops at the lower limit");
    require(rm::apply(s, 9999) == s && rm::apply(s, 0) == s && rm::apply(s, -5) == s, "unknown choices change nothing");
    for (int id : {rm::countInBase, rm::countInBase + 1, rm::volumeBase, rm::overdubId, rm::replaceId, rm::latencyBase + 4, rm::nudgeEarlierId})
        require(rm::apply(s, id).valid(), "every choice yields valid settings");
    require(rm::closestVolumeIndex(0.5f) == 1 && rm::closestVolumeIndex(0.0f) == 0 && rm::closestVolumeIndex(1.0f) == 2 && rm::closestVolumeIndex(0.7f) == 2,
        "a saved volume is shown as the nearest choice");
    std::cout << "PASS: record options menu choices\n";
}

// --- The engine ----------------------------------------------------------------

struct Rig {
    // Debug builds reserve separate stack slots for the many rigs in each test.
    // Keep the production-sized engine off Windows' default 1 MiB thread stack,
    // just as ControllerRig does below. Allocation still precedes callback checks.
    std::unique_ptr<ToneEngine> engineStorage{std::make_unique<ToneEngine>()};
    ToneEngine& engine{*engineStorage};
    std::array<float, 1024> left{}, right{};
    long long total = 0; // samples rendered
    Rig() { engine.prepare(48000); engine.patternMode.store(1); }
    double render(long long samples, int chunk = 256, std::vector<float>* keep = nullptr) {
        double energy = 0;
        while (samples > 0) {
            const int count = static_cast<int>(std::min<long long>(samples, chunk));
            left.fill(std::numeric_limits<float>::quiet_NaN());
            right.fill(std::numeric_limits<float>::quiet_NaN());
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            for (int i = 0; i < count; ++i) {
                const float x = left[static_cast<std::size_t>(i)];
                require(std::isfinite(x) && std::abs(x) <= 1.00001f && x == right[static_cast<std::size_t>(i)], "device callback writes finite matching samples");
                energy += static_cast<double>(x) * x;
                if (keep) keep->push_back(x);
            }
            samples -= count; total += count;
        }
        return energy;
    }
    int phase() const { return engine.recording.phase.load(); }
};
static_assert(sizeof(Rig) < 16 * 1024, "Keep MIDI recording fixtures small enough for the Windows test stack");

void queueClip(ToneEngine& e, int track, int scene, const PianoRollPattern& pattern = {}) {
    e.requestedPianoRollClip[static_cast<std::size_t>(track)].store(scene);
    alien::SessionClipLaunch launch;
    launch.pattern = pattern; launch.clipIndex = scene;
    e.pianoRollLaunches[static_cast<std::size_t>(track)].publish(launch);
}

void startTake(ToneEngine& e, int track, int scene, std::uint32_t take, const RecordSettings& settings, bool launch = true) {
    e.recording.apply(settings);
    e.recording.targetTrack.store(track);
    e.recording.targetScene.store(scene);
    e.recording.take.store(take);
    if (launch) queueClip(e, track, scene);
    e.recording.stopRequested.store(false);
    e.recording.startRequested.store(true);
}

std::vector<RecordEvent> drain(ToneEngine& e) {
    std::vector<RecordEvent> events;
    RecordEvent ev;
    while (e.recording.events.pop(ev)) events.push_back(ev);
    return events;
}

float peakOf(const std::vector<float>& v, std::size_t from, std::size_t count) {
    float m = 0;
    for (std::size_t i = from; i < std::min(v.size(), from + count); ++i) m = std::max(m, std::abs(v[i]));
    return m;
}

void countInTests() {
    // The transport starts on exactly the sample the count-in ends, wherever the host's blocks happen to fall.
    for (int chunk : {256, 193, 1000, 64}) {
        Rig rig;
        RecordSettings s; s.countInBars = 1;
        startTake(rig.engine, 0, 0, 1, s);
        rig.render(256, chunk);
        require(rig.phase() == RecordControls::countIn && !rig.engine.playing.load(), "recording from a stop begins with a count-in");
        rig.render(96000 - 256, chunk);
        require(rig.phase() == RecordControls::countIn && !rig.engine.playing.load() && rig.engine.pianoRollPosition.load() < 0,
            "the count-in lasts one whole bar at 120 bpm and playback has not started");
        rig.render(1, chunk);
        require(rig.phase() == RecordControls::recording && rig.engine.playing.load(), "the take starts the sample after the count-in");
        rig.render(255, chunk);
        const int position = rig.engine.pianoRollPosition.load();
        require(position >= 41 && position <= 43, "playback begins exactly where the count-in ended (not at a later block edge)");
        require(drain(rig.engine).empty(), "a count-in produces no recorded events");
    }
    {
        Rig rig; // two bars at another tempo
        rig.engine.bpm.store(90);
        RecordSettings s; s.countInBars = 2;
        startTake(rig.engine, 0, 0, 1, s);
        rig.render(256000, 300);
        require(rig.phase() == RecordControls::countIn && !rig.engine.playing.load(), "two bars at 90 bpm last 256000 samples");
        rig.render(1, 300);
        require(rig.phase() == RecordControls::recording && rig.engine.playing.load(), "two-bar count-in ends on time");
    }
    {
        Rig rig; // no count-in: start at once
        RecordSettings s; s.countInBars = 0;
        startTake(rig.engine, 0, 0, 1, s);
        rig.render(256);
        require(rig.phase() == RecordControls::recording && rig.engine.playing.load() && rig.engine.pianoRollPosition.load() >= 41,
            "with no count-in recording starts straight away");
    }
    {
        Rig rig; // already playing: no count-in, playback is not restarted
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(10000);
        const int before = rig.engine.pianoRollPosition.load();
        RecordSettings s; s.countInBars = 2;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(256);
        require(rig.phase() == RecordControls::recording && rig.engine.playing.load(), "recording while playing starts at once");
        require(rig.engine.pianoRollPosition.load() > before, "the loop that is already playing carries on without restarting");
    }
    {
        Rig rig; // a repeated start request during the count-in does not restart it
        RecordSettings s; s.countInBars = 1;
        startTake(rig.engine, 0, 0, 1, s);
        rig.render(40000);
        rig.engine.recording.startRequested.store(true);
        rig.render(96000 - 40000);
        require(rig.phase() == RecordControls::countIn && !rig.engine.playing.load(), "fixture: still counting in at the end of the bar");
        rig.render(1);
        require(rig.phase() == RecordControls::recording && rig.engine.playing.load(), "the count-in still ends on time after a repeated start request");
    }
    {
        Rig rig; // cancel during the count-in
        RecordSettings s; s.countInBars = 2;
        startTake(rig.engine, 0, 0, 7, s);
        rig.render(30000);
        require(rig.phase() == RecordControls::countIn, "fixture: counting in");
        rig.engine.recording.stopRequested.store(true);
        rig.render(256);
        require(rig.phase() == RecordControls::idle && !rig.engine.playing.load(), "stopping during the count-in cancels without starting playback");
        const auto events = drain(rig.engine);
        require(events.size() == 1 && events[0].kind == RecordEventKind::end && events[0].take == 7, "a cancelled count-in reports its end for that take");
        require(rig.render(200000) == 0 && rig.engine.pianoRollPosition.load() < 0, "nothing plays or clicks after a cancelled count-in");
    }
    std::cout << "PASS: count-in\n";
}

void clickTests() {
    {
        Rig rig; // the count-in always clicks, even with the metronome switched off
        RecordSettings s; s.countInBars = 1; s.metronome = false; s.metronomeVolume = 0.5f;
        startTake(rig.engine, 0, 0, 1, s);
        std::vector<float> out;
        rig.render(96000, 256, &out);
        float peaks[4];
        for (int k = 0; k < 4; ++k) {
            const auto start = static_cast<std::size_t>(24000 * k);
            peaks[k] = peakOf(out, start, 1920);
            require(peaks[k] > 0.1f, "a click on every beat of the count-in");
            require(std::abs(out[start + 12]) > 0.0f, "each click starts on its beat");
            if (k > 0) require(peakOf(out, start - 3, 3) == 0.0f, "silence right before each click");
            require(peakOf(out, start + 2000, 24000 - 2000) == 0.0f, "nothing but the click is heard");
        }
        require(peaks[0] > peaks[1] * 1.25f && closeTo(peaks[1], peaks[2], 1e-4) && closeTo(peaks[2], peaks[3], 1e-4), "the first beat of the bar is accented");
        require(rig.render(100000) == 0, "after the count-in there is no click when the metronome is off");
    }
    {
        Rig rig; // metronome while the transport plays
        RecordSettings s; s.countInBars = 1; s.metronome = true; s.metronomeVolume = 0.5f;
        rig.engine.recording.apply(s);
        rig.engine.playing.store(true);
        std::vector<float> out;
        rig.render(100000, 256, &out);
        const float accent0 = peakOf(out, 0, 1920), beat1 = peakOf(out, 24000, 1920), beat2 = peakOf(out, 48000, 1920),
                    accent4 = peakOf(out, 96000, 1920);
        require(accent0 > 0.1f && beat1 > 0.1f && beat2 > 0.1f, "the metronome clicks on each beat while playing");
        require(accent0 > beat1 * 1.25f && closeTo(accent0, accent4, 1e-4) && closeTo(beat1, beat2, 1e-4), "and accents the first beat of every bar");
        s.metronome = false;
        rig.engine.recording.apply(s);
        rig.render(2000);
        require(rig.render(60000) == 0, "switching the metronome off silences it");
    }
    {
        Rig rig; // not heard when the transport is stopped
        RecordSettings s; s.metronome = true;
        rig.engine.recording.apply(s);
        require(rig.render(60000) == 0, "the metronome is silent while stopped");
    }
    {
        Rig rig; // independent of the master volume, so it stays audible with the mix turned down
        rig.engine.masterControls.gain.store(0.0f);
        RecordSettings s; s.metronome = true;
        rig.engine.recording.apply(s);
        rig.engine.playing.store(true);
        rig.render(20000);
        std::vector<float> out;
        rig.render(30000, 256, &out);
        require(peakOf(out, 0, out.size()) > 0.1f, "the click does not depend on the master fader");
    }
    {
        Rig rig; // volume
        RecordSettings s; s.metronome = true; s.metronomeVolume = 0.25f;
        rig.engine.recording.apply(s);
        rig.engine.playing.store(true);
        std::vector<float> quiet, loud;
        rig.render(1920, 256, &quiet);
        rig.engine.playing.store(false); rig.render(48000);
        s.metronomeVolume = 0.8f;
        rig.engine.recording.apply(s);
        rig.engine.playing.store(true);
        rig.render(1920, 256, &loud);
        require(peakOf(loud, 0, loud.size()) > peakOf(quiet, 0, quiet.size()) * 2.5f, "metronome volume changes the click level");
    }
    std::cout << "PASS: metronome and count-in clicks\n";
}

void captureTests() {
    // A note pressed when the playhead is at step 4 is recorded at tick 960.
    auto runTake = [](int latencySamples, int offsetMs, int& onTick, int& offTick, std::vector<RecordEvent>& all) {
        Rig rig;
        rig.engine.deviceLatencySamples.store(latencySamples);
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s; s.latencyOffsetMs = offsetMs;
        startTake(rig.engine, 0, 0, 3, s, false);
        rig.render(23000);
        rig.engine.notes.push(60, 0.8f);
        rig.render(256);
        rig.render(36000 - rig.total);
        rig.engine.notes.push(60, 0.0f);
        rig.render(256);
        rig.engine.recording.stopRequested.store(true);
        rig.render(256);
        all = drain(rig.engine);
        onTick = offTick = -1;
        for (const auto& e : all) {
            if (e.kind == RecordEventKind::noteOn) onTick = e.tick;
            if (e.kind == RecordEventKind::noteOff) offTick = e.tick;
        }
        require(rig.phase() == RecordControls::idle, "the take is over after a stop request");
    };
    int onTick = 0, offTick = 0;
    std::vector<RecordEvent> events;
    runTake(0, 0, onTick, offTick, events);
    require(events.size() == 3 && events[0].kind == RecordEventKind::noteOn && events[0].pitch == 60 && events[1].kind == RecordEventKind::noteOff
            && events[2].kind == RecordEventKind::end, "a take reports note on, note off, then its end");
    require(onTick == 960 && offTick == 1440, "notes are stamped with where the loop was when they were played");
    require(closeTo(events[0].velocity, 0.8, 1e-6) && events[0].cycle == 0 && events[0].take == 3, "velocity, loop pass and take are reported");
    runTake(480, 10, onTick, offTick, events);
    require(onTick == 922 && offTick == 1402, "device latency plus the offset move recorded notes earlier (38 ticks for 20 ms at 120 bpm)");
    runTake(0, -10, onTick, offTick, events);
    require(onTick == 960 + 19, "a negative offset moves them later");

    // The assembler turns those into the note the player meant.
    {
        alien::RecordingAssembler a;
        runTake(0, 0, onTick, offTick, events);
        for (const auto& e : events) a.consume(e);
        require(a.isFinished() && a.notes().size() == 1 && a.notes()[0].startTick == 960 && a.notes()[0].lengthTicks == 480, "events assemble into the played note");
    }

    {
        Rig rig; // hardware MIDI as well as the computer keyboard, with velocity
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(23000);
        const auto device = rig.engine.midi.beginSession();
        rig.engine.midi.push(device, 0x90, 64, 100);
        rig.render(256);
        rig.engine.midi.push(device, 0x80, 64, 0);
        rig.render(256);
        rig.engine.midi.push(device, 0x95, 72, 50); // another channel
        rig.render(256);
        const auto events = drain(rig.engine);
        require(events.size() == 3 && events[0].pitch == 64 && closeTo(events[0].velocity, 100.0 / 127.0, 1e-3) && events[1].kind == RecordEventKind::noteOff
                && events[2].pitch == 72 && closeTo(events[2].velocity, 50.0 / 127.0, 1e-3), "hardware MIDI notes are recorded with their velocity on any channel");
    }
    {
        Rig rig; // notes are ignored unless the chosen clip is the one looping on its track
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s;
        startTake(rig.engine, 0, 1, 1, s, false); // asks for scene 1 while scene 0 plays
        rig.engine.notes.push(60, 0.8f); rig.render(256); rig.engine.notes.push(60, 0.0f); rig.render(256);
        require(drain(rig.engine).empty(), "notes played while the chosen clip is not the playing one are not recorded");
    }
    {
        Rig rig; // and nothing once a take is over, although its target is still set
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(256);
        rig.engine.recording.stopRequested.store(true);
        rig.render(256);
        drain(rig.engine);
        rig.engine.notes.push(60, 0.8f); rig.render(256); rig.engine.notes.push(60, 0.0f); rig.render(256);
        const auto midi = rig.engine.midi.beginSession();
        rig.engine.midi.push(midi, 0x90, 62, 100); rig.render(256); rig.engine.midi.push(midi, 0x80, 62, 0); rig.render(256);
        require(drain(rig.engine).empty(), "once a take is over, playing records nothing");
    }
    {
        Rig rig; // nothing is recorded outside a take
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        rig.engine.notes.push(60, 0.8f); rig.render(256); rig.engine.notes.push(60, 0.0f); rig.render(256);
        require(drain(rig.engine).empty(), "playing normally records nothing");
    }
    std::cout << "PASS: note capture and latency compensation\n";
}

void endOfTakeTests() {
    auto setup = [](Rig& rig, std::uint32_t take) {
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s;
        startTake(rig.engine, 0, 0, take, s, false);
        rig.render(256);
        require(rig.phase() == RecordControls::recording, "fixture: recording");
        rig.engine.notes.push(60, 0.8f); rig.render(256); // a note still held at the end
    };
    auto endsOnce = [](Rig& rig, std::uint32_t take, const char* message) {
        const auto events = drain(rig.engine);
        int ends = 0;
        for (const auto& e : events) ends += e.kind == RecordEventKind::end && e.take == take;
        require(ends == 1 && rig.phase() == RecordControls::idle, message);
    };
    { Rig rig; setup(rig, 4); rig.engine.recording.stopRequested.store(true); rig.render(256); endsOnce(rig, 4, "a stop request ends the take once"); }
    { Rig rig; setup(rig, 5); rig.engine.playing.store(false); rig.render(256); endsOnce(rig, 5, "stopping playback ends the take"); }
    { Rig rig; setup(rig, 6); rig.engine.patternMode.store(0); rig.render(256); endsOnce(rig, 6, "leaving the clip views ends the take"); }
    { Rig rig; setup(rig, 8); rig.engine.audioDeviceStopped(); endsOnce(rig, 8, "losing the audio device ends the take"); }
    {
        Rig rig; // a refused start says so instead of leaving the caller waiting
        rig.engine.patternMode.store(0);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 9, s);
        rig.render(256);
        const auto events = drain(rig.engine);
        require(events.size() == 1 && events[0].kind == RecordEventKind::refused && events[0].take == 9 && rig.phase() == RecordControls::idle
                && !rig.engine.playing.load(), "outside the clip views a take is refused and reported");
        rig.engine.patternMode.store(1);
        rig.engine.activeTracks.store(4);
        startTake(rig.engine, 15, 0, 10, s); // the track is not in use
        rig.render(256);
        const auto second = drain(rig.engine);
        require(second.size() == 1 && second[0].kind == RecordEventKind::refused && rig.phase() == RecordControls::idle, "a track that is not in use is refused");
        startTake(rig.engine, -1, 0, 11, s, false);
        rig.render(256);
        require(drain(rig.engine).size() == 1 && rig.phase() == RecordControls::idle, "no target is refused");
    }
    {
        Rig rig; // a second start request while recording does not disturb the take
        setup(rig, 12);
        drain(rig.engine);
        rig.engine.recording.targetTrack.store(1);
        rig.engine.recording.startRequested.store(true);
        rig.render(256);
        require(rig.phase() == RecordControls::recording && rig.engine.recording.targetTrack.load() == 1 && drain(rig.engine).empty(),
            "a start request while recording is ignored (no false end)");
    }
    {
        Rig rig; // start and stop in the same block: the take begins and ends
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 13, s, false);
        rig.engine.recording.stopRequested.store(true);
        rig.render(256);
        endsOnce(rig, 13, "pressing record then stop at once leaves nothing running");
    }
    {
        Rig rig; // a device re-prepare completes an active take before transport state is reset
        setup(rig, 14);
        rig.engine.prepare(44100);
        endsOnce(rig, 14, "preparing a restarted device ends the active take");
        require(rig.engine.recording.targetTrack.load() == -1 && rig.engine.recording.targetScene.load() == -1
                && !rig.engine.recording.startRequested.load() && !rig.engine.recording.stopRequested.load(),
            "device prepare clears the recording target and pending requests");
    }
    {
        Rig rig; // a device transition before the first callback refuses the pending take
        RecordSettings s;
        startTake(rig.engine, 0, 0, 15, s);
        rig.engine.prepare(48000);
        const auto events = drain(rig.engine);
        require(events.size() == 1 && events[0].kind == RecordEventKind::refused && events[0].take == 15
                && rig.phase() == RecordControls::idle && rig.engine.recording.targetTrack.load() == -1
                && rig.engine.recording.targetScene.load() == -1,
            "device prepare refuses a pending take instead of leaving stale state");
    }
    {
        Rig rig; // the audio thread independently rejects an out-of-range scene
        RecordSettings s;
        startTake(rig.engine, 0, alien::maxSessionScenes, 16, s, false);
        rig.render(256);
        const auto events = drain(rig.engine);
        require(events.size() == 1 && events[0].kind == RecordEventKind::refused && events[0].take == 16
                && rig.phase() == RecordControls::idle && rig.engine.recording.targetTrack.load() == -1,
            "an invalid target scene is refused before the callback uses recording state");
    }
    std::cout << "PASS: ending a take\n";
}

void monitoringTests() {
    auto prepare = [](Rig& rig) {
        queueClip(rig.engine, 0, 0);
        rig.engine.playing.store(true);
        rig.render(1000);
    };
    {
        Rig rig; // while recording, played notes go through the target track
        prepare(rig);
        alien::TrackSettings muted; muted.mute = true;
        rig.engine.tracks[0].set(muted);
        rig.render(2000);
        rig.engine.notes.push(60, 0.8f);
        require(rig.render(4000) > 0.001, "fixture: with no take the note is heard on the live synth even though track 1 is muted");
        rig.engine.notes.push(60, 0.0f);
        rig.render(30000);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(2048);
        rig.engine.notes.push(60, 0.8f);
        const double heard = rig.render(6000);
        require(heard < 1.0e-6, "while recording, notes sound through the armed track, so its mute applies");
        rig.engine.notes.push(60, 0.0f);
        rig.render(256);
        rig.engine.recording.stopRequested.store(true);
        rig.render(30000);
        rig.engine.notes.push(62, 0.8f);
        require(rig.render(4000) > 0.001, "after the take the live synth is back");
        rig.engine.notes.push(62, 0.0f);
        rig.render(30000);
        require(rig.render(2048) == 0, "no note is left sounding after monitoring ends");
    }
    {
        Rig rig; // unmuted: heard through the track, and released cleanly when the take ends with a key still down
        prepare(rig);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(1024);
        rig.engine.notes.push(60, 0.8f);
        require(rig.render(6000) > 0.001, "the armed track's own sound is heard while recording");
        rig.engine.recording.stopRequested.store(true); // key still held
        rig.render(256);
        rig.render(24000);
        require(rig.render(4096) == 0, "a key held when the take ends does not stick");
        drain(rig.engine);
    }
    {
        Rig rig; // hardware MIDI is monitored the same way, on the target track
        prepare(rig);
        alien::TrackSettings muted; muted.mute = true;
        rig.engine.tracks[0].set(muted);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(2048);
        const auto device = rig.engine.midi.beginSession();
        rig.engine.midi.push(device, 0x90, 60, 110);
        require(rig.render(6000) < 1.0e-6, "hardware MIDI also sounds through the armed track while recording");
    }
    {
        Rig rig; // a drum-sampler track without samples stays silent and safe
        prepare(rig);
        alien::TrackSettings drums; drums.instrument = 1;
        rig.engine.tracks[0].set(drums);
        rig.render(2048);
        RecordSettings s;
        startTake(rig.engine, 0, 0, 1, s, false);
        rig.render(2048);
        rig.engine.notes.push(36, 0.9f);
        rig.render(4000);
        rig.engine.recording.stopRequested.store(true);
        require(rig.render(4000) >= 0.0, "monitoring a drum track is safe");
    }
    std::cout << "PASS: monitoring through the armed track\n";
}

void realTimeSafetyTests() {
    Rig rig;
    queueClip(rig.engine, 0, 0);
    rig.engine.playing.store(true);
    rig.render(1000);
    RecordSettings s; s.metronome = true; s.countInBars = 1;
    startTake(rig.engine, 0, 0, 1, s, false);
    rig.render(512);
    std::array<float, 256> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    const auto before = allocationCount;
    const auto device = rig.engine.midi.beginSession();
    for (int i = 0; i < 400; ++i) {
        if (i % 7 == 0) rig.engine.notes.push(60 + i % 12, 0.8f);
        if (i % 7 == 3) rig.engine.notes.push(60 + (i - 3) % 12, 0.0f);
        if (i % 11 == 0) rig.engine.midi.push(device, 0x90, 50 + i % 20, 90);
        if (i % 11 == 5) rig.engine.midi.push(device, 0x80, 50 + (i - 5) % 20, 0);
        rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 256, {});
    }
    rig.engine.recording.stopRequested.store(true);
    rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 256, {});
    startTake(rig.engine, 0, 0, 2, s, false);
    rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 256, {});
    rig.engine.audioDeviceStopped();
    require(allocationCount == before, "recording, the count-in and the metronome never allocate on the audio thread");
    // A very busy player cannot overrun the ring unnoticed.
    drain(rig.engine);
    const auto droppedBefore = rig.engine.recording.events.droppedCount();
    std::size_t pushed = 0;
    while (rig.engine.recording.events.droppedCount() == droppedBefore && pushed < 20000) {
        rig.engine.recording.events.push(on(60, 0, 0));
        ++pushed;
    }
    require(rig.engine.recording.events.droppedCount() == droppedBefore + 1 && pushed == alien::RecordEventRing::capacity, "an overrun is counted");
    std::cout << "PASS: recording is real-time safe\n";
}

// --- The controller --------------------------------------------------------------

struct ControllerRig {
    // ProjectController owns several complete project snapshots. Keep the two
    // production-sized fixtures off the small default Windows test stack.
    std::unique_ptr<ToneEngine> engineStorage{std::make_unique<ToneEngine>()};
    ToneEngine& engine{*engineStorage};
    std::unique_ptr<alien::ProjectController> controllerStorage{std::make_unique<alien::ProjectController>(engine)};
    alien::ProjectController& controller{*controllerStorage};
    std::vector<alien::ProjectController::RecordingOutcome> outcomes;
    long long total = 0;
    ControllerRig() {
        engine.prepare(48000);
        controller.onRecordingFinished = [this](const alien::ProjectController::RecordingOutcome& o) { outcomes.push_back(o); };
        controller.pushDocumentToEngine();
        engine.patternMode.store(1);
    }
    double render(long long samples, int chunk = 256, std::vector<float>* keep = nullptr) {
        double energy = 0;
        std::array<float, 1024> left{}, right{};
        while (samples > 0) {
            const int count = static_cast<int>(std::min<long long>(samples, chunk));
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            for (int i = 0; i < count; ++i) {
                energy += static_cast<double>(left[static_cast<std::size_t>(i)]) * left[static_cast<std::size_t>(i)];
                if (keep) keep->push_back(left[static_cast<std::size_t>(i)]);
            }
            samples -= count; total += count;
            controller.pollRecording();
        }
        return energy;
    }
    // Plays the clip on track 0 and gets it going.
    void playClip(int scene = 0) {
        controller.launchClip(0, scene);
        engine.playing.store(true);
        render(1000);
    }
    void tap(int note, long long holdSamples) {
        engine.notes.push(note, 0.8f);
        render(256);
        render(holdSamples - 256);
        engine.notes.push(note, 0.0f);
        render(256);
    }
    const PianoRollNote* find(int pitch, int track = 0, int scene = 0) const {
        const auto& p = controller.document().clip(track, scene).notes();
        for (std::size_t i = 0; i < p.count; ++i) if (p.notes[i].pitch == pitch) return &p.notes[i];
        return nullptr;
    }
    int count(int track = 0, int scene = 0) const { return static_cast<int>(controller.document().clip(track, scene).notes().count); }
};

PianoRollPattern twoNotes() {
    PianoRollPattern p;
    p.add(60, 0.0, 1.0);
    p.add(67, 8.0, 1.0);
    return p;
}

void overdubTests() {
    ControllerRig rig;
    auto& c = rig.controller;
    c.setClipPattern(0, 0, twoNotes(), false);
    c.commit("Add notes");
    const auto labelBefore = c.undoLabel();
    rig.playClip();
    const auto start = c.startRecording();
    require(start.started && start.reason.isEmpty() && c.isRecording() && c.recordingTrack() == 0 && c.recordingScene() == 0, "recording starts on the selected clip");
    require(!c.startRecording().started, "a second start is refused while recording");
    rig.render(23000);
    rig.tap(64, 11744); // pressed when the playhead is at step 4, held 2 steps... (total 36000 at release)
    // Still recording: the note just played is already part of the loop on its next pass.
    rig.render(384000 + 25000 - rig.total);
    require(c.isRecording() && rig.render(2000) > 0.0005, "the loop plays back what has been recorded so far while the take continues");
    c.stopRecording();
    rig.render(512);
    require(!c.isRecording() && rig.outcomes.size() == 1, "the take finishes and reports once");
    const auto& outcome = rig.outcomes[0];
    require(outcome.notesAdded == 1 && !outcome.replaced && !outcome.refused && !outcome.discarded && !outcome.noteLimitReached
            && outcome.droppedEvents == 0 && outcome.track == 0 && outcome.scene == 0, "the outcome says one note was added");
    require(rig.count() == 3 && rig.find(60) != nullptr && rig.find(67) != nullptr, "overdub keeps the notes that were already there");
    const auto* played = rig.find(64);
    require(played != nullptr && closeTo(played->startStep, 4.0, 0.005) && played->lengthSteps > 1.5 && played->lengthSteps < 2.6
            && closeTo(played->velocity, 0.8, 0.01), "the recorded note is where and how it was played");
    require(c.canUndo() && c.undoLabel() == "Record MIDI" && c.undoLabel() != labelBefore, "the take is one undo step");
    require(c.undo() && rig.count() == 2 && rig.find(64) == nullptr, "undo removes the whole take at once");
    require(c.undoLabel() == labelBefore, "and nothing else");
    require(c.redo() && rig.count() == 3 && rig.find(64) != nullptr, "redo brings it back");
    // And the finished clip keeps playing it.
    rig.render(2 * 384000 + 25000 - rig.total);
    require(rig.render(2000) > 0.0005, "the recorded note keeps playing on later passes");
    std::cout << "PASS: overdub\n";
}

void replaceTests() {
    // A long first note, so there is plenty of old sound to silence at the start of the take.
    PianoRollPattern oldNotes;
    oldNotes.add(60, 0.0, 8.0);
    oldNotes.add(67, 8.0, 1.0);
    ControllerRig overdub; // reference: an overdub take keeps hearing the old notes
    overdub.controller.setClipPattern(0, 0, oldNotes, false);
    overdub.controller.commit("Add notes");
    overdub.playClip();
    require(overdub.controller.startRecording().started, "fixture: an overdub take");
    overdub.render(12000);
    const double kept = overdub.render(6000);
    require(kept > 0.01, "fixture: the old note is heard during an overdub");

    for (const bool recordNotes : {true, false}) {
        ControllerRig rig;
        auto& c = rig.controller;
        c.setClipPattern(0, 0, oldNotes, false);
        c.commit("Add notes");
        RecordSettings s = c.document().recording;
        s.replace = true;
        c.setRecordSettings(s);
        const auto label = c.undoLabel();
        rig.playClip();
        require(c.startRecording().started, "a replace take starts");
        rig.render(12000);
        const double silenced = rig.render(6000);
        require(silenced < kept * 0.001, "replace plays the clip without its old notes, so only the new take is heard");
        if (recordNotes) {
            rig.tap(72, 6000);
            c.stopRecording();
            rig.render(512);
            require(rig.outcomes.size() == 1 && rig.outcomes[0].replaced && rig.outcomes[0].notesAdded == 1, "a replace take reports the one new note");
            require(rig.count() == 1 && rig.find(72) != nullptr && rig.find(60) == nullptr && rig.find(67) == nullptr, "the old notes are replaced");
            require(c.undoLabel() == "Record MIDI" && c.undo() && rig.count() == 2 && rig.find(60) != nullptr && rig.find(67) != nullptr,
                "undo brings the old notes back");
        } else {
            c.stopRecording();
            rig.render(512);
            require(rig.outcomes.size() == 1 && rig.outcomes[0].notesAdded == 0 && !rig.outcomes[0].refused, "a take with no notes reports that nothing was recorded");
            require(rig.count() == 2 && c.undoLabel() == label, "playing nothing keeps the old notes and adds no undo step");
            rig.render(384000 + 100 - rig.total);
            require(rig.render(2000) > 0.01, "the clip's old notes sound again on the next pass");
        }
    }
    std::cout << "PASS: replace\n";
}

void countInIntegrationTests() {
    ControllerRig rig;
    auto& c = rig.controller;
    require(c.document().recording.countInBars == 1, "the default is a one-bar count-in");
    require(c.startRecording().started, "record from a stop");
    rig.render(96000 + 24000, 193);
    require(rig.engine.playing.load() && rig.engine.recording.phase.load() == RecordControls::recording, "the count-in has ended and the take is running");
    rig.tap(65, 12000);
    c.stopRecording();
    rig.render(512);
    const auto* note = rig.find(65);
    require(rig.outcomes.size() == 1 && rig.count() == 1 && note != nullptr && closeTo(note->startStep, 4.0, 0.005),
        "a note played one beat after the count-in lands one beat into the clip");
    require(rig.engine.playing.load(), "the transport keeps running after a take, like other recorders");

    // Latency compensation reaches the recorded position.
    ControllerRig compensated;
    compensated.engine.deviceLatencySamples.store(960); // 20 ms
    auto s = compensated.controller.document().recording;
    s.countInBars = 0;
    compensated.controller.setRecordSettings(s);
    require(compensated.controller.startRecording().started, "record with no count-in");
    compensated.render(24000);
    compensated.tap(65, 6000);
    compensated.controller.stopRecording();
    compensated.render(512);
    const auto* shifted = compensated.find(65);
    require(shifted != nullptr && closeTo(shifted->startStep, (960.0 - 38.0) / 240.0, 0.01), "the device's output latency is compensated automatically");
    std::cout << "PASS: count-in through the controller\n";
}

void refusalAndGuardTests() {
    {
        ControllerRig rig;
        auto& c = rig.controller;
        rig.engine.patternMode.store(0);
        auto r = c.startRecording();
        require(!r.started && r.reason.contains("Piano Roll") && !c.isRecording(), "recording needs a clip view");
        rig.engine.patternMode.store(1);
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = 8000; clip->channels = 1; clip->frames = 80; clip->left.assign(80, 0.1f); clip->buildWaveform();
        require(c.installAudio(juce::File("/tmp/not-there.wav"), 1, 2, clip), "fixture: an audio clip");
        c.selectClip(1, 2);
        r = c.startRecording();
        require(!r.started && r.reason.contains("audio") && !c.isRecording() && !rig.engine.recording.active(), "a clip that holds audio cannot be recorded into");
        c.selectClip(0, 0);
        require(c.startRecording().started, "a MIDI clip can");
    }
    {
        ControllerRig rig; // undo, redo and edits to the clip wait until the take ends
        auto& c = rig.controller;
        c.setClipPattern(0, 0, twoNotes(), false);
        c.commit("Add notes");
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(512);
        PianoRollPattern other;
        other.add(40, 0.0, 1.0);
        c.setClipPattern(0, 0, other, true);
        require(rig.count() == 2 && rig.find(60) != nullptr, "the clip being recorded cannot be edited under the take");
        c.setClipPattern(0, 1, other, false);
        require(rig.count(0, 1) == 1, "other clips can");
        require(!c.undo() && !c.redo() && c.isRecording(), "undo and redo wait until the take ends");
        c.stopRecording();
        rig.render(512);
        require(!c.isRecording() && c.undo(), "and work again afterwards");
    }
    {
        ControllerRig rig; // opening or starting another project drops the take
        auto& c = rig.controller;
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(2000);
        rig.tap(60, 3000);
        c.newProject();
        require(!c.isRecording() && rig.outcomes.empty(), "a new project abandons the take without keeping it");
        rig.render(2000);
        require(!rig.engine.recording.active() && rig.count() == 0, "the engine is idle and the new project is untouched");
        require(c.startRecording().started, "recording works again in the new project");
    }
    {
        // Opening a saved project replaces the whole document, so it drops the take too.
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioRecordOpen", "", false);
        require(root.createDirectory().wasOk(), "temp directory");
        const auto song = root.getChildFile("Song.alienstudio");
        {
            ControllerRig saver;
            saver.controller.setBpm(133);
            saver.controller.commit("Tempo");
            bool done = false, ok = false;
            saver.controller.writeProject(song, false, [&](bool r) { ok = r; done = true; });
            for (int i = 0; i < 400 && !done; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            require(done && ok, "fixture: a saved project");
        }
        ControllerRig rig;
        auto& c = rig.controller;
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(2000);
        c.openProject(song);
        for (int i = 0; i < 400 && c.isBusy(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(!c.isBusy() && c.document().bpm == 133, "fixture: the project opened");
        require(!c.isRecording() && rig.outcomes.empty(), "opening a project abandons the take without keeping it");
        rig.render(2000);
        require(!rig.engine.recording.active(), "and the engine is idle again");
        root.deleteRecursively();
    }
    {
        ControllerRig rig; // deleting a track renumbers everything, so the take is dropped
        auto& c = rig.controller;
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(2000);
        require(c.deleteTrack(2) && !c.isRecording() && rig.outcomes.empty(), "removing a track abandons the take");
        rig.render(2000);
        require(!rig.engine.recording.active(), "and the engine stops recording");
    }
    {
        ControllerRig rig; // cancelling before the engine has heard the request
        auto& c = rig.controller;
        require(c.startRecording().started, "start");
        c.stopRecording();
        require(!c.isRecording() && rig.outcomes.size() == 1 && rig.outcomes[0].notesAdded == 0 && !rig.outcomes[0].refused,
            "pressing record twice quickly cancels cleanly");
        rig.render(2000);
        require(!rig.engine.recording.active() && !rig.engine.playing.load(), "and nothing was started");
    }
    {
        ControllerRig rig; // the engine could not start it
        auto& c = rig.controller;
        require(c.startRecording().started, "start");
        rig.engine.patternMode.store(0); // the view changed before the engine looked
        rig.render(512);
        require(!c.isRecording() && rig.outcomes.size() == 1 && rig.outcomes[0].refused, "a refused take is reported as refused");
    }
    {
        ControllerRig rig; // the view changes during a take: what was played is kept
        auto& c = rig.controller;
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(23000);
        rig.tap(64, 6000);
        rig.engine.patternMode.store(0);
        rig.render(512);
        require(!c.isRecording() && rig.outcomes.size() == 1 && rig.outcomes[0].notesAdded == 1 && rig.count() == 1,
            "leaving the view ends the take and keeps the notes played so far");
    }
    {
        ControllerRig rig; // a clip that fills up
        auto& c = rig.controller;
        PianoRollPattern full;
        for (int i = 0; i < 1024; ++i) full.add(60 + i % 12, i % 64, 0.5);
        c.setClipPattern(0, 0, full, false);
        c.commit("Fill");
        const auto label = c.undoLabel();
        rig.playClip();
        require(c.startRecording().started, "start");
        rig.render(23000);
        rig.tap(64, 3000);
        c.stopRecording();
        rig.render(512);
        require(rig.outcomes.size() == 1 && rig.outcomes[0].notesAdded == 0 && rig.outcomes[0].noteLimitReached && rig.count() == 1024 && c.undoLabel() == label,
            "a full clip is left alone and the take says why");
    }
    std::cout << "PASS: refusals, guards and abandoned takes\n";
}

void launchTests() {
    // Recording into a clip that is not the one playing waits for it to launch, and only notes played after that count.
    ControllerRig rig;
    auto& c = rig.controller;
    rig.playClip(0);
    c.selectClip(0, 1);
    require(c.startRecording().started, "record into a clip that is not playing");
    rig.engine.notes.push(60, 0.8f); rig.render(256); rig.engine.notes.push(60, 0.0f);
    rig.render(2000);
    rig.render(96000 - rig.total + 1); // the launch lands on the next bar line
    require(rig.engine.activePianoRollClip[0].load() == 1, "the chosen clip launches at the next bar");
    rig.render(12000 - 1);
    rig.tap(67, 4000);
    c.stopRecording();
    rig.render(512);
    require(rig.outcomes.size() == 1 && rig.outcomes[0].notesAdded == 1 && rig.count(0, 1) == 1 && rig.count(0, 0) == 0,
        "only the note played after the clip launched is recorded, into that clip");
    require(closeTo(rig.find(67, 0, 1)->startStep, 2.0, 0.02), "and it is placed relative to the clip's own start");
    std::cout << "PASS: recording into a clip that is not playing\n";
}

void settingsPersistenceTests() {
    ControllerRig rig;
    auto& c = rig.controller;
    auto s = c.document().recording;
    s.metronome = true; s.countInBars = 2; s.metronomeVolume = 0.8f; s.latencyOffsetMs = 15; s.replace = true;
    c.setRecordSettings(s);
    require(c.document().recording == s && rig.engine.recording.metronome.load() && rig.engine.recording.countInBars.load() == 2
            && rig.engine.recording.metronomeVolume.load() == 0.8f && rig.engine.recording.latencyOffsetMs.load() == 15, "settings reach the engine");
    require(c.undoLabel() == "Change recording settings" && c.isDirty(), "changing them is an undoable edit that marks the project changed");
    auto invalid = s; invalid.countInBars = 9;
    c.setRecordSettings(invalid);
    require(c.document().recording == s, "invalid settings are refused");
    const auto label = c.undoLabel();
    c.setRecordSettings(s);
    require(c.undoLabel() == label, "setting the same values again adds nothing");
    const auto json = alien::serializeSessionProject(alien::toProjectData(c.document()));
    alien::SessionProjectData parsed;
    juce::String error;
    require(alien::parseSessionProject(json, parsed, error) && alien::fromProjectData(parsed).recording == s,
        "settings survive saving and opening");
    require(c.undo() && !rig.engine.recording.metronome.load() && rig.engine.recording.countInBars.load() == 1, "undo puts the engine's settings back");
    c.newProject();
    require(c.document().recording == RecordSettings{} && rig.engine.recording.countInBars.load() == 1, "a new project starts with the defaults");
    std::cout << "PASS: recording settings in the project\n";
}

} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    alien::ProjectController::recoveryDirectoryOverride() =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioRecordingTests", "", false);
    settingsTests();
    ringTests();
    positionTests();
    assemblerTests();
    metronomeClickTests();
    menuTests();
    countInTests();
    clickTests();
    captureTests();
    endOfTakeTests();
    monitoringTests();
    realTimeSafetyTests();
    overdubTests();
    replaceTests();
    countInIntegrationTests();
    refusalAndGuardTests();
    launchTests();
    settingsPersistenceTests();
    std::cout << "MIDI recording tests passed\n";
    return 0;
}
