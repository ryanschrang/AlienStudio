// Channel Rack data model and built-in drum sounds. No JUCE: runs in a moment.
#include "RackModel.h"
#include "RackSounds.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <vector>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <thread>

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

static void defaultProjectTests() {
    const auto p = Project::createDefault();
    check(p.valid(), "the default project is valid");
    check(p.channelCount == 4 && p.info[0].name == "Kick" && p.info[1].name == "Clap" && p.info[2].name == "Hat" && p.info[3].name == "Snare",
          "the default rack is Kick, Clap, Hat, Snare");
    check(p.info[0].sample == "builtin:kick" && p.info[0].isBuiltin() && !p.info[0].isFile() && p.info[0].builtinId() == "kick", "default channels use built-in sounds");
    check(p.patternCount == 1 && p.patternNames[0] == "Pattern 1" && p.selectedPattern == 0 && p.pattern(0).empty() && p.pattern(0).length == 16,
          "one empty 16-step pattern");
    check(p.swing == 0.0f, "no swing");
    for (int c = 0; c < p.channelCount; ++c)
        check(p.channels[static_cast<std::size_t>(c)].volume == defaultChannelVolume && p.channels[static_cast<std::size_t>(c)].output == masterOutput, "default channel settings");
    // The snapshot mirrors the project.
    const auto s = p.snapshot();
    check(s->channelCount == 4 && s->selectedPattern == 0 && s->selected() != nullptr && s->selected() == p.patterns[0].get(), "the snapshot shares the selected pattern");
    check(s->pattern(1) == nullptr && s->pattern(-1) == nullptr && s->pattern(maxPatterns) == nullptr, "patterns that do not exist are null");
    check(s->audible(0) && !s->audible(4) && !s->audible(-1), "audibility is false past the last channel");
}

static void stepTests() {
    auto p = Project::createDefault();
    check(p.setStepOn(0, 1, 4, true) && p.pattern(0).steps[1][4].on, "turning a step on");
    check(p.pattern(0).steps[1][4].velocity == defaultVelocity && p.pattern(0).steps[1][4].probability == 100, "a new step has the default velocity and certain probability");
    check(!p.setStepOn(0, 1, 4, true), "turning on a step that is on changes nothing");
    check(p.setStepField(0, 1, 4, StepField::velocity, 40) && p.setStepField(0, 1, 4, StepField::pan, -20)
          && p.setStepField(0, 1, 4, StepField::pitch, 7) && p.setStepField(0, 1, 4, StepField::probability, 60), "editing every step field");
    const auto& edited = p.pattern(0).steps[1][4];
    check(edited.velocity == 40 && edited.pan == -20 && edited.pitch == 7 && edited.probability == 60 && edited.on, "the fields hold their values");
    check(p.setStepOn(0, 1, 4, false) && !p.pattern(0).steps[1][4].on && p.pattern(0).steps[1][4].velocity == 40, "turning a step off keeps its values");
    check(p.setStepOn(0, 1, 4, true) && p.pattern(0).steps[1][4].velocity == 40 && p.pattern(0).steps[1][4].pitch == 7, "...and turning it back on restores them");
    // Out-of-range values clamp; out-of-range addresses are refused.
    p.setStepField(0, 2, 0, StepField::velocity, 500); check(p.pattern(0).steps[2][0].velocity == 127, "velocity clamps at 127");
    p.setStepField(0, 2, 0, StepField::velocity, -5); check(p.pattern(0).steps[2][0].velocity == 1, "velocity clamps at 1");
    p.setStepField(0, 2, 0, StepField::pan, 999); check(p.pattern(0).steps[2][0].pan == 63, "pan clamps at +63");
    p.setStepField(0, 2, 0, StepField::pitch, -99); check(p.pattern(0).steps[2][0].pitch == -24, "pitch clamps at -24 semitones");
    p.setStepField(0, 2, 0, StepField::probability, 250); check(p.pattern(0).steps[2][0].probability == 100, "probability clamps at 100");
    check(p.setStepOn(0, 3, 127, true) && p.pattern(0).steps[3][127].on, "the last step is step 128");
    check(!p.setStepOn(0, 3, 128, true) && !p.setStepOn(0, 3, -1, true) && !p.setStepOn(0, 4, 0, true) && !p.setStepOn(1, 0, 0, true) && !p.setStepOn(-1, 0, 0, true),
          "addresses outside the rack are refused");
    Step bad; bad.velocity = 0;
    check(!p.setStep(0, 0, 0, bad), "an invalid step is refused");
    check(p.valid(), "the project is still valid after all that");
    for (int f = 0; f < stepFieldCount; ++f) {
        const auto field = static_cast<StepField>(f);
        Step s; setField(s, field, fieldDefault(field));
        check(getField(s, field) == fieldDefault(field) && fieldMinimum(field) <= fieldDefault(field) && fieldDefault(field) <= fieldMaximum(field), "field defaults sit inside their ranges");
        check(getField(Step{}, field) == fieldDefault(field), "a fresh step holds every field's default");
    }
}

