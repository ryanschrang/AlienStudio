// T1-04 C: the engine and the project controller at the compiled track and scene limits, rendered
// offline. Every track slot and scene plays, the callback follows the tracks in use, a slot that leaves
// use (a deleted track, transport stop, device restart) leaves no note sounding, and steady playback at
// the limits allocates nothing.
#include "CommandEngine.h"
#include "PlaceSongClipFixture.h"
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
    rig.engine->setActiveTracks(tracks);
    rig.engine->patternMode.store(alien::sessionMode);
    rig.engine->playing.store(true);
    rig.launch(tracks - 1, 60);
    require(rig.render(2 * blocksPerBar) > 0.0f, "the last track slot plays");
    require(rig.engine->activePianoRollClip[static_cast<std::size_t>(tracks - 1)].load() == 0, "the last slot's clip is active");

    rig.engine->setActiveTracks(tracks - 1);   // the last track was deleted
    rig.render(2);
    require(rig.render(blocksPerBar) == 0.0f, "a slot leaving use is silent from its next block");
    require(rig.engine->activePianoRollClip[static_cast<std::size_t>(tracks - 1)].load() == -1, "its clip is stopped");

    const auto before = allocationCount;
    rig.engine->setActiveTracks(tracks);       // a new track takes the slot
    require(rig.render(2 * blocksPerBar) == 0.0f, "no note or clip resumes when the slot is used again");
    require(allocationCount == before, "slots leaving and entering use allocate nothing");

    // A launch for a slot that is not in use waits for a track to use it, and plays then.
    rig.engine->setActiveTracks(1);
    rig.render(2);
    rig.launch(tracks - 1, 64);
    require(rig.render(2 * blocksPerBar) == 0.0f, "a slot out of use stays silent");
    rig.engine->setActiveTracks(tracks);
    require(rig.render(2 * blocksPerBar) > 0.0f, "its launch plays once a track uses the slot");
}

// A device restart (stopped, then prepared again) with every slot sounding: the transport keeps running and
// the clips start again from the top, and nothing from before the restart outlives a stop.
static void deviceRestartAtTheLimit() {
    Engine rig;
    rig.engine->setActiveTracks(tracks);
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
    rig.engine->setActiveTracks(1);
    rig.engine->audioDeviceStopped();
    rig.engine->prepare(48000.0);
    rig.engine->playing.store(false);
    rig.render(4);
    rig.engine->playing.store(true);
    rig.engine->setActiveTracks(tracks);
    std::vector<float> peaks(static_cast<std::size_t>(tracks), 0.0f);
    for (int b = 0; b < 2 * blocksPerBar; ++b) {
        rig.render(1);
        for (int t = 0; t < tracks; ++t) peaks[static_cast<std::size_t>(t)] = std::max(peaks[static_cast<std::size_t>(t)], rig.engine->tracks[static_cast<std::size_t>(t)].peak.load());
    }
    require(peaks[0] > 0.0f, "the slot still in use plays its own clip again");
    for (int t = 1; t < tracks; ++t)
        require(peaks[static_cast<std::size_t>(t)] == 0.0f, "a slot that left use during a restart brings nothing back");
}

