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
#include "SessionProject.h"
#include "DrumSampler.h"
#include "Arrangement.h"
#include "AudioCapture.h"
#include "PluginRuntime.h"
#include "MidiRecording.h"
#include "RackEngine.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

// Fixed-capacity array stored on the heap. Allocated once at construction, so
// large per-track buffers do not bloat the engine object (or a test's stack)
// and nothing is ever allocated on the audio thread.
template <class T, std::size_t N>
class FixedHeapArray {
public:
    FixedHeapArray() : data(std::make_unique<std::array<T, N>>()) {}
    T& operator[](std::size_t i) noexcept { return (*data)[i]; }
    const T& operator[](std::size_t i) const noexcept { return (*data)[i]; }
    static constexpr std::size_t size() noexcept { return N; }
    auto begin() noexcept { return data->begin(); }
    auto end() noexcept { return data->end(); }
    auto begin() const noexcept { return data->cbegin(); }
    auto end() const noexcept { return data->cend(); }
private:
    std::unique_ptr<std::array<T, N>> data;
};

class ToneEngine : public juce::AudioIODeviceCallback {
public:
    // Pre-allocated capacity; activeTracks says how many the session uses.
    static constexpr std::size_t sessionTracks = static_cast<std::size_t>(alien::maxSessionTracks);
    // Level of decoded audio clips before the track fader (it used to be 0.25, -12 dB).
    static constexpr float audioClipLevel = 0.5f;
    std::array<std::atomic<bool>, 16> steps{};
    std::array<std::atomic<int>, 16> stepPitches{};
    std::array<std::atomic<int>, 16> stepVelocities{};
    std::atomic<bool> playing{false};
    std::atomic<int> bpm{120}, currentStep{-1}, preset{0};
    std::array<alien::TrackControls, sessionTracks> tracks;
    alien::MasterControls masterControls;
    // Record, count-in and metronome state shared with the GUI, and the delay the
    // audio device reports between rendering a sample and hearing it.
    alien::RecordControls recording;
    std::atomic<int> deviceLatencySamples{0};
    std::array<alien::DrumKitControls, sessionTracks> drumKits;
    std::array<NoteQueue, sessionTracks> drumAuditions;
    std::atomic<int> liveDrumTrack{-1};
    alien::ArrangementMailbox arrangementMailbox;
    std::atomic<int> arrangementPosition{-1}; // thousandths of a bar
    NoteQueue notes;
    alien::MidiInputPipe midi;
    alien::MidiControllerMappings controllerMappings;
    alien::MidiControllerActionQueue controllerActions;
    FixedHeapArray<alien::PianoRollMailbox, sessionTracks> pianoRollNotes;
    FixedHeapArray<alien::SessionClipMailbox, sessionTracks> pianoRollLaunches;
    std::array<std::atomic<int>, sessionTracks> sessionTrackPositions;
    std::atomic<int> patternMode{0}, pianoRollPosition{-1};
    std::array<std::atomic<int>, sessionTracks> requestedPianoRollClip;
    std::array<std::atomic<int>, sessionTracks> activePianoRollClip;
    // Number of Session tracks in use (scene launches wait for this many).
    std::atomic<int> activeTracks{alien::sessionProjectTrackCount};
    std::atomic<bool> synchronizedSceneLaunch{false};
    std::atomic<int> launchQuantizationSteps{16}; // One bar by default (16 sixteenth-note steps).
    // Channel Rack. The project is published as an immutable snapshot; each
    // channel's decoded sample is an immutable clip retained by the session
    // owner; previews from the GUI arrive as (channel, velocity) events.
    // Pattern mode (patternMode 3) plays the selected pattern in a loop.
    alien::rack::SnapshotMailbox rackMailbox;
    alien::rack::RackPlayer::SampleTable rackSamples{};
    NoteQueue rackAuditions;
    std::atomic<int> rackStep{-1}; // step of the selected pattern that started last, or -1