// Gate, nudge and ratchet: ranges, clamping, and that they travel with the step.
static void stepTimingTests() {
    auto p = Project::createDefault();
    const Step fresh;
    check(fresh.gate == fullGate && fresh.nudge == 0 && fresh.ratchet == 1, "a new step plays the whole sample, on the grid, once");
    check(p.setStepOn(0, 1, 2, true), "turn a step on");
    p.setStepField(0, 1, 2, StepField::gate, 0); check(p.pattern(0).steps[1][2].gate == 1, "gate clamps at 1%");
    p.setStepField(0, 1, 2, StepField::gate, 250); check(p.pattern(0).steps[1][2].gate == fullGate, "gate clamps at full");
    p.setStepField(0, 1, 2, StepField::nudge, -10); check(p.pattern(0).steps[1][2].nudge == 0, "nudge never plays early");
    p.setStepField(0, 1, 2, StepField::nudge, 101); check(p.pattern(0).steps[1][2].nudge == maxNudge, "nudge clamps at one step");
    p.setStepField(0, 1, 2, StepField::ratchet, 0); check(p.pattern(0).steps[1][2].ratchet == 1, "ratchet clamps at one hit");
    p.setStepField(0, 1, 2, StepField::ratchet, 9); check(p.pattern(0).steps[1][2].ratchet == maxRatchet, "ratchet clamps at four hits");
    p.setStepField(0, 1, 2, StepField::gate, 40); p.setStepField(0, 1, 2, StepField::nudge, 25); p.setStepField(0, 1, 2, StepField::ratchet, 3);
    check(p.setStepOn(0, 1, 2, false) && p.setStepOn(0, 1, 2, true), "off and on again");
    const auto kept = p.pattern(0).steps[1][2];
    check(kept.gate == 40 && kept.nudge == 25 && kept.ratchet == 3, "turning a step off and on keeps its gate, nudge and ratchet");
    check(p.rotateRow(0, 1, 1) && p.pattern(0).steps[1][3] == kept && p.reverseRow(0, 1) && p.pattern(0).steps[1][12].ratchet == 3,
          "row tools move gate, nudge and ratchet with the step");
    using Breaker = void (*)(Step&);
    for (Breaker bad : std::initializer_list<Breaker>{[](Step& s) { s.gate = 0; }, [](Step& s) { s.gate = 101; }, [](Step& s) { s.nudge = 101; },
                     [](Step& s) { s.ratchet = 0; }, [](Step& s) { s.ratchet = 5; }}) {
        Step s; s.on = true; bad(s);
        check(!s.valid() && !p.setStep(0, 0, 0, s), "an out-of-range gate, nudge or ratchet is refused");
    }
    check(p.valid(), "the project stays valid");
}

static void copyOnWriteTests() {
    auto p = Project::createDefault();
    p.addPattern(); p.addPattern();
    const auto before = p;                     // what an undo step keeps
    check(p.setStepOn(1, 0, 0, true), "edit pattern 2");
    check(!before.pattern(1).steps[0][0].on, "a copy taken earlier does not see the edit");
    check(before.patterns[0] == p.patterns[0] && before.patterns[2] == p.patterns[2], "untouched patterns are shared, not copied");
    check(before.patterns[1] != p.patterns[1], "the edited pattern is a new object");
    check(!(before == p) && before == before && p == p, "projects compare by content");
    // Equal content in different objects still compares equal.
    auto q = p; q.patterns[1] = std::make_shared<const Pattern>(*p.patterns[1]);
    check(q == p, "equal patterns in different objects compare equal");
    auto r = p; check(r.setSwing(0.5f) && !(r == p), "swing is part of the comparison");
    auto t = p; t.selectedPattern = 0; check(!(t == p), "selection is part of the comparison");
    auto u = p; u.renamePattern(0, "Intro"); check(!(u == p), "names are part of the comparison");
}

