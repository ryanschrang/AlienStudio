// Channel Rack playback: step clock, swing, voices, pitch, pan, probability,
// gate, nudge, ratchets, choke, mute/solo, routing, and real-time safety. No JUCE.
#include "RackEngine.h"
#include "RackModel.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

// Count heap allocations made on the calling thread, to prove the audio path never allocates.
static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace alien;
using namespace alien::rack;

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

constexpr double kCenter = 0.70710678118654752440;  // constant-power pan, centre

// ---- helpers ----------------------------------------------------------------
static std::shared_ptr<SessionAudioClip> makeClip(std::size_t frames, double rate, const std::function<float(std::size_t)>& left,
                                                  const std::function<float(std::size_t)>& right = {}) {
    auto clip = std::make_shared<SessionAudioClip>();
    clip->sampleRate = rate; clip->channels = right ? 2 : 1; clip->frames = frames;
    clip->left.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) clip->left[i] = left(i);
    if (right) { clip->right.resize(frames); for (std::size_t i = 0; i < frames; ++i) clip->right[i] = right(i); }
    clip->buildWaveform();
    return clip;
}
static std::shared_ptr<SessionAudioClip> dc(std::size_t frames, double rate = 48000.0, float value = 1.0f) {
    return makeClip(frames, rate, [value](std::size_t) { return value; });
}

struct Rig {
    Project project = Project::createDefault();
    std::shared_ptr<const Snapshot> snap;
    RackPlayer::SampleTable clips{};
    RackPlayer player;
    double rate{48000.0};
    std::vector<std::shared_ptr<SessionAudioClip>> keep;
    std::array<std::vector<float>, RackPlayer::busCount> left, right;

    explicit Rig(double sampleRate = 48000.0) : rate(sampleRate) { player.prepare(rate); }
    void setClip(int channel, std::shared_ptr<SessionAudioClip> clip) { keep.push_back(clip); clips[static_cast<std::size_t>(channel)].store(clip.get()); }
    void publish() { snap = project.snapshot(); }
    void on(int channel, int step, int velocity = 127) {
        project.setStepOn(project.selectedPattern, channel, step, true);
        project.setStepField(project.selectedPattern, channel, step, StepField::velocity, velocity);
    }
    void run(int samples, bool running, double bpm = 120.0, int activeTracks = 4, int block = 512) {
        if (!snap) publish();
        std::array<std::vector<float>, RackPlayer::busCount> bl, br;
        std::array<float*, RackPlayer::busCount> pl{}, pr{};
        for (int done = 0; done < samples;) {
            const int n = std::min(block, samples - done);
            for (std::size_t b = 0; b < bl.size(); ++b) { bl[b].assign(static_cast<std::size_t>(n), 0.0f); br[b].assign(static_cast<std::size_t>(n), 0.0f); pl[b] = bl[b].data(); pr[b] = br[b].data(); }
            player.process(*snap, clips, running, bpm, activeTracks, n, pl.data(), pr.data());
            for (std::size_t b = 0; b < bl.size(); ++b) { left[b].insert(left[b].end(), bl[b].begin(), bl[b].end()); right[b].insert(right[b].end(), br[b].begin(), br[b].end()); }
            done += n;
        }
    }
    const std::vector<float>& master() const { return left[RackPlayer::masterBus]; }
    std::size_t length() const { return master().size(); }
};

// Sample indices where the signal rises through the threshold.
static std::vector<std::size_t> onsets(const std::vector<float>& x, float threshold = 0.1f) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) >= threshold && (i == 0 || std::abs(x[i - 1]) < threshold)) result.push_back(i);
    return result;
}
static double peakOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double p = 0; for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, static_cast<double>(std::abs(x[i]))); return p;
}

