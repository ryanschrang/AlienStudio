# Alien Studio feature checklist

Snapshot: 2026-10-20. Baseline: main at 5f73b08919afbedfeea969cc58fbdc1b56538e7d.
Owner: maintainers. Update this file in each feature PR. Link the PR, exact tested
commit, automated test result and Windows listening result before promoting a status.
"Implemented" means code exists; it does not mean all hardware or plugin combinations pass.
Draft work below is not available in main until this PR is merged.

| ID | Capability | Status | Evidence / remaining acceptance |
| --- | --- | --- | --- |
| CORE-01 | C++20 / JUCE 8 Windows application; Debug/Release CI | Implemented | CMakeLists.txt; CI is separate from listening tests |
| INST-01 | Polyphonic synth and 12 presets | Implemented | SynthEngine.h; SynthEngineTests |
| MIDI-01 | External MIDI, sustain, reconnect, typing/mouse piano | Implemented | MIDI tests; physical controllers still need checks |
| PROD-01 | 16-step pitch/velocity editing | Implemented | StepClock and sequencer tests |
| PROD-02 | Piano-roll notes, velocity/probability, processing, undo | Implemented | Piano-roll tests; not MIDI performance recording |
| PROD-03 | Eight-pad drum sample kits and sequencing | Implemented | DrumSamplerTests; representative project uses built-in synths |
| LIVE-01 | Session grid up to 16 tracks / 32 scenes | Implemented | Session project/controller tests |
| LIVE-02 | Beat/bar launch, scene launch and MIDI CC mapping | Implemented | Session/MIDI tests; hardware live checks pending |
| MIX-01 | Per-track volume, pan, mute/solo and meters | Implemented | TrackMixer tests; no general bus/send routing yet |
| FILE-01 | Versioned projects, portable audio collection | Implemented | SessionProjectTests; project v6 |
| FILE-02 | Undo/redo, save prompts, autosave and recovery | Implemented | ProjectControllerTests; recovery hardware checks pending |
| AUDIO-01 | WAV/MP3 Session import and one-shot playback | Implemented | Clips above 128 MiB stream from disk; short clips retain the decoded path |
| REC-01 | Mono/stereo disk recording and interrupted-take recovery | Implemented | LongRecordingTests; PR #38 merged; no multitrack arming yet |
| WARP-01 | Offline fixed-tempo pitch-preserving WAV render | Implemented | LongTempoWarpTests; PR #39 merged via #38; 0.5-2x ratio |
| WARP-02 | Rendered-clip playback workflow | User confirmed | Render, load within budget, launch in Session; not MP3 preview |
| ARR-01 | Arrangement timeline and WAV export | Implemented; Windows CI and listening pending | TimelineTests, TimelinePlaybackTests, TimelineViewTests, ArrangementTests; 16,384 bars / 1,024 clips; fades and sample-level edits are EDIT-01 |
| PLUG-01 | Stereo VST3 instruments, track/master FX and editors | Partial / reported failures | PluginRuntimeTests; some scans absent; editor selection feedback gap. Crash containment, safety check and editor failure reporting: see PLUG-02 |
| QA-01 | Production and live starter sessions | Implemented | Self-contained built-in MIDI sounds; native parser test added |
| QA-02 | Four-track recording reference session | Implemented | Original four-second WAVs; record/monitoring acceptance still pending |
| STREAM-01 | Fixed-size SPSC stereo playback ring | Implemented | Boundary, wrap and concurrency tests; connected to Session playback |
| STREAM-02 | Decoder worker, prebuffer and per-playback reader | Implemented | Two endpoints per track; late launch waits at quantization; duplicate clips use independent readers |
| STREAM-03 | Large-file import and project load | Implemented | Session clips stream above decoded budget; drum and Channel Rack sampler limits remain |
| STREAM-04 | Long waveform cache, seek/relaunch and missing media UI | Implemented in this PR | 65,536-peak background cache, source-stamp invalidation, zoom/scroll and explicit UI states |
| STREAM-05 | Streamed arrangement playback/export | Implemented; Windows CI and listening pending | Timeline playback streams, prefetches a quarter-second ahead and catches up if a stream is late; export renders streamed clips on its own thread (DeterministicStreamedExportTests: streamed = decoded for trimmed, split, replaced and off-grid clips, any block size, 18-hour timeline) |
| EDIT-01 | Non-destructive audio trim, split, slip, fades/crossfades | Planned: milestone 2 | Source offsets and edit bounds persisted with undo |
| REC-02 | Track arming, input routing, monitoring, punch and take lanes | Planned: milestone 3 | Simultaneous recording and latency compensation |
| MIDI-02 | MIDI recording, count-in, overdub and controller editing | Implemented | Record timing and sustain with sample-based tests |
| MIX-02 | Buses, sends/returns, sidechain, multi-output plugins | Planned: milestone 5 | Routing graph and delay compensation |
| PLUG-02 | Recursive VST3 scanning with scan logs, crash containment, editor failure reporting, compatibility check tool | Implemented in this PR; Windows CI, MSVC fault trapping and real plugins pending | FaultGuardTests, PluginFaultTests, PluginScanTests, PluginCheckTests, PluginPanelTests, PluginEndToEndTests (Linux, real VST3 test plugin). NOT RUN: Windows, Valhalla Supermassive and every commercial plugin (docs/PLUGIN_COMPATIBILITY.md). Not contained: heap corruption, plugin threads, hangs during playback |
| AUTO-01 | Mixer/plugin automation and tempo/time-signature maps | Planned: milestone 6 | Sample-accurate automation and project persistence |
| WARP-03 | Interactive warp markers and transient analysis | Planned: milestone 6 | Preserve pitch/stereo alignment |
| FINISH-01 | Freeze, stems and reliable effect tails | Planned: milestone 7 | Offline/live equivalence and portable media |

