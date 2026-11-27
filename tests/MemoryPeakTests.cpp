#include "CommandEngine.h"
#include "WavExport.h"
#include "AudioFileWrite.h"
#include "AudioClipImport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <string_view>

// T1-04 D4: peak heap memory while a full project is saved, opened, edited with undo/redo and exported.
// Every C++ allocation (plain, array, aligned, nothrow) carries a small header with its size, so the
// live total and its high-water mark are known on every thread, including the load and export workers.
// malloc-based storage (JUCE HeapBlock) and the operating system's own allocations are not seen.
// Without arguments the checks run; --report also prints the measurements as a Markdown table.

namespace {
std::atomic<std::int64_t> liveBytes{0}, peakBytes{0};
// T1-04 D7: a throwing allocation of exactly this many bytes fails, as if memory had run out (0: none).
std::atomic<std::size_t> failSize{0};

void track(std::int64_t delta) noexcept {
    const auto now = liveBytes.fetch_add(delta, std::memory_order_relaxed) + delta;
    auto peak = peakBytes.load(std::memory_order_relaxed);
    while (now > peak && !peakBytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
}

// [raw pointer][size] just below the pointer handed out; the offset keeps the requested alignment.
void* allocate(std::size_t size, std::size_t alignment) noexcept {
    alignment = std::max<std::size_t>(alignment, 2 * sizeof(void*));
    auto* raw = static_cast<unsigned char*>(std::malloc(size + alignment + 2 * sizeof(void*)));
    if (raw == nullptr) return nullptr;
    auto address = reinterpret_cast<std::uintptr_t>(raw) + 2 * sizeof(void*);
    address = (address + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    auto* words = reinterpret_cast<std::uintptr_t*>(address);
    words[-1] = reinterpret_cast<std::uintptr_t>(raw);
    words[-2] = size;
    track(static_cast<std::int64_t>(size));
    return reinterpret_cast<void*>(address);
}
void release(void* pointer) noexcept {
    if (pointer == nullptr) return;
    auto* words = static_cast<std::uintptr_t*>(pointer);
    track(-static_cast<std::int64_t>(words[-2]));
    std::free(reinterpret_cast<void*>(words[-1]));
}
void* allocateOrThrow(std::size_t size, std::size_t alignment) {
    if (size != 0 && size == failSize.load(std::memory_order_relaxed)) throw std::bad_alloc();
    if (auto* p = allocate(size, alignment)) return p;
    throw std::bad_alloc();
}
constexpr std::size_t plain = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

void* operator new(std::size_t n) { return allocateOrThrow(n, plain); }
void* operator new[](std::size_t n) { return allocateOrThrow(n, plain); }
void* operator new(std::size_t n, std::align_val_t a) { return allocateOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocateOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate(n, plain); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate(n, plain); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static void pump(int ms = 10) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
static double mib(std::int64_t bytes) { return static_cast<double>(bytes) / 1048576.0; }

struct Phase { const char* name; std::int64_t before{}, peak{}, after{}; };

// Runs `work` and records live memory before and after it and the highest live total during it.
template <class Work>
static Phase measure(const char* name, Work&& work) {
    Phase phase{name};
    phase.before = liveBytes.load();
    peakBytes.store(phase.before);
    work();
    phase.peak = peakBytes.load();
    phase.after = liveBytes.load();
    return phase;
}

static void trackerTests() {
    const auto before = liveBytes.load();
    peakBytes.store(before);
    auto* block = new std::uint8_t[1 << 20];
    struct alignas(64) Wide { std::array<std::uint8_t, 4096> bytes{}; };
    auto* wide = new Wide;
    require(reinterpret_cast<std::uintptr_t>(wide) % 64 == 0, "an aligned allocation keeps its alignment");
    require(liveBytes.load() - before >= (1 << 20) + 4096, "live bytes count plain and aligned allocations");
    delete wide; delete[] block;
    require(liveBytes.load() == before && peakBytes.load() - before >= (1 << 20) + 4096,
            "freeing returns live bytes to where they were and keeps the peak");
}

namespace {
constexpr int scenes = 32, notesPerClip = 64, audioTracks = 4, songBars = 8;
constexpr int clipSteps = 64;   // four bars

alien::PianoRollPattern clipNotes(int track, int scene) {
    alien::PianoRollPattern p;
    p.loopSteps = clipSteps;
    for (int n = 0; n < notesPerClip; ++n)
        p.add(36 + (track * 5 + scene * 3 + n * 7) % 60, static_cast<double>(n), 0.5);
    return p;
}

std::shared_ptr<alien::SessionAudioClip> toneClip(double seconds) {
    auto clip = std::make_shared<alien::SessionAudioClip>();
    clip->sampleRate = 48000; clip->channels = 2;
    clip->frames = static_cast<std::size_t>(seconds * 48000);
    clip->left.resize(clip->frames); clip->right.resize(clip->frames);
    for (std::size_t i = 0; i < clip->frames; ++i)
        clip->left[i] = clip->right[i] = 0.2f * static_cast<float>(std::sin(static_cast<double>(i) * 0.01));
    clip->buildWaveform();
    return clip;
}

void waitIdle(alien::ProjectController& c) {
    for (int i = 0; i < 3000 && c.isBusy(); ++i) pump();
    require(!c.isBusy(), "the controller finishes");
}

// A few callbacks, as a running device would: the engine adopts what was published and hands back what it
// replaced, which the message thread then frees. Without this, replaced snapshots wait in the mailboxes.
void settle(ToneEngine& engine, alien::ProjectController& c) {
    std::array<float, 512> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    for (int pass = 0; pass < 3; ++pass) {
        for (int b = 0; b < 4; ++b) engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        for (int i = 0; i < 5; ++i) pump();
        c.pushDocumentToEngine();
    }
    c.releaseReplacedAudio();   // what the app's timer does
}

bool writeProject(alien::ProjectController& c, const juce::File& file) {
    bool done = false, ok = false;
    c.writeProject(file, false, [&](bool r) { ok = r; done = true; });
    for (int i = 0; i < 3000 && !done; ++i) pump();
    return done && ok;
}

// What the app's Export WAV does, without plugins: a copy of the project and song, a render engine.
// With options.stems, a folder of stems; with options.freezeTrack, the track a freeze renders (T1-04 D19).
bool exportSong(const alien::ProjectController& c, const juce::File& file, alien::WavExportOptions options = {}) {
    auto song = c.captureArrangement();
    std::vector<std::shared_ptr<const alien::SessionAudioClip>> keepAlive;
    options.rackClips = c.rackClipsForExport(keepAlive);
    auto project = std::make_shared<alien::SessionProjectData>(alien::toProjectData(c.document()));
    auto engine = std::make_unique<ToneEngine>();
    engine->prepare(48000);
    alien::DrumSamplePointers drums{};
    std::atomic<bool> never{false};
    juce::String error;
    const bool ok = alien::exportArrangementWav(file, *project, *song, drums, never, error, engine.get(), options);
    if (!ok) std::cerr << "export: " << error << '\n';
    return ok;
}
}

static void projectPeakTests(bool report) {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-memory-peak", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    const auto song = root.getChildFile("Full.alienstudio");
    const auto engineOnly = liveBytes.load();
    std::vector<Phase> phases;
    std::int64_t builtProject = 0;
    {
        auto engine = std::make_unique<ToneEngine>();
        auto c = std::make_unique<alien::ProjectController>(*engine);
        phases.push_back(measure("Build a 16-track, 32-scene project (MIDI in every cell, 4 audio clips, 8-bar song)", [&] {
            while (c->document().trackCount() < alien::maxSessionTracks) require(c->addTrack(), "add a track");
            while (c->document().sceneCount() < std::min(scenes, alien::maxSessionScenes)) require(c->addScene(), "add a scene");
            const int sceneCount = c->document().sceneCount();
            for (int t = 0; t < alien::maxSessionTracks; ++t)
                for (int s = 0; s < sceneCount; ++s)
                    if (t < alien::maxSessionTracks - audioTracks || s != 1) c->setClipPattern(t, s, clipNotes(t, s), false);
            for (int a = 0; a < audioTracks; ++a) {
                const int t = alien::maxSessionTracks - audioTracks + a;
                const auto file = root.getChildFile("tone" + juce::String(a) + ".wav");
                auto clip = toneClip(10.0);
                std::atomic<bool> cancel{false}; juce::String error;
                require(alien::writeClipWav(file, *clip, cancel, error), "write an audio file");
                require(c->installAudio(file, t, 1, clip), "install an audio clip");
            }
            c->commit("Fill project");
            alien::command::CommandEngine commands(*c);
            require(commands.execute(alien::command::SetSongLength{songBars}).ok, "song length");
            for (int t = 0; t < alien::maxSessionTracks; ++t) {
                const int source = t >= alien::maxSessionTracks - audioTracks ? 1 : 0;
                require(c->placeRegion(t, source, 0), "place a Song clip");
                require(c->placeRegion(t, source, 4 * alien::timeline::ticksPerBar), "place a second Song clip");
            }
            c->pushDocumentToEngine();
        }));
        builtProject = liveBytes.load();
        phases.push_back(measure("Save the project", [&] { require(writeProject(*c, song), "save the full project"); }));
        phases.push_back(measure("20 edits, then 20 undo and 20 redo", [&] {
            for (int i = 0; i < 20; ++i) { c->setClipPattern(i % alien::maxSessionTracks, 2, clipNotes(i, 99), true); c->commit("Edit"); }
            for (int i = 0; i < 20; ++i) require(c->undo(), "undo an edit");
            for (int i = 0; i < 20; ++i) require(c->redo(), "redo an edit");
        }));
        phases.push_back(measure("Export the 8-bar song (render engine of its own)", [&] {
            require(exportSong(*c, root.getChildFile("Full.wav")), "export the full project");
        }));
        const auto afterExport = liveBytes.load();
        require(afterExport - phases.back().before < (1 << 20), "an export returns its render engine and copies when it ends");
        // T1-04 D19: stems and freeze, the other offline renders gate 1 names.
        phases.push_back(measure("Export stems of the 8-bar song (one file per track and bus)", [&] {
            alien::WavExportOptions options; options.stems = true;
            require(exportSong(*c, root.getChildFile("Stems"), options), "export stems of the full project");
        }));
        require(root.getChildFile("Stems").getNumberOfChildFiles(juce::File::findFiles, "*.wav") >= alien::maxSessionTracks,
                "a stem export writes a file for every track");
        require(liveBytes.load() - phases.back().before < (1 << 20), "a stem export returns its render engine and copies when it ends");
        // What the app's freeze does: render the track alone, decode the render, then install it with FreezeTrack.
        // Every cell of track 0 holds a clip and the Session is at its scene limit: the freeze is refused before
        // it renders, rather than after (T1-04 D19). Deleting the last scene (no Song clip plays it) makes room.
        if (c->document().sceneCount() == alien::maxSessionScenes) {
            require(c->freezeRefusal(0).contains("scene limit"), "a freeze with nowhere to put its audio was not refused before rendering");
            require(alien::command::CommandEngine(*c).execute(alien::command::DeleteScene{c->document().sceneCount() - 1}).changed,
                    "delete the last scene");
        }
        require(c->freezeRefusal(0).isEmpty(), "a freeze with a free cell was refused");
        const auto frozenFile = root.getChildFile("Freeze.wav");
        const auto target = alien::freeze_edit::capture(c->document(), 0);
        phases.push_back(measure("Freeze one track: render it alone (render engine of its own)", [&] {
            alien::WavExportOptions options; options.freezeTrack = 0; options.toTick = c->freezeEndTick(0);
            require(exportSong(*c, frozenFile, options), "render the track to freeze");
        }));
        require(phases.back().after - phases.back().before < (1 << 20), "a freeze render returns its render engine and copies when it ends");
        std::size_t frozenBytes = 0;
        phases.push_back(measure("Freeze one track: decode the render and install it", [&] {
            auto imported = alien::importSessionAudioClip(frozenFile);
            require(imported.clip != nullptr, "decode the freeze render");
            frozenBytes = imported.clip->decodedBytes();
            const auto installed = alien::command::CommandEngine(*c).execute(alien::command::FreezeTrack{target, frozenFile, std::move(imported.clip)});
            if (!installed.changed) std::cerr << "freeze: " << installed.error << '\n';
            require(installed.changed && c->isTrackFrozen(0), "install the frozen audio");
            c->pushDocumentToEngine();
        }));
        require(frozenBytes > 0 && phases.back().after - phases.back().before < static_cast<std::int64_t>(frozenBytes) + (1 << 20),
                "a freeze keeps only its frozen audio (and its undo step) once installed");
    }
    std::int64_t firstOpen = 0, settledReopen = 0;
    for (int pass = 0; pass < 2; ++pass) {
        auto engine = std::make_unique<ToneEngine>();
        engine->prepare(48000);
        auto c = std::make_unique<alien::ProjectController>(*engine);
        auto opened = measure(pass == 0 ? "Open the saved project (new engine and controller)" : "Open it again", [&] {
            c->openProject(song); waitIdle(*c); settle(*engine, *c);
        });
        require(c->document().trackCount() == alien::maxSessionTracks
                && c->document().clip(0, 0).notes().count == static_cast<std::size_t>(notesPerClip)
                && c->document().clip(alien::maxSessionTracks - 1, 1).isAudioClip(), "the project opens whole");
        if (pass == 1) {
            require(std::llabs((opened.after - opened.before) - firstOpen) < (1 << 20), "opening the same project twice holds the same memory");
            continue;
        }
        firstOpen = opened.after - opened.before;
        phases.push_back(opened);
        // T1-04 D5: the audio of the project an open replaces is freed once the engine has let go of it.
        for (int again = 0; again < 6; ++again) {
            const auto available = c->availableAudioBytes();
            auto reopened = measure(again == 0 ? "Open the same project over itself" : "Open it over itself a fourth time", [&] {
                c->openProject(song); waitIdle(*c); settle(*engine, *c);
            });
            require(c->availableAudioBytes() == available && c->replacedAudioBytes() == 0,
                    "an open over an open project frees the audio it replaced");
            // T1-04 D6: the replaced Song snapshots (which share the old document's note patterns) go too.
            require(reopened.after - reopened.before < (1 << 20), "an open over an open project keeps nothing once settled");
            settledReopen = std::max(settledReopen, reopened.after - reopened.before);
            require(reopened.after - opened.after < (1 << 20), "an open over an open project holds what a single open holds");
            if (again == 0 || again == 3) phases.push_back(reopened);
        }
    }
    require(liveBytes.load() - engineOnly < (1 << 20), "closing everything returns the memory");
    root.deleteRecursively();

    if (report) {
        std::cout << "# Peak heap during project operations\n\n"
                  << "Release build, compiled limits " << alien::maxSessionTracks << " tracks / " << alien::maxSessionScenes
                  << " scenes. Project: every track, " << std::min(scenes, alien::maxSessionScenes) << " scenes, "
                  << notesPerClip << " notes per MIDI clip, " << audioTracks << " ten-second stereo 48 kHz audio clips, "
                  << songBars << "-bar Song with two clips per track; no plugins.\n"
                  << "Live = C++ heap in use; peak = the highest live total during the step, on any thread.\n\n"
                  << "| Step | Live before (MiB) | Peak (MiB) | Peak above before (MiB) | Live after (MiB) |\n"
                  << "| --- | ---: | ---: | ---: | ---: |\n" << std::fixed << std::setprecision(1);
        for (const auto& p : phases)
            std::cout << "| " << p.name << " | " << mib(p.before) << " | " << mib(p.peak) << " | " << mib(p.peak - p.before)
                      << " | " << mib(p.after) << " |\n";
        std::cout << "\nLive with one engine, controller and the project: " << mib(builtProject) << " MiB.\n"
                  << "Once settled, an open over an open project keeps " << mib(settledReopen)
                  << " MiB at most: the audio and Song snapshots it replaced are freed once the engine has left them.\n"
                  << "Not counted: malloc storage (JUCE HeapBlock, such as plugin MIDI buffers), the OS, allocator overhead.\n";
    }
}

// T1-04 D5: replaced audio is freed only after the engine answers, and never while anything else owns it.
static void replacedAudioSafetyTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-audio-release", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    const auto song = root.getChildFile("Small.alienstudio");
    {
        auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
        auto c = std::make_unique<alien::ProjectController>(*engine);
        for (int t = 0; t < 2; ++t) {
            const auto file = root.getChildFile("clip" + juce::String(t) + ".wav");
            auto clip = toneClip(2.0);
            std::atomic<bool> cancel{false}; juce::String error;
            require(alien::writeClipWav(file, *clip, cancel, error) && c->installAudio(file, t, 0, clip), "install audio");
        }
        require(writeProject(*c, song), "save the small project");
    }
    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    auto c = std::make_unique<alien::ProjectController>(*engine);
    c->openProject(song); waitIdle(*c); settle(*engine, *c);
    std::weak_ptr<const alien::SessionAudioClip> played = c->document().clip(0, 0).audio, copied = c->document().clip(1, 0).audio;
    require(!played.expired() && !copied.expired(), "the opened project has its audio");
    require(c->copyCell(1, 0), "copy an audio clip");
    // Play the first clip, so the engine is reading it when the project is replaced.
    engine->patternMode.store(1);
    c->launchClip(0, 0);
    engine->playing.store(true);
    std::array<float, 512> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    double energy = 0;
    for (int b = 0; b < 8; ++b) {
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        for (float v : left) energy += static_cast<double>(v) * v;
    }
    require(energy > 0, "the audio clip is playing");
    const auto available = c->availableAudioBytes();

    c->openProject(song); waitIdle(*c);
    require(c->replacedAudioBytes() > 0 && c->releaseReplacedAudio() == 0 && !played.expired(),
            "with no callback since the open, the replaced audio waits");
    engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    require(engine->audioReleaseDone.load() == engine->audioReleaseRequest.load(), "one callback answers the request");
    require(c->releaseReplacedAudio() > 0 && played.expired() && c->replacedAudioBytes() == 0,
            "after the engine answers, the replaced audio is freed");
    require(!copied.expired(), "a clip on the clipboard is not freed");
    require(c->availableAudioBytes() <= available, "only freed audio is taken off the budget");
    require(alien::command::CommandEngine(*c).execute(alien::command::PasteClip{2, 0}).changed && c->document().clip(2, 0).audio.get() == copied.lock().get(), "the copied clip still pastes");
    for (int b = 0; b < 8; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});

    std::weak_ptr<const alien::SessionAudioClip> opened = c->document().clip(0, 0).audio;
    c->newProject();
    engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    require(c->releaseReplacedAudio() > 0 && opened.expired() && !copied.expired(), "File > New frees the replaced audio too");
    c.reset(); engine.reset();
    root.deleteRecursively();
}

// T1-04 D16: within a session, audio no undo or redo step, clipboard or cache can bring back is freed while the
// transport is stopped, once the engine has let go of it. Audio something can still reach stays.
static void unreachableAudioTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-unreachable-audio", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    auto c = std::make_unique<alien::ProjectController>(*engine);
    std::array<float, 512> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    auto callback = [&] { engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {}); };
    auto install = [&](int track, int scene) {
        const auto file = root.getChildFile("clip" + juce::String(track) + "-" + juce::String(scene) + ".wav");
        auto clip = toneClip(2.0);
        std::atomic<bool> cancel{false}; juce::String error;
        require(alien::writeClipWav(file, *clip, cancel, error) && c->installAudio(file, track, scene, clip), "install audio");
        return std::weak_ptr<const alien::SessionAudioClip>(c->document().clip(track, scene).audio);
    };
    while (c->document().trackCount() < 6) require(c->addTrack(), "add a track");
    c->commit("Tracks");
    std::uint32_t now = 1000000;
    // A sweep a second later, then callbacks, then the timer again: what the app does while stopped.
    auto sweep = [&] { now += 1500; c->releaseUnreachableAudio(now); callback(); return c->releaseUnreachableAudio(now); };
    settle(*engine, *c);
    const auto emptyBudget = c->availableAudioBytes();

