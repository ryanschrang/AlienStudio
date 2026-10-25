#pragma once
// Audio-thread half of the Channel Rack: a sample-accurate step clock with
// swing, and a shared pool of sample/synth voices with pitch, pan, velocity, probability,
// gate, nudge, ratchets, choke groups, mute and solo. Real-time safe: no allocation, locks, or I/O,
// and nothing here touches the message thread's data except the immutable
// Snapshot and the immutable decoded samples it is given.
#include "RackModel.h"
#include "SessionAudioClip.h"
#include "SessionCapacity.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace alien::rack {

// Counts sixteenth-note grid lines, one call per sample. Every second line
// (odd grid index) is delayed by `swing` x half a step, so 0 is straight and 1
// puts the off-beat three quarters of the way to the next beat. Intervals are
// fractional, so tempo is exact over any length of time.
class RackClock {
public:
    void reset() noexcept { untilGrid = 0.0; grid = 0; delayCount = 0; delayedGrid = 0; }
    // Returns the absolute grid index (0, 1, 2, ...) of the step that starts on
    // this sample, or -1. The first call after reset() starts step 0.
    std::int64_t next(double sampleRate, double bpm, double swing) noexcept {
        std::int64_t fire = -1;
        if (delayCount > 0 && --delayCount == 0) fire = delayedGrid;
        if (untilGrid <= 0.0) {
            // A tempo that is not a number (std::clamp passes NaN straight through) would make
            // the interval NaN and freeze the clock for good, so it plays at 120 instead.
            const double tempo = std::isfinite(bpm) ? std::clamp(bpm, 1.0, 1000.0) : 120.0;
            const double interval = sampleRate * 60.0 / (tempo * 4.0);
            const std::int64_t index = grid++;
            const std::int64_t delay = (index & 1) != 0 && std::isfinite(swing)
                ? static_cast<std::int64_t>(std::llround(std::clamp(swing, 0.0, 1.0) * 0.5 * interval)) : 0;
            if (delay <= 0) fire = index;
            else { delayCount = delay; delayedGrid = index; }
            untilGrid += interval;
        }
        untilGrid -= 1.0;
        return fire;
    }
private:
    double untilGrid{};
    std::int64_t grid{}, delayCount{}, delayedGrid{};
};

class RackPlayer {
public:
    static constexpr int voiceCount = 64;
    // Hits waiting for their nudge or ratchet time: at most every channel's ratchets of two
    // overlapping steps (a step nudged a whole step late meets the next one's hits).
    static constexpr int maxPending = maxChannels * (maxRatchet * 2 + static_cast<int>(PianoRollPattern::maxNotes));
    static constexpr int masterBus = maxSessionTracks;  // buses 0..maxSessionTracks-1 are Session tracks
    static constexpr int busCount = maxSessionTracks + 1;
    using SampleTable = std::array<std::atomic<const SessionAudioClip*>, maxChannels>;

    void prepare(double rate) noexcept {
        sampleRate = std::isfinite(rate) && rate > 0.0 ? rate : 44100.0;
        smoothing = 1.0f - static_cast<float>(std::exp(-static_cast<double>(controlInterval) / (0.005 * sampleRate)));
        releaseCoefficient = static_cast<float>(std::exp(-1.0 / (0.004 * sampleRate)));
        for (auto& v : voices) v = Voice{};
        for (auto& synth : *synthStorage) synth.prepare(sampleRate);
        gain.fill(1.0f); pan.fill(0.0f); wasAudible.fill(true);
        clock.reset(); wasRunning = false; lastStep = -1; controlCountdown = 0; pendingCount = 0; now = 0;
    }
    // Fades every sound out over a few milliseconds, drops hits not yet played and rewinds the clock.
    void stop() noexcept { releaseAll(); clock.reset(); wasRunning = false; lastStep = -1; pendingCount = 0; }

