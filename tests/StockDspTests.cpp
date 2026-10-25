// Exact-maths tests for the stock effect kernels. No JUCE: these run in seconds.
#include "StockDsp.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace alien::dsp;

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

static std::vector<float> sine(double frequency, double amplitude, int count, double sampleRate = rate) {
    std::vector<float> x(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) x[static_cast<std::size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * pi * frequency * i / sampleRate));
    return x;
}
static std::vector<float> noise(int count, double amplitude, unsigned seed = 1) {
    std::mt19937 gen(seed); std::uniform_real_distribution<double> d(-1.0, 1.0);
    std::vector<float> x(static_cast<std::size_t>(count));
    for (auto& v : x) v = static_cast<float>(d(gen) * amplitude);
    return x;
}
static double peakOf(const std::vector<float>& x, int from, int to) {
    double p = 0; for (int i = from; i < to; ++i) p = std::max(p, static_cast<double>(std::abs(x[static_cast<std::size_t>(i)]))); return p;
}

// ---------------------------------------------------------------------------
static void smootherTests() {
    Smoother s; s.configure(rate, 10.0); s.reset(0.0); s.setTarget(1.0);
    near(s.advance(480), 1.0 - std::exp(-1.0), 1.0e-9, "smoother reaches 63.2% after one time constant");
    for (int i = 0; i < 100; ++i) s.advance(4800);
    check(s.settled() && s.value() == 1.0, "smoother snaps exactly to its target");
    Smoother t; t.configure(rate, 10.0); t.reset(5.0); t.setTarget(5.0);
    check(t.advance(1) == 5.0 && t.settled(), "a settled smoother does not move");
    // 25 time constants leave a remainder of about 1e-11: tiny but not zero, so only the snap can make it exact.
    Smoother u; u.configure(rate, 10.0); u.reset(0.0); u.setTarget(1.0);
    u.advance(480 * 25);
    check(u.settled() && u.value() == 1.0, "a smoother snaps to its target once the remainder is inaudible, long before it underflows");
    Smoother v; v.configure(rate, 10.0); v.reset(0.0); v.setTarget(1.0);
    v.advance(480 * 10);
    check(!v.settled() && v.value() < 1.0, "...but not while the remainder is still audible (10 time constants)");
    Smoother w; w.configure(rate, 10.0); w.reset(0.0); w.setTarget(-2000.0);
    w.advance(480 * 25);
    check(w.settled() && w.value() == -2000.0, "the snap tolerance scales with the target, so large values and negatives settle too");
}

static void biquadTests() {
    // Peaking bell: exactly the requested gain at its centre, flat far away.
    for (double gain : {12.0, 6.0, -9.0}) {
        const auto c = designBiquad(BiquadKind::peak, 1000, gain, 1.0, rate);
        near(biquadMagnitudeDb(c, 1000, rate), gain, 0.01, "peak gain at centre " + std::to_string(gain));
        check(std::abs(biquadMagnitudeDb(c, 20, rate)) < 0.5 && std::abs(biquadMagnitudeDb(c, 20000, rate)) < 1.0, "peak flat away from centre");
    }
    // Higher Q is narrower.
    const auto wide = designBiquad(BiquadKind::peak, 1000, 12, 0.5, rate), narrow = designBiquad(BiquadKind::peak, 1000, 12, 5.0, rate);
    check(biquadMagnitudeDb(wide, 1500, rate) > biquadMagnitudeDb(narrow, 1500, rate) + 3.0, "Q narrows the bell");
    // Shelves reach the gain on their own side and are flat on the other.
    const auto low = designBiquad(BiquadKind::lowShelf, 200, 6, ParametricEq::shelfQ, rate);
    near(biquadMagnitudeDb(low, 20, rate), 6.0, 0.15, "low shelf boost at 20 Hz");
    near(biquadMagnitudeDb(low, 10000, rate), 0.0, 0.1, "low shelf flat at 10 kHz");
    const auto high = designBiquad(BiquadKind::highShelf, 5000, -8, ParametricEq::shelfQ, rate);
    near(biquadMagnitudeDb(high, 20000, rate), -8.0, 0.5, "high shelf cut at 20 kHz");
    near(biquadMagnitudeDb(high, 100, rate), 0.0, 0.1, "high shelf flat at 100 Hz");
    // At the corner a shelf sits at half its gain (in dB).
    near(biquadMagnitudeDb(designBiquad(BiquadKind::lowShelf, 500, 10, ParametricEq::shelfQ, rate), 500, rate), 5.0, 0.1, "shelf midpoint");
    // No gain is an exact identity, whatever frequency/Q.
    check(designBiquad(BiquadKind::peak, 1234, 0.0, 3.0, rate) == BiquadCoefficients{}, "zero-gain band is the identity");
    // Frequencies beyond Nyquist stay finite and stable.
    const auto extreme = designBiquad(BiquadKind::highShelf, 1.0e6, 12, 1.0, 44100.0);
    check(std::isfinite(extreme.b0) && std::isfinite(extreme.a1) && std::abs(extreme.a2) < 1.0, "extreme frequency is clamped");
    // The time-domain filter agrees with the analytic response.
    const auto bell = designBiquad(BiquadKind::peak, 2000, 9, 1.5, rate);
    for (double f : {500.0, 2000.0, 4500.0}) {
        BiquadState st; double peak = 0;
        const auto x = sine(f, 1.0, 20000);
        for (int i = 0; i < 20000; ++i) { const double y = st.process(bell, x[static_cast<std::size_t>(i)]); if (i > 10000) peak = std::max(peak, std::abs(y)); }
        near(20.0 * std::log10(peak), biquadMagnitudeDb(bell, f, rate), 0.05, "filter matches its response at " + std::to_string(f));
    }
}

