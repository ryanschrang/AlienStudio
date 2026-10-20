// Stock effects as hosted processors: parameters, state, rack integration, latency
// compensation, real-time safety and editor panels.
#include "PluginHost.h"
#include "SessionProject.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <thread>

// Count heap allocations made on the calling thread so the audio path can be proven allocation-free.
static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace alien;

static int failures = 0;
static void check(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message.c_str()); }
}
static void near(double actual, double expected, double tolerance, const std::string& message) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n", message.c_str(), actual, expected, tolerance);
    }
}

constexpr double rate = 48000.0;

struct FixedTempo final : juce::AudioPlayHead {
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setBpm(bpm); return p; }
    double bpm{120.0};
};

static void setParam(stock::Effect& e, const char* id, double realValue) {
    auto* p = e.parameters.getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(realValue)));
}
static double getParam(stock::Effect& e, const char* id) { return *e.parameters.getRawParameterValue(id); }

static std::unique_ptr<stock::Effect> make(const char* id, double sampleRate = rate) {
    auto e = stock::create(id);
    e->setRateAndBufferSizeDetails(sampleRate, 512);
    e->prepareToPlay(sampleRate, 512);
    return e;
}
// Runs `input` through an effect in blocks of `block` samples (left only mirrored to right when `mono`).
static void process(juce::AudioProcessor& e, std::vector<float>& l, std::vector<float>& r, int block = 512) {
    juce::MidiBuffer midi;
    for (std::size_t pos = 0; pos < l.size(); pos += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), l.size() - pos));
        float* ptrs[]{l.data() + pos, r.data() + pos};
        juce::AudioBuffer<float> view(ptrs, 2, n);
        e.processBlock(view, midi);
    }
}
static std::vector<float> noise(int count, double amplitude, unsigned seed = 1) {
    std::mt19937 gen(seed); std::uniform_real_distribution<double> d(-1.0, 1.0);
    std::vector<float> x(static_cast<std::size_t>(count));
    for (auto& v : x) v = static_cast<float>(d(gen) * amplitude);
    return x;
}
static std::vector<float> sine(double f, double a, int count, double sr = rate) {
    std::vector<float> x(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) x[static_cast<std::size_t>(i)] = static_cast<float>(a * std::sin(2.0 * dsp::pi * f * i / sr));
    return x;
}
static double peak(const std::vector<float>& x, int from, int to) {
    double p = 0; for (int i = from; i < to; ++i) p = std::max(p, static_cast<double>(std::abs(x[static_cast<std::size_t>(i)]))); return p;
}
static bool allFinite(const std::vector<float>& x) { for (float v : x) if (!dsp::finite(v)) return false; return true; }

// ---------------------------------------------------------------------------
static void catalogueTests() {
    check(stock::catalog.size() == 5, "five stock effects");
    std::set<juce::String> names, ids;
    for (const auto& info : stock::catalog) {
        auto e = stock::create(info.id);
        check(e != nullptr, std::string("create ") + info.id);
        if (!e) continue;
        check(e->getName() == info.name, std::string("name of ") + info.id);
        check(e->getTotalNumInputChannels() == 2 && e->getTotalNumOutputChannels() == 2, std::string("stereo in/out: ") + info.id);
        check(!e->acceptsMidi() && !e->producesMidi(), std::string("no MIDI: ") + info.id);
        names.insert(info.name); ids.insert(info.id);
        const auto description = stock::describe(info);
        auto xml = description.createXml();
        juce::PluginDescription back;
        check(back.loadFromXml(*xml) && stock::isStock(back) && back.fileOrIdentifier == info.id && !back.isInstrument, std::string("description round trip: ") + info.id);
        check(PluginState{xml->toString(), {}, false}.valid(), std::string("description is a valid rack entry: ") + info.id);
        check(e->hasEditor(), std::string("has an editor: ") + info.id);
    }
    check(names.size() == 5 && ids.size() == 5, "names and ids are unique");
    check(stock::create("nonsense") == nullptr && stock::create("") == nullptr, "unknown ids create nothing");
    juce::PluginDescription vst; vst.pluginFormatName = "VST3";
    check(!stock::isStock(vst), "a VST3 is not a stock effect");
}