    auto imported = install(0, 0);
    const auto clipBytes = imported.lock()->decodedBytes();
    require(sweep() == 0 && !imported.expired(), "audio in the document was freed");
    require(c->undo() && c->document().clip(0, 0).audio == nullptr, "undo the import");
    require(sweep() == 0 && !imported.expired(), "audio a redo can bring back was freed");
    require(c->redo() && c->document().clip(0, 0).audio.get() == imported.lock().get(), "redo brings the import back");
    require(c->undo(), "undo the import again");
    c->setClipPattern(1, 0, clipNotes(1, 0), true); c->commit("Edit");   // the redo step is gone
    // Not while playing: no sweep starts, and a sweep in progress waits for the stop.
    engine->playing.store(true);
    now += 1500;
    require(c->releaseUnreachableAudio(now) == 0 && c->unreachableAudioBytes() == 0, "a sweep ran while playing");
    engine->playing.store(false);
    now += 1500;
    require(c->releaseUnreachableAudio(now) == 0 && c->unreachableAudioBytes() == clipBytes && !imported.expired(),
            "audio no step can reach was not set aside, or was freed before the engine let go");
    engine->playing.store(true);
    callback(); callback();
    require(engine->editedAudioDone.load() < engine->editedAudioRequest.load(), "the engine let go of audio while playing");
    require(c->releaseUnreachableAudio(now) == 0 && !imported.expired(), "audio was freed while the engine was playing");
    engine->playing.store(false);
    callback();
    require(engine->editedAudioDone.load() == engine->editedAudioRequest.load(), "a stopped callback answers the request");
    const auto before = liveBytes.load();
    require(c->releaseUnreachableAudio(now) == clipBytes && imported.expired() && c->unreachableAudioBytes() == 0,
            "after the engine answers, audio no step can reach is freed");
    require(before - liveBytes.load() >= static_cast<std::int64_t>(clipBytes) - 4096, "freeing it did not return its memory");
    require(c->availableAudioBytes() == emptyBudget, "the audio budget did not get the freed audio back");

