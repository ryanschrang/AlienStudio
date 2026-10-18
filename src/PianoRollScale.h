#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>

namespace alien {

enum class ScaleType { major, naturalMinor, harmonicMinor, melodicMinor, dorian,
                       mixolydian, majorPentatonic, minorPentatonic, blues, chromatic };

struct ScaleSettings {
    int rootPitchClass {};
    ScaleType type { ScaleType::major };
    bool highlight {};
    bool snap {};
};

inline std::uint16_t scaleMask(ScaleType type) noexcept {
    const auto mask = [](std::initializer_list<int> intervals) {
        std::uint16_t result = 0;
        for (const auto interval : intervals) result |= static_cast<std::uint16_t>(1u << interval);
        return result;
    };
    switch (type) {
        case ScaleType::major:           return mask({0, 2, 4, 5, 7, 9, 11});
        case ScaleType::naturalMinor:    return mask({0, 2, 3, 5, 7, 8, 10});
        case ScaleType::harmonicMinor:   return mask({0, 2, 3, 5, 7, 8, 11});
        case ScaleType::melodicMinor:    return mask({0, 2, 3, 5, 7, 9, 11});
        case ScaleType::dorian:          return mask({0, 2, 3, 5, 7, 9, 10});
        case ScaleType::mixolydian:      return mask({0, 2, 4, 5, 7, 9, 10});
        case ScaleType::majorPentatonic: return mask({0, 2, 4, 7, 9});
        case ScaleType::minorPentatonic: return mask({0, 3, 5, 7, 10});
        case ScaleType::blues:           return mask({0, 3, 5, 6, 7, 10});
        case ScaleType::chromatic:       return 0x0fff;
    }
    return 0;
}

inline bool isPitchInScale(int pitch, const ScaleSettings& settings) noexcept {
    if (pitch < 0 || pitch > 127 || settings.rootPitchClass < 0 || settings.rootPitchClass > 11) return false;
    const int relative = (pitch - settings.rootPitchClass + 12) % 12;
    return (scaleMask(settings.type) & static_cast<std::uint16_t>(1u << relative)) != 0;
}

// Direction chooses the next scale tone for deliberate up/down motion. A zero
// direction chooses the nearest tone and resolves equal distance downward.
inline int snapPitchToScale(int pitch, const ScaleSettings& settings, int direction = 0) noexcept {
    pitch = std::clamp(pitch, 0, 127);
    if (settings.rootPitchClass < 0 || settings.rootPitchClass > 11 || scaleMask(settings.type) == 0) return pitch;
    if (isPitchInScale(pitch, settings)) return pitch;
    for (int distance = 1; distance <= 12; ++distance) {
        const int lower = pitch - distance, upper = pitch + distance;
        if (direction > 0 && upper <= 127 && isPitchInScale(upper, settings)) return upper;
        if (direction < 0 && lower >= 0 && isPitchInScale(lower, settings)) return lower;
        if (direction == 0) {
            if (lower >= 0 && isPitchInScale(lower, settings)) return lower;
            if (upper <= 127 && isPitchInScale(upper, settings)) return upper;
        }
    }
    for (int distance = 1; distance <= 12; ++distance) {
        const int fallback = direction > 0 ? pitch - distance : pitch + distance;
        if (fallback >= 0 && fallback <= 127 && isPitchInScale(fallback, settings)) return fallback;
    }
    return pitch;
}

} // namespace alien
