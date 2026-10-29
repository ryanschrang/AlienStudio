// Quantized identity-based rack launch, independent of editor selection. No JUCE.
#include "RackEngine.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
using namespace alien;
using namespace alien::rack;
static constexpr auto none = RackPlayer::noLaunchRequest;
struct Rig {
    Project project = Project::createDefault();
    std::shared_ptr<const Snapshot> snap;
    std::unique_ptr<RackPlayer> player = std::make_unique<RackPlayer>();
    std::shared_ptr<SessionAudioClip> sample = std::make_shared<SessionAudioClip>();
    RackPlayer::SampleTable samples{};
    std::vector<float> output;
    bool externalClock{}, multi{};
    RackPlayer::LaunchSources requests = RackPlayer::noRequests(), sceneSources = RackPlayer::noRequests();
    std::uint64_t sceneRequest{none};
    int sceneFrame{-1};
    Rig() {
        player->prepare(8000);
        sample->sampleRate = 8000; sample->channels = 1; sample->frames = 8; sample->left.assign(8, 1);
        samples[0].store(sample.get());
        require(project.setStepOn(0, 0, 0, true) && project.setStepOn(0, 0, 1, true), "first pattern");
        require(project.addPattern(0), "second pattern");
        require(project.setStepField(1, 0, 0, StepField::velocity, 25), "different launch sound");
        project.clips[0] = 101; project.clips[1] = 102; project.selectedPattern = 0; publish();
    }
    void publish() { snap = project.snapshot(); }
    void render(int frames, std::uint64_t request = none, int quantum = 4, bool startSelected = true, bool running = true, int block = 256) {
        std::array<std::array<float, 256>, RackPlayer::busCount> l{}, r{};
        std::array<float*, RackPlayer::busCount> pl{}, pr{};
        for (std::size_t b = 0; b < pl.size(); ++b) { pl[b] = l[b].data(); pr[b] = r[b].data(); }
        for (int done = 0; done < frames;) {
            const auto n = std::min({block, 256, frames - done});
            for (auto& x : l) x.fill(0); for (auto& x : r) x.fill(0);
            std::array<RackPlayer::LaunchTick, 256> ticks{};
            int tickCount = 0;
            const int frame = static_cast<int>(output.size());
            if (externalClock) for (int i = 0; i < n; ++i)
                if ((frame + i) % 1000 == 0) ticks[static_cast<std::size_t>(tickCount++)] = {i, (frame + i) / 1000};
            const auto before = allocations;
            player->processLaunched(*snap, samples, running, 120, 4, n, pl.data(), pr.data(), request, quantum, startSelected,
                externalClock ? ticks.data() : nullptr, tickCount, sceneRequest,
                sceneFrame >= frame && sceneFrame < frame + n ? sceneFrame - frame : -1,
                multi ? &requests : nullptr, multi ? &sceneSources : nullptr);
            requests = RackPlayer::noRequests();
            const auto after = allocations;
            require(before == after, "launch, switch, stop and rendering allocate no memory");
            request = none;
            output.insert(output.end(), l[RackPlayer::masterBus].begin(), l[RackPlayer::masterBus].begin() + n);
            done += n;
        }
    }
};

static void selectionAndTiming() {
    Rig r;
    r.render(1500);
    require(r.player->playingClip() == 101 && r.output[0] > 0.5f, "Rack playback starts selected clip");
    r.project.selectPattern(1); r.publish(); r.render(2000);
    require(r.player->playingClip() == 101, "editor selection cannot switch playback");
    r.render(500, 102);
    require(r.player->playingClip() == 101 && r.player->waitingClip() == 102, "launch waits for beat boundary");
    r.render(1);
    require(r.player->playingClip() == 102 && r.player->currentStep() == 0 && r.output[4000] > 0.1f && r.output[4000] < 0.2f,
            "launch starts the new clip at its step zero, exactly on the beat");
    r.render(500, 0);
    require(r.player->playingClip() == 102 && r.player->waitingClip() == 0, "stop is quantized too");
    r.render(3500);
    require(r.player->playingClip() == 0 && r.player->currentStep() == -1 && r.player->pendingHits() == 0, "boundary stops notes and queued hits");
    r.render(3000);
    require(!r.player->busy(), "stop leaves no stuck voice");
}

static void boundariesAndLifecycle() {
    Rig a, b;
    a.render(1500); b.render(1500, none, 4, true, true, 17);
    a.render(14501, 102, 16); b.render(14501, 102, 16, true, true, 17);
    require(a.output == b.output && a.player->playingClip() == 102 && a.output[16000] > 0.1f && a.output[16000] < 0.2f,
            "bar launch is sample-identical across callback block sizes");
    Rig r;
    r.render(1500, none, 4, false);
    require(r.player->playingClip() == 0 && !r.player->busy(), "Session is silent until explicitly launched");
    r.render(500, 101); r.render(1, 0); r.render(2000);
    require(r.player->playingClip() == 0, "latest request wins before boundary");
    r.render(500, 102); r.render(1, none, 4, false, false); r.render(5000, none, 4, false);
    require(r.player->playingClip() == 0 && r.player->waitingClip() == none, "transport stop clears pending launches");
    r.render(3000, 9999);
    require(r.player->playingClip() == 0, "unknown identity cannot launch unrelated content");
}