    // On the clipboard: kept, and it still pastes.
    auto copied = install(2, 0);
    require(c->copyCell(2, 0) && c->undo(), "copy, then undo the import");
    c->setClipPattern(1, 0, clipNotes(1, 1), true); c->commit("Edit");
    require(sweep() == 0 && !copied.expired() && c->unreachableAudioBytes() == 0, "a clip on the clipboard was freed");
    require(alien::command::CommandEngine(*c).execute(alien::command::PasteClip{3, 0}).changed && c->document().clip(3, 0).audio.get() == copied.lock().get(), "the copied clip still pastes");

    // An undo step that falls off the history: once the deleted clip's last step goes, so does its audio.
    auto deleted = install(4, 0);
    require(alien::command::CommandEngine(*c).execute(alien::command::DeleteClip{4, 0}).changed, "delete the clip");
    require(c->document().clip(4, 0).audio == nullptr, "the clip is deleted");
    require(sweep() == 0 && !deleted.expired(), "audio an undo can bring back was freed");
    for (int i = 0; i <= static_cast<int>(alien::DocumentHistory::limit); ++i) {
        c->setClipPattern(5, 0, clipNotes(5, i), true); c->commit("Edit");
    }
    sweep();
    require(deleted.expired() && !copied.expired(), "audio whose last undo step fell off the history was not freed");
    require(c->availableAudioBytes() == emptyBudget - clipBytes, "the budget counts only the audio still reachable");