static void parameterTests() {
    for (const auto& info : stock::catalog) {
        auto e = stock::create(info.id);
        int count = 0;
        for (auto* p : e->getParameters()) {
            ++count;
            const float def = p->getDefaultValue();
            check(def >= 0.0f && def <= 1.0f, std::string(info.id) + ": default in range for " + p->getName(64).toStdString());
            check(p->getText(def, 64).isNotEmpty(), std::string(info.id) + ": parameter has text " + p->getName(64).toStdString());
            check(p->getParameterIndex() >= 0, "indexed");
        }
        check(count >= 3, std::string(info.id) + " exposes parameters for automation");
    }
    // Typed values.
    auto eq = stock::create("eq");
    auto* freq = eq->parameters.getParameter(stock::eq::lowFreq);
    near(freq->convertFrom0to1(freq->getValueForText("250")), 250, 0.5, "typed Hz");
    auto* high = eq->parameters.getParameter(stock::eq::highFreq);
    near(high->convertFrom0to1(high->getValueForText("12k")), 12000, 1, "typed kHz");
    near(high->convertFrom0to1(high->getValueForText("1.5 kHz")), 1500, 1, "typed kHz with unit");
    auto comp = stock::create("compressor");
    auto* release = comp->parameters.getParameter(stock::comp::release);
    near(release->convertFrom0to1(release->getValueForText("1.5 s")), 1500, 1, "typed seconds");
    near(release->convertFrom0to1(release->getValueForText("250 ms")), 250, 0.5, "typed milliseconds");
    auto* mix = comp->parameters.getParameter(stock::comp::mix);
    near(mix->convertFrom0to1(mix->getValueForText("40%")), 0.4, 0.001, "typed percent");
    check(mix->getText(mix->convertTo0to1(0.4f), 32) == "40 %", "percent text");
    auto* threshold = comp->parameters.getParameter(stock::comp::threshold);
    check(threshold->getText(threshold->convertTo0to1(-18.0f), 32) == "-18.0 dB", "dB text");
    auto dly = stock::create("delay");
    auto* tone = dly->parameters.getParameter(stock::delay::tone);
    check(tone->getText(1.0f, 32) == "Open", "open tone");
    near(tone->convertFrom0to1(tone->getValueForText("open")), 20000, 1, "typed Open");
    // Out-of-range typed values clamp rather than escape the range.
    check(threshold->getValueForText("-500") == 0.0f && threshold->getValueForText("50") == 1.0f, "typed values are clamped to the range");
}