// ---- clock ----------------------------------------------------------------------
static void clockTests() {
    {   // 120 BPM at 48 kHz: a sixteenth every 6000 samples, exactly.
        RackClock clock; std::vector<std::int64_t> fired;
        for (int i = 0; i < 6000 * 40; ++i) { const auto g = clock.next(48000.0, 120.0, 0.0); if (g >= 0) { fired.push_back(i); check(g == static_cast<std::int64_t>(fired.size()) - 1, "grid indices count up from 0"); } }
        check(fired.size() == 40, "forty steps in forty intervals");
        bool exact = true; for (std::size_t k = 0; k < fired.size(); ++k) exact = exact && fired[k] == static_cast<std::int64_t>(k) * 6000;
        check(exact, "steps fall on exact multiples of 6000 samples");
    }
    {   // A fractional interval stays exact over a long time (no drift).
        const double rate = 44100.0, bpm = 133.0, interval = rate * 60.0 / (bpm * 4.0);
        RackClock clock; std::int64_t count = 0; bool exact = true;
        const std::int64_t total = static_cast<std::int64_t>(interval * 20000);
        for (std::int64_t i = 0; i < total; ++i) {
            const auto g = clock.next(rate, bpm, 0.0);
            if (g >= 0) { exact = exact && std::abs(static_cast<double>(i) - std::ceil(static_cast<double>(count) * interval - 1e-6)) <= 1.0; ++count; }
        }
        check(exact, "a fractional interval never drifts by more than a sample");
        check(count == 20000 || count == 20001, "twenty thousand steps took twenty thousand intervals (" + std::to_string(count) + ")");
    }
    {   // Swing delays every odd step by swing x half an interval; even steps never move.
        for (double swing : {0.0, 0.25, 0.5, 1.0}) {
            RackClock clock; std::map<std::int64_t, std::int64_t> when;
            for (int i = 0; i < 6000 * 12; ++i) { const auto g = clock.next(48000.0, 120.0, swing); if (g >= 0) when[g] = i; }
            bool ok = when.size() >= 11;
            for (std::int64_t g = 0; g < 11; ++g) {
                const std::int64_t expected = g * 6000 + ((g & 1) ? static_cast<std::int64_t>(std::llround(swing * 3000.0)) : 0);
                ok = ok && when.count(g) && when[g] == expected;
            }
            check(ok, "swing " + std::to_string(swing) + " puts odd steps " + std::to_string(std::llround(swing * 3000.0)) + " samples late");
        }
    }
    {   // Hostile swing values are clamped.
        for (double swing : {-5.0, 7.0, std::nan(""), HUGE_VAL}) {
            RackClock clock; std::map<std::int64_t, std::int64_t> when;
            for (int i = 0; i < 6000 * 4; ++i) { const auto g = clock.next(48000.0, 120.0, swing); if (g >= 0) when[g] = i; }
            const auto late = when.count(1) ? when[1] - 6000 : -1;
            check(late >= 0 && late <= 3000 && when.size() >= 3, "swing " + std::to_string(swing) + " stays inside 0..100%");
        }
    }
    {   // Reset restarts at step 0 on the very next sample.
        RackClock clock; for (int i = 0; i < 10000; ++i) clock.next(48000.0, 120.0, 0.5);
        clock.reset(); check(clock.next(48000.0, 120.0, 0.5) == 0, "after reset the next sample starts step 0");
    }
    {   // A tempo change takes effect at the next step line.
        RackClock clock; std::vector<int> fired; int i = 0;
        for (; i < 6000 * 2; ++i) { if (clock.next(48000.0, 120.0, 0.0) >= 0) fired.push_back(i); }
        for (; i < 6000 * 2 + 3000 * 4; ++i) { if (clock.next(48000.0, 240.0, 0.0) >= 0) fired.push_back(i); }
        check(fired.size() >= 5 && fired[0] == 0 && fired[1] == 6000 && fired[2] == 12000 && fired[3] == 15000 && fired[4] == 18000, "doubling the tempo halves the spacing from the next step");
    }
    {   // Extreme tempi and rates: always increasing, never stuck, and held to 1..1000 BPM.
        // A tempo that is not a number plays at 120 BPM.
        struct Case { double given, effective; };
        const Case cases[] = {{1.0, 1.0}, {20.0, 20.0}, {999.0, 999.0}, {5000.0, 1000.0}, {-3.0, 1.0}, {0.0, 1.0}, {0.5, 1.0},
                              {std::nan(""), 120.0}, {HUGE_VAL, 120.0}, {-HUGE_VAL, 120.0}};
        for (double rate : {8000.0, 44100.0, 192000.0}) for (const auto& tempo : cases) {
            RackClock clock; std::int64_t last = -1; bool ok = true; std::int64_t count = 0;
            const int total = 300000;
            for (int i = 0; i < total; ++i) { const auto g = clock.next(rate, tempo.given, 0.7); if (g >= 0) { ok = ok && g == last + 1; last = g; ++count; } }
            const double interval = rate * 60.0 / (tempo.effective * 4.0);
            // Swing delays the odd lines, so the last one may still be pending: allow one step either way.
            const auto expected = static_cast<std::int64_t>(std::ceil(total / interval));
            check(ok && count >= expected - 1 && count <= expected,
                  "clock runs at " + std::to_string(tempo.effective) + " BPM when given " + std::to_string(tempo.given) + " at "
                      + std::to_string(rate) + " Hz (" + std::to_string(count) + " steps, expected about " + std::to_string(expected) + ")");
        }
        // The same holds for the player: it keeps time however the host reports its tempo.
        for (const auto& [bpm, hits] : std::vector<std::pair<double, std::size_t>>{{std::nan(""), 4}, {HUGE_VAL, 4}, {0.0, 1}, {-50.0, 1}}) {
            Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 1); rig.on(0, 2); rig.on(0, 3);
            rig.run(48000 * 2, true, bpm);     // 120 BPM gives all four steps in two seconds; 1 BPM only the first
            check(onsets(rig.master()).size() == hits, "the player keeps time when the tempo is " + std::to_string(bpm)
                                                         + " (" + std::to_string(onsets(rig.master()).size()) + " hits, expected " + std::to_string(hits) + ")");
        }
    }
    {   // Reset throws away a step that was waiting out its swing delay.
        RackClock clock;
        int i = 0;
        for (; i < 6000 + 1000; ++i) clock.next(48000.0, 120.0, 1.0);   // step 2 (index 1) is now pending, 2000 samples to go
        clock.reset();
        std::vector<std::pair<int, std::int64_t>> fired;
        for (int n = 0; n < 6000 * 4; ++n) { const auto g = clock.next(48000.0, 120.0, 1.0); if (g >= 0) fired.push_back({n, g}); }
        const std::vector<std::pair<int, std::int64_t>> expected{{0, 0}, {6000 + 3000, 1}, {12000, 2}, {18000 + 3000, 3}};
        check(fired == expected, "after a reset the old swung step is gone and the grid starts clean (" + std::to_string(fired.size()) + " steps)");
    }
}

// ---- player -------------------------------------------------------------------
static void timingTests() {
    {   // 120 BPM: step 0 and step 4 of a 16-step pattern, every 96000 samples.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 4);
        rig.run(48000 * 3, true);
        const auto hits = onsets(rig.master());
        check(hits.size() == 4 && hits[0] == 0 && hits[1] == 24000 && hits[2] == 96000 && hits[3] == 120000, "kick on steps 1 and 5 loops every bar (got " + std::to_string(hits.size()) + " hits)");
    }
    {   // 133 BPM at 44.1 kHz: the bar length follows the tempo to the sample.
        Rig rig(44100.0); rig.setClip(0, dc(10, 44100.0)); rig.on(0, 0);
        rig.run(static_cast<int>(44100 * 14), true, 133.0);
        const double bar = 44100.0 * 60.0 / (133.0 * 4.0) * 16.0;
        const auto hits = onsets(rig.master());
        bool ok = hits.size() >= 6;
        for (std::size_t k = 0; k < hits.size(); ++k) ok = ok && std::abs(static_cast<double>(hits[k]) - std::ceil(static_cast<double>(k) * bar - 1e-6)) <= 1.0;
        check(ok, "bars land where the tempo says they should");
    }
    {   // Not running: silence, and no step reported.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0);
        rig.run(48000, false);
        check(onsets(rig.master()).empty() && rig.player.currentStep() == -1, "a stopped rack is silent");
    }
    {   // Stopping and starting again restarts from step 0.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0);
        rig.run(20000, true); rig.run(5000, false); rig.run(1000, true);
        const auto hits = onsets(rig.master());
        check(hits.size() == 2 && hits[0] == 0 && hits[1] == 25000, "play restarts the pattern from the top");
    }
    {   // Swing through the player.
        for (double swing : {0.0, 0.5, 1.0}) {
            Rig rig; rig.project.setSwing(static_cast<float>(swing)); rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 1);
            rig.run(48000, true);
            const auto hits = onsets(rig.master());
            check(hits.size() >= 2 && hits[0] == 0 && hits[1] == static_cast<std::size_t>(6000 + std::llround(swing * 3000.0)),
                  "swing " + std::to_string(swing) + " delays step 2 (at " + std::to_string(hits.size() > 1 ? hits[1] : 0) + ")");
        }
    }
    {   // Pattern lengths.
        Rig rig; rig.project.setPatternLength(0, 32); rig.setClip(0, dc(10)); rig.on(0, 17);
        rig.run(6000 * 90, true);
        const auto hits = onsets(rig.master());
        check(hits.size() == 3 && hits[0] == 6000u * 17 && hits[1] == 6000u * (17 + 32) && hits[2] == 6000u * (17 + 64), "a 32-step pattern loops every 32 steps");
        Rig rig64; rig64.project.setPatternLength(0, 64); rig64.setClip(0, dc(10)); rig64.on(0, 63);
        rig64.run(6000 * 130, true);
        const auto h64 = onsets(rig64.master());
        check(h64.size() == 2 && h64[0] == 6000u * 63 && h64[1] == 6000u * (63 + 64), "a 64-step pattern loops every 64 steps");
        Rig beyond; beyond.setClip(0, dc(10)); beyond.on(0, 20);
        beyond.run(48000 * 4, true);
        check(onsets(beyond.master()).empty(), "steps past the pattern length do not play");
        Rig shrink; shrink.project.setPatternLength(0, 64); shrink.setClip(0, dc(10)); shrink.on(0, 40);
        shrink.project.setPatternLength(0, 16); shrink.project.setPatternLength(0, 64);
        shrink.run(6000 * 41, true);
        check(onsets(shrink.master()).size() == 1, "steps survive shortening and lengthening the pattern");
    }
    {   // The step display follows the pattern.
        Rig rig; rig.project.setPatternLength(0, 32); rig.setClip(0, dc(10)); rig.on(0, 0);
        rig.run(6000 * 20 + 10, true);
        check(rig.player.currentStep() == 20, "the playhead is on step 21 after 20 steps");
        rig.run(6000 * 15, true);
        check(rig.player.currentStep() == 3, "...and wraps at the pattern length");
    }
    {   // Switching to another pattern while playing takes effect at the next step.
        Rig rig; rig.project.addPattern(); rig.project.selectPattern(0);
        rig.setClip(0, dc(10)); rig.setClip(1, makeClip(10, 48000.0, [](std::size_t) { return 0.5f; }));
        rig.project.setStepOn(0, 0, 0, true); rig.project.setStepOn(0, 0, 4, true); rig.project.setStepOn(1, 1, 4, true);
        rig.publish(); rig.run(6000 * 2, true);
        rig.project.selectPattern(1); rig.publish(); rig.run(6000 * 4, true);
        const auto hits = onsets(rig.master(), 0.05f);
        check(hits.size() == 2 && hits[0] == 0 && hits[1] == 24000, "pattern 1 played step 1, then pattern 2's snare came in on step 5 (" + std::to_string(hits.size()) + " hits)");
        near(peakOf(rig.master(), 24000, 24010), 0.5 * (100.0 / 127.0) * kCenter, 1e-4, "...at the other channel's level");
    }
}