    // A Channel Rack sample decoded again in place (no undo step): the old decode goes, the new one plays.
    const auto wav = root.getChildFile("rack.wav");
    { std::atomic<bool> cancel{false}; juce::String error; require(alien::writeClipWav(wav, *toneClip(1.0), cancel, error), "write a rack sample"); }
    alien::command::loadRackSample(*c, wav, 0); waitIdle(*c); settle(*engine, *c);
    const auto* oldDecode = c->rackChannelAudio(0);
    require(oldDecode != nullptr, "the rack sample loaded");
    const auto withRack = c->availableAudioBytes();
    require(sweep() == 0 && c->availableAudioBytes() == withRack, "a rack sample in use was freed");
    auto redecoded = toneClip(1.0);
    require(c->replaceRackSampleAudio(c->channelRack().info[0].sample, redecoded), "decode the rack sample again");
    settle(*engine, *c);
    require(c->availableAudioBytes() == withRack - redecoded->decodedBytes(), "the new decode is counted");
    const auto rackBefore = liveBytes.load();
    require(sweep() == redecoded->decodedBytes() && c->availableAudioBytes() == withRack, "a rack sample's replaced decode was not freed");
    require(rackBefore - liveBytes.load() >= static_cast<std::int64_t>(redecoded->decodedBytes()) - 4096, "the old decode's memory was not returned");
    require(c->rackChannelAudio(0) == redecoded.get() && !copied.expired(),
            "the new decode or other audio went too");