// ---------------------------------------------------------------------------
static void processingTests() {
    // Defaults leave a quiet signal alone (EQ flat, compressor below threshold, 0% wet where applicable).
    {
        auto e = make("eq"); auto l = noise(4096, 0.5, 2), r = noise(4096, 0.5, 3); const auto l0 = l, r0 = r;
        process(*e, l, r, 100);
        check(l == l0 && r == r0, "default EQ is transparent");
    }
    {
        auto e = make("compressor"); auto l = noise(4096, 0.05, 2), r = noise(4096, 0.05, 3); const auto l0 = l, r0 = r;
        process(*e, l, r, 100);
        check(l == l0 && r == r0, "default compressor does nothing below the threshold");
    }
    for (const char* id : {"reverb", "delay"}) {
        auto e = make(id); setParam(*e, "mix", 0.0);
        auto l = noise(4096, 0.5, 2), r = noise(4096, 0.5, 3); const auto l0 = l, r0 = r;
        process(*e, l, r, 300);
        check(l == l0 && r == r0, std::string(id) + " at 0% mix passes the dry signal bit-exactly");
    }
    // EQ: a +12 dB bell is +12 dB.
    {
        auto e = make("eq"); setParam(*e, stock::eq::m1Freq, 1000); setParam(*e, stock::eq::m1Gain, 12);
        auto l = sine(1000, 0.1, 24000), r = l; process(*e, l, r);
        near(20 * std::log10(peak(l, 12000, 24000) / 0.1), 12.0, 0.1, "EQ band gain through the processor");
    }
    // Compressor: the same static curve as the kernel, and the meter moves.
    {
        auto e = make("compressor");
        setParam(*e, stock::comp::threshold, -18); setParam(*e, stock::comp::ratio, 4); setParam(*e, stock::comp::knee, 0);
        setParam(*e, stock::comp::attack, 5); setParam(*e, stock::comp::release, 50);
        auto l = sine(1000, dsp::dbToGain(-6), 48000 * 2), r = l; process(*e, l, r);
        near(dsp::gainToDb(peak(l, 48000 * 2 - 9600, 48000 * 2)), -18 + 12.0 / 4.0, 0.35, "compressor steady-state level");
        check(dynamic_cast<stock::CompressorEffect&>(*e).gainReductionDb() > 5.0f, "compressor reports gain reduction");
    }
    // Compressor: the attack knob sets the attack and the release knob the release (not the other way round).
    {
        const double target = dsp::compressorReductionDb(dsp::gainToDb(0.5), -30, 10, 0);
        auto e = make("compressor");
        setParam(*e, stock::comp::threshold, -30); setParam(*e, stock::comp::ratio, 10); setParam(*e, stock::comp::knee, 0);
        setParam(*e, stock::comp::attack, 1); setParam(*e, stock::comp::release, 1000);
        const int loud = 48000, quiet = 48000;
        std::vector<float> l(static_cast<std::size_t>(loud + quiet), 0.001f), r;
        for (int i = 0; i < loud; ++i) l[static_cast<std::size_t>(i)] = 0.5f;
        r = l; process(*e, l, r, 128);
        near(-dsp::gainToDb(l[960] / 0.5) / target, 1.0, 0.08, "a 1 ms attack has almost fully clamped down after 20 ms");
        const double remaining = -dsp::gainToDb(l[static_cast<std::size_t>(loud + 4800)] / 0.001) / target;
        check(remaining > 0.7 && remaining < 0.95, "a 1000 ms release is still mostly clamped 100 ms after the signal stops (" + std::to_string(remaining) + " of the reduction left)");
        auto f = make("compressor");
        setParam(*f, stock::comp::threshold, -30); setParam(*f, stock::comp::ratio, 10); setParam(*f, stock::comp::knee, 0);
        setParam(*f, stock::comp::attack, 200); setParam(*f, stock::comp::release, 5);
        std::vector<float> a(static_cast<std::size_t>(loud + quiet), 0.001f), b;
        for (int i = 0; i < loud; ++i) a[static_cast<std::size_t>(i)] = 0.5f;
        b = a; process(*f, a, b, 128);
        check(-dsp::gainToDb(a[960] / 0.5) / target < 0.2, "a 200 ms attack has barely started after 20 ms");
        check(-dsp::gainToDb(a[static_cast<std::size_t>(loud + 4800)] / 0.001) / target < 0.05, "a 5 ms release has let go entirely 100 ms after the signal stops");
    }
    // Delay: an impulse comes back at the set time; tempo sync reads the host tempo.
    {
        auto e = make("delay"); setParam(*e, stock::delay::time, 10); setParam(*e, stock::delay::feedback, 0);
        setParam(*e, stock::delay::mix, 1); setParam(*e, stock::delay::tone, 20000);
        std::vector<float> l(4000, 0.0f), r(4000, 0.0f); l[0] = 1.0f; process(*e, l, r, 333);
        near(l[480], 1.0, 1e-5, "delay echo at the set time, whatever the block size");
    }
    {
        auto e = make("delay"); FixedTempo tempo; tempo.bpm = 120; e->setPlayHead(&tempo);
        setParam(*e, stock::delay::sync, 1); setParam(*e, stock::delay::division, 2); // 1/8 note
        setParam(*e, stock::delay::feedback, 0); setParam(*e, stock::delay::mix, 1); setParam(*e, stock::delay::tone, 20000);
        std::vector<float> l(20000, 0.0f), r(20000, 0.0f); l[0] = 1.0f; process(*e, l, r);
        near(l[12000], 1.0, 1e-5, "1/8 note at 120 BPM is 250 ms (12000 samples)");
        auto f = make("delay"); tempo.bpm = 60; f->setPlayHead(&tempo);
        setParam(*f, stock::delay::sync, 1); setParam(*f, stock::delay::division, 4);
        setParam(*f, stock::delay::feedback, 0); setParam(*f, stock::delay::mix, 1); setParam(*f, stock::delay::tone, 20000);
        std::vector<float> a(60000, 0.0f), b(60000, 0.0f); a[0] = 1.0f; process(*f, a, b);
        near(a[48000], 1.0, 1e-5, "a quarter note at 60 BPM is one second");
        check(e->getTailLengthSeconds() >= 0.25, "delay tail covers at least one repeat");
        check(f->getTailLengthSeconds() >= 1.0, "delay tail follows the tempo-synced time (a quarter note at 60 BPM is 1 s)");
    }
    {   // Without a tempo the delay assumes 120 BPM.
        auto e = make("delay");
        setParam(*e, stock::delay::sync, 1); setParam(*e, stock::delay::division, 4);
        setParam(*e, stock::delay::feedback, 0); setParam(*e, stock::delay::mix, 1); setParam(*e, stock::delay::tone, 20000);
        std::vector<float> l(30000, 0.0f), r(30000, 0.0f); l[0] = 1.0f; process(*e, l, r);
        near(l[24000], 1.0, 1e-5, "default tempo is 120 BPM");
    }
    // Limiter: ceiling holds, latency is reported and applied.
    {
        auto e = make("limiter"); setParam(*e, stock::limiter::ceiling, -6);
        check(e->getLatencySamples() == 95, "limiter reports 95 samples of latency at 48 kHz");
        auto l = noise(48000, 8.0, 5), r = noise(48000, 8.0, 6); process(*e, l, r, 211);
        check(peak(l, 0, 48000) <= dsp::dbToGain(-6) + 1e-6 && peak(r, 0, 48000) <= dsp::dbToGain(-6) + 1e-6, "limiter ceiling through the processor");
        check(dynamic_cast<stock::LimiterEffect&>(*e).gainReductionDb() > 10.0f, "limiter reports gain reduction");
        auto e44 = make("limiter", 44100); check(e44->getLatencySamples() == 87, "limiter latency follows the sample rate");
    }
    // Reverb.
    {
        auto e = make("reverb"); setParam(*e, stock::reverb::mix, 1); setParam(*e, stock::reverb::preDelay, 20); setParam(*e, stock::reverb::width, 1);
        std::vector<float> l(48000, 0.0f), r(48000, 0.0f); l[0] = 1.0f; process(*e, l, r, 128);
        check(peak(l, 0, 960) < 1e-9 && peak(r, 0, 960) < 1e-9, "nothing comes out before the pre-delay");
        check(peak(l, 960, 4000) > 1e-4 || peak(r, 960, 4000) > 1e-4, "the reverb sounds after the pre-delay");
        // The comb filters are silent for their first ~1100 samples anyway, so the pre-delay shows as a shift of the onset.
        auto onset = [](const std::vector<float>& a, const std::vector<float>& b) { for (std::size_t i = 0; i < a.size(); ++i) if (std::abs(a[i]) > 1e-9f || std::abs(b[i]) > 1e-9f) return static_cast<int>(i); return -1; };
        auto dry0 = make("reverb"); setParam(*dry0, stock::reverb::mix, 1); setParam(*dry0, stock::reverb::preDelay, 0);
        std::vector<float> p0(24000, 0.0f), q0(24000, 0.0f); p0[0] = 1.0f; process(*dry0, p0, q0, 128);
        auto dry50 = make("reverb"); setParam(*dry50, stock::reverb::mix, 1); setParam(*dry50, stock::reverb::preDelay, 50);
        std::vector<float> p50(24000, 0.0f), q50(24000, 0.0f); p50[0] = 1.0f; process(*dry50, p50, q50, 128);
        const int shift = onset(p50, q50) - onset(p0, q0);
        check(onset(p0, q0) > 0 && onset(p50, q50) > 0, "the reverb produces sound with and without pre-delay");
        check(std::abs(shift - 2400) <= 2, "a 50 ms pre-delay delays the reverb by 2400 samples (shifted by " + std::to_string(shift) + ")");
        auto f = make("reverb"); setParam(*f, stock::reverb::mix, 1); setParam(*f, stock::reverb::width, 0); setParam(*f, stock::reverb::preDelay, 0);
        auto a = noise(8000, 0.4, 7), b = noise(8000, 0.4, 8); process(*f, a, b);
        double worst = 0; for (int i = 0; i < 8000; ++i) worst = std::max(worst, static_cast<double>(std::abs(a[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)])));
        check(worst < 1e-6, "width 0 is mono");
    }
}