// T1-04 E1: structure edits at the compiled limits, through the command route the UI uses. A full grid
// refuses a new track, a new scene and a scene duplicate with a reason, changing nothing (not even the
// undo history); a clip moves into and out of the last cell; the last track and scene reorder; deleting the last track and undoing it puts
// its clip back and the engine follows; save and reopen at the limits keeps the last cell.
static void structureEditsAtTheLimits() {
    Engine rig;
    alien::ProjectController c(*rig.engine);
    alien::command::CommandEngine commands(c);
    const auto& d = c.document();
    const auto saved = [&] { return alien::serializeSessionProject(alien::toProjectData(d)); };
    while (d.trackCount() < tracks) require(commands.execute(alien::command::CreateTrack{}).ok, "tracks are created up to the limit");
    while (d.sceneCount() < scenes) require(commands.execute(alien::command::CreateScene{}).ok, "scenes are created up to the limit");
    require(rig.engine->activeTracks.load() == tracks, "the engine follows every created track");

    const int lastTrack = tracks - 1, lastScene = scenes - 1;
    c.setClipPattern(lastTrack, lastScene, heldChord(60), false);
    c.commit("Edit notes", lastTrack, lastScene);
    const auto full = saved();
    const auto label = c.undoLabel();
    for (const alien::command::Command& refused : {alien::command::Command{alien::command::CreateTrack{}},
                                                   alien::command::Command{alien::command::CreateScene{}},
                                                   alien::command::Command{alien::command::DuplicateScene{lastScene}},
                                                   alien::command::Command{alien::command::DuplicateScene{0}}}) {
        const auto result = commands.execute(refused);
        require(!result.ok && result.error.isNotEmpty(), "a full grid refuses a new track or scene with a reason");
        require(saved() == full && c.undoLabel() == label && rig.engine->activeTracks.load() == tracks,
                "a refused edit at the limits changes nothing, not even the undo history");
    }

    // The last cell is an ordinary cell: a clip moves out of it and back, and undo follows.
    require(commands.execute(alien::command::MoveClip{lastTrack, lastScene, 0, lastScene - 1}).ok
            && d.clip(0, lastScene - 1).notes().count == 3 && d.clip(lastTrack, lastScene).notes().count == 0,
            "a clip moves out of the last cell");
    require(c.undo() && saved() == full, "undo puts it back in the last cell");
    require(c.redo() && d.clip(0, lastScene - 1).notes().count == 3, "redo moves it again");
    require(c.undo() && saved() == full, "fixture: the clip is back in the last cell");

    // The last track and the last scene move to the front, and back with undo. The first track and scene are
    // selected, so the selection follows them to second place and, on undo, back to the front.
    require(commands.execute(alien::command::MoveTrack{lastTrack, 0}).ok && d.clip(0, lastScene).notes().count == 3
            && d.clip(lastTrack, lastScene).notes().count == 0, "the last track moves to the front with its clip");
    require(c.undo() && saved() == full && d.selectedTrack == 0, "undo moves it back to the end, and the same track stays selected");
    require(commands.execute(alien::command::MoveScene{lastScene, 0}).ok && d.clip(lastTrack, 0).notes().count == 3
            && d.clip(lastTrack, lastScene).notes().count == 0, "the last scene moves to the front with its clip");
    require(c.undo() && saved() == full && d.selectedScene == 0, "undo moves it back to the end, and the same scene stays selected");

    // Deleting the last track and undoing it: the clip comes back and the engine uses the slot again.
    require(commands.execute(alien::command::DeleteTrack{lastTrack}).ok && d.trackCount() == tracks - 1
            && rig.engine->activeTracks.load() == tracks - 1, "the last track is deleted and its slot leaves use");
    require(c.undo() && saved() == full && rig.engine->activeTracks.load() == tracks, "undo brings the last track and its clip back");
    require(c.redo() && d.trackCount() == tracks - 1 && rig.engine->activeTracks.load() == tracks - 1, "redo deletes it again");
    require(c.undo() && saved() == full, "fixture: the full grid again");

    // The restored last track plays its clip from the last scene.
    rig.engine->patternMode.store(alien::sessionMode);
    rig.engine->playing.store(true);
    rig.render(4);
    c.launchClip(lastTrack, lastScene);
    float peak = 0.0f;
    for (int b = 0; b < 2 * blocksPerBar; ++b) { rig.render(1); peak = std::max(peak, rig.engine->tracks[static_cast<std::size_t>(lastTrack)].peak.load()); }
    require(peak > 0.0f && rig.engine->activePianoRollClip[static_cast<std::size_t>(lastTrack)].load() == lastScene,
            "the restored last track plays the last scene's clip");
    rig.engine->playing.store(false);
    rig.render(4);

    // Save and reopen at the limits.
    alien::SessionProjectData reopened; juce::String error;
    require(alien::parseSessionProject(full, reopened, error) && reopened.trackCount() == tracks && reopened.sceneCount() == scenes
            && alien::serializeSessionProject(reopened) == full, "a full grid saves and reopens unchanged");
}