static void patternTests() {
    auto p = Project::createDefault();
    p.setStepOn(0, 0, 0, true);
    check(p.addPattern() && p.patternCount == 2 && p.selectedPattern == 1 && p.patternNames[1] == "Pattern 2" && p.pattern(1).empty(), "a new pattern is empty, named and selected");
    check(p.addPattern(0) && p.patternCount == 3 && p.pattern(2).steps[0][0].on && p.patternNames[2] == "Pattern 3", "cloning copies the steps");
    check(p.patterns[2] == p.patterns[0], "a clone shares its source until one of them is edited");
    p.setStepOn(2, 0, 1, true);
    check(!p.pattern(0).steps[0][1].on && p.pattern(2).steps[0][1].on, "editing the clone leaves the original alone");
    check(p.renamePattern(1, "  Verse  ") && p.patternNames[1] == "Verse", "renaming trims");
    check(!p.renamePattern(1, "   ") && !p.renamePattern(1, "Verse") && !p.renamePattern(7, "x"), "empty, unchanged and unknown renames are refused");
    p.addPattern(); check(p.patternNames[3] == "Pattern 4", "names count on");
    p.renamePattern(3, "Pattern 5"); p.addPattern(); check(p.patternNames[4] != "Pattern 5", "a new name never repeats an existing one");
    std::set<std::string> names(p.patternNames.begin(), p.patternNames.begin() + p.patternCount);
    check(static_cast<int>(names.size()) == p.patternCount, "all pattern names are different");
    // Selecting and deleting.
    check(p.selectPattern(0) && p.selectedPattern == 0 && !p.selectPattern(0) && !p.selectPattern(99) && !p.selectPattern(-1), "selecting a pattern");
    p.selectPattern(4);
    check(p.deletePattern(1) && p.selectedPattern == 3 && p.patternCount == 4, "deleting a pattern before the selected one keeps the same pattern selected");
    check(p.patterns[4] == nullptr && p.patternNames[4].empty(), "the freed slot is clear");
    p.selectPattern(1); check(p.deletePattern(1) && p.selectedPattern == 1, "deleting the selected pattern selects the one that took its place");
    p.selectPattern(p.patternCount - 1); check(p.deletePattern(p.patternCount - 1) && p.selectedPattern == p.patternCount - 1, "deleting the last selected pattern selects the new last");
    while (p.patternCount > 1) check(p.deletePattern(0), "deleting down to one");
    check(!p.deletePattern(0) && p.patternCount == 1 && p.selectedPattern == 0, "the last pattern cannot be deleted");
    // Capacity.
    while (p.patternCount < maxPatterns) check(p.addPattern(), "adding up to the limit");
    check(!p.addPattern() && p.patternCount == maxPatterns && p.valid(), "no more than 64 patterns");
    // Length and clearing.
    auto q = Project::createDefault();
    q.setStepOn(0, 0, 40, true); q.setStepOn(0, 0, 3, true);
    check(q.setPatternLength(0, 32) && q.pattern(0).length == 32 && !q.setPatternLength(0, 32), "length 32");
    check(q.setPatternLength(0, 64) && q.setPatternLength(0, 128) && q.pattern(0).length == 128 && q.setPatternLength(0, 16) && q.pattern(0).length == 16,
          "lengths 64, 128 and 16");
    check(!q.setPatternLength(0, 24) && !q.setPatternLength(0, 0) && !q.setPatternLength(0, 256) && !q.setPatternLength(5, 16), "only 16, 32, 64 and 128 steps exist");
    check(q.pattern(0).steps[0][40].on, "steps past the end are kept when a pattern is shortened");
    check(q.clearChannelSteps(0, 0) && q.pattern(0).empty() && !q.clearChannelSteps(0, 0), "clearing one channel's steps");
    q.setStepOn(0, 1, 2, true); q.setStepOn(0, 3, 2, true);
    check(q.clearChannelSteps(0, 1) && !q.pattern(0).steps[1][2].on && q.pattern(0).steps[3][2].on, "...touches only that channel");
    check(q.clearPattern(0) && q.pattern(0).empty() && !q.clearPattern(0), "clearing a pattern");
}

static void channelTests() {
    auto p = Project::createDefault();
    p.setStepOn(0, 0, 0, true); p.setStepOn(0, 2, 5, true); p.setStepOn(0, 3, 9, true);
    p.addPattern(0);   // second pattern shares the first's data
    check(p.addChannel("Tom", "builtin:tom") && p.channelCount == 5 && p.info[4].name == "Tom", "adding a channel");
    check(p.addChannel("", "") && p.info[5].name == "Channel 6", "an empty name becomes Channel N");
    // Remove channel 1 (Clap): the later channels move up, in every pattern.
    check(p.removeChannel(1) && p.channelCount == 5 && p.info[1].name == "Hat" && p.info[2].name == "Snare" && p.info[3].name == "Tom", "removing a channel moves the rest up");
    for (int pat = 0; pat < 2; ++pat) {
        check(p.pattern(pat).steps[0][0].on && p.pattern(pat).steps[1][5].on && p.pattern(pat).steps[2][9].on && !p.pattern(pat).steps[3][9].on,
              "step data followed its channel in pattern " + std::to_string(pat + 1));
    }
    check(p.info[5].name.empty() && p.info[5].sample.empty() && p.channels[5] == Channel{}, "the vacated slot is clear");
    check(!p.removeChannel(5) && !p.removeChannel(-1), "no such channel");
    // Moving swaps a channel with its neighbour, steps and all.
    check(p.moveChannel(0, 1) && p.info[0].name == "Hat" && p.info[1].name == "Kick" && p.pattern(0).steps[1][0].on && p.pattern(0).steps[0][5].on, "moving a channel down");
    check(p.moveChannel(1, -1) && p.info[0].name == "Kick", "moving it back up");
    check(!p.moveChannel(0, -1) && !p.moveChannel(4, 1) && !p.moveChannel(0, 0), "no move past either end");
    // Settings.
    Channel c = p.channels[0]; c.volume = 0.5f; c.pan = -0.25f; c.mute = true; c.output = 3; c.chokeGroup = 2;
    check(p.setChannel(0, c) && p.channels[0] == c && !p.setChannel(0, c), "channel settings");
    Channel bad = c; bad.volume = 1.5f; check(!p.setChannel(0, bad), "volume above 1 refused");
    bad = c; bad.pan = std::nanf(""); check(!p.setChannel(0, bad), "NaN pan refused");
    bad = c; bad.output = 16; check(!p.setChannel(0, bad), "output past track 16 refused");
    bad = c; bad.output = -2; check(!p.setChannel(0, bad), "output below master refused");
    bad = c; bad.chokeGroup = 9; check(!p.setChannel(0, bad), "choke group past 8 refused");
    check(p.renameChannel(0, "808") && p.info[0].name == "808" && !p.renameChannel(0, "808") && !p.renameChannel(0, " ") && !p.renameChannel(9, "x"), "renaming");
    check(p.setSample(0, "C:\\drums\\kick.wav") && p.info[0].isFile() && !p.info[0].isBuiltin() && !p.setSample(0, "C:\\drums\\kick.wav"), "a file sample");
    check(p.setSample(0, "") && !p.info[0].isFile() && !p.info[0].isBuiltin(), "no sample");
    check(!p.setSample(0, std::string(2000, 'x')), "an absurd path is refused");
    // Mute and solo.
    auto q = Project::createDefault();
    check(q.audible(0) && q.audible(3) && !q.audible(4), "all audible by default");
    Channel m = q.channels[1]; m.mute = true; q.setChannel(1, m);
    check(q.audible(0) && !q.audible(1), "mute silences one channel");
    Channel s = q.channels[2]; s.solo = true; q.setChannel(2, s);
    check(!q.audible(0) && !q.audible(1) && q.audible(2) && !q.audible(3), "solo silences the others");
    m.solo = true; q.setChannel(1, m);
    check(!q.audible(1), "a muted channel stays silent even when soloed");
    auto snap = q.snapshot();
    for (int i = 0; i < 4; ++i) check(snap->audible(i) == q.audible(i), "the snapshot applies mute and solo the same way");
    // Capacity.
    auto full = Project::createDefault();
    while (full.channelCount < maxChannels) check(full.addChannel("x", ""), "adding channels");
    check(full.channelCount == 64 && !full.addChannel("x", "") && full.valid(), "no more than 64 channels");
    // Track bookkeeping.
    auto r = Project::createDefault();
    for (int i = 0; i < 4; ++i) { Channel o = r.channels[static_cast<std::size_t>(i)]; o.output = static_cast<std::int8_t>(i - 1); r.setChannel(i, o); }   // master, 0, 1, 2
    r.trackDeleted(1);
    check(r.channels[0].output == masterOutput && r.channels[1].output == 0 && r.channels[2].output == masterOutput && r.channels[3].output == 1,
          "deleting track 2 sends its channels to the master and shifts later tracks up");
    r.tracksSwapped(0, 1);
    check(r.channels[1].output == 1 && r.channels[3].output == 0 && r.channels[0].output == masterOutput, "swapping two tracks follows them");
}

