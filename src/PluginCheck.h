#pragma once
// A compatibility check for one VST3 plugin: does it load, accept the host's stereo layout, play at
// several sample rates and block sizes, produce clean audio, save and restore its state, survive
// parameter changes while audio runs, run as several instances, open and close its editor, and
// unload. Every step runs under a FaultGuard, so a crash is a result ("CRASH in step X") and the
// remaining steps are skipped. The whole battery normally runs in a child process (checkIsolated),
// so even a crash the guard cannot catch (heap corruption, a plugin's own thread) ends only the child
// and still yields a report that says which step it died in.
//
// Used two ways: a short version ("quick") as the pre-flight before a project loads an unverified
// plugin, and the full version by the AlienPluginCheck command-line tool.
#include "PluginHost.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace alien::check {

enum class Outcome { pass, warn, fail, crash, skipped };
inline const char* label(Outcome o) {
    switch (o) { case Outcome::pass: return "PASS"; case Outcome::warn: return "WARN"; case Outcome::fail: return "FAIL"; case Outcome::crash: return "CRASH"; case Outcome::skipped: return "SKIP"; }
    return "?";
}
inline const char* key(Outcome o) {
    switch (o) { case Outcome::pass: return "pass"; case Outcome::warn: return "warn"; case Outcome::fail: return "fail"; case Outcome::crash: return "crash"; case Outcome::skipped: return "skipped"; }
    return "fail";
}
inline Outcome outcomeFromKey(const juce::String& k) {
    for (auto o : {Outcome::pass, Outcome::warn, Outcome::fail, Outcome::crash, Outcome::skipped}) if (k == key(o)) return o;
    return Outcome::fail;
}

struct Step { juce::String name; Outcome outcome{Outcome::pass}; juce::String detail; double milliseconds{0}; };

struct Report {
    juce::String name, vendor, version, category, path, format, host;
    bool instrument{false}, hasEditor{false};
    bool hung{false};                      // the check process had to be stopped because it never finished
    int inputChannels{0}, outputChannels{0}, parameters{0}, programs{0}, latencySamples{0};
    double declaredTailSeconds{0}, measuredTailSeconds{-1};
    juce::StringArray notes;               // facts worth knowing that are not pass/fail (bus layout, sidechains...)
    std::vector<Step> steps;

    int count(Outcome o) const { int n = 0; for (const auto& s : steps) n += s.outcome == o; return n; }
    // The worst thing that happened, ignoring skipped steps.
    Outcome overall() const {
        if (count(Outcome::crash)) return Outcome::crash;
        if (count(Outcome::fail)) return Outcome::fail;
        if (count(Outcome::warn)) return Outcome::warn;
        return count(Outcome::pass) > 0 ? Outcome::pass : Outcome::skipped;
    }
    bool acceptable() const { const auto o = overall(); return o == Outcome::pass || o == Outcome::warn; }
    juce::String verdict() const {
        switch (overall()) {
            case Outcome::pass: return "COMPATIBLE";
            case Outcome::warn: return "COMPATIBLE WITH WARNINGS";
            case Outcome::fail: return "NOT COMPATIBLE";
            case Outcome::crash: return "CRASHED";
            case Outcome::skipped: return "NOT RUN";
        }
        return "?";
    }
    juce::String firstProblem() const {
        for (const auto& s : steps) if (s.outcome == Outcome::crash || s.outcome == Outcome::fail) return s.name + ": " + s.detail;
        return {};
    }
    juce::String toText() const {
        juce::String t;
        t << "Plugin:  " << name << (version.isNotEmpty() ? " " + version : juce::String()) << (vendor.isNotEmpty() ? "  (" + vendor + ")" : juce::String()) << "   " << format << " " << (instrument ? "instrument" : "FX") << "\n";
        t << "File:    " << path << "\n";
        if (host.isNotEmpty()) t << "Host:    " << host << "\n";
        t << "Info:    " << inputChannels << " in / " << outputChannels << " out, " << parameters << " parameters, " << programs << " program(s), latency " << latencySamples << " samples, declared tail "
          << juce::String(declaredTailSeconds, 2) << " s" << (measuredTailSeconds >= 0 ? ", measured tail " + juce::String(measuredTailSeconds, 2) + " s" : juce::String()) << (hasEditor ? ", has editor" : ", no editor") << "\n";
        for (const auto& n : notes) t << "Note:    " << n << "\n";
        t << "\n";
        for (const auto& s : steps)
            t << "  " << juce::String(label(s.outcome)).paddedRight(' ', 5) << "  " << s.name.paddedRight(' ', 38) << juce::String(juce::roundToInt(s.milliseconds)).paddedLeft(' ', 6) << " ms"
              << (s.detail.isNotEmpty() ? "   " + s.detail : juce::String()) << "\n";
        t << "\nResult:  " << verdict() << "  (" << count(Outcome::pass) << " passed, " << count(Outcome::warn) << " warning(s), " << count(Outcome::fail) << " failed, " << count(Outcome::crash) << " crashed, " << count(Outcome::skipped) << " skipped)\n";
        return t;
    }
    std::unique_ptr<juce::XmlElement> toXml() const {
        auto x = std::make_unique<juce::XmlElement>("PluginCheck");
        x->setAttribute("name", name); x->setAttribute("vendor", vendor); x->setAttribute("version", version); x->setAttribute("category", category);
        x->setAttribute("path", path); x->setAttribute("format", format); x->setAttribute("host", host); x->setAttribute("instrument", instrument); x->setAttribute("editor", hasEditor); x->setAttribute("hung", hung);
        x->setAttribute("inputs", inputChannels); x->setAttribute("outputs", outputChannels); x->setAttribute("parameters", parameters); x->setAttribute("programs", programs);
        x->setAttribute("latency", latencySamples); x->setAttribute("declaredTail", declaredTailSeconds); x->setAttribute("measuredTail", measuredTailSeconds);
        x->setAttribute("verdict", verdict());
        for (const auto& n : notes) x->createNewChildElement("Note")->setAttribute("text", n);
        for (const auto& s : steps) { auto* e = x->createNewChildElement("Step"); e->setAttribute("name", s.name); e->setAttribute("outcome", key(s.outcome)); e->setAttribute("detail", s.detail); e->setAttribute("ms", s.milliseconds); }
        return x;
    }
    static bool fromXml(const juce::XmlElement& x, Report& r) {
        if (x.getTagName() != "PluginCheck") return false;
        r = {};
        r.name = x.getStringAttribute("name"); r.vendor = x.getStringAttribute("vendor"); r.version = x.getStringAttribute("version"); r.category = x.getStringAttribute("category");
        r.path = x.getStringAttribute("path"); r.format = x.getStringAttribute("format"); r.host = x.getStringAttribute("host"); r.instrument = x.getBoolAttribute("instrument"); r.hasEditor = x.getBoolAttribute("editor"); r.hung = x.getBoolAttribute("hung");
        r.inputChannels = x.getIntAttribute("inputs"); r.outputChannels = x.getIntAttribute("outputs"); r.parameters = x.getIntAttribute("parameters"); r.programs = x.getIntAttribute("programs");
        r.latencySamples = x.getIntAttribute("latency"); r.declaredTailSeconds = x.getDoubleAttribute("declaredTail"); r.measuredTailSeconds = x.getDoubleAttribute("measuredTail", -1);
        for (auto* n : x.getChildWithTagNameIterator("Note")) r.notes.add(n->getStringAttribute("text"));
        for (auto* e : x.getChildWithTagNameIterator("Step")) r.steps.push_back({e->getStringAttribute("name"), outcomeFromKey(e->getStringAttribute("outcome")), e->getStringAttribute("detail"), e->getDoubleAttribute("ms")});
        return true;
    }
    static Report forDescription(const juce::PluginDescription& d) {
        Report r; r.name = d.name; r.vendor = d.manufacturerName; r.version = d.version; r.category = d.category; r.path = d.fileOrIdentifier;
        r.format = d.pluginFormatName; r.instrument = d.isInstrument; r.inputChannels = d.numInputChannels; r.outputChannels = d.numOutputChannels;
        r.host = "Alien Studio plugin check, " + juce::SystemStats::getOperatingSystemName() + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "");
        return r;
    }
};

