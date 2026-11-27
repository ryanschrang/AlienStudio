// AlienDeviceTiming: times the shared playback callback on a real audio device (T1-04 D31,
// measurement gate 2 in docs/UNIVERSAL_TRACKS.md). It plays the capacity report's workloads one
// at a time through the chosen output device, records how long each callback took and how far
// apart callbacks arrived, and prints a Markdown table. The output is muted unless --audible is
// given: the engine still renders everything, the device just receives silence.
//
// The capacity report (RackIntegrationTests --capacity-report) times the same work offline. This
// tool is what measures it against a device's real deadlines.
#include "CallbackTiming.h"
#include "CapacityWorkloads.h"
#include "DiskAudioFile.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Allocations made on the audio thread while a workload is being recorded (AGENTS.md: none).
static thread_local bool inAudioCallback = false;
static std::atomic<std::size_t> callbackAllocations{0};
static std::atomic<bool> countingAllocations{false};
void* operator new(std::size_t size) {
    if (inAudioCallback && countingAllocations.load(std::memory_order_relaxed)) callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

double nowUs() {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Times the engine's callback and, unless audible, hands the device silence.
class TimedEngine final : public juce::AudioIODeviceCallback {
public:
    explicit TimedEngine(alien::CallbackTimingRecorder& r) : recorder(r) {}
    ToneEngine* engine{};
    bool audible{};
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputCount, float* const* outputs, int outputCount,
                                          int samples, const juce::AudioIODeviceCallbackContext& context) override {
        inAudioCallback = true;
        const double start = nowUs();
        if (engine != nullptr) engine->audioDeviceIOCallbackWithContext(inputs, inputCount, outputs, outputCount, samples, context);
        recorder.record(start, nowUs() - start);
        inAudioCallback = false;
        if (!audible || engine == nullptr)
            for (int c = 0; c < outputCount; ++c) if (outputs[c] != nullptr) juce::FloatVectorOperations::clear(outputs[c], samples);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override { if (engine != nullptr) engine->audioDeviceAboutToStart(device); }
    void audioDeviceStopped() override { if (engine != nullptr) engine->audioDeviceStopped(); }
    void audioDeviceError(const juce::String& message) override { std::cerr << "device error: " << message << '\n'; }
private:
    alien::CallbackTimingRecorder& recorder;
};

struct Options {
    bool list{}, audible{}, inputs{};
    juce::String type, device, plugin;
    double rate{};
    std::vector<int> buffers;
    double seconds{5.0}, warmup{1.0};
    std::vector<int> workloads;   // indices into alien::capacity::cases(), empty for all
};

std::vector<int> numbers(const std::string& text) {
    std::vector<int> out; std::stringstream in(text); std::string item;
    while (std::getline(in, item, ',')) {
        try { std::size_t used = 0; const int v = std::stoi(item, &used); if (used != item.size()) return {}; out.push_back(v); }
        catch (...) { return {}; }
    }
    return out;
}

std::optional<Options> parse(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::optional<std::string> { if (i + 1 >= argc) return std::nullopt; return std::string(argv[++i]); };
        if (arg == "--list") o.list = true;
        else if (arg == "--audible") o.audible = true;
        else if (arg == "--inputs") o.inputs = true;
        else if (arg == "--plugin") { const auto v = value(); if (!v) return std::nullopt; o.plugin = *v; }
        else if (arg == "--type") { const auto v = value(); if (!v) return std::nullopt; o.type = *v; }
        else if (arg == "--device") { const auto v = value(); if (!v) return std::nullopt; o.device = *v; }
        else if (arg == "--rate") { const auto v = value(); if (!v) return std::nullopt; o.rate = std::atof(v->c_str()); if (o.rate <= 0) return std::nullopt; }
        else if (arg == "--buffers") { const auto v = value(); if (!v) return std::nullopt; o.buffers = numbers(*v); if (o.buffers.empty()) return std::nullopt; }
        else if (arg == "--seconds") { const auto v = value(); if (!v) return std::nullopt; o.seconds = std::atof(v->c_str()); if (o.seconds < 0.5 || o.seconds > 600) return std::nullopt; }
        else if (arg == "--warmup") { const auto v = value(); if (!v) return std::nullopt; o.warmup = std::atof(v->c_str()); if (o.warmup < 0 || o.warmup > 60) return std::nullopt; }
        else if (arg == "--workloads") { const auto v = value(); if (!v) return std::nullopt; o.workloads = numbers(*v); if (o.workloads.empty()) return std::nullopt; }
        else return std::nullopt;
    }
    for (const int b : o.buffers) if (b < 16 || b > alien::PluginRuntime::blockSize * 16) return std::nullopt;
    return o;
}

void usage(const std::vector<alien::capacity::Case>& cases) {
    std::cerr << "usage: AlienDeviceTiming [--list] [--type NAME] [--device NAME] [--rate HZ] [--buffers 64,128,...]\n"
                 "                         [--seconds S] [--warmup S] [--workloads 0,1,...] [--audible] [--inputs]\n"
                 "                         [--plugin FILE.vst3]\n"
                 "  --list       list the device types and output devices, then exit\n"
                 "  --type       device type, e.g. \"Windows Audio\" or \"ASIO\" (default: the system default)\n"
                 "  --device     output device name (default: the type's default)\n"
                 "  --rate       sample rate (default: the device's current rate)\n"
                 "  --buffers    buffer sizes to time, one after another (default: the device's current size)\n"
                 "  --seconds    seconds recorded per workload and buffer size (default 5)\n"
                 "  --warmup     seconds played before recording starts (default 1)\n"
                 "  --workloads  which workloads, by number (default: all)\n"
                 "  --audible    send the rendered audio to the device (default: the device plays silence)\n"
                 "  --inputs     also open the type's default input device (two channels), for the input-monitoring\n"
                 "               workload; without it that workload is skipped. With --audible as well, the input is\n"
                 "               played back out loud: a microphone next to the speakers will feed back\n"
                 "  --plugin     a VST3 effect to put on every playing track, for the VST3 workload; without it\n"
                 "               that workload is skipped. It is loaded in this process, with no crash protection\n\nWorkloads:\n";
    for (std::size_t i = 0; i < cases.size(); ++i) std::cerr << "  " << i << "  " << cases[i].name << " (" << cases[i].tracks << " tracks in use)\n";
}

void wait(double seconds) { juce::MessageManager::getInstance()->runDispatchLoopUntil(static_cast<int>(seconds * 1000.0)); }

// Plays on for `seconds`, emptying a recording row's take queues every 10 ms, as the app's recording
// thread does (T1-04 D34); for any other row there is nothing to empty.
void playFor(alien::capacity::Rig& rig, double seconds) {
    const double end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    do { wait(0.01); rig.drainTakes(); } while (juce::Time::getMillisecondCounterHiRes() < end);
}

// T1-04 D32: the streamed workload plays a real file from disk. Writes `seconds` of a quiet stereo tone
// as 16-bit, 48 kHz WAV, a block at a time, and returns it as a clip that streams from that file, as a
// long import would.
std::shared_ptr<const alien::SessionAudioClip> writeStreamedClip(const juce::File& file, double seconds, juce::String& error) {
    constexpr double rate = 48000.0;
    constexpr int block = 512;
    const auto frames = static_cast<std::int64_t>(seconds * rate);
    {
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        if (!stream) { error = "cannot create " + file.getFullPathName(); return {}; }
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(16));
        if (!writer) { error = "cannot write WAV to " + file.getFullPathName(); return {}; }
        std::array<float, block> left{}, right{};
        for (std::int64_t done = 0; done < frames; done += block) {
            const int count = static_cast<int>(std::min<std::int64_t>(block, frames - done));
            for (int i = 0; i < count; ++i)
                left[static_cast<std::size_t>(i)] = right[static_cast<std::size_t>(i)] =
                    0.1f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * static_cast<double>(done + i) / rate));
            const float* data[]{left.data(), right.data()};
            if (!writer->writeFromFloatArrays(data, 2, count)) { error = "cannot write " + file.getFullPathName(); return {}; }
        }
        if (!writer->flush()) { error = "cannot finish " + file.getFullPathName(); return {}; }
    }
    auto source = alien::JuceDiskAudioFile::inspect(file, error);
    if (!source) return {};
    auto clip = std::make_shared<alien::SessionAudioClip>();
    clip->sampleRate = source->sampleRate; clip->channels = source->channels;
    clip->frames = static_cast<std::size_t>(source->frames); clip->streamSource = std::move(source);
    if (!clip->valid() || !clip->streamed()) { error = "the streamed clip is not valid"; return {}; }
    return clip;
}

