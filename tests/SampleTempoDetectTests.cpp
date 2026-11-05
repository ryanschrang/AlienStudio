#include "SampleTempoDetect.h"
#include <cstdlib>
#include <iostream>
#include <random>

static void require(bool ok, const std::string& message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}

namespace {
using Audio = std::vector<float>;
std::atomic<bool> running{false};

Audio silence(double seconds, double rate) { return Audio(static_cast<std::size_t>(seconds * rate), 0.f); }

// A short decaying tone with a noisy attack, like a drum hit.
void hit(Audio& audio, double second, double rate, float level, double frequency, double decay, std::mt19937& random) {
    std::uniform_real_distribution<float> noise(-1, 1);
    const auto start = static_cast<std::size_t>(second * rate);
    const auto length = static_cast<std::size_t>(decay * 5 * rate);
    for (std::size_t n = 0; n < length && start + n < audio.size(); ++n) {
        const double t = static_cast<double>(n) / rate;
        const auto envelope = static_cast<float>(std::exp(-t / decay));
        const auto tone = static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * frequency * t));
        audio[start + n] += level * envelope * (tone * .7f + noise(random) * .3f);
    }
}

Audio clicks(double bpm, double seconds, double rate, float level = .8f) {
    std::mt19937 random(7); auto audio = silence(seconds, rate);
    for (double at = 0; at < seconds; at += 60 / bpm) hit(audio, at, rate, level, 180, .02, random);
    return audio;
}

// Kick on one and three, snare on two and four, hats on every eighth.
Audio drumLoop(double bpm, double seconds, double rate, float noiseLevel = 0) {
    std::mt19937 random(11); auto audio = silence(seconds, rate);
    const double eighth = 30 / bpm; int step = 0;
    for (double at = 0; at < seconds; at += eighth, ++step) {
        hit(audio, at, rate, .15f, 7000, .012, random);
        if (step % 8 == 0 || step % 8 == 4) hit(audio, at, rate, .9f, 60, .06, random);
        if (step % 8 == 2 || step % 8 == 6) hit(audio, at, rate, .6f, 220, .04, random);
    }
    std::uniform_real_distribution<float> noise(-1, 1);
    if (noiseLevel > 0) for (auto& sample : audio) sample += noise(random) * noiseLevel;
    return audio;
}

std::optional<alien::TempoEstimate> estimate(const Audio& audio, double rate) {
    return alien::estimateTempo(audio.data(), audio.size(), rate, running);
}

void requireTempo(const Audio& audio, double rate, double bpm, double tolerance, const std::string& name) {
    const auto found = estimate(audio, rate);
    std::cout << name << ": " << (found ? juce::String(found->bpm, 1) + " BPM, confidence " + juce::String(found->confidence, 2) : juce::String("none")) << "\n";
    require(found && std::abs(found->bpm - bpm) <= tolerance, name + " expected " + std::to_string(bpm));
}

void requireNone(const Audio& audio, double rate, const std::string& name) {
    const auto found = estimate(audio, rate);
    std::cout << name << ": " << (found ? juce::String(found->bpm, 1) + " BPM, confidence " + juce::String(found->confidence, 2) : juce::String("none")) << "\n";
    require(!found, name + " should find no tempo");
}
} // namespace

int main() {
    const double rate = 44100;
    for (const double bpm : {90.0, 100.0, 120.0, 128.0, 140.0, 150.0, 170.0})
        requireTempo(clicks(bpm, 20, rate), rate, bpm, .5, "clicks at " + std::to_string(bpm));
    requireTempo(clicks(127.3, 30, rate), rate, 127.3, .3, "a tempo between whole numbers");
    for (const double other : {8000.0, 22050.0, 48000.0, 96000.0})
        requireTempo(clicks(128, 15, other), other, 128, .5, "sample rate " + std::to_string(other));
    requireTempo(clicks(128, 15, rate, .03f), rate, 128, .5, "a quiet loop");

    for (const double bpm : {100.0, 128.0, 140.0, 170.0})
        requireTempo(drumLoop(bpm, 20, rate), rate, bpm, .5, "drum loop at " + std::to_string(bpm));
    requireTempo(drumLoop(128, 20, rate, .03f), rate, 128, .5, "a drum loop over noise");
    requireTempo(drumLoop(128, 3.75, rate), rate, 128, 1, "a two-bar loop");

    // Documented limit: a steady beat fits its octaves about equally, so a tempo below 88 can report its double.
    requireTempo(drumLoop(80, 20, rate), rate, 160, .5, "a drum loop at 80 reports its double");
    requireTempo(clicks(80, 20, rate), rate, 80, .5, "a plain pulse at 80 has nothing between its beats, so it stays at 80");
    requireTempo(clicks(190, 20, rate), rate, 95, .5, "a plain pulse at 190 reports its half");

    requireNone(silence(10, rate), rate, "silence");
    {
        std::mt19937 random(3); std::uniform_real_distribution<float> noise(-.5f, .5f);
        auto audio = silence(10, rate); for (auto& sample : audio) sample = noise(random);
        requireNone(audio, rate, "white noise");
    }
    for (const double frequency : {50.0, 110.0, 440.0}) {
        auto audio = silence(10, rate);
        for (std::size_t n = 0; n < audio.size(); ++n) audio[n] = .5f * static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * frequency * static_cast<double>(n) / rate));
        requireNone(audio, rate, "a steady tone at " + std::to_string(frequency));
    }
    {
        std::mt19937 random(5); auto audio = silence(10, rate);
        hit(audio, .1, rate, .9f, 80, .4, random);
        requireNone(audio, rate, "one long hit");
    }
    {
        for (const unsigned seed : {21u, 22u, 23u, 24u, 25u}) {
            std::mt19937 random(seed); std::uniform_real_distribution<double> when(0, 20); auto audio = silence(20, rate);
            for (int i = 0; i < 60; ++i) hit(audio, when(random), rate, .7f, 200, .02, random);
            requireNone(audio, rate, "hits at random times, seed " + std::to_string(seed));
        }
    }
    requireNone(clicks(128, 2.5, rate), rate, "a loop under three seconds");
    {
        Audio audio(static_cast<std::size_t>(10 * rate), std::numeric_limits<float>::quiet_NaN());
        requireNone(audio, rate, "non-finite samples");
    }
    require(!alien::estimateTempo(nullptr, 0, rate, running) && !alien::estimateTempo(clicks(128, 5, rate).data(), 0, rate, running), "no audio finds nothing");
    {
        const auto audio = clicks(128, 15, rate); std::atomic<bool> stopped{true};
        require(!alien::estimateTempo(audio.data(), audio.size(), rate, stopped), "a cancelled estimate finds nothing");
    }
    std::cout << "PASS: sample tempo detection\n";
    return 0;
}
