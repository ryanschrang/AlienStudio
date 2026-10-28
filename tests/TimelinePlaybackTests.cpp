// Plays the Arrangement timeline through the real audio callback, offline: starting
// from any position, jumping while playing, loop ranges, Channel Rack patterns on
// their lanes, live edits that must not restart the song, tempo changes, exactness
// over a long song, and the promise that the callback never allocates.
#include "ToneEngine.h"
#include "RackSounds.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

// Count heap allocation on the calling thread, including actual audio callbacks.
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

using namespace alien::timeline;

// At 120 beats a minute and 48 kHz: a bar is 96000 frames, a sixteenth 6000, a tick 25.
static constexpr int barFrames = 96000, stepFrames = 6000, tickFrames = 25;

struct Rig {
    // The engine is large (about 80 KB, more in a Debug build) and a test function holds a dozen of them. A Debug build
    // on Windows gives every local its own stack space and a thread starts with 1 MB, so on the heap it stays.
    std::unique_ptr<ToneEngine> owned = std::make_unique<ToneEngine>();
    ToneEngine& engine = *owned;
    std::vector<float> out;                  // everything rendered so far, left channel
    int block = 257;                         // an awkward size, so nothing lines up by luck
    Rig() { engine.prepare(48000); engine.patternMode.store(2); engine.bpm.store(120); engine.activeTracks.store(4); }
    void publish(const std::shared_ptr<alien::ArrangementPlayback>& song) {
        require(engine.arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(song)), "publish the song");
    }
    void publishRack(const alien::rack::Project& project) {
        for (int c = 0; c < project.channelCount; ++c) {
            const auto& info = project.info[static_cast<std::size_t>(c)];
            engine.rackSamples[static_cast<std::size_t>(c)].store(info.isBuiltin() ? alien::rack::builtinClip(info.builtinId()).get() : nullptr);
        }
        engine.rackMailbox.publish(project.snapshot());
    }
    void seek(std::int64_t tick) { engine.timelineSeek.store(tick); }
    void play() { engine.playing.store(true); }
    void render(long frames) {
        std::vector<float> l(static_cast<std::size_t>(block)), r(static_cast<std::size_t>(block));
        while (frames > 0) {
            const int count = static_cast<int>(std::min<long>(frames, block));
            float* outputs[]{l.data(), r.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            for (int i = 0; i < count; ++i) {
                require(std::isfinite(l[static_cast<std::size_t>(i)]), "finite output");
                out.push_back(l[static_cast<std::size_t>(i)]);
            }
            frames -= count;
        }
    }
    // Renders as the audio device would, in time order but faster than real time (about four times), so the stream
    // service's worker can refill each stream as it would in the app. An unpaced render can outrun the worker on a
    // busy machine: the stream then runs dry and the audio stops matching, which is not something the app can do.
    void renderPaced(long frames) {
        while (frames > 0) {
            const long count = std::min<long>(frames, 1024);
            render(count);
            frames -= count;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    long frames() const { return static_cast<long>(out.size()); }
    double energy(long from, long to) const {
        double sum = 0;
        for (long i = std::max(0L, from); i < std::min(to, frames()); ++i) sum += static_cast<double>(out[static_cast<std::size_t>(i)]) * out[static_cast<std::size_t>(i)];
        return sum;
    }
    // The first frame in [from, to) louder than the threshold, or -1.
    long firstAbove(long from, long to, float threshold = 1.0e-4f) const {
        for (long i = std::max(0L, from); i < std::min(to, frames()); ++i)
            if (std::abs(out[static_cast<std::size_t>(i)]) > threshold) return i;
        return -1;
    }
    std::int64_t position() const { return engine.timelinePosition.load(); }
};

static std::shared_ptr<alien::ArrangementPlayback> newSong(int bars) {
    auto song = std::make_shared<alien::ArrangementPlayback>();
    song->timeline.setLengthBars(bars);
    return song;
}
static Region regionOf(int row, int source, std::int64_t start, std::int64_t length) {
    Region r; r.row = row; r.source = source; r.start = start; r.length = length; return r;
}
static std::uint32_t place(alien::ArrangementPlayback& song, const Region& r) {
    std::uint32_t id = 0;
    require(song.timeline.place(r, &id) && id != 0, "place a region");
    return id;
}
// One-bar clips: a loop of sixteen sixteenths, so a pattern's cycle is a bar of the timeline.
static alien::PianoRollPattern barLoop() {
    alien::PianoRollPattern pattern; pattern.loopSteps = 16; return pattern;
}
static alien::PianoRollPattern held(int pitch = 60) {
    auto pattern = barLoop();
    require(pattern.add(pitch, 0, pattern.loopSteps, 0.8f) != 0, "held note");
    return pattern;
}
static alien::PianoRollPattern oneNote(int pitch, double step, double length) {
    auto pattern = barLoop();
    require(pattern.add(pitch, step, length, 0.8f) != 0, "one note");
    return pattern;
}
static alien::SessionAudioClip makeSine(int seconds = 2, double hz = 441.0) {
    alien::SessionAudioClip clip;
    clip.sampleRate = 48000; clip.channels = 1; clip.frames = static_cast<std::size_t>(48000 * seconds);
    clip.left.resize(clip.frames);
    for (std::size_t i = 0; i < clip.frames; ++i) clip.left[i] = static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / 48000.0));
    clip.buildWaveform();
    require(clip.valid(), "test audio clip");
    return clip;
}
static alien::rack::Project rackWith(std::initializer_list<std::pair<int, int>> channelSteps, int pattern = 0) {
    auto project = alien::rack::Project::createDefault();
    for (const auto& [channel, step] : channelSteps) require(project.setStepOn(pattern, channel, step, true), "program a step");
    return project;
}
static void closeTo(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) { std::cerr << "FAILED: " << message << " (got " << actual << ", wanted " << expected << ")\n"; std::exit(1); }
}

// ---- starting anywhere --------------------------------------------------------------------

static void startFromAnywhereTests() {
    // A note that starts at the top of each bar: playing from the middle of the bar waits for the next one.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = oneNote(60, 0, 2);
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        rig.publish(song); rig.seek(ticksPerBar / 2); rig.play();
        rig.render(barFrames);
        require(rig.energy(0, barFrames / 2 - 2) == 0, "nothing sounds before the next bar begins");
        const auto onset = rig.firstAbove(0, barFrames);
        require(onset >= barFrames / 2 && onset <= barFrames / 2 + 400, "the note begins on the bar line");
        closeTo(static_cast<double>(rig.position()), ticksPerBar / 2 + barFrames / tickFrames, 1.0, "the playhead started where it was asked to");
    }
    // A held note that began before the starting point is heard straight away.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        rig.publish(song); rig.seek(ticksPerBar + ticksPerBar / 4); rig.play();
        rig.render(2048);
        require(rig.energy(0, 2048) > 0.001, "a note already in progress sounds when playback starts inside it");
        require(rig.firstAbove(0, 2048) >= 0 && rig.firstAbove(0, 2048) < 600, "and promptly");
    }
    // The same position gives the same sound whether you start there or play up to it.
    {
        Rig a, b; auto song = newSong(4);
        song->clips[0][0].pattern = oneNote(64, 8, 4);
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        a.publish(song); b.publish(song);
        a.seek(0); a.play(); a.render(2 * barFrames);
        b.seek(ticksPerBar); b.play(); b.render(barFrames);
        // Bar two of A is bar one of B (the note at step 8 is at the same place in both).
        const auto onsetA = a.firstAbove(barFrames, 2 * barFrames) - barFrames, onsetB = b.firstAbove(0, barFrames);
        require(std::abs(onsetA - onsetB) <= 2 && onsetB >= 8 * stepFrames, "starting on a bar line plays that bar exactly as playing up to it does");
    }
    // Starting at or past the end of the song plays nothing and stops.
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 2 * ticksPerBar));
        rig.publish(song); rig.seek(2 * ticksPerBar); rig.play();
        rig.render(2048);
        require(!rig.engine.playing.load() && rig.energy(0, 2048) == 0, "starting at the end plays nothing and stops");
        rig.seek(100 * ticksPerBar); rig.play(); rig.render(512);
        require(!rig.engine.playing.load() && rig.energy(2048, 2560) == 0, "a position past the end is the same");
    }
    // Jumping while it plays: the playhead lands there and keeps moving.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held(60); song->clips[1][0].pattern = held(72);
        place(*song, regionOf(0, 0, 0, 2 * ticksPerBar)); place(*song, regionOf(0, 1, 2 * ticksPerBar, 2 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames / 2);
        require(rig.engine.playing.load() && rig.position() > 0 && rig.position() < ticksPerBar, "playing in the first half");
        rig.seek(3 * ticksPerBar); rig.render(257);
        const auto landed = rig.position();
        require(landed >= 3 * ticksPerBar && landed < 3 * ticksPerBar + 20, "a jump while playing lands on the requested tick");
        rig.render(barFrames / 4);
        require(rig.position() > landed && rig.engine.playing.load(), "and playback carries on from there");
        rig.seek(0); rig.render(257);
        require(rig.position() >= 0 && rig.position() < 20, "jumping back to the start works too");
    }
    // Stop and play again resumes from the requested position, not the old one.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        rig.publish(song); rig.seek(ticksPerBar); rig.play(); rig.render(barFrames / 2);
        rig.engine.playing.store(false); rig.render(1024);
        require(rig.position() == -1, "the playhead reports not-playing when stopped");
        rig.seek(2 * ticksPerBar); rig.play(); rig.render(300);
        require(rig.position() >= 2 * ticksPerBar && rig.position() < 2 * ticksPerBar + 20, "play starts at the position that was set");
        rig.engine.playing.store(false); rig.render(512); rig.play(); rig.render(300);
        require(rig.position() >= 2 * ticksPerBar && rig.position() < 2 * ticksPerBar + 20, "and again from the same place next time");
    }
}

// ---- loop ranges ---------------------------------------------------------------------------

