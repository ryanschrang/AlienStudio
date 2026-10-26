<!-- Recorded 2026-10-26 on Windows x64 with MSVC. Both configurations built and ran successfully. -->

Release output follows. These are compiled-layout measurements, not an audio-device or CPU benchmark.

# Track capacity baseline

Compiler-measured static object sizes on a 64-bit build.
Compiled limits: 16 Session tracks, 32 scenes, 1024 notes per sequence; 64 rack channels and 64 rack patterns.

| Compiled object | Bytes |
| --- | ---: |
| PianoRollPattern | 32784 |
| PianoRollMailbox (three note buffers) | 98368 |
| SessionClipMailbox (three launch buffers) | 98416 |
| PianoRollScheduler (owned note/event state) | 149128 |
| Selected note state per configured Session track | 345912 |
| ArrangementClip (source references) | 24 |
| ArrangementPlayback (shallow object; not its pointees) | 12472 |
| rack::Pattern (shallow object; not pointed-to note rows) | 67080 |
| rack::Snapshot (shallow object; not its pattern/note pointees) | 2072 |

| Profile | Tracks | Scenes | Selected preallocated note state (bytes) | Three Song reference tables (bytes) | Note state (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| current configured limits | 16 | 32 | 5534592 | 36864 | 5.278 |
| hypothetical | 32 | 64 | 11069184 | 147456 | 10.556 |
| hypothetical | 64 | 128 | 22138368 | 589824 | 21.113 |
| hypothetical; not a supported target | 500 | 128 | 172956000 | 4608000 | 164.944 |

These are selected structural costs, not total process/engine memory or CPU measurements.
Note-state bytes = tracks * (sizeof(PianoRollMailbox) + sizeof(SessionClipMailbox) + sizeof(PianoRollScheduler)).
Table bytes = 3 * tracks * scenes * sizeof(ArrangementClip); snapshot headers and pointees are excluded.
Hypothetical rows keep the compiled note limit and current object layout. They do not raise any limits.
Excluded: plugin instances/state, delay-line sample allocations, decoded audio, stream buffers, synth/drum voices,
rack note pointees, document/history metadata, UI, allocator overhead and live/export performance.
The engine preallocates these note objects by configured capacity, even for inactive tracks.
No engine or hypothetical large project was constructed by this report.

## Debug comparison

Both executables report the same selected per-track note-state and reference-table costs.
The build-specific shallow object-size differences are recorded here:

| Object | Debug bytes | Release bytes |
| --- | ---: | ---: |
| ArrangementPlayback (shallow object; not its pointees) | 12520 | 12472 |

Do not assume every Debug and Release type has an identical layout. These shallow
size differences do not measure the dynamically owned resources excluded above.

## Reproduce

From a configured Windows development build with `BUILD_TESTING=ON`:

```powershell
cmake --build build --config Debug --target TrackCapacityReport
.\build\Debug\TrackCapacityReport.exe
cmake --build build --config Release --target TrackCapacityReport
.\build\Release\TrackCapacityReport.exe
```

This records baseline main `1587c17` types, with only the diagnostic/design
increment added. It does not measure CPU, device latency or total memory.
See [the T1-04 plan](UNIVERSAL_TRACKS.md) for the remaining measurement gates.
