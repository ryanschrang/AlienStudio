// Export and freeze with plugins in the chain (T1-09 part 7): a song with a latent plugin instrument and a latent,
// automated effect on a track, a latent instrument on a Channel Rack channel and a latent master effect renders to
// WAV exactly as it plays live (sample for sample, after the mix's latency), and a frozen track holds exactly what
// that track plays live before its fader. Rack instruments are told they are rendering offline, as track plugins are.
#include "PluginHost.h"
#include "ToneEngine.h"
#include "WavExport.h"
#include <cstdlib>
#include <iostream>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

// Shared by the test plugins: whether one was ever asked to process while rendering offline.
static bool sawOffline(const juce::AudioProcessor& p, bool& flag) { flag = flag || p.isNonRealtime(); return flag; }

// An effect with a lookahead: it delays its input by `latency` samples (and says so) and scales it by an automatable
// "gain". If the host did not make up for the delay, its track would arrive late.
struct LatentEffect final : juce::AudioProcessor {
    explicit LatentEffect(int samples)
        : juce::AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())), latency(samples) {
        addParameter(gain = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, 1.0f));
        setLatencySamples(latency);
    }
    const juce::String getName() const override { return "Latent effect"; }
    void prepareToPlay(double, int) override { for (auto& line : lines) line.assign(static_cast<std::size_t>(latency + 1), 0.0f); position = 0; }
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        sawOffline(*this, offline);
        const float g = gain->get();
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const auto read = (position + 1) % lines[0].size();
            for (int c = 0; c < 2; ++c) {
                lines[static_cast<std::size_t>(c)][position] = buffer.getSample(c, i);
                buffer.setSample(c, i, lines[static_cast<std::size_t>(c)][read] * g);
            }
            position = read;
        }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    int latency{};
    std::array<std::vector<float>, 2> lines;
    std::size_t position{};
    juce::AudioParameterFloat* gain{};
    bool offline{};
};

// An instrument with a lookahead: each note starts a short, deterministic decaying ramp `latency` samples late
// (left and right differ, so a swapped or mono path shows).
struct LatentInstrument final : juce::AudioProcessor {
    explicit LatentInstrument(int samples) : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo())), latency(samples) {
        setLatencySamples(latency);
    }
    const juce::String getName() const override { return "Latent instrument"; }
    void prepareToPlay(double, int) override { count = 0; rendered = 0; }
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        sawOffline(*this, offline);
        for (const auto meta : midi)
            if (const auto m = meta.getMessage(); m.isNoteOn() && count < starts.size())
                starts[count++] = {rendered + meta.samplePosition + latency, m.getFloatVelocity()};
        buffer.clear();
        for (std::size_t n = 0; n < count; ++n)
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                const auto age = rendered + i - starts[n].first;
                if (age < 0 || age >= 4800) continue;
                const float v = 0.2f * starts[n].second * (1.0f - static_cast<float>(age) / 4800.0f);
                buffer.addSample(0, i, v); buffer.addSample(1, i, -0.5f * v);
            }
        rendered += buffer.getNumSamples();
    }
    double getTailLengthSeconds() const override { return 0.1; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    int latency{};
    std::array<std::pair<std::int64_t, float>, 256> starts{};
    std::size_t count{};
    std::int64_t rendered{};
    bool offline{};
};

static juce::String description(const juce::String& id, bool instrument) {
    juce::PluginDescription d; d.name = id; d.pluginFormatName = "VST3"; d.fileOrIdentifier = id; d.isInstrument = instrument;
    return d.createXml()->toString();
}
// "fx:N" and "synth:N", with N samples of latency.
static PluginFactory factory() {
    return [](const juce::PluginDescription& d, double, int, juce::String&) -> std::unique_ptr<juce::AudioProcessor> {
        const int latency = d.fileOrIdentifier.fromFirstOccurrenceOf(":", false, false).getIntValue();
        if (d.fileOrIdentifier.startsWith("fx")) return std::make_unique<LatentEffect>(latency);
        return std::make_unique<LatentInstrument>(latency);
    };
}

