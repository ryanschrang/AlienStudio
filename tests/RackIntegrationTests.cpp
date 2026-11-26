// The Channel Rack inside the real ToneEngine audio callback, rendered offline:
// pattern mode, the other modes staying silent, stopping, track routing through
// the Session mixer strip, previews, live snapshot swaps, and no heap use.
#include "StockEffects.h"
#include "ToneEngine.h"
#include "RackSounds.h"
#include "ViewModes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <string_view>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#include <vector>

static thread_local std::size_t allocationCount = 0, allocatedBytes = 0;
void* operator new(std::size_t size) {
    ++allocationCount; allocatedBytes += size;
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
// Over-aligned types (the stream rings' cache-line counters) use these; count them too.
static void* alignedAllocate(std::size_t size, std::align_val_t alignment) {
    ++allocationCount; allocatedBytes += size;
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    if (auto* pointer = _aligned_malloc(size == 0 ? 1 : size, align)) return pointer;
#else
    if (auto* pointer = std::aligned_alloc(align, ((size == 0 ? 1 : size) + align - 1) / align * align)) return pointer;
#endif
    throw std::bad_alloc();
}
static void alignedFree(void* pointer) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void* operator new(std::size_t size, std::align_val_t alignment) { return alignedAllocate(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return alignedAllocate(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { alignedFree(pointer); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

using namespace alien::rack;

struct Rig {
    ToneEngine engine;
    Project project = Project::createDefault();
    std::vector<std::shared_ptr<const alien::SessionAudioClip>> keepAlive;
    std::array<float, 256> left{}, right{};

    explicit Rig(double rate = 48000.0) {
        engine.prepare(rate);
        engine.bpm.store(120);          // one sixteenth = 6000 samples at 48 kHz
        for (auto& step : engine.steps) step.store(false);
        publish();
    }
    // Message-thread side of the hand-off: clips first, then the snapshot.
    void publish() {
        for (int c = 0; c < maxChannels; ++c) {
            const alien::SessionAudioClip* clip = nullptr;
            if (c < project.channelCount) {
                const auto& info = project.info[static_cast<std::size_t>(c)];
                if (info.isBuiltin()) {
                    auto made = builtinClip(info.builtinId());
                    clip = made.get();
                    keepAlive.push_back(std::move(made));
                }
            }
            engine.rackSamples[static_cast<std::size_t>(c)].store(clip);
        }
        engine.rackMailbox.publish(project.snapshot());
    }
    // Gives a channel its own clip (after the last publish(), which resets every channel's sound).
    void setClip(int channel, std::shared_ptr<const alien::SessionAudioClip> clip) {
        engine.rackSamples[static_cast<std::size_t>(channel)].store(clip.get());
        keepAlive.push_back(std::move(clip));
    }
    static std::shared_ptr<const alien::SessionAudioClip> dc(std::size_t frames, double rate, float value) {
        auto clip = std::make_shared<alien::SessionAudioClip>();
        clip->sampleRate = rate; clip->channels = 1; clip->frames = frames;
        clip->left.assign(frames, value); clip->buildWaveform();
        return clip;
    }
    // What the device receives on its left output, sample by sample.
    std::vector<float> capture(int samples, int block = 256) {
        std::vector<float> out;
        while (samples > 0) {
            const int count = std::min({samples, block, static_cast<int>(left.size())});
            float* outputs[]{left.data(), nullptr, right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 3, count, {});
            out.insert(out.end(), left.begin(), left.begin() + count);
            samples -= count;
        }
        return out;
    }
    // A real stereo device: output 0 is the left ear and output 1 the right.
    struct Stereo { float leftPeak{}, rightPeak{}; };
    Stereo renderStereo(int samples) {
        Stereo r;
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, count, {});
            for (int i = 0; i < count; ++i) {
                const auto k = static_cast<std::size_t>(i);
                require(std::isfinite(left[k]) && std::isfinite(right[k]), "stereo output must be finite");
                r.leftPeak = std::max(r.leftPeak, std::abs(left[k])); r.rightPeak = std::max(r.rightPeak, std::abs(right[k]));
            }
            samples -= count;
        }
        return r;
    }
    struct Result { double energy{}; float peak{}; };
    Result render(int samples) {
        Result r;
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
            left.fill(std::numeric_limits<float>::quiet_NaN());
            right.fill(std::numeric_limits<float>::quiet_NaN());
            float* outputs[]{left.data(), nullptr, right.data()};
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 3, count, {});
            for (int i = 0; i < count; ++i) {
                const auto k = static_cast<std::size_t>(i);
                require(std::isfinite(left[k]) && left[k] == right[k] && std::abs(left[k]) <= 1.00001f,
                        "callback must write finite, matching, in-range samples");
                r.energy += static_cast<double>(left[k]) * left[k];
                r.peak = std::max(r.peak, std::abs(left[k]));
            }
            samples -= count;
        }
        return r;
    }
};

static void silentInEveryOtherMode() {
    for (const int mode : {0, 1, 2}) {
        Rig rig;
        rig.project.setStepOn(0, 0, 0, true);
        rig.publish();
        rig.engine.patternMode.store(mode);
        rig.engine.playing.store(true);
        const auto r = rig.render(24000);
        require(r.energy == 0.0, "the rack must not sound in session, piano-roll or song mode");
    }
    Rig rig;
    rig.project.setStepOn(0, 0, 0, true);
    rig.publish();
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(false);
    require(rig.render(24000).energy == 0.0, "a stopped transport plays no pattern");
}

static void patternModePlaysTheSelectedPattern() {
    Rig rig;
    rig.project.setStepOn(0, 0, 0, true);   // kick on step 1
    rig.project.setStepOn(0, 3, 4, true);   // snare-ish: channel 3 on step 5
    rig.publish();
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(true);
    const auto first = rig.render(256);
    require(first.peak > 0.05f, "a step on the very first grid line sounds in the first block");
    require(rig.engine.rackStep.load() == 0, "the display step follows the grid");
    rig.render(6000 * 4 + 100 - 256);
    require(rig.engine.rackStep.load() == 4, "step five is current after four steps");
    // Rendering the same pattern again from the top is identical (stop/start restarts the bar).
    rig.engine.playing.store(false); rig.render(4096);
    require(rig.engine.rackStep.load() == -1, "no step is shown once stopped");
    rig.engine.playing.store(true);
    const auto again = rig.render(256);
    require(std::abs(again.energy - first.energy) < 1e-9 * std::max(1.0, first.energy),
            "restarting the transport starts the pattern from step one again");
}

static void stoppingAndLeavingModeFadeOut() {
    for (const bool leaveMode : {false, true}) {
        Rig rig;
        rig.project.setStepOn(0, 0, 0, true);
        rig.publish();
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        rig.render(1500);               // kick is ringing
        if (leaveMode) rig.engine.patternMode.store(1); else rig.engine.playing.store(false);
        rig.render(2048);               // longer than the release fade
        require(rig.render(4096).energy == 0.0, "nothing rings on after the transport stops or the mode changes");
    }
}

static void trackRoutingUsesTheMixerStrip() {
    {   // Routed to track 1: that track's mute silences the rack channel.
        Rig rig;
        auto channel = rig.project.channels[0]; channel.output = 1;
        rig.project.setChannel(0, channel);
        rig.project.setStepOn(0, 0, 0, true);
        rig.publish();
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        const auto open = rig.render(3000);
        require(open.peak > 0.05f, "a channel routed to a track is heard through that track");
        rig.engine.playing.store(false); rig.render(4096);
        rig.engine.tracks[1].mute.store(true);
        rig.render(4096);               // let the mute ramp finish
        rig.engine.playing.store(true);
        require(rig.render(3000).energy == 0.0, "muting the track mutes the channels routed to it");
    }
    {   // Routed to master: the track's mute does not matter.
        Rig rig;
        rig.project.setStepOn(0, 0, 0, true);
        rig.publish();
        rig.engine.tracks[1].mute.store(true);
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        require(rig.render(3000).peak > 0.05f, "a master-routed channel ignores track mutes");
    }
    {   // A route to a track the session no longer has falls back to master.
        Rig rig;
        auto channel = rig.project.channels[0]; channel.output = 9;
        rig.project.setChannel(0, channel);
        rig.project.setStepOn(0, 0, 0, true);
        rig.publish();
        rig.engine.setActiveTracks(4);
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        require(rig.render(3000).peak > 0.05f, "a missing track falls back to the master bus");
    }
}

static void previewsSoundEvenWhenStopped() {
    Rig rig;
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(false);
    require(rig.render(1024).energy == 0.0, "silent before the preview");
    rig.engine.rackAuditions.push(0, 1.0f);
    require(rig.render(1024).peak > 0.05f, "clicking a channel plays its sample with the transport stopped");

    auto muted = rig.project.channels[1]; muted.mute = true;
    rig.project.setChannel(1, muted);
    rig.publish();
    rig.render(8192);
    rig.engine.rackAuditions.push(1, 1.0f);
    require(rig.render(1024).peak > 0.05f, "a muted channel still previews");

    rig.engine.rackAuditions.push(200, 1.0f);       // out of range: ignored, no crash
    rig.engine.rackAuditions.push(-1, 1.0f);
    rig.render(512);
}

static void liveSnapshotSwapsAndAllocations() {
    Rig rig;
    rig.project.setStepOn(0, 0, 0, true);
    rig.project.setStepOn(0, 1, 2, true);
    rig.publish();
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(true);
    rig.render(512);                    // warm up (first voices)
    std::vector<Project> variants;
    Project cleared = rig.project; cleared.clearPattern(0);
    variants.push_back(cleared);
    variants.push_back(rig.project);
    // Prepare the snapshots on the "message thread" first, then measure only the audio side.
    std::vector<std::shared_ptr<const Snapshot>> prepared;
    for (const auto& v : variants) prepared.push_back(v.snapshot());
    allocationCount = 0;
    std::size_t spent = 0;
    for (int round = 0; round < 40; ++round) {
        rig.engine.rackMailbox.publish(prepared[static_cast<std::size_t>(round & 1)]);
        const auto before = allocationCount;
        rig.render(1000);
        spent += allocationCount - before;
        rig.engine.rackAuditions.push(round % 4, 0.8f);
    }
    require(spent == 0, "rendering the rack must never touch the heap");

    // A cleared pattern goes quiet once the last hits have rung out.
    rig.engine.rackMailbox.publish(prepared[0]);
    rig.render(48000);                  // longer than any built-in sound, including previews
    require(rig.render(6000).energy == 0.0, "an emptied pattern plays nothing");
}

static void legacyStepSequencerStillWorks() {
    Rig rig;
    rig.engine.steps[0].store(true);
    rig.engine.patternMode.store(0);
    rig.engine.playing.store(true);
    require(rig.render(6000).energy > 0.0, "the 16-step synth sequencer is unchanged");
    require(rig.engine.rackStep.load() == -1, "and the rack reports no step while it is not playing");
}

// ---- the two ears, previews, panics, tempo and rate ---------------------------------
static void leftAndRightStayApart() {
    // Every bus (a track's, or the master's) carries a left and a right side, and the device gets each on its own output.
    for (const int output : {1, alien::rack::masterOutput}) {
        for (const float pan : {-1.0f, 1.0f}) {
            Rig rig;
            auto channel = rig.project.channels[0]; channel.output = static_cast<std::int8_t>(output); channel.pan = pan;
            rig.project.setChannel(0, channel);
            rig.project.setStepOn(0, 0, 0, true);
            rig.publish();
            rig.engine.patternMode.store(3);
            rig.renderStereo(4096);             // the pan settles while nothing plays
            rig.engine.playing.store(true);
            const auto heard = rig.renderStereo(3000);
            const float loud = pan < 0 ? heard.leftPeak : heard.rightPeak, quiet = pan < 0 ? heard.rightPeak : heard.leftPeak;
            require(loud > 0.05f && quiet < 0.01f * loud,
                    "a channel panned hard to one side is heard on that side only, whether it goes to a track or the master");
        }
    }
}

static void previewsHonourPanicAndChannel() {
    {   // A panic wipes out previews that were waiting.
        Rig rig;
        rig.engine.patternMode.store(3);
        rig.engine.rackAuditions.push(0, 1.0f);
        rig.engine.notes.panic.store(true);
        require(rig.render(512).energy == 0.0, "a panic drops the previews it finds waiting");
        rig.engine.rackAuditions.push(0, 1.0f);
        require(rig.render(512).peak > 0.05f, "and the next preview sounds again");
    }
    {   // A preview plays the sound of the channel that was clicked, not always the first.
        float peaks[2]{};
        for (int channel = 0; channel < 2; ++channel) {
            Rig rig;
            rig.publish();
            rig.setClip(0, Rig::dc(4800, 48000.0, 0.2f));
            rig.setClip(1, Rig::dc(4800, 48000.0, 0.6f));
            rig.engine.patternMode.store(3);
            rig.engine.rackAuditions.push(channel, 1.0f);
            peaks[channel] = rig.render(1024).peak;
        }
        require(peaks[0] > 0.05f && peaks[1] > 2.5f * peaks[0], "previewing channel 2 plays channel 2's sound, three times louder than channel 1's here");
    }
}

static void rackPanicsAndRewinds() {
    {   // The rack's own preview queue can ask for everything to stop (it does when it overflows).
        Rig rig;
        rig.project.setSample(0, "builtin:openhat");     // rings for 0.7 s
        rig.publish();
        rig.engine.patternMode.store(3);
        rig.engine.rackAuditions.push(0, 1.0f);
        require(rig.render(256).peak > 0.05f, "a long preview is ringing");
        rig.engine.rackAuditions.panic.store(true);
        rig.render(2304);                                   // the fade-out
        require(rig.render(8192).energy == 0.0, "a stop request silences the rack's voices");
    }
    {   // A keyboard panic (all sound off) while the rack plays cuts the ringing sounds and starts the bar again.
        Rig rig;
        rig.project.setSample(0, "builtin:openhat");
        rig.project.setStepOn(0, 0, 2, true);               // sounds 12000 samples into every bar
        rig.publish();
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        rig.render(12500);
        require(rig.engine.rackStep.load() == 2 && rig.render(10).peak > 0.05f, "the open hat is ringing on step 3");
        rig.engine.notes.panic.store(true);
        rig.render(256);
        require(rig.engine.rackStep.load() == 0, "after the panic the rack starts its bar again");
        rig.render(2048);
        require(rig.render(8000).energy == 0.0, "and nothing rings on from before it");
    }
}

static void timingFollowsTempoAndRate() {
    struct Case { double rate, bpm; };
    for (const Case c : {Case{48000.0, 120.0}, Case{48000.0, 90.0}, Case{96000.0, 120.0}, Case{44100.0, 150.0}, Case{96000.0, 150.0}}) {
        Rig rig(c.rate);
        rig.engine.bpm.store(c.bpm);
        rig.project.setStepOn(0, 0, 4, true);               // step 5 is one beat in
        rig.publish();
        rig.setClip(0, Rig::dc(100, c.rate, 1.0f));
        rig.engine.patternMode.store(3);
        rig.engine.playing.store(true);
        const int beat = static_cast<int>(std::llround(c.rate * 60.0 / c.bpm));
        const auto heard = rig.capture(beat + 300);
        int first = -1;
        for (std::size_t i = 0; i < heard.size() && first < 0; ++i) if (std::abs(heard[i]) > 0.05f) first = static_cast<int>(i);
        require(first == beat, "a step lands exactly where the device's sample rate and the project's tempo put it");
    }
}

static void tracksThatAreNotInUseFallBackToTheMaster() {
    Rig rig;
    auto channel = rig.project.channels[0]; channel.output = 9;
    rig.project.setChannel(0, channel);
    rig.project.setStepOn(0, 0, 0, true);
    rig.publish();
    rig.engine.setActiveTracks(4);               // track 10 is not part of this session...
    rig.engine.tracks[9].mute.store(true);          // ...so what its strip does is none of the rack's business
    rig.render(4096);
    rig.engine.patternMode.store(3);
    rig.engine.playing.store(true);
    require(rig.render(3000).peak > 0.05f, "a channel routed to a track the session does not have plays through the master, not through that strip");
}

static void tabsChooseTheEngineMode() {
    using namespace alien;
    // Every tab that plays something maps to its own mode, exactly as before the rack existed.
    require(patternModeForTab(0, 2) == 0, "Steps plays the 16-step sequencer");
    require(patternModeForTab(1, 0) == 1 && patternModeForTab(2, 0) == 1, "Piano roll and Session play clips");
    require(patternModeForTab(5, 0) == 2, "Arrangement plays the song");
    require(patternModeForTab(8, 0) == 3 && rackTabIndex == 8, "the Channel Rack tab plays the rack");
    require(patternModeForTab(rackTabIndex, 1) == rackMode, "the rack mode is the engine's mode 3");
    // Tabs with no playback of their own leave the current mode alone.
    for (int tab : {3, 4, 6, 7, 9, -1, 100})
        for (int current = 0; current < 4; ++current)
            require(patternModeForTab(tab, current) == current, "tabs with no playback keep the current mode");
    require(stepsMode == 0 && sessionMode == 1 && songMode == 2 && rackMode == 3, "the mode numbers match the engine's");
}

static void melodicCallback() {
    Rig rig;
    alien::PianoRollPattern notes; notes.loopSteps = 16;
    notes.add(60, 0.25, 2); notes.add(64, 0.25, 4); notes.add(67, 0.25, 6);
    require(rig.project.setChannelNotes(0, 0, notes), "store a melodic chord");
    rig.publish(); rig.setClip(0, Rig::dc(48000 * 2, 48000, 0.2f));
    rig.engine.patternMode.store(alien::rackMode); rig.engine.playing.store(true);
    require(rig.render(1500).peak == 0, "real callback waits for fractional chord onset");
    const auto before = allocationCount;
    const auto sounding = rig.render(6000);
    const auto after = allocationCount;
    require(sounding.peak > 0.05f && before == after, "real callback renders chords without heap allocation");
    rig.engine.playing.store(false);
    rig.render(6000);
    require(rig.render(1000).peak < 0.001f, "real callback releases melodic notes after stop");
    Rig songRig;
    songRig.project.setChannelNotes(0, 0, notes);
    songRig.publish(); songRig.setClip(0, Rig::dc(48000 * 2, 48000, 0.2f));
    auto song = std::make_shared<alien::ArrangementPlayback>();
    alien::timeline::Region region;
    region.row = alien::timeline::rackRowBase; region.source = 0;
    region.start = 0; region.length = alien::timeline::ticksPerStep;
    require(song->timeline.place(region), "place a trimmed melodic rack region");
    songRig.engine.arrangementMailbox.publish(song);
    songRig.engine.patternMode.store(alien::songMode);
    songRig.engine.timelineSeek.store(20); // 500 samples into the enclosing step.
    songRig.engine.playing.store(true);
    require(songRig.render(1000).peak == 0 && songRig.render(1000).peak > 0.05f,
            "Song seek into a partial step retains the upcoming fractional chord");
    songRig.render(7000);
    require(songRig.render(1000).peak < 0.001f, "Song region end releases melodic notes early");
    auto loopSong = std::make_shared<alien::ArrangementPlayback>(*song);
    loopSong->timeline.regions.front().length = 16 * alien::timeline::ticksPerStep;
    loopSong->timeline.setLoop(0, alien::timeline::ticksPerStep); loopSong->timeline.setLoopEnabled(true);
    songRig.engine.arrangementMailbox.publish(loopSong); songRig.engine.timelineSeek.store(0);
    songRig.render(6000 + 1250);
    require(songRig.render(150).peak < 0.001f, "Song loop boundary releases prior notes before the next fractional onset");
}

static void synthCallback() {
    Rig rig;
    rig.project.setInstrumentPreset(0, 9);
    alien::PianoRollPattern notes; notes.loopSteps = 16;
    notes.add(60, 0.25, 8); notes.add(64, 0.25, 8); notes.add(67, 0.25, 8);
    rig.project.setChannelNotes(0, 0, notes); rig.publish();
    require(rig.engine.rackSamples[0].load() == nullptr, "synth channel needs no sample buffer");
    rig.engine.patternMode.store(alien::rackMode); rig.engine.playing.store(true);
    require(rig.render(1500).peak == 0, "synth chord waits for fractional note start");
    const auto before = allocationCount;
    const auto audible = rig.render(30000);
    const auto after = allocationCount;
    require(audible.peak > 0.01f && before == after, "real callback sustains a chord without allocating");
    rig.engine.playing.store(false); rig.render(6000);
    require(rig.render(1000).peak < 0.001f, "stopping the real callback silences the synth");
    rig.engine.rackAuditions.push(ToneEngine::rackAuditionEvent(0, -36, true), 0.8f);
    require(rig.render(30000).peak > 0.001f, "held low-pitch piano-roll preview decodes through the real callback");
    rig.engine.rackAuditions.push(ToneEngine::rackAuditionEvent(0, -36, true), 0);
    rig.render(5000); require(rig.render(1000).peak < 0.001f, "piano-roll note-off releases its instrument preview");
    auto song = std::make_shared<alien::ArrangementPlayback>();
    alien::timeline::Region region; region.row = alien::timeline::rackRowBase; region.source = 0; region.start = 0; region.length = alien::timeline::ticksPerStep;
    require(song->timeline.place(region), "place a synth rack region");
    rig.engine.arrangementMailbox.publish(song); rig.engine.patternMode.store(alien::songMode);
    rig.engine.timelineSeek.store(20); rig.engine.playing.store(true);
    require(rig.render(1000).peak == 0 && rig.render(3000).peak > 0.001f, "Song seek retains upcoming fractional synth chord");
    rig.render(6000); require(rig.render(1000).peak < 0.001f, "Song region sends note-offs with the synth's release envelope");
}

static void performLaunches() {
    for (int tab : {1, 2, alien::rackTabIndex}) {
        require(alien::patternModeForTab(tab, alien::sessionMode, false, true) == alien::sessionMode,
                "editing the shared source keeps Perform transport");
        require(alien::patternModeForTab(tab, alien::sessionMode, true, true) == alien::songMode,
                "Song mode still takes precedence over Perform launches");
    }
    Rig rig(8000);
    rig.project.clips[0] = 101;
    require(rig.project.setStepOn(0, 0, 0, true) && rig.project.addPattern(0), "two Perform sources");
    rig.project.clips[1] = 102; rig.project.selectPattern(0); rig.publish();
    rig.engine.patternMode.store(1); rig.engine.launchQuantizationSteps.store(4);
    alien::SessionClipLaunch session;
    session.pattern.add(60, 0, 16); session.clipIndex = 0;
    rig.engine.pianoRollLaunches[0].publish(session);
    rig.engine.requestedRackClip.store(101); rig.engine.playing.store(true);
    rig.render(1500);
    require(rig.engine.activeRackClip.load() == 101 && rig.engine.activePianoRollClip[0].load() == 0,
            "a composite rack clip and a Session track play together");
    rig.project.selectPattern(1); rig.publish();
    rig.engine.requestedRackClip.store(102);
    const auto before = allocationCount;
    rig.render(2500);
    const auto after = allocationCount;
    require(before == after && rig.engine.activeRackClip.load() == 101 && rig.engine.queuedRackClip.load() == 102,
            "callback keeps old identity until beat boundary, without allocation");
    rig.render(1);
    require(rig.engine.activeRackClip.load() == 102 && rig.engine.rackStep.load() == 0,
            "callback launches the requested clip, rather than following selection");
    rig.engine.requestedRackClip.store(0); rig.render(4000);
    require(rig.engine.activeRackClip.load() == 0 && rig.engine.activePianoRollClip[0].load() == 0,
            "rack stop leaves the independently playing Session track alone");
    rig.engine.playing.store(false); rig.render(64);
    rig.engine.playing.store(true); rig.render(8000);
    require(rig.engine.activeRackClip.load() == 0, "Session restart does not launch the editor's selected pattern");
    rig.engine.requestedRackClip.store(101); rig.render(1);
    require(rig.engine.activeRackClip.load() == 101, "rack can be launched again after transport restart");
    rig.engine.resetRackPlayback.store(true); rig.render(64);
    require(rig.engine.activeRackClip.load() == 0,
            "a document reset cannot carry a live clip into a different document's identity namespace");
}

static void overlappingPerformCallback() {
    Rig overlap(8000), reference(8000);
    const auto setup = [](Rig& rig) {
        rig.project.clips[0] = 101;
        rig.project.setInstrumentPreset(0, 9);
        alien::PianoRollPattern notes; notes.loopSteps = 16; notes.add(60, 0, 12);
        require(rig.project.setChannelNotes(0, 0, notes) && rig.project.addPattern(0), "two overlapping synth sources");
        rig.project.clips[1] = 102; rig.publish();
        rig.engine.patternMode.store(1); rig.engine.launchQuantizationSteps.store(4);
        rig.engine.requestedTrackRackClip[1].store(101); rig.engine.playing.store(true);
    };
    setup(overlap); setup(reference);
    overlap.engine.requestedTrackRackClip[0].store(101);
    const auto two = overlap.render(1500), one = reference.render(1500);
    require(two.energy > 1.8 * one.energy && overlap.engine.activeTrackRackClip[0].load() == 101
        && overlap.engine.activeTrackRackClip[1].load() == 101, "same-pitch synth occurrences play together in the real callback");
    overlap.project.selectPattern(1); overlap.publish();
    overlap.engine.requestedTrackRackClip[0].store(0);
    const auto before = allocationCount; overlap.render(4000); const auto after = allocationCount;
    reference.render(4000);
    require(before == after && overlap.engine.activeTrackRackClip[0].load() == 0
        && overlap.engine.activeTrackRackClip[1].load() == 101, "real callback stops only one row without allocating");
    const auto remaining = overlap.capture(1000), expected = reference.capture(1000);
    for (std::size_t n = 0; n < expected.size(); ++n)
        require(std::abs(remaining[n] - expected[n]) < 1.0e-6f, "one occurrence's note-off does not cut or retrigger the other's synth voice");
    overlap.engine.resetRackPlayback.store(true); overlap.render(2000);
    require(overlap.engine.activeTrackRackClip[1].load() == 0 && overlap.render(1000).peak < 0.001f,
        "document reset clears every overlapping occurrence");
    overlap.engine.requestedTrackRackClip[0].store(101); overlap.engine.requestedTrackRackClip[1].store(102);
    overlap.engine.notes.panic.store(true); overlap.render(1000);
    require(overlap.engine.activeTrackRackClip[0].load() == 0 && overlap.engine.activeTrackRackClip[1].load() == 0
        && overlap.engine.queuedTrackRackClip[0].load() == RackPlayer::noLaunchRequest,
        "panic discards unconsumed per-row launch requests");
    overlap.engine.sceneTrackRackClip[0].store(101); overlap.engine.sceneTrackRackClip[1].store(102);
    overlap.engine.synchronizedSceneLaunch.store(true); overlap.engine.requestedRackClip.store(0);
    for (std::size_t t = 0; t < ToneEngine::sessionTracks; ++t) {
        alien::SessionClipLaunch stop; stop.clipIndex = -1; overlap.engine.pianoRollLaunches[t].publish(stop);
    }
    overlap.render(8000);
    require(overlap.engine.activeTrackRackClip[0].load() == 0 && overlap.engine.activeTrackRackClip[1].load() == 0,
        "Stop rack cancels every rack source in a pending scene");
}

static void timelineOverlapLifecycle() {
    using namespace alien::timeline;
    const auto capture = [](int block, bool loop) {
        Rig rig(8000); rig.project.clips[0] = 101;
        alien::PianoRollPattern notes; notes.loopSteps = 16; notes.add(60, 1.05, 8);
        require(rig.project.setChannelNotes(0, 0, notes), "fractional Song note"); rig.publish();
        rig.setClip(0, Rig::dc(20000, 8000, 0.2f));
        auto song = std::make_shared<alien::ArrangementPlayback>();
        Region r; r.row = rackRowBase; r.source = 0; r.length = 16 * ticksPerStep;
        require(song->timeline.place(r), "first Song instance");
        r.id = 0; r.row += 1; require(song->timeline.place(r), "second Song instance");
        if (loop) { song->timeline.setLoop(0, 2 * ticksPerStep); song->timeline.setLoopEnabled(true); }
        else rig.engine.timelineEnd.store(ticksPerStep + 13);
        rig.engine.arrangementMailbox.publish(song); rig.engine.patternMode.store(2); rig.engine.playing.store(true);
        return rig.capture(6000, block);
    };
    const auto endWide = capture(256, false), endNarrow = capture(17, false);
    require(endWide == endNarrow && endWide[1050] > 0.01f && endWide[2000] == 0,
        "Song stop inside a block retains earlier fractional onsets and releases both instances on the exact sample");
    require(capture(256, true) == capture(17, true),
        "Song loop wraps reset instance-owned delayed notes identically across callback sizes");
}

static void stableSongBindingAcrossBankReorder() {
    Rig moved(8000), reference(8000);
    const auto setup = [](Rig& rig) {
        rig.project.clips[0] = 101; rig.project.setStepOn(0, 0, 0, true);
        require(rig.project.addPattern(0), "second Song source"); rig.project.clips[1] = 102;
        rig.project.setStepField(1, 0, 0, StepField::velocity, 40); rig.publish();
        auto song = std::make_shared<alien::ArrangementPlayback>();
        alien::timeline::Region region; region.row = alien::timeline::rackRowBase; region.source = 1; region.rackClip = 102;
        region.length = 32 * alien::timeline::ticksPerStep; require(song->timeline.place(region), "stable Song source placement");
        rig.engine.arrangementMailbox.publish(song); rig.engine.patternMode.store(2); rig.engine.playing.store(true);
    };
    setup(moved); setup(reference);
    require(moved.capture(1500) == reference.capture(1500), "same starting rack placement");
    require(moved.project.deletePattern(0), "remove unused preceding bank slot"); moved.publish();
    // Deliberately do not republish the arrangement: its index cache is stale.
    require(moved.capture(15001) == reference.capture(15001),
        "stable clip identity preserves Song playback when the rack snapshot arrives before remapped Song indices");
}

// Opt-in diagnostic: fixed workloads, warm-up, then individual real callback
// durations. Timing has no pass threshold; allocation and finite-output checks do.
// T1-04 D24: helpers for the repeated capacity report.
static double medianOf(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

// The order round `round` visits `cells` measurements in: forward on even rounds, backward on odd
// ones, so slow drift during a run (heat, other desktop activity) falls on every workload alike.
static std::vector<std::size_t> capacityRoundOrder(std::size_t cells, int round) {
    std::vector<std::size_t> order(cells);
    for (std::size_t i = 0; i < cells; ++i) order[i] = round % 2 ? cells - 1 - i : i;
    return order;
}

// Reads `--capacity-report [--repeat N]`. Returns the round count (1 to 50), 0 when the arguments
// are not a capacity report, or -1 when they ask for one with a bad round count.
static int capacityReportRounds(int argc, const char* const* argv) {
    if (argc < 2 || std::string_view(argv[1]) != "--capacity-report") return 0;
    if (argc == 2) return 1;
    if (argc != 4 || std::string_view(argv[2]) != "--repeat") return -1;
    const std::string_view text(argv[3]);
    if (text.empty() || text.size() > 2) return -1;
    int rounds = 0;
    for (char c : text) { if (c < '0' || c > '9') return -1; rounds = rounds * 10 + (c - '0'); }
    return rounds >= 1 && rounds <= 50 ? rounds : -1;
}

static void capacityHelpers() {
    require(medianOf({}) == 0.0 && medianOf({3.0}) == 3.0, "median of none and one");
    require(medianOf({5.0, 1.0, 3.0}) == 3.0 && medianOf({4.0, 1.0, 3.0, 2.0}) == 2.5, "median of odd and even counts");
    require(capacityRoundOrder(3, 0) == std::vector<std::size_t>{0, 1, 2}, "even rounds run forward");
    require(capacityRoundOrder(3, 1) == std::vector<std::size_t>{2, 1, 0}, "odd rounds run backward");
    const char* plain[]{"t", "--capacity-report"};
    const char* five[]{"t", "--capacity-report", "--repeat", "5"};
    const char* none[]{"t"};
    require(capacityReportRounds(2, plain) == 1 && capacityReportRounds(4, five) == 5, "capacity report round counts");
    require(capacityReportRounds(1, none) == 0, "no arguments run the tests");
    for (const char* bad : {"0", "51", "x", "", "-1", "100"}) {
        const char* args[]{"t", "--capacity-report", "--repeat", bad};
        require(capacityReportRounds(4, args) == -1, "bad round counts are refused");
    }
    const char* missing[]{"t", "--capacity-report", "--repeat"};
    const char* other[]{"t", "--capacity-report", "--again", "5"};
    require(capacityReportRounds(3, missing) == -1 && capacityReportRounds(4, other) == -1, "bad report options are refused");
}

static void capacityReport(int rounds) {
    const int full = alien::maxSessionTracks;
    {
        const auto before = allocatedBytes;
        auto engine = std::make_unique<ToneEngine>();
        const auto constructed = allocatedBytes - before;
        engine->prepare(48000.0);
        const auto prepared = allocatedBytes - before;
        engine->setActiveTracks(full);   // every slot's stream voices (T1-04 D: reserved as a project grows)
        const auto everySlot = allocatedBytes - before;
        std::cout << "# Shared playback capacity\n\n"
            << "Compiled limits: " << full << " Session tracks, " << alien::maxSessionScenes << " scenes.\n\n"
            << "| Engine heap (C++ new, cumulative) | Bytes | MiB |\n| --- | ---: | ---: |\n" << std::fixed << std::setprecision(3)
            << "| sizeof(ToneEngine) (inline object only) | " << sizeof(ToneEngine) << " | " << sizeof(ToneEngine) / 1048576.0 << " |\n"
            << "| Constructed ToneEngine (incl. object) | " << constructed << " | " << constructed / 1048576.0 << " |\n"
            << "| After prepare(48 kHz) | " << prepared << " | " << prepared / 1048576.0 << " |\n"
            << "| After setActiveTracks(" << full << ") | " << everySlot << " | " << everySlot / 1048576.0 << " |\n\n"
            << "Cumulative bytes requested from operator new; frees and std::vector growth during prepare are not subtracted.\n"
            << "Excludes plugins, media, Song/rack snapshots, document/history, UI and allocator overhead.\n"
            << "A new engine holds stream voices and note state for the default project's tracks; a project reserves more as it grows.\n"
            << "Sample preview audio is reserved by the first preview, not counted here.\n\n";
    }
    std::cout << "# Shared playback callback baseline\n\n"
        << "48 kHz, 120 BPM; stock synth preset 9; no device or disk I/O; no plugins except where a row names them.\n"
        << "Tracks in use are stated per row; default four returns; warm-up 32 blocks, measured 256 blocks.\n"
        << "Session rack cases use three held notes per occurrence (loop 16 steps).\n"
        << "Song case uses four overlapping lanes of that same source.\n"
        << "T1-04 D22 rows: loaded versus playing (one playing track among all in use), sends at 0.5 to returns A-D,\n"
        << "in-memory Session audio clips (one shared 8 s mono clip, no streaming), and the stock EQ and compressor\n"
        << "on every playing track's FX rack (prepared before timing).\n"
        << "Times include the whole callback; p95/max are observations, not real-time guarantees.\n"
        << "Output energy is the sum of squared output samples over the measured blocks, printed exactly, so two\n"
        << "builds can be shown to render the same audio (T1-04 D23).\n\n"
        << "| Workload | Tracks in use | Block frames | p50 us | p95 us | max us | p95 / block budget % | Callback allocations | Output energy |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n" << std::fixed << std::setprecision(2);
    struct Case { const char* name; int kind, tracks, occurrences; };
    std::vector<Case> cases{{"Transport, no clips", 0, 1, 0}, {"Transport, no clips", 0, 16, 0}, {"1 Session rack occurrence", 1, 16, 1},
        {"16 Session rack occurrences", 1, 16, 16}, {"4 Song rack occurrences", 3, 16, 0}, {"16 Session MIDI tracks", 4, 16, 16},
        // T1-04 D22: loaded versus playing tracks, sends and returns, audio clips and plugins (measurement gates 2 and 3).
        {"1 Session MIDI track playing", 4, 16, 1}, {"16 Session MIDI tracks, sends to 4 returns", 5, 16, 16},
        {"16 Session audio clips", 6, 16, 16}, {"16 Session MIDI tracks, stock EQ + compressor", 7, 16, 16},
        {"16 Session drum tracks", 8, 16, 16}};
    if (full > 16) {
        cases.push_back({"Transport, no clips", 0, full, 0});
        cases.push_back({"All Session rack occurrences", 1, full, full});
        cases.push_back({"All Session MIDI tracks", 4, full, full});
        cases.push_back({"1 Session MIDI track playing", 4, full, 1});
        cases.push_back({"All Session audio clips", 6, full, full});
        cases.push_back({"All Session MIDI tracks, stock EQ + compressor", 7, full, full});
    }
    // One decoded clip shared by every audio row, long enough to play through warm-up and measurement at 1,024 frames.
    alien::SessionAudioClip audio; audio.sampleRate = 48000; audio.channels = 1; audio.frames = 8 * 48000;
    audio.left.assign(audio.frames, 0.1f); audio.buildWaveform();
    require(audio.valid(), "capacity audio clip");
    // D23: a quarter-second drum hit, so hits on four pads a beat apart overlap in the sampler's voices.
    alien::SessionAudioClip hit; hit.sampleRate = 48000; hit.channels = 1; hit.frames = 12000;
    hit.left.assign(hit.frames, 0.2f); hit.buildWaveform();
    require(hit.valid(), "capacity drum hit");
    alien::PianoRollPattern beat; beat.loopSteps = 16;
    for (int n = 0; n < 4; ++n) beat.add(alien::drumBaseNote + 2 * n, 4 * n, 1);
    struct Run { std::array<double, 256> times{}; std::size_t allocations = 0; double energy = 0; };
    const auto measure = [&](const Case& c, int frames) {
        Run run;
        auto rig = std::make_unique<Rig>();
        rig->engine.setActiveTracks(c.tracks);
        rig->project.clips[0] = 101;
        rig->project.setInstrumentPreset(0, 9);
        alien::PianoRollPattern chord; chord.loopSteps = 16;
        for (int pitch : {60, 64, 67}) chord.add(pitch, 0, 16);
        require(rig->project.setChannelNotes(0, 0, chord), "capacity source notes");
        rig->publish(); rig->engine.patternMode.store(alien::sessionMode);
        const int instances = c.kind == 1 ? c.occurrences : 0;
        for (int t = 0; t < instances; ++t) rig->engine.requestedTrackRackClip[static_cast<std::size_t>(t)].store(101);
        if (c.kind == 3) {
            auto song = std::make_shared<alien::ArrangementPlayback>(); song->timeline.setLengthBars(64);
            for (int lane = 0; lane < alien::timeline::rackLanes; ++lane) {
                alien::timeline::Region r; r.row = alien::timeline::rackRowBase + lane;
                r.source = 0; r.rackClip = 101; r.length = 64 * alien::timeline::ticksPerBar;
                require(song->timeline.place(r), "capacity Song placement");
            }
            require(rig->engine.arrangementMailbox.publish(song), "capacity Song publication");
            rig->engine.patternMode.store(alien::songMode);
        }
        const bool midi = c.kind == 4 || c.kind == 5 || c.kind == 7 || c.kind == 8;
        if (c.kind == 8) for (int t = 0; t < c.occurrences; ++t) {
            rig->engine.tracks[static_cast<std::size_t>(t)].instrument.store(1);
            for (int pad = 0; pad < alien::drumPadCount; ++pad)
                rig->engine.drumKits[static_cast<std::size_t>(t)].samples[static_cast<std::size_t>(pad)].store(&hit);
        }
        if (midi || c.kind == 6) for (int t = 0; t < c.occurrences; ++t) {
            alien::SessionClipLaunch launch; launch.clipIndex = 0;
            if (midi) launch.pattern = c.kind == 8 ? beat : chord; else launch.audio = &audio;
            require(rig->engine.pianoRollLaunches[static_cast<std::size_t>(t)].publish(launch), "capacity Session launch");
        }
        if (c.kind == 5) for (int t = 0; t < c.occurrences; ++t)
            for (int r = 0; r < 4; ++r) rig->engine.tracks[static_cast<std::size_t>(t)].send[static_cast<std::size_t>(r)].store(0.5f);
        if (c.kind == 7) {
            auto& plugins = *rig->engine.plugins;
            for (int t = 0; t < c.occurrences; ++t) {
                auto& rack = plugins.racks[static_cast<std::size_t>(t)];
                for (std::size_t i = 0; i < 2; ++i) {
                    auto& slot = rack.effects[i];
                    slot.processor = alien::stock::create(alien::stock::catalog[i].id);
                    slot.name = alien::stock::catalog[i].name;
                    plugins.prepareSlot(slot, t, static_cast<int>(i));
                    require(slot.prepared, "capacity stock effect prepared");
                }
                rack.effectCount = 2;
            }
            plugins.reserveAlignment();
        }
        rig->engine.playing.store(true);
        std::array<float, 1024> left{}, right{}; float* outputs[]{left.data(), right.data()};
        const auto callback = [&] { rig->engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, frames, {}); };
        for (int n = 0; n < 32; ++n) callback();
        auto& times = run.times; auto& energy = run.energy; const auto before = allocationCount;
        for (auto& elapsed : times) {
            const auto start = std::chrono::steady_clock::now(); callback();
            elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            for (int n = 0; n < frames; ++n) {
                require(std::isfinite(left[n]) && std::isfinite(right[n]), "capacity finite output");
                energy += left[n] * left[n] + right[n] * right[n];
            }
        }
        const auto allocations = run.allocations = allocationCount - before;
        require(allocations == 0, "capacity callbacks allocate nothing");
        require(c.kind == 0 || energy > 0, "capacity workload is audible");
        for (int t = 0; t < instances; ++t)
            require(rig->engine.activeTrackRackClip[static_cast<std::size_t>(t)].load() == 101, "capacity occurrences remain active");
        // D22: every launched Session clip is still playing at the end, and no plugin failed, so each row measures what it names.
        if (midi || c.kind == 6) for (int t = 0; t < c.occurrences; ++t)
            require(rig->engine.activePianoRollClip[static_cast<std::size_t>(t)].load() == 0, "capacity Session clips remain active");
        require(rig->engine.plugins->faultCount.load() == 0, "capacity plugins run without faults");
        std::sort(times.begin(), times.end());
        return run;
    };
    const std::array<int, 3> blockSizes{64, 256, 1024};
    const auto budget = [](int frames) { return frames * 1.0e6 / 48000.0; };
    if (rounds == 1) {
        for (const auto& c : cases) for (int frames : blockSizes) {
            const auto run = measure(c, frames); const auto& times = run.times;
            std::cout << "| " << c.name << " | " << c.tracks << " | " << frames << " | " << times[128] << " | " << times[243]
                << " | " << times.back() << " | " << times[243] / budget(frames) * 100.0 << " | " << run.allocations << " | "
                << std::defaultfloat << std::setprecision(17) << run.energy << std::fixed << std::setprecision(2) << " |\n";
        }
        return;
    }

    // T1-04 D24: every row measured `rounds` times, rounds interleaved across the rows (alternating
    // direction), so that drift during a run falls on every row alike and the spread can be seen.
    struct Cell { std::size_t c; int frames; std::vector<double> p50, p95; double energy = -1; };
    std::vector<Cell> cells;
    for (std::size_t c = 0; c < cases.size(); ++c) for (int frames : blockSizes) cells.push_back({c, frames, {}, {}});
    for (int round = 0; round < rounds; ++round)
        for (auto i : capacityRoundOrder(cells.size(), round)) {
            auto& cell = cells[i];
            const auto run = measure(cases[cell.c], cell.frames);
            cell.p50.push_back(run.times[128]); cell.p95.push_back(run.times[243]);
            // The engine is deterministic: every round of a row must render exactly the same audio.
            require(cell.energy < 0 || cell.energy == run.energy, "capacity rounds render identical audio");
            cell.energy = run.energy;
        }
    std::cout << "\nEach row above was measured " << rounds << " times; the rounds are interleaved across rows, alternating\n"
        << "direction. The table gives the median over rounds of each round's p50 and p95, and their range.\n"
        << "Every round of a row rendered identical output energy and made zero callback allocations.\n\n"
        << "| Workload | Tracks in use | Block frames | p50 us, median | p50 us, range | p95 us, median | p95 us, range | median p95 / block budget % |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& cell : cells) {
        const auto& c = cases[cell.c];
        const auto [p50lo, p50hi] = std::minmax_element(cell.p50.begin(), cell.p50.end());
        const auto [p95lo, p95hi] = std::minmax_element(cell.p95.begin(), cell.p95.end());
        std::cout << "| " << c.name << " | " << c.tracks << " | " << cell.frames << " | " << medianOf(cell.p50) << " | "
            << *p50lo << " - " << *p50hi << " | " << medianOf(cell.p95) << " | " << *p95lo << " - " << *p95hi << " | "
            << medianOf(cell.p95) / budget(cell.frames) * 100.0 << " |\n";
    }
    if (full <= 16) return;
    // From 16 tracks in use to all of them: the same workload with every extra track doing the same work.
    std::cout << "\n| Workload (16 tracks, then " << full << ") | Block frames | 16 tracks, median p50 us | " << full
        << " tracks, median p50 us | Ratio | us per added track |\n| --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& big : cells) {
        const auto& b = cases[big.c];
        if (b.tracks != full) continue;
        for (const auto& small : cells) {
            const auto& s = cases[small.c];
            const bool sameWork = s.kind == b.kind && s.tracks == 16 && small.frames == big.frames
                && (b.occurrences == b.tracks ? s.occurrences == s.tracks : s.occurrences == b.occurrences);
            if (!sameWork) continue;
            const auto lo = medianOf(small.p50), hi = medianOf(big.p50);
            std::cout << "| " << b.name << " | " << big.frames << " | " << lo << " | " << hi << " | " << hi / lo << " | "
                << (hi - lo) / (full - 16) << " |\n";
        }
    }
}

int main(int argc, char** argv) {
    if (const int rounds = capacityReportRounds(argc, argv); rounds != 0) {
        if (rounds < 0) { std::cerr << "usage: RackIntegrationTests --capacity-report [--repeat 1-50]\n"; return 2; }
        capacityReport(rounds); return 0;
    }

    capacityHelpers();

    performLaunches();
    overlappingPerformCallback();
    timelineOverlapLifecycle();
    stableSongBindingAcrossBankReorder();
    synthCallback();
    melodicCallback();
    silentInEveryOtherMode();
    patternModePlaysTheSelectedPattern();
    stoppingAndLeavingModeFadeOut();
    trackRoutingUsesTheMixerStrip();
    previewsSoundEvenWhenStopped();
    liveSnapshotSwapsAndAllocations();
    legacyStepSequencerStillWorks();
    leftAndRightStayApart();
    previewsHonourPanicAndChannel();
    rackPanicsAndRewinds();
    timingFollowsTempoAndRate();
    tracksThatAreNotInUseFallBackToTheMaster();
    tabsChooseTheEngineMode();
    std::cout << "RackIntegrationTests passed\n";
}