static void reverbLevelAndTailTests() {
    // At 100% wet, steady noise comes out at about the level it went in.
    {
        auto e = make("reverb"); setParam(*e, stock::reverb::mix, 1); setParam(*e, stock::reverb::preDelay, 0);
        auto l = noise(96000, 0.3, 21), r = noise(96000, 0.3, 22); const auto l0 = l, r0 = r; process(*e, l, r);
        double in = 0, out = 0; for (int i = 48000; i < 96000; ++i) { in += l0[static_cast<std::size_t>(i)] * l0[static_cast<std::size_t>(i)] + r0[static_cast<std::size_t>(i)] * r0[static_cast<std::size_t>(i)]; out += l[static_cast<std::size_t>(i)] * l[static_cast<std::size_t>(i)] + r[static_cast<std::size_t>(i)] * r[static_cast<std::size_t>(i)]; }
        near(10 * std::log10(out / in), 0.0, 4.0, "wet reverb level is comparable to the input level");
    }
    // The reported tail covers the real decay (to -60 dB) without being absurdly long.
    for (double size : {0.0, 0.5, 1.0}) for (double damping : {0.0, 0.5}) {
        auto e = make("reverb"); setParam(*e, stock::reverb::mix, 1); setParam(*e, stock::reverb::size, size);
        setParam(*e, stock::reverb::damping, damping); setParam(*e, stock::reverb::preDelay, 0);
        const int total = 48000 * 40;
        std::vector<float> l(static_cast<std::size_t>(total), 0.0f), r = l; l[0] = 1.0f; process(*e, l, r, 512);
        // block RMS envelope
        const int win = 480; double top = 0; std::vector<double> env;
        for (int i = 0; i + win <= total; i += win) { double s = 0; for (int k = 0; k < win; ++k) s += l[static_cast<std::size_t>(i + k)] * l[static_cast<std::size_t>(i + k)] + r[static_cast<std::size_t>(i + k)] * r[static_cast<std::size_t>(i + k)]; env.push_back(std::sqrt(s / win)); top = std::max(top, env.back()); }
        double last = 0; for (std::size_t k = 0; k < env.size(); ++k) if (env[k] > top * 1e-3) last = (k + 1) * win / rate;
        const double tail = e->getTailLengthSeconds();
        check(last <= tail, "reverb tail covers the decay: size " + std::to_string(size) + " damping " + std::to_string(damping) + " decays by " + std::to_string(last) + " s, claims " + std::to_string(tail));
        check(tail <= std::max(2.0 * last, last + 0.5), "reverb tail is not absurdly long: size " + std::to_string(size) + " claims " + std::to_string(tail) + " vs " + std::to_string(last));
    }
}

static void reverbPreDelayTailTest() {
    auto a = make("reverb"), b = make("reverb");
    setParam(*a, stock::reverb::preDelay, 0); setParam(*b, stock::reverb::preDelay, 100);
    near(b->getTailLengthSeconds() - a->getTailLengthSeconds(), 0.1, 1e-4, "the reported reverb tail grows by the pre-delay");
    // ...and that is real: with 100 ms pre-delay the sound really does last 100 ms longer, and the claim still covers it.
    auto e = make("reverb"); setParam(*e, stock::reverb::mix, 1); setParam(*e, stock::reverb::size, 0.5); setParam(*e, stock::reverb::damping, 0.5); setParam(*e, stock::reverb::preDelay, 100);
    const int total = 48000 * 12;
    std::vector<float> l(static_cast<std::size_t>(total), 0.0f), r = l; l[0] = 1.0f; process(*e, l, r, 512);
    const int win = 480; double top = 0; std::vector<double> env;
    for (int i = 0; i + win <= total; i += win) { double s = 0; for (int k = 0; k < win; ++k) s += l[static_cast<std::size_t>(i + k)] * l[static_cast<std::size_t>(i + k)] + r[static_cast<std::size_t>(i + k)] * r[static_cast<std::size_t>(i + k)]; env.push_back(std::sqrt(s / win)); top = std::max(top, env.back()); }
    double last = 0; for (std::size_t k = 0; k < env.size(); ++k) if (env[k] > top * 1e-3) last = (k + 1) * win / rate;
    check(last <= e->getTailLengthSeconds(), "the reverb tail covers the decay including a 100 ms pre-delay (decays by " + std::to_string(last) + " s, claims " + std::to_string(e->getTailLengthSeconds()) + ")");
}