// The song: two bars. Track 1 plays notes on a latent instrument through a latent effect whose gain is automated
// from 1 down to 0.25; the Channel Rack plays a latent instrument on the master on bar 2; the master has a latent
// effect. `rack` and `masterFx` leave those out (a freeze renders the track alone).
struct Song {
    std::unique_ptr<SessionProjectData> project = std::make_unique<SessionProjectData>();
    std::unique_ptr<ArrangementPlayback> playback = std::make_unique<ArrangementPlayback>();
    std::uint64_t effectId{};
    static constexpr std::uint64_t rackInstance = 900;
    Song(bool rack, bool masterFx) {
        auto& p = *project;
        p.bpm = 120;
        p.plugins[0].instrument = PluginState(description("synth:200", true), {});
        p.plugins[0].effects = {PluginState(description("fx:300", false), {})};
        effectId = p.plugins[0].effects[0].instanceId;
        if (masterFx) p.masterPlugins.effects = {PluginState(description("fx:50", false), {})};
        require(p.channelRack.setPlugin(0, description("synth:100", true).toStdString(), rackInstance), "the rack channel gets an instrument");
        for (int s = 0; s < 16; s += 4) p.channelRack.setStepOn(0, 0, s, rack);
        auto& t = playback->timeline;
        t.setLengthBars(2); t.tempo = 120;
        require(t.placeBars(0, 0, 0, 2), "the track's clip spans the song");
        if (rack) {
            timeline::Region region; region.row = timeline::rackRowBase; region.source = 0; region.start = timeline::ticksPerBar; region.length = timeline::ticksPerBar;
            require(t.place(region), "the rack pattern is on bar 2");
        }
        PianoRollPattern notes; notes.loopSteps = 32;
        for (int s = 0; s < 32; s += 3) notes.add(60 + s % 7, s, 1, 0.5f + 0.015f * static_cast<float>(s));
        playback->clips[0][0].pattern = notes;
        p.clips[0][0].pattern = notes;
        automation::ParameterLane lane; lane.target = {effectId, "gain"};
        lane.put({0, 1.0f, automation::Curve::linear}); lane.put({2 * timeline::ticksPerBar, 0.25f, automation::Curve::linear});
        t.parameterLanes.push_back(lane);
        p.timeline = t;
        require(p.valid(), "the project is valid");
    }
};

// An engine with the song's plugins, the way the app prepares one (racks, then the Channel Rack's instruments).
struct Rig {
    std::unique_ptr<ToneEngine> engine = std::make_unique<ToneEngine>();
    std::unique_ptr<PluginHost> host;
    Rig(const Song& song, bool onlyTrack = false) {
        engine->prepare(48000);
        host = std::make_unique<PluginHost>(*engine->plugins, juce::File{}, factory());
        auto racks = song.project->plugins;
        require(host->restore(racks, onlyTrack ? PluginRackState{} : song.project->masterPlugins, 48000).isEmpty(), "the mixer's plugins load");
        std::vector<PluginState> channels(1);
        channels[0].descriptionXml = description("synth:100", true); channels[0].instanceId = Song::rackInstance;
        require(host->restoreRackChannels(channels, 48000).isEmpty(), "the rack instrument loads");
    }
    // Plays the song live, from the start, and returns the master output.
    std::vector<std::array<float, 2>> playLive(const Song& song, int frames) {
        engine->activeTracks.store(song.project->trackCount());
        engine->bpm.store(song.project->bpm);
        engine->rackMailbox.publish(song.project->channelRack.snapshot());
        require(engine->arrangementMailbox.publish(*song.playback), "the song is accepted");
        engine->patternMode.store(2);
        engine->playing.store(true);
        std::vector<std::array<float, 2>> out;
        std::array<float, 512> left{}, right{};
        for (int done = 0; done < frames; done += 512) {
            float* outputs[]{left.data(), right.data()};
            engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
            for (int i = 0; i < 512; ++i) out.push_back({left[static_cast<std::size_t>(i)], right[static_cast<std::size_t>(i)]});
        }
        return out;
    }
};

static juce::AudioBuffer<float> readWav(const juce::File& file) {
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
    require(reader != nullptr && reader->numChannels == 2, "the rendered file reads back");
    juce::AudioBuffer<float> samples(2, static_cast<int>(reader->lengthInSamples));
    reader->read(&samples, 0, samples.getNumSamples(), 0, true, true);
    return samples;
}
static juce::File temporary(const char* name) {
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile(name, ".wav");
}
static constexpr int songFrames = 2 * 96000;   // two bars at 120 BPM, 48 kHz

