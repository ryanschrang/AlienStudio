#include "CommandEngine.h"
#include "WavExport.h"
#include "AudioFileWrite.h"
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
bool exportSong(const alien::ProjectController& c, const juce::File& file) {
    auto song = c.captureArrangement();
    alien::WavExportOptions options;
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
    require(c->pasteCell(2, 0, false) && c->document().clip(2, 0).audio.get() == copied.lock().get(), "the copied clip still pastes");
    for (int b = 0; b < 8; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});

    std::weak_ptr<const alien::SessionAudioClip> opened = c->document().clip(0, 0).audio;
    c->newProject();
    engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    require(c->releaseReplacedAudio() > 0 && opened.expired() && !copied.expired(), "File > New frees the replaced audio too");
    c.reset(); engine.reset();
    root.deleteRecursively();
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    const bool report = argc == 2 && std::string_view(argv[1]) == "--report";
    trackerTests();
    replacedAudioSafetyTests();
    projectPeakTests(report);
    std::cout << "PASS: peak heap tracking and memory returned after save, open, undo/redo and export\n";
    return 0;
}
