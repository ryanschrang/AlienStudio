// The Channel Rack inside the real ToneEngine audio callback, rendered offline:
// pattern mode, the other modes staying silent, stopping, track routing through
// the Session mixer strip, previews, live snapshot swaps, and no heap use.
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
#include <vector>

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
        rig.engine.activeTracks.store(4);
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
    rig.engine.activeTracks.store(4);               // track 10 is not part of this session...
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
static void capacityReport() {
    std::cout << "# Shared playback callback baseline\n\n"
        << "48 kHz, 120 BPM; stock synth preset 9; no plugins, device or disk I/O.\n"
        << "All 16 mixer tracks active; default four returns; warm-up 32 blocks, measured 256 blocks.\n"
        << "Session rack cases use three held notes per occurrence (loop 16 steps).\n"
        << "Song case uses four overlapping lanes of that same source.\n"
        << "Times include the whole callback; p95/max are observations, not real-time guarantees.\n\n"
        << "| Workload | Block frames | p50 us | p95 us | max us | p95 / block budget % | Callback allocations |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n" << std::fixed << std::setprecision(2);
    for (int workload = 0; workload < 5; ++workload) for (int frames : {64, 256, 1024}) {
        auto rig = std::make_unique<Rig>();
        rig->engine.activeTracks.store(alien::maxSessionTracks);
        rig->project.clips[0] = 101;
        rig->project.setInstrumentPreset(0, 9);
        alien::PianoRollPattern chord; chord.loopSteps = 16;
        for (int pitch : {60, 64, 67}) chord.add(pitch, 0, 16);
        require(rig->project.setChannelNotes(0, 0, chord), "capacity source notes");
        rig->publish(); rig->engine.patternMode.store(alien::sessionMode);
        const int instances = workload == 1 ? 1 : workload == 2 ? 16 : 0;
        for (int t = 0; t < instances; ++t) rig->engine.requestedTrackRackClip[static_cast<std::size_t>(t)].store(101);
        if (workload == 3) {
            auto song = std::make_shared<alien::ArrangementPlayback>(); song->timeline.setLengthBars(64);
            for (int lane = 0; lane < alien::timeline::rackLanes; ++lane) {
                alien::timeline::Region r; r.row = alien::timeline::rackRowBase + lane;
                r.source = 0; r.rackClip = 101; r.length = 64 * alien::timeline::ticksPerBar;
                require(song->timeline.place(r), "capacity Song placement");
            }
            require(rig->engine.arrangementMailbox.publish(song), "capacity Song publication");
            rig->engine.patternMode.store(alien::songMode);
        }
        if (workload == 4) for (int t = 0; t < alien::maxSessionTracks; ++t) {
            alien::SessionClipLaunch launch; launch.pattern = chord; launch.clipIndex = 0;
            require(rig->engine.pianoRollLaunches[static_cast<std::size_t>(t)].publish(launch), "capacity Session launch");
        }
        rig->engine.playing.store(true);
        std::array<float, 1024> left{}, right{}; float* outputs[]{left.data(), right.data()};
        const auto callback = [&] { rig->engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, frames, {}); };
        for (int n = 0; n < 32; ++n) callback();
        std::array<double, 256> times{}; const auto before = allocationCount;
        double energy = 0;
        for (auto& elapsed : times) {
            const auto start = std::chrono::steady_clock::now(); callback();
            elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            for (int n = 0; n < frames; ++n) {
                require(std::isfinite(left[n]) && std::isfinite(right[n]), "capacity finite output");
                energy += left[n] * left[n] + right[n] * right[n];
            }
        }
        const auto allocations = allocationCount - before;
        require(allocations == 0, "capacity callbacks allocate nothing");
        require(workload == 0 || energy > 0, "capacity workload is audible");
        if (instances != 0) for (int t = 0; t < instances; ++t)
            require(rig->engine.activeTrackRackClip[t].load() == 101, "capacity occurrences remain active");
        std::sort(times.begin(), times.end());
        const char* names[]{"Transport, no clips", "1 Session rack occurrence", "16 Session rack occurrences", "4 Song rack occurrences", "16 Session MIDI tracks"};
        std::cout << "| " << names[workload] << " | " << frames << " | " << times[128] << " | " << times[243]
            << " | " << times.back() << " | " << times[243] / (frames * 1.0e6 / 48000.0) * 100.0 << " | " << allocations << " |\n";
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--capacity-report") { capacityReport(); return 0; }

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
