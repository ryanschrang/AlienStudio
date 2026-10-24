#include "ProjectController.h"
#include "SongRecording.h"
#include "ViewModes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

// Song-view audio takes (REC-02): where a take lands on the timeline, what the engine captures (count-in,
// punch range, loop wrap, stop) through the real device callback, and the controller turning a take into
// clips in one undo step. No audio device or window is needed: input is a ramp fed to the callback.

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

namespace {

constexpr double rate = 48000.0;
constexpr double perSample = 16.0 * 120.0 / rate;   // ticks per sample at 120 BPM: 0.04
constexpr long long samplesPerBar = 96000;           // 3840 ticks

bool closeTo(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
// The input every test feeds: sample n of the device's channel c is the ramp value ramp(n, c).
float ramp(long long n, int c) { return static_cast<float>((n % 400000) * 2.0e-6) * (c == 0 ? 1.0f : -1.0f); }

// --- Placement --------------------------------------------------------------------------------------

void placementTests() {
    using alien::placeTake;
    auto p = placeTake(0.0, 48000, rate, 120, 0.0);
    require(p.valid && p.start == 0 && p.length == 1920 && p.audioOffset == 0.0, "a take from the song's start lasts as long as it was recorded");
    p = placeTake(3840.0, 48000, rate, 120, 480.0);   // 10 ms of latency = 19.2 ticks
    require(p.valid && p.start == 3821 && p.length == 1920 - 1, "latency moves the take earlier, onto the next whole tick");
    require(closeTo(p.audioOffset, 0.2 / perSample / rate, 1.0e-9), "and skips the part of the audio before that tick");
    p = placeTake(0.0, 48000, rate, 120, 480.0);
    require(p.valid && p.start == 0 && p.length == 1900 && closeTo(p.audioOffset, 0.01, 1.0e-9),
            "a take cannot start before the song: the early audio is skipped instead");
    p = placeTake(1920.0, 96000, rate, 120, 480.0, 1920, 3840);
    require(p.valid && p.start == 1920 && closeTo(p.audioOffset, 0.01, 1.0e-9), "with punch the take starts at the punch-in");
    require(p.length == 1920, "and ends at the punch-out");
    p = placeTake(1920.0, 48000, rate, 120, -480.0);   // a negative offset moves the take later
    require(p.valid && p.start == 1940 && closeTo(p.audioOffset, 0.8 / perSample / rate, 1.0e-9), "a negative offset moves the take later");
    require(!placeTake(-1.0, 4800, rate, 120, 0).valid, "nothing captured: no placement");
    require(!placeTake(0.0, 0, rate, 120, 0).valid && !placeTake(0.0, 10, rate, 120, 0).valid, "no frames, or less than a tick: no placement");
    require(!placeTake(0.0, 4800, 0.0, 120, 0).valid && !placeTake(0.0, 4800, rate, 0, 0).valid, "an invalid rate or tempo: no placement");
    std::cout << "PASS: take placement\n";
}

// --- The engine -----------------------------------------------------------------------------------

struct Rig {
    std::unique_ptr<ToneEngine> storage{std::make_unique<ToneEngine>()};
    ToneEngine& engine{*storage};
    std::array<float, 1024> left{}, right{}, in0{}, in1{};
    long long total = 0;   // samples rendered
    Rig(bool loop = false, std::int64_t loopStart = 0, std::int64_t loopEnd = 3840) {
        engine.prepare(rate);
        engine.bpm.store(120);
        auto song = std::make_shared<alien::ArrangementPlayback>();
        song->timeline.loopEnabled = loop; song->timeline.loopStart = loopStart; song->timeline.loopEnd = loopEnd;
        require(engine.arrangementMailbox.publish(std::shared_ptr<const alien::ArrangementPlayback>(song)), "fixture: the song");
        engine.patternMode.store(alien::songMode);
        render(256);   // picks up the song
    }
    void render(long long samples, int chunk = 256) {
        while (samples > 0) {
            const int count = static_cast<int>(std::min<long long>(samples, chunk));
            for (int i = 0; i < count; ++i) { in0[static_cast<std::size_t>(i)] = ramp(total + i, 0); in1[static_cast<std::size_t>(i)] = ramp(total + i, 1); }
            const float* inputs[]{in0.data(), in1.data()};
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2, count, {});
            samples -= count; total += count;
        }
    }
    void start(alien::AudioCaptureSink& sink, int track = 0, int countIn = 0, bool punch = false,
               std::int64_t in = 0, std::int64_t out = 0, std::uint32_t take = 1) {
        alien::RecordSettings settings; settings.countInBars = countIn;
        engine.recording.apply(settings);
        engine.tracks[static_cast<std::size_t>(track)].armed.store(true);
        engine.songTake->sinks[static_cast<std::size_t>(track)].store(&sink);
        engine.songTake->punch.store(punch);
        engine.songTake->punchIn.store(in); engine.songTake->punchOut.store(out);
        engine.recording.take.store(take);
        engine.recording.audioTake.store(true);
        engine.recording.stopRequested.store(false);
        engine.recording.startRequested.store(true);
    }
    // The take's end (or refusal) event, if one arrived.
    int ended(alien::RecordEventKind* kind = nullptr) {
        alien::RecordEvent e;
        bool any = false;
        while (engine.recording.events.pop(e)) { any = true; if (kind) *kind = e.kind; }
        return any ? engine.songTake->ended.load() : -1;
    }
    void detach() {
        for (auto& sink : engine.songTake->sinks) sink.store(nullptr);
        engine.recording.audioTake.store(false);
    }
    int phase() const { return engine.recording.phase.load(); }
};

// The sink holds exactly the input from device sample `from` on.
bool holdsInputFrom(alien::AudioCapture& sink, long long from, int channels) {
    auto clip = sink.finish();
    if (!clip) return false;
    for (std::size_t i = 0; i < clip->frames; ++i) {
        if (clip->left[i] != ramp(from + static_cast<long long>(i), 0)) return false;
        if (channels == 2 && clip->right[i] != ramp(from + static_cast<long long>(i), 1)) return false;
    }
    return true;
}

void captureTests() {
    {
        Rig rig;   // no count-in: the song starts at once and every sample is recorded
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        const auto from = rig.total;
        rig.render(4800, 193);
        require(rig.engine.playing.load() && rig.phase() == alien::RecordControls::recording, "the take starts the song");
        require(sink.frames.load() == 4800 && rig.engine.songTake->frames.load() == 4800, "every played sample is captured");
        require(rig.engine.songTake->firstTick.load() == 0.0, "from the song's start");
        rig.engine.recording.stopRequested.store(true);
        rig.render(256);
        require(rig.ended() == alien::SongTakeCapture::stopped && rig.phase() == alien::RecordControls::idle, "stopping ends the take");
        require(sink.frames.load() == 4800, "nothing is captured after the stop");
        rig.detach();
        require(holdsInputFrom(sink, from, 1), "the take is the track's input, sample for sample");
    }
    {
        Rig rig;   // a one-bar count-in: recording starts on the sample where the song starts
        alien::AudioCapture sink(rate, 2, 5);
        rig.engine.tracks[0].stereoInput.store(true);
        rig.start(sink, 0, 1);
        const auto from = rig.total + samplesPerBar;
        rig.render(samplesPerBar - 100, 333);
        require(rig.phase() == alien::RecordControls::countIn && sink.frames.load() == 0 && !rig.engine.playing.load(),
                "nothing is recorded during the count-in");
        rig.render(100 + 4800, 333);
        require(sink.frames.load() == 4800 && rig.engine.songTake->firstTick.load() == 0.0, "recording begins with the song");
        rig.engine.playing.store(false);   // Stop on the transport
        rig.render(256);
        require(rig.ended() == alien::SongTakeCapture::stopped, "stopping the song ends the take");
        rig.detach();
        require(holdsInputFrom(sink, from, 2), "a stereo take holds both channels of its pair");
    }
    {
        Rig rig;   // punch: only the range between the punch-in and the punch-out is recorded
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink, 0, 0, true, 1920, 3840);
        const auto from = rig.total + samplesPerBar / 2;
        rig.render(samplesPerBar / 2 - 10, 256);
        require(sink.frames.load() == 0 && rig.engine.songTake->firstTick.load() < 0.0, "the pre-roll is not recorded");
        rig.render(samplesPerBar, 256);
        require(sink.frames.load() == static_cast<std::uint64_t>(samplesPerBar / 2), "the punch range is recorded");
        require(closeTo(rig.engine.songTake->firstTick.load(), 1920.0, 1.0e-6), "from the punch-in");
        require(rig.ended() == alien::SongTakeCapture::punchedOut && rig.phase() == alien::RecordControls::idle, "the take ends at the punch-out");
        require(rig.engine.playing.load(), "and the song plays on");
        rig.detach();
        require(holdsInputFrom(sink, from, 1), "the punched take is the input of that range");
    }
    {
        Rig rig(true);   // a loop without punch: the first pass is the take
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        rig.render(samplesPerBar + 5000, 256);
        require(sink.frames.load() == static_cast<std::uint64_t>(samplesPerBar), "one pass of the loop is recorded");
        require(rig.ended() == alien::SongTakeCapture::loopWrapped, "the take ends where the loop wraps");
        rig.detach();
    }
    {
        Rig rig;   // a jump of the playhead ends the take, so it stays one piece
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        rig.render(4800);
        rig.engine.timelineSeek.store(3840 * 4);
        rig.render(1024);
        require(sink.frames.load() == 4800 && rig.ended() == alien::SongTakeCapture::jumped, "a seek ends the take");
        rig.detach();
    }
    {
        Rig rig;   // already playing: recording starts where the song is, with no count-in
        alien::AudioCapture sink(rate, 1, 5);
        rig.engine.playing.store(true);
        rig.render(4800);
        rig.start(sink, 0, 2);
        rig.render(4800);
        require(rig.phase() == alien::RecordControls::recording && sink.frames.load() == 4800, "a take started while playing records at once");
        require(closeTo(rig.engine.songTake->firstTick.load(), 4800 * perSample, 1.0e-6), "from the playhead");
        rig.engine.recording.stopRequested.store(true); rig.render(256); rig.ended(); rig.detach();
    }
    {
        Rig rig;   // two tracks with sinks record at once, each from its own input
        alien::AudioCapture a(rate, 1, 5), b(rate, 1, 5);
        rig.engine.songTake->sinks[3].store(&b);
        rig.engine.tracks[3].input.store(1);
        rig.start(a);
        const auto from = rig.total;
        rig.render(2400);
        require(a.frames.load() == 2400 && b.frames.load() == 2400, "every armed track records the same frames");
        rig.engine.recording.stopRequested.store(true); rig.render(256); rig.ended(); rig.detach();
        auto clip = b.finish();
        bool second = clip != nullptr;
        for (std::size_t i = 0; second && i < clip->frames; ++i) second = clip->left[i] == ramp(from + static_cast<long long>(i), 1);
        require(second, "each track records its own input");
    }
    {
        Rig rig;   // capture never allocates in the callback
        alien::AudioCapture sink(rate, 2, 5);
        rig.engine.tracks[0].stereoInput.store(true);
        rig.start(sink, 0, 0, true, 0, 3840);
        rig.render(256);
        const auto before = allocationCount;
        rig.render(48000, 256);
        require(allocationCount == before, "recording a take must not allocate on the audio thread");
        rig.engine.recording.stopRequested.store(true); rig.render(256); rig.ended(); rig.detach();
    }
    std::cout << "PASS: engine capture\n";
}

