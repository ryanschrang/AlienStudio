// A plugin that crashes, throws or produces garbage must cost the user one plugin, never the session.
// The test plugins below really dereference null, really throw and really emit NaN: on Windows
// those are structured exceptions, on Linux and macOS signals, and the host has to survive them.
#include "PluginHost.h"
#include "PluginFault.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

using namespace alien;

// Counts allocations on the calling thread, so the audio path can be shown not to allocate even when a plugin fails.
static thread_local std::size_t allocations{};
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);} void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

static int failures = 0;
static void check(bool ok, const char* message) { if (!ok) { std::cerr << "FAILED: " << message << '\n'; ++failures; } }
static volatile std::uintptr_t zeroAddress = 0;
static void crash() { *reinterpret_cast<volatile int*>(zeroAddress) = 1; }

struct Counters { std::atomic<int> processCalls{0}, destroyed{0}, prepared{0}; };
enum class Fail { none, nullWrite, throwException, nan, infinity };
struct Plan {
    Fail inProcess{Fail::none};
    int afterBlocks{0};                     // behave for this many blocks first
    bool crashPrepare{}, crashRelease{}, crashGetState{}, crashSetState{}, crashTail{}, crashEditor{}, crashLatencyChange{};
    float gain{1};
    int latency{0};
};

class Probe final : public juce::AudioProcessor {
public:
    Probe(std::shared_ptr<Plan> p, std::shared_ptr<Counters> c, bool isInstrument)
        : juce::AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo(), !isInstrument).withOutput("Out", juce::AudioChannelSet::stereo(), true)),
          plan(std::move(p)), counters(std::move(c)), instrument(isInstrument) { setLatencySamples(plan->latency); }
    ~Probe() override { ++counters->destroyed; }
    void prepareToPlay(double, int) override { ++counters->prepared; if (plan->crashPrepare) crash(); }
    void releaseResources() override { if (plan->crashRelease) crash(); }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        ++counters->processCalls;
        if (blocks++ >= plan->afterBlocks) {
            switch (plan->inProcess) {
                case Fail::nullWrite: b.setSample(0, 0, 0.123f); crash(); break;              // dirty the buffer first, then die
                case Fail::throwException: b.setSample(0, 0, 0.123f); throw std::runtime_error("plugin bug");
                case Fail::nan: for (int c = 0; c < b.getNumChannels(); ++c) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(c, i, std::nanf("")); return;
                case Fail::infinity: b.setSample(1, b.getNumSamples() / 2, INFINITY); return;
                case Fail::none: break;
            }
        }
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(c, i, instrument ? 0.25f * plan->gain : b.getSample(c, i) * plan->gain);
    }
    const juce::String getName() const override { return "Probe"; }
    bool acceptsMidi() const override { return instrument; } bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { if (plan->crashTail) crash(); return 0.5; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; } void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { if (plan->crashEditor) crash(); return nullptr; }
    void getStateInformation(juce::MemoryBlock& d) override { if (plan->crashGetState) crash(); const char text[] = "live"; d.append(text, 4); }
    void setStateInformation(const void*, int) override { if (plan->crashSetState) crash(); }
    std::shared_ptr<Plan> plan; std::shared_ptr<Counters> counters; bool instrument; int blocks{0};
};

// JUCE's own encoding of saved plugin state (it is not standard base64).
static juce::String encoded(const char* text) { return juce::MemoryBlock(text, std::strlen(text)).toBase64Encoding(); }
static PluginState stateFor(const char* id, bool instrument, const juce::String& savedState = {}) {
    juce::PluginDescription d; d.name = id; d.pluginFormatName = "VST3"; d.fileOrIdentifier = id; d.isInstrument = instrument;
    return {d.createXml()->toString(), savedState, false};
}