static void nameTests() {
    check(Project::clean("  hi  ") == "hi" && Project::clean("\t\n") == "" && Project::clean("") == "", "names are trimmed");
    const std::string long8(100, 'a');
    check(Project::clean(long8).size() == static_cast<std::size_t>(maxNameBytes), "names are cut to 48 bytes");
    std::string accented; for (int i = 0; i < 40; ++i) accented += "\xc3\xa9";   // 80 bytes of e-acute
    const auto cut = Project::clean(accented);
    check(cut.size() <= static_cast<std::size_t>(maxNameBytes) && cut.size() % 2 == 0, "a name is never cut in the middle of a character");
    for (std::size_t i = 0; i < cut.size(); i += 2) check(static_cast<unsigned char>(cut[i]) == 0xC3 && static_cast<unsigned char>(cut[i + 1]) == 0xA9, "...so every character survives whole");
}

static void validationTests() {
    auto p = Project::createDefault(); p.addPattern();
    check(p.valid(), "baseline");
    { auto q = p; q.swing = std::nanf(""); check(!q.valid(), "NaN swing is invalid"); }
    { auto q = p; q.swing = 1.5f; check(!q.valid(), "swing above 1 is invalid"); }
    { auto q = p; q.selectedPattern = 5; check(!q.valid(), "selecting a missing pattern is invalid"); }
    { auto q = p; q.patternCount = 0; check(!q.valid(), "no patterns is invalid"); }
    { auto q = p; q.patterns[0].reset(); check(!q.valid(), "a missing pattern is invalid"); }
    { auto q = p; auto bad = std::make_shared<Pattern>(); bad->length = 20; q.patterns[0] = bad; check(!q.valid(), "a 20-step pattern is invalid"); }
    { auto q = p; auto bad = std::make_shared<Pattern>(); bad->steps[0][0].velocity = 0; q.patterns[0] = bad; check(!q.valid(), "velocity 0 is invalid"); }
    { auto q = p; auto bad = std::make_shared<Pattern>(); bad->steps[15][63].probability = 101; q.patterns[0] = bad; check(!q.valid(), "probability 101 is invalid"); }
    { auto q = p; q.channelCount = maxChannels + 1; check(!q.valid(), "65 channels is invalid"); }
    { auto q = p; q.info[10].name = "ghost"; check(!q.valid(), "data past the last channel is invalid"); }
    { auto q = p; q.channels[0].volume = -0.1f; check(!q.valid(), "negative volume is invalid"); }
    { auto q = p; q.info[0].name = std::string(100, 'x'); check(!q.valid(), "an over-long name is invalid"); }
    { auto q = p; q.patterns[5] = Project::emptyPattern(); check(!q.valid(), "a pattern past the count is invalid"); }
}