static void loopTests() {
    // A loop of bar two: the note at the top of the bar returns on every pass, at exactly the loop length.
    {
        Rig rig; auto song = newSong(8);
        song->clips[0][0].pattern = oneNote(60, 0, 2);
        place(*song, regionOf(0, 0, 0, 8 * ticksPerBar));
        require(song->timeline.setLoop(ticksPerBar, 2 * ticksPerBar) && song->timeline.setLoopEnabled(true), "loop bar two");
        rig.publish(song); rig.seek(0); rig.play(); rig.render(5 * barFrames);
        require(rig.engine.playing.load(), "a loop plays on");
        for (int pass = 1; pass <= 4; ++pass) {
            const long top = static_cast<long>(pass) * barFrames;
            const auto onset = rig.firstAbove(top - 1000, top + 2000);
            require(onset >= top && onset <= top + 400, "the note returns on the loop start every pass");
            require(rig.energy(top - 60000, top - 1000) == 0, "the loop is quiet between notes");
        }
        require(rig.position() >= ticksPerBar && rig.position() < 2 * ticksPerBar, "the playhead stays inside the loop");
    }
    // Wrapping lands exactly: the playhead is never left past the loop end by the block size.
    {
        Rig rig; auto song = newSong(8);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 8 * ticksPerBar));
        require(song->timeline.setLoop(ticksPerBar, 2 * ticksPerBar) && song->timeline.setLoopEnabled(true), "loop");
        rig.publish(song); rig.seek(0); rig.play();
        rig.render(2 * barFrames);   // exactly one pass through bar one and one through the loop
        closeTo(static_cast<double>(rig.position()), ticksPerBar, 1.0, "after one pass the playhead is back at the loop start");
        rig.render(barFrames / 2);
        closeTo(static_cast<double>(rig.position()), ticksPerBar * 1.5, 1.0, "and half a pass later it is half way through");
        for (int i = 0; i < 40; ++i) {
            rig.render(1999);
            require(rig.position() >= ticksPerBar - 1 && rig.position() < 2 * ticksPerBar, "never outside the loop");
        }
    }
    // Loop off: it plays through and stops at the end of the song.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(ticksPerBar, 2 * ticksPerBar), "a loop range that is switched off");
        rig.publish(song); rig.seek(0); rig.play(); rig.render(5 * barFrames);
        require(!rig.engine.playing.load(), "a loop that is off does not loop");
        require(rig.energy(4 * barFrames, 5 * barFrames) == 0, "silent after the end");
    }
    // Starting after the loop end plays on to the end of the song instead of jumping back.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(0, ticksPerBar) && song->timeline.setLoopEnabled(true), "loop bar one");
        rig.publish(song); rig.seek(2 * ticksPerBar); rig.play();
        for (int i = 0; i < 100 && rig.engine.playing.load(); ++i) {
            rig.render(2000);
            require(rig.position() < 0 || rig.position() >= 2 * ticksPerBar, "never wraps back past a loop that was already behind");
        }
        require(!rig.engine.playing.load() && rig.frames() >= 2 * barFrames - 2000 && rig.frames() <= 2 * barFrames + 4000, "plays the remaining two bars and stops");
    }
    // Starting inside the loop, it wraps at the end of the loop and not the end of the song.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(ticksPerBar, 2 * ticksPerBar) && song->timeline.setLoopEnabled(true), "loop bar two");
        rig.publish(song); rig.seek(ticksPerBar + ticksPerBar / 2); rig.play(); rig.render(3 * barFrames);
        require(rig.engine.playing.load() && rig.position() >= ticksPerBar && rig.position() < 2 * ticksPerBar, "starting in the loop stays in the loop");
    }
    // Switching the loop on and moving it during playback takes effect without a restart.
    {
        Rig rig; auto song = newSong(8);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 8 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames / 2);
        auto looped = std::make_shared<alien::ArrangementPlayback>(*song);
        require(looped->timeline.setLoop(0, ticksPerBar) && looped->timeline.setLoopEnabled(true), "loop bar one while playing");
        rig.publish(looped); rig.render(barFrames);
        require(rig.engine.playing.load() && rig.position() >= 0 && rig.position() < ticksPerBar, "a loop switched on mid-song wraps at its end");
    }
}

// ---- Channel Rack patterns on the timeline ---------------------------------------------------------