struct Options {
    bool quick{false};                                  // the pre-flight: load, layout, one rate, a short run, state round trip, unload
    bool editor{true};                                  // open the editor (needs a display)
    std::vector<double> sampleRates{44100.0, 48000.0, 96000.0};
    std::vector<int> blockSizes{64, 512};
    double tailSearchSeconds{10.0};
};

inline bool displayAvailable() {
   #if JUCE_LINUX || JUCE_BSD
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
   #else
    return true;
   #endif
}

// ---------------------------------------------------------------------------------------------
// The battery
// ---------------------------------------------------------------------------------------------
class Battery {
public:
    using BeginCallback = std::function<void(const juce::String& stepName)>;
    using StepCallback = std::function<void(const Report&)>;
    Battery(juce::PluginDescription d, PluginFactory f, Options o, BeginCallback begin = {}, StepCallback done = {})
        : description(std::move(d)), factory(std::move(f)), options(std::move(o)), onBegin(std::move(begin)), onStep(std::move(done)) { report = Report::forDescription(description); }

    Report run() {
        const double primaryRate = 48000.0;
        stepMain("Load", [&] { return load(); });
        stepMain("Bus layout", [&] { return layout(); });
        if (options.quick) {
            stepAudio("Play at 48000 Hz / 512 samples", [&] { return play(primaryRate, 512, 40); });
            stepMain("State round trip", [&] { return stateRoundTrip(); });
        } else {
            for (const double rate : options.sampleRates)
                for (const int block : options.blockSizes)
                    stepAudio("Play at " + juce::String(juce::roundToInt(rate)) + " Hz / " + juce::String(block) + " samples", [&, rate, block] { return play(rate, block, 200); });
            stepAudio("Odd block sizes (1 to 512)", [&] { return oddBlocks(primaryRate); });
            stepAudio("Latency and tail", [&] { return latencyAndTail(primaryRate); });
            stepMain("State round trip", [&] { return stateRoundTrip(); });
            stepMain("Parameter changes while playing", [&] { return parameterSweep(primaryRate); });
            stepAudio("Two instances at once", [&] { return twoInstances(primaryRate); });
            stepMain("Damaged state is survived", [&] { return damagedState(primaryRate); });
            if (options.editor) stepMain("Editor opens, resizes and closes", [&] { return editorCycle(primaryRate); });
            else skip("Editor opens, resizes and closes", "Disabled for this run.");
            stepMain("Unload and reload three times", [&] { return reloadCycles(primaryRate); });
        }
        stepMain("Unload", [&] { return unload(); });
        return report;
    }

private:
    struct StepResult { Outcome outcome{Outcome::pass}; juce::String detail; };
    struct Stats { float peak{0}; double energy{0}; long long nonFinite{0}; int blocks{0}; int firstAudible{-1}; };