// ---------------------------------------------------------------------------
static void robustnessTests() {
    // Hostile input never poisons an effect; it recovers with clean input.
    for (const auto& info : stock::catalog) {
        auto e = make(info.id);
        for (const char* id : {"mix"}) if (e->parameters.getParameter(id)) setParam(*e, id, 1.0);
        if (std::string(info.id) == "delay") setParam(*e, stock::delay::feedback, 0.9);
        std::vector<float> l = noise(1024, 0.5, 4), r = noise(1024, 0.5, 5);
        l[10] = std::numeric_limits<float>::quiet_NaN(); r[20] = std::numeric_limits<float>::infinity();
        l[30] = -std::numeric_limits<float>::infinity(); r[40] = std::numeric_limits<float>::quiet_NaN();
        process(*e, l, r);
        check(allFinite(l) && allFinite(r), std::string(info.id) + ": NaN/Inf input gives finite output");
        std::vector<float> a(48000 * 2, 0.0f), b(48000 * 2, 0.0f);
        process(*e, a, b);
        check(allFinite(a) && allFinite(b), std::string(info.id) + ": stays finite afterwards");
    }
    // Random parameters never give NaN or runaway output.
    std::mt19937 gen(5); std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (const auto& info : stock::catalog) {
        bool ok = true; double biggest = 0;
        for (int round = 0; round < 60 && ok; ++round) {
            auto e = make(info.id, round % 2 ? 44100.0 : 96000.0);
            for (auto* p : e->getParameters()) p->setValueNotifyingHost(unit(gen));
            auto l = noise(6000, 2.0, static_cast<unsigned>(round)), r = noise(6000, 2.0, static_cast<unsigned>(round + 99));
            process(*e, l, r, 1 + static_cast<int>(unit(gen) * 600));
            ok = allFinite(l) && allFinite(r);
            biggest = std::max({biggest, peak(l, 0, 6000), peak(r, 0, 6000)});
            ok = ok && biggest < 1.0e5;
        }
        check(ok, std::string(info.id) + ": random parameters stay finite and bounded (largest " + std::to_string(biggest) + ")");
    }
    // Moving parameters from another thread while the audio thread runs.
    for (const auto& info : stock::catalog) {
        auto e = make(info.id);
        std::atomic<bool> stop{false}, bad{false};
        std::thread editor([&] {
            std::mt19937 g(1); std::uniform_real_distribution<float> u(0.0f, 1.0f);
            while (!stop.load()) for (auto* p : e->getParameters()) p->setValueNotifyingHost(u(g));
        });
        auto l = noise(512, 1.0, 1), r = noise(512, 1.0, 2);
        juce::MidiBuffer midi;
        for (int i = 0; i < 3000; ++i) {
            auto a = l, b = r; float* ptrs[]{a.data(), b.data()}; juce::AudioBuffer<float> view(ptrs, 2, 512);
            e->processBlock(view, midi);
            for (float v : a) if (!dsp::finite(v)) bad = true;
        }
        stop = true; editor.join();
        check(!bad.load(), std::string(info.id) + ": editing parameters during playback is safe");
    }
}

static void allocationTests() {
    for (const auto& info : stock::catalog) {
        auto e = make(info.id);
        FixedTempo tempo; e->setPlayHead(&tempo);
        if (std::string(info.id) == "delay") { setParam(*e, stock::delay::sync, 1); setParam(*e, stock::delay::pingPong, 1); }
        std::mt19937 gen(3); std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        auto l = noise(512, 0.8, 1), r = noise(512, 0.8, 2);
        juce::MidiBuffer midi;
        std::size_t used = 0;
        for (int i = 0; i < 400; ++i) {
            if (i % 10 == 0) for (auto* p : e->getParameters()) p->setValueNotifyingHost(unit(gen)); // edits happen off the audio thread
            auto a = l, b = r; float* ptrs[]{a.data(), b.data()}; juce::AudioBuffer<float> view(ptrs, 2, 512);
            const auto before = allocations;
            e->processBlock(view, midi);
            used += allocations - before;
        }
        check(used == 0, std::string(info.id) + ": processBlock allocated " + std::to_string(used) + " time(s)");
    }
}