static void voiceTests() {
    {   // Level: velocity scales linearly, centre pan is -3 dB, channel volume 0.8 is unity.
        Rig rig; rig.setClip(0, dc(100)); rig.on(0, 0, 127);
        rig.run(200, true);
        near(rig.master()[5], kCenter, 1e-5, "full velocity at the centre sounds 0.7071 in each ear");
        near(rig.right[RackPlayer::masterBus][5], kCenter, 1e-5, "...in both channels");
        Rig half; half.setClip(0, dc(100)); half.on(0, 0, 64);
        half.run(200, true);
        near(half.master()[5], kCenter * 64.0 / 127.0, 1e-5, "velocity 64 is about half level");
    }
    {   // Channel pan and step pan.
        Rig rig; rig.setClip(0, dc(100)); rig.on(0, 0);
        auto c = rig.project.channels[0]; c.pan = -1.0f; rig.project.setChannel(0, c);
        rig.publish();
        for (int i = 0; i < 20000; ++i) { /* let the pan glide settle before the first hit */ }
        rig.run(48000, false);   // settle (control smoothing runs while stopped)
        rig.left[RackPlayer::masterBus].clear(); rig.right[RackPlayer::masterBus].clear();
        rig.run(200, true);
        near(rig.master()[5], 1.0, 1e-3, "hard left: the left side gets full level");
        near(rig.right[RackPlayer::masterBus][5], 0.0, 1e-3, "...and the right side nothing");
        Rig step; step.setClip(0, dc(100)); step.on(0, 0);
        step.project.setStepField(0, 0, 0, StepField::pan, 63);
        step.run(200, true);
        near(step.master()[5], 0.0, 1e-3, "step pan +63: nothing on the left");
        near(step.right[RackPlayer::masterBus][5], 1.0, 1e-3, "...everything on the right");
        Rig both; both.setClip(0, dc(100)); both.on(0, 0);
        auto bc = both.project.channels[0]; bc.pan = 0.5f; both.project.setChannel(0, bc); both.publish();
        both.run(48000, false);
        both.left[RackPlayer::masterBus].clear(); both.right[RackPlayer::masterBus].clear();
        both.project.setStepField(0, 0, 0, StepField::pan, 63); both.publish();   // +1 on top of +0.5 clamps at hard right
        both.run(200, true);
        near(both.right[RackPlayer::masterBus][5], 1.0, 1e-3, "channel pan and step pan add, clamped at the edge");
    }
    {   // Channel volume: unity at 0.8, and the fader glides.
        Rig rig; rig.project.setPatternLength(0, 64); rig.setClip(0, dc(48000 * 4)); rig.on(0, 0);
        rig.run(48000, true);   // one second at unity gain
        near(rig.master()[40000], kCenter, 1e-4, "volume 0.8 is unity gain");
        auto c = rig.project.channels[0]; c.volume = 0.4f; rig.project.setChannel(0, c); rig.publish();
        const std::size_t changeAt = rig.length();
        rig.run(48000, true);
        near(rig.master()[changeAt + 40000], kCenter * 0.5, 1e-3, "volume 0.4 is half gain once settled");
        double largestStep = 0; for (std::size_t i = changeAt; i < changeAt + 20000; ++i) largestStep = std::max(largestStep, static_cast<double>(std::abs(rig.master()[i] - rig.master()[i - 1])));
        check(largestStep < 0.01, "the fader glides rather than jumping (largest step " + std::to_string(largestStep) + ")");
        auto d = rig.project.channels[0]; d.volume = 1.0f; rig.project.setChannel(0, d); rig.publish();
        const std::size_t topAt = rig.length();
        rig.run(48000, true);
        near(rig.master()[topAt + 40000], kCenter * 1.25, 2e-3, "volume 1.0 is +1.9 dB");
    }
    {   // Pitch: a ramp read at twice, half and 1.5 octaves' speed.
        auto ramp = [](double rate) { return makeClip(4000, rate, [](std::size_t i) { return static_cast<float>(i + 4) * 0.01f; }); };
        for (auto [semitones, speed] : std::vector<std::pair<int, double>>{{12, 2.0}, {-12, 0.5}, {0, 1.0}, {-24, 0.25}, {24, 4.0}}) {
            Rig rig; rig.setClip(0, ramp(48000.0)); rig.on(0, 0);
            rig.project.setStepField(0, 0, 0, StepField::pitch, semitones);
            rig.run(400, true);
            bool ok = true;
            for (int n = 20; n < 300; ++n) ok = ok && std::abs(rig.master()[static_cast<std::size_t>(n)] - kCenter * (n * speed + 4) * 0.01) < 2e-3;
            check(ok, std::to_string(semitones) + " semitones reads the sample at " + std::to_string(speed) + "x speed");
        }
        // A sample recorded at another rate plays at the right speed.
        Rig slow; slow.setClip(0, ramp(24000.0)); slow.on(0, 0); slow.run(400, true);
        bool ok = true; for (int n = 20; n < 300; ++n) ok = ok && std::abs(slow.master()[static_cast<std::size_t>(n)] - kCenter * (n * 0.5 + 4) * 0.01) < 2e-3;
        check(ok, "a 24 kHz sample on a 48 kHz engine plays at half speed (no pitch change)");
        Rig fast(96000.0); fast.setClip(0, ramp(48000.0)); fast.on(0, 0); fast.run(400, true);
        ok = true; for (int n = 20; n < 300; ++n) ok = ok && std::abs(fast.master()[static_cast<std::size_t>(n)] - kCenter * (n * 0.5 + 4) * 0.01) < 2e-3;
        check(ok, "the same sample on a 96 kHz engine");
        Rig exact; std::mt19937 gen(5); std::uniform_real_distribution<float> d(-1, 1);
        std::vector<float> noise(500); for (auto& v : noise) v = d(gen);
        exact.setClip(0, makeClip(500, 48000.0, [&](std::size_t i) { return noise[i]; })); exact.on(0, 0); exact.run(520, true);
        double worst = 0; for (std::size_t i = 0; i < 500; ++i) worst = std::max(worst, std::abs(exact.master()[i] - kCenter * noise[i]));
        check(worst < 1e-6, "at the sample's own rate and no pitch shift, playback is exact (worst error " + std::to_string(worst) + ")");
    }
    {   // Stereo samples keep their sides.
        Rig rig; rig.setClip(0, makeClip(100, 48000.0, [](std::size_t) { return 0.8f; }, [](std::size_t) { return -0.3f; })); rig.on(0, 0);
        rig.run(120, true);
        near(rig.master()[10], 0.8 * kCenter, 1e-5, "the left side of a stereo sample"); near(rig.right[RackPlayer::masterBus][10], -0.3 * kCenter, 1e-5, "the right side");
    }
    {   // A short sample frees its voice.
        Rig rig; rig.setClip(0, dc(100)); rig.on(0, 0);
        rig.run(50, true); check(rig.player.activeVoices() == 1, "a playing sample holds a voice");
        rig.run(200, true); check(rig.player.activeVoices() == 0, "...and gives it back when it ends");
    }
    {   // A channel with no sample is silent and harmless.
        Rig rig; rig.on(0, 0); rig.on(1, 0); rig.setClip(1, dc(10));
        rig.run(6000, true);
        check(onsets(rig.master()).size() == 1, "only the channel that has a sample sounds");
    }
    {   // Many hits at once never exceed the voice pool.
        Rig rig;
        while (rig.project.channelCount < maxChannels) rig.project.addChannel("x", "");
        for (int c = 0; c < maxChannels; ++c) { rig.setClip(c, dc(300000)); for (int s = 0; s < 16; ++s) rig.on(c, s, 100); }
        rig.run(720 * 20, true, 1000.0);
        check(rig.player.activeVoices() <= RackPlayer::voiceCount && rig.player.activeVoices() > 0, "the voice pool is bounded (" + std::to_string(rig.player.activeVoices()) + " active)");
        bool finite = true; for (float v : rig.master()) finite = finite && std::isfinite(v);
        check(finite, "output stays finite while voices are stolen");
    }
}