    // ---- plumbing ----
    void add(Step s) { report.steps.push_back(std::move(s)); if (onStep) onStep(report); }
    void skip(const juce::String& name, const juce::String& why) { add({name, Outcome::skipped, why, 0}); }
    // Every step runs on the main thread, as the host's own plugin management does (JUCE's VST3 host code takes the
    // message-manager lock in prepareToPlay, which a worker thread cannot do while the main thread waits for it).
    // Only the processBlock calls move to the audio thread.
    template <class F> void runGuarded(const juce::String& name, F&& body) {
        if (tainted) { skip(name, "Not run: the plugin crashed earlier (" + taintedBy + ")."); return; }
        if (name != "Load" && plugin == nullptr) { skip(name, "Not run: the plugin did not load."); return; }
        if (onBegin) onBegin(name);
        StepResult result; fault::Fault fault; bool completed = false;
        phase.clear();
        const auto started = juce::Time::getMillisecondCounterHiRes();
        completed = fault::guarded([&] {
            try { result = body(); }
            catch (const AudioCrash& crash) {
                if (crash.fault.kind == fault::Kind::cppException) result = {Outcome::fail, "The plugin threw a C++ exception while processing audio."};
                else { result = {Outcome::crash, "Crashed on the audio thread: " + juce::String(fault::describe(crash.fault)) + "."}; tainted = true; taintedBy = name; }
            }
        }, fault);
        stopAudio();            // a step abandoned by a crash must not leave audio running behind it
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
        if (!completed) {
            const bool exception = fault.kind == fault::Kind::cppException;
            result.outcome = exception ? Outcome::fail : Outcome::crash;
            const auto during = phase.isEmpty() ? juce::String() : " while " + phase;
            result.detail = exception ? juce::String("The plugin threw a C++ exception") + during + "." : "Crashed" + during + ": " + juce::String(fault::describe(fault)) + ".";
            if (!exception) { tainted = true; taintedBy = name; }
        }
        add({name, result.outcome, result.detail, elapsed});
    }
    template <class F> void stepMain(const juce::String& name, F&& body) { runGuarded(name, std::forward<F>(body)); }
    template <class F> void stepAudio(const juce::String& name, F&& body) { runGuarded(name, std::forward<F>(body)); }