// ---------------------------------------------------------------------------
static void stateTests() {
    std::mt19937 gen(11); std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (const auto& info : stock::catalog) {
        auto a = make(info.id);
        for (auto* p : a->getParameters()) p->setValueNotifyingHost(unit(gen));
        juce::MemoryBlock saved; a->getStateInformation(saved);
        check(saved.getSize() > 0, std::string(info.id) + ": state is written");
        auto b = make(info.id);
        b->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        bool same = true; // compare what the audio thread sees (switches and choices snap to their steps)
        for (auto* p : a->getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
                same = same && std::abs(*a->parameters.getRawParameterValue(ranged->paramID) - *b->parameters.getRawParameterValue(ranged->paramID)) < 1e-4f;
        check(same, std::string(info.id) + ": parameters survive a state round trip");
        auto l1 = noise(3000, 0.5, 1), r1 = noise(3000, 0.5, 2), l2 = l1, r2 = r1;
        process(*a, l1, r1); process(*b, l2, r2);
        double worst = 0; for (std::size_t i = 0; i < l1.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(l1[i] - l2[i])));
        check(worst < 1e-5, std::string(info.id) + ": restored effect sounds identical (" + std::to_string(worst) + ")");
        // Garbage, empty and foreign state must not crash or leave values out of range.
        auto c = make(info.id);
        const std::vector<float> before = [&] { std::vector<float> v; for (auto* p : c->getParameters()) v.push_back(p->getValue()); return v; }();
        std::vector<unsigned char> junk(500); for (auto& byte : junk) byte = static_cast<unsigned char>(unit(gen) * 255);
        c->setStateInformation(junk.data(), static_cast<int>(junk.size()));
        c->setStateInformation(junk.data(), 0);
        c->setStateInformation(nullptr, 0);
        auto other = make(std::string(info.id) == "eq" ? "delay" : "eq");
        juce::MemoryBlock foreign; other->getStateInformation(foreign);
        c->setStateInformation(foreign.getData(), static_cast<int>(foreign.getSize()));
        bool unchanged = true; int i = 0;
        for (auto* p : c->getParameters()) { unchanged = unchanged && p->getValue() == before[static_cast<std::size_t>(i++)]; }
        check(unchanged, std::string(info.id) + ": junk or another effect's state is ignored");
    }
    // A state with only some parameters leaves the rest valid.
    {
        auto e = make("compressor");
        juce::XmlElement root(e->parameters.state.getType().toString());
        auto* p = root.createNewChildElement("PARAM"); p->setAttribute("id", stock::comp::threshold); p->setAttribute("value", -30.0);
        juce::MemoryBlock block; juce::AudioProcessor::copyXmlToBinary(root, block);
        e->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        near(getParam(*e, stock::comp::threshold), -30.0, 1e-4, "partial state sets the parameters it names");
        check(getParam(*e, stock::comp::ratio) >= 1.0 && getParam(*e, stock::comp::ratio) <= 20.0, "...and leaves the others in range");
    }
    // Values outside the range in a saved file are clamped.
    {
        auto e = make("limiter");
        juce::XmlElement root(e->parameters.state.getType().toString());
        auto* p = root.createNewChildElement("PARAM"); p->setAttribute("id", stock::limiter::ceiling); p->setAttribute("value", 900.0);
        juce::MemoryBlock block; juce::AudioProcessor::copyXmlToBinary(root, block);
        e->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        check(getParam(*e, stock::limiter::ceiling) <= 0.0, "an out-of-range saved value cannot raise the ceiling above 0 dBFS");
    }
}

// ---------------------------------------------------------------------------
static PluginState stockState(const char* id, bool bypass = false) {
    for (const auto& info : stock::catalog)
        if (std::string(info.id) == id) return {stock::describe(info).createXml()->toString(), {}, bypass};
    return {};
}

