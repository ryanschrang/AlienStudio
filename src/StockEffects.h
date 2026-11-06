#pragma once
// Alien Studio's stock effects as juce::AudioProcessors. They sit in exactly the
// same per-track and master FX racks as hosted VST3 effects, so bypass, ordering,
// latency compensation, tails, undo, saving and WAV export all come for free.
// Their parameters are ordinary host parameters, which is also what the upcoming
// automation lanes will drive.
#include "StockDsp.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace alien::stock {

inline constexpr const char* formatName = "AlienStock";

struct Info { const char* id; const char* name; const char* summary; };
inline constexpr std::array<Info, 16> catalog{{
    {"eq", "Alien EQ", "Four-band equaliser with low and high shelves and two bells"},
    {"compressor", "Alien Compressor", "Stereo-linked compressor with soft knee and parallel mix"},
    {"reverb", "Alien Reverb", "Stereo room reverb with pre-delay"},
    {"delay", "Alien Delay", "Stereo delay with tempo sync, ping-pong and tone"},
    {"limiter", "Alien Limiter", "Look-ahead peak limiter that never passes its ceiling"},
    {"filter", "Alien Filter", "Resonant low-pass, high-pass, band-pass and notch filter, 12 or 24 dB/oct"},
    {"saturator", "Alien Saturator", "Oversampled soft, warm or hard saturation with tone and mix"},
    {"gate", "Alien Gate", "Noise gate with hold and range; key it from another track for rhythmic gating"},
    {"chorus", "Alien Chorus", "Stereo chorus with modulated delay, rate, depth and mix"},
    {"phaser", "Alien Phaser", "Six-stage stereo phaser with rate, depth, feedback and mix"},
    {"transient", "Alien Transient Shaper", "Stereo-linked attack and sustain shaping with parallel mix and output trim"},
    {"bitcrusher", "Alien Bit Crusher", "Bit-depth reduction and sample-and-hold rate reduction with parallel mix"},
    {"imager", "Alien Stereo Imager", "Mid/side stereo width with parallel mix and output trim"},
    {"flanger", "Alien Flanger", "Short stereo modulated delay with rate, depth, signed feedback and mix"},
    {"deesser", "Alien De-Esser", "Stereo-linked high-band attenuation with frequency, threshold, reduction and mix"},
    {"multiband", "Alien Multiband Compressor", "Three-band stereo-linked compression, fixed 250/2500 Hz splits, shared timing and parallel mix"},
}};

// Parameter identifiers, shared by the processors and their editors.
namespace eq {
inline constexpr const char *lowFreq = "lowFreq", *lowGain = "lowGain", *m1Freq = "m1Freq", *m1Gain = "m1Gain", *m1Q = "m1Q",
    *m2Freq = "m2Freq", *m2Gain = "m2Gain", *m2Q = "m2Q", *highFreq = "highFreq", *highGain = "highGain", *output = "output";
}
namespace comp {
inline constexpr const char *threshold = "threshold", *ratio = "ratio", *attack = "attack", *release = "release",
    *knee = "knee", *makeup = "makeup", *mix = "mix";
}
namespace deesser {
inline constexpr const char *frequency = "frequency", *threshold = "threshold", *reduction = "reduction", *mix = "mix";
}
namespace multiband {
inline constexpr const char *low = "lowThreshold", *mid = "midThreshold", *high = "highThreshold",
    *ratio = "ratio", *attack = "attack", *release = "release", *output = "output", *mix = "mix";
inline constexpr std::array<const char*, 8> ids{low, mid, high, ratio, attack, release, output, mix};
}
namespace flanger {
inline constexpr const char *rate = "rate", *depth = "depth", *feedback = "feedback", *mix = "mix";
}
namespace imager {
inline constexpr const char *width = "width", *mix = "mix", *output = "output";
}
namespace crusher {
inline constexpr const char *bits = "bits", *reduction = "reduction", *mix = "mix";
}
namespace phaser {
inline constexpr const char *rate = "rate", *depth = "depth", *feedback = "feedback", *mix = "mix";
}
namespace transient {
inline constexpr const char *attack = "attack", *sustain = "sustain", *mix = "mix", *output = "output";
}
namespace chorus {
inline constexpr const char *rate = "rate", *depth = "depth", *mix = "mix";
}
namespace gate {
inline constexpr const char *threshold = "threshold", *range = "range", *attack = "attack", *hold = "hold", *release = "release";
}
namespace saturator {
inline constexpr const char *drive = "drive", *character = "character", *tone = "tone", *mix = "mix", *output = "output";
inline const juce::StringArray& characterNames() { static const juce::StringArray n{"Soft", "Warm", "Hard"}; return n; }
}
namespace reverb {
inline constexpr const char *size = "size", *damping = "damping", *width = "width", *preDelay = "preDelay", *mix = "mix";
}
namespace delay {
inline constexpr const char *time = "time", *sync = "sync", *division = "division", *feedback = "feedback",
    *pingPong = "pingPong", *tone = "tone", *mix = "mix";
}
namespace limiter {
inline constexpr const char *gain = "gain", *ceiling = "ceiling", *release = "release";
}
namespace filter {
inline constexpr const char *mode = "mode", *slope = "slope", *cutoff = "cutoff", *resonance = "resonance",
    *mix = "mix", *output = "output";
inline const juce::StringArray& modeNames() { static const juce::StringArray n{"Low-pass", "High-pass", "Band-pass", "Notch"}; return n; }
inline const juce::StringArray& slopeNames() { static const juce::StringArray n{"12 dB/oct", "24 dB/oct"}; return n; }
}

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