// ---- voice stealing and live controls ---------------------------------------------
static void stealingTests() {
    // With every voice busy, each new sound takes the one that has been playing longest.
    Rig rig; rig.publish();
    auto sound = [&](double value) {
        auto clip = dc(5000, 48000.0, static_cast<float>(value));
        rig.keep.push_back(clip);
        rig.player.audition(*rig.snap, 0, 1.0f, clip.get());
    };
    auto heard = [&]() { rig.run(1, false); return static_cast<double>(rig.master().back()); };
    std::vector<double> slot;                                  // the level each voice slot plays
    for (int k = 0; k < RackPlayer::voiceCount; ++k) { slot.push_back(0.01 * (k + 1)); sound(slot.back()); }
    auto total = [&]() { double sum = 0; for (double v : slot) sum += v; return sum * kCenter; };
    check(rig.player.activeVoices() == RackPlayer::voiceCount, "every voice is busy");
    near(heard(), total(), 1e-4, "all thirty-two sounds are heard together");
    for (int steal = 0; steal < 6; ++steal) {
        const double level = 0.5 + 0.1 * steal;
        slot[static_cast<std::size_t>(steal)] = level;         // the oldest voice is the next one in start order
        sound(level);
        near(heard(), total(), 1e-4, "steal " + std::to_string(steal + 1) + " replaces the voice that started longest ago");
        check(rig.player.activeVoices() == RackPlayer::voiceCount, "the pool stays full and bounded");
    }
}

static void liveControlTests() {
    const double quarterTurn = 0.78539816339744830962;
    {   // The pan law holds for as long as a sample rings, not only when it starts:
        // channel pan and step pan add (clamped), and the left and right gains follow.
        for (const double channelPan : {-1.0, -0.5, 0.0, 0.5, 1.0}) for (const int stepPan : {-63, -20, 0, 31, 63}) {
            Rig rig; rig.setClip(0, dc(3000)); rig.on(0, 0);
            auto c = rig.project.channels[0]; c.pan = static_cast<float>(channelPan); rig.project.setChannel(0, c);
            rig.project.setStepField(0, 0, 0, StepField::pan, stepPan);
            rig.publish();
            rig.run(2000, false);                               // settle the pan while nothing plays
            const std::size_t at = rig.length();
            rig.run(400, true);
            const double position = std::clamp(channelPan + stepPan / 63.0, -1.0, 1.0);
            const std::string what = "channel pan " + std::to_string(channelPan) + " with step pan " + std::to_string(stepPan);
            near(rig.master()[at + 300], std::cos((position + 1.0) * quarterTurn), 2e-3, what + ": left level after the first control updates");
            near(rig.right[RackPlayer::masterBus][at + 300], std::sin((position + 1.0) * quarterTurn), 2e-3, what + ": right level");
        }
    }
    {   // Moving the channel pan while a long sample rings glides it across, on both sides.
        Rig rig; rig.setClip(0, dc(48000 * 2)); rig.on(0, 0);
        rig.run(6000, true);
        near(rig.master()[5000], kCenter, 1e-4, "centred before the move");
        auto c = rig.project.channels[0]; c.pan = 1.0f; rig.project.setChannel(0, c); rig.publish();
        const std::size_t at = rig.length();
        rig.run(12000, true);
        near(rig.master()[at + 11000], 0.0, 2e-3, "after a move to hard right the left side is empty");
        near(rig.right[RackPlayer::masterBus][at + 11000], 1.0, 2e-3, "...and the right side is at full level");
        double largest = 0;
        for (std::size_t i = at + 1; i < at + 6000; ++i)
            largest = std::max({largest, static_cast<double>(std::abs(rig.master()[i] - rig.master()[i - 1])),
                                static_cast<double>(std::abs(rig.right[RackPlayer::masterBus][i] - rig.right[RackPlayer::masterBus][i - 1]))});
        check(largest < 0.02, "the pan glides rather than jumping (largest step " + std::to_string(largest) + ")");
        // A hit that starts after the move is already where the pan is.
        Rig later; later.setClip(0, dc(3000)); later.on(0, 8);
        later.run(6000, true);
        auto d = later.project.channels[0]; d.pan = -1.0f; later.project.setChannel(0, d); later.publish();
        later.run(6000 * 8, true);                                       // the hit on step 9 starts at sample 48000
        near(later.master()[6000 * 8 + 100], 1.0, 2e-3, "a hit after the pan moved starts at the new pan (left)");
        near(later.right[RackPlayer::masterBus][6000 * 8 + 100], 0.0, 2e-3, "...(right)");
    }
    {   // A new hit starts at the channel fader's level, from its very first sample.
        Rig rig; rig.setClip(0, dc(1000)); rig.on(0, 0);
        auto c = rig.project.channels[0]; c.volume = 0.4f; rig.project.setChannel(0, c); rig.publish();
        rig.run(2000, false);
        const std::size_t at = rig.length();
        rig.run(100, true);
        for (const std::size_t i : {0u, 1u, 5u, 15u, 16u, 40u}) {
            near(rig.master()[at + i], kCenter * 0.5, 1e-4, "left level of sample " + std::to_string(i) + " of a hit on a half-level fader");
            near(rig.right[RackPlayer::masterBus][at + i], kCenter * 0.5, 1e-4, "right level of sample " + std::to_string(i));
        }
    }
}