static void deletionAndEdits() {
    Rig r;
    r.render(1500, 102);
    require(r.player->playingClip() == 102, "explicit clip launches at transport start");
    r.project.deletePattern(0); r.publish(); r.render(1);
    require(r.player->playingClip() == 102 && r.snap->indexOfClip(102) == 0, "deleting a preceding slot preserves active identity");
    require(r.project.addPattern(), "add independent pattern"); r.project.clips[1] = 103; r.publish();
    r.render(100, 103); r.project.deletePattern(1); r.publish(); r.render(1);
    require(r.player->waitingClip() == none && r.player->playingClip() == 102, "deleting queued clip cancels only its request");
    // Change the playing source while editing another slot: the next loop uses its revision.
    require(r.project.setStepField(0, 0, 0, StepField::velocity, 100), "edit source"); r.publish();
    r.render(16001 - static_cast<int>(r.output.size()));
    require(r.output[16000] > 0.5f && r.player->playingClip() == 102, "source edits are heard without relaunch");
    require(r.project.addPattern(), "retain a bank slot"); r.project.clips[1] = 104;
    r.project.deletePattern(0); r.publish(); r.render(5000);
    require(r.player->playingClip() == 0 && !r.player->busy(), "deleting active source stops safely");
}

static void noteOwnership() {
    Rig r, reference;
    const auto setup = [](Rig& x) {
        x.sample->frames = 50000; x.sample->left.assign(50000, 1);
        PianoRollPattern notes; notes.loopSteps = 16; notes.add(60, 0, 8);
        require(x.project.setChannelNotes(1, 0, notes), "held note source"); x.publish();
    };
    setup(r); setup(reference);
    r.render(1500, 102); reference.render(1500, 102);
    r.project.deletePattern(0); r.publish(); r.render(2500); reference.render(2500);
    require(r.output == reference.output, "deleting another slot neither cuts nor retriggers a held note");
    r.render(1, 0); r.render(6000);
    require(!r.player->busy() && r.player->pendingHits() == 0, "quantized stop releases held notes");
    Rig delayed;
    require(delayed.project.setStepOn(0, 0, 3, true)
        && delayed.project.setStepField(0, 0, 3, StepField::nudge, 100)
        && delayed.project.setStepField(0, 0, 3, StepField::ratchet, 4), "nudged ratchet across launch boundary");
    delayed.publish(); delayed.render(3500); delayed.render(1500, 0);
    require(delayed.player->pendingHits() == 0 && delayed.output[4000] == 0 && delayed.output[4250] == 0,
            "a stop drops delayed hits belonging to the previous playback instance");
}

static void overlappingLaunches() {
    Rig r, reference;
    const auto setup = [](Rig& x) {
        x.multi = true;
        x.sample->frames = 50000; x.sample->left.assign(50000, 1);
        require(x.project.setPatternLength(0, 64), "long overlap pattern");
        PianoRollPattern notes; notes.loopSteps = 64; notes.add(60, 0, 32);
        require(x.project.setChannelNotes(0, 0, notes), "overlap held source"); x.publish();
    };
    setup(r); setup(reference);
    r.requests[1] = r.requests[2] = reference.requests[2] = 101;
    r.render(1500, none, 4, false); reference.render(1500, none, 4, false);
    require(r.player->playingClip(1) == 101 && r.player->playingClip(2) == 101
        && r.player->activeVoices() == 2 && r.output[100] == 2 * reference.output[100],
        "two occurrences of the same source retain separate held-note voices");
    r.project.selectPattern(1); r.publish();
    r.requests[1] = 0;
    r.render(4500, none, 4, false); reference.render(4500, none, 4, false);
    require(r.player->playingClip(1) == 0 && r.player->playingClip(2) == 101
        && r.player->activeVoices() == 1 && r.output[5500] == reference.output[5500],
        "stopping one row releases only that occurrence, without selecting another source");
    // A scene is a complete slot assignment, and can launch different sources together.
    r.externalClock = true; r.sceneSources.fill(0); r.sceneSources[1] = 101; r.sceneSources[2] = 102;
    r.sceneFrame = 8000; r.render(2001, none, 4, false);
    require(r.player->playingClip(1) == 101 && r.player->playingClip(2) == 102, "a scene starts distinct sources on the same boundary");
    r.render(4000, 0, 4, false);
    require(r.player->playingClip(1) == 0 && r.player->playingClip(2) == 0 && r.player->pendingHits() == 0,
        "menu Stop rack stops all slots on the next boundary");
    r.render(2000, none, 4, false, false); r.render(1, none, 4, false);
    require(!r.player->busy() && r.player->playingClip(1) == 0, "transport restart cannot resurrect a Session occurrence");
}