inline juce::String formatFrequency(float hz) {
    return hz >= 1000.0f ? juce::String(hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz" : juce::String(hz, hz >= 100.0f ? 0 : 1) + " Hz";
}
inline juce::String formatMilliseconds(float ms) {
    return ms >= 1000.0f ? juce::String(ms / 1000.0f, 2) + " s" : juce::String(ms, ms >= 100.0f ? 0 : 1) + " ms";
}
// Typed values: "1.5k", "1500", "250 ms", "1.2 s", "-6", "40%".
inline float parseNumber(const juce::String& text) { return text.retainCharacters("0123456789.-").getFloatValue(); }
inline float parseFrequency(const juce::String& text) {
    return parseNumber(text) * (text.containsIgnoreCase("k") ? 1000.0f : 1.0f);
}
inline float parseMilliseconds(const juce::String& text) {
    const bool seconds = text.trim().endsWithIgnoreCase("s") && !text.trim().endsWithIgnoreCase("ms");
    return parseNumber(text) * (seconds ? 1000.0f : 1.0f);
}
struct Format {
    std::function<juce::String(float, int)> text;
    std::function<float(const juce::String&)> parse;
};

struct Range { float low, high, centre; };  // centre: value that sits in the middle of the control (0 = linear)
inline std::unique_ptr<juce::AudioParameterFloat> floatParameter(const char* id, const char* name, Range r, float def, Format format) {
    juce::NormalisableRange<float> range(r.low, r.high);
    if (r.centre > 0.0f) range.setSkewForCentre(r.centre);
    return std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name, range, def,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(std::move(format.text))
                                             .withValueFromStringFunction(std::move(format.parse)));
}
inline Format dbFormat(const char* suffix = " dB") {
    return {[suffix](float v, int) { return juce::String(v, 1) + suffix; }, [](const juce::String& t) { return parseNumber(t); }};
}
inline Format percentFormat() {
    return {[](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; },
            [](const juce::String& t) { return parseNumber(t) / 100.0f; }};
}
inline Format frequencyFormat() {
    return {[](float v, int) { return formatFrequency(v); }, [](const juce::String& t) { return parseFrequency(t); }};
}
inline Format millisecondsFormat() {
    return {[](float v, int) { return formatMilliseconds(v); }, [](const juce::String& t) { return parseMilliseconds(t); }};
}
inline Format plainFormat(const char* suffix = "", int decimals = 2) {
    return {[suffix, decimals](float v, int) { return juce::String(v, decimals) + suffix; }, [](const juce::String& t) { return parseNumber(t); }};
}

// A parameter's current plain value (Hz, dB, a choice index, 0/1), read from the parameter
// itself. Host automation calls setValue without notifying listeners, since none may run on
// the audio thread, so APVTS's separate raw-value cache would keep the old setting.
inline double plainValue(const juce::RangedAudioParameter& p) noexcept {
    if (auto* f = dynamic_cast<const juce::AudioParameterFloat*>(&p)) return f->get();  // exact, no round trip
    return p.convertFrom0to1(p.getValue());
}

// Common behaviour: stereo in / stereo out, one parameter set, saved as XML.
class Effect : public juce::AudioProcessor {
public:
    // Set by StockEffectEditors.h. Without it the host falls back to a generic editor.
    using EditorFactory = juce::AudioProcessorEditor* (*)(Effect&);
    static inline EditorFactory editorFactory = nullptr;

    juce::AudioProcessorValueTreeState parameters;