    // Plays one channel now, as when its name is clicked. Mute, solo and
    // probability do not apply: a preview always sounds.
    void audition(const Snapshot& snap, int channel, float velocity, const SessionAudioClip* clip, int pitch = 0, bool held = false) noexcept {
        if (channel < 0 || channel >= snap.channelCount || !std::isfinite(velocity)) return;
        const auto& settings = snap.channels[static_cast<std::size_t>(channel)];
        const int note = 60 + std::clamp(pitch, -60, 67);
        if (velocity <= 0) {
            for (auto& v : voices) if (v.active && v.preview && v.channel == channel && v.synthPreset >= 0 && v.note == note)
                synthFor(v).noteOff(v.note);
            return;
        }
        if (clip == nullptr && !settings.isSynth()) return;
        auto& voice = start(settings, channel, clip, std::clamp(velocity, 0.0f, 1.0f), 0.0f, note - 60, true);
        // Clicking a channel name must not leave a sustaining instrument held.
        if (voice.synthPreset >= 0 && !held) voice.gateLeft = static_cast<std::int64_t>(sampleRate * 0.5) + 1;
    }

    // Renders `samples` samples and ADDS them to the per-bus output pointers.
    // While `running`, the selected pattern loops on the built-in step clock.
    void process(const Snapshot& snap, const SampleTable& clips, bool running, double bpm, int activeTracks,
                 int samples, float* const* left, float* const* right) noexcept {
        stepSamples = stepLength(bpm);
        render(snap, clips, running, activeTracks, samples, left, right, [&](int) {
            const auto grid = clock.next(sampleRate, bpm, snap.swing);
            if (grid >= 0) trigger(snap, clips, grid);
        });
    }

    // One step of one pattern, due on a given sample of the block. The arrangement
    // timeline decides when each step is due (it knows where every pattern region
    // begins, ends and loops), so no clock runs here.
    struct TimelineHit { int sample{}, pattern{}, step{}; std::int64_t remainingSamples{std::numeric_limits<std::int64_t>::max()}; std::int64_t elapsedSamples{}; };
    // `hits` must be in ascending order of sample. `bpm` sets how far nudges and ratchets reach.
    void processTimeline(const Snapshot& snap, const SampleTable& clips, bool running, const TimelineHit* hits, int hitCount,
                         double bpm, int activeTracks, int samples, float* const* left, float* const* right) noexcept {
        int next = 0;
        stepSamples = stepLength(bpm);
        render(snap, clips, running, activeTracks, samples, left, right, [&](int i) {
            for (; next < hitCount && hits[next].sample <= i; ++next) {
                const Pattern* p = snap.pattern(hits[next].pattern);
                if (p != nullptr) triggerStep(snap, clips, *p, std::clamp(hits[next].step, 0, p->length - 1), hits[next].pattern, hits[next].remainingSamples, hits[next].elapsedSamples);
            }
        });
    }

    // The step (within the pattern) that started most recently, or -1 when stopped.
    int currentStep() const noexcept { return lastStep; }
    int activeVoices() const noexcept {
        int n = 0; for (const auto& v : voices) n += v.active ? 1 : 0; return n;
    }
    bool busy() const noexcept { for (const auto& v : voices) if (v.active) return true; return false; }
    // Nudged or ratcheted hits that are due later.
    int pendingHits() const noexcept { return pendingCount; }