    // The audio thread: one thread for the whole run, like a host's audio callback, so a plugin sees the
    // same thread for every block. A crash on it is caught there and rethrown on the main thread as AudioCrash.
    struct AudioCrash { fault::Fault fault; };
    class AudioThread {
    public:
        ~AudioThread() { { std::lock_guard<std::mutex> lock(mutex); quit = true; } wake.notify_all(); if (thread.joinable()) thread.join(); }
        // Runs `task` on the audio thread and waits for it. False, with the fault, if it crashed.
        bool run(const std::function<void()>& task, fault::Fault& fault) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!thread.joinable()) thread = std::thread([this] { loop(); });
            job = &task; faultOut = &fault; ok = true; busy = true;
            wake.notify_all();
            wake.wait(lock, [this] { return !busy; });
            return ok;
        }
    private:
        void loop() {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                wake.wait(lock, [this] { return job != nullptr || quit; });
                if (quit) return;
                const auto* task = job; job = nullptr;
                lock.unlock();
                const bool completed = fault::guarded([task] { (*task)(); }, *faultOut);
                lock.lock();
                ok = completed; busy = false;
                wake.notify_all();
            }
        }
        std::mutex mutex; std::condition_variable wake; std::thread thread;
        const std::function<void()>* job{nullptr}; fault::Fault* faultOut{nullptr}; bool ok{true}, busy{false}, quit{false};
    };

    // Audio running on its own thread beside a step that works on the main thread. Its state lives on
    // the heap, so a crash on the main thread (which abandons the step's stack) cannot leave it
    // pointing at dead variables; runGuarded stops it whatever happened.
    struct Worker { std::atomic<bool> stop{false}, faulted{false}; std::atomic<long long> bad{0}; fault::Fault info; std::thread thread; };
    struct AudioOutcome { bool faulted{false}; long long nonFinite{0}; juce::String description; };
    template <class Fill> void startAudio(int blockSize, int pauseMs, Fill fill) {
        auto w = std::make_shared<Worker>(); worker = w;
        juce::AudioProcessor* const target = plugin;
        w->thread = std::thread([this, w, target, blockSize, pauseMs, fill] {
            const bool ok = fault::guarded([&] {
                while (!w->stop.load()) {
                    const auto s = blocksImpl(*target, 1, blockSize, fill);
                    w->bad += s.nonFinite;
                    if (pauseMs > 0) juce::Thread::sleep(pauseMs);
                }
            }, w->info);
            if (!ok) w->faulted.store(true);
        });
    }
    AudioOutcome stopAudio() {
        AudioOutcome out;
        if (worker == nullptr) return out;
        worker->stop.store(true);
        if (worker->thread.joinable()) worker->thread.join();
        out.faulted = worker->faulted.load(); out.nonFinite = worker->bad.load();
        if (out.faulted) out.description = juce::String(fault::describe(worker->info));
        worker.reset();
        return out;
    }

    static float db(float linear) { return linear <= 1.0e-9f ? -180.0f : 20.0f * std::log10(linear); }
    static juce::String dbText(float linear) { return juce::String(db(linear), 1) + " dBFS"; }

    // ---- creating and driving the plugin ----
    PluginState stateObject() const { return {description.createXml()->toString(), {}, false}; }
    // Creates and configures one instance; null with `error` set if it cannot. Runs plugin code: call inside a guarded step.
    juce::AudioProcessor* createInstance(juce::String& error, double rate = 48000.0, int block = 512) {
        auto created = factory(description, rate, block, error);
        if (!created) { if (error.isEmpty()) error = "The plugin could not be created."; return nullptr; }
        auto* raw = created.release();
        if (!detail::configureVst3(*raw, stateObject(), description.isInstrument, error)) { destroyInstance(raw); return nullptr; }
        raw->setPlayHead(&playHead);
        return raw;
    }
    static void destroyInstance(juce::AudioProcessor* p) { if (p != nullptr) { p->releaseResources(); delete p; } }
    void prepare(juce::AudioProcessor& p, double rate, int block) {
        playHead.info.setBpm(120.0); playHead.info.setIsPlaying(true);
        p.setRateAndBufferSizeDetails(rate, block); p.prepareToPlay(rate, block);
    }
    // Processes blocks on the audio thread. Throws AudioCrash if the plugin crashes while doing so.
    template <class Fill> Stats runBlocks(juce::AudioProcessor& p, int blocks, int blockSize, Fill&& fill) {
        Stats s; fault::Fault fault;
        const std::function<void()> task = [&] { s = blocksImpl(p, blocks, blockSize, fill); };
        if (!audioThread.run(task, fault)) throw AudioCrash{fault};
        return s;
    }
    template <class Fill> Stats blocksImpl(juce::AudioProcessor& p, int blocks, int blockSize, Fill&& fill) {
        Stats s; juce::AudioBuffer<float> buffer(2, juce::jmax(1, blockSize)); juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            buffer.clear(); midi.clear();
            fill(b, buffer, midi);
            p.processBlock(buffer, midi);
            ++s.blocks;
            for (int c = 0; c < 2; ++c) {
                const float* d = buffer.getReadPointer(c);
                for (int i = 0; i < blockSize; ++i) {
                    std::uint32_t bits; std::memcpy(&bits, d + i, sizeof bits);
                    if ((bits & 0x7f800000u) == 0x7f800000u) { ++s.nonFinite; continue; }
                    const float a = std::abs(d[i]); s.peak = std::max(s.peak, a); s.energy += static_cast<double>(a) * a;
                    if (s.firstAudible < 0 && a > 1.0e-4f) s.firstAudible = b * blockSize + i;
                }
            }
        }
        return s;
    }
    void noise(juce::AudioBuffer<float>& buffer, float amplitude = 0.25f) {
        for (int c = 0; c < buffer.getNumChannels(); ++c) { auto* d = buffer.getWritePointer(c); for (int i = 0; i < buffer.getNumSamples(); ++i) { seed = seed * 1664525u + 1013904223u; d[i] = amplitude * (static_cast<float>((seed >> 8) & 0xffff) / 32768.0f - 1.0f); } }
    }
    static StepResult cleanOr(const Stats& s, const juce::String& what) {
        if (s.nonFinite > 0) return {Outcome::fail, juce::String(what) + ": " + juce::String(static_cast<juce::int64>(s.nonFinite)) + " samples were NaN or infinite."};
        if (s.peak > 1000.0f) return {Outcome::fail, juce::String(what) + ": output peaked at " + dbText(s.peak) + ", which is not a plausible signal."};
        return {};
    }

    // ---- the steps ----
    StepResult load() {
        juce::String error;
        phase = "creating the plugin";
        auto created = factory(description, 48000.0, 512, error);
        if (!created) return {Outcome::fail, error.isNotEmpty() ? error : "The plugin could not be created."};
        plugin = created.release();
        // For a VST3 plugin, asking whether it has an editor makes the plugin create its editor.
        phase = "asking the plugin for its name, editor, programs and parameters";
        report.name = plugin->getName().isNotEmpty() ? plugin->getName() : report.name;
        report.hasEditor = plugin->hasEditor(); report.programs = plugin->getNumPrograms(); report.parameters = plugin->getParameters().size();
        report.declaredTailSeconds = 0;
        return {};
    }
    StepResult layout() {
        for (int in = 0; in < 2; ++in) {
            const bool isInput = in == 0;
            for (int i = 0; i < plugin->getBusCount(isInput); ++i) {
                auto* bus = plugin->getBus(isInput, i);
                report.notes.add(juce::String(isInput ? "Input" : "Output") + " bus " + juce::String(i) + " \"" + bus->getName() + "\": default " + bus->getDefaultLayout().getDescription() + (i > 0 ? " (extra bus, e.g. sidechain: not used by this host version)" : juce::String()));
            }
        }
        juce::String error;
        if (!detail::configureVst3(*plugin, stateObject(), description.isInstrument, error)) return {Outcome::fail, error};
        plugin->setPlayHead(&playHead);
        report.inputChannels = plugin->getTotalNumInputChannels(); report.outputChannels = plugin->getTotalNumOutputChannels();
        return {Outcome::pass, description.isInstrument ? "Stereo output, no input." : "Stereo in, stereo out; extra buses switched off."};
    }
    StepResult play(double rate, int block, int sourceBlocks) {
        prepare(*plugin, rate, block);
        report.latencySamples = plugin->getLatencySamples();
        StepResult out; float silentPeak = 0, signalPeak = 0; double signalEnergy = 0; long long bad = 0;
        const int silenceBlocks = std::max(4, 4096 / block);
        const auto silence = runBlocks(*plugin, silenceBlocks, block, [](int, juce::AudioBuffer<float>&, juce::MidiBuffer&) {});
        silentPeak = silence.peak; bad += silence.nonFinite;
        Stats signal;
        if (description.isInstrument) {
            signal = runBlocks(*plugin, sourceBlocks, block, [&](int b, juce::AudioBuffer<float>&, juce::MidiBuffer& m) {
                if (b == 0) m.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
                if (b == sourceBlocks / 2) m.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            });
            for (int ch = 1; ch <= 16; ++ch) { juce::MidiBuffer m; m.addEvent(juce::MidiMessage::allNotesOff(ch), 0); juce::AudioBuffer<float> b2(2, block); b2.clear(); plugin->processBlock(b2, m); }
        } else {
            signal = runBlocks(*plugin, sourceBlocks, block, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { noise(b); });
        }
        signalPeak = signal.peak; signalEnergy = signal.energy; bad += signal.nonFinite;
        plugin->releaseResources();
        Stats merged; merged.nonFinite = bad; merged.peak = std::max(silentPeak, signalPeak);
        if (auto c = cleanOr(merged, "Audio"); c.outcome != Outcome::pass) return c;
        if (signalEnergy <= 0.0) {
            return {Outcome::warn, description.isInstrument ? "No sound for a middle-C note. Some instruments need a preset or samples loaded first." : "Silent output for a noise input (muted, or a mix control at zero?)."};
        }
        if (silentPeak > 0.01f) return {Outcome::warn, juce::String(description.isInstrument ? "Sounds with no notes playing" : "Produces output from silent input") + " (peak " + dbText(silentPeak) + ")."};
        return {Outcome::pass, "Output peak " + dbText(signalPeak) + "."};
    }
    StepResult oddBlocks(double rate) {
        prepare(*plugin, rate, 512);
        static const int sizes[] = {1, 2, 3, 7, 16, 31, 64, 100, 127, 128, 255, 333, 512, 1, 512, 17};
        long long bad = 0; float peak = 0; int total = 0;
        for (int round = 0; round < 8; ++round)
            for (const int n : sizes) {
                auto s = runBlocks(*plugin, 1, n, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) {
                    if (description.isInstrument) { if (round == 0 && n == 1 && total == 0) m.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(90)), 0); }
                    else noise(b);
                });
                bad += s.nonFinite; peak = std::max(peak, s.peak); total += n;
            }
        plugin->releaseResources();
        Stats st; st.nonFinite = bad; st.peak = peak;
        if (auto c = cleanOr(st, "Odd block sizes"); c.outcome != Outcome::pass) return c;
        return {Outcome::pass, juce::String(total) + " samples in blocks from 1 to 512."};
    }
    StepResult latencyAndTail(double rate) {
        const int block = 512;
        prepare(*plugin, rate, block);
        const int latency = plugin->getLatencySamples(); report.latencySamples = latency;
        const double declared = plugin->getTailLengthSeconds(); report.declaredTailSeconds = std::isfinite(declared) ? declared : -1;
        if (description.isInstrument) { plugin->releaseResources(); return {Outcome::pass, "Latency " + juce::String(latency) + " samples (instrument: tail not measured)."}; }
        juce::String detail; Outcome outcome = Outcome::pass;
        // Where does an impulse come out?
        const auto impulse = runBlocks(*plugin, 20, block, [&](int b, juce::AudioBuffer<float>& buf, juce::MidiBuffer&) { if (b == 0) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); } });
        if (impulse.nonFinite > 0) { plugin->releaseResources(); return {Outcome::fail, "Impulse response contained NaN or infinity."}; }
        if (impulse.firstAudible >= 0 && std::abs(impulse.firstAudible - latency) > 64 && impulse.peak > 0.05f) {
            outcome = Outcome::warn; detail = "Reports latency " + juce::String(latency) + " samples but an impulse first appears at sample " + juce::String(impulse.firstAudible) + ". Tracks would be misaligned. ";
        } else if (impulse.firstAudible >= 0) detail = "Impulse appears at sample " + juce::String(impulse.firstAudible) + " (reported latency " + juce::String(latency) + "). ";
        // How long does it ring after noise stops?
        plugin->releaseResources(); prepare(*plugin, rate, block);
        const int noiseBlocks = static_cast<int>(rate / block);   // one second
        runBlocks(*plugin, noiseBlocks, block, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { noise(b); });
        const int maxBlocks = static_cast<int>(options.tailSearchSeconds * rate / block);
        int lastAudible = -1, quietRun = 0, b = 0; long long bad = 0;
        for (; b < maxBlocks; ++b) {
            const auto s = runBlocks(*plugin, 1, block, [](int, juce::AudioBuffer<float>&, juce::MidiBuffer&) {});
            bad += s.nonFinite;
            if (s.peak > 1.0e-4f) { lastAudible = b; quietRun = 0; } else if (++quietRun > static_cast<int>(rate / block / 2)) break;   // half a second of near-silence ends it
        }
        plugin->releaseResources();
        if (bad > 0) return {Outcome::fail, "NaN or infinity while the tail decayed."};
        const double measured = lastAudible < 0 ? 0.0 : (lastAudible + 1) * block / rate;
        report.measuredTailSeconds = measured;
        const bool capped = b >= maxBlocks;
        detail += "Tail: " + juce::String(measured, 2) + " s" + (capped ? " or more (stopped listening at " + juce::String(options.tailSearchSeconds, 0) + " s)" : juce::String()) + ", plugin declares " + (declared >= 0 && std::isfinite(declared) ? juce::String(declared, 2) + " s" : juce::String("none/infinite")) + ".";
        if (!capped && std::isfinite(declared) && measured > declared + 0.5) {
            outcome = Outcome::warn; detail += " The audible tail is longer than declared, so exports and freezes may cut it off.";
        }
        return {outcome, detail};
    }
    // Whole-state comparison through a second instance.
    StepResult stateRoundTrip() {
        prepare(*plugin, 48000.0, 512);
        const int paramCount = std::min(plugin->getParameters().size(), 16);
        for (int i = 0; i < paramCount; ++i) {
            auto* p = plugin->getParameters()[i];
            if (!p->isAutomatable()) continue;
            float v = 0.37f + 0.01f * static_cast<float>(i % 7);
            if (p->isDiscrete() && p->getNumSteps() > 1) { const float steps = static_cast<float>(p->getNumSteps() - 1); v = std::round(v * steps) / steps; }   // a switch has no 0.4: it would be rounded on restore, and that is not a finding
            p->setValue(v);
        }
        // Parameter changes made through a plugin's controller reach its processor with the next audio block, so a
        // few blocks run before the state is saved (and again after it is restored), as they do in a host that is playing.
        const auto settle = [&](juce::AudioProcessor& p) {
            runBlocks(p, 4, 512, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { if (!description.isInstrument) noise(b); });
        };
        settle(*plugin);
        juce::MemoryBlock first, again; plugin->getStateInformation(first); plugin->getStateInformation(again);
        juce::String error; auto* other = createInstance(error);
        if (other == nullptr) { plugin->releaseResources(); return {Outcome::fail, "A second instance could not be created: " + error}; }
        prepare(*other, 48000.0, 512);
        other->setStateInformation(first.getData(), static_cast<int>(first.getSize()));
        settle(*other);
        juce::MemoryBlock restored; other->getStateInformation(restored);
        juce::StringArray differing;
        for (int i = 0; i < plugin->getParameters().size(); ++i) {
            if (i >= other->getParameters().size()) break;
            if (std::abs(plugin->getParameters()[i]->getValue() - other->getParameters()[i]->getValue()) > 2.0e-3f) differing.add(plugin->getParameters()[i]->getName(40));
        }
        destroyInstance(other);
        plugin->releaseResources();
        juce::String detail = juce::String(static_cast<juce::int64>(first.getSize())) + " bytes of state";
        Outcome outcome = Outcome::pass;
        if (first.getSize() == 0) { outcome = Outcome::warn; detail += ": the plugin saved nothing, so its settings would not survive a project save."; }
        if (first.getSize() != again.getSize()) { outcome = Outcome::warn; detail += ". Asking twice gave different sizes (" + juce::String(static_cast<juce::int64>(first.getSize())) + " then " + juce::String(static_cast<juce::int64>(again.getSize())) + ")."; }
        if (!differing.isEmpty()) {
            outcome = Outcome::warn; detail += ". " + juce::String(differing.size()) + " parameter(s) differ after restore: " + differing.joinIntoString(", ", 0, 5) + (differing.size() > 5 ? ", ..." : "") + ".";
        } else if (outcome == Outcome::pass) detail += ", restored into a second instance with identical parameters.";
        return {outcome, detail};
    }
    StepResult parameterSweep(double rate) {
        prepare(*plugin, rate, 512);
        // Audio runs on its own thread while the parameters change, as in real use.
        startAudio(256, 0, [this](int, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) { if (description.isInstrument) m.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0); else noise(b); });
        const int count = std::min(plugin->getParameters().size(), 64);
        int touched = 0;
        for (int i = 0; i < count && !worker->faulted.load(); ++i) {
            auto* p = plugin->getParameters()[i];
            const float original = p->getValue();
            for (const float v : {0.0f, 1.0f, 0.5f, original}) { p->setValue(v); (void) p->getText(v, 32); (void) p->getValue(); }
            ++touched;
            juce::Thread::sleep(2);
        }
        const auto audio = stopAudio();
        plugin->releaseResources();
        if (audio.faulted) return {Outcome::crash, "Crashed on the audio thread while parameters were changing: " + audio.description + "."};
        if (audio.nonFinite > 0) return {Outcome::fail, "Parameter changes produced NaN or infinite audio."};
        return {Outcome::pass, juce::String(touched) + " parameter(s) moved to 0, 1, 0.5 and back during playback."};
    }
    StepResult twoInstances(double rate) {
        juce::String error; auto* extra = createInstance(error, rate);
        if (extra == nullptr) return {Outcome::fail, "A second instance could not be created: " + error};
        prepare(*plugin, rate, 256); prepare(*extra, rate, 256);
        long long bad = 0;
        for (int round = 0; round < 40; ++round)
            for (auto* p : {plugin, extra}) {
                auto s = runBlocks(*p, 1, 256, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) { if (description.isInstrument) { if (round == 0) m.addEvent(juce::MidiMessage::noteOn(1, p == plugin ? 60 : 67, static_cast<juce::uint8>(100)), 0); } else noise(b, p == plugin ? 0.25f : 0.1f); });
                bad += s.nonFinite;
            }
        destroyInstance(extra);                      // the second one goes first; the first must carry on
        const auto after = runBlocks(*plugin, 20, 256, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { if (!description.isInstrument) noise(b); });
        plugin->releaseResources();
        if (bad + after.nonFinite > 0) return {Outcome::fail, "NaN or infinity with two instances running."};
        return {Outcome::pass, "Two instances ran side by side; removing one left the other working."};
    }