void refusalTests() {
    {
        Rig rig;   // not in the Song view
        alien::AudioCapture sink(rate, 1, 5);
        rig.engine.patternMode.store(alien::sessionMode);
        rig.start(sink);
        rig.render(512);
        alien::RecordEventKind kind{};
        rig.ended(&kind);
        require(kind == alien::RecordEventKind::refused && rig.phase() == alien::RecordControls::idle && sink.frames.load() == 0,
                "an audio take outside the Song view is refused");
        rig.detach();
    }
    {
        Rig rig;   // no sink
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        rig.engine.songTake->sinks[0].store(nullptr);
        rig.render(512);
        alien::RecordEventKind kind{};
        rig.ended(&kind);
        require(kind == alien::RecordEventKind::refused && !rig.engine.playing.load(), "a take with no armed track is refused and plays nothing");
        rig.detach();
    }
    {
        Rig rig;   // leaving the Song view ends the take
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        rig.render(2400);
        rig.engine.patternMode.store(alien::sessionMode);
        rig.render(512);
        require(rig.ended() == alien::SongTakeCapture::stopped && sink.frames.load() == 2400, "changing the view ends the take");
        rig.detach();
    }
    {
        Rig rig;   // a device restart ends the take with what was captured
        alien::AudioCapture sink(rate, 1, 5);
        rig.start(sink);
        rig.render(2400);
        rig.engine.prepare(rate);
        require(rig.ended() == alien::SongTakeCapture::stopped && rig.phase() == alien::RecordControls::idle, "a device restart ends the take");
        rig.detach();
    }
    std::cout << "PASS: refusals and endings\n";
}