// Gives a slot a Probe directly, as the existing runtime tests do.
static PluginRuntime::Slot& place(PluginRuntime& rt, int rack, int index, std::shared_ptr<Plan> plan, std::shared_ptr<Counters> counters, bool instrument = false) {
    auto& r = rt.racks[static_cast<std::size_t>(rack)];
    auto& slot = index < 0 ? r.instrument : r.effects[static_cast<std::size_t>(index)];
    slot.processor = std::make_unique<Probe>(std::move(plan), std::move(counters), instrument);
    slot.name = "Probe";
    if (index < 0) r.instrumentAssigned = true; else r.effectCount = std::max(r.effectCount, index + 1);
    rt.prepareSlot(slot, rack, index);
    return slot;
}
static void fill(PluginRuntime& rt, std::size_t track, float v, int n) { for (int i = 0; i < n; ++i) { rt.tracks[track].setSample(0, i, v); rt.tracks[track].setSample(1, i, v); } }
static bool finite(const juce::AudioBuffer<float>& b, int n) { for (int c = 0; c < b.getNumChannels(); ++c) for (int i = 0; i < n; ++i) if (!std::isfinite(b.getSample(c, i))) return false; return true; }
static std::shared_ptr<Plan> plan(Fail f = Fail::none, float gain = 1, int after = 0) { auto p = std::make_shared<Plan>(); p->inProcess = f; p->gain = gain; p->afterBlocks = after; return p; }

static void queueTests() {
    FaultQueue q; PluginFault out;
    check(!q.pop(out), "an empty queue yields nothing");
    for (int i = 0; i < 100; ++i) { PluginFault f; f.samplePosition = i; q.push(f); }
    int popped = 0; std::int64_t last = -1; bool ordered = true;
    while (q.pop(out)) { ++popped; ordered = ordered && out.samplePosition > last; last = out.samplePosition; }
    check(popped == static_cast<int>(FaultQueue::capacity) && q.droppedCount() == 100 - FaultQueue::capacity && ordered, "a full queue keeps the oldest reports in order and counts the rest as dropped");
    { PluginFault f; f.samplePosition = 7; check(q.push(f) && q.pop(out) && out.samplePosition == 7, "it is usable again once drained"); }
    // Several producers and a consumer at once: nothing is torn, nothing is reordered within one producer.
    FaultQueue shared; std::atomic<bool> done{false}; std::atomic<int> consumed{0}; std::atomic<bool> good{true};
    std::thread consumer([&] {
        std::int64_t next[4] = {0, 0, 0, 0}; PluginFault f;
        auto handle = [&](const PluginFault& r) {
            if (r.rack < 0 || r.rack > 3 || r.samplePosition < next[r.rack] || r.slot != r.rack * 10) good.store(false);
            else next[r.rack] = r.samplePosition + 1;
            consumed.fetch_add(1);
        };
        for (;;) {
            if (shared.pop(f)) { handle(f); continue; }
            if (!done.load()) continue;
            if (shared.pop(f)) { handle(f); continue; }   // the last reports may have landed just before `done`
            break;
        }
    });
    std::vector<std::thread> producers; std::atomic<int> accepted{0};
    for (int t = 0; t < 4; ++t) producers.emplace_back([&, t] { for (int i = 0; i < 5000; ++i) { PluginFault f; f.rack = t; f.slot = t * 10; f.samplePosition = i; if (shared.push(f)) accepted.fetch_add(1); } });
    for (auto& p : producers) p.join();
    done.store(true); consumer.join();
    check(good.load() && consumed.load() == accepted.load() && accepted.load() + static_cast<int>(shared.droppedCount()) == 20000, "concurrent producers: every accepted report arrives intact, the rest are counted");
}