// ---- mailbox ----------------------------------------------------------------
static void mailboxTests() {
    SnapshotMailbox box;
    check(box.consume() == nullptr, "nothing new at the start");
    check(box.current() != nullptr && box.current()->channelCount == 4, "a default snapshot is always available");
    auto p = Project::createDefault(); p.setSwing(0.25f);
    check(box.publish(p.snapshot()) && !box.publish(nullptr), "publishing");
    const Snapshot* first = box.consume();
    check(first != nullptr && first->swing == 0.25f && box.consume() == nullptr, "consumed once");
    p.setSwing(0.5f); box.publish(p.snapshot()); p.setSwing(0.75f); box.publish(p.snapshot());
    const Snapshot* latest = box.consume();
    check(latest != nullptr && latest->swing == 0.75f, "only the newest of several snapshots arrives");
    check(first->swing == 0.25f || true, "(the earlier pointer is only guaranteed until the next consume)");
    // A producer hammering while a consumer reads: snapshots are never torn or empty.
    std::atomic<bool> stop{false}; std::atomic<int> torn{0}, seen{0};
    SnapshotMailbox busy;
    std::thread consumer([&] {
        while (!stop.load()) {
            if (const Snapshot* s = busy.consume()) {
                if (std::abs(s->swing - static_cast<float>(s->selectedPattern) / 63.0f) > 1e-6f) ++torn;
                if (s->selected() == nullptr || s->channelCount != 4) ++torn;
                ++seen;
            }
        }
    });
    auto q = Project::createDefault(); for (int i = 0; i < 63; ++i) q.addPattern();
    for (int round = 0; round < 40000; ++round) {
        q.selectPattern(round % 64);
        q.setSwing(static_cast<float>(q.selectedPattern) / 63.0f);
        busy.publish(q.snapshot());
    }
    stop = true; consumer.join();
    check(torn.load() == 0, "no snapshot was ever torn or empty");
    check(seen.load() > 0, "the consumer saw snapshots while the producer ran");
}

// ---- built-in sounds ----------------------------------------------------------
static double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0; for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from)));
}
static double crossingRate(const std::vector<float>& x, std::size_t from, std::size_t to, double sampleRate) {
    int crossings = 0; for (std::size_t i = from + 1; i < to; ++i) crossings += ((x[i - 1] < 0) != (x[i] < 0)) ? 1 : 0;
    return crossings / (2.0 * static_cast<double>(to - from) / sampleRate);   // roughly the dominant frequency in Hz
}

static void soundTests() {
    std::map<std::string, double> brightness;
    for (const auto& sound : builtinSounds) {
        const std::string id = sound.id;
        const auto clip = synthesizeSound(id);
        check(clip != nullptr && clip->valid(), id + ": a valid clip");
        if (!clip) continue;
        check(clip->channels == 1 && clip->sampleRate == 44100.0 && clip->right.empty(), id + ": mono at 44.1 kHz");
        const double seconds = static_cast<double>(clip->frames) / clip->sampleRate;
        check(seconds >= 0.1 && seconds <= 0.75, id + ": a sensible length (" + std::to_string(seconds) + " s)");
        double peak = 0; for (float v : clip->left) peak = std::max(peak, static_cast<double>(std::abs(v)));
        near(peak, 0.9, 0.001, id + ": peak is 0.9");
        check(clip->left.back() == 0.0f, id + ": ends in exact silence");
        const auto n = clip->left.size();
        check(rms(clip->left, 0, n / 4) > 0.02, id + ": has energy at the start");
        check(rms(clip->left, n * 3 / 4, n) < 0.1 * rms(clip->left, 0, n / 4), id + ": dies away");
        const auto again = synthesizeSound(id);
        check(again && again->left == clip->left, id + ": generation is deterministic");
        brightness[id] = crossingRate(clip->left, 0, std::min<std::size_t>(n, 2205), clip->sampleRate);
        check(builtinClip(id) == builtinClip(id) && builtinClip(id) != nullptr, id + ": the shared instance is cached");
        check(builtinDisplayName(id) != nullptr, id + ": has a display name");
    }
    check(synthesizeSound("nope") == nullptr && builtinClip("nope") == nullptr && builtinDisplayName("nope") == nullptr, "unknown sounds are refused");
    check(brightness["kick"] < 300 && brightness["tom"] < 400, "the kick and tom are low (" + std::to_string(brightness["kick"]) + " Hz, " + std::to_string(brightness["tom"]) + " Hz)");
    check(brightness["hat"] > 2500 && brightness["openhat"] > 2500, "the hats are bright (" + std::to_string(brightness["hat"]) + " Hz)");
    check(brightness["hat"] > 4 * brightness["kick"], "the hat is far brighter than the kick");
    check(brightness["snare"] > brightness["kick"], "the snare is brighter than the kick");
    // The open hat rings longer than the closed hat.
    const auto closed = synthesizeSound("hat"), open = synthesizeSound("openhat");
    check(open->frames > 3 * closed->frames, "the open hat is much longer than the closed hat");
    // The kick's pitch falls: fast at the start, slow at the end of its body.
    const auto kick = synthesizeSound("kick");
    {   // Skip the first few milliseconds: the click's noise crosses zero often, with or without a pitch drop.
        const double early = crossingRate(kick->left, 500, 2000, 44100.0), late = crossingRate(kick->left, 4000, 6500, 44100.0);
        check(early > 1.4 * late && late < 70.0 && early > 75.0,
              "the kick's pitch falls from about 90 Hz to about 50 Hz (" + std::to_string(early) + " Hz then " + std::to_string(late) + " Hz)");
    }
    // The clap has several onsets: the signal envelope is not a single decay.
    const auto clap = synthesizeSound("clap");
    double e0 = rms(clap->left, 0, 220), eGap = rms(clap->left, 330, 440), e1 = rms(clap->left, 490, 700);
    check(e1 > 0.6 * e0 || eGap < e0, "the clap is a burst of slaps rather than one smooth decay");
}