    // D17: the channel is given a different file. The old file stays while undo can bring it back, then goes.
    const std::weak_ptr<const alien::SessionAudioClip> firstFile = redecoded;
    const auto firstBytes = redecoded->decodedBytes();
    redecoded.reset();
    const auto other = root.getChildFile("rack-other.wav");
    { std::atomic<bool> cancel{false}; juce::String error; require(alien::writeClipWav(other, *toneClip(2.0), cancel, error), "write another rack sample"); }
    alien::command::loadRackSample(*c, other, 0); waitIdle(*c); settle(*engine, *c);
    const auto* otherDecode = c->rackChannelAudio(0);
    require(otherDecode != nullptr && otherDecode != firstFile.lock().get(), "the channel plays the other file");
    const auto withBoth = c->availableAudioBytes();
    require(withBoth == withRack - otherDecode->decodedBytes(), "both files are counted");
    require(sweep() == 0 && !firstFile.expired(), "a rack file an undo can bring back was freed");
    require(c->undo() && c->rackChannelAudio(0) == firstFile.lock().get() && !c->rackChannelOffline(0),
            "undo brings the first file back, still loaded");
    require(sweep() == 0 && !firstFile.expired() && c->rackChannelAudio(0) == firstFile.lock().get(), "the file in use was freed");
    require(c->redo() && c->rackChannelAudio(0) == otherDecode, "redo plays the other file again");
    for (int i = 0; i <= static_cast<int>(alien::DocumentHistory::limit); ++i) {
        c->setClipPattern(5, 0, clipNotes(5, i + 1000), true); c->commit("Edit");
    }
    const auto switchedBefore = liveBytes.load();
    require(sweep() == firstBytes && firstFile.expired(), "a rack file no undo step names was not freed");
    require(switchedBefore - liveBytes.load() >= static_cast<std::int64_t>(firstBytes) - 4096, "the old file's memory was not returned");
    require(c->availableAudioBytes() == withBoth + firstBytes, "the budget did not get the old file back");
    require(c->rackChannelAudio(0) == otherDecode && !c->rackChannelOffline(0) && !copied.expired(),
            "the file in use or other audio went too");
    require(sweep() == 0, "a second sweep freed something more");
    c.reset(); engine.reset();
    root.deleteRecursively();
    std::cout << "PASS: audio no edit can bring back (including a rack file switched away from) is freed while stopped\n";
}