static void rackLaneTests() {
    // A pattern on the first lane plays its steps from the region start.
    {
        Rig rig; auto song = newSong(4);
        place(*song, regionOf(rackRowBase, 0, ticksPerBar, ticksPerBar));
        rig.publishRack(rackWith({{0, 8}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        const long hit = barFrames + 8 * stepFrames;
        require(rig.energy(0, hit - 1) == 0, "nothing before the hit");
        const auto first = rig.firstAbove(0, 2 * barFrames);
        require(first >= hit && first <= hit + 40, "the step sounds on its sixteenth of the region");
        require(rig.energy(2 * barFrames + 30000, 3 * barFrames) == 0, "and nothing after the region ends");
    }
    // The pattern repeats for as long as the region lasts.
    {
        Rig rig; auto song = newSong(4);
        place(*song, regionOf(rackRowBase, 0, 0, 3 * ticksPerBar));
        rig.publishRack(rackWith({{0, 0}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(4 * barFrames);
        for (int bar = 0; bar < 3; ++bar) {
            const auto onset = rig.firstAbove(static_cast<long>(bar) * barFrames - 40, static_cast<long>(bar + 1) * barFrames);
            require(onset >= static_cast<long>(bar) * barFrames && onset <= static_cast<long>(bar) * barFrames + 40, "the first step sounds at the top of every bar of the region");
        }
        require(rig.energy(3 * barFrames + 30000, 4 * barFrames) == 0, "nothing after the third bar");
    }
    // Swing delays every second sixteenth, by the same amount as the Channel Rack itself.
    {
        auto timeOfStepOne = [](float swing) {
            Rig rig; auto song = newSong(2);
            place(*song, regionOf(rackRowBase, 0, 0, ticksPerBar));
            auto project = rackWith({{0, 1}}); project.setSwing(swing);
            rig.publishRack(project); rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
            return rig.firstAbove(0, barFrames);
        };
        const auto straight = timeOfStepOne(0.0f), swung = timeOfStepOne(1.0f);
        require(straight >= stepFrames && straight <= stepFrames + 40, "an odd step is on its sixteenth without swing");
        closeTo(static_cast<double>(swung - straight), 120 * tickFrames, 3, "full swing is half a sixteenth later");
    }
    // A region that starts part-way into the pattern (trimmed at the front) begins at that step.
    {
        Rig rig; auto song = newSong(4);
        auto r = regionOf(rackRowBase, 0, ticksPerBar, ticksPerBar); r.offset = 4 * ticksPerStep;
        place(*song, r);
        rig.publishRack(rackWith({{0, 4}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        const auto first = rig.firstAbove(0, 3 * barFrames);
        require(first >= barFrames && first <= barFrames + 40, "step five of the pattern lands on the region's first instant when the region starts there");
    }
    // Starting in the middle of a region fires only the steps still to come.
    {
        Rig rig; auto song = newSong(4);
        place(*song, regionOf(rackRowBase, 0, 0, 2 * ticksPerBar));
        rig.publishRack(rackWith({{0, 0}, {0, 2}, {0, 3}}));
        rig.publish(song); rig.seek(2 * ticksPerStep + ticksPerStep / 2); rig.play(); rig.render(barFrames);
        const auto first = rig.firstAbove(0, barFrames);
        // Step 3 is at tick 720; the start was 600 ticks in, so it is 120 ticks (3000 frames) away.
        require(first >= 2960 && first <= 3040, "steps already past are not played, the next one is");
    }
    // Where the region ends cuts the pattern: a step due after the end never sounds.
    {
        Rig rig; auto song = newSong(2);
        place(*song, regionOf(rackRowBase, 0, 0, 8 * ticksPerStep));
        rig.publishRack(rackWith({{0, 8}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        require(rig.energy(0, barFrames) == 0, "a step after the region's end does not sound");
        Rig other; auto second = newSong(2);
        place(*second, regionOf(rackRowBase, 0, 0, 8 * ticksPerStep));
        other.publishRack(rackWith({{0, 7}})); other.publish(second); other.seek(0); other.play(); other.render(barFrames);
        require(other.energy(0, barFrames) > 0.0001, "and the last step inside it does");
    }
    // Patterns on different lanes play together; they are separate layers.
    {
        auto project = rackWith({{0, 0}});
        require(project.addPattern(), "second pattern"); require(project.setStepOn(1, 3, 0, true), "snare on the second pattern");
        auto run = [&](bool both) {
            Rig rig; auto song = newSong(2);
            place(*song, regionOf(rackRowBase, 0, 0, ticksPerBar));
            if (both) place(*song, regionOf(rackRowBase + 1, 1, 0, ticksPerBar));
            rig.publishRack(project); rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
            return rig.energy(0, barFrames);
        };
        const double one = run(false), two = run(true);
        require(one > 0.001 && two > one * 1.1, "a second lane adds its sound to the first");
    }
    // Muting a region silences it; unmuting is a normal region again.
    {
        Rig rig; auto song = newSong(2);
        auto r = regionOf(rackRowBase, 0, 0, ticksPerBar); r.muted = true;
        place(*song, r);
        rig.publishRack(rackWith({{0, 0}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        require(rig.energy(0, barFrames) == 0, "a muted pattern region is silent");
    }
    // A 32-step pattern spans two bars of the timeline.
    {
        auto project = rackWith({{0, 20}});
        require(project.setPatternLength(0, 32), "32 steps");
        Rig rig; auto song = newSong(4);
        place(*song, regionOf(rackRowBase, 0, 0, 2 * ticksPerBar));
        rig.publishRack(project); rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        const auto first = rig.firstAbove(0, 3 * barFrames);
        require(first >= 20 * stepFrames && first <= 20 * stepFrames + 40, "step 21 of a 32-step pattern is in the second bar");
    }
    // Jumping elsewhere cuts the drums that are still sounding.
    {
        Rig rig; auto song = newSong(8);
        place(*song, regionOf(rackRowBase, 0, 0, ticksPerBar));
        rig.publishRack(rackWith({{0, 0}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(3000);
        require(rig.energy(1000, 3000) > 0.001, "the kick is sounding");
        rig.seek(5 * ticksPerBar); rig.render(9000);
        require(rig.energy(3000 + 3000, 3000 + 9000) < 1.0e-9, "jumping away silences it; nothing rings on");
    }
    // The Channel Rack's own playback and the timeline's do not disturb each other.
    {
        Rig rig; auto song = newSong(2);
        place(*song, regionOf(rackRowBase, 0, 0, ticksPerBar));
        rig.publishRack(rackWith({{0, 0}})); rig.publish(song);
        rig.engine.patternMode.store(3); rig.play(); rig.render(barFrames / 4);
        require(rig.energy(0, barFrames / 4) > 0.0001, "the Channel Rack tab still plays its own pattern");
        rig.engine.playing.store(false); rig.render(2048);
        rig.engine.patternMode.store(2); rig.seek(0); rig.play(); rig.render(barFrames);
        require(rig.firstAbove(barFrames / 4 + 2048, 2 * barFrames) >= 0, "and the Arrangement tab plays the timeline afterwards");
    }
}

// ---- audio clips -------------------------------------------------------------------------------------

static void audioTests() {
    const auto clip = makeSine(2);
    // Whole clip: an audio region of one bar plays the clip from the start.
    std::vector<float> reference;
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames + 2000);
        require(rig.energy(0, barFrames) > 100, "an audio region plays its clip");
        reference = rig.out;
        require(rig.energy(barFrames + 100, barFrames + 2000) == 0, "and is silent after it");
    }
    // Starting half way through the clip plays the second half, sample for sample.
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(ticksPerBar / 2); rig.play(); rig.render(barFrames / 2);
        double worst = 0;
        for (long i = 64; i < barFrames / 2 - 100; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(rig.out[static_cast<std::size_t>(i)] - reference[static_cast<std::size_t>(barFrames / 2 + i)])));
        require(worst < 1.0e-5, "starting mid-clip is the same audio as playing up to that point");
        require(rig.energy(0, 64) < 0.5 * [&] { double s = 0; for (long i = 0; i < 64; ++i) s += double(reference[static_cast<std::size_t>(barFrames / 2 + i)]) * reference[static_cast<std::size_t>(barFrames / 2 + i)]; return s; }(),
            "the first few milliseconds fade in rather than click");
    }
    // Trimming the front (a clip offset) starts the clip later in its own time.
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        auto r = regionOf(0, 0, 0, ticksPerBar / 2); r.audioOffset = 0.5;
        place(*song, r);
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames / 2);
        Rig plain; auto song2 = newSong(2);
        song2->clips[0][0].audio = &clip;
        place(*song2, regionOf(0, 0, 0, ticksPerBar));
        plain.publish(song2); plain.seek(ticksPerBar / 4); plain.play(); plain.render(barFrames / 2);
        double worst = 0;
        for (long i = 0; i < barFrames / 2 - 100; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(rig.out[static_cast<std::size_t>(i)] - plain.out[static_cast<std::size_t>(i)])));
        require(worst < 1.0e-6, "a trimmed-start region plays the same audio as starting there");
    }
    // A region shorter than the clip stops at its end, without a click.
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, ticksPerBar / 2));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        require(rig.energy(barFrames / 2 + 200, barFrames) == 0, "silent soon after the region's end");
        double biggestStep = 0;
        for (long i = barFrames / 2 - 5; i < barFrames / 2 + 200; ++i)
            biggestStep = std::max(biggestStep, static_cast<double>(std::abs(rig.out[static_cast<std::size_t>(i + 1)] - rig.out[static_cast<std::size_t>(i)])));
        require(biggestStep < 0.05, "the cut at the end of the region is a short fade, not a jump");
    }
    // Tempo does not stretch audio: the clip keeps its speed while the grid moves.
    {
        Rig rig; rig.engine.bpm.store(60); auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(4 * 48000);
        double worst = 0;
        for (long i = 64; i < 2 * 48000 - 100; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(rig.out[static_cast<std::size_t>(i)] - reference[static_cast<std::size_t>(i)])));
        require(worst < 1.0e-6, "at half the tempo the audio is unchanged (it is not stretched)");
    }
}

// ---- edits while it plays ---------------------------------------------------------------------------------

static void liveEditTests() {
    const auto clip = makeSine(2);
    // Making a playing region longer is invisible: the output equals the song that was always that way.
    {
        Rig whole; auto full = newSong(2);
        full->clips[0][0].audio = &clip;
        place(*full, regionOf(0, 0, 0, ticksPerBar));
        whole.publish(full); whole.seek(0); whole.play(); whole.render(barFrames);

        Rig edited; auto shortSong = newSong(2);
        shortSong->clips[0][0].audio = &clip;
        place(*shortSong, regionOf(0, 0, 0, ticksPerBar / 2));
        edited.publish(shortSong); edited.seek(0); edited.play(); edited.render(20000);
        edited.publish(full); edited.render(barFrames - 20000);
        double worst = 0;
        for (long i = 0; i < barFrames; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(edited.out[static_cast<std::size_t>(i)] - whole.out[static_cast<std::size_t>(i)])));
        require(worst < 1.0e-6, "lengthening a region while it plays does not disturb the sound");
    }
    // Removing it fades it out within a few milliseconds and the song carries on.
    {
        Rig rig; auto song = newSong(2);
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(20000);
        auto emptied = newSong(2);
        emptied->clips[0][0].audio = &clip;
        rig.publish(emptied); rig.render(5000);
        require(rig.energy(20000 + 600, 25000) == 0, "deleting the region silences it");
        double biggestStep = 0;
        for (long i = 19990; i < 20800; ++i) biggestStep = std::max(biggestStep, static_cast<double>(std::abs(rig.out[static_cast<std::size_t>(i + 1)] - rig.out[static_cast<std::size_t>(i)])));
        require(biggestStep < 0.05, "without a click");
        require(rig.engine.playing.load() && rig.position() > 0, "and the song keeps playing");
    }
    // Adding a note to the clip that is playing: it sounds when its time comes, exactly as if it had always been there.
    {
        auto before = oneNote(60, 8, 2);
        auto after = before; require(after.add(67, 12, 2, 0.8f) != 0, "add a note");
        Rig whole; auto full = newSong(2);
        full->clips[0][0].pattern = after;
        place(*full, regionOf(0, 0, 0, 2 * ticksPerBar));
        whole.publish(full); whole.seek(0); whole.play(); whole.render(barFrames);

        Rig edited; auto first = newSong(2);
        first->clips[0][0].pattern = before;
        place(*first, regionOf(0, 0, 0, 2 * ticksPerBar));
        edited.publish(first); edited.seek(0); edited.play(); edited.render(3 * stepFrames);   // before the first note
        edited.publish(full); edited.render(barFrames - 3 * stepFrames);
        double worst = 0;
        for (long i = 0; i < barFrames; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(edited.out[static_cast<std::size_t>(i)] - whole.out[static_cast<std::size_t>(i)])));
        require(worst < 1.0e-6, "a note added to the playing clip is heard on time, with nothing else disturbed");
    }
    // Moving a region so it no longer covers the playhead releases it; moving it back brings it in.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(20000);
        auto moved = newSong(4);
        moved->clips[0][0].pattern = held();
        place(*moved, regionOf(0, 0, 2 * ticksPerBar, ticksPerBar));
        rig.publish(moved); rig.render(40000);
        require(rig.energy(20000 + 30000, 60000) == 0, "a region moved away stops sounding (after its release)");
        rig.render(2 * barFrames - 60000 + 4000);
        require(rig.energy(2 * barFrames + 200, 2 * barFrames + 4000) > 0.0001, "and sounds in its new place");
        require(rig.engine.playing.load() || rig.frames() > 3 * barFrames, "the song kept its place throughout");
    }
    // Moving a playing pattern region lines its steps up with the new position: the next step is the first one still to come there.
    {
        Rig rig; auto song = newSong(8);
        place(*song, regionOf(rackRowBase, 0, 0, 4 * ticksPerBar));
        rig.publishRack(rackWith({{2, 3}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(39000);   // 1560 ticks in: step 3 has sounded, step 4 and on are still to come
        require(rig.energy(37000, 39000) < 1.0e-9, "the closed hat has died away before the move");
        auto moved = newSong(8);
        place(*moved, regionOf(rackRowBase, 0, 4 * ticksPerStep, 4 * ticksPerBar));
        rig.publish(moved); rig.render(9000);
        const auto onset = rig.firstAbove(39000, 48000);
        // In its new place step 3 is due at tick 960 + 720 = 1680: 120 ticks (3000 frames) after the move.
        require(onset >= 41800 && onset <= 42400, "after a move the pattern's steps line up with the new position, and the next one still comes");
    }
    // Edits never move the playhead.
    {
        Rig rig; auto song = newSong(8);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 8 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        std::int64_t last = rig.position();
        for (int i = 0; i < 20; ++i) {
            auto next = std::make_shared<alien::ArrangementPlayback>(*song);
            Region extra = regionOf(1, 0, static_cast<std::int64_t>(i % 5) * ticksPerBar, ticksPerBar / 2);
            next->timeline.place(extra);
            next->timeline.addMarker(ticksPerBar * (i % 7), "m");
            rig.publish(next); rig.render(3000);
            require(rig.position() > last, "the playhead only ever moves forward through edits");
            last = rig.position();
        }
    }
    // Tempo changes during playback: the playhead continues without a jump and moves at the new rate.
    {
        Rig rig; auto song = newSong(8);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 8 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        closeTo(static_cast<double>(rig.position()), ticksPerBar, 1.0, "one bar at 120");
        rig.engine.bpm.store(240); rig.render(barFrames / 2);
        closeTo(static_cast<double>(rig.position()), 2 * ticksPerBar, 1.0, "half a bar's frames at twice the tempo is a whole bar");
        rig.engine.bpm.store(60); rig.render(barFrames / 2);
        closeTo(static_cast<double>(rig.position()), 2.25 * ticksPerBar, 1.0, "and at half the tempo, a quarter bar");
    }
}

// ---- the whole song ------------------------------------------------------------------------------------------

static void exactnessTests() {
    // Sixteen bars at an awkward block size: the playhead is on every bar line when it should be.
    {
        Rig rig; auto song = newSong(16);
        song->clips[0][0].pattern = oneNote(60, 0, 1);
        place(*song, regionOf(0, 0, 0, 16 * ticksPerBar));
        rig.block = 1021;
        rig.publish(song); rig.seek(0); rig.play();
        for (int bar = 1; bar < 16; ++bar) {
            rig.render(barFrames);
            closeTo(static_cast<double>(rig.position()), static_cast<double>(bar) * ticksPerBar, 1.0, "the playhead is on the bar line after whole bars of audio");
        }
        // Every bar has the same note at the same place: no drift in the note timing across the song.
        long previous = -1;
        for (int bar = 0; bar < 15; ++bar) {
            const auto onset = rig.firstAbove(static_cast<long>(bar) * barFrames, static_cast<long>(bar + 1) * barFrames) - static_cast<long>(bar) * barFrames;
            require(onset >= 0 && onset < 400, "a note at the top of the bar is at the top of the bar");
            require(previous < 0 || onset == previous, "and at exactly the same place in every bar");
            previous = onset;
        }
    }
    // The block size changes nothing about what is heard.
    {
        auto run = [&](int block) {
            Rig rig; rig.block = block; auto song = newSong(2);
            song->clips[0][0].pattern = oneNote(60, 4, 4); song->clips[1][0].pattern = oneNote(67, 2, 6);
            place(*song, regionOf(0, 0, 0, ticksPerBar)); place(*song, regionOf(0, 1, ticksPerBar, ticksPerBar));
            place(*song, regionOf(rackRowBase, 0, 0, 2 * ticksPerBar));
            auto project = rackWith({{0, 0}, {0, 6}, {3, 4}}); project.setSwing(0.6f);
            rig.publishRack(project); rig.publish(song); rig.seek(2 * ticksPerStep + 40); rig.play(); rig.render(2 * barFrames);
            return rig.out;
        };
        const auto a = run(64), b = run(257), c = run(1024);
        double worstAB = 0, worstAC = 0;
        for (std::size_t i = 0; i < a.size(); ++i) { worstAB = std::max(worstAB, static_cast<double>(std::abs(a[i] - b[i]))); worstAC = std::max(worstAC, static_cast<double>(std::abs(a[i] - c[i]))); }
        require(worstAB < 1.0e-5 && worstAC < 1.0e-5, "the same song sounds the same at any callback size");
    }
}

// ---- details that are easy to get slightly wrong ---------------------------------------------------------

static void detailTests() {
    // Two clips that meet end to start: which one plays does not depend on the order the song lists them in.
    {
        auto build = [&](bool reversed) {
            auto song = newSong(4);
            song->clips[0][0].pattern = held(60);
            song->clips[1][0].pattern = held(72);
            const Region first = regionOf(0, 0, 0, ticksPerBar), second = regionOf(0, 1, ticksPerBar, ticksPerBar);
            if (reversed) { place(*song, second); place(*song, first); } else { place(*song, first); place(*song, second); }
            return song;
        };
        Rig forward, backward;
        forward.publish(build(false)); forward.seek(0); forward.play(); forward.render(2 * barFrames);
        backward.publish(build(true)); backward.seek(0); backward.play(); backward.render(2 * barFrames);
        double worst = 0;
        for (long i = 0; i < 2 * barFrames; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(forward.out[static_cast<std::size_t>(i)] - backward.out[static_cast<std::size_t>(i)])));
        require(forward.energy(barFrames, barFrames + 20000) > 0.001 && worst < 1.0e-9, "the clip that begins where another ends takes over cleanly, whichever is listed first");
    }
    // A muted clip is silent, and an unmuted one beside it is not.
    {
        auto run = [&](bool muted) {
            Rig rig; auto song = newSong(2);
            song->clips[0][0].pattern = held();
            auto r = regionOf(0, 0, 0, ticksPerBar); r.muted = muted;
            place(*song, r);
            rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
            return rig.energy(0, barFrames);
        };
        require(run(false) > 0.001, "a clip that is not muted sounds");
        require(run(true) == 0, "a muted clip is silent");
    }
    // Jumping somewhere else while playing silences the old sound at once, release tails included.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(20000);
        require(rig.energy(10000, 20000) > 0.001, "the note is sounding before the jump");
        rig.seek(3 * ticksPerBar); rig.render(5000);
        require(rig.energy(20000, 25000) == 0, "a jump to somewhere with nothing in it cuts the old note dead");
        require(rig.position() >= 3 * ticksPerBar, "and the playhead went there");
    }
    // A clip trimmed at the front plays from the part that is left: its first sound is that part's first sound.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = oneNote(60, 8, 2);   // sounds half way through its bar
        const auto id = place(*song, regionOf(0, 0, 0, 3 * ticksPerBar));
        require(song->timeline.trimStart(id, 8 * ticksPerStep), "hide the first half bar of the clip");
        rig.publish(song); rig.seek(0); rig.play(); rig.render(2 * barFrames);
        const auto first = rig.firstAbove(0, 2 * barFrames);
        require(first >= 8 * stepFrames && first <= 8 * stepFrames + 400, "the region starts half a bar in, and its content starts with that note");
    }
    // Moving a clip that is playing makes it follow its new place: its notes now fall on the new bar lines.
    {
        auto build = [&](std::int64_t start) {
            auto song = newSong(4);
            song->clips[0][0].pattern = oneNote(60, 0, 2);
            place(*song, regionOf(0, 0, start, 3 * ticksPerBar));
            return song;
        };
        Rig rig;
        rig.publish(build(0)); rig.seek(0); rig.play(); rig.render(barFrames + barFrames / 2);
        const long now = rig.frames();
        rig.publish(build(4 * ticksPerStep)); rig.render(2 * barFrames);
        const long next = 2 * barFrames + 4 * stepFrames;   // four sixteenths after the old bar line
        require(rig.energy(now + 1000, next - 200) == 0, "no note falls on the old bar line");
        const auto onset = rig.firstAbove(now, 4 * barFrames);
        require(onset >= next && onset <= next + 400, "the next note comes four sixteenths later, with the moved clip");
    }
    // The same for a Channel Rack pattern.
    {
        auto build = [&](std::int64_t start) {
            auto song = newSong(4);
            place(*song, regionOf(rackRowBase, 0, start, 3 * ticksPerBar));
            return song;
        };
        Rig rig; rig.publishRack(rackWith({{0, 0}}));
        rig.publish(build(0)); rig.seek(0); rig.play(); rig.render(barFrames + barFrames / 2);
        const long now = rig.frames();
        rig.publish(build(4 * ticksPerStep)); rig.render(2 * barFrames);
        const long next = 2 * barFrames + 4 * stepFrames;
        require(rig.energy(now + 1000, next - 200) == 0, "no step falls on the old bar line");
        const auto onset = rig.firstAbove(now + 1000, 4 * barFrames);
        require(onset >= next && onset <= next + 40, "the moved pattern's first step comes four sixteenths later");
    }
    // A step that swing pushes past the end of its region does not sound.
    {
        auto run = [&](float swing) {
            Rig rig; auto song = newSong(2);
            place(*song, regionOf(rackRowBase, 0, 0, 300));   // step 2 is at tick 240; full swing puts it at 360
            auto project = rackWith({{0, 1}}); project.setSwing(swing);
            rig.publishRack(project); rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
            return rig.energy(0, barFrames);
        };
        require(run(0.0f) > 0.001, "without swing the step is inside the region and sounds");
        require(run(1.0f) == 0, "swung past the end of the region it does not");
    }
    // The loop wraps without losing the fraction of a sample it overshot: dozens of passes later it is still in time.
    {
        Rig rig; rig.engine.bpm.store(121);
        auto song = newSong(4);
        song->clips[0][0].pattern = oneNote(60, 0, 2);
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(0, ticksPerBar) && song->timeline.setLoopEnabled(true), "loop bar one");
        rig.publish(song); rig.seek(0); rig.play();
        const double pass = 48000.0 * 3840.0 / (121.0 * 960.0 / 60.0);   // frames in one bar at 121 BPM: not a whole number
        rig.render(static_cast<long>(pass * 31));
        const auto early = rig.firstAbove(static_cast<long>(pass * 1 - 50), static_cast<long>(pass * 1 + 2000));
        const auto late = rig.firstAbove(static_cast<long>(pass * 30 - 50), static_cast<long>(pass * 30 + 2000));
        require(early > 0 && late > 0, "the note comes round on every pass");
        closeTo(static_cast<double>(late - early), pass * 29, 3, "29 passes later the note is where 29 exact bars put it");
    }
    // What is ringing when the loop wraps finishes its release instead of being cut off.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = oneNote(60, 8, 8);
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(0, 12 * ticksPerStep) && song->timeline.setLoopEnabled(true), "a loop that ends while the note is held");
        rig.publish(song); rig.seek(0); rig.play();
        const long wrap = 12 * stepFrames;
        rig.render(wrap + 6000);
        require(rig.energy(wrap - 3000, wrap) > 0.001, "the note is sounding when the loop ends");
        require(rig.energy(wrap + 100, wrap + 1500) > 1.0e-6, "and its release rings on past the wrap");
    }
    // A rack pattern comes round with the loop on its first step every pass.
    {
        Rig rig; auto song = newSong(4);
        place(*song, regionOf(rackRowBase, 0, 0, 4 * ticksPerBar));
        require(song->timeline.setLoop(ticksPerBar, 2 * ticksPerBar) && song->timeline.setLoopEnabled(true), "loop bar two");
        rig.publishRack(rackWith({{0, 0}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(5 * barFrames);
        for (int pass = 1; pass <= 4; ++pass) {
            const long top = static_cast<long>(pass) * barFrames;
            const auto onset = rig.firstAbove(top - 40, top + 4000);
            require(onset >= top - 40 && onset <= top + 40, "the pattern's first step lands on the loop start every pass");
            require(rig.energy(top - 60000, top - 100) == 0, "and nothing else sounds between");
        }
    }
    // A song with an end point set (an export of part of it) stops there.
    {
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        rig.publish(song); rig.engine.timelineEnd.store(ticksPerBar); rig.seek(0); rig.play();
        rig.render(2 * barFrames);
        require(!rig.engine.playing.load(), "the song stops at the requested end");
        require(rig.energy(barFrames + 30000, 2 * barFrames) == 0, "and is silent after it");
    }
    // A MIDI clip followed by an audio clip on the same track: the notes end when the audio begins.
    {
        auto silent = makeSine(2);
        std::fill(silent.left.begin(), silent.left.end(), 0.0f);
        silent.buildWaveform();
        Rig rig; auto song = newSong(4);
        song->clips[0][0].pattern = held();
        song->clips[1][0].audio = &silent;
        place(*song, regionOf(0, 0, 0, ticksPerBar));
        place(*song, regionOf(0, 1, ticksPerBar, ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(2 * barFrames);
        require(rig.energy(barFrames - 20000, barFrames) > 0.001, "the MIDI clip sounds");
        require(rig.energy(barFrames + 30000, 2 * barFrames) == 0, "its notes do not hang on into the audio clip after it");
    }
    // Changing the loop length of a clip that is playing makes it follow the new length from the playhead on.
    {
        auto build = [&](int loopSteps) {
            auto song = newSong(4);
            alien::PianoRollPattern pattern; pattern.loopSteps = loopSteps;
            require(pattern.add(60, 0, 1, 0.8f) != 0, "a note on the downbeat");
            song->clips[0][0].pattern = pattern;
            place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
            return song;
        };
        Rig rig; rig.publish(build(16)); rig.seek(0); rig.play();
        rig.render(barFrames + 19200);   // 1.2 bars in
        const long now = rig.frames();
        rig.publish(build(8)); rig.render(barFrames);
        const long next = barFrames + barFrames / 2;
        require(rig.energy(now + 2000, next - 200) == 0, "nothing sounds until the shorter loop comes round");
        const auto onset = rig.firstAbove(next - 200, 2 * barFrames);
        require(onset >= next && onset <= next + 400, "and the downbeat of the shorter loop is half a bar after the last one");
    }
}

// ---- real-time safety ---------------------------------------------------------------------------------------------

static void realTimeTests() {
    const auto clip = makeSine(2);
    Rig rig; auto song = newSong(8);
    song->clips[0][0].pattern = oneNote(60, 0, 3); song->clips[1][0].pattern = held(64);
    song->clips[2][2].audio = &clip;
    place(*song, regionOf(0, 0, 0, 2 * ticksPerBar)); place(*song, regionOf(0, 1, 2 * ticksPerBar, 2 * ticksPerBar));
    place(*song, regionOf(2, 2, ticksPerBar / 2, ticksPerBar)); place(*song, regionOf(rackRowBase, 0, 0, 8 * ticksPerBar));
    place(*song, regionOf(rackRowBase + 1, 1, ticksPerBar, 4 * ticksPerBar));
    require(song->timeline.setLoop(ticksPerBar, 3 * ticksPerBar) && song->timeline.setLoopEnabled(true), "loop");
    auto project = rackWith({{0, 0}, {1, 4}, {2, 2}, {3, 12}});
    require(project.addPattern(), "second pattern"); require(project.setStepOn(1, 0, 8, true), "step");
    project.setSwing(0.4f);
    rig.publishRack(project); rig.publish(song);
    rig.seek(0); rig.play(); rig.render(2048);   // warm up: first use of every code path
    auto edited = std::make_shared<alien::ArrangementPlayback>(*song);
    require(edited->timeline.setLoopEnabled(false), "loop off");
    const auto sparse = std::make_shared<alien::ArrangementPlayback>(*song);
    const auto before = allocationCount;
    std::array<float, 311> l{}, r{}; float* outputs[]{l.data(), r.data()};
    for (int i = 0; i < 2500; ++i) {
        if (i % 400 == 100) rig.engine.timelineSeek.store((i * 137) % (6 * ticksPerBar));
        if (i % 500 == 250) rig.engine.bpm.store(90 + (i % 7) * 20);
        if (i % 700 == 300) { rig.engine.playing.store(false); }
        if (i % 700 == 330) { rig.engine.playing.store(true); }
        rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {});
    }
    require(before == allocationCount, "playing, looping, seeking, tempo changes and starting and stopping never allocate on the audio thread");
    // Taking a new snapshot is also free of allocation (the snapshot was built beforehand).
    require(rig.engine.arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(edited)), "publish");
    const auto beforeSnapshot = allocationCount;
    rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {});
    require(beforeSnapshot == allocationCount, "taking a new snapshot does not allocate");
}


// ---- audio streamed from disk -----------------------------------------------------------------------------------
// Played through the real stream service and its worker thread. A source can be held back to see what the
// timeline does when a stream is late.

struct GatedFile final : alien::DiskAudioFile {
    struct Reader final : alien::DiskAudioReader {
        explicit Reader(const GatedFile& owner) : file(owner) {}
        bool read(std::int64_t start, float* left, float*, int count) override {
            const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!file.gate.load() && std::chrono::steady_clock::now() < giveUp) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard<std::mutex> lock(file.mutex);
                if (file.firstStart < 0) file.firstStart = start;
                ++file.reads;
            }
            for (int i = 0; i < count; ++i) left[i] = value(start + i);
            return true;
        }
        const GatedFile& file;
    };
    GatedFile(std::int64_t frameCount, bool open) : DiskAudioFile(48000.0, 1, frameCount), gate(open) {}
    std::unique_ptr<alien::DiskAudioReader> open() const override { return std::make_unique<Reader>(*this); }
    static float value(std::int64_t i) {
        return static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979323846 * 441.0 * static_cast<double>(i) / 48000.0));
    }
    int readCount() const { std::lock_guard<std::mutex> lock(mutex); return reads; }
    std::int64_t firstRead() const { std::lock_guard<std::mutex> lock(mutex); return firstStart; }
    mutable std::mutex mutex;
    mutable std::int64_t firstStart{-1};
    mutable int reads{};
    std::atomic<bool> gate;
};