    static int busFor(int output, int activeTracks) noexcept {
        return output >= 0 && output < activeTracks && output < maxSessionTracks ? output : masterBus;
    }

private:
    // Shared block loop. `onSample(i)` runs before sample i while `running` and
    // starts whatever is due then.
    template <class OnSample>
    void render(const Snapshot& snap, const SampleTable& clips, bool running, int activeTracks, int samples,
                float* const* left, float* const* right, OnSample&& onSample) noexcept {
        tracksInUse = std::clamp(activeTracks, 1, maxSessionTracks);
        if (running && !wasRunning) { clock.reset(); random = seed; lastStep = -1; pendingCount = 0; }
        if (!running && wasRunning) { releaseAll(); lastStep = -1; pendingCount = 0; }
        wasRunning = running;
        syncChannels(snap);
        if (!running && !busy()) {
            // Nothing is sounding: skip the per-sample work, but leave every
            // fader and pan exactly where it should be for the next hit.
            for (int c = 0; c < snap.channelCount; ++c) {
                const auto i = static_cast<std::size_t>(c);
                gain[i] = snap.channels[i].volume / defaultChannelVolume; pan[i] = snap.channels[i].pan;
            }
            return;
        }
        for (int i = 0; i < samples; ++i) {
            if (controlCountdown-- <= 0) { controlCountdown = controlInterval - 1; updateControl(snap); }
            if (running) onSample(i);
            if (pendingCount > 0 && now >= nextDue) firePending(snap, clips);
            renderSample(i, left, right);
            ++now;
        }
    }

    // The length of one straight sixteenth, in samples (the clock's tempo rule).
    double stepLength(double bpm) const noexcept {
        const double tempo = std::isfinite(bpm) ? std::clamp(bpm, 1.0, 1000.0) : 120.0;
        return sampleRate * 60.0 / (tempo * 4.0);
    }

    static constexpr int controlInterval = 16;
    static constexpr std::uint32_t seed = 0x9E3779B9u;
    struct Voice {
        const SessionAudioClip* clip{};
        double position{}, rate{};
        float velocity{}, stepPan{}, gl{}, gr{}, dl{}, dr{}, release{1.0f};
        std::uint64_t age{};
        std::int64_t gateLeft{-1};   // samples until the gate releases the voice; -1: plays to the end
        int channel{-1}, bus{masterBus}, pattern{-1}, synthPreset{-1}, note{60};
        std::uint64_t sequence{};
        std::uint8_t choke{};
        bool active{false}, releasing{false}, preview{false};
    };

    SynthEngine& synthFor(const Voice& voice) noexcept { return (*synthStorage)[static_cast<std::size_t>(&voice - voices.data())]; }
    void releaseVoice(Voice& v) noexcept {
        if (!v.active) return;
        v.releasing = true;
        if (v.synthPreset >= 0) synthFor(v).noteOff(v.note);
    }
    void releaseAll() noexcept { for (auto& v : voices) releaseVoice(v); }

    // Voices of a channel that has just been muted (or removed) are released.
    void syncChannels(const Snapshot& snap) noexcept {
        // Visit the voice pool once per block, rather than once per channel.
        for (auto& voice : voices) {
            if (!voice.active) continue;
            if (voice.channel < 0 || voice.channel >= snap.channelCount) { releaseVoice(voice); continue; }
            const auto c = static_cast<std::size_t>(voice.channel);
            const bool audible = snap.audible(voice.channel);
            if ((!voice.preview && wasAudible[c] && !audible)
                || voice.synthPreset != snap.channels[c].instrumentPreset) releaseVoice(voice);
            if (voice.sequence != 0) {
                const auto* pattern = snap.pattern(voice.pattern);
                if (!pattern || pattern->noteRevisions[c] != voice.sequence) releaseVoice(voice);
            }
        }
        for (int channel = 0; channel < maxChannels; ++channel)
            wasAudible[static_cast<std::size_t>(channel)] = snap.audible(channel);
    }

    // Channel faders and pans glide, and every voice's gains follow, every 16 samples.
    void updateControl(const Snapshot& snap) noexcept {
        for (int c = 0; c < snap.channelCount; ++c) {
            const auto i = static_cast<std::size_t>(c);
            gain[i] += (snap.channels[i].volume / defaultChannelVolume - gain[i]) * smoothing;
            pan[i] += (snap.channels[i].pan - pan[i]) * smoothing;
        }
        for (auto& v : voices) {
            if (!v.active) continue;
            const auto c = static_cast<std::size_t>(std::clamp(v.channel, 0, maxChannels - 1));
            if (v.synthPreset >= 0) v.bus = busFor(snap.channels[c].output, tracksInUse);
            const float position = std::clamp(pan[c] + v.stepPan, -1.0f, 1.0f);
            const float angle = (position + 1.0f) * 0.78539816339744830962f; // 0 .. pi/2
            const float amplitude = v.velocity * gain[c];
            // Gains glide to their new values across the next 16 samples, so a
            // fader or pan move never steps.
            v.dl = (std::cos(angle) * amplitude - v.gl) * (1.0f / controlInterval);
            v.dr = (std::sin(angle) * amplitude - v.gr) * (1.0f / controlInterval);
        }
    }

