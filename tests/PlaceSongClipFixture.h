#pragma once
#include "CommandEngine.h"

namespace alien::test {
// Fixture convenience only: commands own mutation/history, while the existing
// read-only metadata helper identifies the accepted placement for later checks.
inline bool placeSongClip(ProjectController& controller, int row, int source, std::int64_t start,
                          std::uint32_t* placed = nullptr) {
    const auto before = controller.timeline();
    const auto result = command::CommandEngine(controller).execute(command::PlaceSongClip{row,source,start});
    if (!result.ok || !result.changed) return false;
    if (placed) *placed = song_clip::placedClip(controller.document(),before,controller.timeline(),row,source,start);
    return true;
}
} // namespace alien::test
