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

## Recorded Windows evidence

Measured 2026-10-29 on Intel Core Ultra 9 288V, Windows x64,
MSVC 14.51.36231 (Visual Studio 18 2026). Tested implementation commit:
`dc79146599241f3dc4c341365d49f56c16c8c17b` (based on main `a54622a8`).
The following documentation-only update changes no tested code.

- Release: all 15 affected suites passed (84.45 seconds): SessionProjectTests,
  TimelinePlaybackTests, RackIntegrationTests, ProjectControllerTests,
  RackProjectTests, TimelineViewTests, SessionViewTests, ArrangementTests,
  TimelineTests, ClipLibraryTests, RackLibraryTests, RackModelTests,
  RackEngineTests, RackLaunchTests and DeterministicStreamedExportTests.
- Debug: RackIntegrationTests, ProjectControllerTests and RackLaunchTests passed
  (3/3, 20.09 seconds). TrackCapacityReport built and ran in both configurations.
- Release callback report: all 15 workload/block-size combinations produced
  finite output (audible for active cases) with zero measured C++ new/new[]
  allocations. Existing regression tests additionally exercise launch, stop,
  source publication and ownership changes; the timing fixture measures steady
  warmed playback, not a complete transition or saturation stress test.
- Full local solution build was interrupted and is not claimed as verified.
  Full Windows Debug/Release application builds and the complete 68-test suite
  remain CI gates on PR #148. At recording time both fault-guard jobs passed;
  the three main build jobs were still running. Windows hardware listening,
  commercial plugins, MIDI devices and DPI checks remain pending in
  WINDOWS_AUDIO_CHECKLIST.md.

### Compiled memory costs

These are selected structural sizes, not process memory. Release output:

| Compiled object | Bytes |
| --- | ---: |
| PianoRollPattern | 32784 |
| PianoRollMailbox (three note buffers) | 98368 |
| SessionClipMailbox (three launch buffers) | 98416 |
| PianoRollScheduler (owned note/event state) | 149128 |
| Selected note state per configured Session track | 345912 |
| ArrangementClip (source references) | 24 |
| ArrangementPlayback (shallow object; not its pointees) | 12544 |
| rack::RackPlayer (shallow) | 1384 |
| rack::RackPlayer fixed heap pools | 5046272 |
| rack::Pattern (shallow object; not pointed-to note rows) | 67080 |
| rack::Snapshot (shallow object; not its pattern/note pointees) | 2584 |

The three selected note-state objects consume 5,534,592 bytes (5.278 MiB) at
16 tracks, even when tracks are inactive. Three Song source-reference tables
consume 36,864 bytes, excluding headers and shared payloads. The rack's fixed
heap pools consume 5,046,272 bytes (4.813 MiB), shared by all 17 launch and four
Song slots; 64 voices and 66,048 pending hits remain shared bounds. These costs
are separate from media, plugin/delay/stream allocations, document/history,
allocator overhead and UI. They must not be presented as total engine memory.

Debug reported the same selected per-track and rack-pool costs; its shallow
ArrangementPlayback object was 12,616 bytes versus Release's 12,544 bytes.
The earlier, separately dated TRACK_CAPACITY_BASELINE.md remains historical.

### Release callback costs

One offline run, after local compiler and regression jobs had ended. Other
Windows desktop activity was uncontrolled. The Session MIDI case uses the
default Session synth preset 0; the rack cases use preset 9. Each row has its
own freshly prepared fixture, 32 warm-up callbacks and 256 measured callbacks.
The allocations column uses the same thread-local C++ allocation counter as
RackIntegrationTests; it is not a driver or third-party allocator measurement.

| Workload | Block frames | p50 us | p95 us | max us | p95 / block budget % | Callback allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips | 64 | 66.00 | 78.40 | 148.00 | 5.88 | 0 |
| Transport, no clips | 256 | 243.80 | 294.50 | 394.10 | 5.52 | 0 |
| Transport, no clips | 1024 | 988.60 | 1142.10 | 1485.70 | 5.35 | 0 |
| 1 Session rack occurrence | 64 | 71.50 | 83.20 | 132.90 | 6.24 | 0 |
| 1 Session rack occurrence | 256 | 267.10 | 320.60 | 389.10 | 6.01 | 0 |
| 1 Session rack occurrence | 1024 | 1078.10 | 1265.10 | 1671.80 | 5.93 | 0 |
| 16 Session rack occurrences | 64 | 136.80 | 170.40 | 291.90 | 12.78 | 0 |
| 16 Session rack occurrences | 256 | 539.00 | 625.10 | 789.10 | 11.72 | 0 |
| 16 Session rack occurrences | 1024 | 2226.50 | 2668.30 | 3355.90 | 12.51 | 0 |
| 4 Song rack occurrences | 64 | 82.00 | 98.90 | 141.00 | 7.42 | 0 |
| 4 Song rack occurrences | 256 | 313.30 | 366.50 | 477.50 | 6.87 | 0 |
| 4 Song rack occurrences | 1024 | 1262.00 | 1466.20 | 1671.90 | 6.87 | 0 |
| 16 Session MIDI tracks | 64 | 93.10 | 107.30 | 147.70 | 8.05 | 0 |
| 16 Session MIDI tracks | 256 | 351.20 | 445.60 | 666.40 | 8.36 | 0 |
| 16 Session MIDI tracks | 1024 | 1419.00 | 2093.50 | 3645.70 | 9.81 | 0 |

Callback budget is frames / 48,000 seconds. Percentages and observed maxima
are descriptive of this run; neither these sparse chord workloads nor their
zero-allocation results establish a higher supported limit or physical-device
latency. No before/after speedup is inferred from this one implementation run.

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