    const juce::String getName() const override { return info.name; }
    const Info& effectInfo() const noexcept { return info; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        // An effect with a sidechain input takes it in stereo, or not at all.
        for (int bus = 1; bus < layouts.inputBuses.size(); ++bus)
            if (!layouts.inputBuses[bus].isDisabled() && layouts.inputBuses[bus] != juce::AudioChannelSet::stereo()) return false;
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    // The key the host gave this block (channels 2 and 3, when the sidechain input is on), or nulls.
    std::pair<const float*, const float*> sidechainKey(const juce::AudioBuffer<float>& buffer) const {
        if (getBusCount(true) < 2 || !getBus(true, 1)->isEnabled() || buffer.getNumChannels() < 4) return {nullptr, nullptr};
        return {buffer.getReadPointer(2), buffer.getReadPointer(3)};
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return editorFactory != nullptr; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory(*this) : nullptr; }
    void releaseResources() override {}
    // Saves each parameter's own value: APVTS's tree misses host automation, which sets values
    // without notifying it. The snapshot is a private copy, so saving sends no host edit events.
    void getStateInformation(juce::MemoryBlock& destination) override {
        auto state = parameters.copyState();
        for (auto child : state) if (auto* p = parameters.getParameter(child.getProperty("id").toString()))
            child.setProperty("value", plainValue(*p), nullptr);
        if (auto xml = state.createXml()) copyXmlToBinary(*xml, destination);
    }
    void setStateInformation(const void* data, int size) override {
        // Anything that is not this effect's own state is ignored, not trusted.
        if (auto xml = getXmlFromBinary(data, size))
            if (xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
    double currentSampleRate() const noexcept { return rate.load(); }

protected:
    // `sidechainInput`: a second, stereo input the host can switch on to key the effect from another track.
    Effect(const Info& effectInformation, Layout layout, bool sidechainInput = false)
        : juce::AudioProcessor(buses(sidechainInput)),
          parameters(*this, nullptr, juce::String("ALIEN_") + juce::String(effectInformation.id).toUpperCase(), std::move(layout)),
          info(effectInformation) {}
    // Audio thread: one parameter's plain value, read from the parameter itself (see plainValue),
    // with the type check done once when the effect is built.
    class Source {
    public:
        Source() = default;
        explicit Source(const juce::RangedAudioParameter* p) noexcept
            : parameter(p), floating(dynamic_cast<const juce::AudioParameterFloat*>(p)) {}
        double get() const noexcept {
            return floating != nullptr ? static_cast<double>(floating->get()) : static_cast<double>(parameter->convertFrom0to1(parameter->getValue()));
        }
    private:
        const juce::RangedAudioParameter* parameter{};
        const juce::AudioParameterFloat* floating{};
    };
    double value(const Source& p) const noexcept { return p.get(); }
    Source source(const char* id) const { return Source(parameters.getParameter(id)); }
    static BusesProperties buses(bool sidechainInput) {
        auto properties = BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput("Output", juce::AudioChannelSet::stereo(), true);
        return sidechainInput ? properties.withInput("Sidechain", juce::AudioChannelSet::stereo(), false) : properties;
    }
    // A NaN or infinity from upstream would poison feedback paths for good.
    static void sanitise(float* left, float* right, int count) noexcept {
        for (int i = 0; i < count; ++i) {
            if (!dsp::finite(left[i])) left[i] = 0.0f;
            if (!dsp::finite(right[i])) right[i] = 0.0f;
        }
    }
    std::atomic<double> rate{44100.0};
    bool prepared{false};
    const Info& info;
};

// ---- Equaliser --------------------------------------------------------------

class EqEffect final : public Effect {
public:
    EqEffect() : Effect(catalog[0], layout()) {
        for (std::size_t i = 0; i < pointers.size(); ++i) pointers[i] = source(names()[i]);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; equaliser.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        equaliser.process(left, right, buffer.getNumSamples(), settings());
    }
    double getTailLengthSeconds() const override { return 0.1; }
    dsp::ParametricEq::Settings settings() const noexcept {
        dsp::ParametricEq::Settings s;
        s.band[0] = {value(pointers[0]), value(pointers[1]), 0.7071};
        s.band[1] = {value(pointers[2]), value(pointers[3]), value(pointers[4])};
        s.band[2] = {value(pointers[5]), value(pointers[6]), value(pointers[7])};
        s.band[3] = {value(pointers[8]), value(pointers[9]), 0.7071};
        s.outputDb = value(pointers[10]);
        return s;
    }

private:
    static const std::array<const char*, 11>& names() {
        static const std::array<const char*, 11> n{eq::lowFreq, eq::lowGain, eq::m1Freq, eq::m1Gain, eq::m1Q, eq::m2Freq,
            eq::m2Gain, eq::m2Q, eq::highFreq, eq::highGain, eq::output};
        return n;
    }
    static Layout layout() {
        Layout l;
        const auto hz = frequencyFormat();
        const auto q = plainFormat();
        l.add(floatParameter(eq::lowFreq, "Low shelf frequency", {20, 500, 100}, 100, hz));
        l.add(floatParameter(eq::lowGain, "Low shelf gain", {-18, 18, 0}, 0, dbFormat()));
        l.add(floatParameter(eq::m1Freq, "Low-mid frequency", {50, 5000, 500}, 400, hz));
        l.add(floatParameter(eq::m1Gain, "Low-mid gain", {-18, 18, 0}, 0, dbFormat()));
        l.add(floatParameter(eq::m1Q, "Low-mid Q", {0.2f, 10, 1}, 1, q));
        l.add(floatParameter(eq::m2Freq, "High-mid frequency", {200, 16000, 2500}, 3000, hz));
        l.add(floatParameter(eq::m2Gain, "High-mid gain", {-18, 18, 0}, 0, dbFormat()));
        l.add(floatParameter(eq::m2Q, "High-mid Q", {0.2f, 10, 1}, 1, q));
        l.add(floatParameter(eq::highFreq, "High shelf frequency", {1000, 20000, 6000}, 8000, hz));
        l.add(floatParameter(eq::highGain, "High shelf gain", {-18, 18, 0}, 0, dbFormat()));
        l.add(floatParameter(eq::output, "Output", {-12, 12, 0}, 0, dbFormat()));
        return l;
    }
    std::array<Source, 11> pointers{};
    dsp::ParametricEq equaliser;
};

// ---- Compressor ---------------------------------------------------------------

class CompressorEffect final : public Effect {
public:
    CompressorEffect() : Effect(catalog[1], layout(), true) {
        threshold = source(comp::threshold); ratio = source(comp::ratio); attack = source(comp::attack); release = source(comp::release);
        knee = source(comp::knee); makeup = source(comp::makeup); mix = source(comp::mix);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; compressor.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        const auto key = sidechainKey(buffer);   // another track's signal, when the host keys it
        compressor.process(left, right, buffer.getNumSamples(), settings(), key.first, key.second);
        reduction.store(static_cast<float>(compressor.lastBlockReductionDb()), std::memory_order_relaxed);
    }
    double getTailLengthSeconds() const override { return 0.0; }
    dsp::Compressor::Settings settings() const noexcept {
        return {value(threshold), value(ratio), value(attack), value(release), value(knee), value(makeup), value(mix)};
    }
    // Gain reduction in the most recent block, for the editor's meter.
    float gainReductionDb() const noexcept { return reduction.load(std::memory_order_relaxed); }

private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(comp::threshold, "Threshold", {-60, 0, -24}, -18, dbFormat()));
        l.add(floatParameter(comp::ratio, "Ratio", {1, 20, 4}, 4, plainFormat(" : 1", 1)));
        l.add(floatParameter(comp::attack, "Attack", {0.1f, 200, 10}, 10, millisecondsFormat()));
        l.add(floatParameter(comp::release, "Release", {5, 2000, 150}, 120, millisecondsFormat()));
        l.add(floatParameter(comp::knee, "Knee", {0, 24, 0}, 6, dbFormat()));
        l.add(floatParameter(comp::makeup, "Make-up gain", {0, 24, 0}, 0, dbFormat()));
        l.add(floatParameter(comp::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        return l;
    }
    Source threshold, ratio, attack, release, knee, makeup, mix;
    std::atomic<float> reduction{0.0f};
    dsp::Compressor compressor;
};

// ---- Gate ---------------------------------------------------------------------

class GateEffect final : public Effect {
public:
    GateEffect() : Effect(catalog[7], layout(), true) {
        threshold = source(gate::threshold); range = source(gate::range); attack = source(gate::attack);
        hold = source(gate::hold); release = source(gate::release);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        const auto key = sidechainKey(buffer);   // another track's, bus's or return's signal, when the host keys it
        engine.process(left, right, buffer.getNumSamples(), settings(), key.first, key.second);
        attenuation.store(static_cast<float>(engine.lastBlockAttenuationDb()), std::memory_order_relaxed);
    }
    double getTailLengthSeconds() const override { return 0.0; }
    dsp::Gate::Settings settings() const noexcept {
        return {value(threshold), value(range), value(attack), value(hold), value(release)};
    }
    // How far the gate turned the sound down in the most recent block, for the editor's meter.
    float attenuationDb() const noexcept { return attenuation.load(std::memory_order_relaxed); }

private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(gate::threshold, "Threshold", {-80, 0, -30}, -40, dbFormat()));
        l.add(floatParameter(gate::range, "Range", {-80, 0, -24}, -80, dbFormat()));
        l.add(floatParameter(gate::attack, "Attack", {0.05f, 50, 2}, 1, millisecondsFormat()));
        l.add(floatParameter(gate::hold, "Hold", {0, 500, 60}, 50, millisecondsFormat()));
        l.add(floatParameter(gate::release, "Release", {5, 2000, 150}, 150, millisecondsFormat()));
        return l;
    }
    Source threshold, range, attack, hold, release;
    std::atomic<float> attenuation{0.0f};
    dsp::Gate engine;
};

// ---- Saturator ----------------------------------------------------------------

class SaturatorEffect final : public Effect {
public:
    SaturatorEffect() : Effect(catalog[6], layout()) {
        drive = source(saturator::drive); character = source(saturator::character); tone = source(saturator::tone);
        mix = source(saturator::mix); output = source(saturator::output);
        setLatencySamples(dsp::Saturator::latency); // fixed: the oversampling filters
    }
    void prepareToPlay(double sampleRate, int) override {
        rate = sampleRate; engine.prepare(sampleRate);
        setLatencySamples(dsp::Saturator::latency); // the host delays every other track to match
        prepared = true;
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        engine.process(left, right, buffer.getNumSamples(), settings());
    }
    double getTailLengthSeconds() const override { return 0.01; }
    dsp::Saturator::Settings settings() const noexcept {
        dsp::Saturator::Settings s;
        s.curve = static_cast<dsp::SaturationCurve>(std::clamp(static_cast<int>(value(character) + 0.5), 0, 2));
        s.driveDb = value(drive); s.toneHz = value(tone); s.mix = value(mix); s.outputDb = value(output);
        return s;
    }

private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(saturator::drive, "Drive", {0, 36, 12}, 6, dbFormat()));
        l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{saturator::character, 1}, "Character", saturator::characterNames(), 0));
        l.add(floatParameter(saturator::tone, "Tone", {500, 20000, 4000}, 20000,
            Format{[](float v, int) { return v >= 19999.0f ? juce::String("Open") : formatFrequency(v); },
                   [](const juce::String& t) { return t.containsIgnoreCase("open") ? 20000.0f : parseFrequency(t); }}));
        l.add(floatParameter(saturator::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        l.add(floatParameter(saturator::output, "Output", {-24, 12, 0}, 0, dbFormat()));
        return l;
    }
    Source drive, character, tone, mix, output;
    dsp::Saturator engine;
};

