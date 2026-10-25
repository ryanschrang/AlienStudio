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
inline constexpr std::array<Info, 7> catalog{{
    {"eq", "Alien EQ", "Four-band equaliser with low and high shelves and two bells"},
    {"compressor", "Alien Compressor", "Stereo-linked compressor with soft knee and parallel mix"},
    {"reverb", "Alien Reverb", "Stereo room reverb with pre-delay"},
    {"delay", "Alien Delay", "Stereo delay with tempo sync, ping-pong and tone"},
    {"limiter", "Alien Limiter", "Look-ahead peak limiter that never passes its ceiling"},
    {"filter", "Alien Filter", "Resonant low-pass, high-pass, band-pass and notch filter, 12 or 24 dB/oct"},
    {"saturator", "Alien Saturator", "Oversampled soft, warm or hard saturation with tone and mix"},
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
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
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
    void getStateInformation(juce::MemoryBlock& destination) override {
        if (auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, destination);
    }
    void setStateInformation(const void* data, int size) override {
        // Anything that is not this effect's own state is ignored, not trusted.
        if (auto xml = getXmlFromBinary(data, size))
            if (xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
    double currentSampleRate() const noexcept { return rate.load(); }

protected:
    Effect(const Info& effectInformation, Layout layout)
        : juce::AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                                .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          parameters(*this, nullptr, juce::String("ALIEN_") + juce::String(effectInformation.id).toUpperCase(), std::move(layout)),
          info(effectInformation) {}
    double value(std::atomic<float>* p) const noexcept { return static_cast<double>(p->load(std::memory_order_relaxed)); }
    std::atomic<float>* raw(const char* id) const { return parameters.getRawParameterValue(id); }
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
        for (std::size_t i = 0; i < pointers.size(); ++i) pointers[i] = raw(names()[i]);
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
    std::array<std::atomic<float>*, 11> pointers{};
    dsp::ParametricEq equaliser;
};

// ---- Compressor ---------------------------------------------------------------

class CompressorEffect final : public Effect {
public:
    CompressorEffect() : Effect(catalog[1], layout()) {
        threshold = raw(comp::threshold); ratio = raw(comp::ratio); attack = raw(comp::attack); release = raw(comp::release);
        knee = raw(comp::knee); makeup = raw(comp::makeup); mix = raw(comp::mix);
    }
    void prepareToPlay(double sampleRate, int) override { rate = sampleRate; compressor.prepare(sampleRate); prepared = true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        if (!prepared || buffer.getNumChannels() < 2) return;
        float* left = buffer.getWritePointer(0);
        float* right = buffer.getWritePointer(1);
        sanitise(left, right, buffer.getNumSamples());
        compressor.process(left, right, buffer.getNumSamples(), settings());
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
    std::atomic<float> *threshold{}, *ratio{}, *attack{}, *release{}, *knee{}, *makeup{}, *mix{};
    std::atomic<float> reduction{0.0f};
    dsp::Compressor compressor;
};

// ---- Saturator ----------------------------------------------------------------

class SaturatorEffect final : public Effect {
public:
    SaturatorEffect() : Effect(catalog[6], layout()) {
        drive = raw(saturator::drive); character = raw(saturator::character); tone = raw(saturator::tone);
        mix = raw(saturator::mix); output = raw(saturator::output);
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
    std::atomic<float> *drive{}, *character{}, *tone{}, *mix{}, *output{};
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
        size = raw(reverb::size); damping = raw(reverb::damping); width = raw(reverb::width);
        preDelay = raw(reverb::preDelay); mix = raw(reverb::mix);
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
    std::atomic<float> *size{}, *damping{}, *width{}, *preDelay{}, *mix{};
    juce::Reverb engine;
    dsp::FractionalDelayLine preLeft, preRight;
    std::array<float, scratchSize> wetLeft{}, wetRight{};
};

// ---- Delay --------------------------------------------------------------------

class DelayEffect final : public Effect {
public:
    DelayEffect() : Effect(catalog[3], layout()) {
        time = raw(delay::time); sync = raw(delay::sync); division = raw(delay::division); feedback = raw(delay::feedback);
        pingPong = raw(delay::pingPong); tone = raw(delay::tone); mix = raw(delay::mix);
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
    std::atomic<float> *time{}, *sync{}, *division{}, *feedback{}, *pingPong{}, *tone{}, *mix{};
    std::atomic<double> lastTimeMs{350.0};
    dsp::StereoDelay engine;
};

// ---- Limiter ------------------------------------------------------------------

class LimiterEffect final : public Effect {
public:
    LimiterEffect() : Effect(catalog[4], layout()) {
        gain = raw(limiter::gain); ceiling = raw(limiter::ceiling); release = raw(limiter::release);
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
    std::atomic<float> *gain{}, *ceiling{}, *release{};
    std::atomic<float> reduction{0.0f};
    dsp::Limiter engine;
};

// ---- Filter -------------------------------------------------------------------

class FilterEffect final : public Effect {
public:
    FilterEffect() : Effect(catalog[5], layout()) {
        mode = raw(filter::mode); slope = raw(filter::slope); cutoff = raw(filter::cutoff); resonance = raw(filter::resonance);
        mix = raw(filter::mix); output = raw(filter::output);
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
    std::atomic<float> *mode{}, *slope{}, *cutoff{}, *resonance{}, *mix{}, *output{};
    dsp::MultimodeFilter engine;
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