## Completion rule

A capability is verified only for the tested configuration. Record the build
commit, sample rate, buffer size, device/driver, plugin version, scenario and
result. Keep known defects visible. Passing CI alone never establishes DAW parity.

## Agreed milestone order

1. Reliable long-file playback, waveforms and media errors.
2. Non-destructive audio arrangement editing.
3. Multitrack recording, monitoring and takes.
4. MIDI recording and performance editing.
5. Mixer routing and plugin reliability.
6. Automation, tempo maps and warp editing.
7. Freeze, stems and final delivery.

Plugin scanning failures and confusing launch/editor behavior remain active
defects throughout; this ordering does not defer fixes needed to run the examples.

## Channel Rack expansion (feature branch)

- Implemented: compact rows, four-step visual grouping, selected-row accent and labelled channel controls.
- Implemented: duplicate channel including sound, routing and steps in every pattern; refuses a full rack.
- Implemented: row copy/paste, fill every 2/4/8 steps, rotate left/right, reverse and repeat first 4/8/16 steps.
- Row transformations affect the active pattern length and preserve hidden steps and per-step expression.
- Commands use the existing undo path; Ctrl/Cmd+C/V copies/pastes the selected channel's steps, Ctrl/Cmd+D duplicates the channel when the grid has focus.
- Existing project format and playback scheduling are unchanged.
- Tests: RackModelTests and ChannelRackViewTests expanded; Windows Debug/Release CI and manual visual/listening checks pending.
- Next: resizable name columns, multi-channel selection, richer graph gestures, instrument/piano-roll integration and advanced rhythm tools.

## Automation and tempo work in progress

| ID | Capability | Status | Evidence / remaining acceptance |
| --- | --- | --- | --- |
| AUTO-01 | Track volume/pan automation lanes, linear/hold points, read bypass, undo | Implemented on feature branch | AutomationTests, TimelinePlaybackTests, ProjectControllerTests; Windows listening and UI checks pending |
| AUTO-02 | Sample-position evaluation shared by playback and WAV export | Implemented on feature branch | Exact-sample and block-size-independent playback regression; Windows CI pending |
| AUTO-03 | Sends, stock-effect and plugin parameter automation | Planned | Requires routing/parameter addressing and plugin scheduling |
| TEMPO-01 | Tempo and time-signature maps | Planned | Must update transport, MIDI, click, streaming and deterministic export together |
| WARP-01 | Interactive warp markers and transient detection | Planned | Requires source-to-musical-time mapping and background analysis |

Automation editor: Arrangement > Automation. Choose a track and Volume/Pan. Click to add,
drag a point, right-click a point to delete. Linear and Hold control interpolation;
Read bypass restores the manual fader. Points use absolute gain (0..2) and pan (-1..1).
The editor shows the entire song. Format 10 saves automation; older projects open without lanes.