void listDevices(juce::AudioDeviceManager& manager) {
    for (auto* type : manager.getAvailableDeviceTypes()) {
        type->scanForDevices();
        std::cout << type->getTypeName() << '\n';
        for (const auto& name : type->getDeviceNames(false)) std::cout << "  " << name << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto cases = alien::capacity::cases(static_cast<int>(alien::maxSessionTracks));
    const auto options = parse(argc, argv);
    if (!options) { usage(cases); return 2; }
    for (const int w : options->workloads) if (w < 0 || w >= static_cast<int>(cases.size())) { usage(cases); return 2; }

    juce::AudioDeviceManager manager;
    if (options->list) { listDevices(manager); return 0; }
    if (const auto error = manager.initialise(options->inputs ? 2 : 0, 2, nullptr, true); error.isNotEmpty()) { std::cerr << "cannot open audio: " << error << '\n'; return 1; }
    if (options->type.isNotEmpty()) {
        manager.setCurrentAudioDeviceType(options->type, true);
        if (manager.getCurrentAudioDeviceType() != options->type) { std::cerr << "no device type \"" << options->type << "\" (try --list)\n"; return 1; }
    }
    auto setup = manager.getAudioDeviceSetup();
    if (options->device.isNotEmpty()) setup.outputDeviceName = options->device;
    if (options->rate > 0) setup.sampleRate = options->rate;
    // T1-04 D33: inputs only when asked for (--inputs), the first two of the type's default input device.
    if (options->inputs) { setup.useDefaultInputChannels = false; setup.inputChannels.clear(); setup.inputChannels.setRange(0, 2, true); }
    else { setup.inputDeviceName = {}; setup.useDefaultInputChannels = false; setup.inputChannels.clear(); }
    if (const auto error = manager.setAudioDeviceSetup(setup, true); error.isNotEmpty()) { std::cerr << "cannot open the device: " << error << '\n'; return 1; }
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr) { std::cerr << "no output device is open (try --list)\n"; return 1; }
    auto buffers = options->buffers;
    if (buffers.empty()) buffers.push_back(device->getCurrentBufferSizeSamples());

    alien::capacity::Media media;
    if (!media.valid()) { std::cerr << "workload media could not be built\n"; return 1; }
    std::vector<int> chosen = options->workloads;
    if (chosen.empty()) for (std::size_t i = 0; i < cases.size(); ++i) chosen.push_back(static_cast<int>(i));
    // T1-04 D32: the file the streamed row plays, in the temporary folder, removed when the tool ends.
    // It lasts through warm-up, up to 3 s waiting for the clips to start, and recording, with 2 s to spare.
    std::optional<juce::TemporaryFile> streamedFile;
    if (std::any_of(chosen.begin(), chosen.end(), [&](int i) { return cases[static_cast<std::size_t>(i)].kind == 9; })) {
        streamedFile.emplace(".wav");
        juce::String error;
        media.streamed = writeStreamedClip(streamedFile->getFile(), options->warmup + 3.0 + options->seconds + 2.0, error);
        if (!media.streamed) { std::cerr << "streamed workload: " << error << '\n'; return 1; }
    }
    // T1-04 D36: the VST3 the plugin row puts on every playing track, one instance each. The format and
    // the description outlive every rig, so every instance is gone before they are.
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> effectTypes;
    if (options->plugin.isNotEmpty()) {
        const juce::File file(juce::File::getCurrentWorkingDirectory().getChildFile(options->plugin));
        if (!file.exists()) { std::cerr << "no plugin at " << file.getFullPathName() << '\n'; return 1; }
        vst3.findAllTypesForFile(effectTypes, file.getFullPathName());
        if (effectTypes.isEmpty()) { std::cerr << "no VST3 plugin found in " << file.getFullPathName() << '\n'; return 1; }
        const auto& type = *effectTypes[0];
        media.effectName = type.name;
        media.makeEffect = [&vst3, &type]() -> std::unique_ptr<juce::AudioProcessor> {
            juce::String error;
            auto instance = vst3.createInstanceFromDescription(type, 48000.0, alien::PluginRuntime::blockSize, error);
            if (!instance) std::cerr << type.name << ": " << error << '\n';
            return instance;   // prepared again at the device's rate when its slot is
        };
    }
    // T1-04 D35: the folder the disk-recording row writes its takes (and their recovery journals) into,
    // in the temporary folder and removed, with everything in it, when the tool ends.
    struct RemovedAtExit { juce::File folder; ~RemovedAtExit() { if (folder != juce::File()) folder.deleteRecursively(); } } recordings;
    if (std::any_of(chosen.begin(), chosen.end(), [&](int i) { return cases[static_cast<std::size_t>(i)].kind == 12; })) {
        recordings.folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienDeviceTiming takes", {}, false);
        if (!recordings.folder.createDirectory().wasOk()) { std::cerr << "cannot create " << recordings.folder.getFullPathName() << '\n'; return 1; }
        media.recordingFolder = recordings.folder;
    }

    std::cout << "# Shared playback callback on an audio device\n\n"
        << "| Setting | Value |\n| --- | --- |\n"
        << "| Device type | " << device->getTypeName() << " |\n"
        << "| Output device | " << device->getName() << " |\n"
        << "| Sample rate | " << device->getCurrentSampleRate() << " Hz |\n"
        << "| CPU | " << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumPhysicalCpus() << " cores / "
        << juce::SystemStats::getNumCpus() << " threads |\n"
        << "| OS | " << juce::SystemStats::getOperatingSystemName() << " |\n"
        << "| Build | " << alien::maxSessionTracks << " tracks / " << alien::maxSessionScenes << " scenes compiled |\n"
        << "| Output | " << (options->audible ? "audible" : "muted (rendered, then silenced)") << " |\n"
        << "| Input | " << (options->inputs ? manager.getAudioDeviceSetup().inputDeviceName + ", " + juce::String(device->getActiveInputChannels().countNumberOfSetBits()) + " channels open"
                                             : juce::String("none (input-monitoring rows need --inputs)")) << " |\n"
        << "| Plugin | " << (media.makeEffect ? media.effectName + " (" + options->plugin + ")" : juce::String("none (the VST3 row needs --plugin)")) << " |\n"
        << "| Recorded | " << options->seconds << " s per row, after " << options->warmup << " s of warm-up |\n\n"
        << "Durations are the engine's whole callback, timed inside the device callback. Late: a callback that\n"
        << "started more than 1.5 buffer periods after the one before. Over budget: one that took longer than a\n"
        << "buffer period. Xruns: the device's own count, where the driver reports one. Other desktop activity was\n"
        << "not controlled; one run is an observation on this machine, not a guarantee.\n\n"
        << "| Workload | Tracks in use | Buffer frames | Callbacks | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late | Longest gap us | Xruns | Callback allocations |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n"
        << std::fixed << std::setprecision(1);

    int failures = 0;
    std::vector<int> timedSizes;   // a size the device substituted for another is timed once
    for (const int frames : buffers) {
        setup = manager.getAudioDeviceSetup();
        setup.bufferSize = frames;
        if (const auto error = manager.setAudioDeviceSetup(setup, true); error.isNotEmpty()) {
            std::cerr << "buffer size " << frames << " refused: " << error << '\n'; ++failures; continue;
        }
        device = manager.getCurrentAudioDevice();
        if (device == nullptr) { std::cerr << "the device closed\n"; return 1; }
        const int actual = device->getCurrentBufferSizeSamples();
        const double rate = device->getCurrentSampleRate();
        if (actual != frames) std::cerr << "asked for " << frames << " frames, the device uses " << actual << '\n';
        if (std::find(timedSizes.begin(), timedSizes.end(), actual) != timedSizes.end()) {
            std::cerr << actual << " frames already timed; skipped\n"; continue;
        }
        timedSizes.push_back(actual);
        const double periodUs = actual * 1.0e6 / rate;
        alien::CallbackTimingRecorder recorder(static_cast<std::size_t>(options->seconds * rate / std::max(1, actual) * 2.0) + 256);
        TimedEngine timed(recorder);
        timed.audible = options->audible;
        for (const int index : chosen) {
            const auto& c = cases[static_cast<std::size_t>(index)];
            if (alien::capacity::monitorsInput(c) && !options->inputs) { std::cerr << c.name << ": skipped (needs --inputs)\n"; continue; }
            if (c.kind == 13 && !media.makeEffect) { std::cerr << c.name << ": skipped (needs --plugin)\n"; continue; }
            auto rig = std::make_unique<alien::capacity::Rig>(rate);
            if (const char* failed = alien::capacity::configure(*rig, c, media)) { std::cerr << c.name << ": " << failed << '\n'; ++failures; continue; }
            timed.engine = &rig->engine;
            manager.addAudioCallback(&timed);   // prepares the engine at the device's rate, as the app does
            alien::capacity::begin(*rig, c);   // after: preparing refuses a take asked for before it (D34)
            playFor(*rig, options->warmup);
            // Record only once the row is playing: a streamed clip starts when its first audio is read (D32).
            for (int waited = 0; !alien::capacity::started(*rig, c) && waited < 3000; waited += 10) playFor(*rig, 0.01);
            const int xrunsBefore = device->getXRunCount();
            callbackAllocations.store(0); countingAllocations.store(true);
            recorder.arm();
            playFor(*rig, options->seconds);
            recorder.disarm();
            countingAllocations.store(false);
            const int xrunsAfter = device->getXRunCount();
            playFor(*rig, std::max(0.05, 4.0 * periodUs / 1.0e6));   // a callback inside record() finishes its entry
            const char* stopped = alien::capacity::stillPlaying(*rig, c);
            manager.removeAudioCallback(&timed);
            timed.engine = nullptr;
            // D35: with no callback left to race the writers, the disk takes are finished and checked.
            const char* unfinished = rig->finishDiskTakes();
            const auto s = recorder.summarise(periodUs);
            std::cout << "| " << c.name << " | " << c.tracks << " | " << actual << " | " << s.callbacks << " | " << s.p50 << " | " << s.p95
                << " | " << s.p99 << " | " << s.max << " | " << s.p95BudgetPercent() << " | " << s.overBudget << " | " << s.late << " | "
                << s.maxIntervalUs << " | " << (xrunsBefore < 0 || xrunsAfter < 0 ? std::string("n/a") : std::to_string(xrunsAfter - xrunsBefore))
                << " | " << callbackAllocations.load() << " |" << std::endl;
            if (s.dropped != 0) std::cerr << c.name << ": " << s.dropped << " callbacks arrived with the recorder full\n";
            if (s.callbacks == 0) { std::cerr << c.name << ": no callbacks arrived\n"; ++failures; }
            if (stopped != nullptr) { std::cerr << c.name << ": " << stopped << '\n'; ++failures; }
            if (unfinished != nullptr) { std::cerr << c.name << ": " << unfinished << '\n'; ++failures; }
        }
    }
    manager.closeAudioDevice();
    return failures == 0 ? 0 : 1;
}