static void runtimeTests() {
    // 1. An effect that crashes mid-block: this block passes the input on, the plugin is switched off, nothing is called again.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>();
        place(*rt, 0, 0, plan(Fail::nullWrite), c);
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.5f, 64);
        const auto before = allocations;
        rt->effectsAndAlign(64);
        check(allocations == before, "containing a crash does not allocate on the audio thread");
        check(rt->tracks[0].getSample(0, 0) == 0.5f && rt->tracks[0].getSample(1, 63) == 0.5f, "a crashed effect hands back the signal it was given, not its half-written output");
        check(rt->racks[0].effects[0].faulted.get(), "the slot is marked failed");
        PluginFault f; check(rt->faults.pop(f), "a report is queued");
        check(f.rack == 0 && f.slot == 0 && f.phase == PluginPhase::process && f.fault.kind == fault::Kind::accessViolation && !f.nonFiniteOutput, "the report says where, when and how");
        check(describe(f).find("access violation") != std::string::npos, "and reads as a sentence");
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.25f, 64); rt->effectsAndAlign(64);
        check(c->processCalls.load() == 1 && rt->tracks[0].getSample(0, 10) == 0.25f && !rt->faults.pop(f), "a failed plugin is never called again and reports only once");
        check(rt->faultCount.load() == 1, "the fault counter saw exactly one failure");
    }
    // 2. A thrown C++ exception is contained the same way.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>();
        place(*rt, 1, 0, plan(Fail::throwException), c);
        rt->begin(32, 120, true, 0); fill(*rt, 1, 0.5f, 32); rt->effectsAndAlign(32);
        PluginFault f; check(rt->faults.pop(f) && f.fault.kind == fault::Kind::cppException && rt->tracks[1].getSample(0, 5) == 0.5f, "an exception thrown by a plugin is contained");
    }
    // 3. NaN and infinity never reach the mix.
    for (Fail kind : {Fail::nan, Fail::infinity}) {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>();
        place(*rt, 0, 0, plan(kind), c);
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.5f, 64); rt->effectsAndAlign(64);
        PluginFault f; check(rt->faults.pop(f) && f.nonFiniteOutput && finite(rt->tracks[0], 64) && rt->tracks[0].getSample(0, 40) == 0.5f, "garbage output is replaced by the dry signal and reported");
        check(rt->racks[0].effects[0].faulted.get(), "and the plugin that made it is switched off");
    }
    // 4. In a chain only the broken plugin is skipped: 0.25 * 2 -> (crash) -> * 0.5 = 0.25.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto a = std::make_shared<Counters>(), b = std::make_shared<Counters>(), d = std::make_shared<Counters>();
        place(*rt, 0, 0, plan(Fail::none, 2), a); place(*rt, 0, 1, plan(Fail::nullWrite), b); place(*rt, 0, 2, plan(Fail::none, 0.5f), d);
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.25f, 64); rt->effectsAndAlign(64);
        check(rt->tracks[0].getSample(0, 30) == 0.25f, "the plugins before and after a failed one still process");
        check(!rt->racks[0].effects[0].faulted.get() && rt->racks[0].effects[1].faulted.get() && !rt->racks[0].effects[2].faulted.get(), "only the broken one is marked");
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.25f, 64); rt->effectsAndAlign(64);
        check(rt->tracks[0].getSample(0, 30) == 0.25f && a->processCalls.load() == 2 && d->processCalls.load() == 2 && b->processCalls.load() == 1, "the chain keeps working block after block");
    }
    // 5. An instrument that crashes goes silent; its neighbour is untouched.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto a = std::make_shared<Counters>(), b = std::make_shared<Counters>();
        place(*rt, 0, -1, plan(Fail::nullWrite), a, true); place(*rt, 1, -1, plan(), b, true);
        rt->begin(64, 120, true, 0); rt->instruments(64);
        { bool silent = true; for (int c = 0; c < 2; ++c) for (int i = 0; i < 64; ++i) silent = silent && rt->tracks[0].getSample(c, i) == 0.0f;
          check(silent, "a crashed instrument is silent, not left holding what it half-wrote"); }
        check(rt->tracks[1].getSample(0, 5) == 0.25f, "other tracks' instruments are unaffected");
        PluginFault f; check(rt->faults.pop(f) && f.rack == 0 && f.slot == -1, "the instrument slot is identified");
    }
    // 6. Master effects.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>();
        place(*rt, PluginRuntime::masterIndex, 0, plan(Fail::nan), c);
        rt->begin(64, 120, true, 0); for (int i = 0; i < 64; ++i) { rt->master.setSample(0, i, 0.4f); rt->master.setSample(1, i, 0.4f); }
        rt->processMaster(64);
        PluginFault f; check(rt->faults.pop(f) && f.rack == PluginRuntime::masterIndex && finite(rt->master, 64) && rt->master.getSample(0, 3) == 0.4f, "a failing master effect leaves the master bus intact");
    }
    // 7. A plugin that works for a while first.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>();
        place(*rt, 0, 0, plan(Fail::nullWrite, 1, 1000), c);
        int blocks = 0; for (; blocks < 1000; ++blocks) { rt->begin(64, 120, true, 0); fill(*rt, 0, 0.5f, 64); rt->effectsAndAlign(64); }
        check(rt->faultCount.load() == 0 && !rt->racks[0].effects[0].faulted.get(), "a healthy plugin is not disturbed by the guard");
        for (int i = 0; i < 100; ++i) { rt->begin(64, 120, true, 0); fill(*rt, 0, 0.5f, 64); rt->effectsAndAlign(64); }
        check(rt->faultCount.load() == 1 && c->processCalls.load() == 1001, "it fails once, late, and is then left alone");
    }
    // 8. A failed plugin stops contributing latency, so the other tracks stop waiting for it.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>(); auto p = plan(Fail::nullWrite); p->latency = 48;
        place(*rt, 0, 0, p, c);
        check(rt->rackLatency(rt->racks[0]) == 48, "latency counts while the plugin is healthy");
        rt->begin(64, 120, true, 0); fill(*rt, 0, 0.5f, 64); rt->effectsAndAlign(64);
        check(rt->rackLatency(rt->racks[0]) == 0 && rt->latencySamples() == 0, "...and stops when it has failed");
    }
    // 9. Failing to prepare.
    {
        auto rt = std::make_unique<PluginRuntime>(); auto c = std::make_shared<Counters>(); auto p = plan(); p->crashPrepare = true;
        auto& slot = place(*rt, 2, 0, p, c);
        PluginFault f; check(slot.faulted.get() && !slot.prepared && rt->faults.pop(f) && f.phase == PluginPhase::prepare && f.rack == 2 && f.slot == 0, "a plugin that crashes in prepareToPlay is switched off and reported");
        rt->begin(64, 120, true, 0); fill(*rt, 2, 0.5f, 64); rt->effectsAndAlign(64);
        check(c->processCalls.load() == 0 && rt->tracks[2].getSample(0, 1) == 0.5f, "...and never asked to process");
    }
    // 10. A failed plugin is parked, never destroyed: running its destructor would run its broken code again.
    {
        const auto parkedBefore = PluginRuntime::quarantinedCount();
        auto bad = std::make_shared<Counters>(), good = std::make_shared<Counters>();
        {
            auto rt = std::make_unique<PluginRuntime>();
            place(*rt, 0, 0, plan(Fail::nullWrite), bad); place(*rt, 0, 1, plan(), good);
            rt->begin(32, 120, true, 0); fill(*rt, 0, 0.5f, 32); rt->effectsAndAlign(32);
        }
        check(bad->destroyed.load() == 0, "a failed plugin's destructor is never run");
        check(good->destroyed.load() == 1, "a healthy plugin is destroyed normally");
        check(PluginRuntime::quarantinedCount() == parkedBefore + 1, "the failed one was parked");
    }
    // 11. Crashing while being released parks it too.
    {
        auto c = std::make_shared<Counters>(); auto p = plan(); p->crashRelease = true;
        auto rt = std::make_unique<PluginRuntime>(); auto& slot = place(*rt, 0, 0, p, c);
        rt->destroy(slot, 0, 0);
        PluginFault f; check(rt->faults.pop(f) && f.phase == PluginPhase::release && c->destroyed.load() == 0 && !slot.processor, "a plugin that crashes in releaseResources is reported and parked");
    }
    // 12. Asking for the tail length can fail too.
    {
        auto c = std::make_shared<Counters>(); auto p = plan(); p->crashTail = true;
        auto rt = std::make_unique<PluginRuntime>(); place(*rt, 0, 0, p, c);
        const double tail = rt->tailSeconds();
        PluginFault f; check(tail == 0.0 && rt->faults.pop(f) && f.phase == PluginPhase::query, "a plugin that crashes when asked its tail is switched off and counted as having none");
    }
    // 13. Many tracks failing in one block do not overflow anything.
    {
        auto rt = std::make_unique<PluginRuntime>(); std::vector<std::shared_ptr<Counters>> keep;
        int placed = 0;
        for (int t = 0; t < maxSessionTracks && placed < 40; ++t) for (int i = 0; i < 8 && placed < 40; ++i) { keep.push_back(std::make_shared<Counters>()); place(*rt, t, i, plan(Fail::nullWrite), keep.back()); ++placed; }
        for (int t = 0; t < maxSessionTracks; ++t) fill(*rt, static_cast<std::size_t>(t), 0.5f, 16);
        rt->begin(16, 120, true, 0); for (int t = 0; t < maxSessionTracks; ++t) fill(*rt, static_cast<std::size_t>(t), 0.5f, 16);
        rt->effectsAndAlign(16);
        check(rt->faultCount.load() == static_cast<std::uint32_t>(placed), "every failure in one block is counted");
        int reports = 0; PluginFault f; while (rt->faults.pop(f)) ++reports;
        check(reports + static_cast<int>(rt->faults.droppedCount()) == placed, "...and reported or counted as dropped");
    }
}