static ParametricEq::Settings flatEq() { return {}; }

static void eqTests() {
    ParametricEq eq; eq.prepare(rate);
    // Flat is bit-exact pass-through.
    auto l = noise(5000, 0.8, 3), r = noise(5000, 0.8, 4); const auto l0 = l, r0 = r;
    eq.process(l.data(), r.data(), 5000, flatEq());
    check(l == l0 && r == r0, "a flat EQ leaves audio bit-identical");
    // +12 dB bell at 1 kHz.
    ParametricEq::Settings bell; bell.band[1] = {1000, 12, 1.0};
    eq.prepare(rate);
    auto tone = sine(1000, 0.1, 24000), tone2 = tone;
    eq.process(tone.data(), tone2.data(), 24000, bell);
    near(20 * std::log10(peakOf(tone, 12000, 24000) / 0.1), 12.0, 0.1, "EQ boosts the centre frequency by the set gain");
    eq.prepare(rate);
    auto low = sine(100, 0.1, 24000), low2 = low;
    eq.process(low.data(), low2.data(), 24000, bell);
    near(20 * std::log10(peakOf(low, 12000, 24000) / 0.1), biquadMagnitudeDb(designBiquad(BiquadKind::peak, 1000, 12, 1.0, rate), 100, rate), 0.1, "EQ leaves distant frequencies near flat");
    // Output trim.
    ParametricEq::Settings trim; trim.outputDb = -6.0;
    eq.prepare(rate);
    auto a = sine(440, 0.5, 4800), b = a;
    eq.process(a.data(), b.data(), 4800, trim);
    near(peakOf(a, 2400, 4800), 0.5 * dbToGain(-6.0), 1.0e-4, "output trim");
    // Channels are independent.
    eq.prepare(rate);
    auto left = sine(1000, 0.3, 4800), right = std::vector<float>(4800, 0.0f);
    eq.process(left.data(), right.data(), 4800, bell);
    check(peakOf(right, 0, 4800) == 0.0, "EQ does not leak left into right");
    // Splitting into odd blocks changes nothing while settings are constant.
    ParametricEq::Settings busy; busy.band[0] = {120, 5, 1}; busy.band[1] = {700, -7, 2}; busy.band[2] = {3000, 6, 0.8}; busy.band[3] = {9000, -4, 1}; busy.outputDb = 1.5;
    const auto in = noise(6000, 0.5, 8);
    auto wholeL = in, wholeR = in, partL = in, partR = in;
    eq.prepare(rate); eq.process(wholeL.data(), wholeR.data(), 6000, busy);
    eq.prepare(rate);
    for (int pos = 0, size = 1; pos < 6000; pos += size, size = size * 3 % 211 + 1) {
        const int n = std::min(size, 6000 - pos); eq.process(partL.data() + pos, partR.data() + pos, n, busy);
    }
    double worst = 0; for (int i = 0; i < 6000; ++i) worst = std::max(worst, static_cast<double>(std::abs(wholeL[static_cast<std::size_t>(i)] - partL[static_cast<std::size_t>(i)])));
    check(worst < 1.0e-6, "EQ output does not depend on block size (worst " + std::to_string(worst) + ")");
    // A sudden +12 dB change is smoothed: no click.
    eq.prepare(rate);
    auto s1 = sine(1000, 0.2, 9600), s2 = s1;
    eq.process(s1.data(), s2.data(), 4800, flatEq());
    eq.process(s1.data() + 4800, s2.data() + 4800, 4800, bell);
    double step = 0; for (int i = 4801; i < 9600; ++i) step = std::max(step, static_cast<double>(std::abs(s1[static_cast<std::size_t>(i)] - s1[static_cast<std::size_t>(i - 1)])));
    check(step < 2.0 * 0.2 * 4.0 * 2 * pi * 1000 / rate, "a gain change does not click (largest step " + std::to_string(step) + ")");
    // Settles on the final response.
    check(peakOf(s1, 8400, 9600) > 0.2 * 3.8, "EQ reaches the new gain after the change");
}

// ---------------------------------------------------------------------------
static void compressorCurveTests() {
    near(compressorReductionDb(-30, -18, 4, 0), 0, 1e-12, "no reduction below threshold");
    near(compressorReductionDb(-18, -18, 4, 0), 0, 1e-12, "no reduction exactly at threshold (hard knee)");
    near(compressorReductionDb(-8, -18, 4, 0), 7.5, 1e-9, "10 dB over at 4:1 is reduced by 7.5 dB");
    near(compressorReductionDb(-8, -18, 20, 0), 9.5, 1e-9, "10 dB over at 20:1 is reduced by 9.5 dB");
    near(compressorReductionDb(-8, -18, 1, 0), 0, 1e-12, "1:1 never reduces");
    // Soft knee: half the knee below threshold it just starts; at threshold it is (1-1/R)*knee/8.
    near(compressorReductionDb(-21, -18, 4, 6), 0, 1e-12, "knee starts half its width below the threshold");
    near(compressorReductionDb(-18, -18, 4, 6), 0.75 * 6.0 / 8.0, 1e-9, "knee value at the threshold");
    near(compressorReductionDb(-15, -18, 4, 6), 0.75 * 3.0, 1e-9, "knee joins the straight line at its upper edge");
    for (double over = -4; over < 4; over += 0.01) // continuous and monotonic through the knee
        check(compressorReductionDb(-18 + over + 0.01, -18, 4, 6) >= compressorReductionDb(-18 + over, -18, 4, 6) - 1e-12, "curve is monotonic");
}

