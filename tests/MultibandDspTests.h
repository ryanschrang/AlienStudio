#pragma once
static void multibandTests() {
    using M = MultibandCompressor;
    const auto energy = [](const std::vector<float>& x) { double sum = 0; for (std::size_t i = x.size() / 2; i < x.size(); ++i) sum += x[i] * x[i]; return sum; };
    const auto render = [](const std::vector<float>& input, M::Settings settings) {
        M m; m.prepare(rate); auto l = input, r = input; m.process(l.data(), r.data(), static_cast<int>(l.size()), settings); return l;
    };
    const auto signal = noise(24000, .8);
    M::Settings unity; unity.ratio = 1;
    check(render(signal, unity) == signal, "multiband unity gains recombine exactly, including transients");
    auto dry = unity; dry.ratio = 20; dry.thresholdDb = {-60, -60, -60}; dry.mix = 0; dry.outputDb = 12;
    check(render(signal, dry) == signal, "multiband mix zero is exact dry including wet output trim");
    for (int band = 0; band < 3; ++band) {
        M::Settings selected; selected.thresholdDb = {0, 0, 0}; selected.thresholdDb[band] = -36; selected.ratio = 10;
        const auto target = sine(std::array<double, 3>{50, 800, 10000}[band], .4, 24000);
        check(energy(render(target, selected)) < energy(target) * .55, "selected multiband threshold reduces its frequency region");
        const auto other = sine(band == 0 ? 10000 : 50, .4, 24000);
        check(energy(render(other, selected)) > energy(other) * .8, "independent multiband threshold largely preserves a distant band");
    }
    M m; m.prepare(rate); auto l = sine(700, .4, 24000), r = l;
    for (auto& v : r) v *= .25f;
    m.process(l.data(), r.data(), 24000, {});
    bool linked = true; for (std::size_t i = 0; i < l.size(); ++i) linked = linked && std::abs(r[i] - l[i] * .25f) < 1e-7;
    check(linked, "multiband stereo-linked detectors preserve left/right amplitude ratios");
    m.reset(); l.assign(24000, 0); r = sine(700, .8, 24000); m.process(l.data(), r.data(), 24000, {});
    check(energy(l) == 0, "multiband processing never crossfeeds stereo channels");
    const auto automated = [&](int block) {
        M engine; engine.prepare(rate); auto a = signal, b = signal;
        for (int at = 0; at < 24000;) {
            const int n = std::min({block, 24000 - at, at < 12000 ? 12000 - at : 24000 - at});
            M::Settings settings; if (at >= 12000) { settings.thresholdDb = {-40, -32, -24}; settings.ratio = 12; settings.mix = .4; settings.outputDb = 4; settings.attackMs = 3; settings.releaseMs = 80; }
            engine.process(a.data() + at, b.data() + at, n, settings); at += n;
        }
        return a;
    };
    const auto reference = automated(512);
    for (int block : {1, 37, 1024}) check(automated(block) == reference, "multiband automation and smoothing independent of block boundaries");
    m.reset(); l = signal; r = signal; m.process(l.data(), r.data(), 24000, {}); const auto first = l;
    m.reset(); l = signal; r = signal; m.process(l.data(), r.data(), 24000, {});
    check(l == first, "multiband reset clears filters, detectors and smoothed settings");
    for (double sr : {8000., 44100., 96000., 192000.}) {
        M engine; engine.prepare(sr); const int count = static_cast<int>(sr * .2);
        std::vector<float> a(count), b(count); a[0] = 1;
        engine.process(a.data(), b.data(), count, {{-60, -60, -60}, 20, .1, 5, 0, 1});
        check(std::all_of(a.begin(), a.end(), [](float v) { return finite(v); })
            && peakOf(a, static_cast<int>(sr * M::tailSeconds), count) < 1e-6 && energy(b) == 0,
            "multiband split stable and tail bounded at supported sample rates");
    }
    m.prepare(rate); l = sine(800, .8, 96000); r = l;
    M::Settings compression; compression.thresholdDb = {-30, -30, -30}; compression.attackMs = 20;
    m.process(l.data(), r.data(), 24000, compression);
    check(peakOf(l, 0, 480) > peakOf(l, 23000, 24000) * 2, "multiband attack preserves the initial transient before sustained reduction");
    check(m.reductionDb(1) > 10, "multiband detector develops sustained gain reduction");
    std::fill(l.begin(), l.end(), 0); std::fill(r.begin(), r.end(), 0);
    m.process(l.data(), r.data(), 96000, compression);
    check(m.reductionDb(1) < .001, "multiband gain recovers after release on silence");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    m.prepare(nan); l = {0, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), .5f}; r = l;
    m.process(l.data(), r.data(), 4, {{nan, nan, nan}, nan, nan, nan, nan, nan});
    check(std::all_of(l.begin(), l.end(), [](float v) { return finite(v); }), "multiband sanitises invalid samples, rates and controls");
    m.prepare(rate); l = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}; r = l;
    m.process(l.data(), r.data(), 2, {{0, 0, 0}, 1, 10, 120, 12, 1});
    check(std::all_of(l.begin(), l.end(), [](float v) { return finite(v); }), "multiband extreme finite input cannot overflow the float output");
}
