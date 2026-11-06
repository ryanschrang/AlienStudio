#pragma once
#include "ConvolutionReverb.h"
static void convolutionTests() {
    using C = alien::dsp::ConvolutionReverb;
    // Sparse direct convolution is independent of the FFT implementation. Hits
    // straddle partitions and run beyond the history ring to test wrap/overlap.
    for (double sr : {8000., 44100., 48000., 96000., 192000.}) {
        C engine; engine.prepare(sr);
        const int count = static_cast<int>(sr * .8) + C::latency;
        std::vector<float> l(count), r(count);
        const std::array<int, 5> times{0, 511, 512, 1031, static_cast<int>(sr * .35)};
        const std::array<double, 5> gains{1, -.3, .2, .1, -.4};
        for (int n = 0; n < 5; ++n) { l[times[n]] = static_cast<float>(gains[n]); r[times[n]] = static_cast<float>(gains[n] * .5); }
        engine.process(l.data(), r.data(), count, {10000, 6, .6});
        const double cutoff = std::min(10000., sr * .45), coefficient = 1 - std::exp(-2 * pi * cutoff / sr);
        for (int c = 0; c < 2; ++c) {
            const auto& ir = engine.impulseResponse(c); const auto& output = c ? r : l;
            double state = 0, worst = 0;
            for (int i = 0; i < count; ++i) {
                double ideal = 0, dry = 0;
                for (int n = 0; n < 5; ++n) {
                    const int frame = i - C::latency - times[n];
                    if (frame >= 0 && frame < static_cast<int>(ir.size())) ideal += ir[frame] * static_cast<double>(static_cast<float>(gains[n] * (c ? .5 : 1)));
                    if (frame == 0) dry += static_cast<double>(static_cast<float>(gains[n] * (c ? .5 : 1)));
                }
                state += coefficient * (ideal - state);
                worst = std::max(worst, std::abs(output[i] - (.4 * dry + .6 * state * dbToGain(6))));
            }
            check(worst < 1e-7, "FFT convolution matches independent direct FIR, tone, output gain and aligned mix at supported sample rates");
        }
        check(peakOf(l, 0, C::latency) == 0, "convolution wet path respects reported latency");
        check(peakOf(l, count - static_cast<int>(sr * .05), count) < 1e-7, "convolution finite response and tone tail finish within reported bound");
        engine.reset();
        std::fill(l.begin(), l.end(), 0); std::fill(r.begin(), r.end(), 0);
        engine.process(l.data(), r.data(), count, {10000, 0, 1});
        check(peakOf(l, 0, count) < 1e-12 && peakOf(r, 0, count) < 1e-12, "convolution reset clears spectra, overlap and delays");
    }
    const auto input = noise(24000, .7);
    const auto render = [&](int block, bool automate) {
        C engine; engine.prepare(rate); auto l = input, r = input;
        for (int at = 0; at < static_cast<int>(l.size());) {
            const int end = at < 12000 ? 12000 : static_cast<int>(l.size());
            const int n = std::min(block, end - at);
            engine.process(l.data() + at, r.data() + at, n, automate && at >= 12000 ? C::Settings{2000, -6, .7} : C::Settings{10000, 0, .25}); at += n;
        }
        return std::pair{l, r};
    };
    const auto reference = render(512, true);
    for (int block : {1, 37, 1024}) check(render(block, true) == reference, "convolution partition clock, automation and smoothing ignore host block boundaries");
    C engine; engine.prepare(rate);
    auto l = input, r = input; engine.process(l.data(), r.data(), 24000, {500, 12, 0});
    bool dry = true; for (int i = 0; i < 24000; ++i) dry = dry && l[i] == (i < C::latency ? 0.f : input[i - C::latency]);
    check(dry, "convolution zero mix is exact latency-aligned dry despite wet output trim");
    engine.reset(); l.assign(24000, 0); r = input; engine.process(l.data(), r.data(), 24000, {10000, 0, 1});
    check(peakOf(l, 0, 24000) == 0, "dual-mono convolution has no channel crossfeed");
    engine.reset(); l = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 0}; r = l;
    const double nan = std::numeric_limits<double>::quiet_NaN(); engine.process(l.data(), r.data(), 3, {nan, nan, nan});
    check(std::all_of(l.begin(), l.end(), [](float v) { return finite(v); }), "convolution rejects nonfinite samples and sanitises controls");
}