static double steadyOutputDb(double inputDb, const Compressor::Settings& s, double frequency = 1000) {
    Compressor c; c.prepare(rate);
    const int count = static_cast<int>(rate * 3);
    auto l = sine(frequency, dbToGain(inputDb), count), r = l;
    c.process(l.data(), r.data(), count, s);
    return gainToDb(peakOf(l, count - 9600, count));
}

static void compressorTests() {
    Compressor::Settings hard; hard.thresholdDb = -18; hard.ratio = 4; hard.kneeDb = 0; hard.attackMs = 5; hard.releaseMs = 50;
    near(steadyOutputDb(-6, hard), -18 + 12.0 / 4.0, 0.35, "12 dB over at 4:1 comes out 3 dB over");
    near(steadyOutputDb(-6, {-18, 10, 5, 50, 0, 0, 1}), -18 + 12.0 / 10.0, 0.35, "10:1");
    near(steadyOutputDb(-30, hard), -30, 0.01, "below the threshold the level is untouched");
    near(steadyOutputDb(-30, {-18, 4, 5, 50, 0, 6, 1}), -24, 0.01, "makeup gain adds");
    near(steadyOutputDb(-6, {-18, 4, 5, 50, 0, 0, 0}), -6, 0.01, "mix 0 is the dry signal");
    {
        const double wet = steadyOutputDb(-6, {-18, 4, 5, 50, 0, 0, 1});
        const double half = steadyOutputDb(-6, {-18, 4, 5, 50, 0, 0, 0.5});
        check(half > wet + 0.3 && half < -6.0 - 0.3, "parallel mix lies between wet and dry");
    }
    // Below threshold the code path is an exact copy.
    {
        Compressor c; c.prepare(rate);
        auto l = noise(4000, 0.05, 11), r = noise(4000, 0.05, 12); const auto l0 = l, r0 = r;
        c.process(l.data(), r.data(), 4000, {-18, 4, 10, 100, 6, 0, 0.6});
        check(l == l0 && r == r0, "a quiet signal passes through bit-exactly");
    }
    // Attack: reduction reaches 63.2% of its target after one attack time constant.
    {
        Compressor c; c.prepare(rate);
        const int total = static_cast<int>(rate * 0.2), attackSamples = static_cast<int>(rate * 0.02);
        std::vector<float> l(static_cast<std::size_t>(total), 0.5f), r = l;
        // measure gain per sample by feeding DC and watching the output
        c.process(l.data(), r.data(), total, {-30, 10, 20, 500, 0, 0, 1});
        const double target = compressorReductionDb(gainToDb(0.5), -30, 10, 0);
        const double atAttack = -gainToDb(l[static_cast<std::size_t>(attackSamples)] / 0.5);
        near(atAttack / target, 1.0 - std::exp(-1.0), 0.02, "attack time constant");
        near(-gainToDb(l[static_cast<std::size_t>(total - 1)] / 0.5) / target, 1.0, 0.01, "attack settles at the full reduction");
    }
    // Release: after the signal stops, half the reduction is gone in about ln(2) * release (+ detector fall).
    {
        Compressor c; c.prepare(rate);
        const int loud = static_cast<int>(rate * 1.0), quiet = static_cast<int>(rate * 2.0);
        std::vector<float> l(static_cast<std::size_t>(loud + quiet), 0.0f);
        for (int i = 0; i < loud; ++i) l[static_cast<std::size_t>(i)] = 0.5f;
        for (int i = loud; i < loud + quiet; ++i) l[static_cast<std::size_t>(i)] = 0.001f; // tiny signal to read the gain
        auto r = l;
        c.process(l.data(), r.data(), loud + quiet, {-30, 10, 5, 500, 0, 0, 1});
        const double start = compressorReductionDb(gainToDb(0.5), -30, 10, 0);
        int half = -1;
        for (int i = loud; i < loud + quiet; ++i) if (-gainToDb(l[static_cast<std::size_t>(i)] / 0.001) < start / 2) { half = i - loud; break; }
        check(half > 0, "reduction does release");
        const double seconds = half / rate;
        check(seconds > 0.33 && seconds < 0.46, "release time constant (half-way after " + std::to_string(seconds) + " s)");
    }
    // Linked detection: a loud left channel also pulls down the right one equally.
    {
        Compressor c; c.prepare(rate);
        std::vector<float> l(24000, 0.5f), r(24000, 0.01f);
        c.process(l.data(), r.data(), 24000, {-30, 10, 5, 100, 0, 0, 1});
        near(static_cast<double>(r[23999]) / 0.01, static_cast<double>(l[23999]) / 0.5, 1.0e-6, "both channels receive the same gain");
        check(l[23999] / 0.5 < 0.5, "the loud channel is reduced");
    }
    {   // ...and the same when the loud channel is the right one.
        Compressor c; c.prepare(rate);
        std::vector<float> l(24000, 0.01f), r(24000, 0.5f);
        c.process(l.data(), r.data(), 24000, {-30, 10, 5, 100, 0, 0, 1});
        near(static_cast<double>(l[23999]) / 0.01, static_cast<double>(r[23999]) / 0.5, 1.0e-6, "both channels receive the same gain (loud right channel)");
        check(r[23999] / 0.5 < 0.5, "a loud right channel is reduced too");
    }
    // The meter reports reduction.
    {
        Compressor c; c.prepare(rate);
        std::vector<float> l(24000, 0.5f), r = l;
        c.process(l.data(), r.data(), 24000, {-30, 10, 5, 100, 0, 0, 1});
        near(c.lastBlockReductionDb(), compressorReductionDb(gainToDb(0.5), -30, 10, 0), 0.05, "meter shows the reduction");
    }
    // Extremes stay finite.
    for (double ratio : {1.0, 20.0}) for (double attack : {0.1, 200.0}) for (double release : {5.0, 2000.0}) for (double knee : {0.0, 24.0}) {
        Compressor c; c.prepare(rate);
        auto l = noise(9600, 4.0, 5), r = noise(9600, 4.0, 6);
        c.process(l.data(), r.data(), 9600, {-60, ratio, attack, release, knee, 24, 1});
        bool ok = true; for (int i = 0; i < 9600; ++i) ok = ok && std::isfinite(l[static_cast<std::size_t>(i)]) && std::isfinite(r[static_cast<std::size_t>(i)]);
        check(ok, "compressor stays finite at extreme settings");
    }
}