public:
    // Damaged copies of state a plugin saved: random bytes of several sizes, then truncated and partly overwritten
    // copies of the real thing. A damaged project file is far more often the second kind than the first.
    static std::vector<juce::MemoryBlock> damagedVariants(const juce::MemoryBlock& real, juce::Random& random) {
        std::vector<juce::MemoryBlock> out;
        for (const int size : {0, 1, 7, 64, 4096}) {
            juce::MemoryBlock junk(static_cast<std::size_t>(size));
            for (int i = 0; i < size; ++i) static_cast<char*>(junk.getData())[i] = static_cast<char>(random.nextInt(256));
            out.push_back(std::move(junk));
        }
        if (real.getSize() > 4) {
            const auto n = real.getSize();
            for (const std::size_t keep : {n - 1, n * 3 / 4, n / 2, n / 4, std::size_t{1}}) out.emplace_back(real.getData(), keep);
            for (int round = 0; round < 3; ++round) {
                juce::MemoryBlock flipped(real);
                for (int k = 0; k < 8; ++k) static_cast<unsigned char*>(flipped.getData())[random.nextInt(static_cast<int>(n))] ^= 0xFF;
                out.push_back(std::move(flipped));
            }
        }
        return out;
    }
private:
    StepResult damagedState(double rate) {
        prepare(*plugin, rate, 512);
        juce::Random random(1234);
        juce::MemoryBlock real; plugin->getStateInformation(real);
        const auto variants = damagedVariants(real, random);
        for (const auto& damaged : variants) {
            plugin->setStateInformation(damaged.getData(), static_cast<int>(damaged.getSize()));
            runBlocks(*plugin, 2, 256, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { if (!description.isInstrument) noise(b); });
        }
        plugin->releaseResources();
        return {Outcome::pass, juce::String(static_cast<int>(variants.size())) + " damaged states (random bytes, and truncated or corrupted copies of its own state) were accepted or refused without crashing."};
    }
    StepResult editorCycle(double rate) {
        if (!plugin->hasEditor()) return {Outcome::pass, "No editor of its own; Alien Studio shows a generic parameter panel."};
        if (!displayAvailable()) return {Outcome::skipped, "No display is available on this machine."};
       #if !JUCE_MODAL_LOOPS_PERMITTED
        juce::ignoreUnused(rate);
        return {Outcome::skipped, "This build cannot run the nested message loop the editor test needs."};
       #else
        prepare(*plugin, rate, 256);
        startAudio(256, 2, [this](int, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { if (!description.isInstrument) noise(b); });
        const auto pump = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };
        juce::String detail; Outcome outcome = Outcome::pass;
        {
            auto* editor = plugin->createEditorIfNeeded();
            if (editor == nullptr) { stopAudio(); plugin->releaseResources(); return {Outcome::fail, "hasEditor() is true but no editor was created."}; }
            const int w = editor->getWidth(), h = editor->getHeight();
            const bool resizable = editor->isResizable();
            if (w < 10 || h < 10 || w > 8000 || h > 8000) { outcome = Outcome::warn; detail = "Editor size looks wrong (" + juce::String(w) + " x " + juce::String(h) + "). "; }
            auto window = std::make_unique<HostWindow>(plugin->getName());
            window->setContentOwned(editor, true);
            window->setResizable(resizable, false);
            window->setTopLeftPosition(40, 40);
            window->setVisible(true);
            pump(500);
            if (resizable) { window->setSize(window->getWidth() + 40, window->getHeight() + 30); pump(200); }
            window->setVisible(false); pump(100);
            window.reset();                 // destroys the editor with the window
            pump(200);
            detail += "Opened a " + juce::String(w) + " x " + juce::String(h) + " editor" + (resizable ? ", resized it" : "") + " and closed it.";
        }
        const auto audio = stopAudio();
        plugin->releaseResources();
        if (audio.faulted) return {Outcome::crash, "Crashed on the audio thread while its editor was open: " + audio.description + "."};
        if (audio.nonFinite > 0) return {Outcome::fail, "NaN or infinity while the editor was open."};
        return {outcome, detail};
       #endif
    }
    StepResult reloadCycles(double rate) {
        for (int cycle = 0; cycle < 3; ++cycle) {
            destroyInstance(plugin); plugin = nullptr;
            juce::String error; plugin = createInstance(error, rate);
            if (plugin == nullptr) return {Outcome::fail, "Reload " + juce::String(cycle + 1) + " failed: " + error};
            prepare(*plugin, rate, 256);
            const auto s = runBlocks(*plugin, 10, 256, [&](int, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) { if (description.isInstrument) m.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0); else noise(b); });
            plugin->releaseResources();
            if (s.nonFinite > 0) return {Outcome::fail, "NaN or infinity after reload " + juce::String(cycle + 1) + "."};
        }
        return {Outcome::pass, "Created, played and destroyed the plugin three more times."};
    }
    StepResult unload() {
        destroyInstance(plugin); plugin = nullptr;
        return {};
    }

    struct HostWindow final : juce::DocumentWindow {
        explicit HostWindow(const juce::String& title) : juce::DocumentWindow(title, juce::Colours::darkgrey, juce::DocumentWindow::closeButton) { setUsingNativeTitleBar(true); }
        void closeButtonPressed() override { setVisible(false); }
    };

    juce::PluginDescription description;
    PluginFactory factory;
    Options options;
    BeginCallback onBegin;
    StepCallback onStep;
    Report report;
    juce::AudioProcessor* plugin{nullptr};      // a plain pointer: after a crash nothing may run its destructor
    PluginRuntime::PlayHead playHead;
    bool tainted{false};
    juce::String taintedBy;
    std::shared_ptr<Worker> worker;
    AudioThread audioThread;
    juce::String phase;                 // what the current step is doing, for the report if it crashes
    std::uint32_t seed{12345};
};

