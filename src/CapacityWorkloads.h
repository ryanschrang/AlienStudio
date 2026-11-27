#pragma once
// The playback workloads of the capacity report (T1-04 D22-D24), shared by the offline report in
// RackIntegrationTests and the real-device timing tool (AlienDeviceTiming, T1-04 D31), so both time
// exactly the same work. Everything here runs on the control thread, before or between callbacks.
#include "DiskRecording.h"
#include "StockEffects.h"
#include "ToneEngine.h"
#include "RackSounds.h"
#include "ViewModes.h"
#include <functional>
#include <memory>
#include <vector>

namespace alien::capacity {

// kind: 0 transport only, 1 Session rack occurrences, 3 Song rack lanes, 4 Session MIDI tracks,
// 5 MIDI with sends to the four returns, 6 Session audio clips, 7 MIDI through the stock EQ and
// compressor, 8 Session drum tracks, 9 Session audio clips streamed from disk (T1-04 D32), 10 tracks
// monitoring a stereo input (T1-04 D33), 11 tracks recording a stereo input to a Song take (T1-04
// D34), 12 the same take written to disk by the app's recording writer (T1-04 D35), 13 MIDI through a
// VST3 effect on every playing track (T1-04 D36), 14 MIDI played by a VST3 instrument on every playing
// track (T1-04 D37). `occurrences` is how many tracks play (or rack
// occurrences, or tracks monitoring or recording).
// deviceOnly: the offline report leaves the row out. It plays blocks back to back, far faster than
// real time, which no disk stream keeps up with; on a device the callbacks come at playback's pace.
struct Case { const char* name; int kind, tracks, occurrences; bool deviceOnly = false; };

inline std::vector<Case> cases(int full) {
    std::vector<Case> all{{"Transport, no clips", 0, 1, 0}, {"Transport, no clips", 0, 16, 0}, {"1 Session rack occurrence", 1, 16, 1},
        {"16 Session rack occurrences", 1, 16, 16}, {"4 Song rack occurrences", 3, 16, 0}, {"16 Session MIDI tracks", 4, 16, 16},
        // T1-04 D22: loaded versus playing tracks, sends and returns, audio clips and plugins (measurement gates 2 and 3).
        {"1 Session MIDI track playing", 4, 16, 1}, {"16 Session MIDI tracks, sends to 4 returns", 5, 16, 16},
        {"16 Session audio clips", 6, 16, 16}, {"16 Session MIDI tracks, stock EQ + compressor", 7, 16, 16},
        {"16 Session drum tracks", 8, 16, 16},
        // T1-04 D32: every track plays its own decoder of one WAV file through the engine's disk-stream worker.
        {"16 Session audio clips, streamed from disk", 9, 16, 16, true},
        // T1-04 D33: every track plays the device's first two inputs through its own strip (monitor "always").
        {"16 tracks monitoring a stereo input", 10, 16, 16},
        // T1-04 D34: a Song-view audio take on every track, armed (so also monitored), each into its own queue.
        {"16 tracks recording a stereo input to a Song take", 11, 16, 16},
        // T1-04 D35: that take written to 16 WAV files as it records, one writer thread per track, as the app
        // records. Device only: a writer cannot keep up with blocks played faster than real time.
        {"16 tracks recording a stereo input to disk", 12, 16, 16, true},
        // T1-04 D36: a third-party plugin, one instance per playing track. Device only: the offline report's
        // build does not host VST3, and the tool is given the plugin to load (--plugin).
        {"16 Session MIDI tracks through a VST3 effect", 13, 16, 16, true},
        // T1-04 D37: the tracks' MIDI played by an outside instrument, one instance per track, in place of the
        // built-in synth. Device only, as for the effect (--instrument).
        {"16 Session MIDI tracks playing a VST3 instrument", 14, 16, 16, true}};
    if (full > 16) {
        all.push_back({"Transport, no clips", 0, full, 0});
        all.push_back({"All Session rack occurrences", 1, full, full});
        all.push_back({"All Session MIDI tracks", 4, full, full});
        all.push_back({"1 Session MIDI track playing", 4, full, 1});
        all.push_back({"All Session audio clips", 6, full, full});
        all.push_back({"All Session MIDI tracks, stock EQ + compressor", 7, full, full});
        // T1-04 D38: every other row at the compiled maximum too, so a larger build can be timed on a device
        // doing all the work its 16-track rows do (sends, drums, streaming, inputs, recording, VST3).
        all.push_back({"All Session MIDI tracks, sends to 4 returns", 5, full, full});
        all.push_back({"All Session drum tracks", 8, full, full});
        all.push_back({"All Session audio clips, streamed from disk", 9, full, full, true});
        all.push_back({"All tracks monitoring a stereo input", 10, full, full});
        all.push_back({"All tracks recording a stereo input to a Song take", 11, full, full});
        all.push_back({"All tracks recording a stereo input to disk", 12, full, full, true});
        all.push_back({"All Session MIDI tracks through a VST3 effect", 13, full, full, true});
        all.push_back({"All Session MIDI tracks playing a VST3 instrument", 14, full, full, true});
    }
    return all;
}

// The media every workload shares. It must outlive the engines that play it.
struct Media {
    SessionAudioClip audio, hit;
    PianoRollPattern beat, chord;
    // The streamed clip of kind 9, made by whoever runs that row (the device tool writes a WAV file for
    // it); empty otherwise. It must be long enough to play through warm-up and recording.
    std::shared_ptr<const SessionAudioClip> streamed;
    // The folder kind 12 records into (its takes, and their recovery journals in a "Recovery" folder
    // inside it, never the app's own); empty otherwise. Whoever runs the row creates and removes it.
    juce::File recordingFolder;
    // D36: makes one instance of the effect kind 13 puts on each playing track (the device tool loads a
    // VST3 for it), or nothing. Called on the control thread, before the row starts.
    std::function<std::unique_ptr<juce::AudioProcessor>()> makeEffect;
    juce::String effectName;
    // D37: makes one instance of the instrument kind 14 gives each playing track, or nothing. As makeEffect.
    std::function<std::unique_ptr<juce::AudioProcessor>()> makeInstrument;
    juce::String instrumentName;
    Media() {
        // One decoded clip shared by every audio row, long enough to play through warm-up and measurement at 1,024 frames.
        audio.sampleRate = 48000; audio.channels = 1; audio.frames = 8 * 48000;
        audio.left.assign(audio.frames, 0.1f); audio.buildWaveform();
        // D23: a quarter-second drum hit, so hits on four pads a beat apart overlap in the sampler's voices.
        hit.sampleRate = 48000; hit.channels = 1; hit.frames = 12000;
        hit.left.assign(hit.frames, 0.2f); hit.buildWaveform();
        beat.loopSteps = 16;
        for (int n = 0; n < 4; ++n) beat.add(drumBaseNote + 2 * n, 4 * n, 1);
        chord.loopSteps = 16;
        for (int pitch : {60, 64, 67}) chord.add(pitch, 0, 16);
    }
    bool valid() const { return audio.valid() && hit.valid(); }
};

// An engine with the default Channel Rack project published to it, as the capacity report uses.
struct Rig {
    // D34: the queues a Song take records into, one per armed track. Declared before the engine so they
    // outlive it: its take holds pointers to them.
    std::vector<std::unique_ptr<RecordingQueue>> takes;
    // D35: the takes kind 12 writes to disk, one writer thread each. Also before the engine.
    std::vector<std::shared_ptr<DiskRecording>> diskTakes;
    ToneEngine engine;
    rack::Project project = rack::Project::createDefault();
    std::vector<std::shared_ptr<const SessionAudioClip>> keepAlive;