// T1-04 D14: the Channel Rack's decoded samples, slice views and key maps belong to the open project. Opening
// another one (or the same one again) frees those the new project does not use, once the engine has let go.
static void rackCacheTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-rack-cache", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    // A sliced, key-zoned 10-second sample in one project, a short whole sample in the other.
    const auto make = [&](const juce::String& name, double seconds, bool cut) {
        const auto wav = root.getChildFile(name + ".wav"), song = root.getChildFile(name + ".alienstudio");
        std::atomic<bool> cancel{false}; juce::String error;
        require(alien::writeClipWav(wav, *toneClip(seconds), cancel, error), "write a rack sample");
        auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
        auto c = std::make_unique<alien::ProjectController>(*engine);
        c->pushDocumentToEngine();
        alien::command::loadRackSample(*c, wav, 0); waitIdle(*c);
        if (cut) {
            require(c->editRack("Slice", [](alien::rack::Project& p) { return p.sliceChannel(0, {0, 96000, 240000, 480000}); }), "slice the sample");
            require(c->editRack("Zones", [](alien::rack::Project& p) {
                return p.setKeyZones(0, {{0, 120000, 60, 60, 60}, {120000, 480000, 61, 61, 61}}); }), "put the sample on keys");
        }
        require(writeProject(*c, song), "save a rack project");
        return song;
    };
    const auto sliced = make("Sliced", 10.0, true), plain = make("Plain", 1.0, false);
    const auto sampleBytes = [](double seconds) { return static_cast<std::size_t>(seconds * 48000) * 2 * sizeof(float); };

    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    auto c = std::make_unique<alien::ProjectController>(*engine);
    const auto empty = c->availableAudioBytes();
    c->openProject(sliced); waitIdle(*c); settle(*engine, *c);
    require(c->channelRack().info[0].hasZones() && !c->channelRack().info[1].wholeSample(), "the sliced project opens with its zones and slices");
    require(c->rackChannelAudio(0) != nullptr && c->rackChannelAudio(0)->keyZones && c->rackChannelAudio(1)->sliceSource,
            "its channels play a key map and slice views");
    require(empty - c->availableAudioBytes() == sampleBytes(10.0), "the sliced project's sample is counted once");
    const auto slicedLive = liveBytes.load();

    // The same project again: the decode it replaces goes, with the views and key map made from it.
    for (int again = 0; again < 3; ++again) {
        c->openProject(sliced); waitIdle(*c); settle(*engine, *c);
        require(empty - c->availableAudioBytes() == sampleBytes(10.0), "reopening a rack project frees the sample it replaced");
        require(liveBytes.load() - slicedLive < (1 << 20), "reopening a rack project frees its old slice views and key map");
    }

    // Another project: the first one's sample is not used, so it goes, but only once the engine has let go.
    c->openProject(plain); waitIdle(*c);
    require(c->replacedAudioBytes() >= sampleBytes(10.0) && c->releaseReplacedAudio() == 0,
            "with no callback since the open, the replaced rack sample waits");
    settle(*engine, *c);
    require(c->replacedAudioBytes() == 0 && empty - c->availableAudioBytes() == sampleBytes(1.0),
            "opening another project frees the rack sample it no longer uses");
    require(slicedLive - liveBytes.load() > static_cast<std::int64_t>(sampleBytes(10.0)) - (1 << 20), "and its memory is returned");
    require(c->rackChannelAudio(0) != nullptr && !c->rackChannelOffline(0), "the new project's own sample stays");

    // File > New lets go of the last one too.
    c->newProject(); settle(*engine, *c);
    require(c->availableAudioBytes() == empty, "File > New frees the rack sample");
    c.reset(); engine.reset();
    root.deleteRecursively();
}

