# Shared playback capacity handoff (T1-03 E)

This increment keeps format 28 and the existing capacities. It removes document
rack source-index ownership fallbacks; legacy loading and generated runtime
bindings remain supported. See SHARED_CLIP_MODEL.md for the boundaries.

## Reproduce

From a configured Windows x64 build with BUILD_TESTING=ON:

```powershell
cmake --build build --config Release --target TrackCapacityReport RackIntegrationTests
./build/Release/TrackCapacityReport.exe
./build/Release/RackIntegrationTests.exe --capacity-report
```

Use the same commands with Debug to inspect that configuration. The callback
report constructs the full engine offline, warms each independent workload and
measures 256 callbacks after 32 warm-up blocks at each block size. The 16-track
MIDI workload plays a three-note looping chord on each Session synth. Rack cases
share a three-note source on stock preset 9. All 16 mixer tracks and four default
returns remain configured; unused rack rows are silent. There are no plugins,
audio media or physical devices in these fixtures. The Song lasts 64 bars so the
measurement never reaches its end. Finite, audible output and zero allocations
are required; timing percentiles have no pass threshold. Normal CTest execution
runs the regression suite without the opt-in timing workload.

## Validation

The feature PR records exact tested commits and configurations. Complete local
Release regression, Debug checks and final measurements are in progress. An
initial focused Release run passed RackLaunchTests, RackIntegrationTests,
RackProjectTests and ProjectControllerTests. Hardware listening, plugin, MIDI
device and DPI checks remain pending in WINDOWS_AUDIO_CHECKLIST.md.

## T1-04 decision

Keep immutable library content and ID-based document ownership. Compile
track/device/routing bindings into bounded engine slots rather than introducing
another writable note store. Session cell coordinates and the low-level legacy
index adapter are bindings, not content owners. Existing per-track mailboxes and
scheduler arrays still contain fixed note/event storage; right-size or replace
those before a large track/note profile. The rack's 64 voices and shared pending
queue must not be multiplied by the number of launch occurrences.

Offline timings and selected compiled sizes do not establish process memory,
physical-device latency or a safe higher limit. Measure prepared/full engine and
peak load/undo/export memory; dense note/ratchet/media/FX loads; larger proposed
profiles; publication and reclamation; and representative physical Windows
hardware before changing a supported capacity. Universal track identity and
routing migration remains T1-04 B/C; this increment does not raise a limit.