static void probabilityTests() {
    auto fill = [](Rig& rig, int probability) {
        rig.setClip(0, dc(10));
        for (int s = 0; s < 16; ++s) { rig.on(0, s); rig.project.setStepField(0, 0, s, StepField::probability, probability); }
    };
    const int bars = 400, samples = 720 * 16 * bars;
    {   Rig rig; fill(rig, 0); rig.run(samples, true, 1000.0); check(onsets(rig.master()).empty(), "probability 0 never plays"); }
    {   Rig rig; fill(rig, 100); rig.run(samples, true, 1000.0); check(onsets(rig.master()).size() == 16u * bars, "probability 100 always plays"); }
    {   Rig rig; fill(rig, 50); rig.run(samples, true, 1000.0);
        const double hits = static_cast<double>(onsets(rig.master()).size());
        check(hits > 0.45 * 16 * bars && hits < 0.55 * 16 * bars, "probability 50 plays about half the time (" + std::to_string(hits) + " of " + std::to_string(16 * bars) + ")"); }
    {   // Same seed, same result, however the audio is chopped into blocks.
        Rig a, b; fill(a, 35); fill(b, 35);
        a.run(samples / 4, true, 1000.0, 4, 512); b.run(samples / 4, true, 1000.0, 4, 77);
        check(a.master() == b.master() && !onsets(a.master()).empty(), "probability is deterministic and independent of the block size");
        Rig again; fill(again, 35); again.run(samples / 4, true, 1000.0);
        check(again.master() == a.master(), "every play of the same pattern gives the same random hits");
    }
    {   // Stopping and playing again repeats exactly the same random hits.
        Rig rig; fill(rig, 35);
        const int part = samples / 8;
        rig.run(part, true, 1000.0);
        rig.run(3000, false, 1000.0);               // stop: every short hit has ended
        rig.run(part, true, 1000.0);
        const std::vector<float> first(rig.master().begin(), rig.master().begin() + part);
        const std::vector<float> second(rig.master().begin() + part + 3000, rig.master().end());
        check(first == second && !onsets(first).empty(), "each play of a pattern gets the same random hits as the one before");
    }
    {   // Steps that always play do not disturb the random choices of the others.
        Rig alone; alone.setClip(0, dc(10)); for (int s = 0; s < 16; ++s) { alone.on(0, s); alone.project.setStepField(0, 0, s, StepField::probability, 50); }
        Rig mixed; mixed.setClip(0, dc(10)); mixed.setClip(1, dc(10));
        for (int s = 0; s < 16; ++s) { mixed.on(0, s); mixed.project.setStepField(0, 0, s, StepField::probability, 50); mixed.on(1, s); }
        auto c0 = alone.project.channels[0]; c0.output = 0; alone.project.setChannel(0, c0);
        auto m0 = mixed.project.channels[0]; m0.output = 0; mixed.project.setChannel(0, m0);
        auto m1 = mixed.project.channels[1]; m1.output = 1; mixed.project.setChannel(1, m1);
        alone.run(720 * 16 * 50, true, 1000.0); mixed.run(720 * 16 * 50, true, 1000.0);
        check(alone.left[0] == mixed.left[0] && !onsets(alone.left[0]).empty(), "certain steps on other channels do not change which uncertain steps play");
    }
}

static void mixerTests() {
    {   // Mute and solo.
        Rig rig; rig.setClip(0, dc(10)); rig.setClip(1, dc(10)); rig.on(0, 0); rig.on(1, 0);
        auto a = rig.project.channels[0]; a.mute = true; rig.project.setChannel(0, a);
        rig.run(1000, true);
        near(rig.master()[0], kCenter, 1e-5, "a muted channel does not play (only the other one is heard)");
        Rig solo; solo.setClip(0, dc(10)); solo.setClip(1, dc(10)); solo.setClip(2, dc(10)); solo.on(0, 0); solo.on(1, 0); solo.on(2, 0);
        auto s = solo.project.channels[2]; s.solo = true; solo.project.setChannel(2, s);
        solo.run(1000, true);
        near(solo.master()[0], kCenter, 1e-5, "solo plays only the soloed channel");
        Rig both; both.setClip(0, dc(10)); both.setClip(1, dc(10)); both.on(0, 0); both.on(1, 0);
        auto m = both.project.channels[0]; m.solo = true; m.mute = true; both.project.setChannel(0, m);
        auto n = both.project.channels[1]; n.solo = true; both.project.setChannel(1, n);
        both.run(1000, true);
        near(both.master()[0], kCenter, 1e-5, "mute beats solo");
    }
    {   // Muting while a long sound rings fades it out quickly, without a click.
        Rig rig; rig.setClip(0, dc(48000 * 3)); rig.on(0, 0);
        rig.run(6000, true);
        auto c = rig.project.channels[0]; c.mute = true; rig.project.setChannel(0, c); rig.publish();
        const std::size_t at = rig.length();
        rig.run(6000, true);
        check(peakOf(rig.master(), at + 2500, at + 6000) < 1e-3, "a muted channel stops ringing within a few milliseconds");
        double largestStep = 0; for (std::size_t i = at; i < at + 3000; ++i) largestStep = std::max(largestStep, static_cast<double>(std::abs(rig.master()[i] - rig.master()[i - 1])));
        check(largestStep < 0.02, "...gently (largest step " + std::to_string(largestStep) + ")");
    }
    {   // Output routing.
        Rig rig; for (int c = 0; c < 3; ++c) rig.setClip(c, dc(10));
        rig.project.addChannel("x", ""); rig.setClip(3, dc(10));
        for (int c = 0; c < 4; ++c) rig.on(c, 0);
        const int outputs[4] = {2, masterOutput, 5, 0};
        for (int c = 0; c < 4; ++c) { auto ch = rig.project.channels[static_cast<std::size_t>(c)]; ch.output = static_cast<std::int8_t>(outputs[c]); rig.project.setChannel(c, ch); }
        rig.run(100, true, 120.0, 4);
        near(rig.left[2][0], kCenter, 1e-5, "channel routed to track 3 lands on bus 2");
        near(rig.left[0][0], kCenter, 1e-5, "channel routed to track 1 lands on bus 0");
        near(rig.master()[0], 2 * kCenter, 1e-5, "the master-routed channel and the one routed to a track that does not exist (track 6 of 4) both land on the master");
        Rig more; for (int c = 0; c < 3; ++c) more.setClip(c, dc(10));
        more.on(2, 0); auto ch = more.project.channels[2]; ch.output = 5; more.project.setChannel(2, ch);
        more.run(100, true, 120.0, 8);
        near(more.left[5][0], kCenter, 1e-5, "with eight tracks, track 6 exists and gets the sound");
        check(RackPlayer::busFor(-1, 4) == RackPlayer::masterBus && RackPlayer::busFor(3, 4) == 3 && RackPlayer::busFor(4, 4) == RackPlayer::masterBus
              && RackPlayer::busFor(15, 16) == 15 && RackPlayer::busFor(99, 16) == RackPlayer::masterBus, "bus mapping");
    }
    {   // Choke groups: a closed hat cuts the open hat.
        for (int group : {0, 1}) {
            Rig rig; rig.setClip(0, dc(40000, 48000.0, 1.0f)); rig.setClip(1, dc(40000, 48000.0, 0.25f));
            rig.on(0, 0); rig.on(1, 2);
            auto a = rig.project.channels[0]; a.chokeGroup = static_cast<std::uint8_t>(group); rig.project.setChannel(0, a);
            auto b = rig.project.channels[1]; b.chokeGroup = static_cast<std::uint8_t>(group); rig.project.setChannel(1, b);
            rig.run(12000 + 4000, true);
            const double late = rig.master()[12000 + 3500];
            if (group == 0) near(late, kCenter * 1.25, 1e-3, "without a choke group the open hat keeps ringing under the closed hat");
            else near(late, kCenter * 0.25, 2e-3, "with a choke group the closed hat cuts the open hat off");
        }
        Rig other; other.setClip(0, dc(40000)); other.setClip(1, dc(40000, 48000.0, 0.25f)); other.on(0, 0); other.on(1, 2);
        auto a = other.project.channels[0]; a.chokeGroup = 1; other.project.setChannel(0, a);
        auto b = other.project.channels[1]; b.chokeGroup = 2; other.project.setChannel(1, b);
        other.run(12000 + 4000, true);
        near(other.master()[12000 + 3500], kCenter * 1.25, 1e-3, "different choke groups do not cut each other");
    }
}