    ToneEngine() noexcept {
        for (std::size_t t = 0; t < sessionTracks; ++t)
            audioPlayers[t].bind(diskStreams.voice(t * 2), diskStreams.voice(t * 2 + 1));
        for (auto& pitch : stepPitches) pitch.store(57, std::memory_order_relaxed);
        for (auto& velocity : stepVelocities) velocity.store(102, std::memory_order_relaxed);
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            sessionTrackPositions[t].store(-1000, std::memory_order_relaxed);
            requestedPianoRollClip[t].store(-1, std::memory_order_relaxed);
            activePianoRollClip[t].store(-1, std::memory_order_relaxed);
        }
    }

    const alien::DiskAudioFile* retainDiskAudioSource(std::shared_ptr<const alien::DiskAudioFile> source) {
        return diskStreams.retain(std::move(source));
    }
    std::uint64_t sessionStreamUnderruns(std::size_t track) const noexcept {
        return track < sessionTracks ? audioPlayers[track].underrunCount() : 0;
    }
    bool sessionStreamFailed(std::size_t track) const noexcept {
        return track < sessionTracks && audioPlayers[track].streamFailed();
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        prepare(device->getCurrentSampleRate());
        deviceLatencySamples.store(std::max(0, device->getOutputLatencyInSamples()));
    }
    // Shared by device startup and offline callback regression tests.
    void prepare(double rate) {
        // A device restart may arrive while a take is active or still pending.
        // Complete/refuse it before resetting transport state so the controller
        // cannot remain attached to a take the audio thread has forgotten.
        resetRecordingForDeviceChange();
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 44100.0;
        plugins->prepare(sampleRate);
        for (auto& buffer : clipBuffers) buffer.setSize(2, alien::PluginRuntime::blockSize);
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        for (auto& player : audioPlayers) player.stop();
        audioClipMode.fill(false);
        synth.prepare(sampleRate);
        for (auto& voice : trackSynths) voice.prepare(sampleRate);
        for (auto& sampler : drumSamplers) sampler.prepare(sampleRate);
        rackPlayer.prepare(sampleRate); rackStep.store(-1);
        for (auto& mixer : trackMixers) mixer.prepare(sampleRate);
        masterRampCoefficient = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * 0.005)));
        masterRamp = alien::sanitizedMasterGain(masterControls.gain.load()); // no fade-in on start
        click.prepare(sampleRate);
        monitorTrack = -1; lastClickBeat = -1; countInBeats = 0; countInTotalBeats = 0;
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
        discardPendingNotes();
        resetTransport();
    }
    void audioDeviceStopped() override {
        if (auto* take = capture.load()) take->done.store(true);
        resetRecordingForDeviceChange();
        for (std::size_t track = 0; track < sessionTracks; ++track)
            rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        for (auto& player : audioPlayers) player.stop();
        audioClipMode.fill(false);
        allSoundOff(); discardPendingNotes(); resetTransport();
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
    }
    std::atomic<alien::AudioCaptureSink*> capture{};
    std::unique_ptr<alien::PluginRuntime> plugins = std::make_unique<alien::PluginRuntime>();

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputChannels,
        float* const* outputs, int channels, int samples,
        const juce::AudioIODeviceCallbackContext&) override {
        juce::ScopedNoDenormals noDenormals;
        if (auto* take = capture.load(std::memory_order_acquire)) take->append(inputs,inputChannels,samples);
        for (int offset = 0; offset < samples;) {
            // A block boundary falls exactly where a count-in ends, so playback starts on that very sample.
            const int count = std::min({alien::PluginRuntime::blockSize, samples - offset, samplesUntilCountInEnds()});
            renderBlock(outputs, channels, count, offset);
            offset += count;
        }
    }