// ---- Reverb -------------------------------------------------------------------

class ReverbEffect final : public Effect {
public:
    static constexpr int scratchSize = 512;
    // Wet level calibration: a steady signal comes back from the reverb at about
    // the level it went in (checked by the tests), so Mix behaves like a crossfade.
    static constexpr float wetCalibration = 0.72f;

    ReverbEffect() : Effect(catalog[2], layout()) {
        size = source(reverb::size); damping = source(reverb::damping); width = source(reverb::width);
        preDelay = source(reverb::preDelay); mix = source(reverb::mix);
    }
    void prepareToPlay(double sampleRate, int) override {
        rate = sampleRate;
        // setSampleRate snaps JUCE's internal smoothing to the parameters set just before it, so the
        // reverb starts at the current settings (and no allocation is left for the audio thread).
        engine.setParameters(currentParameters());
        engine.setSampleRate(sampleRate);
        engine.reset();
        const int maxPre = static_cast<int>(std::ceil(0.1 * sampleRate)) + 8;
        preLeft.prepare(maxPre); preRight.prepare(maxPre);
        prepared = true;
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        const int total = buffer.getNumSamples();
        sanitise(left, right, total);
        engine.setParameters(currentParameters());
        const double delaySamples = value(preDelay) * 0.001 * rate;
        const float wetMix = static_cast<float>(std::clamp(value(mix), 0.0, 1.0));
        for (int start = 0; start < total; start += scratchSize) {
            const int n = std::min(scratchSize, total - start);
            for (int i = 0; i < n; ++i) {
                const float inL = left[start + i], inR = right[start + i];
                if (delaySamples >= 2.0) {
                    wetLeft[static_cast<std::size_t>(i)] = preLeft.read(delaySamples); wetRight[static_cast<std::size_t>(i)] = preRight.read(delaySamples);
                } else { wetLeft[static_cast<std::size_t>(i)] = inL; wetRight[static_cast<std::size_t>(i)] = inR; }
                preLeft.push(inL); preRight.push(inR);
            }
            engine.processStereo(wetLeft.data(), wetRight.data(), n);
            for (int i = 0; i < n; ++i) {
                left[start + i] = left[start + i] * (1.0f - wetMix) + wetLeft[static_cast<std::size_t>(i)] * wetMix;
                right[start + i] = right[start + i] * (1.0f - wetMix) + wetRight[static_cast<std::size_t>(i)] * wetMix;
            }
        }
    }
    // Time for the reverb to fall 60 dB (about the slowest comb filter), plus pre-delay.
    static double tailSecondsFor(double roomSize, double preDelayMs) {
        const double feedback = 0.7 + 0.28 * std::clamp(roomSize, 0.0, 1.0);
        return std::min(30.0, 0.0367 * std::log(1000.0) / -std::log(feedback) + preDelayMs * 0.001 + 0.05);
    }
    double getTailLengthSeconds() const override { return tailSecondsFor(value(size), value(preDelay)); }

private:
    juce::Reverb::Parameters currentParameters() const {
        juce::Reverb::Parameters p;
        p.roomSize = static_cast<float>(value(size)); p.damping = static_cast<float>(value(damping));
        p.width = static_cast<float>(value(width)); p.wetLevel = wetCalibration; p.dryLevel = 0.0f; p.freezeMode = 0.0f;
        return p;
    }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(reverb::size, "Size", {0, 1, 0}, 0.5f, percentFormat()));
        l.add(floatParameter(reverb::damping, "Damping", {0, 1, 0}, 0.5f, percentFormat()));
        l.add(floatParameter(reverb::width, "Width", {0, 1, 0}, 1.0f, percentFormat()));
        l.add(floatParameter(reverb::preDelay, "Pre-delay", {0, 100, 0}, 10, millisecondsFormat()));
        l.add(floatParameter(reverb::mix, "Mix", {0, 1, 0}, 0.25f, percentFormat()));
        return l;
    }
    Source size, damping, width, preDelay, mix;
    juce::Reverb engine;
    dsp::FractionalDelayLine preLeft, preRight;
    std::array<float, scratchSize> wetLeft{}, wetRight{};
};