struct StreamedRig {
    std::shared_ptr<GatedFile> file;
    alien::SessionAudioClip clip;
    Rig rig;
    std::shared_ptr<alien::ArrangementPlayback> song = newSong(4);
    explicit StreamedRig(bool gateOpen) : file(std::make_shared<GatedFile>(96000, gateOpen)) {
        clip.sampleRate = 48000; clip.channels = 1; clip.frames = 96000; clip.streamSource = file;
        require(clip.valid(), "streamed test clip");
        require(rig.engine.retainDiskAudioSource(file) == file.get(), "the engine keeps the source");
        song->clips[0][0].audio = &clip;
    }
    ~StreamedRig() { file->gate.store(true); }   // the worker must not be left waiting when the engine goes
    void start(std::int64_t tick = 0) { rig.publish(song); rig.seek(tick); rig.play(); }
};
template <class Predicate> static bool waitUntil(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
static double worstDifference(const std::vector<float>& a, const std::vector<float>& b, long from, long to) {
    double worst = 0;
    for (long i = from; i < to; ++i)
        worst = std::max(worst, static_cast<double>(std::abs(a[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)])));
    return worst;
}
// What the same song sounds like with the clip in memory.
static std::vector<float> decodedReference(const alien::SessionAudioClip& clip, std::int64_t start, std::int64_t length, double offset, long frames) {
    Rig rig; auto song = newSong(4); song->clips[0][0].audio = &clip;
    auto r = regionOf(0, 0, start, length); r.audioOffset = offset; place(*song, r);
    rig.publish(song); rig.seek(0); rig.play(); rig.render(frames);
    return rig.out;
}