static void hostTests() {
    // instantiatePlugin
    {
        juce::String error;
        auto plugin = instantiatePlugin(stockState("compressor"), false, 48000, error);
        check(plugin && error.isEmpty() && plugin->getName() == "Alien Compressor", "a stock effect is created from its rack entry");
        check(!instantiatePlugin(stockState("eq"), true, 48000, error) && error.isNotEmpty(), "stock effects are refused in the instrument slot");
        auto unknown = stockState("eq");
        unknown.descriptionXml = unknown.descriptionXml.replace("\"eq\"", "\"warp-drive\"");
        check(!instantiatePlugin(unknown, false, 48000, error) && error.contains("Unknown stock effect"), "an unknown stock effect is reported, not substituted");
        auto badState = stockState("eq"); badState.stateBase64 = "!!!not base64!!!";
        check(!instantiatePlugin(badState, false, 48000, error) && error.isNotEmpty(), "corrupt saved state is reported");
        juce::PluginDescription vst; vst.name = "x"; vst.pluginFormatName = "VST3"; vst.fileOrIdentifier = "/missing.vst3";
        check(!instantiatePlugin({vst.createXml()->toString(), {}, false}, false, 48000, error) && error.isNotEmpty(), "a missing VST3 still fails cleanly");
    }
    // An effect this build does not know stays in the project untouched and does not stop the rest of the rack.
    {
        auto runtime = std::make_unique<PluginRuntime>();
        runtime->prepare(48000);
        PluginHost host(*runtime);
        std::vector<PluginRackState> racks(1); PluginRackState master;
        auto future = stockState("eq"); future.descriptionXml = future.descriptionXml.replace("\"eq\"", "\"warp-drive\""); future.stateBase64 = "YWJj";
        racks[0].effects = {future, stockState("limiter")};
        const auto errors = host.restore(racks, master, 48000);
        check(errors.contains("Unknown stock effect"), "an unknown stock effect is reported when the rack loads");
        check(runtime->racks[0].effectCount == 2 && !runtime->racks[0].effects[0].processor && runtime->racks[0].effects[1].processor, "...and the rest of the rack still loads");
        host.snapshot(racks, master);
        check(racks[0].effects[0] == future, "...its saved description and state are kept for a build that knows it");
        runtime->begin(512, 120, true, 0);
        for (int i = 0; i < 512; ++i) { runtime->tracks[0].setSample(0, i, 0.25f); runtime->tracks[0].setSample(1, i, 0.25f); }
        runtime->effectsAndAlign(512); // the limiter after the placeholder delays the track by its 95 samples
        near(runtime->tracks[0].getSample(0, 400), 0.25, 1e-6, "audio passes through a placeholder slot");
    }
    // A rack with stock effects restores, edits are noticed, and state survives snapshot and restore.
    {
        auto runtime = std::make_unique<PluginRuntime>();
        runtime->prepare(48000);
        PluginHost host(*runtime);
        std::vector<PluginRackState> racks(2); PluginRackState master;
        racks[0].effects = {stockState("eq"), stockState("compressor"), stockState("limiter")};
        master.effects = {stockState("reverb")};
        check(host.restore(racks, master, 48000).isEmpty(), "stock effects restore without errors");
        check(runtime->racks[0].effectCount == 3 && runtime->racks[0].effects[1].processor && runtime->racks[PluginRuntime::masterIndex].effectCount == 1, "effects are placed in the racks");
        check(runtime->rackLatency(runtime->racks[0]) == 95, "rack latency includes the limiter");
        host.changed.store(false);
        auto* comp = dynamic_cast<stock::CompressorEffect*>(runtime->racks[0].effects[1].processor.get());
        setParam(*comp, stock::comp::threshold, -33);
        check(host.changed.load(), "editing a stock effect marks the project as changed");
        host.snapshot(racks, master);
        check(racks[0].effects[1].stateBase64.isNotEmpty() && racks[0].effects[1].descriptionXml == stockState("compressor").descriptionXml, "snapshot keeps the identity and stores parameters");
        PluginHost second(*runtime);
        check(second.restore(racks, master, 48000).isEmpty(), "restoring from the snapshot works");
        near(getParam(dynamic_cast<stock::Effect&>(*runtime->racks[0].effects[1].processor), stock::comp::threshold), -33.0, 1e-3, "parameter values survive snapshot and restore");
        // Project file round trip, including bypass.
        SessionProjectData project; project.plugins[0] = racks[0]; project.plugins[0].effects[0].bypass = true; project.masterPlugins = master;
        juce::String error; SessionProjectData parsed;
        check(parseSessionProject(serializeSessionProject(project), parsed, error) && parsed.plugins[0] == project.plugins[0] && parsed.masterPlugins == master, "stock effects survive the project file");
    }
    // Processing through the runtime: order, bypass, master.
    {
        auto runtime = std::make_unique<PluginRuntime>();
        runtime->prepare(48000);
        PluginHost host(*runtime);
        std::vector<PluginRackState> racks(1); PluginRackState master;
        racks[0].effects = {stockState("eq")};
        check(host.restore(racks, master, 48000).isEmpty(), "restore");
        auto* eq = dynamic_cast<stock::Effect*>(runtime->racks[0].effects[0].processor.get());
        setParam(*eq, stock::eq::output, -6.0206);
        runtime->begin(64, 120, true, 0);
        for (int i = 0; i < 64; ++i) { runtime->tracks[0].setSample(0, i, 0.5f); runtime->tracks[0].setSample(1, i, 0.5f); }
        runtime->effectsAndAlign(64);
        near(runtime->tracks[0].getSample(0, 63), 0.25, 2e-3, "an EQ in a track rack processes that track");
        runtime->racks[0].effects[0].bypass = true;
        runtime->begin(64, 120, true, 0);
        for (int i = 0; i < 64; ++i) { runtime->tracks[0].setSample(0, i, 0.5f); runtime->tracks[0].setSample(1, i, 0.5f); }
        runtime->effectsAndAlign(64);
        check(runtime->tracks[0].getSample(0, 63) == 0.5f, "bypass leaves the audio untouched");
    }
    // Latency compensation across tracks.
    {
        auto runtime = std::make_unique<PluginRuntime>();
        runtime->prepare(48000);
        PluginHost host(*runtime);
        std::vector<PluginRackState> racks(2); PluginRackState master;
        racks[0].effects = {stockState("limiter")};
        check(host.restore(racks, master, 48000).isEmpty(), "restore");
        runtime->begin(512, 120, true, 0);
        for (int t = 0; t < 2; ++t) { runtime->tracks[static_cast<std::size_t>(t)].setSample(0, 0, 0.5f); runtime->tracks[static_cast<std::size_t>(t)].setSample(1, 0, 0.5f); }
        runtime->effectsAndAlign(512);
        near(runtime->tracks[0].getSample(0, 95), 0.5, 1e-6, "the limited track arrives 95 samples late...");
        near(runtime->tracks[1].getSample(0, 95), 0.5, 1e-6, "...and the other track is delayed to match");
        near(runtime->tracks[1].getSample(0, 0), 0.0, 1e-9, "nothing arrives early");
    }
}