// ---------------------------------------------------------------------------------------------
// Running the battery in a child process
// ---------------------------------------------------------------------------------------------
// Child side of "--check-vst3 <description.xml> <result.xml> <quick|full> <editor|noeditor>".
// The result file is rewritten after every step, so a crash that cannot be caught still leaves
// the steps that finished; the ".stage" file names the step that was running.
inline int checkVst3InChild(const juce::File& descriptionFile, const juce::File& output, bool quick, bool editor) {
    scan::redirectOutputToFile(output);
    scan::writeStage(output, "start");
    auto xml = juce::parseXML(descriptionFile);
    juce::PluginDescription description;
    if (!xml || !description.loadFromXml(*xml)) return scan::exitBadArguments;
    Options options; options.quick = quick; options.editor = editor;
    Battery battery(description, &createVst3Instance, options,
                    [&](const juce::String& stepName) { scan::writeStage(output, stepName); },
                    [&](const Report& r) { if (auto x = r.toXml()) x->writeTo(output); });
    const auto report = battery.run();
    if (auto x = report.toXml()) x->writeTo(output);
    scan::writeStage(output, "done");
    return scan::exitOk;
}

struct IsolatedOptions { juce::File executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile); juce::File scratch = juce::File::getSpecialLocation(juce::File::tempDirectory); int timeoutMs{120000}; Options battery; };