static void streamedTests() {
    const auto decoded = makeSine(2);
    // A stream that is ready: asked for a quarter of a second ahead, from where the region begins in the clip,
    // and then the same sound as the clip in memory.
    {
        StreamedRig s(true);
        auto r = regionOf(0, 0, ticksPerBar, ticksPerBar / 2); r.audioOffset = 0.25; place(*s.song, r);
        s.start();
        s.rig.render(barFrames - 13000);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        require(s.file->readCount() == 0, "a streamed clip is not read until a quarter of a second before it is needed");
        s.rig.render(1500);
        require(waitUntil([&] { return s.file->readCount() > 0; }), "it is asked for ahead of time");
        require(s.file->firstRead() == 12000, "...from where the region begins inside the clip");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s.rig.renderPaced(2 * barFrames - s.rig.frames());
        const auto reference = decodedReference(decoded, ticksPerBar, ticksPerBar / 2, 0.25, 2 * barFrames);
        require(s.rig.energy(barFrames, barFrames + barFrames / 2) > 100, "the streamed region is heard");
        // Checked first, so a failure says whether the stream ran dry or the audio really differs.
        require(s.rig.engine.sessionStreamUnderruns(0) == 0 && !s.rig.engine.sessionStreamFailed(0), "with no underruns");
        require(worstDifference(s.rig.out, reference, 0, 2 * barFrames) < 1.0e-6, "a prefetched streamed clip is the same sound as the clip in memory");
    }
    // Starting the song in the middle of a streamed region: it is asked for from that point.
    {
        StreamedRig s(false);
        place(*s.song, regionOf(0, 0, 0, ticksPerBar));
        s.start(ticksPerBar / 2);   // a second in
        s.rig.render(10);
        require(s.rig.energy(0, 10) == 0, "the gated stream starts late");
        s.file->gate.store(true);
        // Each read supplies 1024 frames. Entering the third read means the worker has
        // published the first two and marked the stream ready. A fixed sleep does not
        // establish that. Stay below those 2048 frames: this is a launch-alignment test,
        // not a race between an unpaced offline renderer and background disk refill.
        require(waitUntil([&] { return s.file->readCount() >= 3; }) && s.file->firstRead() == 48000,
                "playing from the middle prebuffers from the middle of the clip");
        s.rig.render(1500);
        const auto reference = decodedReference(decoded, 0, ticksPerBar, 0.0, barFrames);
        require(s.rig.energy(10, 1500) > 100, "and is heard");
        require(s.rig.engine.sessionStreamUnderruns(0) == 0, "the launch comparison stays within prebuffered audio");
        require(worstDifference(s.rig.out, std::vector<float>(reference.begin() + 48000, reference.end()), 100, 1500) < 1.0e-5,
                "starting a moment late because the stream was not yet buffered keeps the audio in time");
    }
    // A stream that arrives late joins the song where it has got to, rather than starting late. While it is late the
    // stream is asked again for a point a little ahead of the song (0.1 s, then 0.2 s, ... ahead) and the audio starts
    // when the song gets there. The region begins at frame 24000.
    auto lateStream = [&](long framesBeforeArrival, long earliest, long latest, const char* what) {
        StreamedRig s(false);
        place(*s.song, regionOf(0, 0, ticksPerBar / 4, ticksPerBar / 2));
        s.start();
        s.rig.render(24000 + framesBeforeArrival);
        require(s.rig.energy(0, s.rig.frames()) == 0, "nothing is heard while the stream is not ready");
        s.file->gate.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the stream is ready long before the song needs it
        const long resumed = s.rig.frames();
        s.rig.renderPaced(30000);
        const long heard = s.rig.firstAbove(resumed, s.rig.frames());
        std::cerr << what << ": first heard at frame " << heard << '\n';
        require(heard >= earliest && heard <= latest, what);
        const auto reference = decodedReference(decoded, ticksPerBar / 4, ticksPerBar / 2, 0.0, static_cast<int>(s.rig.frames()));
        // Checked first, so a failure says whether the stream ran dry or the audio really is out of time.
        if (s.rig.engine.sessionStreamUnderruns(0) != 0) std::cerr << "underruns: " << s.rig.engine.sessionStreamUnderruns(0) << '\n';
        require(s.rig.engine.sessionStreamUnderruns(0) == 0, "joining late is not an underrun");
        require(worstDifference(s.rig.out, reference, heard + 70, heard + 20000) < 1.0e-5, "and it is in time with the song: the same audio as if it had been ready");
    };
    lateStream(3000, 29810, 29930, "a stream that arrives early in its wait starts when the song reaches the point it was asked for (0.1 s ahead)");
    lateStream(12000, 40430, 40560, "a stream that is later still is asked for further ahead each time and starts there");
    {   // Arriving only just late, inside the window, it starts at once where the song is.
        StreamedRig s(false);
        place(*s.song, regionOf(0, 0, ticksPerBar / 4, ticksPerBar / 2));
        s.start();
        s.rig.render(24000 + 500);
        s.file->gate.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const long resumed = s.rig.frames();
        s.rig.renderPaced(20000);
        const long heard = s.rig.firstAbove(resumed, s.rig.frames());
        require(heard >= resumed && heard < resumed + 200, "a stream that is only a little late starts as soon as it is ready");
        const auto reference = decodedReference(decoded, ticksPerBar / 4, ticksPerBar / 2, 0.0, static_cast<int>(s.rig.frames()));
        if (s.rig.engine.sessionStreamUnderruns(0) != 0) std::cerr << "underruns: " << s.rig.engine.sessionStreamUnderruns(0) << '\n';
        require(s.rig.engine.sessionStreamUnderruns(0) == 0, "a stream that starts a little late does not then run dry");
        require(worstDifference(s.rig.out, reference, heard + 70, heard + 15000) < 1.0e-5, "in time with the song");
    }
    // A stream that is still not ready when its region ends is never started late; the next clip is not disturbed.
    {
        StreamedRig s(false);
        alien::SessionAudioClip other = makeSine(2);
        s.song->clips[1][0].audio = &other;
        place(*s.song, regionOf(0, 0, 0, 2 * ticksPerStep));                      // frames 0 to 12000
        place(*s.song, regionOf(0, 1, ticksPerBar / 2, ticksPerBar / 4));          // frames 48000 to 72000, in memory
        s.start();
        s.rig.render(20000);
        s.file->gate.store(true);
        for (int i = 0; i < 40; ++i) { s.rig.render(257); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(s.rig.frames() > 30000 && s.rig.energy(0, 40000) == 0, "audio that arrives after its region is over is never played");
        s.rig.render(72000 - s.rig.frames() + 2000);
        require(s.rig.energy(48100, 72000) > 100, "the next clip on the track plays as usual");
    }
    // Jumping away from a stream that is still waiting forgets it.
    {
        StreamedRig s(false);
        place(*s.song, regionOf(0, 0, 0, ticksPerBar));
        s.start();
        s.rig.render(8000);
        s.rig.seek(3 * ticksPerBar);
        s.rig.render(2000);
        s.file->gate.store(true);
        for (int i = 0; i < 40; ++i) { s.rig.render(257); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(s.rig.energy(0, s.rig.frames()) == 0, "a stream the song jumped away from does not start when it arrives");
    }
    // None of this allocates on the audio thread.
    {
        StreamedRig s(true);
        place(*s.song, regionOf(0, 0, 0, ticksPerBar)); place(*s.song, regionOf(0, 0, 2 * ticksPerBar, ticksPerBar));
        s.start();
        std::array<float, 311> l{}, r{}; float* outputs[]{l.data(), r.data()};
        for (int i = 0; i < 50; ++i) { s.rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {}); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        const auto before = allocationCount;
        for (int i = 0; i < 600; ++i) {
            if (i % 150 == 10) s.rig.engine.timelineSeek.store(i % 300 == 10 ? 2 * ticksPerBar - 300 : ticksPerBar / 3);
            if (i % 150 == 70) s.rig.engine.timelineSeek.store(0);
            s.rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {});
            if (i % 20 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(before == allocationCount, "streaming, prefetching and catching up never allocate on the audio thread");
    }
}


// Set ALIEN_TEST_TIMING to see how long each group takes (a Debug build is much slower than Release).
template <class Test> static void timed(const char* name, Test test) {
    const auto begin = std::chrono::steady_clock::now();
    test();
    if (std::getenv("ALIEN_TEST_TIMING"))
        std::cerr << name << ": " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() << " ms\n";
}


static void automationPlaybackTests() {
    auto clip = makeSine(2);
    auto song = newSong(1); song->clips[0][0].audio = &clip;
    place(*song, regionOf(0, 0, 0, ticksPerBar));
    Rig reference; reference.publish(song); reference.play(); reference.render(4000);
    auto automated = std::make_shared<alien::ArrangementPlayback>(*song);
    automated->timeline.automationLanes = {
        {0, alien::automation::Parameter::volume, true, {{0, 0.0f}, {100, 1.0f}}},
        {0, alien::automation::Parameter::pan, true, {{0, 0.0f, alien::automation::Curve::hold},
            {50, 1.0f, alien::automation::Curve::hold}, {100, 0.0f, alien::automation::Curve::hold}}}
    };
    Rig one; one.block = 1; one.publish(automated); one.play(); one.render(4000);
    Rig awkward; awkward.block = 257; awkward.publish(automated); awkward.play(); awkward.render(4000);
    require(one.out == awkward.out, "automation is identical across single-sample and awkward blocks");
    for (std::size_t i = 0; i < one.out.size(); ++i) {
        const double tick = static_cast<double>(i) / tickFrames;
        const double gain = std::min(1.0, tick / 100.0);
        const double balance = tick >= 50.0 && tick < 100.0 ? 0.0 : 1.0;
        require(std::abs(one.out[i] - reference.out[i] * gain * balance) < 2.0e-6,
            "volume and pan breakpoints affect the exact intended output sample");
    }
    Rig seek; seek.publish(automated); seek.seek(50); seek.play(); seek.render(800);
    require(seek.energy(0, 800) == 0.0, "seek evaluates hold automation immediately");
}

static void sendAutomationPlaybackTests() {
    using alien::automation::Parameter;
    using alien::automation::Curve;
    auto clip = makeSine(2);
    for (auto& sample : clip.left) sample *= 0.1f; // keep summed returns below the output limiter
    auto song = newSong(1); song->clips[0][0].audio = &clip;
    place(*song, regionOf(0, 0, 0, ticksPerBar));
    Rig reference; reference.publish(song); reference.play(); reference.render(4000);
    auto automated = std::make_shared<alien::ArrangementPlayback>(*song);
    automated->timeline.automationLanes = {
        {0, Parameter::sendA, true, {{0, 0}, {100, 1}}},
        {0, Parameter::sendD, true, {{0, 0, Curve::hold}, {50, 0.5f, Curve::hold}}}
    };
    Rig one; one.block = 1; one.publish(automated); one.play(); one.render(4000);
    Rig awkward; awkward.block = 257; awkward.publish(automated); awkward.play(); awkward.render(4000);
    require(one.out == awkward.out, "send automation is block-size independent with manual sends off");
    for (std::size_t i = 0; i < one.out.size(); ++i) {
        const double tick = static_cast<double>(i) / tickFrames;
        const double gain = 1 + std::min(1.0, tick / 100) + (tick >= 50 ? 0.5 : 0);
        require(std::abs(one.out[i] - reference.out[i] * gain) < 2e-6, "send curves reach the returns at the intended sample");
    }
    // A send-only curve must not change fader smoothing. A pre-fader send
    // still plays with the dry fader at zero and still obeys mute/solo.
    auto preSong = std::make_shared<alien::ArrangementPlayback>(*song);
    preSong->timeline.automationLanes = {{0, Parameter::sendB, true, {{0, 1}}}};
    Rig pre; pre.engine.tracks[0].gain.store(0); pre.engine.tracks[0].sendPre[1].store(true);
    pre.render(4000); pre.out.clear(); pre.publish(preSong); pre.play(); pre.render(4000);
    for (std::size_t i = 0; i < pre.out.size(); ++i) require(std::abs(pre.out[i] - reference.out[i]) < 2e-6, "pre-fader automation ignores dry gain");
    pre.engine.tracks[0].mute.store(true); pre.out.clear(); pre.render(800);
    require(pre.energy(0, 800) == 0, "mute cuts automated pre sends");
    Rig solo; solo.engine.tracks[1].solo.store(true); solo.engine.tracks[0].sendPre[1].store(true);
    solo.render(4000); solo.publish(preSong); solo.play(); solo.out.clear(); solo.render(800);
    require(solo.energy(0, 800) == 0, "another track solo cuts automated pre sends");
    Rig seek; seek.publish(automated); seek.seek(100); seek.play(); seek.render(800);
    Rig seekDry; seekDry.publish(song); seekDry.seek(100); seekDry.play(); seekDry.render(800);
    for (std::size_t i = 0; i < seek.out.size(); ++i)
        require(std::abs(seek.out[i] - seekDry.out[i] * 2.5f) < 2e-6, "seek evaluates send curves immediately");
    auto loopSong = std::make_shared<alien::ArrangementPlayback>(*automated);
    // The shortest loop the timeline accepts (one step, 6000 frames), so 7000 frames wrap once.
    require(loopSong->timeline.setLoop(0, ticksPerStep) && loopSong->timeline.setLoopEnabled(true), "a one-step send automation loop");
    Rig loopOne; loopOne.block = 1; loopOne.publish(loopSong); loopOne.play(); loopOne.render(7000);
    Rig loopOdd; loopOdd.block = 257; loopOdd.publish(loopSong); loopOdd.play(); loopOdd.render(7000);
    require(loopOne.out == loopOdd.out, "send curves restart at loop wraps independently of block size");
    auto bypass = std::make_shared<alien::ArrangementPlayback>(*automated);
    for (auto& lane : bypass->timeline.automationLanes) lane.enabled = false;
    awkward.publish(bypass); awkward.render(12000); awkward.out.clear(); awkward.seek(0); awkward.render(4000);
    for (std::size_t i = 0; i < awkward.out.size(); ++i)
        require(std::abs(awkward.out[i] - reference.out[i]) < 2e-6, "Read bypass restores manual send levels");
    require(awkward.engine.tracks[0].send[0].load() == 0, "automation never overwrites manual knobs");
    float l[257]{}, r[257]{}; float* outputs[]{l, r};
    one.block = 257;
    const auto before = allocationCount;
    one.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 257, {});
    require(allocationCount == before, "automated sends do not allocate in callback");
}

// ---- tempo changes ---------------------------------------------------------------------------------------------
// The rig plays at 120 BPM (25 frames a tick). A change to 60 BPM makes a tick 50 frames from there on.

// Noise that never repeats, so audio that starts at the wrong second cannot line up by chance (a sine with a whole
// number of cycles a second would).
static alien::SessionAudioClip makeNoise(int seconds) {
    alien::SessionAudioClip clip;
    clip.sampleRate = 48000; clip.channels = 1; clip.frames = static_cast<std::size_t>(48000 * seconds);
    clip.left.resize(clip.frames);
    std::uint32_t state = 12345u;
    for (auto& sample : clip.left) { state = state * 1664525u + 1013904223u; sample = static_cast<float>(state >> 8) / 16777216.0f * 0.8f - 0.4f; }
    clip.buildWaveform();
    require(clip.valid(), "test noise clip");
    return clip;
}

static void tempoMapPlaybackTests() {
    const long slowBar = 2 * barFrames;       // a bar at 60 BPM
    const long fastBar = barFrames * 2 / 3;   // a bar at 180 BPM
    // The playhead and MIDI notes follow the change, at any block size, and every bar's note keeps its place.
    {
        Rig rig; rig.block = 1021; auto song = newSong(4);
        require(song->timeline.setTempoChange(ticksPerBar, 180.0), "180 BPM from bar 2");
        song->clips[0][0].pattern = oneNote(60, 0, 1);
        place(*song, regionOf(0, 0, 0, 4 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play();
        rig.render(barFrames);
        closeTo(static_cast<double>(rig.position()), ticksPerBar, 1.0, "one bar at 120 BPM");
        rig.render(fastBar / 2);
        closeTo(static_cast<double>(rig.position()), 1.5 * ticksPerBar, 1.0, "two thirds of the time covers half a bar at 180 BPM");
        rig.render(fastBar / 2 + fastBar);
        closeTo(static_cast<double>(rig.position()), 3 * ticksPerBar, 1.0, "and every later bar takes two thirds as long");
        const long first = rig.firstAbove(0, barFrames);
        for (long top : {static_cast<long>(barFrames), barFrames + fastBar}) {
            const auto onset = rig.firstAbove(top - 1000, top + 4000);
            require(onset >= 0 && std::abs((onset - top) - first) <= 1, "a bar's first note is on its bar line after the change");
        }
    }
    // A change part-way through a sixteenth: the rest of that sixteenth runs at the new tempo.
    {
        Rig rig; auto song = newSong(2);
        require(song->timeline.setTempoChange(ticksPerBar + ticksPerStep / 2, 60.0), "60 BPM half a sixteenth into bar 2");
        song->clips[0][0].pattern = oneNote(60, 1, 1);
        place(*song, regionOf(0, 0, 0, 2 * ticksPerBar));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames + 12000);
        const long first = rig.firstAbove(0, barFrames) - stepFrames;
        const long due = barFrames + (ticksPerStep / 2) * tickFrames + (ticksPerStep / 2) * 2 * tickFrames;
        const auto onset = rig.firstAbove(barFrames, barFrames + 12000);
        require(onset >= 0 && std::abs((onset - due) - first) <= 1, "the second sixteenth of the bar comes where the tempo map puts it");
    }
    // Channel Rack steps land on the map's sample positions.
    {
        Rig rig; auto song = newSong(4);
        require(song->timeline.setTempoChange(ticksPerBar, 180.0), "180 BPM from bar 2");
        place(*song, regionOf(rackRowBase, 0, ticksPerBar, 2 * ticksPerBar));
        rig.publishRack(rackWith({{0, 4}})); rig.publish(song); rig.seek(0); rig.play();
        rig.render(barFrames + 2 * fastBar);
        for (long top : {static_cast<long>(barFrames), barFrames + fastBar}) {
            const long hit = top + 4 * stepFrames * 2 / 3;
            const auto onset = rig.firstAbove(top, top + fastBar);
            require(onset >= hit && onset <= hit + 40, "a rack step sounds on its sixteenth at the new tempo");
        }
    }
    // Audio is not stretched: it plays at its own speed straight through a tempo change.
    const auto clip = makeNoise(4);
    {
        Rig plain; auto steady = newSong(4);
        steady->clips[0][0].audio = &clip;
        place(*steady, regionOf(0, 0, 0, 4 * ticksPerBar));
        plain.publish(steady); plain.seek(0); plain.play(); plain.render(3 * 48000);
        auto changing = std::make_shared<alien::ArrangementPlayback>(*steady);
        require(changing->timeline.setTempoChange(ticksPerBar / 2, 60.0), "60 BPM from half way through bar 1");
        Rig rig; rig.publish(changing); rig.seek(0); rig.play(); rig.render(3 * 48000 + 24000);
        require(worstDifference(rig.out, plain.out, 0, 3 * 48000) < 1.0e-6, "the same audio, sample for sample, across the change");
        // Seeking after the change starts the clip at the second the song has reached there: 1 s, then half a bar at 60 BPM (2 s).
        Rig late; late.publish(changing); late.seek(ticksPerBar); late.play(); late.render(24000 - 200);
        const long into = 3 * 48000;
        double worst = 0;
        for (long i = 64; i < late.frames(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(late.out[static_cast<std::size_t>(i)] - rig.out[static_cast<std::size_t>(into + i)])));
        require(worst < 1.0e-5, "seeking past a change plays what playing up to it does");
    }
    // A region that starts after a change starts on the map's frame.
    {
        auto song = newSong(4);
        require(song->timeline.setTempoChange(ticksPerBar / 2, 60.0), "60 BPM from half way through bar 1");
        song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, ticksPerBar, ticksPerBar));
        Rig rig; rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * 48000 + 4800);
        const long start = barFrames / 2 + (ticksPerBar / 2) * 2 * tickFrames;   // 1 s, then half a bar at 60 BPM (2 s)
        require(rig.energy(0, start - 1) == 0, "silent before the region");
        const auto onset = rig.firstAbove(0, rig.frames());
        require(onset >= start && onset <= start + 64, "the region begins three seconds in");
    }
    // Loops: every pass replays the change. Any block size gives the same audio.
    {
        auto build = [&] {
            auto song = newSong(4);
            require(song->timeline.setTempoChange(ticksPerBar / 4 + 100, 75.0) && song->timeline.setTempoChange(ticksPerBar / 2 + 1, 140.0),
                    "two changes off the grid");
            song->clips[0][0].pattern = oneNote(60, 0, 1); song->clips[1][0].pattern = oneNote(67, 5, 3);
            song->clips[2][0].audio = &clip;
            place(*song, regionOf(0, 0, 0, 2 * ticksPerBar)); place(*song, regionOf(1, 0, 0, 2 * ticksPerBar));
            place(*song, regionOf(2, 0, ticksPerBar / 3, ticksPerBar)); place(*song, regionOf(rackRowBase, 0, 0, 2 * ticksPerBar));
            require(song->timeline.setLoop(0, 3 * ticksPerBar / 4) && song->timeline.setLoopEnabled(true), "loop three beats");
            return song;
        };
        auto run = [&](int block) {
            Rig rig; rig.block = block; rig.publishRack(rackWith({{0, 0}, {1, 6}}));
            rig.publish(build()); rig.seek(0); rig.play(); rig.render(barFrames + barFrames / 2);   // into the second pass
            return rig.out;
        };
        const auto one = run(7), odd = run(257), big = run(4096);
        require(one == odd && one == big, "tempo changes sound the same at any block size");
        const auto song = build();
        const long pass = static_cast<long>(std::llround(song->timeline.secondsBetween(0, 3 * ticksPerBar / 4) * 48000.0));
        long first = 0;
        while (first < pass && std::abs(one[static_cast<std::size_t>(first)]) <= 1.0e-4f) ++first;
        long again = pass - 200;
        while (again < pass + 2000 && std::abs(one[static_cast<std::size_t>(again)]) <= 1.0e-4f) ++again;
        require(std::abs((again - pass) - first) <= 2, "the second pass starts again at the song's first tempo");
    }
    // The click follows the song's beats: half as often at 60 BPM.
    {
        Rig rig; auto song = newSong(4);
        require(song->timeline.setTempoChange(ticksPerBar, 60.0), "60 BPM from bar 2");
        rig.engine.recording.metronome.store(true);
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames + 3 * 48000);
        const auto beat5 = rig.firstAbove(barFrames + 10000, barFrames + 60000);
        require(beat5 >= barFrames + 48000 && beat5 <= barFrames + 48002, "the beat after the change is a second later");
    }
    // Changing tempo while playing allocates nothing.
    {
        Rig rig; auto song = newSong(4);
        require(song->timeline.setTempoChange(ticksPerBar / 3, 75.0) && song->timeline.setTempoChange(2 * ticksPerBar + 7, 170.0), "two changes");
        song->clips[0][0].pattern = oneNote(60, 0, 3); song->clips[1][0].audio = &clip;
        place(*song, regionOf(0, 0, 0, 3 * ticksPerBar)); place(*song, regionOf(1, 0, ticksPerBar / 2, 2 * ticksPerBar));
        place(*song, regionOf(rackRowBase, 0, 0, 3 * ticksPerBar));
        rig.publishRack(rackWith({{0, 0}, {2, 3}})); rig.publish(song); rig.seek(0); rig.play(); rig.render(4096);
        const auto before = allocationCount;
        float l[311]{}, r[311]{}; float* outputs[]{l, r};
        for (int i = 0; i < 700; ++i) {
            if (i % 200 == 100) rig.engine.timelineSeek.store((i * 997) % (3 * ticksPerBar));
            if (i % 150 == 70) rig.engine.bpm.store(80 + (i % 5) * 20);
            rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {});
        }
        require(allocationCount == before, "playing through tempo changes does not allocate in the callback");
    }
}

