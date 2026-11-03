// T1-04 C: the engine and the project controller at the compiled track and scene limits, rendered
// offline. Every track slot and scene plays, the callback follows the tracks in use, a slot that leaves
// use (a deleted track, transport stop, device restart) leaves no note sounding, and steady playback at
// the limits allocates nothing.
#include "ProjectController.h"
#include "ViewModes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

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
static void* alignedAllocate(std::size_t size, std::align_val_t alignment) {
    ++allocationCount;
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    if (auto* pointer = _aligned_malloc(size == 0 ? 1 : size, align)) return pointer;
#else
    if (auto* pointer = std::aligned_alloc(align, ((size == 0 ? 1 : size) + align - 1) / align * align)) return pointer;
#endif
    throw std::bad_alloc();
}
static void alignedFree(void* pointer) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void* operator new(std::size_t size, std::align_val_t alignment) { return alignedAllocate(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return alignedAllocate(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { alignedFree(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { alignedFree(pointer); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr int tracks = alien::maxSessionTracks, scenes = alien::maxSessionScenes, frames = 256;
constexpr int blocksPerBar = 2 * 48000 / frames + 1;   // one bar at 120 BPM, rounded up

static alien::PianoRollPattern heldChord(int root) {
    alien::PianoRollPattern p; p.loopSteps = 16;
    for (int pitch : {root, root + 4, root + 7}) p.add(pitch, 0, 16);   // held for the whole loop
    return p;
}

struct Engine {
    std::unique_ptr<ToneEngine> engine = std::make_unique<ToneEngine>();
    std::array<float, frames> left{}, right{};
    Engine() { engine->prepare(48000.0); engine->bpm.store(120); }
    // The loudest output sample over `blocks` callbacks.
    float render(int blocks) {
        float peak = 0.0f;
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) {
            engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, frames, {});
            for (int i = 0; i < frames; ++i) {
                require(std::isfinite(left[static_cast<std::size_t>(i)]) && std::isfinite(right[static_cast<std::size_t>(i)]), "finite output");
                peak = std::max({peak, std::abs(left[static_cast<std::size_t>(i)]), std::abs(right[static_cast<std::size_t>(i)])});
            }
        }
        return peak;
    }
    void launch(int track, int root) {
        alien::SessionClipLaunch launch; launch.pattern = heldChord(root); launch.clipIndex = 0;
        require(engine->pianoRollLaunches[static_cast<std::size_t>(track)].publish(launch), "Session launch published");
    }
};

// The controller at the compiled limits: a scene launch in the last scene starts a clip on every track,
// across the whole 64-bit synchronized-launch mask, and each track's own strip sounds.
static void everyTrackAndSceneLaunches() {
    Engine rig;
    alien::ProjectController c(*rig.engine);
    while (c.addTrack()) {}
    while (c.addScene()) {}
    const auto& d = c.document();
    require(d.trackCount() == tracks && d.sceneCount() == scenes, "the document reaches the compiled limits");
    require(rig.engine->activeTracks.load() == tracks, "the engine follows every track");
    for (int t = 0; t < tracks; ++t) c.setClipPattern(t, scenes - 1, heldChord(48 + t % 24), false);
    rig.engine->patternMode.store(alien::sessionMode);
    rig.engine->playing.store(true);
    rig.render(4);
    c.launchScene(scenes - 1);
    std::vector<float> peaks(static_cast<std::size_t>(tracks), 0.0f);
    float output = 0.0f;
    for (int b = 0; b < 2 * blocksPerBar; ++b) {
        output = std::max(output, rig.render(1));
        for (int t = 0; t < tracks; ++t) peaks[static_cast<std::size_t>(t)] = std::max(peaks[static_cast<std::size_t>(t)], rig.engine->tracks[static_cast<std::size_t>(t)].peak.load());
    }
    require(output > 0.0f, "the scene is audible");
    for (int t = 0; t < tracks; ++t) {
        require(peaks[static_cast<std::size_t>(t)] > 0.0f, "every track's strip sounds, including the last");
        require(rig.engine->activePianoRollClip[static_cast<std::size_t>(t)].load() == scenes - 1, "every track plays the last scene's clip");
    }

    // Steady playback of every track at the limits allocates nothing.
    const auto before = allocationCount;
    rig.render(256);
    require(allocationCount == before, "callbacks with every track playing allocate nothing");

    // Stopping the transport ends every note on every slot.
    rig.engine->playing.store(false);
    rig.render(4);
    require(rig.render(blocksPerBar) == 0.0f, "stop leaves no track sounding");
}

// A slot that leaves use plays one more block with its notes ended, and is then silent; when a track takes
// the slot again nothing from before comes back.
static void slotsLeavingUseFallSilent() {
    Engine rig;
    rig.engine->activeTracks.store(tracks);
    rig.engine->patternMode.store(alien::sessionMode);
    rig.engine->playing.store(true);
    rig.launch(tracks - 1, 60);
    require(rig.render(2 * blocksPerBar) > 0.0f, "the last track slot plays");
    require(rig.engine->activePianoRollClip[static_cast<std::size_t>(tracks - 1)].load() == 0, "the last slot's clip is active");

    rig.engine->activeTracks.store(tracks - 1);   // the last track was deleted
    rig.render(2);
    require(rig.render(blocksPerBar) == 0.0f, "a slot leaving use is silent from its next block");
    require(rig.engine->activePianoRollClip[static_cast<std::size_t>(tracks - 1)].load() == -1, "its clip is stopped");

    const auto before = allocationCount;
    rig.engine->activeTracks.store(tracks);       // a new track takes the slot
    require(rig.render(2 * blocksPerBar) == 0.0f, "no note or clip resumes when the slot is used again");
    require(allocationCount == before, "slots leaving and entering use allocate nothing");

    // A launch for a slot that is not in use waits for a track to use it, and plays then.
    rig.engine->activeTracks.store(1);
    rig.render(2);
    rig.launch(tracks - 1, 64);
    require(rig.render(2 * blocksPerBar) == 0.0f, "a slot out of use stays silent");
    rig.engine->activeTracks.store(tracks);
    require(rig.render(2 * blocksPerBar) > 0.0f, "its launch plays once a track uses the slot");
}

// A device restart (stopped, then prepared again) with every slot sounding: the transport keeps running and
// the clips start again from the top, and nothing from before the restart outlives a stop.
static void deviceRestartAtTheLimit() {
    Engine rig;
    rig.engine->activeTracks.store(tracks);
    rig.engine->patternMode.store(alien::sessionMode);
    rig.engine->playing.store(true);
    for (int t = 0; t < tracks; ++t) rig.launch(t, 48 + t % 24);
    require(rig.render(2 * blocksPerBar) > 0.0f, "every slot plays");
    rig.engine->audioDeviceStopped();
    rig.engine->prepare(48000.0);
    require(rig.render(blocksPerBar) > 0.0f, "the clips play on after the restart");
    for (int t = 0; t < tracks; ++t)
        require(rig.engine->activePianoRollClip[static_cast<std::size_t>(t)].load() == 0, "every slot's clip restarted");
    rig.engine->playing.store(false);
    rig.render(4);
    require(rig.render(blocksPerBar) == 0.0f, "no note survives a device restart and a stop");

    // The device stops while the slot set shrinks: the slot that left use stays silent afterwards.
    rig.engine->playing.store(true);
    rig.launch(tracks - 1, 60);
    require(rig.render(2 * blocksPerBar) > 0.0f, "the last slot plays again");
    rig.engine->activeTracks.store(1);
    rig.engine->audioDeviceStopped();
    rig.engine->prepare(48000.0);
    rig.engine->playing.store(false);
    rig.render(4);
    rig.engine->playing.store(true);
    rig.engine->activeTracks.store(tracks);
    std::vector<float> peaks(static_cast<std::size_t>(tracks), 0.0f);
    for (int b = 0; b < 2 * blocksPerBar; ++b) {
        rig.render(1);
        for (int t = 0; t < tracks; ++t) peaks[static_cast<std::size_t>(t)] = std::max(peaks[static_cast<std::size_t>(t)], rig.engine->tracks[static_cast<std::size_t>(t)].peak.load());
    }
    require(peaks[0] > 0.0f, "the slot still in use plays its own clip again");
    for (int t = 1; t < tracks; ++t)
        require(peaks[static_cast<std::size_t>(t)] == 0.0f, "a slot that left use during a restart brings nothing back");
}

int main() {
    everyTrackAndSceneLaunches();
    slotsLeavingUseFallSilent();
    deviceRestartAtTheLimit();
    std::cout << "TrackCapacityTests passed (" << tracks << " tracks, " << scenes << " scenes; ToneEngine object "
        << sizeof(ToneEngine) << " bytes, ProjectController object " << sizeof(alien::ProjectController) << " bytes)\n";
    return 0;
}
