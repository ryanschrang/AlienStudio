#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SynthEngine.h"
#include "NoteQueue.h"
#include "StepClock.h"
#include "PianoRollSequence.h"
#include "MidiInputEngine.h"
#include "MidiControllerMappings.h"
#include "SessionAudioClip.h"
#include "TrackMixer.h"
#include "DrumSampler.h"
#include "Arrangement.h"
#include "AudioCapture.h"
#include <array>
#include <atomic>
#include <cmath>

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    static constexpr std::size_t sessionTracks = 4;
    std::array<std::atomic<bool>, 16> steps{};
    std::array<std::atomic<int>, 16> stepPitches{};
    std::array<std::atomic<int>, 16> stepVelocities{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, preset{0};
    std::array<alien::TrackControls, sessionTracks> tracks;
    std::array<alien::DrumKitControls, sessionTracks> drumKits;
    std::array<NoteQueue, sessionTracks> drumAuditions;
    std::atomic<int> liveDrumTrack{-1};
    alien::ArrangementMailbox arrangementMailbox;
    std::atomic<int> arrangementPosition{-1}; // thousandths of a bar
    NoteQueue notes;
    alien::MidiInputPipe midi;
    alien::MidiControllerMappings controllerMappings;
    alien::MidiControllerActionQueue controllerActions;
    std::array<alien::PianoRollMailbox, sessionTracks> pianoRollNotes;
    std::array<alien::SessionClipMailbox, sessionTracks> pianoRollLaunches;
    std::array<std::atomic<int>, sessionTracks> sessionTrackPositions{-1000, -1000, -1000, -1000};
    std::atomic<int> patternMode{0}, pianoRollPosition{-1};
    std::array<std::atomic<int>, sessionTracks> requestedPianoRollClip{-1, -1, -1, -1};
    std::array<std::atomic<int>, sessionTracks> activePianoRollClip{-1, -1, -1, -1};
    std::atomic<bool> synchronizedSceneLaunch{false};
    std::atomic<int> launchQuantizationSteps{16}; // One bar by default (16 sixteenth-note steps).

    ToneEngine() noexcept {
        for (auto& pitch : stepPitches) pitch.store(57, std::memory_order_relaxed);
        for (auto& velocity : stepVelocities) velocity.store(102, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        prepare(device->getCurrentSampleRate());
    }
    // Shared by device startup and offline callback regression tests.
    void prepare(double rate) {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        for (auto& player : audioPlayers) player.stop();
        audioClipMode.fill(false);
        synth.prepare(sampleRate);
        for (auto& voice : trackSynths) voice.prepare(sampleRate);
        for (auto& sampler : drumSamplers) sampler.prepare(sampleRate);
        for (auto& mixer : trackMixers) mixer.prepare(sampleRate);
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
        discardPendingNotes();
        resetTransport();
    }
    void audioDeviceStopped() override {
        if (auto* take = capture.load()) take->done.store(true);
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        for (auto& player : audioPlayers) player.stop();
        audioClipMode.fill(false);
        allSoundOff(); discardPendingNotes(); resetTransport();
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
    }
    std::atomic<alien::AudioCapture*> capture{};

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputChannels,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        if (auto* take = capture.load(std::memory_order_acquire)) take->append(inputs,inputChannels,samples);
        bool playNow = playing.load();
        const int modeNow = patternMode.load();
        const bool songChanged = arrangementMailbox.consume(*song);
        if (songChanged && modeNow == 2) {
            allSoundOff(); resetTransport();
            for (std::size_t t = 0; t < sessionTracks; ++t) {
                rolls[t].stop([this,t](int n,float v,bool on) { sendPattern(t,n,v,on); });
                audioPlayers[t].stop();
            }
        }
        if (synchronizedSceneLaunch.exchange(false)) {
            synchronizedScenePending = true;
            synchronizedSceneTracksQueued = 0;
            synchronizedSceneQuantizationSteps = launchQuantizationSteps.load();
        }
        if ((wasPlaying && !playNow) || wasMode != modeNow) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            for (auto& player : audioPlayers) player.stop();
            audioClipMode.fill(false);
            allSoundOff(); resetTransport();
            midi.requestReset();
        }
        wasPlaying = playNow; wasMode = modeNow;
        synth.setPatch(alien::presets[static_cast<std::size_t>(juce::jlimit(0, 2, preset.load()))]);
        bool anySolo = false;
        for (const auto& track : tracks) anySolo = anySolo || track.solo.load();
        std::array<float, sessionTracks> peaks{};
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            const auto settings = tracks[t].snapshot();
            trackSynths[t].setPatch(alien::presets[static_cast<std::size_t>(std::clamp(settings.preset, 0, 2))]);
            trackMixers[t].setTarget(settings, anySolo);
            if (instrumentModes[t] != settings.instrument) {
                trackSynths[t].allSoundOff(); drumSamplers[t].stop();
                instrumentModes[t] = settings.instrument;
            }
            for (int pad = 0; pad < alien::drumPadCount; ++pad)
                drumSamplers[t].setSample(pad, drumKits[t].samples[static_cast<std::size_t>(pad)].load());
        }
        const int requestedRoute = std::clamp(liveDrumTrack.load(), -1, 3);
        const int route = requestedRoute >= 0 && instrumentModes[static_cast<std::size_t>(requestedRoute)] == 1 ? requestedRoute : -1;
        if (route != currentLiveDrumTrack) {
            midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
            stopMidiDrums();
            for (int channel = 1; channel <= 16; ++channel) synth.allSoundOff(channel + 8);
            currentLiveDrumTrack = route;
        }
        NoteQueue::Event event{};
        const bool panic = notes.panic.exchange(false);
        // Bound work per callback even if the producer keeps publishing.
        for (int n = 0; n < 256 && notes.pop(event); ++n) {
            if (!panic) {
                if (event.velocity > 0) synth.noteOn(event.note, event.velocity);
                else synth.noteOff(event.note);
            }
        }
        if (panic) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            for (auto& player : audioPlayers) player.stop();
            audioClipMode.fill(false);
            synchronizedScenePending = false;
            synchronizedSceneTracksQueued = 0;
            synchronizedSceneLaunch.store(false);
            activeStepPitch = -1;
            allSoundOff(); midi.requestReset();
            if (modeNow == 2) resetTransport();
        }
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            NoteQueue::Event hit{};
            for (int n = 0; n < 256 && drumAuditions[t].pop(hit); ++n)
                if (!panic && instrumentModes[t] == 1) drumSamplers[t].noteOn(hit.note, hit.velocity, 1);
            if (drumAuditions[t].panic.exchange(false)) drumSamplers[t].stop(1);
        }
        midi.process([this](int n, float v, int ch) { sendMidi(n, v, ch); },
            [this](int channel, int controller, int value) {
                if (channel < 0) { controllerMappings.resetEdges(); stopMidiDrums(); return; }
                const int action = controllerMappings.processCC(channel, controller, value);
                if (action >= 0) controllerActions.push(action);
            });
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            alien::SessionClipLaunch requestedClip;
            if (modeNow != 2 && pianoRollLaunches[track].consume(requestedClip)) {
                const int quantization = synchronizedScenePending
                    ? synchronizedSceneQuantizationSteps : launchQuantizationSteps.load();
                const int clipIndex = requestedClip.clipIndex == -2
                    ? requestedPianoRollClip[track].load() : requestedClip.clipIndex;
                rolls[track].queuePattern(requestedClip.pattern, clipIndex,
                    synchronizedScenePending, quantization);
                audioPlayers[track].queue(requestedClip.audio, quantization);
                if (synchronizedScenePending)
                    synchronizedSceneTracksQueued |= (1u << track);
            }
            if (modeNow == 2)
                rolls[track].beginBlock(playNow, sampleRate, bpm.load(),
                    [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            else
                rolls[track].beginBlock(pianoRollNotes[track], playNow && modeNow == 1, sampleRate, bpm.load(),
                    [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        }
        for (int i = 0; i < samples; ++i) {
            if (playNow && modeNow == 2) {
                const int bar = static_cast<int>((songStep + 1.0e-8) / 16.0);
                if (bar >= song->arrangement.lengthBars) {
                    allSoundOff(); playing.store(false); playNow = false;
                } else {
                    if (bar != lastSongBar) {
                        for (std::size_t t = 0; t < sessionTracks; ++t) {
                            int regionIndex = -1;
                            for (std::size_t r = 0; r < song->arrangement.count; ++r) {
                                const auto& region = song->arrangement.regions[r];
                                if (region.track == static_cast<int>(t) && bar >= region.startBar
                                    && bar < region.startBar + region.lengthBars) regionIndex = static_cast<int>(r);
                            }
                            if (regionIndex != activeSongRegions[t]) {
                                startSongRegion(t, regionIndex);
                                activeSongRegions[t] = regionIndex;
                            }
                        }
                        lastSongBar = bar;
                    }
                    for (std::size_t t = 0; t < sessionTracks; ++t)
                        if (activeSongRegions[t] >= 0) rolls[t].nextSample(
                            [this,t](int n,float v,bool on) { sendPattern(t,n,v,on); });
                    arrangementPosition.store(static_cast<int>(songStep * 1000.0 / 16.0));
                    songStep += static_cast<double>(bpm.load()) * 4.0 / (sampleRate * 60.0);
                }
            }
            if (playNow && modeNow == 0) {
                const auto tick = clock.next(sampleRate, bpm.load());
                if (tick.noteOff && activeStepPitch >= 0) {
                    synth.noteOff(activeStepPitch, 1);
                    activeStepPitch = -1;
                }
                if (tick.step >= 0) {
                    currentStep.store(tick.step);
                    const auto step = static_cast<std::size_t>(tick.step);
                    if (steps[step].load()) {
                        activeStepPitch = juce::jlimit(0, 127, stepPitches[step].load());
                        const int velocity = juce::jlimit(1, 127, stepVelocities[step].load());
                        synth.noteOn(activeStepPitch, static_cast<float>(velocity) / 127.0f, 1);
                    }
                }
            }
            if (playNow && modeNow == 1) {
                constexpr unsigned allSessionTracks = (1u << sessionTracks) - 1u;
                if (synchronizedScenePending && synchronizedSceneTracksQueued == allSessionTracks
                    && rolls[0].launchBoundaryDue(synchronizedSceneQuantizationSteps)) {
                    for (std::size_t track = 0; track < sessionTracks; ++track)
                        rolls[track].launchPendingNow([this, track](int n, float v, bool on) {
                            sendPattern(track, n, v, on);
                        });
                    for (std::size_t track = 0; track < sessionTracks; ++track) {
                        const bool isAudioClip = audioPlayers[track].pendingIsAudio();
                        if (audioPlayers[track].launchPendingNow(sampleRate))
                            audioClipMode[track] = isAudioClip;
                    }
                    synchronizedScenePending = false;
                    synchronizedSceneTracksQueued = 0;
                }
                for (std::size_t track = 0; track < sessionTracks; ++track) {
                    if (!synchronizedScenePending && audioPlayers[track].hasPending()
                        && rolls[track].launchBoundaryDue(audioPlayers[track].pendingQuantizationSteps())) {
                        const bool isAudioClip = audioPlayers[track].pendingIsAudio();
                        if (audioPlayers[track].launchPendingNow(sampleRate)) audioClipMode[track] = isAudioClip;
                    }
                    rolls[track].nextSample([this, track](int n, float v, bool on) {
                        sendPattern(track, n, v, on);
                    });
                }
            }
            const float value = synth.nextSample();
            float left = value, right = value;
            for (std::size_t track = 0; track < sessionTracks; ++track) {
                const float voice = trackSynths[track].nextSample();
                const auto drum = drumSamplers[track].nextSample();
                float trackLeft = voice + drum[0], trackRight = voice + drum[1];
                if (playNow && (modeNow == 1 || modeNow == 2)) {
                    const auto sample = audioPlayers[track].nextSample();
                    trackLeft += sample[0] * 0.25f;
                    trackRight += sample[1] * 0.25f;
                }
                trackMixers[track].process(trackLeft, trackRight);
                peaks[track] = std::max(peaks[track], std::max(std::abs(trackLeft), std::abs(trackRight)));
                left += trackLeft; right += trackRight;
            }
            left = std::clamp(left, -1.0f, 1.0f);
            right = std::clamp(right, -1.0f, 1.0f);
            for (int c = 0; c < channels; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = c == 0 ? left : (c == 1 ? right : (left + right) * 0.5f);
        }
        pianoRollPosition.store(static_cast<int>(rolls[0].positionStep() * 1000.0));
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            tracks[track].peak.store(peaks[track]);
            sessionTrackPositions[track].store(static_cast<int>(rolls[track].positionStep() * 1000.0));
            activePianoRollClip[track].store(audioClipMode[track] && !audioPlayers[track].isPlaying()
                ? -1 : rolls[track].activeClipIndex());
        }
    }
private:
    void startSongRegion(std::size_t track, int regionIndex) noexcept {
        auto send = [this,track](int n,float v,bool on) { sendPattern(track,n,v,on); };
        rolls[track].stop(send); audioPlayers[track].stop();
        trackSynths[track].allSoundOff(); drumSamplers[track].stop(0);
        audioClipMode[track] = false;
        if (regionIndex < 0) return;
        const auto& region = song->arrangement.regions[static_cast<std::size_t>(regionIndex)];
        const auto& clip = song->clips[static_cast<std::size_t>(region.scene)][track];
        rolls[track].queuePattern(clip.audio == nullptr ? clip.pattern : alien::PianoRollPattern{}, region.scene, true);
        rolls[track].launchPendingNow(send);
        rolls[track].beginBlock(true, sampleRate, bpm.load(), send);
        audioPlayers[track].queue(clip.audio, 1); audioPlayers[track].launchPendingNow(sampleRate);
        audioClipMode[track] = clip.audio != nullptr;
    }
    void stopMidiDrums() noexcept {
        for (auto& sampler : drumSamplers) for (int channel = 1; channel <= 16; ++channel) sampler.stop(channel + 8);
    }
    void sendMidi(int note, float velocity, int channel) noexcept {
        if (currentLiveDrumTrack >= 0) {
            auto& sampler = drumSamplers[static_cast<std::size_t>(currentLiveDrumTrack)];
            if (note < 0) sampler.stop(channel + 8);
            else sampler.noteOn(note, velocity, channel + 8);
            return;
        }
        const int source = channel + 8; // Reserve source channels 2..5 for four Session tracks.
        if (note < 0) synth.allSoundOff(source);
        else if (velocity > 0) synth.noteOn(note, velocity, source);
        else synth.noteOff(note, source);
    }
    void sendPattern(std::size_t track, int note, float velocity, bool on) noexcept {
        const int source = static_cast<int>(track) + 2;
        if (instrumentModes[track] == 1) {
            if (on) drumSamplers[track].noteOn(note, velocity);
            return;
        }
        if (on) trackSynths[track].noteOn(note, velocity, source);
        else trackSynths[track].noteOff(note, source);
    }
    void allSoundOff() noexcept {
        synth.allSoundOff();
        for (auto& voice : trackSynths) voice.allSoundOff();
        for (auto& sampler : drumSamplers) sampler.stop();
        for (auto& track : tracks) track.peak.store(0.0f);
    }
    void discardPendingNotes() noexcept {
        NoteQueue::Event event{};
        for (int n = 0; n < 256 && notes.pop(event); ++n) {}
        notes.panic.store(false);
        for (auto& queue : drumAuditions) {
            for (int n = 0; n < 256 && queue.pop(event); ++n) {}
            queue.panic.store(false);
        }
    }
    void resetTransport() noexcept {
        songStep = 0; lastSongBar = -1; activeSongRegions.fill(-2); arrangementPosition.store(-1);
        clock.reset(); wasPlaying = false; currentStep.store(-1); pianoRollPosition.store(-1000);
        activeStepPitch = -1;
        synchronizedScenePending = false;
        synchronizedSceneTracksQueued = 0;
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            activePianoRollClip[track].store(-1);
            sessionTrackPositions[track].store(-1000);
        }
    }
    std::unique_ptr<alien::ArrangementPlayback> song = std::make_unique<alien::ArrangementPlayback>();
    double songStep{};
    int lastSongBar{-1};
    std::array<int, sessionTracks> activeSongRegions{-2,-2,-2,-2};
    alien::SynthEngine synth; // Live keyboard and legacy step sequencer.
    std::array<alien::SynthEngine, sessionTracks> trackSynths;
    std::array<alien::TrackMixer, sessionTracks> trackMixers;
    std::array<alien::DrumSampler, sessionTracks> drumSamplers;
    std::array<int, sessionTracks> instrumentModes{};
    int currentLiveDrumTrack{-1};
    alien::StepClock clock;
    std::array<alien::PianoRollScheduler, sessionTracks> rolls{};
    std::array<alien::SessionAudioPlayer, sessionTracks> audioPlayers{};
    std::array<bool, sessionTracks> audioClipMode{};
    double sampleRate{44100.0};
    bool wasPlaying{};
    int wasMode{};
    bool synchronizedScenePending{};
    unsigned synchronizedSceneTracksQueued{};
    int synchronizedSceneQuantizationSteps{16};
    int activeStepPitch{-1};
};