// ---- time signatures --------------------------------------------------------------------------------------------
// 4/4 for the first bar, 3/4 for the second, then 6/8 (eighth-note beats). At 120 BPM a quarter is 24000 frames.

static void meterPlaybackTests() {
    auto song = newSong(4);
    require(song->timeline.setMeterChange(1, 3, 4) && song->timeline.setMeterChange(2, 6, 8), "4/4, 3/4, then 6/8");
    // The click follows the song's bars: each bar's first beat is accented.
    {
        Rig rig; rig.engine.recording.metronome.store(true);
        rig.publish(song); rig.seek(0); rig.play();
        const long threeFour = barFrames, sixEight = barFrames + 3 * 24000;
        rig.render(sixEight + 6 * 12000 + 2000);
        std::vector<long> beats{0, 24000, 48000, 72000, threeFour, threeFour + 24000, threeFour + 48000};
        for (int k = 0; k < 6; ++k) beats.push_back(sixEight + k * 12000);
        const auto peak = [&](long at) { double p = 0; for (long i = at; i < at + 600; ++i) p = std::max(p, std::abs(static_cast<double>(rig.out[static_cast<std::size_t>(i)]))); return p; };
        for (const long at : beats) {
            const auto onset = rig.firstAbove(at - 200, at + 200);
            require(onset >= at && onset <= at + 2, "a click on every beat of every bar, eighths in 6/8");
        }
        const double accented = peak(0), plain = peak(24000);
        require(accented > plain * 1.2, "fixture: an accent is louder");
        for (const long bar : {threeFour, sixEight}) require(std::abs(peak(bar) - accented) < 1.0e-3, "the first beat of each bar is accented");
        for (const long beat : {threeFour + 48000, sixEight + 3 * 12000, sixEight + 5 * 12000})
            require(std::abs(peak(beat) - plain) < 1.0e-3, "the other beats are not");
    }
    // Plug-ins hear the meter and where the bar began.
    {
        Rig rig; rig.publish(song); rig.seek(ticksPerBar + 960 + 100); rig.play(); rig.render(512);
        const auto& info = rig.engine.plugins->playHead.info;
        const auto signature = info.getTimeSignature();
        require(signature && signature->numerator == 3 && signature->denominator == 4, "a plug-in in bar 2 is told 3/4");
        require(info.getPpqPositionOfLastBarStart() && std::abs(*info.getPpqPositionOfLastBarStart() - 4.0) < 1.0e-9,
                "and that the bar began on the fourth quarter note");
        rig.seek(ticksPerBar + 3 * 960 + 5 * 480 + 10); rig.render(512);
        require(info.getTimeSignature()->numerator == 6 && info.getTimeSignature()->denominator == 8
                && std::abs(*info.getPpqPositionOfLastBarStart() - 7.0) < 1.0e-9, "and 6/8 in bar 3");
    }
}