static void auditionTests() {
    Rig rig; rig.setClip(0, dc(100)); rig.publish();
    rig.player.audition(*rig.snap, 0, 1.0f, rig.clips[0].load());
    rig.run(200, false);
    near(rig.master()[3], kCenter, 1e-5, "an audition sounds while the transport is stopped");
    Rig muted; muted.setClip(0, dc(100));
    auto c = muted.project.channels[0]; c.mute = true; muted.project.setChannel(0, c); muted.publish();
    muted.player.audition(*muted.snap, 0, 0.5f, muted.clips[0].load());
    muted.run(200, false);
    near(muted.master()[3], 0.5 * kCenter, 1e-5, "...even on a muted channel, at the velocity given");
    Rig bad; bad.setClip(0, dc(100)); bad.publish();
    bad.player.audition(*bad.snap, 0, std::nanf(""), bad.clips[0].load());
    bad.player.audition(*bad.snap, 0, 0.0f, bad.clips[0].load());
    bad.player.audition(*bad.snap, 9, 1.0f, bad.clips[0].load());
    bad.player.audition(*bad.snap, -1, 1.0f, bad.clips[0].load());
    bad.player.audition(*bad.snap, 0, 1.0f, nullptr);
    check(bad.player.activeVoices() == 0, "nonsense auditions do not even take a voice");
    bad.player.audition(*bad.snap, 0, -0.5f, bad.clips[0].load());
    bad.player.audition(*bad.snap, 0, HUGE_VALF, bad.clips[0].load());
    bad.player.audition(*bad.snap, 0, -HUGE_VALF, bad.clips[0].load());
    bad.run(200, false);
    bool clean = true; for (float v : bad.master()) clean = clean && v == 0.0f;
    // (an infinite velocity is not a number the player can use, so it is refused like NaN)
    check(clean, "nonsense auditions are silent: no NaN, no infinity, nothing");
    Rig over; over.setClip(0, dc(100)); over.publish();
    over.player.audition(*over.snap, 0, 7.0f, over.clips[0].load());
    over.run(50, false);
    near(over.master()[3], kCenter, 1e-5, "a velocity above 1 plays at full level, not louder");
}

static void stopTests() {
    {   // The transport going quiet (process called with running = false) releases every sound too.
        Rig rig; rig.setClip(0, dc(48000 * 2)); rig.on(0, 0);
        rig.run(6000, true);
        check(rig.player.activeVoices() == 1, "a long sample is ringing");
        const std::size_t at = rig.length();
        rig.run(4800, false);
        check(peakOf(rig.master(), at + 2400, at + 4800) < 1e-3 && rig.player.activeVoices() == 0,
              "stopping the transport fades a ringing sample out within about 50 ms");
        double largestStep = 0; for (std::size_t i = at; i < at + 2400; ++i) largestStep = std::max(largestStep, static_cast<double>(std::abs(rig.master()[i] - rig.master()[i - 1])));
        check(largestStep < 0.02, "...without a click (largest step " + std::to_string(largestStep) + ")");
        check(rig.player.currentStep() == -1, "...and the playhead clears");
    }
    Rig rig; rig.setClip(0, dc(48000 * 2)); rig.on(0, 0);
    rig.run(6000, true);
    rig.player.stop();
    const std::size_t at = rig.length();
    rig.run(4800, false);
    check(peakOf(rig.master(), at + 2400, at + 4800) < 1e-3 && rig.player.activeVoices() == 0, "stop fades a ringing sample out within about 50 ms");
    double largestStep = 0; for (std::size_t i = at; i < at + 2400; ++i) largestStep = std::max(largestStep, static_cast<double>(std::abs(rig.master()[i] - rig.master()[i - 1])));
    check(largestStep < 0.02, "...without a click (largest step " + std::to_string(largestStep) + ")");
    check(rig.player.currentStep() == -1, "stop clears the playhead");
}

