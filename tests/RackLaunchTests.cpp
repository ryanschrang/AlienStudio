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
            const auto before = allocations;
            player->processLaunched(*snap, samples, running, 120, 4, n, pl.data(), pr.data(), request, quantum, startSelected);
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

int main() {
    selectionAndTiming(); boundariesAndLifecycle(); deletionAndEdits(); noteOwnership();
    std::puts("PASS: identity-based rack launch (quantization, selection, revisions, deletion, lifecycle, allocation safety)");
}
