# Alien Studio feature checklist

## Detachable workspaces

- One native resizable window per editor: Steps, Piano Roll, Session, Song,
  Mixer, Rack, Drums, Audio lab and Plugins; multiple editors can remain visible.
- Toolbar detach icon and View > Detach current view / Dock current view /
  Dock all views. Closing a detached window docks its original editor.
- Stable tab hosts preserve tab indices, editor identity and shared controller /
  engine state. Main-window resizing does not reparent or resize floated content.
- Shared project shortcuts and typing-piano routing follow detached windows;
  app blur still panics notes, while switching between app windows avoids a MIDI reset.
- SongFlowTests covers native detach/dock/close/shutdown, concurrent Song/Mixer,
  shared editor state, toolbar states and detached typing/text-focus lifecycle.
- Window layout is session-only. Real multi-monitor/mixed-DPI and listening
  verification remains pending; build and regression results are in the feature PR.

## Application toolbar

- Added File, Edit, View, Transport, Options and Help above the existing transport.
- Audio/MIDI quick access, F10 menu access and Ctrl+, audio settings.
- Piano icon toggles the bottom piano panel from every workspace; starts hidden,
  lights while open, stays in sync with View > Show on-screen keyboard, and keeps
  external MIDI/playback running when hidden.
- Project file/export/undo commands, all workspaces, on-screen-keyboard visibility,
  transport toggles, recording options, plugin rack and take recovery use existing actions.
- Menu labels, tick marks and enabled states are queried live; selections recheck
  recording/busy state to reject stale commands. Stopping an active take remains available.
- Validation: SongFlowTests covers commands, state, stale selections, direct settings
  buttons, existing top-bar behavior and layouts from 960 to 1920 pixels. Windows
  build/test results and remaining manual checks are recorded in the feature PR.

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
| LIVE-03 | Shared clip storage (groundwork for raising the Session limits) | Implemented on feature branch | The song snapshot and the save/load data point at the document's immutable clip notes (SharedPianoRoll) instead of copying a 32 KiB pattern per clip: a snapshot drops from about 16 MiB to a few KiB, and empty clips with the default loop keep no notes at all. No format or behaviour change. ArrangementTests (snapshot size, SharedPianoRoll), ProjectControllerTests (sharing to and from save data, live snapshot shares notes). Next: raise tracks/scenes/notes; the per-track engine buffers (pattern copies in the scheduler and mailboxes) still grow with the note limit |
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
| EDIT-01 | Non-destructive audio trim, split, slip, fades/crossfades | Implemented on feature branch; Windows listening and manual drag checks pending | Trim/split/slip/gain/fades were already in the model; this branch adds overlapping equal-power crossfades (the outgoing clip plays on past its end on a per-track tail player, streamed clips hand their stream over at the join), fade handles and fade drawing in the Song view, Remove fades, project format 11. TimelineTests, SessionProjectTests, TimelinePlaybackTests (fade shape, crossfade curves, seek into a crossfade, block-size independence, streamed hand-off, no allocation), TimelineViewTests (handle hit/drag, menu, undo). Not done: selectable fade curves, Alt-drag slip |
| REC-02 | Track arming, input routing, monitoring, punch and take lanes | Part 1 on feature branch: arm, per-track mono/stereo input from the enabled device inputs, Auto/In/Off monitoring through the track's FX and mixer, input meters, saved per track (optional fields, no format change). Part 2 on feature branch: Record in the Song view records every armed track to disk (REC-01 writer) with count-in, punch in/out at the loop range, input+output latency compensation plus an audio offset setting, and places each take as a clip in one undo step. Windows listening, loopback latency and feedback checks pending | ToneEngineTests (routing, stereo pairs, missing inputs, mute, no allocation), SessionProjectTests (round-trip, older projects, invalid values), TimelineViewTests (arm button, menu, undo, warnings, meters), SongRecordingTests (placement, sample-exact capture, count-in, punch, loop wrap, seek, refusals, no allocation, controller undo/redo, abandon, cancel). Take list and manual range comps now implemented (format 15; TakeCompTests, SongRecordingTests, SessionProjectTests, TimelineViewTests, TimelinePlaybackTests, DeterministicStreamedExportTests). Next: waveform lanes and continuous loop-pass recording; Windows listening pending |
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
- Implemented: FL Studio-style channel row (light, pan and volume knobs, raised name button, selection lamp) and raised, glowing step buttons in the channel's colour; geometry and look covered by ChannelRackViewTests (Linux); Windows look/DPI check pending.
- Implemented: duplicate channel including sound, routing and steps in every pattern; refuses a full rack.
- Implemented: row copy/paste, fill every 2/4/8 steps, rotate left/right, reverse and repeat first 4/8/16 steps.
- Row transformations affect the active pattern length and preserve hidden steps and per-step expression.
- Commands use the existing undo path; Ctrl/Cmd+C/V copies/pastes the selected channel's steps, Ctrl/Cmd+D duplicates the channel when the grid has focus.
- Existing project format and playback scheduling are unchanged.
- Tests: RackModelTests and ChannelRackViewTests expanded; Windows Debug/Release CI and manual visual/listening checks pending.
- Implemented: open a channel in the piano roll (menu or Ctrl+P): its steps in the selected pattern become one-step notes (pitch, velocity, chance) and edits rewrite them, one undo step per gesture, while the rack pattern plays; previews play at the note's pitch. ChannelRackViewTests channelPianoRollChecks; Windows piano-roll check pending.
- Implemented: multi-channel selection (Ctrl/Shift-click on the name or lamp, Ctrl+A); mute, solo, knobs (relative), sound, output, choke, pattern tools, paste, clear, duplicate, move and delete act on every selected channel as one undo step. ChannelRackViewTests multiSelectChecks; Windows mouse/keyboard check pending.
- Implemented: resizable name column (drag its right edge, double-click to fit the longest name); the header controls, steps, graph editor and scroll bar follow it. Saved with the rack as an optional field (no format change), not an undo step. ChannelRackViewTests nameColumnChecks, RackProjectTests; Windows mouse/cursor and DPI check pending.
- Implemented (step sequencer v2, part 1; project format 12): 64 channels (was 16) and 128-step patterns (was 64), and per-step gate, nudge (up to a step late) and ratchets (1-4 hits) drawn in the graph editor. The engine queues nudged and ratcheted hits in a fixed 512-entry queue (no allocation), releases gated hits with the existing 4 ms fade, and rolls a step's chance once for all its hits; the Song timeline and WAV export use the same path. Version 11 steps open with the defaults. RackModelTests, RackEngineTests (sample-exact nudge/ratchet positions, gate release, chance, mute/stop dropping queued hits, timeline path, queue bound, block-size independence, no allocation), RackProjectTests (round-trip, older steps, damaged values, 64 channels), ChannelRackViewTests (64-channel selection, 128 steps, new graph lanes). Windows listening check pending.
- Next: Session track/scene/note limits (needs clip storage that does not grow with the fixed arrays), per-channel length and rate, trigger conditions, generators, and instrument channels (see ROADMAP).