static void independenceTests() {
    // A busy rack gives bit-identical audio whatever the block size.
    auto build = [](Rig& rig) {
        rig.project.setPatternLength(0, 32); rig.project.setSwing(0.37f);
        std::mt19937 gen(11);
        std::vector<float> noise(3000); std::uniform_real_distribution<float> d(-1, 1); for (auto& v : noise) v = d(gen);
        while (rig.project.channelCount < 6) rig.project.addChannel("x", "");
        for (int c = 0; c < 6; ++c) {
            rig.setClip(c, makeClip(1500 + 300 * static_cast<std::size_t>(c), 44100.0 + 1000.0 * c, [&noise, c](std::size_t i) { return noise[(i + static_cast<std::size_t>(c) * 97) % noise.size()]; }));
            auto ch = rig.project.channels[static_cast<std::size_t>(c)]; ch.pan = (c - 2.5f) / 3.0f; ch.volume = 0.5f + 0.08f * c; ch.output = static_cast<std::int8_t>(c % 3 - 1); ch.chokeGroup = static_cast<std::uint8_t>(c % 2 ? 1 : 0);
            rig.project.setChannel(c, ch);
            for (int s = 0; s < 32; ++s) if ((s * 7 + c * 3) % 5 < 2) {
                rig.on(c, s, 20 + (s * 13 + c) % 100);
                rig.project.setStepField(0, c, s, StepField::pan, (s * 5) % 127 - 63);
                rig.project.setStepField(0, c, s, StepField::pitch, (s * 3 + c) % 49 - 24);
                rig.project.setStepField(0, c, s, StepField::probability, 40 + (s * 11) % 61);
                if ((s + c) % 3 == 0) {
                    rig.project.setStepField(0, c, s, StepField::gate, (s * 17) % 100 + 1);
                    rig.project.setStepField(0, c, s, StepField::nudge, (s * 29 + c) % 101);
                    rig.project.setStepField(0, c, s, StepField::ratchet, 1 + (s + c) % 4);
                }
            }
        }
    };
    Rig reference; build(reference); reference.run(48000 * 2, true, 140.0, 4, 512);
    check(!onsets(reference.master(), 0.01f).empty(), "the busy rack makes sound");
    for (int block : {1, 7, 64, 333, 4096}) {
        Rig other; build(other); other.run(48000 * 2, true, 140.0, 4, block);
        bool same = true; for (std::size_t b = 0; b < reference.left.size(); ++b) same = same && reference.left[b] == other.left[b] && reference.right[b] == other.right[b];
        check(same, "blocks of " + std::to_string(block) + " give exactly the same audio as blocks of 512");
    }
}

// Gate, nudge and ratchets. At 120 BPM and 48 kHz a step is 6000 samples.
static void stepTimingTests() {
    auto set = [](Rig& rig, int c, int s, StepField f, int v) { rig.project.setStepField(0, c, s, f, v); };
    {   // Nudge delays a step by a percent of a step, up to a whole step.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 4);
        set(rig, 0, 0, StepField::nudge, 50); set(rig, 0, 4, StepField::nudge, 100);
        rig.run(48000, true);
        const auto hits = onsets(rig.master());
        check(hits == std::vector<std::size_t>{3000, 30000}, "nudge 50% plays half a step late and 100% a whole step late");
    }
    {   // Ratchets split the step evenly; a nudge moves every hit.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 8);
        set(rig, 0, 0, StepField::ratchet, 3);
        set(rig, 0, 8, StepField::ratchet, 4); set(rig, 0, 8, StepField::nudge, 25);
        rig.run(60000, true);
        const auto hits = onsets(rig.master());
        check(hits == std::vector<std::size_t>{0, 2000, 4000, 49500, 51000, 52500, 54000}, "three and four ratchet hits land evenly inside their steps");
    }
    {   // Gate: full plays the sample to its end, 50% releases it half a step in.
        Rig full; full.setClip(0, dc(48000)); full.on(0, 0); full.run(6000, true);
        check(full.master()[5990] > 0.6f, "a full gate keeps the sample playing");
        Rig half; half.setClip(0, dc(48000)); half.on(0, 0); set(half, 0, 0, StepField::gate, 50); half.run(6000, true);
        check(half.master()[2990] > 0.6f && std::abs(half.master()[4500]) < 0.001f, "a 50% gate releases the hit half a step in");
    }
    {   // With ratchets the gate is a share of the time between hits.
        Rig rig; rig.setClip(0, dc(48000)); rig.on(0, 0);
        set(rig, 0, 0, StepField::ratchet, 2); set(rig, 0, 0, StepField::gate, 50); rig.run(6000, true);
        check(rig.master()[1400] > 0.6f && std::abs(rig.master()[2950]) < 0.01f && rig.master()[3010] > 0.6f,
              "each of two ratchet hits is released a quarter of a step after it starts");
    }
    {   // One chance roll decides the whole step: all four hits or none.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); rig.on(0, 8);
        for (int s : {0, 8}) { set(rig, 0, s, StepField::ratchet, 4); set(rig, 0, s, StepField::probability, 50); }
        const double step = 48000.0 * 60.0 / (1000.0 * 4.0);   // 720 samples at 1000 BPM
        rig.run(static_cast<int>(step * 16 * 20), true, 1000.0);
        std::map<long long, int> perStep;
        for (const auto at : onsets(rig.master())) ++perStep[static_cast<long long>(static_cast<double>(at) / step)];
        bool whole = !perStep.empty() && perStep.size() < 40;
        for (const auto& [index, count] : perStep) whole = whole && count == 4;
        check(whole, "ratchets of a step that may miss play all together or not at all (" + std::to_string(perStep.size()) + " of 40 steps played)");
    }
    {   // Muting a channel drops its hits still waiting for their nudge.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); set(rig, 0, 0, StepField::nudge, 50);
        rig.run(1000, true);
        auto ch = rig.project.channels[0]; ch.mute = true; rig.project.setChannel(0, ch); rig.publish();
        rig.run(5000, true);
        check(onsets(rig.master()).empty() && rig.player.pendingHits() == 0, "a hit muted before its nudge time is not played");
    }
    {   // Stopping drops waiting hits, so nothing plays after the transport stops.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); set(rig, 0, 0, StepField::nudge, 80);
        rig.run(1000, true);
        rig.run(10000, false);
        check(onsets(rig.master()).empty() && rig.player.pendingHits() == 0, "stop drops hits waiting for their nudge time");
        Rig other; other.setClip(0, dc(10)); other.on(0, 0); set(other, 0, 0, StepField::ratchet, 4);
        other.run(1000, true); other.player.stop(); other.run(10000, false);
        check(onsets(other.master()).size() == 1 && other.player.pendingHits() == 0, "stop() drops the rest of a ratchet");
    }
    {   // The Song's timeline path: a hit due on sample 100 with half a step of nudge plays on sample 3100.
        Rig rig; rig.setClip(0, dc(10)); rig.on(0, 0); set(rig, 0, 0, StepField::nudge, 50); set(rig, 0, 0, StepField::ratchet, 2); rig.publish();
        std::array<std::vector<float>, RackPlayer::busCount> bl, br; std::array<float*, RackPlayer::busCount> pl{}, pr{};
        for (std::size_t b = 0; b < bl.size(); ++b) { bl[b].assign(8000, 0.0f); br[b].assign(8000, 0.0f); pl[b] = bl[b].data(); pr[b] = br[b].data(); }
        const RackPlayer::TimelineHit hit{100, 0, 0};
        rig.player.processTimeline(*rig.snap, rig.clips, true, &hit, 1, 120.0, 4, 8000, pl.data(), pr.data());
        check(onsets(bl[RackPlayer::masterBus]) == std::vector<std::size_t>{3100, 6100}, "timeline hits are nudged and ratcheted at the song's tempo");
    }
    {   // A full rack of nudged four-hit ratchets stays inside the fixed queue and voice pool.
        Rig rig; rig.project.setPatternLength(0, 16);
        while (rig.project.channelCount < maxChannels) rig.project.addChannel("x", "");
        for (int c = 0; c < maxChannels; ++c) {
            rig.setClip(c, dc(200));
            for (int s = 0; s < 16; ++s) { rig.on(c, s); set(rig, c, s, StepField::ratchet, 4); set(rig, c, s, StepField::nudge, 100); }
        }
        int most = 0;
        rig.publish();
        for (int block = 0; block < 60; ++block) { rig.run(256, true, 1000.0); most = std::max(most, rig.player.pendingHits()); }
        check(most > 0 && most <= RackPlayer::maxPending && rig.player.activeVoices() <= RackPlayer::voiceCount,
              "the hit queue is bounded (" + std::to_string(most) + " waiting at most)");
    }
}