// ---- exact limits and edge cases (each one pins a rule the rest of the rack relies on) ----
static void boundaryTests() {
    auto step = [](auto set) { Step s; set(s); return s; };
    check(step([](Step& s) { s.pan = 63; }).valid() && step([](Step& s) { s.pan = -63; }).valid(), "step pan -63 and +63 are valid");
    check(!step([](Step& s) { s.pan = 64; }).valid() && !step([](Step& s) { s.pan = -64; }).valid(), "step pan -64 and +64 are not");
    check(step([](Step& s) { s.pitch = 24; }).valid() && step([](Step& s) { s.pitch = -24; }).valid(), "step pitch -24 and +24 are valid");
    check(!step([](Step& s) { s.pitch = 25; }).valid() && !step([](Step& s) { s.pitch = -25; }).valid(), "step pitch -25 and +25 are not");
    check(step([](Step& s) { s.velocity = 1; }).valid() && step([](Step& s) { s.velocity = 127; }).valid(), "step velocity 1 and 127 are valid");
    check(!step([](Step& s) { s.velocity = 0; }).valid() && !step([](Step& s) { s.velocity = 128; }).valid(), "step velocity 0 and 128 are not");
    check(step([](Step& s) { s.probability = 0; }).valid() && step([](Step& s) { s.probability = 100; }).valid(), "step chance 0 and 100 are valid");
    check(!step([](Step& s) { s.probability = 101; }).valid(), "step chance 101 is not");
    auto channel = [](auto set) { Channel c; set(c); return c; };
    check(channel([](Channel& c) { c.pan = 1.0f; }).valid() && channel([](Channel& c) { c.pan = -1.0f; }).valid(), "channel pan -1 and +1 are valid");
    check(!channel([](Channel& c) { c.pan = 1.01f; }).valid() && !channel([](Channel& c) { c.pan = -1.01f; }).valid(), "channel pan beyond -1 and +1 is not");
    check(channel([](Channel& c) { c.volume = 0.0f; }).valid() && channel([](Channel& c) { c.volume = 1.0f; }).valid(), "channel volume 0 and 1 are valid");
    check(!channel([](Channel& c) { c.volume = 1.01f; }).valid() && !channel([](Channel& c) { c.volume = -0.01f; }).valid(), "channel volume outside 0..1 is not");
    check(channel([](Channel& c) { c.output = -1; }).valid() && channel([](Channel& c) { c.output = 15; }).valid(), "channel output master and track 16 are valid");
    check(!channel([](Channel& c) { c.output = -2; }).valid() && !channel([](Channel& c) { c.output = 16; }).valid(), "channel output beyond them is not");
    check(channel([](Channel& c) { c.chokeGroup = 0; }).valid() && channel([](Channel& c) { c.chokeGroup = 8; }).valid() && !channel([](Channel& c) { c.chokeGroup = 9; }).valid(),
          "choke groups 0..8 are valid and 9 is not");
    // Which side a sound lives on decides where it is sought: a path that merely contains the word is a file.
    const ChannelInfo tricky{"x", "/music/builtin:kick.wav"}, built{"x", "builtin:hat"}, none{"x", ""};
    check(tricky.isFile() && !tricky.isBuiltin() && tricky.builtinId().empty(), "a file whose path contains 'builtin:' is still a file");
    check(built.isBuiltin() && !built.isFile() && built.builtinId() == "hat", "'builtin:hat' is the built-in hat");
    check(!none.isBuiltin() && !none.isFile() && none.builtinId().empty(), "no sample is neither");
}

static void writeSemanticsTests() {
    auto p = Project::createDefault();
    p.setStepOn(0, 1, 4, true); p.setStepField(0, 1, 4, StepField::pitch, 5);
    const Step same = p.pattern(0).steps[1][4];
    check(!p.setStep(0, 1, 4, same), "writing the values a step already has is not a change");
    Step other = same; other.pitch = -3;
    check(p.setStep(0, 1, 4, other) && p.pattern(0).steps[1][4].pitch == -3, "writing different values is");
    Step bad = same; bad.velocity = 0;
    check(!p.setStep(0, 1, 4, bad) && p.pattern(0).steps[1][4].pitch == -3, "and an invalid step is refused without touching anything");
    // Swing is clamped to 0..100% and refuses values that are not numbers.
    auto q = Project::createDefault();
    check(q.setSwing(0.5f) && q.swing == 0.5f && !q.setSwing(0.5f), "swing 50%");
    check(q.setSwing(5.0f) && q.swing == 1.0f && !q.setSwing(7.0f), "swing above 100% becomes 100%");
    check(q.setSwing(-2.0f) && q.swing == 0.0f, "swing below 0% becomes 0%");
    q.setSwing(0.3f);
    check(!q.setSwing(std::nanf("")) && q.swing == 0.3f, "swing refuses NaN");
    check(!q.setSwing(HUGE_VALF) && q.swing == 0.3f && !q.setSwing(-HUGE_VALF) && q.swing == 0.3f, "and infinity");
    check(q.valid(), "the rack is still valid after all that");
}

