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
#include "SongRecording.h"
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
    std::atomic<int> deviceLatencySamples{0}, deviceInputLatencySamples{0};
    // A Song-view audio take (recording.audioTake): the armed tracks' sinks and what was captured.
    std::unique_ptr<alien::SongTakeCapture> songTake = std::make_unique<alien::SongTakeCapture>();
    std::array<alien::DrumKitControls, sessionTracks> drumKits;
    std::array<NoteQueue, sessionTracks> drumAuditions;
    std::atomic<int> liveDrumTrack{-1};
    alien::ArrangementMailbox arrangementMailbox;
    // The arrangement timeline. The GUI asks for a position with timelineSeek (the
    // song starts there when playback starts, or jumps there if it is playing) and
    // reads where the song is from timelinePosition. Both count ticks (960 to a
    // quarter note); -1 means "no request" and "not playing".
    std::atomic<std::int64_t> timelineSeek{-1}, timelinePosition{-1};
    // Export only: a tick before the end of the song where playback stops (-1: the song's end).
    std::atomic<std::int64_t> timelineEnd{-1};
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
    // A rack audition names a channel, and optionally a pitch in semitones (-24..24) to play it at.
    static constexpr int rackAuditionEvent(int channel, int pitch = 0) noexcept { return pitch == 0 ? channel : channel | ((pitch + 25) << 8); }
    std::atomic<int> rackStep{-1}; // step of the selected pattern that started last, or -1

    ToneEngine() noexcept {
        for (std::size_t t = 0; t < sessionTracks; ++t)
            audioPlayers[t].bind(diskStreams.voice(t * 2), diskStreams.voice(t * 2 + 1));
        for (std::size_t t = 0; t < sessionTracks; ++t)
            tailPlayers[t].bind(diskStreams.voice(sessionTracks * 2 + t * 2), diskStreams.voice(sessionTracks * 2 + t * 2 + 1));
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
    // For a render's own engine, before it plays anything: disk-streamed clips are then read on the
    // rendering thread, so the same project always renders to the same samples.
    void renderStreamsDeterministically() noexcept { diskStreams.setDeterministic(); }
    std::uint64_t sessionStreamUnderruns(std::size_t track) const noexcept {
        return track < sessionTracks ? audioPlayers[track].underrunCount() + tailPlayers[track].underrunCount() : 0;
    }
    bool sessionStreamFailed(std::size_t track) const noexcept {
        return track < sessionTracks && (audioPlayers[track].streamFailed() || tailPlayers[track].streamFailed());
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        prepare(device->getCurrentSampleRate());
        deviceLatencySamples.store(std::max(0, device->getOutputLatencyInSamples()));
        deviceInputLatencySamples.store(std::max(0, device->getInputLatencyInSamples()));
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
        stopClipPlayers();
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
        stopClipPlayers();
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
        deviceInputs = inputs; deviceInputCount = inputs != nullptr ? std::max(0, inputChannels) : 0;
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
        plugins->begin(samples, bpm.load(), playNow, modeNow == 2 ? timelineTick / alien::timeline::ticksPerBeat : transportBeat);
        for (auto& b : clipBuffers) b.clear();
        // A new snapshot is only a pointer swap. While the song plays, an edit
        // never restarts it: every lane re-checks what should be sounding at the
        // playhead and changes only what differs (a moved, trimmed or deleted
        // region, edited notes, a new loop range).
        if (const auto* fresh = arrangementMailbox.consume()) {
            song = fresh;
            if (modeNow == 2 && playNow && wasPlaying && wasMode == 2) resyncTimeline();
        }
        if (synchronizedSceneLaunch.exchange(false)) {
            synchronizedScenePending = true;
            synchronizedSceneTracksQueued = 0;
            synchronizedSceneQuantizationSteps = launchQuantizationSteps.load();
        }
        if ((wasPlaying && !playNow) || wasMode != modeNow) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            stopClipPlayers();
            audioClipMode.fill(false);
            allSoundOff(); resetTransport();
            midi.requestReset();
        }
        if (const auto* fresh = rackMailbox.consume()) rackSnapshot = fresh;
        if (modeNow == 2) {
            // A requested position is where the song starts when it next plays, or
            // where it jumps to if it already is.
            const bool begins = playNow && (!wasPlaying || wasMode != modeNow);
            const auto stopAt = timelineEnd.load(std::memory_order_relaxed);
            timelineStopAt = stopAt >= 0 ? static_cast<double>(stopAt) : std::numeric_limits<double>::max();
            const auto request = timelineSeek.exchange(-1);
            if (request >= 0) timelineStart = static_cast<double>(std::min<std::int64_t>(request, alien::timeline::maxLength));
            if (begins) seekTimeline(timelineStart, true);
            else if (request >= 0 && playNow) seekTimeline(timelineStart, true);
            else if (request >= 0) setTimelineTick(timelineStart);
        }
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
            for (int n = 0; n < 64 && rackAuditions.pop(preview); ++n) {
                const int channel = preview.note & 0xff, pitch = preview.note >> 8 == 0 ? 0 : (preview.note >> 8) - 25;
                if (!panic && preview.note >= 0 && channel < alien::rack::maxChannels)
                    rackPlayer.audition(*rackSnapshot, channel, preview.velocity,
                                        rackSamples[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed), pitch);
            }
            if (rackAuditions.panic.exchange(false)) rackPlayer.stop();
        }
        if (panic) {
            for (std::size_t track = 0; track < sessionTracks; ++track)
                rolls[track].stop([this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
            stopClipPlayers();
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
                if (channel < 0) {
                    controllerMappings.resetEdges(); stopMidiDrums(); resetMidiControllers(); return;
                }
                sendMidiControl(channel,controller,value);
                if(controller<128) {
                    const int action = controllerMappings.processCC(channel, controller, value);
                    if (action >= 0) controllerActions.push(action);
                }
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
                rolls[track].setTiming(sampleRate, bpm.load());   // the timeline starts and stops each clip itself
            else
                rolls[track].beginBlock(pianoRollNotes[track], playNow && modeNow == 1, sampleRate, bpm.load(),
                    [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); });
        }
        const bool countingIn = recording.phase.load(std::memory_order_relaxed) == alien::RecordControls::countIn;
        const bool clockRunning = countingIn || playNow;
        const bool clickEnabled = countingIn || recording.metronome.load(std::memory_order_relaxed);
        const float clickVolume = recording.metronomeVolume.load(std::memory_order_relaxed);
        const double beatsPerSample = bpm.load() / (sampleRate * 60.0);
        const double ticksPerSample = beatsPerSample * static_cast<double>(alien::timeline::ticksPerBeat);
        rackHitCount = 0;
        if (timelineRate != ticksPerSample) { timelineRate = ticksPerSample; setTimelineTick(timelineTick); }   // a tempo change starts a new segment
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
            automationTicks[static_cast<std::size_t>(i)] = -1.0;
            if (playNow && modeNow == 2 && !timelineSample(i)) {
                allSoundOff(); playing.store(false); playNow = false;   // the song reached its end
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
                    const auto& lane = trackLanes[track];
                    const double into = std::max(0.0, timelineTick - static_cast<double>(lane.start));
                    const double remaining = std::max(0.0, static_cast<double>(lane.end) - timelineTick);
                    double editGain = lane.gain;
                    if (lane.fadeIn > 0) editGain *= std::clamp(into / static_cast<double>(lane.fadeIn), 0.0, 1.0);
                    if (lane.fadeOut > 0) editGain *= std::clamp(remaining / static_cast<double>(lane.fadeOut), 0.0, 1.0);
                    if (lane.crossfade > 0) editGain *= equalPowerIn(into / static_cast<double>(lane.crossfade));
                    const float level = static_cast<float>(editGain) * audioClipLevel;
                    float clipLeft = sample[0] * level, clipRight = sample[1] * level;
                    if (const auto& tail = tails[track]; tail.active) {
                        const auto out = tailPlayers[track].nextSample();
                        const float tailLevel = static_cast<float>(tail.gain
                            * equalPowerOut((timelineTick - tail.start) / (tail.end - tail.start))) * audioClipLevel;
                        clipLeft += out[0] * tailLevel; clipRight += out[1] * tailLevel;
                    }
                    clipBuffers[track].setSample(0,i,clipLeft);
                    clipBuffers[track].setSample(1,i,clipRight);
                }
            }
        }
        plugins->instruments(samples);
        for (std::size_t t = 0; t < sessionTracks; ++t)
            for (int c = 0; c < 2; ++c) plugins->tracks[t].addFrom(c,0,clipBuffers[t],c,0,samples);
        addTrackInputs(samples, outputOffset);
        if (modeNow == 2) captureSongTake(samples, outputOffset);
        renderRack(samples, playNow && modeNow == 3, playNow && modeNow == 2);
        plugins->effectsAndAlign(samples);
        for (int i = 0; i < samples; ++i) {
            float left = plugins->live.getSample(0,i), right = plugins->live.getSample(1,i);
            for (std::size_t t = 0; t < sessionTracks; ++t) {
                float l = plugins->tracks[t].getSample(0,i), r = plugins->tracks[t].getSample(1,i);
                auto settings = tracks[t].snapshot();
                bool automated = false;
                const double automationTick = automationTicks[static_cast<std::size_t>(i)];
                if (modeNow == 2 && automationTick >= 0.0) {
                    for (const auto& lane : song->timeline.automationLanes) {
                        if (lane.track != static_cast<int>(t) || !lane.enabled || lane.points.empty()) continue;
                        automated = true;
                        if (lane.parameter == alien::automation::Parameter::volume) settings.gain = lane.valueAt(automationTick, settings.gain);
                        else settings.pan = lane.valueAt(automationTick, settings.pan);
                    }
                }
                if (automated) trackMixers[t].processAutomated(l, r, settings, anySolo);
                else trackMixers[t].process(l,r);
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
        {
            // The playhead is shown where it will be: a loop wraps at the start of the next sample, so a block that
            // ends exactly on the loop's end already reports the loop's start.
            const auto& tl = song->timeline;
            double shown = timelineTick;
            if (tl.loopEnabled && loopArmed && shown + tickEpsilon >= static_cast<double>(tl.loopEnd))
                shown = static_cast<double>(tl.loopStart) + std::max(0.0, shown - static_cast<double>(tl.loopEnd));
            timelinePosition.store(playNow && modeNow == 2 ? static_cast<std::int64_t>(shown) : -1, std::memory_order_relaxed);
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
    // Renders the Channel Rack and adds each output bus after the track
    // instruments, so a plug-in instrument on a track cannot replace it: buses
    // for Session tracks join that track (and so pass through its effects and
    // mixer strip), the master bus joins the live bus, aligned like the tracks.
    void renderRack(int samples, bool running, bool timelineRunning) noexcept {
        auto& buses = *rackBuses;
        std::array<float*, alien::rack::RackPlayer::busCount> left{}, right{};
        for (std::size_t b = 0; b < left.size(); ++b) {
            std::fill_n(buses.left[b].begin(), samples, 0.0f);
            std::fill_n(buses.right[b].begin(), samples, 0.0f);
            left[b] = buses.left[b].data(); right[b] = buses.right[b].data();
        }
        if (timelineRunning)
            rackPlayer.processTimeline(*rackSnapshot, rackSamples, true, rackHits.data(), rackHitCount, activeTracks.load(),
                                       samples, left.data(), right.data());
        else
            rackPlayer.process(*rackSnapshot, rackSamples, running, bpm.load(), activeTracks.load(), samples, left.data(), right.data());
        rackStep.store(running ? rackPlayer.currentStep() : -1, std::memory_order_relaxed);
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            plugins->tracks[t].addFrom(0, 0, left[t], samples);
            plugins->tracks[t].addFrom(1, 0, right[t], samples);
        }
        plugins->live.addFrom(0, 0, left[alien::rack::RackPlayer::masterBus], samples);
        plugins->live.addFrom(1, 0, right[alien::rack::RackPlayer::masterBus], samples);
    }
    // --- Live audio input --------------------------------------------------------------
    // An armed or monitoring track meters its input; a monitoring one also plays it, through the
    // track's own effects and fader like anything else on the track. A mono input feeds both sides.
    const float* inputChannel(int index) const noexcept {
        return deviceInputs != nullptr && index >= 0 && index < deviceInputCount ? deviceInputs[index] : nullptr;
    }
    void addTrackInputs(int samples, int offset) noexcept {
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            auto& controls = tracks[t];
            const bool armed = controls.armed.load(std::memory_order_relaxed);
            const int monitor = controls.monitor.load(std::memory_order_relaxed);
            const bool monitoring = monitor == static_cast<int>(alien::InputMonitor::always)
                || (monitor == static_cast<int>(alien::InputMonitor::automatic) && armed);
            if (!armed && !monitoring) continue;
            const int first = controls.input.load(std::memory_order_relaxed);
            const float* left = inputChannel(first);
            const float* right = controls.stereoInput.load(std::memory_order_relaxed) ? inputChannel(first + 1) : left;
            if (left == nullptr && right == nullptr) continue;
            float peak = 0.0f;
            for (int i = 0; i < samples; ++i) {
                float l = left != nullptr ? left[offset + i] : 0.0f, r = right != nullptr ? right[offset + i] : 0.0f;
                if (!std::isfinite(l)) l = 0.0f;
                if (!std::isfinite(r)) r = 0.0f;
                peak = std::max({peak, std::abs(l), std::abs(r)});
                if (monitoring) { plugins->tracks[t].addSample(0, i, l); plugins->tracks[t].addSample(1, i, r); }
            }
            if (peak > controls.inputPeak.load(std::memory_order_relaxed)) controls.inputPeak.store(peak, std::memory_order_relaxed);
        }
    }
    // A Song-view audio take: every armed track's sink receives the input of the samples whose song
    // position is being recorded (all of them, or only those inside the punch range). Those samples
    // are one run in the block, so each sink gets one append. The take ends by itself at the punch-out
    // or when the loop wraps (each pass would need a take lane of its own).
    void captureSongTake(int samples, int offset) noexcept {
        auto& take = *songTake;
        if (!recording.audioTake.load(std::memory_order_relaxed)
            || recording.phase.load(std::memory_order_relaxed) != alien::RecordControls::recording) return;
        const bool punch = take.punch.load(std::memory_order_relaxed);
        const double punchIn = static_cast<double>(take.punchIn.load(std::memory_order_relaxed));
        const double punchOut = static_cast<double>(take.punchOut.load(std::memory_order_relaxed));
        const bool started = take.frames.load(std::memory_order_relaxed) > 0;
        int first = -1, last = -1, ending = alien::SongTakeCapture::notEnded;
        double previous = songTakeLastTick;
        for (int i = 0; i < samples && ending == alien::SongTakeCapture::notEnded; ++i) {
            const double tick = automationTicks[static_cast<std::size_t>(i)];
            if (tick < 0.0) continue;
            const bool capturing = (started || first >= 0);
            // The playhead went back (the loop wrapped) or jumped ahead (a seek): the take would no longer be one piece.
            if (capturing && tick + tickEpsilon < previous) { ending = alien::SongTakeCapture::loopWrapped; break; }
            if (capturing && tick > previous + timelineRate + 1.0) { ending = alien::SongTakeCapture::jumped; break; }
            previous = tick;
            const bool inside = !punch || (tick + tickEpsilon >= punchIn && tick + tickEpsilon < punchOut);
            if (inside) { if (first < 0) first = i; last = i; }
            else if (capturing) ending = alien::SongTakeCapture::punchedOut;
        }
        songTakeLastTick = previous;
        if (first >= 0) {
            const int count = last - first + 1;
            if (!started) take.firstTick.store(automationTicks[static_cast<std::size_t>(first)], std::memory_order_relaxed);
            for (std::size_t t = 0; t < sessionTracks; ++t) {
                auto* sink = take.sinks[t].load(std::memory_order_acquire);
                if (sink == nullptr) continue;
                const int input = tracks[t].input.load(std::memory_order_relaxed);
                const float* left = inputChannel(input);
                const float* right = sink->channels == 2 ? inputChannel(input + 1) : nullptr;
                const float* from[2]{left != nullptr ? left + offset + first : nullptr,
                                     right != nullptr ? right + offset + first : nullptr};
                // A missing channel (the device lost it) records silence instead of shifting the take.
                sink->append(from, sink->channels, count);
            }
            take.frames.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_release);
        }
        if (ending != alien::SongTakeCapture::notEnded) endSongTake(ending);
    }
    void endSongTake(int reason) noexcept {
        songTake->ended.store(reason, std::memory_order_relaxed);
        endRecording();
    }

    // --- Arrangement timeline --------------------------------------------------------
    // Each Session track and each rack lane plays at most one region at a time.
    // A lane remembers which region it started and the tick at which that changes
    // (the region ends, or the next one begins), so the per-sample cost is one
    // comparison per lane; looking regions up costs only at those moments.
    static constexpr int timelineFadeSamples = 64;     // about 1.5 ms: joining or leaving the middle of a waveform
    struct TimelineLane {
        bool active{}, midi{};
        int source{-1};
        std::int64_t start{}, end{}, offset{};
        double audioOffset{}, gain{1.0};
        std::int64_t fadeIn{}, fadeOut{}, crossfade{};
        bool loopSource{}, reversed{};
        const alien::SessionAudioClip* audio{};
        double boundary{-1.0};         // sync when the playhead reaches this tick
        std::int64_t nextStep{};       // rack lanes: the next pattern step due, counted from the region's content start
        // Audio streamed from disk: the track waits for the stream to be buffered, and the next region's stream is asked for ahead of time.
        bool audioWaiting{};
        const alien::SessionAudioClip* nextAudio{};
        double nextAudioOffset{}, prefetchAt{-1.0};
        bool prefetched{};
        double lead{};                 // while waiting: how far ahead of the song (seconds) the stream was last asked to start
    };
    // The clip a crossfade leaves: it plays on past its region's end on the track's tail player,
    // fading out over [start, end) while the region that starts at `start` fades in.
    struct CrossfadeTail {
        bool active{}, waiting{};      // waiting: its stream was asked for and is not buffered yet
        double start{}, end{};
        double gain{1.0};
        const alien::SessionAudioClip* audio{};
        std::int64_t origin{};         // tick at which the clip's audioOffset plays
        double audioOffset{};
    };
    static double equalPowerIn(double x) noexcept { return std::sin(std::clamp(x, 0.0, 1.0) * 1.5707963267948966); }
    static double equalPowerOut(double x) noexcept { return std::cos(std::clamp(x, 0.0, 1.0) * 1.5707963267948966); }
    void stopClipPlayers() noexcept {
        for (auto& player : audioPlayers) player.stop();
        for (auto& player : tailPlayers) player.stop();
        for (auto& tail : tails) tail = {};
    }
    void endTail(std::size_t track, bool hard) noexcept {
        if (!tails[track].active) return;
        if (hard) tailPlayers[track].stop();
        else { tailPlayers[track].release(timelineFadeSamples); tailPlayers[track].cancelPending(); }
        tails[track] = {};
    }
    // The audio region that ends where `region` starts on its track, if a crossfade can continue it.
    const alien::timeline::Region* crossfadePartner(std::size_t track, const alien::timeline::Region& region) const noexcept {
        for (const auto& r : song->timeline.regions)
            if (r.row == region.row && r.end() == region.start && !r.muted && r.fadeOut == 0
                && song->clips[static_cast<std::size_t>(r.source)][track].audio != nullptr) return &r;
        return nullptr;
    }
    // Starts a crossfade's outgoing clip part-way through (the song started or jumped inside the crossfade).
    void startTail(std::size_t track, const alien::timeline::Region& region) noexcept {
        const auto* left = crossfadePartner(track, region);
        if (left == nullptr) return;
        const auto* audio = song->clips[static_cast<std::size_t>(left->source)][track].audio;
        if (audio->streamed() && (left->reversed || left->loopSource)) return;   // as startTrackRegion: no reverse/loop streaming
        auto& tail = tails[track];
        tail = {}; tail.active = true; tail.start = static_cast<double>(region.start);
        tail.end = static_cast<double>(region.start + region.crossfade); tail.gain = left->gain;
        tail.audio = audio; tail.origin = left->start; tail.audioOffset = left->audioOffset;
        const double frame = tailFrame(tail, left->reversed);
        if (frame < 0.0 || frame >= static_cast<double>(audio->frames)) { tail = {}; return; }
        auto& player = tailPlayers[track];
        player.queue(audio, 1, frame);
        if (!player.launchPendingNow(sampleRate, frame, timelineFadeSamples, left->reversed, left->loopSource,
                                     left->audioOffset * audio->sampleRate) && player.hasPending()) tail.waiting = true;
    }
    double tailFrame(const CrossfadeTail& tail, bool reversed) const noexcept {
        const double seconds = tail.audioOffset + alien::timeline::secondsForTicks(timelineTick - static_cast<double>(tail.origin), bpm.load());
        const double forward = seconds * tail.audio->sampleRate;
        return reversed ? static_cast<double>(tail.audio->frames - 1) - forward : forward;
    }
    // A crossfade ends where its outgoing side is silent; a streamed one that was not ready starts once it is.
    void advanceTail(std::size_t track, double at) noexcept {
        auto& tail = tails[track];
        if (!tail.active) return;
        if (at >= tail.end) { tailPlayers[track].stop(); tail = {}; return; }
        if (!tail.waiting) return;
        auto& player = tailPlayers[track];
        if (!player.hasPending()) { tail.waiting = false; return; }
        if (player.pendingReady() || player.pendingFailed()) {
            player.launchPendingNow(sampleRate, tailFrame(tail, false), timelineFadeSamples);
            if (!player.hasPending()) tail.waiting = false;
        }
    }
    static constexpr double timelinePrefetchSeconds = 0.25;   // how long before a streamed region starts its audio is requested
    static constexpr double timelineLaunchWindow = 1024.0;    // a buffered stream may start this many frames after the point it was asked for
    static constexpr double timelineCatchUpSeconds = 0.1, timelineMaxCatchUpSeconds = 1.6;
    // The region on `row` that covers the playhead, and the tick at which "what plays on this row" next changes.
    // `upcoming`, if given, is the first region on the row that starts after the playhead.
    const alien::timeline::Region* regionNow(int row, double& boundary, const alien::timeline::Region** upcoming = nullptr) const noexcept {
        const alien::timeline::Region* found = nullptr;
        const alien::timeline::Region* next = nullptr;
        const double at = timelineTick + tickEpsilon;
        boundary = std::numeric_limits<double>::infinity();
        for (const auto& r : song->timeline.regions) {
            if (r.row != row) continue;
            if (at >= static_cast<double>(r.start) && at < static_cast<double>(r.end())) {
                found = &r; boundary = std::min(boundary, static_cast<double>(r.end()));
            } else if (static_cast<double>(r.start) > at) {
                boundary = std::min(boundary, static_cast<double>(r.start));
                if (next == nullptr || r.start < next->start) next = &r;
            }
        }
        if (upcoming != nullptr) *upcoming = next;
        return found;
    }
    // Stops everything a track is playing for the timeline. A hard cut also silences
    // the voices' release tails, as when the playhead jumps somewhere else.
    void clearTrack(std::size_t track, bool hard) noexcept {
        auto send = [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); };
        rolls[track].stop(send);
        endTail(track, hard);
        if (hard) { audioPlayers[track].stop(); trackSynths[track].allSoundOff(); drumSamplers[track].stop(0); }
        else {
            audioPlayers[track].release(timelineFadeSamples);
            if (trackLanes[track].audioWaiting) audioPlayers[track].cancelPending();   // its audio never started
        }
        audioClipMode[track] = false;
        const double boundary = trackLanes[track].boundary;
        trackLanes[track] = {};
        trackLanes[track].boundary = boundary;
    }
    void startTrackRegion(std::size_t track, const alien::timeline::Region& region) noexcept {
        auto send = [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); };
        const auto& clip = song->clips[static_cast<std::size_t>(region.source)][track];
        const double into = std::max(0.0, timelineTick - static_cast<double>(region.start));
        auto& lane = trackLanes[track];
        lane.active = true; lane.source = region.source; lane.start = region.start; lane.end = region.end();
        lane.offset = region.offset; lane.audioOffset = region.audioOffset; lane.gain = region.gain;
        lane.fadeIn = region.fadeIn; lane.fadeOut = region.fadeOut; lane.loopSource = region.loopSource; lane.reversed = region.reversed; lane.audio = clip.audio;
        lane.crossfade = clip.audio != nullptr ? region.crossfade : 0;
        if (clip.audio != nullptr && region.crossfade > 0 && !tails[track].active && into < static_cast<double>(region.crossfade))
            startTail(track, region);
        if (clip.audio != nullptr) {
            // DiskAudioStream currently reads forward. Older or externally edited projects may
            // nevertheless contain these flags, so keep their audio audible as a forward one-shot
            // until reverse/loop-aware streaming is available.
            const bool reverse = region.reversed && !clip.audio->streamed();
            const bool loopSource = region.loopSource && !clip.audio->streamed();
            lane.reversed = reverse; lane.loopSource = loopSource;
            const double seconds = region.audioOffset + alien::timeline::secondsForTicks(into, bpm.load());
            const double forwardFrame = seconds * clip.audio->sampleRate;
            const double startFrame = reverse ? static_cast<double>(clip.audio->frames - 1) - forwardFrame : forwardFrame;
            if (startFrame >= 0.0 && startFrame < static_cast<double>(clip.audio->frames)) {
                auto& player = audioPlayers[track];
                // A streamed clip asked for ahead of time is ready now; one that is not waits, and
                // starts where the song has got to by then, so it stays in time.
                if (!player.pendingMatches(clip.audio, startFrame, timelineLaunchWindow)) player.queue(clip.audio, 1, startFrame);
                if (!player.launchPendingNow(sampleRate, startFrame, timelineFadeSamples, reverse, loopSource,
                                             region.audioOffset * clip.audio->sampleRate) && player.hasPending()) lane.audioWaiting = true;
            }
            audioClipMode[track] = true;
        } else {
            lane.midi = rolls[track].startAt(clip.pattern, region.source,
                (static_cast<double>(region.offset) + into) / alien::timeline::ticksPerStep, send);
        }
    }
    // Makes the track play what the timeline says belongs at the playhead, changing
    // only what differs from what it is already playing.
    void syncTrack(std::size_t track) noexcept {
        const alien::timeline::Region* upcoming = nullptr;
        syncTrackNow(track, upcoming);
        noteUpcoming(trackLanes[track], track, upcoming);
    }
    // Remembers the next region on a track if its audio streams from disk, so that audio can be requested a moment early.
    void noteUpcoming(TimelineLane& lane, std::size_t track, const alien::timeline::Region* next) noexcept {
        lane.nextAudio = nullptr; lane.prefetchAt = -1.0; lane.prefetched = false;
        if (next == nullptr || next->muted) return;
        const auto& clip = song->clips[static_cast<std::size_t>(next->source)][track];
        if (clip.audio == nullptr || !clip.audio->streamed()) return;
        const double ticksPerSecond = static_cast<double>(bpm.load()) * static_cast<double>(alien::timeline::ticksPerBeat) / 60.0;
        lane.nextAudio = clip.audio; lane.nextAudioOffset = next->audioOffset;
        lane.prefetchAt = static_cast<double>(next->start) - timelinePrefetchSeconds * ticksPerSecond;
    }
    void prefetchNext(std::size_t track) noexcept {
        auto& lane = trackLanes[track];
        lane.prefetched = true;
        const double startFrame = lane.nextAudioOffset * lane.nextAudio->sampleRate;
        if (lane.audioWaiting || startFrame >= static_cast<double>(lane.nextAudio->frames)) return;
        if (!audioPlayers[track].pendingMatches(lane.nextAudio, startFrame)) audioPlayers[track].queue(lane.nextAudio, 1, startFrame);
    }
    // A streamed region whose audio was not buffered when it began. Once the stream is ready the audio starts where
    // the song has got to, so it stays in time. If the song has moved on too far for what was buffered, the stream is
    // asked again for a point just ahead of the song and starts when the song gets there.
    void launchWaiting(std::size_t track) noexcept {
        auto& lane = trackLanes[track];
        auto& player = audioPlayers[track];
        if (!player.hasPending() || lane.audio == nullptr) { lane.audioWaiting = false; return; }
        const double into = std::max(0.0, timelineTick - static_cast<double>(lane.start));
        const double frames = static_cast<double>(lane.audio->frames);
        const double startFrame = (lane.audioOffset + alien::timeline::secondsForTicks(into, bpm.load())) * lane.audio->sampleRate;
        if (startFrame >= frames) { player.cancelPending(); lane.audioWaiting = false; return; }
        const double ahead = startFrame - player.pendingStartFrame();
        if (ahead < -0.5) return;                                   // asked for a moment ahead of the song: not due yet
        if (ahead <= timelineLaunchWindow) {
            if (player.pendingReady() || player.pendingFailed()) {
                player.launchPendingNow(sampleRate, startFrame, timelineFadeSamples);
                if (!player.hasPending()) lane.audioWaiting = false;   // started, or the stream failed
            }
            return;
        }
        lane.lead = std::clamp(lane.lead * 2.0, timelineCatchUpSeconds, timelineMaxCatchUpSeconds);
        const double target = startFrame + lane.lead * lane.audio->sampleRate;
        if (target >= frames) { player.cancelPending(); lane.audioWaiting = false; return; }   // the clip would be over by then
        player.queue(lane.audio, 1, target);
    }
    void syncTrackNow(std::size_t track, const alien::timeline::Region*& upcoming) noexcept {
        auto& lane = trackLanes[track];
        const auto* region = regionNow(static_cast<int>(track), lane.boundary, &upcoming);
        if (region != nullptr && region->muted) region = nullptr;
        if (region == nullptr) {
            if (lane.active) clearTrack(track, false);
            return;
        }
        const auto& clip = song->clips[static_cast<std::size_t>(region->source)][track];
        // The same content keeps playing through any edit that does not move it: a region made longer or
        // shorter, or trimmed at the front, is the same sound starting or stopping elsewhere.
        const bool same = lane.active && lane.source == region->source && lane.audio == clip.audio
            && lane.reversed == region->reversed && lane.loopSource == region->loopSource
            && sameOrigin(lane, *region, clip.audio != nullptr);
        if (same) {
            lane.start = region->start; lane.end = region->end(); lane.offset = region->offset; lane.audioOffset = region->audioOffset;
            lane.gain = region->gain; lane.fadeIn = region->fadeIn; lane.fadeOut = region->fadeOut;
            lane.loopSource = region->loopSource; lane.reversed = region->reversed;
            lane.crossfade = clip.audio != nullptr ? region->crossfade : 0;
            if (tails[track].active) {
                if (lane.crossfade == 0 || static_cast<double>(region->start) != tails[track].start) endTail(track, false);
                else tails[track].end = static_cast<double>(region->start + lane.crossfade);
            }
            // Edited notes join the clip that is playing; a different loop length changes what the position means, so that restarts.
            if (lane.midi) {
                auto send = [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); };
                if (clip.pattern.loopSteps == rolls[track].loopLengthSteps()) rolls[track].updatePattern(clip.pattern, send);
                else { clearTrack(track, false); startTrackRegion(track, *region); }
            }
            return;
        }
        const bool replacesAudio = clip.audio != nullptr;
        // Reaching a crossfade: the clip that is ending plays on, on the tail player, while this one comes in.
        const bool crossing = replacesAudio && region->crossfade > 0 && lane.active && lane.audio != nullptr && !lane.audioWaiting
            && lane.end == region->start && lane.fadeOut == 0 && timelineTick < static_cast<double>(region->start + region->crossfade);
        endTail(track, false);
        if (crossing && audioPlayers[track].handOffActive(tailPlayers[track])) {
            auto& tail = tails[track];
            tail.active = true; tail.start = static_cast<double>(region->start);
            tail.end = static_cast<double>(region->start + region->crossfade); tail.gain = lane.gain;
            tail.audio = lane.audio; tail.origin = lane.start; tail.audioOffset = lane.audioOffset;
        }
        if (lane.active || audioPlayers[track].isPlaying()) {
            // A new audio region replaces the old one at once; anything else lets it fade.
            auto send = [this, track](int n, float v, bool on) { sendPattern(track, n, v, on); };
            rolls[track].stop(send);
            if (!replacesAudio) {
                audioPlayers[track].release(timelineFadeSamples);
                if (lane.audioWaiting) audioPlayers[track].cancelPending();
            }
        }
        const double boundary = lane.boundary;
        lane = {}; lane.boundary = boundary;
        audioClipMode[track] = false;
        startTrackRegion(track, *region);
    }
    // Whether `region` places its content where the lane already has it: the same origin, however much of it is shown.
    bool sameOrigin(const TimelineLane& lane, const alien::timeline::Region& region, bool audio) const noexcept {
        if (!audio) return region.start - region.offset == lane.start - lane.offset;
        const double ticksPerSecond = static_cast<double>(bpm.load()) * static_cast<double>(alien::timeline::ticksPerBeat) / 60.0;
        return std::abs(static_cast<double>(region.start - lane.start) - (region.audioOffset - lane.audioOffset) * ticksPerSecond) < 0.5;
    }
    double rackStepTick(std::int64_t n) const noexcept {
        const double swing = std::clamp(static_cast<double>(rackSnapshot->swing), 0.0, 1.0);
        return static_cast<double>(n) * alien::timeline::ticksPerStep + ((n & 1) != 0 ? swing * 0.5 * alien::timeline::ticksPerStep : 0.0);
    }
    void syncRackLane(int index) noexcept {
        auto& lane = rackLaneState[static_cast<std::size_t>(index)];
        const auto* region = regionNow(alien::timeline::rackRowBase + index, lane.boundary);
        if (region != nullptr && region->muted) region = nullptr;
        if (region == nullptr) { const double boundary = lane.boundary; lane = {}; lane.boundary = boundary; return; }
        if (lane.active && lane.source == region->source && region->start - region->offset == lane.start - lane.offset) {
            lane.start = region->start; lane.end = region->end(); lane.offset = region->offset;   // trimmed or stretched, not moved
            return;
        }
        const double boundary = lane.boundary;
        lane = {}; lane.boundary = boundary;
        lane.active = true; lane.source = region->source; lane.start = region->start; lane.end = region->end();
        lane.offset = region->offset;
        // The first step whose hit has not happened yet.
        const double content = std::max(0.0, timelineTick - static_cast<double>(region->start)) + static_cast<double>(region->offset);
        lane.nextStep = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(content / alien::timeline::ticksPerStep)) - 1);
        while (rackStepTick(lane.nextStep) < content - 1.0e-6) ++lane.nextStep;
    }
    void fireRackSteps(int sample) noexcept {
        for (auto& lane : rackLaneState) {
            if (!lane.active) continue;
            const double content = timelineTick - static_cast<double>(lane.start) + static_cast<double>(lane.offset);
            for (int guard = 0; guard < 4 && content >= rackStepTick(lane.nextStep) - 1.0e-9; ++guard) {
                const double hitTick = static_cast<double>(lane.start - lane.offset) + rackStepTick(lane.nextStep);
                if (hitTick >= static_cast<double>(lane.end)) break;    // swing pushed it past the end of the region
                const auto* pattern = rackSnapshot->pattern(lane.source);
                if (pattern != nullptr && rackHitCount < static_cast<int>(rackHits.size()))
                    rackHits[static_cast<std::size_t>(rackHitCount++)] = {sample, lane.source, static_cast<int>(lane.nextStep % pattern->length)};
                ++lane.nextStep;
            }
        }
    }
    // Puts the playhead at `tick` and makes every lane catch up there on the next
    // sample. `hard` (the user jumped) silences what was sounding; a loop wrap
    // lets release tails ring.
    void seekTimeline(double tick, bool hard) noexcept {
        setTimelineTick(std::max(0.0, tick));
        loopArmed = timelineTick < static_cast<double>(song->timeline.loopEnd);
        for (std::size_t t = 0; t < sessionTracks; ++t) { clearTrack(t, hard); trackLanes[t].boundary = -1.0; }   // look again at the new place
        for (auto& lane : rackLaneState) lane = {};
        if (hard) rackPlayer.stop();
    }
    // The timeline changed while the song plays: every lane looks again.
    void resyncTimeline() noexcept {
        loopArmed = timelineTick < static_cast<double>(song->timeline.loopEnd);
        for (auto& lane : trackLanes) lane.boundary = -1.0;
        for (auto& lane : rackLaneState) lane.boundary = -1.0;
    }
    // One sample of the song: wraps at the loop, brings every lane up to date when
    // the playhead reaches one of its boundaries, and advances. False at the end.
    bool timelineSample(int sample) noexcept {
        const auto& tl = song->timeline;
        const double now = timelineTick + tickEpsilon;
        if (tl.loopEnabled && loopArmed && now >= static_cast<double>(tl.loopEnd))
            seekTimeline(static_cast<double>(tl.loopStart) + std::max(0.0, timelineTick - static_cast<double>(tl.loopEnd)), false);
        if (timelineTick + tickEpsilon >= std::min(static_cast<double>(tl.length()), timelineStopAt)) return false;
        automationTicks[static_cast<std::size_t>(sample)] = timelineTick;
        const double at = timelineTick + tickEpsilon;
        for (std::size_t t = 0; t < sessionTracks; ++t)
            if (at >= trackLanes[t].boundary) syncTrack(t);
        for (int l = 0; l < alien::timeline::rackLanes; ++l)
            if (at >= rackLaneState[static_cast<std::size_t>(l)].boundary) syncRackLane(l);
        for (std::size_t t = 0; t < sessionTracks; ++t) {
            auto& lane = trackLanes[t];
            if (lane.audioWaiting) launchWaiting(t);
            advanceTail(t, at);
            if (lane.nextAudio != nullptr && !lane.prefetched && at >= lane.prefetchAt) prefetchNext(t);
        }
        for (std::size_t t = 0; t < sessionTracks; ++t)
            if (trackLanes[t].midi) rolls[t].nextSample([this, t](int n, float v, bool on) { sendPattern(t, n, v, on); });
        fireRackSteps(sample);
        // The playhead is a base plus a whole number of samples at the current rate, not a running
        // sum, so it stays exact however long the song plays.
        timelineTick = timelineSegmentBase + static_cast<double>(++timelineSegmentSamples) * timelineRate;
        return true;
    }
    void setTimelineTick(double tick) noexcept {
        timelineTick = timelineSegmentBase = tick;
        timelineSegmentSamples = 0;
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
        if (recording.audioTake.load(std::memory_order_relaxed)
            && songTake->ended.load(std::memory_order_relaxed) == alien::SongTakeCapture::notEnded)
            songTake->ended.store(alien::SongTakeCapture::stopped, std::memory_order_relaxed);
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
        const bool audio = recording.audioTake.load();
        if (audio) {
            // A Song-view take: the song must be what plays, and at least one armed track must have a sink.
            if (patternMode.load() != 2 || !songTake->anySink()) {
                pushRecordEvent(alien::RecordEventKind::refused, 0, 0.0f);
                return;
            }
            songTake->resetReport();
            songTake->latencySamples.store(deviceInputLatencySamples.load() + deviceLatencySamples.load());
            songTakeLastTick = -1.0;
        } else if (!validRecordingTarget(track, scene) || patternMode.load() != 1) {
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
        } else if (phase != alien::RecordControls::idle && patternMode.load() != (recording.audioTake.load() ? 2 : 1)) {
            endRecording(); // the view changed to one that does not play what the take records over
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
    void sendMidiControl(int channel,int controller,int value) noexcept {
        const int midiChannel=channel+1;
        auto apply=[&](std::size_t track) {
            if(plugins->hasInstrument(track)) {
                if(controller==128) plugins->pitchWheel(track,midiChannel,value);
                else plugins->controller(track,midiChannel,controller,value);
            } else if(instrumentModes[track]==0) {
                if(controller==128) trackSynths[track].setPitchWheel(channel+9,value);
                else if(controller==1) trackSynths[track].setModulation(channel+9,value);
            }
        };
        if(monitorTrack>=0) {apply(static_cast<std::size_t>(monitorTrack));return;}
        if(currentLiveDrumTrack>=0) {apply(static_cast<std::size_t>(currentLiveDrumTrack));return;}
        if(controller==128) synth.setPitchWheel(channel+9,value);
        else if(controller==1) synth.setModulation(channel+9,value);
    }
    void resetMidiControllers() noexcept {
        for(int channel=0;channel<16;++channel) {
            synth.setPitchWheel(channel+9,8192); synth.setModulation(channel+9,0);
            for(std::size_t track=0;track<sessionTracks;++track) {
                trackSynths[track].setPitchWheel(channel+9,8192);
                trackSynths[track].setModulation(channel+9,0);
                if(plugins->hasInstrument(track)) {
                    plugins->pitchWheel(track,channel+1,8192);
                    plugins->controller(track,channel+1,1,0);
                }
            }
        }
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
        setTimelineTick(timelineStart); loopArmed = false; timelinePosition.store(-1);
        for (auto& lane : trackLanes) lane = {};
        for (auto& lane : rackLaneState) lane = {};
        for (auto& player : tailPlayers) player.stop();
        for (auto& tail : tails) tail = {};
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
    static constexpr double tickEpsilon = 1.0e-7;   // slack for comparing the playhead with whole-tick boundaries
    double timelineStopAt = std::numeric_limits<double>::max();
    double timelineTick{}, timelineStart{}, transportBeat{};   // the playhead, and where the next play begins (ticks)
    double timelineSegmentBase{}, timelineRate{};              // playhead = base + samples * rate since the last jump or tempo change
    std::int64_t timelineSegmentSamples{};
    bool loopArmed{};   // the playhead is before the loop's end, so reaching it wraps (starting past it plays on)
    std::array<TimelineLane, sessionTracks> trackLanes{};
    std::array<TimelineLane, alien::timeline::rackLanes> rackLaneState{};
    std::array<alien::rack::RackPlayer::TimelineHit, 128> rackHits{};
    int rackHitCount{};
    std::array<juce::AudioBuffer<float>, sessionTracks> clipBuffers;
    alien::SynthEngine synth; // Live keyboard and legacy step sequencer.
    FixedHeapArray<alien::SynthEngine, sessionTracks> trackSynths;
    FixedHeapArray<alien::TrackMixer, sessionTracks> trackMixers;
    float masterRamp{1.0f}, masterRampCoefficient{1.0f};
    FixedHeapArray<alien::DrumSampler, sessionTracks> drumSamplers;
    std::array<int, sessionTracks> instrumentModes{};
    int currentLiveDrumTrack{-1};
    alien::MetronomeClick click;
    std::array<float, alien::PluginRuntime::blockSize> clickBuffer{};
    std::array<double, alien::PluginRuntime::blockSize> automationTicks{};
    std::int64_t lastClickBeat{-1};
    double countInBeats{}, countInTotalBeats{};
    double songTakeLastTick{-1.0};   // song position of the last sample a Song-view take looked at
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
    FixedHeapArray<alien::SessionAudioPlayer, sessionTracks> tailPlayers;   // a crossfade's outgoing clip
    FixedHeapArray<CrossfadeTail, sessionTracks> tails;   // on the heap: tests keep whole engines on the stack
    const float* const* deviceInputs{};   // this callback's enabled input channels, in order (null offline)
    int deviceInputCount{};
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