// ---- Delay --------------------------------------------------------------------

class DelayEffect final : public Effect {
public:
    DelayEffect() : Effect(catalog[3], layout()) {
        time = source(delay::time); sync = source(delay::sync); division = source(delay::division); feedback = source(delay::feedback);
        pingPong = source(delay::pingPong); tone = source(delay::tone); mix = source(delay::mix);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        const auto s = settings(hostTempo());
        lastTimeMs.store(s.timeMs, std::memory_order_relaxed);
        engine.process(left, right, buffer.getNumSamples(), s);
    }
    double getTailLengthSeconds() const override {
        return dsp::delayTailSeconds(lastTimeMs.load(std::memory_order_relaxed), value(feedback));
    }
    // The delay time in milliseconds for the current controls at a given tempo.
    dsp::StereoDelay::Settings settings(double bpm) const noexcept {
        const double ms = value(sync) > 0.5f ? dsp::syncedDelayMs(static_cast<int>(value(division) + 0.5), bpm) : value(time);
        return {ms, value(feedback), value(tone), value(mix), value(pingPong) > 0.5};
    }

private:
    double hostTempo() const noexcept {
        if (auto* head = getPlayHead())
            if (auto position = head->getPosition())
                if (auto bpm = position->getBpm()) return *bpm;
        return 120.0;
    }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(delay::time, "Time", {1, 2000, 300}, 350, millisecondsFormat()));
        l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{delay::sync, 1}, "Tempo sync", false));
        l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{delay::division, 1}, "Sync division",
            juce::StringArray{"1/16", "1/8 triplet", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2", "1 bar"}, 2));
        l.add(floatParameter(delay::feedback, "Feedback", {0, 0.95f, 0}, 0.4f, percentFormat()));
        l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{delay::pingPong, 1}, "Ping-pong", false));
        l.add(floatParameter(delay::tone, "Tone", {500, 20000, 4000}, 12000,
            Format{[](float v, int) { return v >= 19999.0f ? juce::String("Open") : formatFrequency(v); },
                   [](const juce::String& t) { return t.containsIgnoreCase("open") ? 20000.0f : parseFrequency(t); }}));
        l.add(floatParameter(delay::mix, "Mix", {0, 1, 0}, 0.3f, percentFormat()));
        return l;
    }
    Source time, sync, division, feedback, pingPong, tone, mix;
    std::atomic<double> lastTimeMs{350.0};
    dsp::StereoDelay engine;
};