// ---------------------------------------------------------------------------
static void delayLineTests() {
    FractionalDelayLine line; line.prepare(100);
    for (int i = 1; i <= 250; ++i) line.push(static_cast<float>(i));
    for (int k : {2, 3, 10, 57, 100}) near(line.read(k), 250 - k + 1, 1e-4, "integer read " + std::to_string(k));
    near(line.read(10.5), 250 - 10.5 + 1, 1e-3, "a ramp interpolates exactly between samples");
    near(line.read(1000), line.read(100), 0, "reads beyond the buffer are clamped");
}

static void delayTests() {
    const int d = 480; // 10 ms
    StereoDelay::Settings s; s.timeMs = 10; s.feedback = 0; s.toneHz = 20000; s.mix = 1; s.pingPong = false;
    {
        StereoDelay delay; delay.prepare(rate);
        std::vector<float> l(2000, 0.0f), r(2000, 0.0f); l[0] = 1.0f;
        delay.process(l.data(), r.data(), 2000, s);
        near(l[d], 1.0, 1e-6, "echo lands exactly on the delay time");
        double elsewhere = 0; for (int i = 0; i < 2000; ++i) if (i != d) elsewhere = std::max(elsewhere, static_cast<double>(std::abs(l[static_cast<std::size_t>(i)])));
        check(elsewhere < 1e-6, "no other output without feedback");
        check(peakOf(r, 0, 2000) == 0.0, "the right channel stays silent");
    }
    s.feedback = 0.5;
    {
        StereoDelay delay; delay.prepare(rate);
        std::vector<float> l(4000, 0.0f), r(4000, 0.0f); l[0] = 1.0f;
        delay.process(l.data(), r.data(), 4000, s);
        for (int k = 1; k <= 7; ++k) near(l[static_cast<std::size_t>(d * k)], std::pow(0.5, k - 1), 1e-5, "feedback echo " + std::to_string(k));
    }
    {   // Ping-pong: the echoes alternate sides and the mono sum enters the left line.
        StereoDelay::Settings pp = s; pp.pingPong = true;
        StereoDelay delay; delay.prepare(rate);
        std::vector<float> l(4000, 0.0f), r(4000, 0.0f); l[0] = 1.0f;
        delay.process(l.data(), r.data(), 4000, pp);
        near(l[d], 0.5, 1e-6, "ping-pong first echo is on the left"); near(r[d], 0.0, 1e-6, "...and not on the right");
        near(r[2 * d], 0.25, 1e-6, "second echo is on the right"); near(l[2 * d], 0.0, 1e-6, "...not on the left");
        near(l[3 * d], 0.125, 1e-6, "third echo is back on the left");
    }
    {   // Mix.
        StereoDelay::Settings m = s; m.feedback = 0; m.mix = 0.25;
        StereoDelay delay; delay.prepare(rate);
        std::vector<float> l(1000, 0.0f), r(1000, 0.0f); l[0] = 1.0f;
        delay.process(l.data(), r.data(), 1000, m);
        near(l[0], 0.75, 1e-6, "dry part of the mix"); near(l[d], 0.25, 1e-6, "wet part of the mix");
    }
    {   // Tone filter darkens repeats; wide open leaves them alone.
        auto energy = [&](double toneHz) {
            StereoDelay::Settings t = s; t.feedback = 0.9; t.toneHz = toneHz; t.mix = 1;
            StereoDelay delay; delay.prepare(rate);
            auto l = sine(10000, 0.5, 9600), r = l; delay.process(l.data(), r.data(), 9600, t);
            double sum = 0; for (int i = 4800; i < 9600; ++i) sum += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)];
            return sum;
        };
        check(energy(1000) < 0.1 * energy(20000), "tone control removes high frequencies from the repeats");
    }
    {   // A fractional delay is a true time shift of a low tone.
        StereoDelay::Settings f = s; f.timeMs = 10.3; f.feedback = 0; f.mix = 1;
        StereoDelay delay; delay.prepare(rate);
        auto l = sine(440, 0.8, 8000), r = l; delay.process(l.data(), r.data(), 8000, f);
        double worst = 0; const double D = 10.3 * 0.001 * rate;
        for (int i = 2000; i < 8000; ++i) worst = std::max(worst, std::abs(l[static_cast<std::size_t>(i)] - 0.8 * std::sin(2 * pi * 440 * (i - D) / rate)));
        check(worst < 2e-3, "fractional delay time shifts a sine accurately (error " + std::to_string(worst) + ")");
    }
    {   // Changing the time glides instead of jumping; the delay never reads out of range.
        StereoDelay delay; delay.prepare(rate);
        StereoDelay::Settings a = s; a.timeMs = 50; a.feedback = 0.7; a.mix = 0.5;
        auto l = noise(48000, 0.5, 9), r = noise(48000, 0.5, 10);
        delay.process(l.data(), r.data(), 24000, a);
        a.timeMs = 3000;
        delay.process(l.data() + 24000, r.data() + 24000, 24000, a);
        bool ok = true; for (int i = 0; i < 48000; ++i) ok = ok && std::isfinite(l[static_cast<std::size_t>(i)]) && std::abs(l[static_cast<std::size_t>(i)]) < 20.0f;
        check(ok, "delay stays finite and bounded while its time changes");
        StereoDelay::Settings huge = a; huge.timeMs = 1.0e6;
        delay.process(l.data(), r.data(), 1000, huge); // clamps, does not crash
    }
    near(syncedDelayMs(2, 120), 250, 1e-9, "1/8 at 120 BPM");
    near(syncedDelayMs(4, 120), 500, 1e-9, "1/4 at 120 BPM");
    near(syncedDelayMs(0, 120), 125, 1e-9, "1/16 at 120 BPM");
    near(syncedDelayMs(1, 90), 60000.0 / 90 / 3, 1e-9, "1/8 triplet at 90 BPM");
    near(syncedDelayMs(3, 120), 375, 1e-9, "dotted 1/8");
    near(syncedDelayMs(7, 120), 2000, 1e-9, "one bar at 120 BPM");
    near(syncedDelayMs(2, 0), syncedDelayMs(2, 20), 1e-9, "tempo is clamped");
    near(delayTailSeconds(500, 0.5), 0.5 * 11, 1e-9, "tail of a 500 ms, 50% delay");
    near(delayTailSeconds(500, 0), 0.5, 1e-9, "tail without feedback");
    near(delayTailSeconds(2000, 0.95), 30, 1e-9, "tail is capped");
}