static void hostTests() {
    const auto logFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-plugin-fault-test", ".log");
    std::map<std::string, std::shared_ptr<Plan>> plans; std::map<std::string, std::shared_ptr<Counters>> counters;
    PluginFactory factory = [&](const juce::PluginDescription& d, double, int, juce::String& error) -> std::unique_ptr<juce::AudioProcessor> {
        const auto id = d.fileOrIdentifier.toStdString();
        if (id == "factory-crash") crash();
        if (id == "factory-throw") throw std::runtime_error("cannot load");
        if (id == "factory-missing") { error = "File not found"; return {}; }
        auto it = plans.find(id); if (it == plans.end()) { error = "unknown test plugin"; return {}; }
        return std::make_unique<Probe>(it->second, counters[id], d.isInstrument);
    };
    auto add = [&](const char* id, std::shared_ptr<Plan> p) { plans[id] = p; counters[id] = std::make_shared<Counters>(); };

    // Loading: crashes and exceptions while a plugin is created or given its saved state.
    {
        add("good", plan()); add("crash-set-state", plan()); plans["crash-set-state"]->crashSetState = true;
        add("crash-prepare", plan()); plans["crash-prepare"]->crashPrepare = true;
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, logFile, factory);
        std::vector<PluginRackState> racks(1); PluginRackState master;
        racks[0].effects = {stateFor("factory-crash", false, encoded("abc")), stateFor("factory-throw", false), stateFor("crash-set-state", false, encoded("abc")), stateFor("crash-prepare", false), stateFor("good", false), stateFor("factory-missing", false)};
        const auto saved = racks;
        const auto errors = host.restore(racks, master, 48000);
        check(errors.contains("crashed while loading"), "a plugin that crashes while loading is reported");
        check(errors.contains("File not found"), "ordinary load errors are still reported");
        check(runtime->racks[0].effectCount == 6 && !runtime->racks[0].effects[0].processor && !runtime->racks[0].effects[1].processor && !runtime->racks[0].effects[2].processor, "crashed or throwing loads leave empty placeholder slots");
        check(runtime->racks[0].effects[3].processor && runtime->racks[0].effects[3].faulted.get(), "a plugin that crashes in prepareToPlay stays visible but switched off");
        check(runtime->racks[0].effects[4].processor && !runtime->racks[0].effects[4].faulted.get(), "a healthy plugin after the broken ones loads normally");
        host.snapshot(racks, master);
        check(racks[0].effects[0] == saved[0].effects[0] && racks[0].effects[1] == saved[0].effects[1] && racks[0].effects[2] == saved[0].effects[2] && racks[0].effects[3] == saved[0].effects[3], "saved descriptions and state of the failed plugins are kept for the next try");
        check(racks[0].effects[4].stateBase64 == encoded("live"), "while the healthy plugin's state is captured as usual");
        check(host.diagnosticLog().read().contains("crashed while loading"), "the failure is written to the log");
    }
    // Reloading a rack that contains a failed plugin parks the old instance and starts a fresh one.
    {
        add("flaky", plan(Fail::nullWrite));
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, {}, factory);
        std::vector<PluginRackState> racks(1); PluginRackState master; racks[0].effects = {stateFor("flaky", false)};
        host.restore(racks, master, 48000);
        runtime->begin(32, 120, true, 0); fill(*runtime, 0, 0.5f, 32); runtime->effectsAndAlign(32);
        check(runtime->racks[0].effects[0].faulted.get(), "the plugin fails during playback");
        plans["flaky"]->inProcess = Fail::none;
        const auto parked = PluginRuntime::quarantinedCount();
        host.restore(racks, master, 48000);
        check(PluginRuntime::quarantinedCount() == parked + 1 && counters["flaky"]->destroyed.load() == 0, "reloading parks the failed instance instead of destroying it");
        check(runtime->racks[0].effects[0].processor && !runtime->racks[0].effects[0].faulted.get(), "and the fresh instance is healthy");
        runtime->begin(32, 120, true, 0); fill(*runtime, 0, 0.5f, 32); runtime->effectsAndAlign(32);
        check(!runtime->racks[0].effects[0].faulted.get() && runtime->tracks[0].getSample(0, 5) == 0.5f, "which now processes");
    }
    // The message thread learns about a runtime failure through poll().
    {
        add("dies", plan(Fail::nullWrite));
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, logFile, factory);
        juce::String reported; host.onProblem = [&](const juce::String& text) { reported += text; };
        std::vector<PluginRackState> racks(3); PluginRackState master; racks[2].effects = {stateFor("good", false), stateFor("dies", false)};
        host.restore(racks, master, 48000);
        host.poll(); check(reported.isEmpty(), "nothing is reported while all is well");
        runtime->begin(32, 120, true, 0); fill(*runtime, 2, 0.5f, 32); runtime->effectsAndAlign(32);
        host.poll();
        check(reported.contains("dies") && reported.contains("track 3 FX 2") && reported.contains("processing audio") && reported.contains("access violation"), "the report names the plugin, the slot, what it was doing and what went wrong");
        check(reported.contains("saved settings are kept"), "and tells the user their settings are safe");
        check(host.health(2, 1).contains("FAILED") && host.health(2, 0).isEmpty(), "the panel can ask which slots have failed");
        reported.clear(); host.poll(); check(reported.isEmpty(), "each failure is reported once");
        check(juce::File(logFile).loadFileAsString().contains("track 3 FX 2"), "and logged");
        std::vector<PluginRackState> snap = racks; host.snapshot(snap, master);
        check(snap[2].effects[1] == racks[2].effects[1] && snap[2].effects[0].stateBase64 == encoded("live"), "snapshots do not query a failed plugin");
    }
    // Saving state from a plugin that crashes when asked.
    {
        add("state-bomb", plan()); plans["state-bomb"]->crashGetState = true;
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, {}, factory);
        std::vector<PluginRackState> racks(1); PluginRackState master; racks[0].effects = {stateFor("state-bomb", false, encoded("old"))};
        host.restore(racks, master, 48000);
        host.snapshot(racks, master);
        check(racks[0].effects[0].stateBase64 == encoded("old"), "a crash while saving keeps the previous saved state");
        PluginFault f; check(runtime->racks[0].effects[0].faulted.get() && runtime->faults.pop(f) && f.phase == PluginPhase::saveState, "and the plugin is switched off and reported");
    }
    // The safety check before loading, and what the panel is told about slots that did not load.
    {
        add("fine", plan()); add("refused", plan());
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, logFile, factory);
        std::vector<juce::String> asked;
        host.preflight = [&](const juce::PluginDescription& d) -> juce::String { asked.push_back(d.name); return d.name == "refused" ? "refused because it crashed during the safety check" : juce::String(); };
        std::vector<PluginRackState> racks(2); PluginRackState master;
        racks[0].instrument = stateFor("refused", true, encoded("precious"));
        racks[0].effects = {stateFor("fine", false), stateFor("refused", false, encoded("precious")), stateFor("factory-crash", false), stateFor("factory-missing", false)};
        racks[1].effects = {stateFor("fine", false)};
        const auto saved = racks;
        const auto errors = host.restore(racks, master, 48000);
        check(std::count(asked.begin(), asked.end(), juce::String("refused")) == 2 && std::count(asked.begin(), asked.end(), juce::String("fine")) == 2, "every VST3 slot is checked before it is loaded");
        check(counters["refused"]->prepared.load() == 0 && counters["refused"]->destroyed.load() == 0, "a refused plugin is never created");
        check(errors.contains("refused because it crashed during the safety check"), "the reason reaches the caller");
        check(!runtime->racks[0].instrument.processor && !runtime->racks[0].effects[1].processor && runtime->racks[0].instrumentAssigned, "refused slots stay as placeholders and the track still has its instrument assigned");
        check(runtime->racks[0].effects[0].processor && runtime->racks[1].effects[0].processor && !runtime->racks[0].effects[0].faulted.get(), "the other plugins load normally");
        check(host.health(0, -1) == "NOT LOADED - blocked by the safety check" && host.health(0, 1) == "NOT LOADED - blocked by the safety check", "the panel is told a refused slot was blocked");
        check(host.health(0, 2) == "NOT LOADED - crashed while loading" && host.health(0, 3) == "NOT LOADED - could not be loaded", "and why a plugin that failed to load did not load");
        check(host.health(0, 0).isEmpty() && host.health(1, 0).isEmpty() && host.health(1, 1).isEmpty() && host.health(0, 9).isEmpty() && host.health(99, 0).isEmpty(), "healthy, empty and out-of-range slots have nothing to report");
        host.snapshot(racks, master);
        check(racks[0].instrument == saved[0].instrument && racks[0].effects[1] == saved[0].effects[1] && racks[0].effects[2] == saved[0].effects[2], "the project keeps the description and saved settings of everything that did not load");
        check(host.diagnosticLog().read().contains("refused because it crashed during the safety check"), "refusals are logged");
        // Loading again, with the safety check now satisfied, loads them and clears the notes.
        host.preflight = [&](const juce::PluginDescription&) { return juce::String(); };
        racks = saved; racks[0].effects[2] = stateFor("fine", false); racks[0].effects[3] = stateFor("fine", false);
        host.restore(racks, master, 48000);
        check(runtime->racks[0].effects[1].processor && host.health(0, 1).isEmpty() && host.health(0, 2).isEmpty() && host.health(0, 3).isEmpty(), "a later load that succeeds clears the 'not loaded' notes");
        // The note belongs to the load that produced it: a slot that is emptied afterwards must not keep saying "not loaded".
        host.preflight = [&](const juce::PluginDescription& d) -> juce::String { asked.push_back(d.name); return d.name == "refused" ? "refused again" : juce::String(); };
        racks = saved; host.restore(racks, master, 48000);
        check(host.health(0, -1).contains("NOT LOADED"), "(control) the refused instrument is reported again");
        racks[0].instrument = {};
        host.restore(racks, master, 48000);
        check(host.health(0, -1).isEmpty(), "once the refused instrument has been removed its slot no longer says 'not loaded'");
        // A refused slot keeps its bypass setting, so it comes back as the user left it.
        racks = saved; racks[0].effects[1].bypass = true;
        host.restore(racks, master, 48000);
        check(!runtime->racks[0].effects[1].processor && runtime->racks[0].effects[1].bypass, "a refused slot remembers that it was bypassed");
        // Stock effects are not plugin files: they are never sent to the safety check.
        {
            asked.clear();
            juce::PluginDescription stockDescription = stock::describe(stock::catalog[0]);
            PluginState stockState{stockDescription.createXml()->toString(), {}, false};
            std::vector<PluginRackState> stockRacks(1); stockRacks[0].effects = {stockState};
            host.restore(stockRacks, master, 48000);
            check(asked.empty() && runtime->racks[0].effects[0].processor != nullptr, "a stock effect loads without being checked");
        }
        // Without a safety check (the default), loading is as it always was.
        host.preflight = nullptr;
        host.restore(racks, master, 48000);
        check(runtime->racks[0].effects[1].processor && host.health(0, 1).isEmpty(), "with no safety check set, plugins load as before");
    }
    // Editors.
    {
        add("editor-bomb", plan()); plans["editor-bomb"]->crashEditor = true;
        auto runtime = std::make_unique<PluginRuntime>(); PluginHost host(*runtime, logFile, factory);
        std::vector<PluginRackState> racks(1); PluginRackState master; racks[0].effects = {stateFor("editor-bomb", false), stateFor("good", false)};
        host.restore(racks, master, 48000);
        const auto first = host.openEditor(0, 0);
        check(first.contains("failed while opening its editor") && first.contains("access violation"), "an editor that crashes while opening is reported to the user, not fatal");
        check(!runtime->racks[0].effects[0].faulted.get(), "the plugin's audio keeps running");
        check(host.openEditor(0, 0).contains("not opened again") && host.health(0, 0).contains("editor crashed"), "it is not opened again, and the slot says so");
        check(host.openEditor(0, 7).isEmpty() && host.openEditor(5, 0).isEmpty() && host.openEditor(-1, 0).isEmpty(), "empty slots and out-of-range requests do nothing");
        check(host.diagnosticLog().read().contains("failed while opening its editor"), "editor failures are logged");
        runtime->begin(32, 120, true, 0); fill(*runtime, 0, 0.5f, 32); runtime->effectsAndAlign(32);
        PluginFault f; check(!runtime->faults.pop(f), "no audio fault is recorded for an editor failure");
        runtime->racks[0].effects[1].faulted.set(true);
        check(host.openEditor(0, 1).contains("has failed and is switched off"), "a failed plugin's editor is not opened");
    }
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    queueTests(); runtimeTests(); hostTests();
    if (failures != 0) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "PASS: plugin crashes, exceptions and NaN are contained; failed plugins are parked, reported and logged; state and editors are protected\n";
    return 0;
}