    const double sampleRate;   // what the engine is prepared at (the device tool prepares it at the device's rate)

    explicit Rig(double rate = 48000.0) : sampleRate(rate) {
        engine.prepare(rate);
        engine.bpm.store(120);          // one sixteenth = 6000 samples at 48 kHz
        for (auto& step : engine.steps) step.store(false);
        publish();
    }
    // D34: empties the take queues, as the app's recording thread does when it writes them to disk (here
    // the audio is dropped). Call it between callbacks, never from one: it is the queues' only consumer.
    void drainTakes() {
        for (auto& take : takes)
            while (take->drain([](const float* const*, int, int) { return true; }, 1 << 16)) {}
    }
    // D35: stops the disk takes and finishes their files. Only once no callback can run (the engine is
    // detached from the device): the writer must not race the callback's last append. Returns what
    // failed, or nullptr when every take wrote all it recorded.
    const char* finishDiskTakes() {
        const char* failed = nullptr;
        for (auto& take : diskTakes) {
            const auto recorded = take->frames.load();
            const auto result = take->finish();
            if (failed == nullptr) {
                if (result.error.isNotEmpty() || result.interrupted) failed = "capacity disk takes finish without errors";
                else if (recorded == 0 || result.writtenFrames != recorded) failed = "capacity disk takes write every frame recorded";
            }
        }
        return failed;
    }
    // Message-thread side of the hand-off: clips first, then the snapshot.
    void publish() {
        for (int c = 0; c < rack::maxChannels; ++c) {
            const SessionAudioClip* clip = nullptr;
            if (c < project.channelCount) {
                const auto& info = project.info[static_cast<std::size_t>(c)];
                if (info.isBuiltin()) {
                    auto made = rack::builtinClip(info.builtinId());
                    clip = made.get();
                    keepAlive.push_back(std::move(made));
                }
            }
            engine.rackSamples[static_cast<std::size_t>(c)].store(clip);
        }
        engine.rackMailbox.publish(project.snapshot());
    }
};

inline bool playsMidi(const Case& c) { return c.kind == 4 || c.kind == 5 || c.kind == 7 || c.kind == 8 || c.kind == 13 || c.kind == 14; }
inline bool playsAudio(const Case& c) { return c.kind == 6 || c.kind == 9; }
// The row needs input audio: the report hands the callback a test signal, the device tool opens an input.
// A recording track is armed, so it monitors too (monitor "automatic").
inline bool monitorsInput(const Case& c) { return c.kind == 10 || c.kind == 11 || c.kind == 12; }
inline bool records(const Case& c) { return c.kind == 11 || c.kind == 12; }

// Sets the workload up on a prepared rig, short of starting the transport. Returns what failed, or nullptr.
inline const char* configure(Rig& rig, const Case& c, const Media& media) {
    auto& engine = rig.engine;
    engine.setActiveTracks(c.tracks);
    rig.project.clips[0] = 101;
    rig.project.setInstrumentPreset(0, 9);
    if (!rig.project.setChannelNotes(0, 0, media.chord)) return "capacity source notes";
    rig.publish(); engine.patternMode.store(sessionMode);
    const int instances = c.kind == 1 ? c.occurrences : 0;
    for (int t = 0; t < instances; ++t) engine.requestedTrackRackClip[static_cast<std::size_t>(t)].store(101);
    if (c.kind == 3) {
        auto song = std::make_shared<ArrangementPlayback>(); song->timeline.setLengthBars(64);
        for (int lane = 0; lane < timeline::rackLanes; ++lane) {
            timeline::Region r; r.row = timeline::rackRowBase + lane;
            r.source = 0; r.rackClip = 101; r.length = 64 * timeline::ticksPerBar;
            if (!song->timeline.place(r)) return "capacity Song placement";
        }
        if (!engine.arrangementMailbox.publish(song)) return "capacity Song publication";
        engine.patternMode.store(songMode);
    }
    const bool midi = playsMidi(c);
    if (c.kind == 8) for (int t = 0; t < c.occurrences; ++t) {
        engine.tracks[static_cast<std::size_t>(t)].instrument.store(1);
        for (int pad = 0; pad < drumPadCount; ++pad)
            engine.drumKits[static_cast<std::size_t>(t)].samples[static_cast<std::size_t>(pad)].store(&media.hit);
    }
    // D34: a Song-view audio take with no count-in, as the Record button makes with the song playing;
    // begin() asks for it. The song is empty and 512 bars long (17 min at 120 BPM), longer than any run.
    // D35: the same take, each track's sink the app's DiskRecording, writing a WAV file as it records.
    if (c.kind == 12 && !media.recordingFolder.isDirectory()) return "capacity recording folder";
    if (records(c)) {
        auto song = std::make_shared<ArrangementPlayback>(); song->timeline.setLengthBars(512);
        if (!engine.arrangementMailbox.publish(song)) return "capacity Song publication";
        engine.patternMode.store(songMode);
        for (int t = 0; t < c.occurrences; ++t) {
            auto& track = engine.tracks[static_cast<std::size_t>(t)];
            track.input.store(0); track.stereoInput.store(true); track.armed.store(true);
            track.monitor.store(static_cast<int>(InputMonitor::automatic));
            AudioCaptureSink* sink = nullptr;
            if (c.kind == 11) {
                rig.takes.push_back(std::make_unique<RecordingQueue>(rig.sampleRate, 2));
                sink = rig.takes.back().get();
            } else {
                juce::String error;
                auto take = DiskRecording::start(media.recordingFolder.getNonexistentChildFile("Track " + juce::String(t + 1), ".wav", false),
                                                 rig.sampleRate, 2, error, media.recordingFolder.getChildFile("Recovery"));
                if (!take) return "capacity disk take started";
                rig.diskTakes.push_back(take);
                sink = take.get();
            }
            engine.songTake->sinks[static_cast<std::size_t>(t)].store(sink);
        }
        RecordSettings settings; settings.countInBars = 0;
        engine.recording.apply(settings);
        engine.recording.take.store(1);
        engine.recording.audioTake.store(true);
    }
    // D33: monitoring "always", so the input plays whether the track is armed or not; inputs 1 and 2.
    if (c.kind == 10) for (int t = 0; t < c.occurrences; ++t) {
        auto& track = engine.tracks[static_cast<std::size_t>(t)];
        track.input.store(0); track.stereoInput.store(true);
        track.monitor.store(static_cast<int>(InputMonitor::always));
    }
    if (c.kind == 9) {
        if (!media.streamed || !media.streamed->streamed()) return "capacity streamed clip";
        // As the controller does: the engine's stream service holds the source before any clip names it.
        if (engine.retainDiskAudioSource(media.streamed->streamSource) != media.streamed->streamSource.get()) return "capacity stream source retained";
    }
    if (midi || playsAudio(c)) for (int t = 0; t < c.occurrences; ++t) {
        SessionClipLaunch launch; launch.clipIndex = 0;
        if (midi) launch.pattern = c.kind == 8 ? media.beat : media.chord;
        else launch.audio = c.kind == 9 ? media.streamed.get() : &media.audio;
        if (!engine.pianoRollLaunches[static_cast<std::size_t>(t)].publish(launch)) return "capacity Session launch";
    }
    if (c.kind == 5) for (int t = 0; t < c.occurrences; ++t)
        for (int r = 0; r < 4; ++r) engine.tracks[static_cast<std::size_t>(t)].send[static_cast<std::size_t>(r)].store(0.5f);
    if (c.kind == 7) {
        auto& plugins = *engine.plugins;
        for (int t = 0; t < c.occurrences; ++t) {
            auto& rack = plugins.racks[static_cast<std::size_t>(t)];
            for (std::size_t i = 0; i < 2; ++i) {
                auto& slot = rack.effects[i];
                slot.processor = stock::create(stock::catalog[i].id);
                slot.name = stock::catalog[i].name;
                plugins.prepareSlot(slot, t, static_cast<int>(i));
                if (!slot.prepared) return "capacity stock effect prepared";
            }
            rack.effectCount = 2;
        }
        plugins.reserveAlignment();
    }
    // D36: as kind 7, but one instance of an outside plugin in each playing track's first FX slot.
    if (c.kind == 13) {
        if (!media.makeEffect) return "capacity VST3 effect";
        auto& plugins = *engine.plugins;
        for (int t = 0; t < c.occurrences; ++t) {
            auto& slot = plugins.racks[static_cast<std::size_t>(t)].effects[0];
            slot.processor = media.makeEffect();
            if (!slot.processor) return "capacity VST3 effect instance";
            slot.name = media.effectName;
            plugins.prepareSlot(slot, t, 0);
            if (!slot.prepared) return "capacity VST3 effect prepared";
            plugins.racks[static_cast<std::size_t>(t)].effectCount = 1;
        }
        plugins.reserveAlignment();
    }
    // D37: each playing track's instrument slot, assigned as a project's plugin instrument is: the track's
    // notes go to the plugin (on MIDI channel 2, as a clip's do) instead of the built-in synth.
    if (c.kind == 14) {
        if (!media.makeInstrument) return "capacity VST3 instrument";
        auto& plugins = *engine.plugins;
        for (int t = 0; t < c.occurrences; ++t) {
            auto& rack = plugins.racks[static_cast<std::size_t>(t)];
            rack.instrument.processor = media.makeInstrument();
            if (!rack.instrument.processor) return "capacity VST3 instrument instance";
            rack.instrument.name = media.instrumentName;
            plugins.prepareSlot(rack.instrument, t, -1);
            if (!rack.instrument.prepared) return "capacity VST3 instrument prepared";
            rack.instrumentAssigned = true;
        }
        plugins.reserveAlignment();
    }
    return nullptr;
}

// Starts the row: the transport, and for the recording row the take. Call it once the engine is attached
// to whatever will play it, not before: a device's start prepares the engine again, and preparing refuses
// a take that was asked for but had not begun, as a device change does in the app (D34).
inline void begin(Rig& rig, const Case& c) {
    auto& engine = rig.engine;
    if (records(c)) {
        engine.recording.stopRequested.store(false);
        engine.recording.startRequested.store(true);
    }
    engine.playing.store(true);
}

// Whether everything the row plays has started: the device tool waits for this before it records,
// because a streamed clip starts only once its first audio has been read (D32).
inline bool started(const Rig& rig, const Case& c) {
    const auto& engine = rig.engine;
    if (records(c) && engine.recording.phase.load() != RecordControls::recording) return false;
    for (int t = 0; t < c.occurrences; ++t) {
        const auto track = static_cast<std::size_t>(t);
        if (c.kind == 1 && engine.activeTrackRackClip[track].load() != 101) return false;
        if ((playsMidi(c) || playsAudio(c)) && engine.activePianoRollClip[track].load() != 0) return false;
    }
    return true;
}

// After the measured blocks: everything the row names is still playing and no plugin failed (D22).
// Returns what failed, or nullptr.
inline const char* stillPlaying(const Rig& rig, const Case& c) {
    const auto& engine = rig.engine;
    if (c.kind == 1)
        for (int t = 0; t < c.occurrences; ++t)
            if (engine.activeTrackRackClip[static_cast<std::size_t>(t)].load() != 101) return "capacity occurrences remain active";
    // D32: a stream that ran dry played silence where the clip has audio, so the row did not play what it
    // names. Checked before the clips are, so that a stream that could not be read is named as the cause.
    if (c.kind == 9)
        for (int t = 0; t < c.occurrences; ++t) {
            const auto track = static_cast<std::size_t>(t);
            if (engine.sessionStreamFailures(track) != 0) return "capacity streams are read without failures";
            if (engine.sessionStreamUnderruns(track) != 0) return "capacity streams never run dry";
        }
    if (playsMidi(c) || playsAudio(c))
        for (int t = 0; t < c.occurrences; ++t)
            if (engine.activePianoRollClip[static_cast<std::size_t>(t)].load() != 0) return "capacity Session clips remain active";
    // D34: every armed track's queue took input, none overran (its take would be cut short), and the take
    // is still recording.
    if (c.kind == 11) {
        for (const auto& take : rig.takes) {
            if (take->overrun.load()) return "capacity takes never overrun their queues";
            if (take->frames.load() == 0) return "capacity takes record every armed track";
        }
    }
    // D35: every disk take is taking input and its writer is keeping up (a full queue ends the take).
    if (c.kind == 12)
        for (const auto& take : rig.diskTakes) {
            if (take->done.load()) return "capacity disk takes never fall behind";
            if (take->frames.load() == 0 || take->writtenFrames.load() == 0) return "capacity disk takes record and write every armed track";
        }
    if (records(c) && (engine.recording.phase.load() != RecordControls::recording
                       || engine.songTake->ended.load() != SongTakeCapture::notEnded)) return "capacity take is still recording";
    // D33: each monitoring track metered input, so the input reached every strip.
    if (monitorsInput(c))
        for (int t = 0; t < c.occurrences; ++t)
            if (!(engine.tracks[static_cast<std::size_t>(t)].inputPeak.load() > 0.0f)) return "capacity input reaches every monitoring track";
    if (engine.plugins->faultCount.load() != 0) return "capacity plugins run without faults";
    return nullptr;
}

} // namespace alien::capacity
