#pragma once
// A plugin that misbehaves on request, for testing the host. It is built two ways from this one file:
//   * as a real VST3 bundle (AlienTestPlugin) whose behaviour comes from the ALIEN_TEST_PLUGIN
//     environment variable, so the scanner, the loader and the check tool can be tested against a
//     genuine plugin binary; and
//   * as a plain AudioProcessor created in-process by unit tests, with the behaviour passed in.
//
// Behaviour is a comma-separated list, e.g. "crash-process=40,latency=64":
//   crash-load  throw-load  hang-load        the constructor crashes / throws / never returns
//   crash-prepare                            prepareToPlay crashes
//   crash-process[=N]  nan-process[=N]  hang-process[=N]   from block N on (default 0): crash / return NaN / never return
//   crash-thread[=N]                         a thread the plugin started crashes (no host can contain this)
//   crash-save  crash-restore                getStateInformation / setStateInformation crash
//   fragile-state                            crashes on state data it does not recognise
//   fragile-truncated                        crashes on a cut-short copy of its own state (right header, wrong length)
//   no-state                                 saves nothing           forgets-state   ignores restored state
//   crash-editor  throw-editor               opening the editor crashes / throws
//   crash-unload                             the destructor crashes
//   latency=N                                reports N samples of latency (and really delays by N)
//   false-latency=N                          reports N samples but delays by 0 (a lying plugin)
//   tail=S                                   declares S seconds of tail
//   ring=S                                   really rings on for S seconds after its input stops
//   sidechain                                has a second, sidechain input bus
//   instrument                               a synth: no input, plays a sine for each note
//   silent                                   outputs nothing at all
// Nothing here is ever installed: the VST3 is only built next to the tests.
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace alien::testplugin {

struct Behaviour {
    bool crashLoad{}, throwLoad{}, hangLoad{}, crashPrepare{}, crashSave{}, crashRestore{}, fragileState{}, fragileTruncated{}, noState{}, forgetsState{};
    bool crashEditor{}, throwEditor{}, crashUnload{}, sidechain{}, instrument{}, silent{};
    int crashProcessAt{-1}, nanProcessAt{-1}, hangProcessAt{-1}, crashThreadAt{-1};       // the block from which it misbehaves; -1 = never
    int latency{}, delay{-1};                        // reported; actual (-1 = same as reported)
    double tail{}, ring{};

    static Behaviour parse(const juce::String& text) {
        Behaviour b;
        for (auto token : juce::StringArray::fromTokens(text, ",", "")) {
            token = token.trim();
            const auto name = token.upToFirstOccurrenceOf("=", false, false).trim();
            const auto value = token.fromFirstOccurrenceOf("=", false, false).trim();
            const auto number = value.getDoubleValue();
            if (name == "crash-load") b.crashLoad = true;
            else if (name == "throw-load") b.throwLoad = true;
            else if (name == "hang-load") b.hangLoad = true;
            else if (name == "crash-prepare") b.crashPrepare = true;
            else if (name == "crash-process") b.crashProcessAt = value.isEmpty() ? 0 : static_cast<int>(number);
            else if (name == "crash-thread") b.crashThreadAt = value.isEmpty() ? 0 : static_cast<int>(number);
            else if (name == "hang-process") b.hangProcessAt = value.isEmpty() ? 0 : static_cast<int>(number);
            else if (name == "nan-process") b.nanProcessAt = value.isEmpty() ? 0 : static_cast<int>(number);
            else if (name == "crash-save") b.crashSave = true;
            else if (name == "crash-restore") b.crashRestore = true;
            else if (name == "fragile-state") b.fragileState = true;
            else if (name == "fragile-truncated") b.fragileTruncated = true;
            else if (name == "no-state") b.noState = true;
            else if (name == "forgets-state") b.forgetsState = true;
            else if (name == "crash-editor") b.crashEditor = true;
            else if (name == "throw-editor") b.throwEditor = true;
            else if (name == "crash-unload") b.crashUnload = true;
            else if (name == "latency") b.latency = static_cast<int>(number);
            else if (name == "false-latency") { b.latency = static_cast<int>(number); b.delay = 0; }
            else if (name == "tail") b.tail = number;
            else if (name == "ring") b.ring = number;
            else if (name == "sidechain") b.sidechain = true;
            else if (name == "instrument") b.instrument = true;
            else if (name == "silent") b.silent = true;
        }
        return b;
    }
    static Behaviour fromEnvironment() { return parse(juce::SystemStats::getEnvironmentVariable("ALIEN_TEST_PLUGIN", {})); }
};

// A write to address 0 that the compiler cannot remove.
[[maybe_unused]] static void crashNow() {
    static volatile std::uintptr_t zero = 0;
    *reinterpret_cast<volatile int*>(zero) = 1;
}

class Processor final : public juce::AudioProcessor {
public:
    explicit Processor(const Behaviour& behaviourToUse) : juce::AudioProcessor(busesFor(behaviourToUse)), behaviour(behaviourToUse) {
        if (behaviour.hangLoad) for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        if (behaviour.crashLoad) crashNow();
        if (behaviour.throwLoad) throw std::runtime_error("test plugin: throw-load");
        addParameter(gain = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, 1.0f));
        addParameter(tone = new juce::AudioParameterFloat(juce::ParameterID{"tone", 1}, "Tone", 0.0f, 1.0f, 0.5f));
        addParameter(mix = new juce::AudioParameterFloat(juce::ParameterID{"mix", 1}, "Mix", 0.0f, 1.0f, 1.0f));
        setLatencySamples(behaviour.latency);
    }
    ~Processor() override { if (behaviour.crashUnload) crashNow(); }

    const juce::String getName() const override { return "Alien Test Plugin"; }
    bool acceptsMidi() const override { return behaviour.instrument; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return behaviour.tail; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        const auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::stereo()) return false;
        if (!behaviour.instrument && layouts.getMainInputChannelSet() != out) return false;
        for (int i = 1; i < layouts.inputBuses.size(); ++i) if (!layouts.inputBuses[i].isDisabled() && layouts.inputBuses[i] != juce::AudioChannelSet::stereo()) return false;
        return true;
    }

    void prepareToPlay(double rate, int blockSize) override {
        if (behaviour.crashPrepare) crashNow();
        sampleRate = rate;
        const int delay = actualDelay();
        line.assign(static_cast<std::size_t>(std::max(1, delay)) * 2, 0.0f); linePos = 0;
        envelope = 0.0f; phase = 0.0; note = 60; gate = false; blocks = 0;
        decay = behaviour.ring > 0 ? static_cast<float>(std::pow(1.0e-4, 1.0 / (behaviour.ring * rate))) : 0.0f;
        juce::ignoreUnused(blockSize);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        const int index = blocks++;
        if (behaviour.crashProcessAt >= 0 && index >= behaviour.crashProcessAt) crashNow();
        if (behaviour.crashThreadAt >= 0 && index == behaviour.crashThreadAt) {
            std::thread([] { crashNow(); }).detach();               // the fault happens on a thread the host knows nothing about
            std::this_thread::sleep_for(std::chrono::seconds(5));    // and this thread waits to be taken down with it
        }
        if (behaviour.hangProcessAt >= 0 && index >= behaviour.hangProcessAt) for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        if (behaviour.nanProcessAt >= 0 && index >= behaviour.nanProcessAt) {
            for (int c = 0; c < buffer.getNumChannels(); ++c) for (int i = 0; i < buffer.getNumSamples(); ++i) buffer.setSample(c, i, std::numeric_limits<float>::quiet_NaN());
            return;
        }
        const int n = buffer.getNumSamples(), channels = std::min(2, buffer.getNumChannels());
        if (behaviour.silent) { buffer.clear(); return; }
        const float g = gain->get(), m = mix->get(), t = tone->get();
        const int delay = actualDelay();
        if (behaviour.instrument) {
            for (const auto meta : midi) {
                const auto msg = meta.getMessage();
                if (msg.isNoteOn()) { note = msg.getNoteNumber(); gate = true; envelope = 1.0f; }
                else if (msg.isNoteOff() && msg.getNoteNumber() == note) gate = false;
                else if (msg.isAllNotesOff() || msg.isAllSoundOff()) gate = false;
            }
            const double hz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
            for (int i = 0; i < n; ++i) {
                envelope = gate ? 1.0f : envelope * 0.9995f;
                const float s = static_cast<float>(std::sin(phase)) * 0.3f * g * envelope;
                phase += juce::MathConstants<double>::twoPi * hz / sampleRate;
                for (int c = 0; c < channels; ++c) buffer.setSample(c, i, s);
            }
            return;
        }
        for (int i = 0; i < n; ++i) {
            float in[2] = {buffer.getSample(0, i), buffer.getNumChannels() > 1 ? buffer.getSample(1, i) : buffer.getSample(0, i)};
            float out[2] = {in[0], in[1]};
            if (delay > 0) {
                for (int c = 0; c < 2; ++c) { const auto slot = static_cast<std::size_t>(linePos * 2 + c); out[c] = line[slot]; line[slot] = in[c]; }
                linePos = (linePos + 1) % delay;
            }
            float ringing = 0.0f;
            if (decay > 0.0f) {
                envelope = std::max(envelope * decay, std::abs(in[0]));
                ringing = envelope * 0.5f * static_cast<float>(std::sin(phase)); phase += juce::MathConstants<double>::twoPi * 440.0 / sampleRate;
            }
            for (int c = 0; c < channels; ++c) buffer.setSample(c, i, (in[c] * (1.0f - m) + (out[c] * (0.5f + 0.5f * t) + ringing) * m) * g);
        }
        for (int c = channels; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);
    }

    void getStateInformation(juce::MemoryBlock& destination) override {
        if (behaviour.crashSave) crashNow();
        if (behaviour.noState) return;
        juce::MemoryOutputStream out(destination, false);
        out.writeInt(0x41545031);            // "ATP1"
        out.writeFloat(gain->get()); out.writeFloat(tone->get()); out.writeFloat(mix->get());
    }
    void setStateInformation(const void* data, int size) override {
        if (behaviour.crashRestore) crashNow();
        if (behaviour.forgetsState) return;
        juce::MemoryInputStream in(data, static_cast<std::size_t>(size), false);
        if (behaviour.fragileTruncated && size >= 4 && juce::ByteOrder::littleEndianInt(data) == 0x41545031 && size != 16) crashNow();   // right header, wrong length
        if (size != 16 || in.readInt() != 0x41545031) { if (behaviour.fragileState && size > 0) crashNow(); return; }
        *gain = in.readFloat(); *tone = in.readFloat(); *mix = in.readFloat();
    }

    const Behaviour behaviour;
    juce::AudioParameterFloat* gain{};
    juce::AudioParameterFloat* tone{};
    juce::AudioParameterFloat* mix{};

private:
    static BusesProperties busesFor(const Behaviour& b) {
        BusesProperties p;
        if (!b.instrument) p = p.withInput("Input", juce::AudioChannelSet::stereo(), true);
        if (b.sidechain) p = p.withInput("Sidechain", juce::AudioChannelSet::stereo(), false);
        return p.withOutput("Output", juce::AudioChannelSet::stereo(), true);
    }
    int actualDelay() const { return behaviour.delay >= 0 ? behaviour.delay : behaviour.latency; }

    double sampleRate{48000.0};
    std::vector<float> line;
    int linePos{0}, blocks{0}, note{60};
    bool gate{false};
    float envelope{0}, decay{0};
    double phase{0};
};

class Editor final : public juce::AudioProcessorEditor {
public:
    explicit Editor(Processor& p) : juce::AudioProcessorEditor(p) {
        if (p.behaviour.crashEditor) crashNow();
        if (p.behaviour.throwEditor) throw std::runtime_error("test plugin: throw-editor");
        setSize(320, 180); setResizable(true, true);
    }
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::darkslategrey); g.setColour(juce::Colours::white); g.drawFittedText("Alien Test Plugin", getLocalBounds(), juce::Justification::centred, 1); }
};
inline juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }

} // namespace alien::testplugin