    void trigger(const Snapshot& snap, const SampleTable& clips, std::int64_t grid) noexcept {
        const Pattern* p = snap.selected();
        if (p == nullptr) return;
        triggerStep(snap, clips, *p, static_cast<int>(grid % p->length), snap.selectedPattern);
    }

    // A hit waiting for its time. The sample and mute/solo are looked up again when it plays.
    struct Pending {
        std::int64_t due{}, gate{-1};
        float velocity{}, pan{};
        int channel{}, pitch{};
        int pattern{-1}; std::uint64_t sequence{};
    };
    static bool later(const Pending& a, const Pending& b) noexcept { return a.due > b.due; }

    void triggerStep(const Snapshot& snap, const SampleTable& clips, const Pattern& pattern, int step, int patternIndex,
                     std::int64_t remainingSamples = std::numeric_limits<std::int64_t>::max(), std::int64_t elapsedSamples = 0) noexcept {
        const Pattern* p = &pattern;
        lastStep = step;
        for (int c = 0; c < snap.channelCount; ++c) {
            const auto& sequence = p->notes[static_cast<std::size_t>(c)];
            if (sequence) {
                if (!snap.audible(c)) continue;
                const auto token = p->noteRevisions[static_cast<std::size_t>(c)];
                for (std::size_t n = 0; n < sequence->count; ++n) {
                    const auto& note = sequence->notes[n];
                    if (note.startStep < step || note.startStep >= step + 1) continue;
                    if (note.probability < 1.0f && static_cast<double>(nextRandom()) / 4294967296.0 >= note.probability) continue;
                    const auto delay = std::llround((note.startStep - step) * stepSamples) - elapsedSamples;
                    if (delay < 0) continue; // No chase: only note-ons still ahead of the seek/trim point.
                    if (delay >= remainingSamples) continue;
                    const auto gate = std::min(std::max<std::int64_t>(1, std::llround(note.lengthSteps * stepSamples)), remainingSamples - delay) + 1;
                    if (delay == 0) play(snap, clips, c, note.velocity, 0, note.pitch - 60, gate, patternIndex, token);
                    else schedule({now + delay, gate, note.velocity, 0, c, note.pitch - 60, patternIndex, token});
                }
                continue;
            }
            const Step& s = p->steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(step)];
            if (elapsedSamples > 0 || !s.on || !snap.audible(c)) continue;
            // The generator only advances for steps that can miss, so a pattern of
            // certain hits plays identically however the probabilities are edited elsewhere.
            // One draw decides the whole step, ratchets included.
            if (s.probability < 100 && nextRandom() % 100u >= s.probability) continue;
            if (clips[static_cast<std::size_t>(c)].load(std::memory_order_relaxed) == nullptr
                && !snap.channels[static_cast<std::size_t>(c)].isSynth()) continue;
            // Ratchets split the step evenly; the gate is a share of the time between them.
            const int hits = std::clamp(static_cast<int>(s.ratchet), 1, maxRatchet);
            const double spacing = stepSamples / hits;
            const std::int64_t gate = s.gate >= fullGate && !snap.channels[static_cast<std::size_t>(c)].isSynth() ? -1
                : std::max<std::int64_t>(1, std::llround(spacing * static_cast<double>(s.gate) / 100.0));
            const double offset = stepSamples * static_cast<double>(std::min<int>(s.nudge, maxNudge)) / 100.0;
            const float velocity = static_cast<float>(s.velocity) / 127.0f, pan = static_cast<float>(s.pan) / 63.0f;
            for (int k = 0; k < hits; ++k) {
                const auto delay = static_cast<std::int64_t>(std::llround(offset + spacing * k));
                if (delay <= 0) play(snap, clips, c, velocity, pan, s.pitch, gate);
                else schedule({now + delay, gate, velocity, pan, c, s.pitch});
            }
        }
    }

    // A fixed-capacity min heap bounds both memory and scheduling work. No allocation.
    void schedule(const Pending& hit) noexcept {
        if (pendingCount >= maxPending) return;
        pending[static_cast<std::size_t>(pendingCount++)] = hit;
        std::push_heap(pending.begin(), pending.begin() + pendingCount, later);
        nextDue = pending[0].due;
    }
    void firePending(const Snapshot& snap, const SampleTable& clips) noexcept {
        while (pendingCount > 0 && pending[0].due <= now) {
            std::pop_heap(pending.begin(), pending.begin() + pendingCount, later);
            const auto due = pending[static_cast<std::size_t>(--pendingCount)];
            bool current = true;
            if (due.sequence != 0) {
                const auto* p = snap.pattern(due.pattern);
                current = p && p->noteRevisions[static_cast<std::size_t>(due.channel)] == due.sequence;
            }
            if (current && snap.audible(due.channel))
                play(snap, clips, due.channel, due.velocity, due.pan, due.pitch, due.gate, due.pattern, due.sequence);
        }
        nextDue = pendingCount > 0 ? pending[0].due : std::numeric_limits<std::int64_t>::max();
    }
    void play(const Snapshot& snap, const SampleTable& clips, int c, float velocity, float stepPan, int pitch, std::int64_t gate, int pattern = -1, std::uint64_t sequence = 0) noexcept {
        const SessionAudioClip* clip = clips[static_cast<std::size_t>(c)].load(std::memory_order_relaxed);
        if (clip == nullptr && !snap.channels[static_cast<std::size_t>(c)].isSynth()) return;
        Voice& v = start(snap.channels[static_cast<std::size_t>(c)], c, clip, velocity, stepPan, pitch);
        v.gateLeft = v.synthPreset >= 0 && gate < 0 ? static_cast<std::int64_t>(stepSamples) + 1 : gate; v.pattern = pattern; v.sequence = sequence;
    }

    Voice& start(const Channel& channel, int index, const SessionAudioClip* clip, float velocity, float stepPan, int semitones,
                 bool preview = false) noexcept {
        if (channel.chokeGroup != 0)
            for (auto& v : voices) if (v.active && v.choke == channel.chokeGroup) releaseVoice(v);
        Voice* chosen = nullptr;
        for (auto& v : voices) if (!v.active) { chosen = &v; break; }
        if (chosen == nullptr) {  // steal the oldest
            chosen = &voices[0];
            for (auto& v : voices) if (v.age < chosen->age) chosen = &v;
        }
        const auto c = static_cast<std::size_t>(index);
        *chosen = Voice{};
        chosen->clip = clip;
        chosen->synthPreset = channel.instrumentPreset;
        chosen->note = std::clamp(60 + semitones, 0, 127);
        if (chosen->synthPreset >= 0) {
            auto& synth = synthFor(*chosen);
            synth.prepare(sampleRate);
            synth.setPatch(alien::presets[static_cast<std::size_t>(std::clamp(chosen->synthPreset, 0, alien::presetCount - 1))]);
            synth.noteOn(chosen->note, 1.0f); // Velocity is applied once, by the rack's stereo gains.
        } else chosen->rate = clip->sampleRate / sampleRate * (semitones == 0 ? 1.0 : std::exp2(semitones / 12.0));
        chosen->velocity = velocity; chosen->stepPan = stepPan;
        chosen->channel = index; chosen->choke = channel.chokeGroup;
        chosen->bus = busFor(channel.output, tracksInUse);
        chosen->age = ++ageCounter; chosen->active = true; chosen->preview = preview;
        const float position = std::clamp(pan[c] + stepPan, -1.0f, 1.0f);
        const float angle = (position + 1.0f) * 0.78539816339744830962f;
        chosen->gl = std::cos(angle) * velocity * gain[c];
        chosen->gr = std::sin(angle) * velocity * gain[c];
        return *chosen;
    }

    static float hermite(const std::vector<float>& x, std::size_t frames, std::size_t i, float f) noexcept {
        const float y0 = x[i];
        const float ym = i > 0 ? x[i - 1] : 0.0f;
        const float y1 = i + 1 < frames ? x[i + 1] : 0.0f;
        const float y2 = i + 2 < frames ? x[i + 2] : 0.0f;
        const float c1 = 0.5f * (y1 - ym);
        const float c2 = ym - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    void renderSample(int index, float* const* left, float* const* right) noexcept {
        for (auto& v : voices) {
            if (!v.active) continue;
            if (v.gateLeft > 0 && --v.gateLeft == 0) {
                if (v.synthPreset >= 0) synthFor(v).noteOff(v.note);
                else v.releasing = true;
            }
            float l = 0, r = 0;
            if (v.synthPreset >= 0) {
                auto& synth = synthFor(v);
                l = r = synth.nextSample();
                if (synth.activeVoices() == 0) { v.active = false; continue; }
            } else {
                const SessionAudioClip& clip = *v.clip;
                const auto i = static_cast<std::size_t>(v.position);
                if (i >= clip.frames) { v.active = false; continue; }
                const float f = static_cast<float>(v.position - static_cast<double>(i));
                l = f == 0.0f ? clip.left[i] : hermite(clip.left, clip.frames, i, f);
                r = clip.channels == 2 ? (f == 0.0f ? clip.right[i] : hermite(clip.right, clip.frames, i, f)) : l;
            }
            float fade = 1.0f;
            if (v.releasing) {
                v.release *= releaseCoefficient;
                fade = v.release;
                if (fade < 0.001f) { v.active = false; if (v.synthPreset >= 0) synthFor(v).allSoundOff(); continue; }
            }
            left[v.bus][index] += l * v.gl * fade;
            right[v.bus][index] += r * v.gr * fade;
            v.gl += v.dl; v.gr += v.dr;
            v.position += v.rate;
        }
    }

    std::uint32_t nextRandom() noexcept {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return random;
    }

    RackClock clock;
    // The voice pool and fixed-capacity hit queue live on the heap, allocated once here, so the
    // engine that holds this player stays small enough for the tests that keep one on the stack.
    std::unique_ptr<std::array<Voice, voiceCount>> voiceStorage = std::make_unique<std::array<Voice, voiceCount>>();
    std::unique_ptr<std::array<Pending, maxPending>> pendingStorage = std::make_unique<std::array<Pending, maxPending>>();
    // Independent one-note engines keep same-pitch overlaps and releases separate.
    // The shared rack voice pool still limits the total to 64 voices, not 64x16.
    std::unique_ptr<std::array<SynthEngine, voiceCount>> synthStorage = std::make_unique<std::array<SynthEngine, voiceCount>>();
    std::array<Voice, voiceCount>& voices = *voiceStorage;
    std::array<Pending, maxPending>& pending = *pendingStorage;
    int pendingCount{};
    std::int64_t now{}, nextDue{};   // samples rendered so far, and when the soonest pending hit is due
    double stepSamples{5512.5};
    std::array<float, maxChannels> gain = [] { std::array<float, maxChannels> g{}; g.fill(1.0f); return g; }();
    std::array<float, maxChannels> pan{};
    std::array<bool, maxChannels> wasAudible = [] { std::array<bool, maxChannels> a{}; a.fill(true); return a; }();
    double sampleRate{44100.0};
    float smoothing{0.1f}, releaseCoefficient{0.99f};
    std::uint64_t ageCounter{};
    std::uint32_t random{seed};
    int controlCountdown{};
    int tracksInUse{4};
    int lastStep{-1};
    bool wasRunning{false};
};

} // namespace alien::rack
