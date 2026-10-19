# Alien Studio feature checklist

Snapshot: 2026-10-19. Baseline: main at 8fbc9f483fe7dd560fb9cb2a9a6798d7068f3165.
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
| AUDIO-01 | WAV/MP3 Session import and one-shot playback | Limited | 128 MiB decoded per clip; 512 MiB retained project budget |
| REC-01 | Mono/stereo disk recording and interrupted-take recovery | Implemented | LongRecordingTests; PR #38 merged; no multitrack arming yet |
| WARP-01 | Offline fixed-tempo pitch-preserving WAV render | Implemented | LongTempoWarpTests; PR #39 merged via #38; 0.5-2x ratio |
| WARP-02 | Rendered-clip playback workflow | User confirmed | Render, load within budget, launch in Session; not MP3 preview |
| ARR-01 | Bar-based arrangement and WAV export | Limited | ArrangementTests; 128 bars / 128 regions; no sample-level editing |
| PLUG-01 | Stereo VST3 instruments, track/master FX and editors | Partial / reported failures | PluginRuntimeTests; some scans absent; editor selection feedback gap |
| QA-01 | Production and live starter sessions | Draft in this PR | Self-contained built-in MIDI sounds; native parser test added |
| QA-02 | Four-track recording reference session | Draft in this PR | Original four-second WAVs; record/monitoring acceptance still pending |
| STREAM-01 | Fixed-size SPSC stereo playback ring | Draft in this PR | New boundary/wrap/concurrency tests; not connected to Session |
| STREAM-02 | Decoder worker, prebuffer and per-playback reader | Planned: next implementation | Independent cursors for duplicate clips; no callback file I/O |
| STREAM-03 | Large-file import, project load and render auto-load | Planned | Stream above decoded budget; preserve drum sampler limits |
| STREAM-04 | Long waveform cache, seek/relaunch and missing media UI | Planned | Background peaks; no blank or misleading waveform |
| STREAM-05 | Streamed arrangement playback/export | Planned | Deterministic offline reads; no wall-clock underruns in export |
| EDIT-01 | Non-destructive audio trim, split, slip, fades/crossfades | Planned: milestone 2 | Source offsets and edit bounds persisted with undo |
| REC-02 | Track arming, input routing, monitoring, punch and take lanes | Planned: milestone 3 | Simultaneous recording and latency compensation |
| MIDI-02 | MIDI recording, count-in, overdub and controller editing | Planned: milestone 4 | Record timing and sustain with sample-based tests |
| MIX-02 | Buses, sends/returns, sidechain, multi-output plugins | Planned: milestone 5 | Routing graph and delay compensation |
| PLUG-02 | Actionable scan logs, editor failures and compatibility suite | Planned: milestone 5 reliability | Track current user scan failures immediately |
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
