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

The user enabled app control for the same fresh Debug executable used for the automated pass. The first launch returned no targetable window. A retry opened the application, and the following checks ran in the Windows UI.

The application reports Windows Audio, Speaker (Realtek(R) Audio), output channels 1–2, 48000 Hz, and a 480-sample / 10.0 ms buffer. Audio input is disabled. The MIDI selector offers None (off) and Test input (no controller). A physical MIDI controller was unavailable. Driver version, Windows build and display scale were not recorded.

| Scenario | Observed result |
| --- | --- |
| Distinct rack sources | Created Pattern 1 with a kick on step 1 and Pattern 2 with a clap on step 5. Assigned them to Track 1 / Scene 1 and Track 2 / Scene 1. Both assignments were accepted. |
| Scene launch with Bar and Beat | Launched the scene under both settings. The two rack tiles entered playback and showed progress. Exact sample timing remains covered by the automated scheduler tests. |
| Scoped row stop | Stopped Track 1 at its row stop button. Its tile lost the playback progress while Track 2 continued progressing. |
| Shared editing from Perform and Arrange | Opening the Session rack tile entered Pattern 1 in Channel Rack. Added kick step 5. Toolbar Undo removed it and Redo restored it. Placed the shared source into Song, then double-clicked that Song region. Pattern 1 opened with steps 1 and 5. |
| Independent copy | Session Copy / link > Make unique changed the assignment to Pattern 3. Added kick step 9 to Pattern 3. Reopened the original Pattern 1 from Song and confirmed it still had only steps 1 and 5. |
| Save and reopen | Saved a new local SharedClipVisualAcceptance.alienstudio, reopened it through the application, and confirmed Session assignments Pattern 3 / Pattern 2. Opened Pattern 3 and confirmed steps 1, 5 and 9 survived. Transport was stopped after reopening. |

The local acceptance session is at `C:/Users/ryans/AlienStudio/.worktrees/t1-03-windows-acceptance/SharedClipVisualAcceptance.alienstudio`. It contains original built-in drum sounds and test edits made during this pass.

### Findings fixed and retested

The follow-up code revision is `7d884e9773cdcf65b3453fdc5a3dde6a310c2c33`.

1. Corrected the successful Session assignment status to explain that each assigned row launches its own occurrence while the source channels keep their routing. Reassigned Pattern 1 in the rebuilt application and observed the corrected text.
2. Exposed Make this placement unique for Song rack placements using the existing `MakeSongClipUnique` command. The reducer, history handling and rack publication already support this operation. In the rebuilt application, the Song region menu created Pattern 4 from Pattern 1. One toolbar Undo restored Pattern 1 and Redo restored Pattern 4. Opening Pattern 4 and adding kick step 13 left Pattern 1 with only its original steps 1 and 5.

Added `rackPlacementUniqueMenuChecks` to TimelineViewTests. It drives the actual context menu, checks busy-project refusal with unchanged document/history, verifies the new rack identity and retained Session source, proves a single Undo/Redo step, and checks that editing the copied content preserves the original. The test failed before the menu fix with “the Song rack placement menu offers Make this placement unique”, then passed after the fix.

The fresh follow-up Debug app, TimelineViewTests and SessionViewTests built successfully. The affected regression run passed 5/5 suites in 31.86 seconds.

```powershell
cmake --build "$qaSource/build" --config Debug --target AlienStudio TimelineViewTests SessionViewTests --parallel 2 -- /nr:false
ctest --test-dir "$qaSource/build" -C Debug -R '^(TimelineView|SessionView|RackProject|RackIntegration|RackLaunch)Tests$' --output-on-failure --no-tests=error
```

The local acceptance project was saved with Session Pattern 1 / Pattern 2, the independent Pattern 3 retained in the bank, and the Song placement using Pattern 4 with kick steps 1, 5 and 13. The transport was stopped.

### Listening confirmation

The user answered “Both sound clear” when asked whether the test scene's kick and clap through Realtek speakers were clear without clicks, dropouts or a stuck sound. This confirms the basic two-source drum playback listening check. Sustained synth ownership, physical device changes and the remaining scenarios below still require their own acceptance.

### Remaining acceptance

- Basic kick/clap playback has user listening confirmation. Longer dropout/stability checks and listening across other instruments and buffer sizes remain pending.
- Sustained same-pitch synth ownership, nudges, ratchets, fractional notes, choke groups and loaded sample channels still need the physical listening pass.
- Physical MIDI disconnection, audio-device changes with held notes, panic/restart, and project replacement during playback still need the hardware pass.
- Manual overlapping Song lane edits, live-versus-export listening, portable media, and format 26/27 legacy reopen checks remain pending. Existing automated coverage is listed above.
- Layout was observed at the available 1190 × 746 captured window size. Attempts to resize through the computer-use tool did not change that size. Minimum-size and 150/200% DPI acceptance remain pending.
- The D1/D2/D3 procedures in `docs/WINDOWS_AUDIO_CHECKLIST.md` remain the full manual matrix, with the Song rack Make unique action now available and retested.

ARCH-01 and QA-03 remain pending the remaining hardware, listening and display acceptance checks.
