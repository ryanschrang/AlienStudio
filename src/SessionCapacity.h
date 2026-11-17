#pragma once
// Session track and scene capacity (T1-04). The supported profile is 16 tracks and 32 scenes.
// A build can compile another profile for measurement and testing by defining
// ALIEN_MAX_SESSION_TRACKS and ALIEN_MAX_SESSION_SCENES (CMake: -DALIEN_SESSION_TRACKS=32
// -DALIEN_SESSION_SCENES=64); see docs/UNIVERSAL_TRACKS.md, increment C3. Only the default
// profile is supported for release until the T1-04 D/E gates pass.
#ifndef ALIEN_MAX_SESSION_TRACKS
#define ALIEN_MAX_SESSION_TRACKS 16
#endif
#ifndef ALIEN_MAX_SESSION_SCENES
#define ALIEN_MAX_SESSION_SCENES 32
#endif
namespace alien {
inline constexpr int maxSessionTracks = ALIEN_MAX_SESSION_TRACKS;
inline constexpr int maxSessionScenes = ALIEN_MAX_SESSION_SCENES;
// The supported release profile, which saved projects and the documentation assume today.
inline constexpr int supportedSessionTracks = 16;
inline constexpr int supportedSessionScenes = 32;
inline constexpr bool isSupportedCapacityProfile = maxSessionTracks == supportedSessionTracks
                                                && maxSessionScenes == supportedSessionScenes;
// Scene launches wait on a 64-bit mask of tracks; formats 8-31 numbered rack lanes after 16 rows.
static_assert(maxSessionTracks >= supportedSessionTracks && maxSessionTracks <= 64,
              "Session track capacity must be between 16 and 64");
static_assert(maxSessionScenes >= supportedSessionScenes && maxSessionScenes <= 256,
              "Session scene capacity must be between 32 and 256");
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