// ---------------------------------------------------------------------------
static bool imageHasDetail(const juce::Image& image) {
    if (image.isNull() || image.getWidth() < 50 || image.getHeight() < 50) return false;
    std::set<juce::uint32> colours;
    for (int y = 0; y < image.getHeight(); y += 3) for (int x = 0; x < image.getWidth(); x += 3) colours.insert(image.getPixelAt(x, y).getARGB());
    return colours.size() > 8;
}

static void editorTests() {
    ui::StudioLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const char* snapshotDirectory = std::getenv("ALIEN_SNAPSHOT_DIR");
    for (const auto& info : stock::catalog) {
        auto e = make(info.id);
        std::unique_ptr<juce::AudioProcessorEditor> editor(e->createEditorIfNeeded());
        check(editor != nullptr && editor->getWidth() >= 300 && editor->getHeight() >= 200, std::string(info.id) + ": editor opens at a sensible size");
        if (!editor) continue;
        auto* base = dynamic_cast<stock::EffectEditor*>(editor.get());
        check(base != nullptr && base->knobCount() >= 3, std::string(info.id) + ": editor has knobs");
        if (!base) continue;
        // Every knob drives its parameter, and the parameter drives the knob.
        for (std::size_t k = 0; k < base->knobCount(); ++k) {
            auto& slider = base->knob(k).slider;
            const double mid = 0.5 * (slider.getMinimum() + slider.getMaximum());
            slider.setValue(mid, juce::sendNotificationSync);
            check(std::abs(slider.getValue() - mid) < (slider.getMaximum() - slider.getMinimum()) * 0.02, std::string(info.id) + ": knob holds its value");
        }
        for (std::size_t k = 0; k < base->knobCount(); ++k) { // every control has been laid out inside the window
            const auto bounds = base->knob(k).getBounds();
            check(bounds.getWidth() >= 40 && bounds.getHeight() >= 60 && editor->getLocalBounds().contains(bounds), std::string(info.id) + ": knob " + std::to_string(k) + " is laid out inside the editor");
        }
        for (int step = 0; step < 4; ++step) { // the editor repaints without trouble while the sound runs
            auto l = noise(512, 0.5, static_cast<unsigned>(step)), r = l; process(*e, l, r); base->repaint();
        }
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
        check(imageHasDetail(image), std::string(info.id) + ": editor draws real content");
        if (snapshotDirectory) {
            juce::File(snapshotDirectory).createDirectory();
            juce::File file = juce::File(snapshotDirectory).getChildFile(juce::String(info.id) + ".png");
            juce::FileOutputStream out(file); out.setPosition(0); out.truncate();
            juce::PNGImageFormat().writeImageToStream(image, out);
        }
    }
    // Moving a knob moves the parameter.
    {
        auto e = make("compressor");
        std::unique_ptr<juce::AudioProcessorEditor> editor(e->createEditorIfNeeded());
        auto* base = dynamic_cast<stock::EffectEditor*>(editor.get());
        auto& threshold = base->knob(0).slider;
        threshold.setValue(-40.0, juce::sendNotificationSync);
        near(getParam(*e, stock::comp::threshold), -40.0, 0.05, "turning the threshold knob sets the parameter");
        setParam(*e, stock::comp::threshold, -12.0);
        near(threshold.getValue(), -12.0, 0.05, "changing the parameter (automation, undo) moves the knob");
        near(threshold.getDoubleClickReturnValue(), -18.0, 0.01, "double-clicking a knob returns it to the default");
    }
    // EQ curve: coordinates invert, the curve matches the filters, and moving a handle edits the bands.
    {
        for (double hz : {20.0, 100.0, 1000.0, 20000.0}) near(stock::eqmap::xToFrequency(stock::eqmap::frequencyToX(hz, 600.0f), 600.0f), hz, hz * 1e-4, "frequency to x and back");
        for (double db : {-18.0, -5.0, 0.0, 7.5, 18.0}) near(stock::eqmap::yToGain(stock::eqmap::gainToY(db, 200.0f), 200.0f), db, 1e-4, "gain to y and back");
        auto e = make("eq");
        std::unique_ptr<juce::AudioProcessorEditor> editor(e->createEditorIfNeeded());
        auto* eqEditor = dynamic_cast<stock::EqEditor*>(editor.get());
        check(eqEditor != nullptr, "EQ editor type");
        auto& curve = eqEditor->response();
        near(curve.responseDb(1000.0), 0.0, 1e-9, "a flat EQ draws a flat curve");
        curve.moveBand(1, 2000.0, 9.0);
        near(getParam(*e, stock::eq::m1Freq), 2000.0, 1.0, "dragging a handle sets the band frequency");
        near(getParam(*e, stock::eq::m1Gain), 9.0, 0.01, "...and its gain");
        near(curve.responseDb(2000.0), 9.0, 0.3, "the curve follows the band");
        curve.moveBand(0, 10.0, 99.0); // beyond the limits: clamped
        check(getParam(*e, stock::eq::lowFreq) >= 20.0 && getParam(*e, stock::eq::lowGain) <= 18.0, "dragging beyond the edge is clamped");
    }
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    catalogueTests();
    parameterTests();
    processingTests();
    reverbLevelAndTailTests();
    reverbPreDelayTailTest();
    robustnessTests();
    allocationTests();
    stateTests();
    hostTests();
    editorTests();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("PASS: stock effects (parameters, state, racks, latency compensation, real-time safety, editors)");
    return 0;
}
