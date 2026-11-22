#pragma once
#include "Timeline.h"
#include <algorithm>
#include <cstdint>

namespace alien::sample_preview {
struct TempoTarget {
    double bpm{120};
    bool song{};
};

// Message-thread context for a bounded, fixed-tempo preview. Song playback publishes its
// wrapped playhead; while stopped (or before the first position arrives), use the editor cursor.
inline TempoTarget tempoTarget(const timeline::Timeline& song, double projectBpm, bool songMode,
    bool playing, std::int64_t position, std::int64_t cursor) noexcept {
    if (!songMode) return {projectBpm, false};
    const auto tick = std::max<std::int64_t>(0, playing && position >= 0 ? position : cursor);
    return {timeline::TempoMap{song.tempoChanges, projectBpm}.tempoAt(static_cast<double>(tick)), true};
}
} // namespace alien::sample_preview