static void overlappingDelayedHits() {
    Rig a, b;
    const auto setup = [](Rig& r) {
        r.multi = true;
        r.project.setStepOn(0, 0, 1, false);
        r.project.setStepField(0, 0, 0, StepField::nudge, 100);
        r.project.setStepField(0, 0, 0, StepField::ratchet, 4);
        r.requests[1] = 101; r.requests[2] = 102;
        r.publish();
    };
    setup(a); setup(b);
    a.render(500, none, 4, false); b.render(500, none, 4, false, true, 17);
    a.requests[1] = b.requests[1] = 0;
    a.render(5501, none, 4, false); b.render(5501, none, 4, false, true, 17);
    require(a.output == b.output && a.player->playingClip(2) == 102, "overlapping launch and per-row stop are callback-size independent");
    // Queue hits on both occurrences just before the boundary, then stop only one.
    Rig r; r.multi = true;
    r.project.setStepOn(0, 0, 3, true); r.project.setStepField(0, 0, 3, StepField::nudge, 100);
    r.project.setStepField(0, 0, 3, StepField::ratchet, 4);
    r.project.setStepOn(1, 0, 3, true); r.project.setStepField(1, 0, 3, StepField::nudge, 100);
    r.project.setStepField(1, 0, 3, StepField::ratchet, 4); r.publish();
    r.requests[1] = 101; r.requests[2] = 102; r.render(3500, none, 4, false);
    require(r.player->pendingHits() == 8, "both instances own delayed ratchets");
    r.requests[1] = 0; r.render(501, none, 4, false);
    require(r.player->pendingHits() == 3 && r.output[4000] > 0.5f && r.output[4000] < 0.6f,
        "the boundary cancels only the stopped instance's delayed hits");
    r.project.deletePattern(0); r.publish(); r.render(751, none, 4, false);
    require(r.output[4250] > 0.5f && r.player->playingClip(2) == 102,
        "pending step hits follow stable source identity after bank reorder");
}

static void timelineInstanceOwnership() {
    Rig r;
    r.project.setStepOn(0, 0, 1, false);
    r.project.setStepField(0, 0, 0, StepField::nudge, 100);
    r.project.setStepField(0, 0, 0, StepField::ratchet, 4); r.publish();
    std::array<std::array<float, 2000>, RackPlayer::busCount> l{}, rr{};
    std::array<float*, RackPlayer::busCount> pl{}, pr{};
    for (std::size_t b = 0; b < pl.size(); ++b) { pl[b] = l[b].data(); pr[b] = rr[b].data(); }
    constexpr auto forever = std::numeric_limits<std::int64_t>::max();
    const RackPlayer::TimelineHit hits[] = {
        {0, 0, 0, forever, 0, RackPlayer::launchSlots, true},
        {0, 0, 0, forever, 0, RackPlayer::launchSlots + 1, true},
        {500, -1, 0, 0, 0, RackPlayer::launchSlots, true}
    };
    const auto before = allocations;
    r.player->processTimeline(*r.snap, r.samples, true, hits, 3, 120, 4, 2000, pl.data(), pr.data());
    require(allocations == before && l[RackPlayer::masterBus][1000] > 0.5f && l[RackPlayer::masterBus][1000] < 0.6f
        && l[RackPlayer::masterBus][1250] > 0.5f && r.player->pendingHits() == 0,
        "ending one Song occurrence drops only its ratchets, with no callback allocation");
    // Fill every launch slot while retaining the existing global voice bound.
    Rig capacity; capacity.multi = true;
    capacity.sample->frames = 50000; capacity.sample->left.assign(50000, 1);
    capacity.requests.fill(101); capacity.render(1, 101, 4, false);
    require(capacity.player->activeVoices() == RackPlayer::launchSlots, "all fixed Session slots fit in the shared voice pool");
}

int main() {
    {
        Rig r; r.externalClock = true; r.project.setSwing(1); r.publish(); r.render(2000);
        require(r.output[1000] == 0 && r.output[1500] > 0.5f, "external Session clock retains rack swing");
        r.sceneRequest = 102; r.sceneFrame = 4000; r.render(2000);
        require(r.player->playingClip() == 101, "scene source waits for its precise shared readiness sample");
        r.render(1);
        require(r.player->playingClip() == 102 && r.output[4000] > 0.1f && r.output[4000] < 0.2f,
                "scene starts step zero on the exact Session-clock boundary");
    }
    selectionAndTiming(); boundariesAndLifecycle(); deletionAndEdits(); noteOwnership();
    overlappingLaunches(); overlappingDelayedHits(); timelineInstanceOwnership();
    std::puts("PASS: identity-based rack launch (quantization, selection, revisions, deletion, lifecycle, allocation safety)");
}