// ---- Limiter ------------------------------------------------------------------

class LimiterEffect final : public Effect {
public:
    LimiterEffect() : Effect(catalog[4], layout()) {
        gain = source(limiter::gain); ceiling = source(limiter::ceiling); release = source(limiter::release);
    }
    void prepareToPlay(double sampleRate, int) override {
        rate = sampleRate; engine.prepare(sampleRate);
        setLatencySamples(engine.latencySamples()); // the host delays every other track to match
        prepared = true;
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        engine.process(left, right, buffer.getNumSamples(), settings());
        reduction.store(static_cast<float>(engine.lastBlockReductionDb()), std::memory_order_relaxed);
    }
    double getTailLengthSeconds() const override { return dsp::Limiter::lookaheadMs * 0.001; }
    dsp::Limiter::Settings settings() const noexcept { return {value(gain), value(ceiling), value(release)}; }
    float gainReductionDb() const noexcept { return reduction.load(std::memory_order_relaxed); }

private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(limiter::gain, "Gain", {0, 24, 0}, 0, dbFormat()));
        l.add(floatParameter(limiter::ceiling, "Ceiling", {-12, 0, 0}, -0.3f, dbFormat(" dBFS")));
        l.add(floatParameter(limiter::release, "Release", {10, 1000, 100}, 100, millisecondsFormat()));
        return l;
    }
    Source gain, ceiling, release;
    std::atomic<float> reduction{0.0f};
    dsp::Limiter engine;
};

// ---- Filter -------------------------------------------------------------------

class FilterEffect final : public Effect {
public:
    FilterEffect() : Effect(catalog[5], layout()) {
        mode = source(filter::mode); slope = source(filter::slope); cutoff = source(filter::cutoff); resonance = source(filter::resonance);
        mix = source(filter::mix); output = source(filter::output);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        engine.process(left, right, buffer.getNumSamples(), settings());
    }
    // A resonant filter rings on after its input stops; the tail follows cutoff and resonance.
    double getTailLengthSeconds() const override {
        const auto s = settings();
        return dsp::filterTailSeconds(s.frequency, s.resonance, s.steep);
    }
    dsp::MultimodeFilter::Settings settings() const noexcept {
        dsp::MultimodeFilter::Settings s;
        s.mode = static_cast<dsp::FilterMode>(std::clamp(static_cast<int>(value(mode) + 0.5), 0, 3));
        s.steep = value(slope) > 0.5;
        s.frequency = value(cutoff); s.resonance = value(resonance); s.mix = value(mix); s.outputDb = value(output);
        return s;
    }

private:
    static Layout layout() {
        Layout l;
        l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{filter::mode, 1}, "Mode", filter::modeNames(), 0));
        l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{filter::slope, 1}, "Slope", filter::slopeNames(), 0));
        l.add(floatParameter(filter::cutoff, "Cutoff", {20, 20000, 1000}, 1000, frequencyFormat()));
        l.add(floatParameter(filter::resonance, "Resonance (Q)", {0.5f, 10, 1.5f}, 0.7071f, plainFormat()));
        l.add(floatParameter(filter::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        l.add(floatParameter(filter::output, "Output", {-12, 12, 0}, 0, dbFormat()));
        return l;
    }
    Source mode, slope, cutoff, resonance, mix, output;
    dsp::MultimodeFilter engine;
};

// ---- Chorus -------------------------------------------------------------------
class ChorusEffect final : public Effect {
public:
    ChorusEffect() : Effect(catalog[8], layout()) { speed = source(chorus::rate); depth = source(chorus::depth); mix = source(chorus::mix); }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), {value(speed), value(depth), value(mix)});
    }
    double getTailLengthSeconds() const override { return dsp::StereoChorus::tailSeconds; }
private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(chorus::rate, "Rate", {.05f, 5, .8f}, .8f, plainFormat(" Hz")));
        l.add(floatParameter(chorus::depth, "Depth", {0, 1, 0}, .5f, percentFormat()));
        l.add(floatParameter(chorus::mix, "Mix", {0, 1, 0}, .35f, percentFormat()));
        return l;
    }
    Source speed, depth, mix;
    dsp::StereoChorus engine;
};