// T1-04 D7: when an engine's track slots cannot be prepared, the edit or open is refused with a reason, and
// the project, the engine and the track count stay as they were.
static void resourceRefusalTests() {
    const auto slotMemory = sizeof(alien::PianoRollMailbox);   // one of a track slot's objects, made only when it is reserved
    {
        auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
        require(engine->preparedTracks() == 4, "a new engine prepares the default four tracks");
        failSize.store(slotMemory);
        const bool grew = engine->setActiveTracks(8);
        failSize.store(0);
        require(!grew && engine->preparedTracks() == 4 && engine->activeTracks.load() == 4 && engine->reservedNoteSlots() == 4,
                "a slot that cannot be prepared is not counted, and the published count stays at the slots that exist");
        std::array<float, 512> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        require(engine->prepareTracks(8) && engine->preparedTracks() == 8 && engine->reservedNoteSlots() == 8,
                "once memory is available again, preparing carries on");
        require(engine->setActiveTracks(8) && engine->activeTracks.load() == 8, "and the count is published");
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    }
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-track-refusal", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    const auto eight = root.getChildFile("Eight.alienstudio");
    {
        auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
        auto c = std::make_unique<alien::ProjectController>(*engine);
        for (int t = 4; t < 8; ++t) require(c->addTrack(), "add a track");
        require(writeProject(*c, eight), "save an eight-track project");
    }
    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    auto c = std::make_unique<alien::ProjectController>(*engine);
    juce::String said, why;
    c->onProjectStatus = [&](const juce::String& text, const juce::String& tip) { said = text; why = tip; };
    const auto four = root.getChildFile("Four.alienstudio");
    require(writeProject(*c, four), "save a four-track project");
    c->openProject(four); waitIdle(*c);
    require(engine->pianoRollLaunches.reservedCount() == 4, "stopping playback for an open creates no launch mailbox for unused slots");
    const auto before = c->document().tracks.front().id;

    failSize.store(slotMemory);
    c->openProject(eight); waitIdle(*c);
    failSize.store(0);
    require(said.startsWith("Open failed: not enough memory for 8 tracks") && why.contains("could prepare 4 tracks"),
            "an open whose tracks cannot be prepared says why");
    require(c->document().trackCount() == 4 && c->document().tracks.front().id == before && engine->activeTracks.load() == 4,
            "and leaves the open project and the engine as they were");

    c->openProject(eight); waitIdle(*c); settle(*engine, *c);
    require(c->document().trackCount() == 8 && engine->preparedTracks() == 8, "with memory available, the same open succeeds");

    failSize.store(slotMemory);
    const bool added = c->addTrack();
    failSize.store(0);
    require(!added && c->document().trackCount() == 8 && said.startsWith("Could not add a track: not enough memory for 9 tracks"),
            "adding a track whose slot cannot be prepared is refused with a reason");
    require(c->addTrack() && c->document().trackCount() == 9 && engine->preparedTracks() == 9, "and succeeds once memory is available");
    c->onProjectStatus = nullptr;
    c.reset(); engine.reset();
    root.deleteRecursively();
}