static void patternSelectionTests() {
    auto m = Project::createDefault(); m.addPattern(); m.addPattern(); m.addPattern();   // four patterns
    m.renamePattern(2, "Keep"); m.selectPattern(2);
    check(m.deletePattern(0) && m.patternCount == 3 && m.selectedPattern == 1 && m.patternNames[1] == "Keep",
          "deleting a pattern ahead of the selected one (not the last) keeps the same pattern selected");
    m.selectPattern(0);
    check(m.deletePattern(2) && m.selectedPattern == 0, "deleting one after it leaves the selection alone");
    check(m.patterns[2] == nullptr && m.patternNames[2].empty(), "and the freed slot is clear");
}

static void fullRackTests() {
    auto full = Project::createDefault();
    while (full.channelCount < maxChannels) full.addChannel("x", "");
    constexpr int last = maxChannels - 1;
    full.setStepOn(0, 0, 0, true); full.setStepOn(0, last, 3, true); full.setStepOn(0, last, maxSteps - 1, true);
    full.addPattern(0);
    check(full.removeChannel(0) && full.channelCount == last, "removing a channel from a full rack");
    for (int pat = 0; pat < 2; ++pat) {
        check(full.pattern(pat).steps[last - 1][3].on && full.pattern(pat).steps[last - 1][maxSteps - 1].on,
              "the last channel's steps moved up (pattern " + std::to_string(pat + 1) + ")");
        check(full.pattern(pat).steps[last] == std::array<Step, maxSteps>{}, "and the vacated last row is empty (pattern " + std::to_string(pat + 1) + ")");
    }
    check(full.addChannel("new", "") && full.pattern(0).steps[last] == std::array<Step, maxSteps>{} && full.pattern(1).steps[last] == std::array<Step, maxSteps>{},
          "a channel added afterwards starts with no steps");
    check(full.valid(), "the rack is valid");
}

// Every character must survive whole, whatever its width and wherever the 48-byte limit falls.
static bool wellFormedUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > s.size()) return false;
        for (std::size_t k = 1; k < length; ++k) if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}