static void exportMatchesLive() {
    const Song song(true, true);
    Rig live(song);
    const int latency = live.engine->plugins->latencySamples();
    require(latency == 100 + 200 + 300 + 50, "the mix's latency counts the rack instrument, the track's chain and the master FX");
    const auto heard = live.playLive(song, songFrames + latency + 512);

    Rig render(song);
    auto options = WavExportOptions{};
    const auto file = temporary("alien-plugin-parity");
    juce::String error; std::atomic<bool> never{false}; DrumSamplePointers noDrums{};
    require(exportArrangementWav(file, *song.project, *song.playback, noDrums, never, error, render.engine.get(), options), error.toRawUTF8());
    const auto wav = readWav(file);
    require(wav.getNumSamples() >= songFrames, "the export covers the song");
    double worst = 0, energy = 0, rackEnergy = 0;
    for (int i = 0; i < songFrames; ++i)
        for (int c = 0; c < 2; ++c) {
            const float expected = heard[static_cast<std::size_t>(i + latency)][static_cast<std::size_t>(c)];
            worst = std::max(worst, static_cast<double>(std::abs(wav.getSample(c, i) - expected)));
            energy += static_cast<double>(expected) * expected;
            if (i >= 96000 && i < 96000 + 4800) rackEnergy += static_cast<double>(expected) * expected;
        }
    require(energy > 1.0 && rackEnergy > 0.1, "the song is heard: the track and, on bar 2, the rack instrument");
    require(worst < 2.0e-5, "the exported WAV is what plays live, sample for sample after the mix's latency (24-bit rounding aside)");
    // The first note is at the very start of the file: the latency was trimmed, not left as silence.
    require(std::abs(wav.getSample(0, 0)) > 0.05f, "the export starts on the first note, not after the plugins' latency");
    for (int r = 0; r < PluginRuntime::rackCount; ++r)
        if (const auto& slot = render.engine->plugins->racks[static_cast<std::size_t>(r)]; slot.instrument.processor)
            require(dynamic_cast<LatentInstrument&>(*slot.instrument.processor).offline, "the track instrument renders offline");
    require(dynamic_cast<LatentInstrument&>(*render.engine->plugins->rackChannels[0].processor).offline,
            "the rack instrument is told it is rendering offline too");
    require(!dynamic_cast<LatentInstrument&>(*live.engine->plugins->rackChannels[0].processor).offline, "a live one is not");
    file.deleteFile();
    std::puts("PASS: a WAV export with latent, automated plugins and a rack instrument matches live playback");
}

static void freezeMatchesLive() {
    // The rack instrument is loaded but plays nothing: it still adds its latency, as it does in the app.
    const Song song(false, false);
    Rig live(song);
    const int latency = live.engine->plugins->latencySamples();
    require(latency == 100 + 200 + 300, "the rack instrument's latency counts even when it is silent");
    const auto heard = live.playLive(song, songFrames + latency + 512);

    Rig render(song, true);
    WavExportOptions options; options.freezeTrack = 0; options.toTick = 2 * timeline::ticksPerBar;
    const auto file = temporary("alien-plugin-freeze");
    juce::String error; std::atomic<bool> never{false}; DrumSamplePointers noDrums{};
    require(exportArrangementWav(file, *song.project, *song.playback, noDrums, never, error, render.engine.get(), options), error.toRawUTF8());
    const auto frozen = readWav(file);
    require(frozen.getNumSamples() >= songFrames, "the frozen audio covers the track");
    // A frozen track is captured before its fader and pan; live, the centred track reaches the master at a constant
    // gain per side. Work it out from the first loud sample, then every sample must agree with it.
    int first = 0;
    while (first < songFrames && std::abs(frozen.getSample(0, first)) < 0.05f) ++first;
    require(first == 0, "the frozen audio starts on the first note, not after the plugins' latency");
    std::array<double, 2> gain{};
    for (int c = 0; c < 2; ++c) gain[static_cast<std::size_t>(c)] = heard[static_cast<std::size_t>(latency)][static_cast<std::size_t>(c)] / frozen.getSample(c, 0);
    require(gain[0] > 0.5 && gain[0] <= 1.0001 && std::abs(gain[0] - gain[1]) < 1e-4, "the live track is the frozen audio at its pan law");
    double worst = 0;
    for (int i = 0; i < songFrames; ++i)
        for (int c = 0; c < 2; ++c) {
            const double expected = heard[static_cast<std::size_t>(i + latency)][static_cast<std::size_t>(c)];
            worst = std::max(worst, std::abs(frozen.getSample(c, i) * gain[static_cast<std::size_t>(c)] - expected));
        }
    require(worst < 1.0e-5, "the frozen track is what the track plays live, sample for sample, with its automation");
    file.deleteFile();
    std::puts("PASS: freezing a track with latent, automated plugins matches live playback");
}

// Without a prepared engine an export cannot load plugins, so one with a rack instrument must be refused rather than
// render without it.
static void rackInstrumentNeedsPreparedEngine() {
    Song song(true, false);
    song.project->plugins[0] = {};
    song.playback->timeline.parameterLanes.clear(); song.project->timeline.parameterLanes.clear();
    juce::String error; std::atomic<bool> never{false}; DrumSamplePointers noDrums{};
    const auto file = temporary("alien-plugin-unprepared");
    require(!exportArrangementWav(file, *song.project, *song.playback, noDrums, never, error) && error.contains("Prepare plugin"),
            "an export with only a rack instrument still needs its plugins prepared");
    require(!file.exists(), "and writes nothing");
    std::puts("PASS: an export never silently drops a rack instrument");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    exportMatchesLive();
    freezeMatchesLive();
    rackInstrumentNeedsPreparedEngine();
    return 0;
}