## Song flow (feature branch)

- Implemented: **Song** is a main view in the top bar (Steps, Piano Roll, Session, Song, Mixer, Rack); More... keeps Drums, Audio lab and Plugins.
- Implemented: **Song mode** switch plays the whole song from Steps, Piano Roll, Session, Song and Rack; off by default, not saved in the project.
- Implemented: Play on an empty song says why it is silent; Record turns Song mode off; the clock shows the song's bar, beat and 16th.
- Existing project format, tab numbers and engine playback are unchanged.
- Tests: SongFlowTests (new), ChannelRackViewTests top-bar test updated; Windows Debug/Release CI and manual checks (WINDOWS_AUDIO_CHECKLIST item 36) pending.
- Implemented (PR 2): clip list beside the Song; drag a pattern or clip onto a lane (ghost + help line), double-click/Enter places at the play position.
- Implemented (PR 2): a track with no clip gets a new empty four-bar clip from a double-click, the lane menu or Place...; it uses a free Session cell (adds a scene if none), one undo step.
- Tests (PR 2): TimelineViewTests expanded (new clips, clip list, dropping); manual drag with a real mouse on Windows pending.
- Implemented (REC-02): arm tracks and record vocals onto them from the Song view.

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

## Session view layout (feature branch)

- LIVE-01 look and layout: tracks are rows and scenes are columns (FL Studio Performance-mode style). A track header holds a light (mute/solo), pan and volume knobs, the name button, a solo chip and a per-track stop button; the knobs, mute and solo are the Mixer's own controls. Clip tiles are raised and coloured with a launch triangle.
- Tests: SessionViewTests (layout, launching, per-track stop through the offline engine, header controls, look, mouse wheel). Windows Debug/Release CI and manual visual/feel checks pending.

## Channel Rack melodic notes (feature branch)

- Full immutable note sequences per channel/pattern: lengths, fractional timing, overlaps, chords, full MIDI pitch, velocity and chance; drum steps remain a separate mode.
- Miniature rack note previews open Piano Roll; step tools refuse melodic rows. Explicit channel clear returns to steps. Channel/pattern copies, move/delete, undo/redo and project format 13 retain notes.
- Sample playback schedules note starts and independent releases without allocation. Song regions bound note endings; partial-step trim/seek keeps future note-ons. Sample looping, built-in synth channels and VST3 channels are not part of this milestone.
- Windows listening, mouse/DPI checks and Debug/Release CI remain pending until recorded in the PR.

## Channel Rack built-in synth channels (feature branch)

- Add a built-in synth from + Channel, open its piano roll, and change among the 12 existing factory presets in the channel's Sound menu.
- Sustained notes, independent same-pitch overlaps, preset release envelopes, mixer pan/volume/output, mute/solo and stop/panic. Fixed 64-voice pool shared with sample voices; no audio-callback allocation.
- Timed channel-name previews and held piano-roll previews with note-off handling. Synth step rows have finite gates; sample sequencing retains its one-shot behavior.
- Undo/redo, channel copies/movement/deletion, project format 14, legacy sample defaults, Song playback and deterministic WAV export.
- VST3 rack instruments, patch editing and live MIDI/global keyboard routing into rack instruments remain pending. Hardware listening, manual DPI checks and CI results are recorded separately in the PR.
