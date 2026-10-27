#pragma once
namespace alien {
inline constexpr int maxSessionTracks = 16;
inline constexpr int maxSessionScenes = 32;
// Return track slots A-H: every track can send to each return in use, and each has its own FX rack.
// A slot keeps its letter while other returns come and go, so a track's send to it never moves.
// Eight is pre-allocated (docs/FEATURE_CHECKLIST.md, MIX-05: what each slot costs); returns A-D
// exist in a new project, and in every project saved before returns could be added and removed.
inline constexpr int maxReturns = 8;
inline constexpr int defaultReturns = 4;
// Group buses: a track's output can go to one of them instead of the master, and each has its own
// strip and FX rack. Every bus feeds only the master, so routing can never loop.
inline constexpr int maxBuses = 8;
}