// ---------------------------------------------------------------------------
static void slidingMinimumTests() {
    for (int window : {1, 2, 5, 96}) {
        SlidingMinimum m; m.prepare(window);
        std::mt19937 gen(static_cast<unsigned>(window)); std::uniform_real_distribution<double> d(0.0, 1.0);
        std::vector<double> history;
        bool ok = true;
        for (int i = 0; i < 3000; ++i) {
            const double v = d(gen); history.push_back(v);
            double expected = 1.0e9; for (int k = std::max(0, i - window + 1); k <= i; ++k) expected = std::min(expected, history[static_cast<std::size_t>(k)]);
            ok = ok && m.push(v) == expected;
        }
        check(ok, "sliding minimum equals brute force, window " + std::to_string(window));
    }
}

static std::vector<float> runLimiter(Limiter& lim, std::vector<float> l, std::vector<float> r, const Limiter::Settings& s, int block) {
    for (std::size_t pos = 0; pos < l.size(); pos += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), l.size() - pos));
        lim.process(l.data() + pos, r.data() + pos, n, s);
    }
    l.insert(l.end(), r.begin(), r.end()); // left then right, for comparison
    return l;
}

static void limiterTests() {
    Limiter probe; probe.prepare(48000);
    check(probe.latencySamples() == 95, "look-ahead of 2 ms at 48 kHz is 95 samples of latency");
    { Limiter p; p.prepare(44100); check(p.latencySamples() == 87, "look-ahead at 44.1 kHz"); }
    { Limiter p; p.prepare(96000); check(p.latencySamples() == 191, "look-ahead at 96 kHz"); }

    const double ceilingDb = -6.0, ceiling = dbToGain(ceilingDb);
    const Limiter::Settings s{0, ceilingDb, 100};
    struct Case { const char* name; std::vector<float> signal; };
    std::vector<Case> cases;
    cases.push_back({"loud noise", noise(48000, 10.0, 21)});
    { std::vector<float> step(48000, 0.0f); for (int i = 4000; i < 48000; ++i) step[static_cast<std::size_t>(i)] = 5.0f; cases.push_back({"DC step", step}); }
    { std::vector<float> alt(48000); for (int i = 0; i < 48000; ++i) alt[static_cast<std::size_t>(i)] = (i % 2 ? -3.0f : 3.0f); cases.push_back({"alternating full scale", alt}); }
    { std::vector<float> one(48000, 0.0f); one[10000] = 1000.0f; cases.push_back({"single huge impulse", one}); }
    { auto burst = sine(220, 0.05, 48000); for (int i = 20000; i < 20300; ++i) burst[static_cast<std::size_t>(i)] *= 200.0f; cases.push_back({"short loud burst", burst}); }
    { std::vector<float> ramp(48000); for (int i = 0; i < 48000; ++i) ramp[static_cast<std::size_t>(i)] = static_cast<float>(i) / 2000.0f; cases.push_back({"rising ramp", ramp}); }
    for (const auto& c : cases) {
        for (int block : {1, 7, 64, 513}) {
            Limiter lim; lim.prepare(48000);
            const auto out = runLimiter(lim, c.signal, c.signal, s, block);
            check(peakOf(out, 0, static_cast<int>(out.size())) <= ceiling + 1e-6, std::string("limiter never exceeds its ceiling: ") + c.name + ", block " + std::to_string(block));
        }
    }
    // Block size does not matter.
    {
        Limiter a, b; a.prepare(48000); b.prepare(48000);
        const auto sig = noise(30000, 3.0, 5);
        check(runLimiter(a, sig, sig, s, 1) == runLimiter(b, sig, sig, s, 511), "limiter output does not depend on block size");
    }
    // A quiet signal is only delayed.
    {
        Limiter lim; lim.prepare(48000);
        const auto x = noise(20000, 0.3, 31);
        auto l = x, r = x; lim.process(l.data(), r.data(), 20000, {0, -0.3, 100});
        bool same = true; for (int i = 95; i < 20000; ++i) same = same && l[static_cast<std::size_t>(i)] == x[static_cast<std::size_t>(i - 95)];
        check(same, "below the ceiling the signal is untouched, only delayed by the reported latency");
        check(peakOf(l, 0, 95) == 0.0, "the look-ahead pre-roll is silent");
    }
    // Input gain.
    {
        Limiter lim; lim.prepare(48000);
        auto l = sine(1000, 0.1, 9600), r = l;
        lim.process(l.data(), r.data(), 9600, {12, 0, 100});
        near(peakOf(l, 4800, 9600), 0.1 * dbToGain(12.0), 2e-4, "input gain");
    }
    // Sustained overload: the gain settles on ceiling/peak, and the meter shows it.
    {
        Limiter lim; lim.prepare(48000);
        std::vector<float> l(48000, 1.0f), r = l;
        lim.process(l.data(), r.data(), 48000, s);
        near(l[47999], ceiling, 1e-5, "sustained overload sits on the ceiling");
        std::vector<float> l2(4800, 1.0f), r2 = l2; lim.process(l2.data(), r2.data(), 4800, s);
        near(lim.lastBlockReductionDb(), -ceilingDb, 0.05, "meter shows the reduction");
    }
    // Release: gain returns to unity after a burst.
    {
        Limiter lim; lim.prepare(48000);
        std::vector<float> l(96000, 0.0f);
        for (int i = 0; i < 4800; ++i) l[static_cast<std::size_t>(i)] = 4.0f;
        for (int i = 4800; i < 96000; ++i) l[static_cast<std::size_t>(i)] = 0.1f;
        auto r = l; lim.process(l.data(), r.data(), 96000, {0, -6.0, 100});
        check(l[9700] < 0.1f * 0.9f, "gain is still recovering 0.1 s after the burst (release is gradual)");
        check(l[9700] > 0.1f * 0.4f, "...and is already coming back");
        near(l[95999], 0.1, 1e-4, "gain fully recovers after the release time");
    }
    // A ceiling above 0 dBFS is refused: the output still cannot pass full scale.
    {
        Limiter lim; lim.prepare(48000);
        auto l = noise(20000, 10.0, 41), r = noise(20000, 10.0, 42);
        lim.process(l.data(), r.data(), 20000, {0, 6.0, 100});
        check(peakOf(l, 0, 20000) <= 1.0 && peakOf(r, 0, 20000) <= 1.0, "a +6 dB ceiling is clamped to 0 dBFS");
        check(peakOf(l, 0, 20000) > 0.9, "...and the limiter still lets signal through up to full scale");
    }
    // Fuzz: random settings and levels never break the ceiling.
    {
        std::mt19937 gen(99); std::uniform_real_distribution<double> unit(0.0, 1.0);
        bool ok = true;
        for (int round = 0; round < 300 && ok; ++round) {
            Limiter::Settings f{unit(gen) * 24.0, -unit(gen) * 12.0, 1.0 + unit(gen) * 999.0};
            Limiter lim; lim.prepare(round % 3 == 0 ? 44100 : round % 3 == 1 ? 48000 : 96000);
            auto l = noise(3000, unit(gen) * 20.0, static_cast<unsigned>(round)), r = noise(3000, unit(gen) * 20.0, static_cast<unsigned>(round + 1000));
            for (int i = 0; i < 3000; i += 1 + static_cast<int>(unit(gen) * 40)) if (unit(gen) < 0.1) l[static_cast<std::size_t>(i)] = static_cast<float>(unit(gen) * 1000.0);
            lim.process(l.data(), r.data(), 3000, f);
            ok = peakOf(l, 0, 3000) <= dbToGain(f.ceilingDb) + 1e-6 && peakOf(r, 0, 3000) <= dbToGain(f.ceilingDb) + 1e-6;
        }
        check(ok, "limiter keeps its ceiling for random settings and levels");
    }
}