private:
    void renderBlock(float* const* outputs, int channels, int samples, int outputOffset) {
        handleRecording();
        bool playNow = playing.load();
        const int modeNow = patternMode.load();
        plugins->begin(samples, bpm.load(), playNow, modeNow == 2 ? songStep / 4.0 : transportBeat);
        for (auto& b : clipBuffers) b.clear();
        // A new snapshot is only a pointer swap. The song restarts only when the
        // timeline itself changed (regions or length); a republish that carries
        // the same layout (a mixer, tempo or note edit elsewhere in the project)
        // keeps playing, and edited clip contents are picked up the next time a
        // region starts.
        if (const auto* fresh = arrangementMailbox.consume()) {
            const bool layoutChanged = !fresh->arrangement.sameLayout(playingLayout);
            song = fresh;
            playingLayout = fresh->arrangement;
            if (layoutChanged && modeNow == 2) {
                allSoundOff(); resetTransport();
                for (std::size_t t = 0; t < sessionTracks; ++t) {
                    rolls[t].stop([this,t](int n,float v,bool on) { sendPattern(t,n,v,on); });
                    audioPlayers[t].stop();
                }
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
        if (const auto* fresh = rackMailbox.consume()) rackSnapshot = fresh;
        wasPlaying = playNow; wasMode = modeNow;
        synth.setPatch(alien::presets[static_cast<std::size_t>(juce::jlimit(0, alien::presetCount - 1, preset.load()))]);
        bool anySolo = false;
        for (const auto& track : tracks) anySolo = anySolo || track.solo.load();
        std::array<float, sessionTracks> peaks{};
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            const auto settings = tracks[t].snapshot();
            trackSynths[t].setPatch(alien::presets[static_cast<std::size_t>(std::clamp(settings.preset, 0, alien::presetCount - 1))]);
            trackMixers[t].setTarget(settings, anySolo);
            if (instrumentModes[t] != settings.instrument) {
                trackSynths[t].allSoundOff(); drumSamplers[t].stop();
                instrumentModes[t] = settings.instrument;
            }
            for (int pad = 0; pad < alien::drumPadCount; ++pad)
                drumSamplers[t].setSample(pad, drumKits[t].samples[static_cast<std::size_t>(pad)].load());
        }
        const int requestedRoute = std::clamp(liveDrumTrack.load(), -1, static_cast<int>(sessionTracks) - 1);
        const int route = requestedRoute >= 0 && (instrumentModes[static_cast<std::size_t>(requestedRoute)] == 1 || plugins->hasInstrument(static_cast<std::size_t>(requestedRoute))) ? requestedRoute : -1;
        if (route != currentLiveDrumTrack) {
            midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); });
            stopMidiDrums();
            for (int channel = 1; channel <= 16; ++channel) synth.allSoundOff(channel + 8);
            currentLiveDrumTrack = route;
        }
        updateMonitorRoute();
        NoteQueue::Event event{};
        const bool panic = notes.panic.exchange(false);
        // Bound work per callback even if the producer keeps publishing.
        for (int n = 0; n < 256 && notes.pop(event); ++n) {
            if (!panic) keyboardNote(event.note, event.velocity);
        }
        {   // Previews from the Channel Rack screen (sound whatever the transport is doing).
            NoteQueue::Event preview{};
            for (int n = 0; n < 64 && rackAuditions.pop(preview); ++n)
                if (!panic && preview.note >= 0 && preview.note < alien::rack::maxChannels)
                    rackPlayer.audition(*rackSnapshot, preview.note, preview.velocity,
                                        rackSamples[static_cast<std::size_t>(preview.note)].load(std::memory_order_relaxed));
            if (rackAuditions.panic.exchange(false)) rackPlayer.stop();
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
            auto& requestedClip = *launchScratch;
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
        const bool countingIn = recording.phase.load(std::memory_order_relaxed) == alien::RecordControls::countIn;
        const bool clockRunning = countingIn || playNow;
        const bool clickEnabled = countingIn || recording.metronome.load(std::memory_order_relaxed);
        const float clickVolume = recording.metronomeVolume.load(std::memory_order_relaxed);
        const double beatsPerSample = bpm.load() / (sampleRate * 60.0);
        const double beatBase = countingIn ? countInBeats : transportBeat;
        if (!clockRunning) lastClickBeat = -1;
        for (int i = 0; i < samples; ++i) {
            plugins->sampleOffset = i;
            if (clockRunning) {
                const auto beat = static_cast<std::int64_t>(std::floor(beatBase + i * beatsPerSample + 1.0e-9));
                if (beat != lastClickBeat) {
                    lastClickBeat = beat;
                    if (clickEnabled) click.trigger(beat % 4 == 0, clickVolume);
                }
            }
            clickBuffer[static_cast<std::size_t>(i)] = click.next();
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
                const unsigned allSessionTracks = (1u << static_cast<unsigned>(std::clamp(activeTracks.load(), 1,
                    static_cast<int>(sessionTracks)))) - 1u;
                bool synchronizedAudioReady = true;
                if (synchronizedScenePending)
                    for (std::size_t track = 0; track < sessionTracks; ++track)
                        synchronizedAudioReady = synchronizedAudioReady && (!audioPlayers[track].pendingIsAudio()
                            || audioPlayers[track].pendingReady() || audioPlayers[track].pendingFailed());
                if (synchronizedScenePending && synchronizedAudioReady
                    && (synchronizedSceneTracksQueued & allSessionTracks) == allSessionTracks
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
            plugins->live.setSample(0,i,value); plugins->live.setSample(1,i,value);
            for (std::size_t track = 0; track < sessionTracks; ++track) {
                const float voice = trackSynths[track].nextSample();
                const auto drum = drumSamplers[track].nextSample();
                plugins->tracks[track].setSample(0,i,voice + drum[0]);
                plugins->tracks[track].setSample(1,i,voice + drum[1]);
                if (playNow && (modeNow == 1 || modeNow == 2)) {
                    const auto sample = audioPlayers[track].nextSample();
                    clipBuffers[track].setSample(0,i,sample[0] * audioClipLevel);
                    clipBuffers[track].setSample(1,i,sample[1] * audioClipLevel);
                }
            }
        }
        plugins->instruments(samples);
        for (std::size_t t = 0; t < sessionTracks; ++t)
            for (int c = 0; c < 2; ++c) plugins->tracks[t].addFrom(c,0,clipBuffers[t],c,0,samples);
        renderRack(samples, playNow && modeNow == 3);
        plugins->effectsAndAlign(samples);
        for (int i = 0; i < samples; ++i) {
            float left = plugins->live.getSample(0,i), right = plugins->live.getSample(1,i);
            for (std::size_t t = 0; t < sessionTracks; ++t) {
                float l = plugins->tracks[t].getSample(0,i), r = plugins->tracks[t].getSample(1,i);
                trackMixers[t].process(l,r);
                peaks[t] = std::max(peaks[t], std::max(std::abs(l), std::abs(r)));
                left += l; right += r;
            }
            plugins->master.setSample(0,i,left); plugins->master.setSample(1,i,right);
        }
        plugins->processMaster(samples);
        const float masterTarget = alien::sanitizedMasterGain(masterControls.gain.load(std::memory_order_relaxed));
        float masterPeak = 0.0f;
        for (int i = 0; i < samples; ++i) {
            masterRamp += masterRampCoefficient * (masterTarget - masterRamp);
            if (std::abs(masterRamp - masterTarget) < 1.0e-6f) masterRamp = masterTarget;
            const float left = plugins->master.getSample(0,i) * masterRamp, right = plugins->master.getSample(1,i) * masterRamp;
            if (std::isfinite(left) && std::isfinite(right)) masterPeak = std::max(masterPeak, std::max(std::abs(left), std::abs(right)));
            for (int c = 0; c < channels; ++c) if (outputs[c]) {
                const float sample = (c == 0 ? left : c == 1 ? right : (left + right) * 0.5f) + clickBuffer[static_cast<std::size_t>(i)];
                outputs[c][i + outputOffset] = std::isfinite(sample) ? alien::softLimit(sample) : 0.0f;
            }
        }
        masterControls.peak.store(masterPeak, std::memory_order_relaxed);
        if (playNow) transportBeat += samples * bpm.load() / (sampleRate * 60.0);
        if (countingIn) countInBeats += samples * beatsPerSample;
        pianoRollPosition.store(static_cast<int>(rolls[0].positionStep() * 1000.0));
        for (std::size_t track = 0; track < sessionTracks; ++track) {
            tracks[track].peak.store(peaks[track]);
            sessionTrackPositions[track].store(static_cast<int>(rolls[track].positionStep() * 1000.0));
            activePianoRollClip[track].store(audioClipMode[track] && !audioPlayers[track].isPlaying()
                ? -1 : rolls[track].activeClipIndex());
        }
    }
private:
    // Renders the Channel Rack and adds each output bus after the track
    // instruments, so a plug-in instrument on a track cannot replace it: buses
    // for Session tracks join that track (and so pass through its effects and
    // mixer strip), the master bus joins the live bus, aligned like the tracks.
    void renderRack(int samples, bool running) noexcept {
        auto& buses = *rackBuses;
        std::array<float*, alien::rack::RackPlayer::busCount> left{}, right{};
        for (std::size_t b = 0; b < left.size(); ++b) {
            std::fill_n(buses.left[b].begin(), samples, 0.0f);
            std::fill_n(buses.right[b].begin(), samples, 0.0f);
            left[b] = buses.left[b].data(); right[b] = buses.right[b].data();
        }
        rackPlayer.process(*rackSnapshot, rackSamples, running, bpm.load(), activeTracks.load(), samples, left.data(), right.data());
        rackStep.store(running ? rackPlayer.currentStep() : -1, std::memory_order_relaxed);
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            plugins->tracks[t].addFrom(0, 0, left[t], samples);
            plugins->tracks[t].addFrom(1, 0, right[t], samples);
        }
        plugins->live.addFrom(0, 0, left[alien::rack::RackPlayer::masterBus], samples);
        plugins->live.addFrom(1, 0, right[alien::rack::RackPlayer::masterBus], samples);
    }
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
        for (std::size_t t=0;t<sessionTracks;++t) if (plugins->hasInstrument(t)) plugins->silence(t,1);
        for (auto& sampler : drumSamplers) for (int channel = 1; channel <= 16; ++channel) sampler.stop(channel + 8);
    }
    // --- Recording --------------------------------------------------------------
    // Where the clip on `track` is right now, moved earlier by the output delay
    // plus the user's offset, as a loop pass and a tick inside the loop.
    alien::RecordPosition recordPositionNow(std::size_t track) const noexcept {
        const auto& roll = rolls[track];
        const int loopTicks = roll.loopLengthSteps() * alien::PianoRollPattern::ticksPerStep;
        const double steps = roll.positionStep();
        const double ticks = steps < 0.0 ? 0.0 : steps * alien::PianoRollPattern::ticksPerStep;
        const double delaySamples = deviceLatencySamples.load(std::memory_order_relaxed)
            + recording.latencyOffsetMs.load(std::memory_order_relaxed) * sampleRate / 1000.0;
        const auto shift = std::llround(delaySamples * alien::ticksPerSample(bpm.load(), sampleRate));
        return alien::recordPosition(roll.cycleCount(), ticks, shift, loopTicks);
    }
    bool validRecordingTarget(int track, int scene) const noexcept {
        return track >= 0 && track < std::clamp(activeTracks.load(std::memory_order_relaxed), 1, static_cast<int>(sessionTracks))
            && scene >= 0 && scene < alien::maxSessionScenes;
    }
    void pushRecordEvent(alien::RecordEventKind kind, int note, float velocity) noexcept {
        const int target = recording.targetTrack.load(std::memory_order_relaxed);
        const bool validTarget = target >= 0 && target < static_cast<int>(sessionTracks);
        if (!validTarget && kind != alien::RecordEventKind::end && kind != alien::RecordEventKind::refused) return;
        // A refused take still reports its end so the GUI is never left waiting.
        const auto position = validTarget ? recordPositionNow(static_cast<std::size_t>(target)) : alien::RecordPosition{};
        recording.events.push({kind, static_cast<std::uint8_t>(std::clamp(note, 0, 127)), velocity, position.cycle, position.tick,
                               recording.take.load(std::memory_order_relaxed)});
    }
    // Only notes played while the chosen clip is the one actually looping on its
    // track are captured, so they always line up with that clip.
    void captureNote(int note, float velocity) noexcept {
        if (note < 0 || note > 127 || recording.phase.load(std::memory_order_relaxed) != alien::RecordControls::recording) return;
        const int target = recording.targetTrack.load(std::memory_order_relaxed);
        const int scene = recording.targetScene.load(std::memory_order_relaxed);
        if (!validRecordingTarget(target, scene)) return;
        const auto& roll = rolls[static_cast<std::size_t>(target)];
        if (!roll.isRunning() || roll.activeClipIndex() != scene) return;
        pushRecordEvent(velocity > 0 ? alien::RecordEventKind::noteOn : alien::RecordEventKind::noteOff, note, velocity);
    }
    void endRecording() noexcept {
        pushRecordEvent(alien::RecordEventKind::end, 0, 0.0f);
        recording.phase.store(alien::RecordControls::idle);
        recording.targetTrack.store(-1, std::memory_order_relaxed);
        recording.targetScene.store(-1, std::memory_order_relaxed);
    }
    void resetRecordingForDeviceChange() noexcept {
        const bool pending = recording.startRequested.exchange(false);
        recording.stopRequested.store(false, std::memory_order_relaxed);
        if (recording.phase.load(std::memory_order_relaxed) != alien::RecordControls::idle) endRecording();
        else if (pending) {
            pushRecordEvent(alien::RecordEventKind::refused, 0, 0.0f);
            recording.targetTrack.store(-1, std::memory_order_relaxed);
            recording.targetScene.store(-1, std::memory_order_relaxed);
        } else {
            recording.targetTrack.store(-1, std::memory_order_relaxed);
            recording.targetScene.store(-1, std::memory_order_relaxed);
        }
        recording.phase.store(alien::RecordControls::idle, std::memory_order_relaxed);
    }
    void beginRecording() noexcept {
        if (recording.phase.load() != alien::RecordControls::idle) return; // a take is already running
        const int track = recording.targetTrack.load(), scene = recording.targetScene.load();
        if (!validRecordingTarget(track, scene) || patternMode.load() != 1) {
            pushRecordEvent(alien::RecordEventKind::refused, 0, 0.0f); // tells the GUI the request was refused
            recording.targetTrack.store(-1, std::memory_order_relaxed);
            recording.targetScene.store(-1, std::memory_order_relaxed);
            return;
        }
        const int bars = std::clamp(recording.countInBars.load(), 0, alien::RecordSettings::maxCountInBars);
        if (playing.load()) {
            recording.phase.store(alien::RecordControls::recording);
        } else if (bars > 0) {
            countInBeats = 0; countInTotalBeats = bars * 4.0; lastClickBeat = -1;
            recording.phase.store(alien::RecordControls::countIn);
        } else {
            recording.phase.store(alien::RecordControls::recording);
            playing.store(true);
        }
    }
    void handleRecording() noexcept {
        const bool stopAsked = recording.stopRequested.exchange(false);
        const bool startAsked = recording.startRequested.exchange(false);
        // Start first: a stop that arrives in the same block ends the take it just began.
        if (startAsked) beginRecording();
        if (stopAsked && recording.phase.load() != alien::RecordControls::idle) endRecording();
        const int phase = recording.phase.load();
        if (phase == alien::RecordControls::countIn && countInBeats >= countInTotalBeats - 1.0e-6) {
            recording.phase.store(alien::RecordControls::recording);
            lastClickBeat = -1; // the first beat of playback is an accented click
            playing.store(true);
        } else if (phase != alien::RecordControls::idle && patternMode.load() != 1) {
            endRecording(); // the view changed to one that does not play Session clips
        } else if (phase == alien::RecordControls::recording && !playing.load()) {
            endRecording(); // playback was stopped
        }
    }
    int samplesUntilCountInEnds() const noexcept {
        if (recording.phase.load(std::memory_order_relaxed) != alien::RecordControls::countIn) return std::numeric_limits<int>::max();
        const double perSample = bpm.load() / (sampleRate * 60.0);
        const double left = countInTotalBeats - countInBeats;
        if (!(perSample > 0.0) || left <= 0.0) return 1;
        return static_cast<int>(std::clamp(std::ceil(left / perSample - 1.0e-9), 1.0, 1.0e9));
    }
    // While a take is in progress the played notes go to the chosen track's own
    // instrument and mixer, so you hear what the recording will sound like.
    void updateMonitorRoute() noexcept {
        const int target = recording.targetTrack.load(std::memory_order_relaxed);
        const int scene = recording.targetScene.load(std::memory_order_relaxed);
        const int wanted = recording.phase.load(std::memory_order_relaxed) != alien::RecordControls::idle
                               && validRecordingTarget(target, scene) ? target : -1;
        if (wanted == monitorTrack) return;
        midi.resetAudio([this](int n, float v, int ch) { sendMidi(n, v, ch); }); // releases through the old route
        if (monitorTrack >= 0) releaseMonitor(static_cast<std::size_t>(monitorTrack));
        else synth.allSoundOff(0);
        monitorTrack = wanted;
    }
    void releaseMonitor(std::size_t track) noexcept {
        if (plugins->hasInstrument(track)) { plugins->silence(track, 1); return; }
        if (instrumentModes[track] == 1) { for (int c = 8; c <= 24; ++c) drumSamplers[track].stop(c); return; }
        for (int source = 64; source <= 80; ++source)
            for (int note = 0; note < 128; ++note) trackSynths[track].noteOff(note, source);
    }
    void routeToTrack(std::size_t track, int note, float velocity, int source) noexcept {
        if (plugins->hasInstrument(track)) { plugins->note(track, note, velocity, velocity > 0); return; }
        if (instrumentModes[track] == 1) { if (velocity > 0) drumSamplers[track].noteOn(note, velocity, source - 56); return; }
        if (velocity > 0) trackSynths[track].noteOn(note, velocity, source);
        else trackSynths[track].noteOff(note, source);
    }
    // Computer-keyboard and mouse piano notes.
    void keyboardNote(int note, float velocity) noexcept {
        captureNote(note, velocity);
        if (monitorTrack >= 0) { routeToTrack(static_cast<std::size_t>(monitorTrack), note, velocity, 64); return; }
        if (velocity > 0) synth.noteOn(note, velocity);
        else synth.noteOff(note);
    }

    void sendMidi(int note, float velocity, int channel) noexcept {
        captureNote(note, velocity);
        if (monitorTrack >= 0) {
            if (note < 0) releaseMonitor(static_cast<std::size_t>(monitorTrack));
            else routeToTrack(static_cast<std::size_t>(monitorTrack), note, velocity, 64 + channel);
            return;
        }
        if (currentLiveDrumTrack >= 0 && plugins->hasInstrument(static_cast<std::size_t>(currentLiveDrumTrack))) {
            const auto track = static_cast<std::size_t>(currentLiveDrumTrack);
            if (note < 0) for (int n=0;n<128;++n) plugins->note(track,n,0,false);
            else plugins->note(track,note,velocity,velocity>0);
            return;
        }
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
        if (plugins->hasInstrument(track)) { plugins->note(track, note, velocity, on, 2); return; }
        if (instrumentModes[track] == 1) {
            if (on) drumSamplers[track].noteOn(note, velocity);
            return;
        }
        if (on) trackSynths[track].noteOn(note, velocity, source);
        else trackSynths[track].noteOff(note, source);
    }
    void allSoundOff() noexcept {
        synth.allSoundOff();
        rackPlayer.stop();

        for (std::size_t t=0;t<sessionTracks;++t) if (plugins->hasInstrument(t))
            plugins->silence(t);
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
        transportBeat = 0;
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
    // Immutable snapshot owned by arrangementMailbox; only the audio thread moves it.
    const alien::ArrangementPlayback* song = arrangementMailbox.current();
    alien::Arrangement playingLayout; // Layout of `song`, to tell a restart-worthy change from a refresh.
    double songStep{}, transportBeat{};
    std::array<juce::AudioBuffer<float>, sessionTracks> clipBuffers;
    int lastSongBar{-1};
    std::array<int, sessionTracks> activeSongRegions{-2,-2,-2,-2};
    alien::SynthEngine synth; // Live keyboard and legacy step sequencer.
    FixedHeapArray<alien::SynthEngine, sessionTracks> trackSynths;
    FixedHeapArray<alien::TrackMixer, sessionTracks> trackMixers;
    float masterRamp{1.0f}, masterRampCoefficient{1.0f};
    FixedHeapArray<alien::DrumSampler, sessionTracks> drumSamplers;
    std::array<int, sessionTracks> instrumentModes{};
    int currentLiveDrumTrack{-1};
    alien::MetronomeClick click;
    std::array<float, alien::PluginRuntime::blockSize> clickBuffer{};
    std::int64_t lastClickBeat{-1};
    double countInBeats{}, countInTotalBeats{};
    int monitorTrack{-1}; // track whose instrument plays live input during a take, or -1
    alien::StepClock clock;
    struct RackBuses {
        std::array<std::array<float, alien::PluginRuntime::blockSize>, alien::rack::RackPlayer::busCount> left{}, right{};
    };
    std::unique_ptr<RackBuses> rackBuses = std::make_unique<RackBuses>();
    alien::rack::RackPlayer rackPlayer;
    const alien::rack::Snapshot* rackSnapshot = rackMailbox.current();
    alien::DiskAudioStreamService diskStreams;
    FixedHeapArray<alien::PianoRollScheduler, sessionTracks> rolls;
    FixedHeapArray<alien::SessionAudioPlayer, sessionTracks> audioPlayers;
    std::unique_ptr<alien::SessionClipLaunch> launchScratch = std::make_unique<alien::SessionClipLaunch>();
    std::array<bool, sessionTracks> audioClipMode{};
    double sampleRate{44100.0};
    bool wasPlaying{};
    int wasMode{};
    bool synchronizedScenePending{};
    unsigned synchronizedSceneTracksQueued{};
    static_assert(sessionTracks <= 32, "Scene launch tracking uses a 32-bit mask");
    int synchronizedSceneQuantizationSteps{16};
    int activeStepPitch{-1};
};

