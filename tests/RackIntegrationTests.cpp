// The Channel Rack inside the real ToneEngine audio callback, rendered offline:
// pattern mode, the other modes staying silent, stopping, track routing through
// the Session mixer strip, previews, live snapshot swaps, and no heap use.
#include "ToneEngine.h"
#include "RackSounds.h"
#include "ViewModes.h"
#include <algorithm>
#include <array>
#include <cmath>
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
    std::vector<float> capture(int samples) {
        std::vector<float> out;
        while (samples > 0) {
            const int count = std::min(samples, static_cast<int>(left.size()));
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

int main() {
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