// Parent side: runs the check in a separate process and always returns a report, including when
// the process crashed (the report then ends in a CRASH step naming where) or hung.
inline Report checkIsolated(const juce::PluginDescription& description, const IsolatedOptions& isolated) {
    auto report = Report::forDescription(description);
    const auto output = scan::uniqueOutput(isolated.scratch, "AlienPluginCheck");
    const auto descriptionFile = scan::siblingOf(output, ".desc"), stageFile = scan::siblingOf(output, ".stage"), logFile = scan::siblingOf(output, ".log");
    for (const auto& f : {output, stageFile, logFile}) f.deleteFile();
    if (auto x = description.createXml()) x->writeTo(descriptionFile);
    bool started = false, timedOut = false; juce::uint32 exitCode = 0;
    const auto begin = juce::Time::getMillisecondCounter();
    if (isolated.executable.existsAsFile()) {
        juce::ChildProcess child;
        started = child.start(juce::StringArray{isolated.executable.getFullPathName(), "--check-vst3", descriptionFile.getFullPathName(), output.getFullPathName(),
                                                isolated.battery.quick ? "quick" : "full", isolated.battery.editor ? "editor" : "noeditor"}, 0);
        if (started) {
            if (!child.waitForProcessToFinish(isolated.timeoutMs)) { child.kill(); child.waitForProcessToFinish(3000); timedOut = true; }
            else exitCode = child.getExitCode();
        }
    }
    const auto elapsed = static_cast<double>(juce::Time::getMillisecondCounter() - begin);
    const auto stage = stageFile.loadFileAsString().trim();
    if (auto x = juce::parseXML(output)) { Report parsed; if (Report::fromXml(*x, parsed)) { const auto host = report.host; report = parsed; if (report.host.isEmpty()) report.host = host; } }
    const auto printed = logFile.loadFileAsString().substring(0, 2000).trim();
    for (const auto& f : {output, descriptionFile, stageFile, logFile}) f.deleteFile();
    if (!started) { report.steps.push_back({"Start the check", Outcome::fail, "The check process could not be started (" + isolated.executable.getFullPathName() + ").", 0}); return report; }
    if (stage != "done") {
        const bool ntCrash = (exitCode & 0xC0000000u) == 0xC0000000u;
        juce::String why;
        if (timedOut) why = "Hung: no result after " + juce::String(isolated.timeoutMs / 1000) + " s.";
        else if (ntCrash) why = "The process crashed (exit code " + scan::hex32(exitCode) + (scan::exceptionName(exitCode).isNotEmpty() ? ", " + scan::exceptionName(exitCode) : juce::String()) + ").";
        else why = "The process ended without finishing (it most likely crashed; this was not a fault the host can intercept, such as heap corruption or a crash on a plugin-owned thread).";
        report.hung = timedOut;
        report.steps.push_back({stage.isEmpty() || stage == "start" ? juce::String("Starting the check") : stage, timedOut ? Outcome::fail : Outcome::crash, why + (printed.isNotEmpty() ? " Plugin output: " + printed.substring(0, 300) : juce::String()), elapsed});
    }
    return report;
}

} // namespace alien::check