// ---- fades and crossfades ------------------------------------------------------------------------------------
// Expected gains are computed from the tick of each output frame. The engine reads the playhead one sample on,
// so a gain may differ by one frame's slope (about 4e-5 here): the tolerances allow that and nothing more.

static void fadeTests() {
    const auto first = makeSine(4, 441.0), second = makeSine(4, 662.0);
    constexpr long beat = barFrames / 4;
    const double pi = 3.14159265358979323846;
    // Plays one region alone (no edits), the reference for everything else.
    auto plain = [&](const alien::SessionAudioClip& clip, std::int64_t start, std::int64_t length, long frames) {
        Rig rig; auto song = newSong(4); song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, start, length));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(frames);
        return rig.out;
    };
    // Edge fades are straight lines from silence over their length, and region gain scales the whole clip.
    {
        const auto reference = plain(first, 0, ticksPerBar, barFrames);
        Rig rig; auto song = newSong(2); song->clips[0][0].audio = &first;
        auto r = regionOf(0, 0, 0, ticksPerBar); r.fadeIn = ticksPerBeat; r.fadeOut = ticksPerBeat; r.gain = 0.5;
        place(*song, r);
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        double worst = 0;
        for (long i = 0; i < barFrames; ++i) {
            const double gain = 0.5 * std::min(1.0, static_cast<double>(i) / beat) * std::min(1.0, static_cast<double>(barFrames - i) / beat);
            worst = std::max(worst, std::abs(rig.out[static_cast<std::size_t>(i)] - reference[static_cast<std::size_t>(i)] * gain));
        }
        require(worst < 1.0e-4, "edge fades and region gain shape the clip exactly");
        require(rig.energy(0, 200) < 1.0e-3 && rig.energy(barFrames - 200, barFrames) < 1.0e-3, "a faded clip starts and ends in silence");
    }
    // A crossfade: the first clip plays on past its end and falls on an equal-power curve while the second rises.
    const auto outgoing = plain(first, ticksPerBar, 2 * ticksPerBar, 3 * barFrames);   // the first clip as if it had not ended
    const auto incoming = plain(second, 2 * ticksPerBar, ticksPerBar, 3 * barFrames);
    auto crossfadeSong = [&](std::int64_t gap) {
        auto song = newSong(4); song->clips[0][0].audio = &first; song->clips[1][0].audio = &second;
        place(*song, regionOf(0, 0, ticksPerBar, ticksPerBar - gap));
        auto r = regionOf(0, 1, 2 * ticksPerBar, ticksPerBar); r.crossfade = ticksPerBeat;
        place(*song, r);
        return song;
    };
    std::vector<float> crossed;
    {
        Rig rig; rig.publish(crossfadeSong(0)); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        crossed = rig.out;
        double before = 0, during = 0, after = 0;
        for (long i = barFrames; i < 2 * barFrames; ++i)
            before = std::max(before, std::abs(static_cast<double>(crossed[static_cast<std::size_t>(i)] - outgoing[static_cast<std::size_t>(i)])));
        for (long i = 2 * barFrames; i < 2 * barFrames + beat; ++i) {
            const double x = static_cast<double>(i - 2 * barFrames) / beat;
            const double expected = outgoing[static_cast<std::size_t>(i)] * std::cos(x * pi / 2) + incoming[static_cast<std::size_t>(i)] * std::sin(x * pi / 2);
            during = std::max(during, std::abs(crossed[static_cast<std::size_t>(i)] - expected));
        }
        for (long i = 2 * barFrames + beat + 1; i < 3 * barFrames; ++i)
            after = std::max(after, std::abs(static_cast<double>(crossed[static_cast<std::size_t>(i)] - incoming[static_cast<std::size_t>(i)])));
        require(before < 1.0e-6, "before the crossfade the first clip plays untouched");
        require(during < 1.0e-4, "during it the clips cross on equal-power curves, the first playing on past its end");
        require(after < 1.0e-6, "after it only the second clip is heard");
        double biggestStep = 0;
        for (long i = 2 * barFrames - 100; i < 2 * barFrames + 100; ++i)
            biggestStep = std::max(biggestStep, std::abs(static_cast<double>(crossed[static_cast<std::size_t>(i + 1)] - crossed[static_cast<std::size_t>(i)])));
        require(biggestStep < 0.05, "the join has no click and no dip");
        Rig single; single.block = 1; single.publish(crossfadeSong(0)); single.seek(0); single.play(); single.render(3 * barFrames);
        require(worstDifference(single.out, crossed, 0, 3 * barFrames) < 1.0e-7, "a crossfade sounds the same whatever the block size");
    }
    // Starting the song inside a crossfade starts the outgoing clip there too.
    {
        const long from = 2 * barFrames + beat / 2;
        Rig rig; rig.publish(crossfadeSong(0)); rig.seek(from / tickFrames); rig.play(); rig.render(3 * barFrames - from);
        require(worstDifference(rig.out, std::vector<float>(crossed.begin() + from, crossed.end()), 100, 3 * barFrames - from) < 1.0e-5,
                "joining a crossfade part-way sounds as if the song had played up to there");
    }
    // A clip with a crossfade but nothing ending where it starts fades in on the same curve, from silence.
    {
        Rig rig; rig.publish(crossfadeSong(ticksPerStep)); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        double worst = 0;
        for (long i = 2 * barFrames; i < 2 * barFrames + beat; ++i) {
            const double x = static_cast<double>(i - 2 * barFrames) / beat;
            worst = std::max(worst, std::abs(rig.out[static_cast<std::size_t>(i)] - incoming[static_cast<std::size_t>(i)] * std::sin(x * pi / 2)));
        }
        require(worst < 1.0e-4, "without a neighbour a crossfade is an equal-power fade in");
    }
    // A streamed first clip hands its stream to the tail player at the crossfade (rendered as an export renders).
    {
        auto file = std::make_shared<GatedFile>(4 * 48000, true);
        alien::SessionAudioClip streamed; streamed.sampleRate = 48000; streamed.channels = 1; streamed.frames = 4 * 48000; streamed.streamSource = file;
        require(streamed.valid(), "streamed test clip");
        Rig rig; rig.engine.renderStreamsDeterministically();
        require(rig.engine.retainDiskAudioSource(file) == file.get(), "the engine keeps the source");
        auto song = crossfadeSong(0); song->clips[0][0].audio = &streamed;
        rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * barFrames);
        require(worstDifference(rig.out, crossed, 0, 3 * barFrames) < 1.0e-5, "a streamed clip crosses exactly like one in memory");
        require(rig.engine.sessionStreamUnderruns(0) == 0 && !rig.engine.sessionStreamFailed(0), "with no underruns");
    }
    // Crossing, jumping into and out of crossfades and looping over them never allocates on the audio thread.
    {
        Rig rig; auto song = crossfadeSong(0);
        require(song->timeline.setLoop(2 * ticksPerBar - ticksPerBeat, 2 * ticksPerBar + ticksPerBeat) && song->timeline.setLoopEnabled(true), "loop over the crossfade");
        rig.publish(song); rig.seek(0); rig.play(); rig.render(2048);
        const auto before = allocationCount;
        std::array<float, 311> l{}, r{}; float* outputs[]{l.data(), r.data()};
        for (int i = 0; i < 1500; ++i) {
            if (i % 300 == 100) rig.engine.timelineSeek.store(2 * ticksPerBar + (i % 7) * 100);
            if (i % 300 == 200) rig.engine.timelineSeek.store(2 * ticksPerBar - 200);
            rig.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 311, {});
        }
        require(before == allocationCount, "crossfades never allocate on the audio thread");
    }
}