static void realTimeSafetyTests() {
    Rig rig; rig.setClip(0, dc(48000)); rig.setClip(1, makeClip(2000, 44100.0, [](std::size_t i) { return std::sin(0.1f * static_cast<float>(i)); }));
    for (int s = 0; s < 16; ++s) { rig.on(0, s); rig.on(1, s); rig.project.setStepField(0, 1, s, StepField::pitch, s - 8); rig.project.setStepField(0, 1, s, StepField::probability, 60);
                              rig.project.setStepField(0, 1, s, StepField::ratchet, 1 + s % 4); rig.project.setStepField(0, 1, s, StepField::nudge, (s * 13) % 101);
                              rig.project.setStepField(0, 0, s, StepField::gate, 10 + s * 5); }
    rig.project.setSwing(0.4f); rig.publish();
    std::array<std::vector<float>, RackPlayer::busCount> bl, br; std::array<float*, RackPlayer::busCount> pl{}, pr{};
    for (std::size_t b = 0; b < bl.size(); ++b) { bl[b].assign(512, 0.0f); br[b].assign(512, 0.0f); pl[b] = bl[b].data(); pr[b] = br[b].data(); }
    const auto before = allocations;
    for (int block = 0; block < 400; ++block) {
        rig.player.process(*rig.snap, rig.clips, block < 300, 150.0, 4, 512, pl.data(), pr.data());
        if (block == 100) rig.player.audition(*rig.snap, 0, 0.9f, rig.clips[0].load());
        if (block == 350) rig.player.stop();
    }
    const auto made = allocations - before;
    check(made == 0, "the audio path made no heap allocations (" + std::to_string(made) + ")");
}

static void melodicPlayback() {
    PianoRollPattern notes; notes.loopSteps = 16;
    notes.add(60, 0.25, 4, 1); notes.add(64, 0.25, 2, 1); notes.add(67, 1.5, 3, 1);
    auto setup = [&](Rig& rig) {
        rig.on(0, 0); // Dormant steps must not also trigger.
        rig.project.setChannelNotes(0, 0, notes); rig.setClip(0, dc(48000 * 4)); rig.publish();
    };
    Rig a; setup(a); a.run(40000, true, 120, 4, 257);
    near(peakOf(a.master(), 0, 1500), 0, 0, "fractional notes do not fire at their enclosing step");
    near(a.master()[1500], 2 * kCenter, 0.0001, "both chord notes start at sample 1500");
    near(a.master()[9000], 3 * kCenter, 0.0001, "later note overlaps the chord");
    near(a.master()[13499], 3 * kCenter, 0.0001, "short chord note holds its full length");
    check(a.master()[13500] < a.master()[13499], "short chord note releases independently after its exact duration");
    check(peakOf(a.master(), 29000, 40000) < 0.001, "long notes release without waiting for sample end");
    Rig b; setup(b); b.run(40000, true, 120, 4, 1);
    check(a.master() == b.master(), "melodic playback is independent of callback size");
    Rig cancel; setup(cancel); cancel.run(100, true);
    PianoRollPattern empty; empty.loopSteps = 16;
    cancel.project.setChannelNotes(0, 0, empty); cancel.publish(); cancel.run(10000, true);
    near(peakOf(cancel.master(), 0, cancel.length()), 0, 0, "replaced notes cancel pending fractional hits");
    Rig stopped; setup(stopped); stopped.run(100, true); stopped.run(10000, false);
    near(peakOf(stopped.master(), 0, stopped.length()), 0, 0, "stop cancels queued melodic notes");
    Rig muted; setup(muted); muted.run(2000, true);
    auto channel = muted.project.channels[0]; channel.mute = true; muted.project.setChannel(0, channel); muted.publish(); muted.run(8000, true);
    check(peakOf(muted.master(), 5000, 10000) < 0.001, "muting releases active melodic voices");
    Rig timeline; setup(timeline);
    std::array<std::vector<float>, RackPlayer::busCount> left, right;
    std::array<float*, RackPlayer::busCount> pl{}, pr{};
    for (std::size_t bus = 0; bus < pl.size(); ++bus) { left[bus].resize(12000); right[bus].resize(12000); pl[bus] = left[bus].data(); pr[bus] = right[bus].data(); }
    RackPlayer::TimelineHit hit {0, 0, 0, 6000};
    const auto before = allocations;
    timeline.player.processTimeline(*timeline.snap, timeline.clips, true, &hit, 1, 120, 4, 12000, pl.data(), pr.data());
    const auto after = allocations;
    check(before == after, "melodic timeline scheduling and rendering allocate nothing");
    near(left[RackPlayer::masterBus][1500], 2 * kCenter, 0.0001, "Song path plays fractional chords");
    check(peakOf(left[RackPlayer::masterBus], 8500, 12000) < 0.001, "trimmed Song region limits note lengths");
    for (std::size_t bus = 0; bus < pl.size(); ++bus) { std::fill(left[bus].begin(), left[bus].end(), 0); std::fill(right[bus].begin(), right[bus].end(), 0); }
    timeline.player.prepare(48000);
    hit = {0, 0, 0, 5000, 1000};
    timeline.player.processTimeline(*timeline.snap, timeline.clips, true, &hit, 1, 120, 4, 12000, pl.data(), pr.data());
    near(peakOf(left[RackPlayer::masterBus], 0, 500), 0, 0, "trim into a partial step waits for the note onset");
    near(left[RackPlayer::masterBus][500], 2 * kCenter, 0.0001, "fractional chord ahead of a trim point still plays");
}

int main() {
    clockTests();
    melodicPlayback();
    timingTests();
    voiceTests();
    stealingTests();
    liveControlTests();
    probabilityTests();
    mixerTests();
    auditionTests();
    stopTests();
    independenceTests();
    stepTimingTests();
    realTimeSafetyTests();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("PASS: channel rack engine (clock, swing, voices, pitch, pan, probability, gate, nudge, ratchets, choke, routing)");
    return 0;
}