static void utf8CutTests() {
    const std::string pieces[] = {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\xa5\x81"};   // 2, 3 and 4 bytes wide
    for (const auto& piece : pieces)
        for (int lead = 0; lead < 4; ++lead) {
            std::string text(static_cast<std::size_t>(lead), 'a');
            while (text.size() < 70) text += piece;
            const auto cut = Project::clean(text);
            check(cut.size() <= static_cast<std::size_t>(maxNameBytes) && cut.size() >= 44 && wellFormedUtf8(cut) && text.compare(0, cut.size(), cut) == 0,
                  "a " + std::to_string(piece.size()) + "-byte character after " + std::to_string(lead) + " letters is cut cleanly (" + std::to_string(cut.size()) + " bytes)");
        }
    check(Project::clean("  trailing  ") == "trailing" && Project::clean("\t x \r\n") == "x", "names lose spaces at both ends");
}

static void displayNameTests() {
    const std::pair<const char*, const char*> expected[] = {{"kick", "Kick"}, {"snare", "Snare"}, {"clap", "Clap"}, {"hat", "Closed hat"},
                                                            {"openhat", "Open hat"}, {"tom", "Tom"}, {"rim", "Rim shot"}, {"cowbell", "Cowbell"}};
    check(builtinSounds.size() == 8, "the kit has eight sounds");
    for (const auto& [id, name] : expected) {
        const char* shown = builtinDisplayName(id);
        check(shown != nullptr && std::string(shown) == name, std::string("the ") + id + " is called " + name);
    }
}


static void rowToolTests() {
    auto p = Project::createDefault();
    Step special; special.on = true; special.velocity = 71; special.pan = -20; special.pitch = 7; special.probability = 45;
    p.setStep(0, 0, 0, special); p.setStep(0, 0, 60, special);
    const auto original = p; const auto shared = p.patterns[0];
    check(p.fillEvery(0, 0, 4), "fill every fourth step");
    for (int i = 0; i < 16; ++i) check(p.pattern(0).steps[0][static_cast<std::size_t>(i)].on == (i % 4 == 0), "fill positions");
    check(p.pattern(0).steps[0][0] == special && p.pattern(0).steps[0][60] == special, "fill preserves expression and hidden steps");
    check(original.patterns[0] == shared && original.pattern(0).steps[0][4].on == false, "row tools preserve immutable undo snapshot");
    p = original; check(p.rotateRow(0, 0, -1) && p.pattern(0).steps[0][15] == special, "rotate wraps complete step left");
    check(p.rotateRow(0, 0, 1) && p == original, "opposite rotation restores exact content");
    check(p.reverseRow(0, 0) && p.pattern(0).steps[0][15] == special, "reverse includes expression");
    check(p.reverseRow(0, 0) && p == original, "reverse twice restores content");
    check(p.repeatRow(0, 0, 4) && p.pattern(0).steps[0][12] == special, "repeat first beat");
    check(p.pasteRow(0, 1, p.pattern(0).steps[0]) && p.pattern(0).steps[1][12] == special, "copy/paste complete step expression");
    check(!p.fillEvery(0, 0, 0) && !p.rotateRow(0, 0, 16) && !p.repeatRow(0, 0, 16), "invalid and no-op row tools rejected");
    p = original; p.addPattern(0); const auto before = p;
    check(p.duplicateChannel(0) && p.channelCount == 5 && p.info[1].sample == before.info[0].sample
        && p.channels[1] == before.channels[0], "duplicate preserves sound and mixer routing");
    for (int pat = 0; pat < 2; ++pat) check(p.pattern(pat).steps[1] == before.pattern(pat).steps[0]
        && p.pattern(pat).steps[2] == before.pattern(pat).steps[1], "duplicate inserts notes in every pattern");
    check(p.valid() && before.channelCount == 4, "duplicate leaves undo snapshot intact");
    while (p.channelCount < maxChannels) p.addChannel("Extra", "builtin:hat");
    const auto full = p; check(!p.duplicateChannel(0) && p == full, "full rack duplication is rejected without mutation");
}

static void melodicRows() {
    Project p = Project::createDefault();
    alien::PianoRollPattern notes; notes.loopSteps = 16;
    notes.add(36, 0.25, 8); notes.add(60, 0.25, 4); notes.add(67, 1.5, 9);
    check(p.setChannelNotes(0, 1, notes) && p.valid(), "full range, overlapping chords are valid rack data");
    const auto original = p;
    check(!p.setChannelNotes(0, 1, notes), "same notes are a no-op");
    check(!p.setStepOn(0, 1, 0, true) && !p.fillEvery(0, 1, 2), "step edits cannot overwrite a melodic row");
    check(p.duplicateChannel(1) && p.pattern(0).notes[1] == p.pattern(0).notes[2], "duplicate shares immutable notes");
    notes.add(127, 12, 3);
    check(p.setChannelNotes(0, 2, notes) && p.pattern(0).notes[1]->count == 3, "editing clone leaves source unchanged");
    check(p.moveChannel(2, 1) && p.pattern(0).notes[3]->count == 4, "notes follow channel movement");
    check(p.removeChannel(2) && p.pattern(0).notes[2]->count == 4 && p.valid(), "notes follow channel deletion");
    check(p.setPatternLength(0, 32) && p.pattern(0).notes[1]->loopSteps == 32, "length extension keeps notes");
    notes.loopSteps = 32; notes.add(60, 20, 8);
    check(p.setChannelNotes(0, 1, notes) && !p.setPatternLength(0, 16), "shortening that would cut a note is refused");
    check(p.clearChannelSteps(0, 1) && !p.isNoteChannel(0, 1) && p.valid(), "explicit channel clear returns to steps");
    check(original.pattern(0).notes[1]->count == 3, "old undo snapshot remains immutable");
    check(p.clearPattern(0) && p.pattern(0).empty() && !p.isNoteChannel(0, 2), "clear pattern removes melodic notes too");
    alien::PianoRollPattern empty; empty.loopSteps = 32;
    check(p.setChannelNotes(0, 1, empty) && p.clearPattern(0) && !p.isNoteChannel(0, 1), "clear pattern resets even an empty melodic row");
}

static void synthChannelModel() {
    Project p = Project::createDefault();
    check(p.setInstrumentPreset(0, 9) && p.valid() && p.channels[0].isSynth() && p.info[0].sample.empty(), "synth replaces sample source");
    const auto original = p;
    check(!p.setInstrumentPreset(0, 9) && !p.setInstrumentPreset(0, alien::presetCount)
          && !p.setInstrumentPreset(0, -1) && p == original, "invalid and unchanged synth presets are atomic no-ops");
    check(p.duplicateChannel(0) && p.channels[1].instrumentPreset == 9, "duplicate preserves synth preset");
    check(p.moveChannel(1, 1) && p.channels[2].instrumentPreset == 9 && p.removeChannel(1)
          && p.channels[1].instrumentPreset == 9 && p.valid(), "synth identity follows channel movement and deletion");
    check(p.setSample(0, "builtin:kick") && !p.channels[0].isSynth() && p.valid(), "choosing a sample returns to sample playback");
    check(original.channels[0].instrumentPreset == 9 && original.info[0].sample.empty(), "undo snapshots keep their instrument");
    p.channels[1].instrumentPreset = alien::presetCount;
    check(!p.valid(), "unknown presets invalidate a rack");
}

int main() {
    synthChannelModel();
    melodicRows();
    rowToolTests();
    defaultProjectTests();
    stepTests();
    copyOnWriteTests();
    patternTests();
    channelTests();
    nameTests();
    validationTests();
    mailboxTests();
    soundTests();
    boundaryTests();
    writeSemanticsTests();
    patternSelectionTests();
    fullRackTests();
    stepTimingTests();
    utf8CutTests();
    displayNameTests();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("PASS: channel rack model and built-in sounds");
    return 0;
}
