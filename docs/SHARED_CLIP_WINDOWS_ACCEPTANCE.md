# Shared clip Windows acceptance

Workstream T1-03 / T1-12, ARCH-01 / QA-03. Related issue #144.

## Scope

Verify shared content across Create, Perform and Arrange, independent overlapping rack occurrences, note ownership, persistence, Undo/Redo, and live-render versus WAV parity.

## Baseline and environment

Baseline is main at `9ae3643f804639bebc57f5b075857210d8bb3e5b` (T1-04 C3 merged). Windows x64 with Visual Studio 18 2026 and MSVC 14.51.36231. A separate exported source snapshot is used for builds because the workspace sandbox prevents creating branch refs in the primary checkout. JUCE 8.0.10 is reused from the existing dependency checkout.

## Automated verification

The fresh Windows x64 Debug application and all ten selected test targets compiled successfully. The final combined CTest run passed 10/10 tests in 99.28 seconds.

| Suite | Coverage exercised |
| --- | --- |
| ClipLibraryTests | Shared definitions, revisions, linked and independent content |
| RackLibraryTests | Lossless rack/library adapters and stable source identity |
| RackLaunchTests | Beat/bar scheduling, overlapping occurrences, scoped stops and delayed hits |
| RackIntegrationTests | Real offline audio callback, per-row overlap/stop, lifecycle and allocation checks |
| RackProjectTests | Round trips, legacy files and live-render/WAV overlap parity |
| SessionViewTests | Actual Session callbacks, linked assignments, row state and refusal paths |
| TimelineViewTests | Song editing entry points and placement behavior |
| ProjectControllerTests | Shared identity, independent copies, history and controller validation |
| ToneEngineTests | Offline device callback, stop/restart and voice cleanup |
| MidiInputTests | MIDI parsing, sustain, queue and simulated device lifecycle |

```powershell
$qaSource = 'C:/Users/ryans/AlienStudio/.worktrees/t1-03-windows-acceptance'
cmake -S $qaSource -B "$qaSource/build" -G 'Visual Studio 18 2026' -A x64 -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/Users/ryans/AlienStudio/build/_deps/juce-src -DBUILD_TESTING=ON -DCMAKE_SUPPRESS_REGENERATION=ON
cmake --build "$qaSource/build" --config Debug --target AlienStudio ClipLibraryTests RackLibraryTests RackLaunchTests RackIntegrationTests RackProjectTests SessionViewTests TimelineViewTests ProjectControllerTests ToneEngineTests MidiInputTests --parallel 2 -- /nr:false
ctest --test-dir "$qaSource/build" -C Debug -R '^(ClipLibrary|RackLibrary|RackLaunch|RackIntegration|RackProject|SessionView|TimelineView|ProjectController|ToneEngine|MidiInput)Tests

## Windows interactive verification

Pending. Launch through the computer-use integration returned `Computer Use app approval timed out`. No app interaction or visual behavior has been verified. The device inventory query was refused, so interface, driver, sample rate, buffer size and MIDI-controller identity are unverified.

The manual D1/D2/D3 procedures remain in `docs/WINDOWS_AUDIO_CHECKLIST.md`. Record an exact build commit and the device configuration before performing them.

- Launch distinct sources in one scene with Beat and Bar quantization.
- Launch the same source and pitch on two rows, stop one, and confirm the other continues.
- Exercise nudges, ratchets, fractional notes, choke groups and sample voices during replacement.
- Edit linked content from Create, Perform and Arrange, then Undo/Redo. Verify independent copies and Make unique.
- Change the editor selection while playing and confirm the playing source stays selected for playback.
- Overlap Song rack lanes, trim/move/mute/remove/replace one occurrence, seek and loop, and compare a WAV export with playback.
- Save/reopen new and format 26/27 projects, preserving legacy duplicate-source scene behavior.
- Hold notes through stop/panic, transport restart, document replacement and MIDI/audio-device changes.
- Inspect queued/playing row indicators at minimum window size and 150/200% display scaling.

ARCH-01 and QA-03 remain pending hardware and listening acceptance.
 --output-on-failure --no-tests=error
```

The first local build stopped making progress during CMake build-system regeneration and was interrupted. Reconfiguration with `CMAKE_SUPPRESS_REGENERATION=ON` allowed the fresh build to complete. This setting applies only to the isolated build directory. Build output includes MSVC C4244 float-conversion warnings traced to `StreamingTempoWarp.h`, plus JUCE bundle-ID warnings at configure time. No source or CMake project configuration was edited.

Full GitHub Actions Debug/Release builds and tests were still running when the local results were recorded. They must be checked on the current PR head before merge. Local Release testing was not performed.

These suites exercise existing automated scenarios. Physical-device behavior, audible playback and manual visual acceptance remain pending.

## Windows interactive verification

Pending. Launch through the computer-use integration returned `Computer Use app approval timed out`. No app interaction or visual behavior has been verified. The device inventory query was refused, so interface, driver, sample rate, buffer size and MIDI-controller identity are unverified.

The manual D1/D2/D3 procedures remain in `docs/WINDOWS_AUDIO_CHECKLIST.md`. Record an exact build commit and the device configuration before performing them.

- Launch distinct sources in one scene with Beat and Bar quantization.
- Launch the same source and pitch on two rows, stop one, and confirm the other continues.
- Exercise nudges, ratchets, fractional notes, choke groups and sample voices during replacement.
- Edit linked content from Create, Perform and Arrange, then Undo/Redo. Verify independent copies and Make unique.
- Change the editor selection while playing and confirm the playing source stays selected for playback.
- Overlap Song rack lanes, trim/move/mute/remove/replace one occurrence, seek and loop, and compare a WAV export with playback.
- Save/reopen new and format 26/27 projects, preserving legacy duplicate-source scene behavior.
- Hold notes through stop/panic, transport restart, document replacement and MIDI/audio-device changes.
- Inspect queued/playing row indicators at minimum window size and 150/200% display scaling.

ARCH-01 and QA-03 remain pending hardware and listening acceptance.
