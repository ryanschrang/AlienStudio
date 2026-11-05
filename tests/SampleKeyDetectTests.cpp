#include "SampleKeyDetect.h"
#include "SamplePreviewRender.h"
#include <cstdlib>
#include <iostream>
#include <random>

static void require(bool ok, const std::string& message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}

namespace {
using Audio = std::vector<float>;
constexpr double pi2 = juce::MathConstants<double>::twoPi;

// A tone with a few harmonics, as an instrument has, faded in and out to avoid clicks.
void note(Audio& audio, double start, double length, double rate, double midi, float level, double cents = 0) {
    const double frequency = 440 * std::pow(2.0, (midi + cents / 100 - 69) / 12.0);
    const auto first = static_cast<std::size_t>(start * rate);
    const auto count = static_cast<std::size_t>(length * rate);
    for (std::size_t n = 0; n < count && first + n < audio.size(); ++n) {
        const double t = static_cast<double>(n) / rate;
        const double fade = std::min({1.0, t / .01, (length - t) / .05});
        double sample = 0;
        for (int partial = 1; partial <= 4; ++partial) sample += std::sin(pi2 * frequency * partial * t) / (partial * partial);
        audio[first + n] += level * static_cast<float>(fade * sample);
    }
}

// A chord loop with a bass note under each chord. Each chord is given as semitones above the key's root.
Audio chordLoop(int root, const std::vector<std::vector<int>>& chords, double rate, double seconds = 16, double cents = 0) {
    Audio audio(static_cast<std::size_t>(seconds * rate), 0.f);
    const double each = 1;
    int step = 0;
    for (double at = 0; at < seconds; at += each, ++step) {
        const auto& chord = chords[static_cast<std::size_t>(step) % chords.size()];
        for (const auto interval : chord) note(audio, at, each, rate, 60 + root + interval, .12f, cents);
        note(audio, at, each, rate, 48 + root + chord[0], .2f, cents);
    }
    return audio;
}

const std::vector<std::vector<int>> majorChords{{0, 4, 7}, {5, 9, 12}, {7, 11, 14}, {0, 4, 7}};      // I IV V I
const std::vector<std::vector<int>> minorChords{{0, 3, 7}, {5, 8, 12}, {7, 10, 14}, {0, 3, 7}};      // i iv v i

// A scale melody that stays near the tonic.
Audio melody(int root, bool minor, double rate) {
    static const std::array<int, 7> majorScale{0, 2, 4, 5, 7, 9, 11}, minorScale{0, 2, 3, 5, 7, 8, 10};
    static const std::array<int, 16> degrees{0, 2, 4, 2, 0, 4, 3, 1, 0, 2, 4, 6, 5, 4, 1, 0};
    const auto& scale = minor ? minorScale : majorScale;
    Audio audio(static_cast<std::size_t>(16 * rate), 0.f);
    for (int i = 0; i < 16; ++i) note(audio, i, 1, rate, 60 + root + scale[static_cast<std::size_t>(degrees[static_cast<std::size_t>(i)])], .4f);
    return audio;
}

juce::String show(const std::optional<alien::KeyEstimate>& key) {
    return key ? juce::String(alien::previewKeyNames[key->root]) + (key->minor ? " minor" : " major") + ", confidence " + juce::String(key->confidence, 2) : juce::String("none");
}

std::optional<alien::KeyEstimate> estimate(const Audio& audio, double rate, const std::string& name) {
    const auto found = alien::estimateKey(audio.data(), audio.size(), rate);
    std::cout << name << ": " << show(found) << "\n";
    return found;
}

void requireKey(const Audio& audio, double rate, int root, bool minor, const std::string& name) {
    const auto found = estimate(audio, rate, name);
    require(found && found->root == root && found->minor == minor, name + " found the wrong key");
}

void requireNone(const Audio& audio, double rate, const std::string& name) {
    require(!estimate(audio, rate, name), name + " should find no key");
}
} // namespace

int main() {
    const double rate = 44100;
    for (int root = 0; root < 12; ++root) {
        const auto name = std::string(alien::previewKeyNames[root]);
        requireKey(chordLoop(root, majorChords, rate), rate, root, false, name + " major chords");
        requireKey(chordLoop(root, minorChords, rate), rate, root, true, name + " minor chords");
    }
    requireKey(melody(7, false, rate), rate, 7, false, "a melody in G major");
    requireKey(melody(9, true, rate), rate, 9, true, "a melody in A minor");
    for (const double other : {8000.0, 22050.0, 48000.0, 96000.0})
        requireKey(chordLoop(2, minorChords, other), other, 2, true, "D minor at " + std::to_string(other));
    requireKey(chordLoop(5, majorChords, rate, 16, 25), rate, 5, false, "F major tuned 25 cents sharp");
    {
        auto quiet = chordLoop(0, majorChords, rate);
        for (auto& sample : quiet) sample *= .05f;
        requireKey(quiet, rate, 0, false, "a quiet loop");
    }
    {
        auto noisy = chordLoop(4, minorChords, rate);
        std::mt19937 random(9); std::uniform_real_distribution<float> noise(-.03f, .03f);
        for (auto& sample : noisy) sample += noise(random);
        requireKey(noisy, rate, 4, true, "E minor over noise");
    }
    {
        // A very long sample keeps working: the collector holds only one window at a time.
        requireKey(chordLoop(0, majorChords, 22050, 60), 22050, 0, false, "a minute of C major");
    }

    require(alien::KeyEstimate{9, true}.relativeRoot() == 0 && alien::KeyEstimate{0, false}.relativeRoot() == 9, "relative keys");
    require(alien::KeyEstimate{0, true}.relativeRoot() == 3 && alien::KeyEstimate{3, false}.relativeRoot() == 0, "relative keys wrap");

    requireNone(Audio(static_cast<std::size_t>(10 * rate), 0.f), rate, "silence");
    {
        std::mt19937 random(3); std::uniform_real_distribution<float> noise(-.5f, .5f);
        Audio audio(static_cast<std::size_t>(10 * rate));
        for (auto& sample : audio) sample = noise(random);
        requireNone(audio, rate, "white noise");
    }
    {
        Audio audio(static_cast<std::size_t>(10 * rate));
        for (std::size_t n = 0; n < audio.size(); ++n) audio[n] = .5f * static_cast<float>(std::sin(pi2 * 440 * static_cast<double>(n) / rate));
        requireNone(audio, rate, "a lone sine tone");
    }
    {
        // Drums have no key: a kick below the analysed range, and bursts of noise.
        std::mt19937 random(11); std::uniform_real_distribution<float> noise(-1, 1);
        Audio audio(static_cast<std::size_t>(16 * rate), 0.f);
        for (double at = 0; at < 16; at += .25) {
            const auto first = static_cast<std::size_t>(at * rate);
            for (std::size_t n = 0; n < static_cast<std::size_t>(.08 * rate) && first + n < audio.size(); ++n) {
                const double t = static_cast<double>(n) / rate;
                audio[first + n] += static_cast<float>(std::exp(-t / .02)) * (.7f * static_cast<float>(std::sin(pi2 * 50 * t)) + .3f * noise(random));
            }
        }
        requireNone(audio, rate, "a drum loop");
    }
    requireNone(chordLoop(0, majorChords, rate, 1.5), rate, "a sample under two seconds");
    {
        Audio audio(static_cast<std::size_t>(5 * rate), std::numeric_limits<float>::quiet_NaN());
        requireNone(audio, rate, "non-finite samples");
    }
    require(!alien::estimateKey(nullptr, 0, rate), "no audio finds nothing");
    std::cout << "PASS: sample key detection\n";
    return 0;
}