// ---- Phaser -------------------------------------------------------------------
class PhaserEffect final : public Effect {
public:
    PhaserEffect() : Effect(catalog[9], layout()) { speed = parameter(phaser::rate); depth = parameter(phaser::depth); feedback = parameter(phaser::feedback); mix = parameter(phaser::mix); }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), settings());
    }
    dsp::StereoPhaser::Settings settings() const noexcept { return {speed->get(), depth->get(), feedback->get(), mix->get()}; }
    double getTailLengthSeconds() const override { return dsp::StereoPhaser::tailSeconds; }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(phaser::rate, "Rate", {.05f, 5, .8f}, .5f, plainFormat(" Hz")));
        l.add(floatParameter(phaser::depth, "Depth", {0, 1, 0}, .7f, percentFormat()));
        l.add(floatParameter(phaser::feedback, "Feedback", {0, .85f, 0}, .3f, percentFormat()));
        l.add(floatParameter(phaser::mix, "Mix", {0, 1, 0}, .5f, percentFormat()));
        return l;
    }
    juce::AudioParameterFloat *speed{}, *depth{}, *feedback{}, *mix{};
    dsp::StereoPhaser engine;
};

// ---- Transient shaper ---------------------------------------------------------
class TransientEffect final : public Effect {
public:
    TransientEffect() : Effect(catalog[10], layout()) {
        attack = parameter(transient::attack); sustain = parameter(transient::sustain);
        mix = parameter(transient::mix); output = parameter(transient::output);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), settings());
    }
    dsp::TransientShaper::Settings settings() const noexcept { return {attack->get(), sustain->get(), mix->get(), output->get()}; }
    double getTailLengthSeconds() const override { return 0; }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(transient::attack, "Attack", {-12, 12, 0}, 0, dbFormat()));
        l.add(floatParameter(transient::sustain, "Sustain", {-12, 12, 0}, 0, dbFormat()));
        l.add(floatParameter(transient::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        l.add(floatParameter(transient::output, "Output", {-12, 12, 0}, 0, dbFormat()));
        return l;
    }
    juce::AudioParameterFloat *attack{}, *sustain{}, *mix{}, *output{};
    dsp::TransientShaper engine;
};

class BitCrusherEffect final : public Effect {
public:
    BitCrusherEffect() : Effect(catalog[11], layout()) {
        bits = parameter(crusher::bits); reduction = parameter(crusher::reduction); mix = parameter(crusher::mix);
    }
    void prepareToPlay(double sampleRate, int) override { engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (prepared && buffer.getNumChannels() >= 2) engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), {bits->get(), reduction->get(), mix->get()});
    }
    double getTailLengthSeconds() const override { return 32.0 / std::max(8000.0, getSampleRate()); }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(crusher::bits, "Bits", {4, 24, 0}, 12, plainFormat(" bits", 0)));
        l.add(floatParameter(crusher::reduction, "Reduction", {1, 32, 0}, 1, plainFormat(" samples", 0)));
        l.add(floatParameter(crusher::mix, "Mix", {0, 1, 0}, 0, percentFormat()));
        return l;
    }
    juce::AudioParameterFloat *bits{}, *reduction{}, *mix{};
    dsp::BitCrusher engine;
};

class StereoImagerEffect final : public Effect {
public:
    StereoImagerEffect() : Effect(catalog[12], layout()) {
        width = parameter(imager::width); mix = parameter(imager::mix); output = parameter(imager::output);
    }
    void prepareToPlay(double sampleRate, int) override { engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (prepared && buffer.getNumChannels() >= 2) engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), {width->get(), mix->get(), output->get()});
    }
    double getTailLengthSeconds() const override { return 0; }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(imager::width, "Width", {0, 2, 0}, 1, percentFormat()));
        l.add(floatParameter(imager::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        l.add(floatParameter(imager::output, "Output", {-12, 12, 0}, 0, dbFormat()));
        return l;
    }
    juce::AudioParameterFloat *width{}, *mix{}, *output{};
    dsp::StereoImager engine;
};

class FlangerEffect final : public Effect {
public:
    FlangerEffect() : Effect(catalog[13], layout()) {
        speed = parameter(flanger::rate); depth = parameter(flanger::depth);
        feedback = parameter(flanger::feedback); mix = parameter(flanger::mix);
    }
    void prepareToPlay(double sampleRate, int) override { engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (prepared && buffer.getNumChannels() >= 2) engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), {speed->get(), depth->get(), feedback->get(), mix->get()});
    }
    double getTailLengthSeconds() const override { return dsp::StereoFlanger::tailSeconds; }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(flanger::rate, "Rate", {.05f, 5, .8f}, .25f, plainFormat(" Hz")));
        l.add(floatParameter(flanger::depth, "Depth", {0, 1, 0}, .7f, percentFormat()));
        l.add(floatParameter(flanger::feedback, "Feedback", {-.85f, .85f, 0}, .35f, percentFormat()));
        l.add(floatParameter(flanger::mix, "Mix", {0, 1, 0}, .5f, percentFormat()));
        return l;
    }
    juce::AudioParameterFloat *speed{}, *depth{}, *feedback{}, *mix{};
    dsp::StereoFlanger engine;
};

