#pragma once
// The playback workloads of the capacity report (T1-04 D22-D24), shared by the offline report in
// RackIntegrationTests and the real-device timing tool (AlienDeviceTiming, T1-04 D31), so both time
// exactly the same work. Everything here runs on the control thread, before or between callbacks.
#include "StockEffects.h"
#include "ToneEngine.h"
#include "RackSounds.h"
#include "ViewModes.h"
#include <memory>
#include <vector>

namespace alien::capacity {

// kind: 0 transport only, 1 Session rack occurrences, 3 Song rack lanes, 4 Session MIDI tracks,
// 5 MIDI with sends to the four returns, 6 Session audio clips, 7 MIDI through the stock EQ and
// compressor, 8 Session drum tracks, 9 Session audio clips streamed from disk (T1-04 D32).
// `occurrences` is how many tracks play (or rack occurrences).
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
        {"16 Session audio clips, streamed from disk", 9, 16, 16, true}};
    if (full > 16) {
        all.push_back({"Transport, no clips", 0, full, 0});
        all.push_back({"All Session rack occurrences", 1, full, full});
        all.push_back({"All Session MIDI tracks", 4, full, full});
        all.push_back({"1 Session MIDI track playing", 4, full, 1});
        all.push_back({"All Session audio clips", 6, full, full});
        all.push_back({"All Session MIDI tracks, stock EQ + compressor", 7, full, full});
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
    ToneEngine engine;
    rack::Project project = rack::Project::createDefault();
    std::vector<std::shared_ptr<const SessionAudioClip>> keepAlive;

    explicit Rig(double rate = 48000.0) {
        engine.prepare(rate);
        engine.bpm.store(120);          // one sixteenth = 6000 samples at 48 kHz
        for (auto& step : engine.steps) step.store(false);
        publish();
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

inline bool playsMidi(const Case& c) { return c.kind == 4 || c.kind == 5 || c.kind == 7 || c.kind == 8; }
inline bool playsAudio(const Case& c) { return c.kind == 6 || c.kind == 9; }

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
    return nullptr;
}

// Whether everything the row plays has started: the device tool waits for this before it records,
// because a streamed clip starts only once its first audio has been read (D32).
inline bool started(const Rig& rig, const Case& c) {
    const auto& engine = rig.engine;
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
    if (engine.plugins->faultCount.load() != 0) return "capacity plugins run without faults";
    return nullptr;
}

} // namespace alien::capacity