// T1-04 E3: the clipboard, duplicate and make-unique at the full grid, in the Session and on the Song. The last cell takes a copy, a
// linked copy and a duplicate like any other, each one undo step; a Song audio placement whose track has no free cell and no scene
// left is refused its own copy with a reason and nothing changes, and takes the first cell that is freed.
static void clipboardAtTheLimits() {
    Engine rig;
    alien::ProjectController c(*rig.engine);
    alien::command::CommandEngine commands(c);
    const auto& d = c.document();
    const auto saved = [&] { return alien::serializeSessionProject(alien::toProjectData(d)); };
    while (d.trackCount() < tracks) require(commands.execute(alien::command::CreateTrack{}).ok, "fixture: tracks up to the limit");
    while (d.sceneCount() < scenes) require(commands.execute(alien::command::CreateScene{}).ok, "fixture: scenes up to the limit");
    const int lastTrack = tracks - 1, lastScene = scenes - 1;

    // Session: copy, linked copy, make unique and duplicate into the last cell.
    c.setClipPattern(lastTrack - 1, 0, heldChord(48), false);
    c.commit("Edit notes", lastTrack - 1, 0);
    c.selectClip(lastTrack - 1, 0);   // undoing an edit of this clip selects it
    const auto before = saved();
    require(c.copyCell(lastTrack - 1, 0), "fixture: copy a clip");
    require(commands.execute(alien::command::PasteClip{lastTrack, lastScene}).changed && d.clip(lastTrack, lastScene).notes().count == 3
            && d.linkOf(lastTrack, lastScene) != d.linkOf(lastTrack - 1, 0), "a copy pastes into the last cell, independent of its source");
    require(c.undo() && saved() == before, "undo empties the last cell again");
    require(commands.execute(alien::command::PasteLinkedClip{lastTrack, lastScene}).changed
            && d.linkOf(lastTrack, lastScene) == d.linkOf(lastTrack - 1, 0), "a linked copy pastes into the last cell");
    c.setClipPattern(lastTrack - 1, 0, heldChord(60), false);
    c.commit("Edit notes", lastTrack - 1, 0);
    require(d.clip(lastTrack, lastScene).notes().notes[0].pitch == 60, "an edit of the source reaches the linked last cell");
    require(commands.execute(alien::command::MakeClipUnique{lastTrack, lastScene}).changed
            && d.linkOf(lastTrack, lastScene) != d.linkOf(lastTrack - 1, 0), "the last cell is made unique");
    require(c.undo() && d.linkOf(lastTrack, lastScene) == d.linkOf(lastTrack - 1, 0), "undo links the last cell again");
    require(c.undo() && d.clip(lastTrack, lastScene).notes().notes[0].pitch == 48, "undo takes back the source edit in both cells");
    require(c.undo() && d.clip(lastTrack, lastScene).notes().count == 0, "undo empties the last cell");
    require(saved() == before, "each step undoes on its own, back to the project before the paste");
    require(commands.execute(alien::command::DuplicateClip{lastTrack - 1, 0, lastTrack, lastScene}).changed
            && d.clip(lastTrack, lastScene).notes().count == 3 && d.linkOf(lastTrack, lastScene) != d.linkOf(lastTrack - 1, 0),
            "a clip duplicates into the last cell");
    require(commands.execute(alien::command::DuplicateClip{lastTrack, lastScene, 0, lastScene}).changed && d.clip(0, lastScene).notes().count == 3,
            "and out of it");
    require(c.undo() && c.undo() && saved() == before, "undo removes both duplicates");

    // Song: an audio placement needs a free cell of its own track to be made unique.
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 48000; audio->channels = 1; audio->frames = 4800;
    audio->left.assign(4800, 0.2f); audio->buildWaveform();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("capacity-clipboard.wav");
    require(alien::command::installDecodedAudio(c, file, lastTrack, 0, audio).ok, "fixture: audio in the last track's first cell");
    for (int s = 1; s < scenes; ++s) c.setClipPattern(lastTrack, s, heldChord(36), false);
    c.commit("Fill the last track");
    std::uint32_t placed = 0;
    require(alien::test::placeSongClip(c, lastTrack, 0, 0, &placed) && placed != 0
            && alien::test::placeSongClip(c, lastTrack, 0, 4 * 3840), "fixture: the audio clip placed twice on the Song");
    const auto full = saved();
    const auto label = c.undoLabel();
    const auto refused = commands.execute(alien::command::MakeSongClipUnique{placed});
    require(!refused.ok && refused.error.contains("no room") && saved() == full && c.undoLabel() == label,
            "with every cell of its track in use and no scene left, the placement is refused its own copy and nothing changes");
    const int freed = scenes / 2;
    require(commands.execute(alien::command::DeleteClip{lastTrack, freed}).changed, "free one cell of the last track");
    require(commands.execute(alien::command::MakeSongClipUnique{placed}).changed && d.timeline.find(placed) != nullptr
            && d.timeline.find(placed)->source == freed && d.clip(lastTrack, freed).audio == audio && d.sceneCount() == scenes,
            "then the placement takes the freed cell, adding no scene");
    require(c.undo() && d.timeline.find(placed)->source == 0 && d.clip(lastTrack, freed).audio == nullptr, "undo gives it back its shared cell");
}

int main() {
    structureEditsAtTheLimits();
    clipboardAtTheLimits();
    everyTrackAndSceneLaunches();
    slotsLeavingUseFallSilent();
    deviceRestartAtTheLimit();
    std::cout << "TrackCapacityTests passed (" << tracks << " tracks, " << scenes << " scenes; ToneEngine object "
        << sizeof(ToneEngine) << " bytes, ProjectController object " << sizeof(alien::ProjectController) << " bytes)\n";
    return 0;
}