// --- The controller ---------------------------------------------------------------------------------

struct ControllerRig {
    std::unique_ptr<ToneEngine> engineStorage{std::make_unique<ToneEngine>()};
    ToneEngine& engine{*engineStorage};
    std::unique_ptr<alien::ProjectController> controllerStorage{std::make_unique<alien::ProjectController>(engine)};
    alien::ProjectController& controller{*controllerStorage};
    std::vector<alien::ProjectController::SongTakeOutcome> outcomes;
    std::array<float, 1024> in0{}, in1{};
    long long total = 0;
    ControllerRig() {
        engine.prepare(rate);
        controller.onSongTakeFinished = [this](const alien::ProjectController::SongTakeOutcome& o) { outcomes.push_back(o); };
        controller.pushDocumentToEngine();
        engine.patternMode.store(alien::songMode);
        render(256);
    }
    void render(long long samples, int chunk = 256) {
        std::array<float, 1024> left{}, right{};
        while (samples > 0) {
            const int count = static_cast<int>(std::min<long long>(samples, chunk));
            for (int i = 0; i < count; ++i) { in0[static_cast<std::size_t>(i)] = ramp(total + i, 0); in1[static_cast<std::size_t>(i)] = ramp(total + i, 1); }
            const float* inputs[]{in0.data(), in1.data()};
            float* outputs[]{left.data(), right.data()};
            engine.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2, count, {});
            samples -= count; total += count;
            controller.pollRecording();
        }
    }
    void waitForFinish() {
        for (int i = 0; i < 500 && (controller.isBusy() || outcomes.empty()); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    void arm(int track, bool stereo = false, int input = 0) {
        require(controller.editTrackRecording(track, "Arm", [=](alien::TrackSettings& s) { s.armed = true; s.stereoInput = stereo; s.input = input; }),
                "fixture: arm a track");
    }
};

void controllerTests(const juce::File& root) {
    {
        ControllerRig rig;
        auto& c = rig.controller;
        auto settings = c.document().recording;
        settings.countInBars = 0; settings.audioLatencyOffsetMs = 10;
        c.setRecordSettings(settings);
        const auto start = c.startSongTake(root.getChildFile("plain"), rate, 2);
        require(!start.started && start.reason.contains("Arm"), "with no armed track there is nothing to record");
        rig.arm(0);
        rig.arm(2, true);
        const auto scenesBefore = c.document().sceneCount();
        require(c.startSongTake(root.getChildFile("plain"), rate, 2).started && c.isRecording(), "a take starts with two armed tracks");
        require(c.songTakeTracks() == std::vector<int>({0, 2}), "both armed tracks are recorded");
        require(!c.startSongTake(root.getChildFile("plain"), rate, 2).started, "a second take cannot start while one runs");
        require(!c.undo(), "undo waits for the take to end");
        rig.render(9600);
        c.stopRecording();
        rig.render(512);
        rig.waitForFinish();
        require(!c.isRecording() && !c.isBusy() && rig.outcomes.size() == 1, "the take finishes");
        const auto& o = rig.outcomes.back();
        require(o.placed == 2 && o.recorded == 2 && o.problems.isEmpty() && o.kept.empty(), "both takes are placed on the timeline");
        require(c.undoLabel() == "Record take", "the whole take is one undo step");
        std::vector<alien::timeline::Region> regions;
        for (const auto& r : c.timeline().regions) if (!r.isPattern()) regions.push_back(r);
        require(regions.size() == 2, "one region per recorded track");
        for (const auto& r : regions) {
            require(r.start == 0 && closeTo(r.audioOffset, 0.01, 1.0e-9), "a take at the song's start skips its latency instead of moving before 0");
            require(r.length == static_cast<std::int64_t>(std::floor(9600 * perSample - 0.01 * rate * perSample + 1.0e-6)),
                    "the region lasts as long as the recorded audio after the skipped latency");
            const auto& clip = c.document().clip(r.row, r.source);
            require(clip.audio != nullptr && juce::File(clip.audioFilePath).existsAsFile(), "the region plays a Session clip of the new WAV");
            require(clip.audio->channels == (r.row == 2 ? 2 : 1) && clip.audio->frames == 9600, "the clip has the track's channels and every frame");
        }
        require(regions[0].source != regions[1].source || regions[0].row != regions[1].row, "each take has a Session cell of its own");
        const auto file = juce::File(c.document().clip(regions[0].row, regions[0].source).audioFilePath);
        require(c.undo(), "undo");
        bool any = false;
        for (const auto& r : c.timeline().regions) any = any || !r.isPattern();
        require(!any && (regions[0].source >= c.document().sceneCount() || c.document().clip(regions[0].row, regions[0].source).audio == nullptr)
                && c.document().sceneCount() == scenesBefore, "undo removes the takes, their clips and any added scene together");
        require(file.existsAsFile(), "the WAV stays on disk, so redo can bring the take back");
        require(c.redo(), "redo");
        int count = 0;
        for (const auto& r : c.timeline().regions) count += r.isPattern() ? 0 : 1;
        require(count == 2, "redo brings both takes back");
    }
    {
        ControllerRig rig;   // punch and a playhead later in the song
        auto& c = rig.controller;
        auto settings = c.document().recording;
        settings.countInBars = 0; settings.punch = true;
        c.setRecordSettings(settings);
        require(c.editTimeline("Loop", [](alien::timeline::Timeline& t) { return t.setLoop(3840, 7680); }), "fixture: a loop range");
        rig.render(256);
        rig.arm(1);
        rig.engine.timelineSeek.store(1920);
        rig.render(256);
        require(c.startSongTake(root.getChildFile("punch"), rate, 2).started, "start a punched take");
        rig.render(samplesPerBar * 2);
        rig.waitForFinish();
        require(rig.outcomes.size() == 1 && rig.outcomes.back().placed == 1 && rig.outcomes.back().ended == alien::SongTakeCapture::punchedOut,
                "the take ends by itself at the punch-out and is placed");
        const alien::timeline::Region* region = nullptr;
        for (const auto& r : c.timeline().regions) if (r.row == 1) region = &r;
        require(region != nullptr && region->start == 3840 && region->length == 3840 && region->audioOffset == 0.0,
                "the take fills the punch range exactly");
    }
    {
        ControllerRig rig;   // a take over an existing clip replaces that part of it
        auto& c = rig.controller;
        auto settings = c.document().recording;
        settings.countInBars = 0;
        c.setRecordSettings(settings);
        require(c.placeNewClip(0, 0), "fixture: a MIDI clip on track 0");
        rig.render(256);
        rig.arm(0);
        require(c.startSongTake(root.getChildFile("over"), rate, 2).started, "start");
        rig.render(48000);
        c.stopRecording(); rig.render(512); rig.waitForFinish();
        int audio = 0, midi = 0;
        for (const auto& r : c.timeline().regions) if (r.row == 0) (c.document().clip(r.row, r.source).isAudioClip() ? audio : midi)++;
        require(audio == 1 && midi == 1, "the take is laid over the clip, which keeps the part after it");
    }
    {
        ControllerRig rig;   // inputs the device does not have, and a take abandoned by a new project
        auto& c = rig.controller;
        auto settings = c.document().recording;
        settings.countInBars = 0;
        c.setRecordSettings(settings);
        rig.arm(0, true, 1);
        auto start = c.startSongTake(root.getChildFile("missing"), rate, 2);
        require(!start.started && start.reason.contains("inputs"), "an armed track whose input pair is not enabled cannot record");
        rig.arm(1);
        require(c.startSongTake(root.getChildFile("abandon"), rate, 2).started, "start with the track that has its input");
        rig.render(4800);
        c.newProject();
        rig.render(512);
        rig.waitForFinish();
        require(rig.outcomes.size() == 1 && rig.outcomes.back().abandoned && rig.outcomes.back().placed == 0
                && rig.outcomes.back().kept.size() == 1 && rig.outcomes.back().kept[0].existsAsFile(),
                "an abandoned take is kept on disk but not placed");
    }
    {
        ControllerRig rig;   // cancelled before the engine started it: nothing is left behind
        auto& c = rig.controller;
        rig.arm(0);
        const auto folder = root.getChildFile("cancel");
        require(c.startSongTake(folder, rate, 2).started, "start");
        c.stopRecording();
        rig.waitForFinish();
        require(rig.outcomes.size() == 1 && rig.outcomes.back().refused && rig.outcomes.back().recorded == 0, "a cancelled take records nothing");
        require(folder.findChildFiles(juce::File::findFiles, false).isEmpty(), "and leaves no files");
        rig.render(512);
        require(!rig.engine.recording.active() && !rig.engine.playing.load(), "the engine stays idle");
    }
    std::cout << "PASS: controller takes\n";
}

void settingsTests() {
    namespace rm = alien::recordmenu;
    alien::RecordSettings s;
    require(s.valid() && s.audioLatencyOffsetMs == 0 && !s.punch, "audio takes default to no offset and no punch");
    require(rm::apply(s, rm::punchId).punch && !rm::apply(rm::apply(s, rm::punchId), rm::punchId).punch, "the punch item toggles");
    require(rm::apply(s, rm::audioLatencyBase + 8).audioLatencyOffsetMs == 100 && rm::apply(s, rm::audioLatencyBase).audioLatencyOffsetMs == -50,
            "audio latency choices");
    require(rm::apply(s, rm::audioLatencyBase + 8).latencyOffsetMs == 0, "the audio offset is separate from the MIDI one");
    require(rm::apply(s, rm::audioNudgeEarlierId).audioLatencyOffsetMs == 5 && rm::apply(s, rm::audioNudgeLaterId).audioLatencyOffsetMs == -5, "nudges");
    s.audioLatencyOffsetMs = 101;
    require(!s.valid(), "the audio offset has the same limits");
    std::cout << "PASS: audio take settings\n";
}

} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    alien::ProjectController::recoveryDirectoryOverride() =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioSongRecordingRecovery", "", false);
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienStudioSongRecordingTests", "", false);
    require(root.createDirectory().wasOk(), "temp directory");
    settingsTests();
    placementTests();
    captureTests();
    refusalTests();
    controllerTests(root);
    root.deleteRecursively();
    std::cout << "Song recording tests passed\n";
    return 0;
}