// T1-04 D8: the Channel Rack's pending-hit queue is reserved a channel at a time, before a snapshot with
// those channels is published, and an open whose channels cannot have it is refused.
static void rackQueueTests() {
    const auto chunk = alien::rack::RackPlayer::pendingBytesPerChannel();
    const int defaultChannels = alien::rack::Project::createDefault().channelCount;
    std::array<float, 512> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    {
        const auto before = liveBytes.load();
        auto engine = std::make_unique<ToneEngine>();
        const auto made = liveBytes.load() - before;
        require(engine->preparedRackChannels() == defaultChannels, "a new engine reserves the default rack's channels");
        std::cout << "Engine heap after construction: " << std::fixed << std::setprecision(1)
                  << static_cast<double>(made) / (1024.0 * 1024.0) << " MiB (rack hit queue "
                  << static_cast<double>(chunk * static_cast<std::size_t>(defaultChannels)) / (1024.0 * 1024.0) << " MiB of it)\n";
        require(made < 8 * 1024 * 1024, "the engine no longer holds every rack channel's hit queue");
        engine->prepare(48000);
        auto rack = alien::rack::Project::createDefault();
        while (rack.channelCount < 12) rack.addChannel("x", "");
        engine->rackMailbox.publish(rack.snapshot());
        require(engine->preparedRackChannels() == 12, "publishing a larger rack reserves its channels first");
        const auto settled = liveBytes.load();
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
        require(liveBytes.load() == settled, "the callback adopts the new room without allocating");
    }
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-rack-refusal", "");
    require(root.createDirectory().wasOk(), "scratch folder");
    const auto big = root.getChildFile("BigRack.alienstudio");
    {
        auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
        auto c = std::make_unique<alien::ProjectController>(*engine);
        while (c->document().channelRack.channelCount < 10)
            require(c->editRack("Add", [](alien::rack::Project& p) { return p.addChannel("x", ""); }), "add a rack channel");
        require(writeProject(*c, big), "save a ten-channel project");
    }
    auto engine = std::make_unique<ToneEngine>(); engine->prepare(48000);
    auto c = std::make_unique<alien::ProjectController>(*engine);
    juce::String said, why;
    c->onProjectStatus = [&](const juce::String& text, const juce::String& tip) { said = text; why = tip; };
    const auto before = c->document().tracks.front().id;

    failSize.store(chunk);
    c->openProject(big); waitIdle(*c);
    failSize.store(0);
    require(said.startsWith("Open failed: not enough memory for 10 Channel Rack channels") && why.contains("could prepare " + juce::String(defaultChannels)),
            "an open whose rack channels cannot be prepared says why");
    require(c->document().channelRack.channelCount == defaultChannels && c->document().tracks.front().id == before,
            "and leaves the open project as it was");

    c->openProject(big); waitIdle(*c); settle(*engine, *c);
    require(c->document().channelRack.channelCount == 10 && engine->preparedRackChannels() == 10, "with memory available, the same open succeeds");

    said.clear();
    failSize.store(chunk);
    const bool added = c->editRack("Add", [](alien::rack::Project& p) { return p.addChannel("x", ""); });
    failSize.store(0);
    require(added && c->document().channelRack.channelCount == 11 && engine->preparedRackChannels() == 10
                && said.startsWith("Not enough memory for 11 Channel Rack channels"),
            "a rack edit whose channel cannot be reserved still happens, and says so");
    engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    require(c->editRack("Add", [](alien::rack::Project& p) { return p.addChannel("x", ""); }) && engine->preparedRackChannels() == 12,
            "the next edit reserves what was missing");
    c->onProjectStatus = nullptr;
    c.reset(); engine.reset();
    root.deleteRecursively();
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    const bool report = argc == 2 && std::string_view(argv[1]) == "--report";
    trackerTests();
    replacedAudioSafetyTests();
    rackCacheTests();
    unreachableAudioTests();
    resourceRefusalTests();
    rackQueueTests();
    projectPeakTests(report);
    std::cout << "PASS: peak heap tracking and memory returned after save, open, undo/redo and export\n";
    return 0;
}