// Fade shapes (T1-08): every shape plays exactly the gain curve it names, on edge fades and on both sides of a crossfade.
static void fadeShapeTests() {
    using alien::timeline::FadeCurve;
    const auto first = makeSine(4, 441.0), second = makeSine(4, 662.0);
    constexpr long beat = barFrames / 4;
    auto plain = [&](const alien::SessionAudioClip& clip, std::int64_t start, std::int64_t length, long frames) {
        Rig rig; auto song = newSong(4); song->clips[0][0].audio = &clip;
        place(*song, regionOf(0, 0, start, length));
        rig.publish(song); rig.seek(0); rig.play(); rig.render(frames);
        return rig.out;
    };
    const auto reference = plain(first, 0, ticksPerBar, barFrames);
    for (int c = 0; c < alien::timeline::fadeCurveCount; ++c) {
        const auto shape = static_cast<FadeCurve>(c);
        Rig rig; auto song = newSong(2); song->clips[0][0].audio = &first;
        auto r = regionOf(0, 0, 0, ticksPerBar); r.fadeIn = ticksPerBeat; r.fadeOut = ticksPerBeat;
        r.fadeInCurve = shape; r.fadeOutCurve = shape;
        place(*song, r);
        rig.publish(song); rig.seek(0); rig.play(); rig.render(barFrames);
        double worst = 0;
        for (long i = 0; i < barFrames; ++i) {
            const double gain = alien::timeline::fadeGain(shape, static_cast<double>(i) / beat)
                              * alien::timeline::fadeGain(shape, static_cast<double>(barFrames - i) / beat);
            worst = std::max(worst, std::abs(rig.out[static_cast<std::size_t>(i)] - reference[static_cast<std::size_t>(i)] * gain));
        }
        require(worst < 1.0e-4, "each fade shape plays the gain curve it names, in and out");
    }
    // Shapes differ where it matters: half way through a fade in, slow is quieter than linear and fast is louder.
    require(alien::timeline::fadeGain(FadeCurve::slow, 0.5) < 0.5 && alien::timeline::fadeGain(FadeCurve::fast, 0.5) > 0.5
            && std::abs(alien::timeline::fadeGain(FadeCurve::sCurve, 0.5) - 0.5) < 1.0e-12 && alien::timeline::fadeGain(FadeCurve::equalPower, 0.5) > 0.7,
            "the shapes are the curves their names promise");
    // An S-curve crossfade: the outgoing clip falls on the mirrored S while the incoming one rises on it.
    const auto outgoing = plain(first, ticksPerBar, 2 * ticksPerBar, 3 * barFrames);
    const auto incoming = plain(second, 2 * ticksPerBar, ticksPerBar, 3 * barFrames);
    Rig rig; auto song = newSong(4); song->clips[0][0].audio = &first; song->clips[1][0].audio = &second;
    place(*song, regionOf(0, 0, ticksPerBar, ticksPerBar));
    auto r = regionOf(0, 1, 2 * ticksPerBar, ticksPerBar); r.crossfade = ticksPerBeat; r.crossfadeCurve = FadeCurve::sCurve;
    place(*song, r);
    rig.publish(song); rig.seek(0); rig.play(); rig.render(3 * barFrames);
    double worst = 0;
    for (long i = 2 * barFrames; i < 2 * barFrames + beat; ++i) {
        const double x = static_cast<double>(i - 2 * barFrames) / beat;
        const double expected = outgoing[static_cast<std::size_t>(i)] * alien::timeline::fadeGain(FadeCurve::sCurve, 1.0 - x)
                              + incoming[static_cast<std::size_t>(i)] * alien::timeline::fadeGain(FadeCurve::sCurve, x);
        worst = std::max(worst, std::abs(rig.out[static_cast<std::size_t>(i)] - expected));
    }
    require(worst < 1.0e-4, "an S-curve crossfade follows the S on both sides");
}

static void takeCompPlaybackTests() {
    const auto first = makeSine(2, 220), second = makeSine(2, 660);
    auto song = newSong(2);
    auto& t = song->timeline;
    require(t.recordTake(regionOf(0, 0, 0, ticksPerBar)) && t.recordTake(regionOf(0, 1, 0, ticksPerBar)), "record two audio passes");
    require(t.compTake(t.takes.front().id, ticksPerBeat, 2 * ticksPerBeat), "comp one beat from first pass");
    song->clips[0][0].audio = &first; song->clips[1][0].audio = &second;
    Rig actual; actual.publish(song); actual.seek(0); actual.play(); actual.render(barFrames);
    auto plain = std::make_shared<alien::ArrangementPlayback>(*song); plain->timeline.takes.clear();
    Rig reference; reference.block = 113; reference.publish(plain); reference.seek(0); reference.play(); reference.render(barFrames);
    require(actual.out == reference.out && actual.energy(0, barFrames) > 100, "archive never mixes with active comp and block sizes agree");
    const auto before = allocationCount;
    std::array<float, 257> l{}, r{}; float* outputs[]{l.data(), r.data()};
    actual.engine.timelineSeek.store(ticksPerBeat + 300);
    actual.engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 257, {});
    require(allocationCount == before, "seeking within a comp allocates nothing on callback");
}

// A placement clip (Region::clip) plays exactly as the same notes in a Session cell would, and an edit of it
// joins the playing region as a cell edit does.
static void placementClipTests() {
    const auto notes = oneNote(60, 4, 4);
    Rig cell; auto viaCell = newSong(2);
    viaCell->clips[0][0].pattern = notes;
    place(*viaCell, regionOf(0, 0, 0, 2 * ticksPerBar));
    cell.publish(viaCell); cell.seek(0); cell.play(); cell.render(2 * barFrames);

    auto placed = [](const alien::PianoRollPattern& content) {
        auto song = newSong(2);
        Region r = regionOf(0, -1, 0, 2 * ticksPerBar); r.clip = 0x5150;
        place(*song, r);
        alien::ArrangementPlacement mine{0x5150, {}}, other{0x9000, {}};   // other: a clip nothing plays here
        mine.content.pattern = content; other.content.pattern = held(40);
        song->placements.push_back(mine); song->placements.push_back(other);
        return song;
    };
    Rig own; own.publish(placed(notes)); own.seek(0); own.play(); own.render(2 * barFrames);
    require(cell.energy(0, 2 * barFrames) > 0, "the cell's notes sound");
    double worst = 0;
    for (long i = 0; i < 2 * barFrames; ++i)
        worst = std::max(worst, static_cast<double>(std::abs(own.out[static_cast<std::size_t>(i)] - cell.out[static_cast<std::size_t>(i)])));
    require(worst < 1.0e-6, "a placement clip sounds exactly like the same notes in a cell");

    auto more = notes; require(more.add(67, 12, 2, 0.8f) != 0, "add a note");
    Rig whole; whole.publish(placed(more)); whole.seek(0); whole.play(); whole.render(barFrames);
    Rig edited; edited.publish(placed(notes)); edited.seek(0); edited.play(); edited.render(20000);
    edited.publish(placed(more)); edited.render(barFrames - 20000);
    worst = 0;
    for (long i = 0; i < barFrames; ++i)
        worst = std::max(worst, static_cast<double>(std::abs(edited.out[static_cast<std::size_t>(i)] - whole.out[static_cast<std::size_t>(i)])));
    require(worst < 1.0e-6, "an edit of a playing placement clip is heard as if it had always been there");
}

static void streamedSessionRackScene() {
    StreamedRig s(false);
    auto rack = alien::rack::Project::createDefault(); rack.clips[0] = 101;
    require(rack.setStepOn(0, 0, 0, true), "scene rack step"); s.rig.publishRack(rack);
    auto& e = s.rig.engine;
    e.patternMode.store(1); e.launchQuantizationSteps.store(4); e.activeTracks.store(4);
    e.sceneRackClip.store(101); e.synchronizedSceneLaunch.store(true);
    for (int t = 0; t < 4; ++t) {
        alien::SessionClipLaunch launch; launch.clipIndex = 3;
        if (t == 0) launch.audio = &s.clip;
        e.pianoRollLaunches[static_cast<std::size_t>(t)].publish(launch);
    }
    e.playing.store(true); s.rig.render(4096);
    require(e.activeRackClip.load() == 0 && e.queuedRackClip.load() == 101 && e.activePianoRollClip[0].load() == -1,
            "rack and Session scene both wait while streamed audio is unready");
    s.file->gate.store(true);
    bool launched = false;
    for (int i = 0; i < 400 && !launched; ++i) {
        s.rig.render(256);
        const bool rackStarted = e.activeRackClip.load() == 101;
        const bool audioStarted = e.activePianoRollClip[0].load() == 3;
        require(rackStarted == audioStarted, "rack and streamed scene activate in the same callback");
        launched = rackStarted;
        if (!launched) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(launched, "ready streamed scene eventually launches its rack source");
}

int main() {
    timed("streamed Session rack scene", streamedSessionRackScene);
    timed("take comp", takeCompPlaybackTests);
    timed("fades", fadeTests);
    timed("fade shapes", fadeShapeTests);
    timed("automation", automationPlaybackTests);
    timed("send automation", sendAutomationPlaybackTests);
    timed("tempo changes", tempoMapPlaybackTests);
    timed("time signatures", meterPlaybackTests);
    timed("start from anywhere", startFromAnywhereTests);
    timed("loops", loopTests);
    timed("rack lanes", rackLaneTests);
    timed("audio", audioTests);
    timed("live edits", liveEditTests);
    timed("placement clips", placementClipTests);
    timed("exactness", exactnessTests);
    timed("details", detailTests);
    timed("real time", realTimeTests);
    timed("streamed", streamedTests);
    std::cout << "Timeline playback passed\n";
}