static void filterTests() {
    using Mode = FilterMode;
    const double fc = 1000.0;
    // Second-order shapes.
    const auto lp = designFilter(Mode::lowPass, fc, MultimodeFilter::butterworthQ, rate);
    near(biquadMagnitudeDb(lp, 1.0, rate), 0.0, 0.01, "low-pass passes DC at unity");
    near(biquadMagnitudeDb(lp, fc, rate), -3.0103, 0.02, "Butterworth low-pass is -3 dB at the cutoff");
    check(biquadMagnitudeDb(lp, 2.0 * fc, rate) < -11.5 && biquadMagnitudeDb(lp, 2.0 * fc, rate) > -13.0, "12 dB/oct one octave above the cutoff");
    const auto hp = designFilter(Mode::highPass, fc, MultimodeFilter::butterworthQ, rate);
    near(biquadMagnitudeDb(hp, 20000.0, rate), 0.0, 0.1, "high-pass passes highs at unity");
    near(biquadMagnitudeDb(hp, fc, rate), -3.0103, 0.02, "high-pass is -3 dB at the cutoff");
    check(biquadMagnitudeDb(hp, 20.0, rate) < -60.0, "high-pass removes 20 Hz");
    const auto bp = designFilter(Mode::bandPass, fc, 2.0, rate);
    near(biquadMagnitudeDb(bp, fc, rate), 0.0, 0.01, "band-pass is 0 dB at its centre");
    check(biquadMagnitudeDb(bp, fc / 8.0, rate) < -20.0 && biquadMagnitudeDb(bp, fc * 8.0, rate) < -20.0, "band-pass rejects both sides");
    const auto notch = designFilter(Mode::notch, fc, 2.0, rate);
    check(biquadMagnitudeDb(notch, fc, rate) < -60.0, "notch removes its centre");
    near(biquadMagnitudeDb(notch, fc / 10.0, rate), 0.0, 0.2, "notch leaves distant frequencies alone");
    // Resonance: a high-Q low-pass peaks by about the Q (in dB) near the cutoff.
    const auto resonant = designFilter(Mode::lowPass, fc, 4.0, rate);
    near(biquadMagnitudeDb(resonant, fc, rate), 20.0 * std::log10(4.0), 0.2, "resonance lifts the cutoff by the Q");

    // 24 dB/oct: default resonance is an exact fourth-order Butterworth.
    MultimodeFilter::Settings steep; steep.steep = true; steep.frequency = fc;
    const auto q = MultimodeFilter::stageQ(true, MultimodeFilter::butterworthQ);
    near(q[0], MultimodeFilter::fourthOrderQ[0], 1e-12, "first steep stage keeps the Butterworth Q");
    near(q[1], MultimodeFilter::fourthOrderQ[1], 1e-12, "second steep stage too, at the default resonance");
    near(MultimodeFilter::responseDb(steep, fc, rate), -3.0103, 0.02, "24 dB/oct Butterworth is -3 dB at the cutoff");
    const double octave = MultimodeFilter::responseDb(steep, 2.0 * fc, rate);
    check(octave < -23.0 && octave > -25.5, "24 dB/oct one octave above the cutoff (" + std::to_string(octave) + ")");
    MultimodeFilter::Settings gentle = steep; gentle.steep = false;
    check(MultimodeFilter::responseDb(steep, 8.0 * fc, rate) < MultimodeFilter::responseDb(gentle, 8.0 * fc, rate) - 30.0, "steep slope removes far more");

    auto steadyPeak = [](const std::vector<float>& x) { return peakOf(x, static_cast<int>(x.size()) / 2, static_cast<int>(x.size())); };
    // Processing: a low-pass keeps the bass and removes the treble.
    {
        MultimodeFilter f; f.prepare(rate);
        MultimodeFilter::Settings s; s.frequency = fc;
        auto low = sine(100.0, 0.5, 9600), lowR = low;
        f.process(low.data(), lowR.data(), 9600, s);
        near(steadyPeak(low), 0.5, 0.01, "100 Hz passes a 1 kHz low-pass");
        MultimodeFilter g; g.prepare(rate);
        auto high = sine(10000.0, 0.5, 9600), highR = high;
        g.process(high.data(), highR.data(), 9600, s);
        check(steadyPeak(high) < 0.5 * dbToGain(-35.0), "10 kHz is removed by a 1 kHz low-pass");
    }
    // Mix 0 is exact pass-through; output gain applies to the dry path too.
    {
        MultimodeFilter f; f.prepare(rate);
        MultimodeFilter::Settings s; s.mix = 0.0;
        const auto input = noise(4096, 0.7, 3);
        auto l = input, r = input;
        f.process(l.data(), r.data(), 4096, s);
        check(l == input && r == input, "mix 0 is bit-exact pass-through");
        MultimodeFilter g; g.prepare(rate);
        s.outputDb = 6.0;
        auto a = input, b = input;
        g.process(a.data(), b.data(), 4096, s);
        near(a[1000] / input[1000], dbToGain(6.0), 1e-5, "output level is applied");
    }
    // Switching mode fades out, switches with clean state and fades back in: no click.
    {
        MultimodeFilter f; f.prepare(rate);
        MultimodeFilter::Settings s; s.frequency = 5000.0;
        auto l = sine(100.0, 0.5, 24000), r = l;
        f.process(l.data(), r.data(), 12000, s);
        s.mode = Mode::highPass;
        f.process(l.data() + 12000, r.data() + 12000, 200, s);
        check(f.activeMode() == Mode::highPass, "the switch completes within a few milliseconds");
        f.process(l.data() + 12200, r.data() + 12200, 11800, s);
        double jump = 0;
        for (int i = 11000; i < 13000; ++i) jump = std::max(jump, static_cast<double>(std::abs(l[static_cast<std::size_t>(i)] - l[static_cast<std::size_t>(i - 1)])));
        check(jump < 0.02, "switching low-pass to high-pass does not click (largest step " + std::to_string(jump) + ")");
        check(peakOf(l, 18000, 24000) < 0.5 * dbToGain(-50.0), "after the switch the 100 Hz tone is removed by the high-pass");
        MultimodeFilter g; g.prepare(rate);
        MultimodeFilter::Settings t; t.frequency = 300.0;
        auto a = sine(100.0, 0.5, 24000), b = a;
        g.process(a.data(), b.data(), 12000, t);
        t.steep = true;
        g.process(a.data() + 12000, b.data() + 12000, 12000, t);
        jump = 0;
        for (int i = 11000; i < 13000; ++i) jump = std::max(jump, static_cast<double>(std::abs(a[static_cast<std::size_t>(i)] - a[static_cast<std::size_t>(i - 1)])));
        check(g.activeSteep() && jump < 0.02, "changing the slope does not click (largest step " + std::to_string(jump) + ")");
    }
    // Settled settings: the block size makes no difference.
    {
        MultimodeFilter::Settings s; s.mode = Mode::bandPass; s.frequency = 700.0; s.resonance = 3.0; s.mix = 0.8; s.outputDb = -2.0; s.steep = true;
        const auto input = noise(9000, 0.5, 9);
        std::vector<float> reference;
        for (int block : {9000, 512, 37, 1}) {
            MultimodeFilter f; f.prepare(rate);
            auto l = input, r = input;
            for (int pos = 0; pos < 9000; pos += block) { const int n = std::min(block, 9000 - pos); f.process(l.data() + pos, r.data() + pos, n, s); }
            if (reference.empty()) { reference = l; continue; }
            double worst = 0;
            for (std::size_t i = 0; i < l.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(l[i] - reference[i])));
            check(worst < 1e-6, "block size " + std::to_string(block) + " sounds the same (" + std::to_string(worst) + ")");
        }
    }
    // Moving the cutoff glides and stays finite, even sweeping hard at high resonance.
    {
        MultimodeFilter f; f.prepare(rate);
        MultimodeFilter::Settings s; s.resonance = 10.0; s.steep = true;
        auto l = noise(48000, 0.5, 4), r = l;
        bool finiteAll = true; double biggest = 0;
        for (int pos = 0, step = 0; pos < 48000; pos += 256, ++step) {
            s.frequency = step % 2 ? 20.0 : 18000.0;
            f.process(l.data() + pos, r.data() + pos, std::min(256, 48000 - pos), s);
        }
        for (float v : l) { finiteAll = finiteAll && finite(v); biggest = std::max(biggest, static_cast<double>(std::abs(v))); }
        check(finiteAll && biggest < 100.0, "hard cutoff sweeps at high resonance stay finite and bounded (" + std::to_string(biggest) + ")");
    }
    // Tail: more resonance and lower cutoff ring longer, and the reported tail covers the ringing.
    check(filterTailSeconds(200.0, 10.0, false) > filterTailSeconds(200.0, 0.7, false), "resonance lengthens the tail");
    check(filterTailSeconds(100.0, 5.0, false) > filterTailSeconds(5000.0, 5.0, false), "a lower cutoff rings longer");
    check(filterTailSeconds(20.0, 10.0, true) <= 5.0 && filterTailSeconds(20000.0, 0.5, false) >= 0.05, "tail stays within 50 ms and 5 s");
    {
        MultimodeFilter f; f.prepare(rate);
        MultimodeFilter::Settings s; s.frequency = 200.0; s.resonance = 10.0;
        const int tail = static_cast<int>(filterTailSeconds(200.0, 10.0, false) * rate);
        std::vector<float> l(static_cast<std::size_t>(tail + 4800), 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        f.process(l.data(), r.data(), static_cast<int>(l.size()), s);
        const double ring = peakOf(l, 0, 4800), after = peakOf(l, tail, tail + 4800);
        check(after < ring * 1.0e-3, "the ringing has fallen 60 dB by the end of the reported tail");
    }
}

int main() {
    smootherTests();
    biquadTests();
    eqTests();
    compressorCurveTests();
    compressorTests();
    delayLineTests();
    delayTests();
    slidingMinimumTests();
    limiterTests();
    filterTests();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("PASS: stock DSP (EQ, compressor, delay, limiter, filter)");
    return 0;
}