class DeEsserEffect final : public Effect {
public:
    DeEsserEffect() : Effect(catalog[14], layout()) {
        frequency = parameter(deesser::frequency); threshold = parameter(deesser::threshold);
        reduction = parameter(deesser::reduction); mix = parameter(deesser::mix);
    }
    void prepareToPlay(double sampleRate, int) override { engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (prepared && buffer.getNumChannels() >= 2) engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), {frequency->get(), threshold->get(), reduction->get(), mix->get()});
    }
    double getTailLengthSeconds() const override { return dsp::DeEsser::tailSeconds; }
private:
    juce::AudioParameterFloat* parameter(const char* id) { return static_cast<juce::AudioParameterFloat*>(parameters.getParameter(id)); }
    static Layout layout() {
        Layout l;
        l.add(floatParameter(deesser::frequency, "Frequency", {2000, 12000, 5000}, 5000, frequencyFormat()));
        l.add(floatParameter(deesser::threshold, "Threshold", {-60, 0, 0}, -18, dbFormat(" dBFS")));
        l.add(floatParameter(deesser::reduction, "Reduction", {0, 18, 0}, 6, dbFormat()));
        l.add(floatParameter(deesser::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        return l;
    }
    juce::AudioParameterFloat *frequency{}, *threshold{}, *reduction{}, *mix{};
    dsp::DeEsser engine;
};

class MultibandEffect final : public Effect {
public:
    MultibandEffect() : Effect(catalog[15], layout()) {
        for (std::size_t i = 0; i < controls.size(); ++i)
            controls[i] = static_cast<juce::AudioParameterFloat*>(parameters.getParameter(multiband::ids[i]));
    }
    void prepareToPlay(double sampleRate, int) override { engine.prepare(sampleRate); prepared = true; }
    void reset() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (prepared && buffer.getNumChannels() >= 2) engine.process(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(),
            {{controls[0]->get(), controls[1]->get(), controls[2]->get()}, controls[3]->get(), controls[4]->get(), controls[5]->get(), controls[6]->get(), controls[7]->get()});
    }
    double getTailLengthSeconds() const override { return dsp::MultibandCompressor::tailSeconds; }
private:
    static Layout layout() {
        Layout l;
        l.add(floatParameter(multiband::low, "Low threshold", {-60, 0, 0}, -18, dbFormat(" dBFS")));
        l.add(floatParameter(multiband::mid, "Mid threshold", {-60, 0, 0}, -18, dbFormat(" dBFS")));
        l.add(floatParameter(multiband::high, "High threshold", {-60, 0, 0}, -18, dbFormat(" dBFS")));
        l.add(floatParameter(multiband::ratio, "Ratio", {1, 20, 4}, 4, plainFormat(" : 1", 1)));
        l.add(floatParameter(multiband::attack, "Attack", {.1f, 200, 10}, 10, millisecondsFormat()));
        l.add(floatParameter(multiband::release, "Release", {5, 2000, 150}, 120, millisecondsFormat()));
        l.add(floatParameter(multiband::output, "Output", {-12, 12, 0}, 0, dbFormat()));
        l.add(floatParameter(multiband::mix, "Mix", {0, 1, 0}, 1, percentFormat()));
        return l;
    }
    std::array<juce::AudioParameterFloat*, 8> controls{};
    dsp::MultibandCompressor engine;
};

// ---- Catalogue and project-file identity -----------------------------------------

inline std::unique_ptr<Effect> create(const juce::String& id) {
    if (id == catalog[0].id) return std::make_unique<EqEffect>();
    if (id == catalog[1].id) return std::make_unique<CompressorEffect>();
    if (id == catalog[2].id) return std::make_unique<ReverbEffect>();
    if (id == catalog[3].id) return std::make_unique<DelayEffect>();
    if (id == catalog[4].id) return std::make_unique<LimiterEffect>();
    if (id == catalog[5].id) return std::make_unique<FilterEffect>();
    if (id == catalog[6].id) return std::make_unique<SaturatorEffect>();
    if (id == catalog[7].id) return std::make_unique<GateEffect>();
    if (id == catalog[8].id) return std::make_unique<ChorusEffect>();
    if (id == catalog[9].id) return std::make_unique<PhaserEffect>();
    if (id == catalog[10].id) return std::make_unique<TransientEffect>();
    if (id == catalog[11].id) return std::make_unique<BitCrusherEffect>();
    if (id == catalog[12].id) return std::make_unique<StereoImagerEffect>();
    if (id == catalog[13].id) return std::make_unique<FlangerEffect>();
    if (id == catalog[14].id) return std::make_unique<DeEsserEffect>();
    if (id == catalog[15].id) return std::make_unique<MultibandEffect>();
    return {};
}

// How a stock effect is recorded in a project's rack, in the same shape as a hosted plugin.
inline juce::PluginDescription describe(const Info& info) {
    juce::PluginDescription d;
    d.name = info.name; d.descriptiveName = info.summary; d.pluginFormatName = formatName; d.category = "Stock FX";
    d.manufacturerName = "Alien Studio"; d.version = "1.0"; d.fileOrIdentifier = info.id; d.isInstrument = false;
    d.numInputChannels = 2; d.numOutputChannels = 2; d.uniqueId = d.deprecatedUid = juce::String(info.id).hashCode();
    return d;
}
inline bool isStock(const juce::PluginDescription& d) { return d.pluginFormatName == formatName; }

} // namespace alien::stock
