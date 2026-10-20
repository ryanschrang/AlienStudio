#pragma once
// A small synthesized drum kit, so a new Channel Rack makes a sound before any
// sample has been loaded. Every sound is generated, deterministically, from
// maths in this file: no recordings, no third-party material, no files to
// lose. Sounds are mono at 44.1 kHz and are played back at the device rate by
// the rack's resampling voices.
#include "SessionAudioClip.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace alien::rack {

struct BuiltinSound { const char* id; const char* name; };
inline constexpr std::array<BuiltinSound, 8> builtinSounds{{
    {"kick", "Kick"}, {"snare", "Snare"}, {"clap", "Clap"}, {"hat", "Closed hat"},
    {"openhat", "Open hat"}, {"tom", "Tom"}, {"rim", "Rim shot"}, {"cowbell", "Cowbell"}}};

namespace detail {
inline constexpr double soundRate = 44100.0;
inline constexpr double twoPi = 6.283185307179586476925286766559;

// Deterministic white noise in [-1, 1).
struct Noise {
    std::uint32_t state;
    explicit Noise(std::uint32_t seed) : state(seed) {}
    double next() noexcept {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<double>(state) / 2147483648.0 - 1.0;
    }
};
struct OnePole {
    double a, z{};
    OnePole(double cutoffHz) : a(1.0 - std::exp(-twoPi * cutoffHz / soundRate)) {}
    double low(double x) noexcept { z += a * (x - z); return z; }
    double high(double x) noexcept { return x - low(x); }
};
inline double square(double phase) noexcept { return std::fmod(phase, 1.0) < 0.5 ? 1.0 : -1.0; }

// Calls sample(t) for every sample of a sound lasting `seconds`, fades the last
// few milliseconds so the sound ends in exact silence, and scales the peak to 0.9.
template <class Fn> std::vector<float> render(double seconds, Fn&& sample) {
    const auto frames = static_cast<std::size_t>(seconds * soundRate);
    std::vector<double> raw(frames);
    for (std::size_t i = 0; i < frames; ++i) raw[i] = sample(static_cast<double>(i) / soundRate);
    const auto fade = static_cast<std::size_t>(0.006 * soundRate);
    for (std::size_t i = 0; i < fade && i < frames; ++i) raw[frames - 1 - i] *= static_cast<double>(i) / static_cast<double>(fade);
    double peak = 0.0;
    for (double v : raw) peak = std::max(peak, std::abs(v));
    const double scale = peak > 0.0 ? 0.9 / peak : 0.0;
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) out[i] = static_cast<float>(raw[i] * scale);
    return out;
}

inline std::vector<float> kick() {
    double phase = 0.0; Noise noise(0x4b1c4u); OnePole softener(1800.0);
    return render(0.5, [&](double t) {
        phase += (45.0 + 115.0 * std::exp(-t / 0.035)) / soundRate;
        const double body = std::tanh(1.8 * std::sin(twoPi * phase) * std::exp(-t / 0.12));
        const double click = softener.low(noise.next()) * std::exp(-t / 0.002) * 0.35;
        return body + click;
    });
}
inline std::vector<float> snare() {
    Noise noise(0x5a4eu); OnePole splitter(1500.0);
    return render(0.4, [&](double t) {
        const double tone = (0.6 * std::sin(twoPi * 185.0 * t) + 0.3 * std::sin(twoPi * 330.0 * t)) * std::exp(-t / 0.055);
        const double hiss = splitter.high(noise.next()) * std::exp(-t / 0.09) * 0.9;
        return std::tanh(1.4 * (tone + hiss));
    });
}
inline std::vector<float> clap() {
    Noise noise(0xc1a9u); OnePole lowCut(700.0), highCut(3200.0);
    return render(0.4, [&](double t) {
        const double band = lowCut.high(highCut.low(noise.next()));
        // Three quick slaps and then the room.
        double envelope = std::exp(-std::max(0.0, t - 0.033) / 0.07) * (t >= 0.033 ? 1.0 : 0.0);
        for (double start : {0.0, 0.011, 0.022})
            if (t >= start) envelope = std::max(envelope, std::exp(-(t - start) / 0.006));
        return band * envelope;
    });
}
// 808-style metallic hat: six detuned square oscillators plus noise, high-passed.
inline std::vector<float> hat(double seconds, double decay) {
    static constexpr std::array<double, 6> ratios{205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    std::array<double, 6> phase{}; Noise noise(0x4a7u); OnePole highPass(7000.0);
    return render(seconds, [&](double t) {
        double metal = 0.0;
        for (std::size_t i = 0; i < ratios.size(); ++i) { phase[i] += ratios[i] * 2.0 / soundRate; metal += square(phase[i]); }
        return highPass.high(metal * 0.25 + noise.next() * 0.5) * std::exp(-t / decay);
    });
}
inline std::vector<float> tom() {
    double phase = 0.0;
    return render(0.5, [&](double t) {
        phase += (110.0 + 110.0 * std::exp(-t / 0.05)) / soundRate;
        return std::sin(twoPi * phase) * std::exp(-t / 0.13);
    });
}
inline std::vector<float> rim() {
    Noise noise(0x71cu);
    return render(0.12, [&](double t) {
        const double ring = (std::sin(twoPi * 1700.0 * t) + 0.6 * std::sin(twoPi * 480.0 * t)) * std::exp(-t / 0.012);
        return ring + noise.next() * std::exp(-t / 0.0006) * 0.5;
    });
}
inline std::vector<float> cowbell() {
    double a = 0.0, b = 0.0; OnePole smooth(4200.0);
    return render(0.5, [&](double t) {
        a += 540.0 / soundRate; b += 800.0 / soundRate;
        const double attack = std::min(1.0, t / 0.001);
        return smooth.low(square(a) + square(b)) * attack * std::exp(-t / 0.1);
    });
}
} // namespace detail

// Synthesizes one sound; nullptr for an unknown id.
inline std::shared_ptr<SessionAudioClip> synthesizeSound(std::string_view id) {
    std::vector<float> samples;
    if (id == "kick") samples = detail::kick();
    else if (id == "snare") samples = detail::snare();
    else if (id == "clap") samples = detail::clap();
    else if (id == "hat") samples = detail::hat(0.15, 0.022);
    else if (id == "openhat") samples = detail::hat(0.7, 0.2);
    else if (id == "tom") samples = detail::tom();
    else if (id == "rim") samples = detail::rim();
    else if (id == "cowbell") samples = detail::cowbell();
    else return nullptr;
    auto clip = std::make_shared<SessionAudioClip>();
    clip->sampleRate = detail::soundRate;
    clip->channels = 1;
    clip->frames = samples.size();
    clip->left = std::move(samples);
    clip->buildWaveform();
    return clip;
}

// The shared, immortal instance of a built-in sound: generated once, the first
// time it is asked for, and never freed, so the audio thread can hold a raw
// pointer to it for as long as the program runs. Message thread only.
inline std::shared_ptr<const SessionAudioClip> builtinClip(std::string_view id) {
    static std::mutex guard;
    static std::map<std::string, std::shared_ptr<const SessionAudioClip>, std::less<>> cache;
    std::lock_guard<std::mutex> lock(guard);
    if (auto found = cache.find(id); found != cache.end()) return found->second;
    auto made = synthesizeSound(id);
    if (made == nullptr) return nullptr;
    return cache.emplace(std::string(id), std::move(made)).first->second;
}

inline const char* builtinDisplayName(std::string_view id) {
    for (const auto& sound : builtinSounds) if (id == sound.id) return sound.name;
    return nullptr;
}

} // namespace alien::rack
