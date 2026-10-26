#pragma once
namespace alien {
inline constexpr int maxSessionTracks = 16;
inline constexpr int maxSessionScenes = 32;
// Return tracks A-D: every track can send to each of them, and each has its own FX rack.
inline constexpr int maxReturns = 4;
// Group buses: a track's output can go to one of them instead of the master, and each has its own
// strip and FX rack. Every bus feeds only the master, so routing can never loop.
inline constexpr int maxBuses = 8;
}
