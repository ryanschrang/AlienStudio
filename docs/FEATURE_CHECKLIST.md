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
- T1-11 saved layouts: main/detached normal geometry and maximized state, active
  workspace, device view and keyboard visibility persist locally across restart.
  View > Save window layout saves immediately; View > Reset window layout docks
  all editors and restores defaults. Docked editors remember their floating bounds.
  Strict versioned preferences, safe fallback and monitor-aware logical-coordinate
  fitting are covered by SongFlowTests. Real multi-monitor/mixed-DPI and listening
  verification remains pending; build/regression results are in the feature PR.

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

Snapshot: 2026-10-26. Baseline: main at 1587c17 (after #89 stems, #93 freeze, #94 recording queue tests,
#95 send automation and #98 plugin automation targets).
Priority: work Tier 1 first, then the partly done Tier 2 items. The tiers, workstream IDs (T1-xx, T2-xx),
dependencies and the rules for several people or agents working at once are in ROADMAP.md
("Priority plan: Alien Studio Core 1.0"); the table "Roadmap tier rows" at the end lists the IDs it added.
Owner: maintainers. Update this file in each feature PR. Link the PR, exact tested
commit, automated test result and Windows listening result before promoting a status.
"Implemented" means code exists; it does not mean all hardware or plugin combinations pass.
Every row and section below describes main unless it says otherwise. "Planned" rows are not
started; the gap list from the FL Studio / Ableton Live / Logic Pro comparison is at the end.
The current project format is 28 (18: song tempo changes, #104; 19: group buses, #106; 20: song time signatures, #114; 21: effect sidechains; 22: linked Session clips, #124; 23: return slots, #130; 24: Song placement clips, #131; 25: sidechain keys from faders, buses and returns, #137; 26: saved rack bindings, #141; 27: fade shapes; 28: per-row rack launch semantics and imported-scene compatibility).

| ID | Capability | Status | Evidence / remaining acceptance |
| --- | --- | --- | --- |
| CORE-01 | C++20 / JUCE 8 Windows application; Debug/Release CI | Implemented | CMakeLists.txt; CI is separate from listening tests |
| INST-01 | Polyphonic synth and 12 presets | Implemented | SynthEngine.h; SynthEngineTests |
| MIDI-01 | External MIDI, sustain, reconnect, typing/mouse piano | Implemented | MIDI tests; physical controllers still need checks |
| PROD-01 | Step sequencer: 16/32/64/128 steps, 64 channels, per-step pitch, velocity, gate, nudge and ratchets | Implemented (#73, format 12) | StepClock, RackModelTests, RackEngineTests, RackProjectTests; Windows listening pending |
| PROD-02 | Piano-roll notes, velocity/probability, processing, chord progression builder, undo | Implemented (chords: #79) | Piano-roll tests, PianoRollChordStampTests; MIDI performance recording is MIDI-02 |
| PROD-03 | Eight-pad drum sample kits and sequencing | Implemented | DrumSamplerTests; representative project uses built-in synths |
| LIVE-01 | Session grid up to 16 tracks / 32 scenes | Implemented | Session project/controller tests, SessionViewTests; raising the limits is LIVE-04 |
| LIVE-03 | Shared clip storage (groundwork for raising the Session limits) | Implemented (#77) | The song snapshot and the save/load data point at the document's immutable clip notes (SharedPianoRoll) instead of copying a 32 KiB pattern per clip: a snapshot drops from about 16 MiB to a few KiB, and empty clips with the default loop keep no notes at all. No format or behaviour change. ArrangementTests (snapshot size, SharedPianoRoll), ProjectControllerTests (sharing to and from save data, live snapshot shares notes). Next: raise tracks/scenes/notes; the per-track engine buffers (pattern copies in the scheduler and mailboxes) still grow with the note limit |
| LIVE-02 | Beat/bar launch, scene launch and MIDI CC mapping | Implemented | Session/MIDI tests; hardware live checks pending |
| MIX-01 | Per-track volume, pan, mute/solo and meters | Implemented | TrackMixer tests; sends and returns are MIX-02 |
| FILE-01 | Versioned projects, portable audio collection | Implemented | SessionProjectTests; project format 17 (older formats still open) |
| FILE-02 | Undo/redo, save prompts, autosave and recovery | Implemented | ProjectControllerTests; recovery hardware checks pending |
| AUDIO-01 | WAV/MP3 Session import and one-shot playback | Implemented | Clips above 128 MiB stream from disk; short clips retain the decoded path |
| REC-01 | Mono/stereo disk recording and interrupted-take recovery | Implemented | LongRecordingTests; PR #38 merged; multitrack arming is REC-02 |
| WARP-01 | Offline fixed-tempo pitch-preserving WAV render | Implemented | LongTempoWarpTests; PR #39 merged via #38; 0.5-2x ratio |
| WARP-02 | Rendered-clip playback workflow | User confirmed | Render, load within budget, launch in Session; not MP3 preview |
| ARR-01 | Arrangement timeline and WAV export | Implemented; Windows CI and listening pending | TimelineTests, TimelinePlaybackTests, TimelineViewTests, ArrangementTests; 16,384 bars / 1,024 clips; fades and sample-level edits are EDIT-01 |
| PLUG-01 | Stereo VST3 instruments, track/master FX and editors | Partial / reported failures | PluginRuntimeTests; some scans absent; editor selection feedback gap. Crash containment, safety check and editor failure reporting: see PLUG-02 |
| QA-01 | Production and live starter sessions | Implemented | Self-contained built-in MIDI sounds; native parser test added |
| QA-02 | Four-track recording reference session | Implemented | Original four-second WAVs; record/monitoring acceptance still pending |
| STREAM-01 | Fixed-size SPSC stereo playback ring | Implemented | Boundary, wrap and concurrency tests; connected to Session playback |
| STREAM-02 | Decoder worker, prebuffer and per-playback reader | Implemented | Two endpoints per track; late launch waits at quantization; duplicate clips use independent readers |
| STREAM-03 | Large-file import and project load | Implemented | Session clips stream above decoded budget; drum and Channel Rack sampler limits remain |
| STREAM-04 | Long waveform cache, seek/relaunch and missing media UI | Implemented | 65,536-peak background cache, source-stamp invalidation, zoom/scroll and explicit UI states |
| STREAM-05 | Streamed arrangement playback/export | Implemented; Windows CI and listening pending | Timeline playback streams, prefetches a quarter-second ahead and catches up if a stream is late; export renders streamed clips on its own thread (DeterministicStreamedExportTests: streamed = decoded for trimmed, split, replaced and off-grid clips, any block size, 18-hour timeline) |
| EDIT-01 | Non-destructive audio trim, split, slip, fades/crossfades | Implemented; Windows listening and manual drag checks pending | Trim/split/slip/gain/fades, plus overlapping equal-power crossfades (the outgoing clip plays on past its end on a per-track tail player, streamed clips hand their stream over at the join), fade handles and fade drawing in the Song view, Remove fades, project format 11. Alt-drag an audio body to slip selected audio sources freely with fixed boundaries, preview and one undo step (T1-08, #112; no format change). TimelineTests, SessionProjectTests, TimelinePlaybackTests (fade shape, crossfade curves, seek into a crossfade, block-size independence, streamed hand-off, no allocation), TimelineViewTests (handle hit/drag, menu, slip preview/commit/cancel, source-start clamp, mixed selection, tempo-aware source seconds, busy refusal, undo/redo). Build/test results in the feature PR. Fade shapes (T1-08): each fade in, fade out and crossfade can be Linear, Equal power, Slow (exponential), Fast (logarithmic) or S-curve from the clip's menu (Fade-in / Fade-out / Crossfade shape), applied to every selected audio clip as one undo step; a fade out and a crossfade's outgoing side are the shape mirrored; playback, export, stems and freeze play the same gain, and the Song view draws each fade in its shape. Defaults keep straight fades and an equal-power crossfade, so older projects sound exactly as before; non-default shapes are saved from format 27 (TimelineTests, TimelinePlaybackTests, TimelineViewTests) |
| REC-02 | Track arming, input routing, monitoring, punch and take lanes | Implemented. Waveform take lanes (T1-07): a track with takes shows a take-count button in its header (and "Show take lanes" in its menu); it opens one lane per take or loop pass under the row, in recording order, each drawing its waveform at its song position with the part the arrangement plays highlighted. Click a lane to pick its take; drag across it to comp that range through the existing take comp (one undo step, edge fades, take kept); its menu has Play this take, Play this take in the loop range, Forget take and Hide take lanes. Which tracks show lanes is a session-only view setting (no format change). Loop recording (T1-07): recording goes on through each loop wrap (up to 64 passes); every pass becomes a take of the one recorded file, the last full pass plays on the song and the others wait in Takes for comping, in one undo step. Arm, per-track mono/stereo input from the enabled device inputs, Auto/In/Off monitoring through the track's FX and mixer, input meters, saved per track (optional fields, no format change). Record in the Song view records every armed track to disk (REC-01 writer) with count-in, punch in/out at the loop range, input+output latency compensation plus an audio offset setting, and places each take as a clip in one undo step. Windows listening, loopback latency and feedback checks pending | ToneEngineTests (routing, stereo pairs, missing inputs, mute, no allocation), SessionProjectTests (round-trip, older projects, invalid values), TimelineViewTests (arm button, menu, undo, warnings, meters), SongRecordingTests (placement, sample-exact capture, count-in, punch, loop wrap, seek, refusals, no allocation, controller undo/redo, abandon, cancel). Take list and manual range comps (#88, format 15; TakeCompTests, SongRecordingTests, SessionProjectTests, TimelineViewTests, TimelinePlaybackTests, DeterministicStreamedExportTests). Loop passes: SongRecordingTests (capture through wraps, pass limit, placement with latency carried into the next pass, punch, controller end to end), TimelineTests. Take lanes: TimelineViewTests (lane geometry and hit-testing, header button and menu toggle not in undo, drag-to-comp undo/offset/clamping, menu actions, loop passes, highlight pixels, hiding with no takes, track deletion), TakeCompTests (which parts of a take play). Windows listening and real-mouse lane checks pending |
| MIDI-02 | MIDI recording, count-in, overdub and controller editing | Implemented | Record timing and sustain with sample-based tests |
| MIX-02 | Buses, sends/returns, sidechain, multi-output plugins | Part 1 implemented (#84): four return tracks (A-D) with level, pan, mute, meter and an eight-slot FX rack each (stock or VST3, chosen in Plugins); per-track send knobs with a pre/post-fader switch; mute and solo gate every send; track solo never silences a return; delay compensation through the returns; project format 15 (older projects: sends off, returns at 0 dB). Windows listening and real-plugin latency checks pending | MixerRoutingTests (exact send/return levels pre and post fader, mute, solo, return level/pan/mute/FX, send glide, dry/return alignment with a latent return FX, reported latency, no allocation, format round-trip/older files/damage, undo, mixer and plugin panels). Remaining: MIX-03 to MIX-05 and send automation (AUTO-03) |
| PLUG-02 | Recursive VST3 scanning with scan logs, crash containment, editor failure reporting, compatibility check tool | Implemented; MSVC fault trapping and real commercial plugins pending | FaultGuardTests, PluginFaultTests, PluginScanTests, PluginCheckTests, PluginPanelTests, PluginEndToEndTests (Linux, real VST3 test plugin). NOT RUN: Windows, Valhalla Supermassive and every commercial plugin (docs/PLUGIN_COMPATIBILITY.md). Not contained: heap corruption, plugin threads, hangs during playback. T1-09 part 1: a plugin that crashes while hosted (loading, preparing, playing, saving state, queries, editor) is blocked from loading again until its file changes, and is listed under Scan VST3 > Blocked plugins to allow it again (invalid audio switches a plugin off but does not block it); a background scan for new and updated plugins runs at startup (Scan VST3 > Look for new plugins when Alien Studio starts). PluginBlocklistTests, PluginFaultTests. A crash that takes the whole application down is not recorded, so it is not blocked |
| MIDI-03 | Live MIDI destination: play a Session track's VST3 or built-in instrument from MIDI, typing or on-screen keys | Implemented (#83) | Held notes released when the destination changes; not saved with the project. Rack channels are not destinations yet |
| INST-02 | Built-in synth channels in the Channel Rack (12 presets, 64 shared voices) | Implemented (#82, format 14) | See "Channel Rack built-in synth channels" below |
| FINISH-01 | Freeze, stems and reliable effect tails | Implemented: stem export (#89), track freeze/unfreeze (#93, integrated in #98) | Aligned stems, pre-fader float caches, preserved original clips and processing. Hardware/listening remains pending. |

## Piano-roll keyboard and pitch visibility upgrade (PROD-06)

Status: Implemented in code: part 1 (labels, key highlights, auditioning; PianoRollKeyboardTests) and part 2 (All /
Used / Scale rows and shared scale shading; PianoRollPitchViewTests). Windows visual and listening checks are pending,
so the upgrade is not yet complete. Added to the completion checklist on 2026-10-27.
Build on PROD-02 / PROD-05 and coordinate scale behavior with MIDI-06.
Existing auditioning and scale guides are foundations, not evidence that this
complete upgrade has passed acceptance.

- [x] Note-name labels with octave numbers (C3, C#3, D3, etc.), with All labels
  and C notes only display options. (Edit > Keyboard labels; C4 is MIDI 60, as the piano roll already
  named notes; all names show when rows are tall enough to read them.)
- [x] Pitch visibility toolbar modes: All notes, Used notes and Scale notes;
  keyboard keys and grid rows stay aligned in every mode. (The rows button above the keyboard, or Edit > Show rows.
  Keys and rows are drawn from one row list; a line marks where pitches are folded away, and every key is named
  while rows are hidden. Used rows also show ghost notes' pitches; an empty clip shows every row.)
- [x] Scale notes mode keeps existing out-of-scale notes visible; changing
  visibility never changes or deletes MIDI data. (A note's pitch always has a row in Used and Scale rows; an
  out-of-scale row is tinted red on its key and in the grid.)
- [x] Selected MIDI notes highlight the corresponding piano keys in matching
  colors; sounding notes also light up their keys. Preserve multi-clip colors.
  (A note under the playhead lights its key brighter; another clip's notes keep that clip's colour.)
- [x] Click-and-hold keyboard auditioning uses the active track/channel's
  instrument; release, focus loss and destination changes stop preview notes.
  (Previews play on the edited clip's track instrument, or the rack channel, never on the live synth and never
  into a take; a note-off always reaches the instrument its note-on went to, PianoRollAuditionRouter.)
- [x] Root-note emphasis and scale-note shading use the selected key/scale
  consistently with MIDI-06. (The Scale tool's root and scale drive the grid rows, the keys, Scale rows and snapping
  alike: a wide mark on root keys, a narrow one on the scale's other keys. Shown when Highlight scale is on or Scale
  rows are shown. MIDI-06 itself, project-wide key awareness, is still planned; the scale stays per editor.)
- [x] Drawing, selecting, moving, resizing and auditioning target the correct
  MIDI pitches with hidden rows, zoom and scrolling; undo/redo preserves edits. (Dragging and the arrow keys move
  a row at a time across hidden pitches; a drag keeps its starting rows until it ends. Scrolling is covered; the
  piano roll has no vertical zoom yet, and resizing does not depend on rows.)
- [ ] Verify empty clips, chords, out-of-scale notes and multi-clip editing;
  record automated results plus Windows visual/listening checks under the
  existing completion rule before marking the upgrade complete.

Suggested delivery: labels, highlights and auditioning first; pitch filtering
and integrated scale guidance next.

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

## Channel Rack expansion

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
- Next: Session track/scene/note limits (LIVE-04; clip storage is done), per-channel length and rate and trigger conditions (PROD-04), and generators (MIDI-05). Instrument channels: built-in synth done (#82), VST3 pending (INST-10).

## Song flow

- Implemented: **Song** is a main view in the top bar (Steps, Piano Roll, Session, Song, Mixer, Rack); More... keeps Drums, Audio lab and Plugins.
- Implemented: **Song mode** switch plays the whole song from Steps, Piano Roll, Session, Song and Rack; off by default, not saved in the project.
- Implemented: Play on an empty song says why it is silent; Record turns Song mode off; the clock shows the song's bar, beat and 16th.
- Existing project format, tab numbers and engine playback are unchanged.
- Tests: SongFlowTests (new), ChannelRackViewTests top-bar test updated; Windows Debug/Release CI and manual checks (WINDOWS_AUDIO_CHECKLIST item 36) pending.
- Implemented (PR 2): clip list beside the Song; drag a pattern or clip onto a lane (ghost + help line), double-click/Enter places at the play position.
- Implemented (PR 2): a track with no clip gets a new empty four-bar clip from a double-click, the lane menu or Place...; it uses a free Session cell (adds a scene if none), one undo step.
- Tests (PR 2): TimelineViewTests expanded (new clips, clip list, dropping); manual drag with a real mouse on Windows pending.
- Implemented (REC-02): arm tracks and record vocals onto them from the Song view.

## Automation and tempo

| ID | Capability | Status | Evidence / remaining acceptance |
| --- | --- | --- | --- |
| AUTO-01 | Track volume/pan automation lanes, linear/hold points, read bypass, undo | Implemented | AutomationTests, TimelinePlaybackTests, ProjectControllerTests; Windows listening and UI checks pending |
| AUTO-02 | Sample-position evaluation shared by playback and WAV export | Implemented | Exact-sample and block-size-independent playback regression |
| AUTO-03 | Sends, stock-effect and plugin parameter automation | Partial: Send A–D automation (#95); plugin parameter targets saved and kept across reorder/undo/freeze (#98); plugin parameter editor and playback scheduling remain | Format 17 saves instance IDs and normalized lanes; reorder/undo/freeze migration retain targets; missing or replaced plugins remain unresolved. ParameterAutomationTests, SessionProjectTests, ProjectControllerTests, MixerRoutingTests, StockEffectsTests. Plugin parameter editor and playback scheduling remain unfinished. |
| TEMPO-01 | Tempo and time-signature maps | Implemented on the Song: tempo map (#104, format 18; ruler editor in #107) and time signatures (#114, format 20). Windows listening pending | Tempo changes on the Song ruler (add, edit, drag, delete, undo); the project tempo plays until the first change. The Song playhead, MIDI and rack timing, ordinary audio regions (unstretched; opt-in warp markers use background tempo-aware caches, WARP-03), crossfades, stream prefetch, the click and count-in, plugin playheads, recorded take placement, clip lengths, freeze and WAV/stem export follow the map; editing it unfreezes frozen tracks. TimelineTests, TimelinePlaybackTests (sample positions, mid-sixteenth changes, unstretched audio, loops, block-size independence, click, no allocation), SessionProjectTests, SongRecordingTests, ProjectControllerTests, MixerRoutingTests, DeterministicStreamedExportTests, TimelineViewTests. Time signatures (#114): changes start at a bar (4/4 before the first); bars, the song length, the ruler, grid, snapping, bar/beat readouts, the transport clock, the click (accent on each bar, the meter's beats such as eighths in 6/8), the count-in and the plug-in playhead (meter and bar start) follow them, while clips, notes and tempo changes keep their ticks; shorter bars add bars so nothing falls outside the song. TimelineTests, SessionProjectTests, TimelinePlaybackTests, SongRecordingTests, TimelineViewTests. Session and Steps keep the project tempo and 4/4. Windows listening pending |
| WARP-03 | Interactive warp markers and transient detection | Implemented in code; Windows listening pending | Format 30 stores ordered source-seconds/musical-tick anchors per Song region. Enable fit-to-region, add/move/remove interior markers, disable/rebuild, and worker-side stereo onset detection. Pitch-preserving bounded WSOLA renders float caches against the full tempo map; playback pauses during preparation, Escape cancels, failure silences the affected warp. Trims/splits/slip preserve source identity; playback/export share the renderer. TimelineTests, SessionProjectTests, TimelineViewTests, TimelinePlaybackTests and DeterministicStreamedExportTests cover model/format/UI/history/tempo/pitch/stereo/seek/callback/export behavior. Physical listening/long-file stress checks remain; see WARP_MARKERS.md. |

Automation editor: Arrangement > Automation. Choose a track and Volume/Pan. Click to add,
drag a point, right-click a point to delete. Linear and Hold control interpolation;
Read bypass restores the manual fader. Points use absolute gain (0..2) and pan (-1..1).
The editor shows the entire song. Format 10 saves automation; older projects open without lanes.

## Session view layout

- LIVE-01 look and layout: tracks are rows and scenes are columns (FL Studio Performance-mode style). A track header holds a light (mute/solo), pan and volume knobs, the name button, a solo chip and a per-track stop button; the knobs, mute and solo are the Mixer's own controls. Clip tiles are raised and coloured with a launch triangle.
- Tests: SessionViewTests (layout, launching, per-track stop through the offline engine, header controls, look, mouse wheel). Windows Debug/Release CI and manual visual/feel checks pending.

## Channel Rack melodic notes

- Full immutable note sequences per channel/pattern: lengths, fractional timing, overlaps, chords, full MIDI pitch, velocity and chance; drum steps remain a separate mode.
- Miniature rack note previews open Piano Roll; step tools refuse melodic rows. Explicit channel clear returns to steps. Channel/pattern copies, move/delete, undo/redo and project format 13 retain notes.
- Sample playback schedules note starts and independent releases without allocation. Song regions bound note endings; partial-step trim/seek keeps future note-ons. Sample looping, built-in synth channels and VST3 channels are not part of this milestone.
- Windows listening, mouse/DPI checks and Debug/Release CI remain pending until recorded in the PR.

## Channel Rack built-in synth channels

- Add a built-in synth from + Channel, open its piano roll, and change among the 12 existing factory presets in the channel's Sound menu.
- Sustained notes, independent same-pitch overlaps, preset release envelopes, mixer pan/volume/output, mute/solo and stop/panic. Fixed 64-voice pool shared with sample voices; no audio-callback allocation.
- Timed channel-name previews and held piano-roll previews with note-off handling. Synth step rows have finite gates; sample sequencing retains its one-shot behavior.
- Undo/redo, channel copies/movement/deletion, project format 14, legacy sample defaults, Song playback and deterministic WAV export.
- VST3 rack instruments, patch editing and live MIDI/global keyboard routing into rack instruments remain pending. Hardware listening, manual DPI checks and CI results are recorded separately in the PR.

## Missing features (FL Studio / Ableton Live / Logic Pro comparison, 2026-10-25)

Gaps found by comparing Alien Studio's main with the current feature pages of FL Studio,
Ableton Live 12 Suite and Logic Pro. Every row is Planned; order within a group is not a
priority. Reference products are named only to describe the capability; designs, names and
artwork stay original.

| ID | Capability (for comparison) | Status | Notes / acceptance |
| --- | --- | --- | --- |
| FX-01 | Stock filter, saturator, chorus/phaser, gate and transient shaper | In progress: filter (#91), saturator (#92) and gate (MIX-04 part 2) implemented; chorus/phaser and transient shaper planned | One small PR each; DSP tests, no callback allocation, automatable parameters (AUTO-03). Filter and saturator: StockDspTests, StockEffectsTests; Windows listening pending |
| FX-02 | More stock effects: flanger, multiband compressor, de-esser, stereo imager, bit crusher, convolution reverb | Planned | FL, Live and Logic all ship large native effect sets |
| INST-03 | Wavetable synth (Live Wavetable; FL Sytrus/Harmor-style sound design) | Planned | Band-limited tables; preset scaffolding shared with INST-01 |
| INST-04 | FM / additive synth (Live Operator, FL Sytrus, Logic Retro Synth) | Planned | Voice allocation and stealing tests as for INST-01 |
| INST-05 | Physical-modelling instruments (Live Collision/Tension) | Planned | |
| INST-06 | Drum synth (FL Drumaxx, Logic Drum Synth) | Planned | Kick/snare/hat voices usable from the Channel Rack |
| INST-07 | Sampler with chopping and slicing (Logic Quick Sampler, Live Simpler slicing, FL Slicex) | Planned | Transient slicing shares analysis with WARP-03 |
| INST-08 | Preset browser for instruments (FL FLEX-style browsing) | Partial | T1-10 factory-synth slice: searchable Devices list, name/type tokens, stable filtered preset IDs, explicit Use preset for live/step synth and isolated bounded C4 audition. SongFlowTests covers search, empty/apply/busy state, menu access and search typing focus; ToneEngineTests covers audible factory previews, timed release, independent live notes, monitor-only routing, master gain and allocation safety. Plugin/sample browsing remains. |
| INST-09 | MPE instrument (Live Meld) | Planned | Needs MIDI-08 |
| INST-10 | VST3 instruments as Channel Rack channels | Planned | Live MIDI and keyboard routing into rack instruments included |
| MIX-03 | Group buses and output routing (track to bus to master) | Implemented (T1-05): up to eight group buses, each with volume, pan, mute, solo, meter and an eight-slot FX rack; each track's Output goes to the master or a bus; buses feed only the master (no loops); soloing a track keeps its bus audible and soloing a bus keeps its tracks audible; delay compensation across buses (bus stage before the returns); stems add one file per bus and mark tracks routed into one; freeze renders the track without its bus; mixer Add bus / rename / delete and Output menus; project format 19 (older projects: every track to the master). Windows listening and real-plugin latency checks pending | GroupBusTests (exact bus levels, pan, mute, solo across tracks and buses, missing-bus fallback, delay alignment with latent bus and return FX, no allocation, format round-trip/older files/damage, controller undo/redo, mixer and plugin panels, stem capture alignment and files, freeze) |
| MIX-04 | Sidechain inputs for compressors, gates and VST3 plugins | Part 1 implemented: the stock compressor can be keyed by any other Session track (Plugins > Sidechain...), the key being that track's signal before its own FX; the choice is saved with the effect (format 21), follows track moves and is removed when its track is deleted. The host switches on an effect's second input bus in stereo, so a VST3 effect with a sidechain bus can be keyed the same way, but that is untested with real plugins. Part 2 implemented: the stock Alien Gate (threshold, range, attack, hold, release, with hysteresis; keyable like the compressor), and keys from a track after its fader and pan, from a group bus's output and from a return's output. A key must be made before the effect runs in the mix (tracks' FX, faders, buses' FX, returns' FX, master FX), so a track's FX take track keys before their FX only, a bus's FX also post-fader keys, a return's FX also bus keys, and the master's FX every kind; the Sidechain menu offers exactly those, the project refuses others, and removing a bus or return removes keys from it (format 25) | SidechainTests (detector; the gate: threshold, hold, close, range, keyed opening and closing, broken key, hysteresis; the sidechain buses; runtime ducking; post-fader, bus and return keys through the real mix; refusal of a key a rack cannot hear; no allocation; saving, kinds of key, rack rules, format 24 refusal; track moves and deletes; bus and return removal; Plugins panel menus per rack). StockEffectsTests. Windows listening and real VST3 sidechain checks pending |
| MIX-05 | Add, remove and rename returns; more than four | Implemented (T1-05): up to eight returns (A-H), added from the mixer (Add return) and renamed or removed from their name button, each one undo step; a return keeps its slot letter, so removing one never moves another's sends, send automation or FX; removing one turns its sends off and drops its send automation and FX; a new project and every older project have returns A-D exactly as before; track strips show one send knob per return (rows of four), and Plugins, the send tooltips and the automation editor name them; a slot not in use costs no audio work and one that comes back starts with clear delay lines; stems write one file per return; project format 23 (refuses a send, pre-fader switch or automation lane pointing at a missing return). Capacity from measured cost (Release, this PC, 16 tracks sending to every return, 512-sample callback): about 460 us with no returns, 580 us with four, 610 us with eight, against a 10.7 ms block at 48 kHz; each slot also holds 1.5 MB of delay lines (about 12 MB for eight). Windows listening and real-plugin latency checks pending | FlexibleReturnTests (exact levels, pan, mute, pre-fader and solo beyond return D, delay alignment with a latent return G, a reused slot starts clean, no allocation while removing and restoring returns, controller add/rename/remove with undo and redo, sends and automation staying on their returns after a removal, format round trip, version 21 and 12 projects, damaged values, mixer, Plugins and automation panels, stem capture and files, freeze; prints the cost above) |
| LIVE-04 | Raise Session track, scene and clip-note limits (toward FL's 500 mixer tracks) | Planned | Groundwork done in LIVE-03; per-track engine buffers still grow with the limits |
| PROD-04 | Per-channel pattern length and rate, step trigger conditions | Planned | From the existing ROADMAP |
| MIDI-04 | Capture MIDI: recover what was just played without recording (Live Capture MIDI) | Planned | Fixed-size history buffer filled off the audio thread |
| MIDI-05 | MIDI transformations and generators (Live 12) | Planned | Undoable, deterministic with a seed |
| MIDI-06 | Key and scale awareness across Piano Roll, Channel Rack and generators | Planned | Scale highlight, fold and snap |
| MIDI-07 | Note repeat (Logic Step Sequencer) | Planned | Tempo-synced from live input |
| MIDI-08 | MPE recording and editing | Planned | Per-note pitch bend, pressure and slide |
| MIDI-09 | Alternative tuning systems | Planned | Tuning files applied to built-in instruments |
| WARP-04 | Real-time clip warping that follows the song tempo (Live Warp, Logic Flex Time) | Partial | Song warp markers follow tempo changes through background-prepared pitch-preserving caches (WARP-03). Continuous live stretching while the tempo is being changed remains future work. |
| TEMPO-02 | Tempo detection for imported audio (Logic Smart Tempo) | Planned | Background analysis; never on the audio thread |
| PITCH-01 | Pitch correction and note-level pitch editing (Logic Flex Pitch, FL Newtone) | Planned | |
| CONV-01 | Audio to MIDI for drums, melody and harmony (Live) | Planned | |
| CONV-02 | Stem separation into vocals, drums, bass and other (Live 12 Suite, Logic Stem Splitter) | Planned | Check model and dependency licenses before release |
| CONV-03 | Chord detection from audio or MIDI (Logic Chord ID) | Planned | Feeds the chord progression builder |
| FINISH-02 | Mastering assistant (Logic Mastering Assistant) | Planned | Built from stock EQ, compressor and limiter |
| AI-01 | Assistant that answers questions and can act on the project (FL Gopher; Logic Session Players) | Planned | Every action undoable; opt-in network use |
| BROWSE-01 | Sound browser with search and tempo/key-synced auditioning (Live's Splice integration) | Partial | First slice: searchable built-in synth presets with isolated audition (INST-08). Local sample library, plugin browsing and tempo/key-synced sample previews remain. |
| SYNC-01 | Ableton Link tempo sync | Planned | Check the Link license before release |
| EXT-01 | Scripting or device SDK for custom MIDI tools and devices (Max for Live, FL scripting) | Planned | Sandboxed, outside the audio callback |
| CLOUD-01 | Optional project backup and version history (FL Cloud) | Planned | Opt-in; never required to save |

Out of scope for now: macOS and iPad versions (Alien Studio is a Windows 11 DAW), and paid
editions or update tiers (Alien Studio is free and open source).

## Roadmap tier rows (added 2026-10-26)

New IDs used by the tiered priority plan in ROADMAP.md. Every row is Planned unless it says
otherwise. The Tier column is the roadmap tier; existing rows keep their place above.

| ID | Tier | Capability | Status | Notes / acceptance |
| --- | --- | --- | --- | --- |
| CMD-01 | 1 (T1-02) | Command Engine: named, undoable commands (create/delete/move/rename/colour tracks, clips, parameters, buses, sends, devices, groups, tempo, chords, sections) with Execute, Undo, Redo, Batch and Preview | Partial | Structural track commands (#101), plus SetTrackVolume (linear gain 0-2) and SetTrackPan (-1 to 1), strict parsing/validation, side-effect-free Preview, atomic refusal and one-step batch undo/redo (#123). CommandEngineTests covers predicted/live values, endpoints/non-finite inputs, no-ops, redo preservation and sequential create/reorder batches. Session add-track button and track add/delete/move/rename menus use commands (#126), with one command-owned undo step and busy refusal tested in SessionViewTests. SetTrackMute and SetTrackSolo add strict boolean arguments, additive solo, preview and one-step batch undo/redo; CommandEngineTests covers live states, refusal, no-ops and redo preservation (#128). Session mute light and solo controls use these commands, with refresh-only mixer notifications and no legacy commit; SessionViewTests covers click/undo/redo, dirty state, additive solo and busy refusal (claim #132). Session volume/pan drag, Shift fine adjustment, wheel and reset now use the same commands with refresh-only host notifications; unique continuous keys keep a paused drag in one undo step and timed keys group wheel edits. CommandEngineTests and SessionViewTests cover grouping, no-op/redo behavior, busy transitions and stale rows (claim #151). Mixer track volume/pan/mute/solo now use the same commands with refresh-only notifications; each slider drag owns a unique continuous history key, and stale strip events refuse after track replacement/reorder. MixerRoutingTests covers long/separate/no-op drags, busy refusal, additive solo, undo/redo and Session-to-Mixer synchronization (claim #157). Other UI adoption, clips and remaining parameter/routing/device/tempo/chord/section commands remain. Build/test results in the feature PR; hardware listening pending. See docs/COMMAND_ENGINE.md; built on FILE-02 |
| ARCH-01 | 1 (T1-03) | One clip/pattern object shared by Create (Steps, Piano Roll), Perform (Session) and Arrange (Song); editing any view updates all | Partial | Design: docs/SHARED_CLIP_MODEL.md (#102). Increment A: `src/ClipLibrary.h`, typed ids saved as 16 hex digits, immutable pattern revisions with note, Channel Rack step (lossless) and audio lanes, clip definitions, duplicate / linked duplicate / make unique, stale-revision refusal, validation and pruning; ClipLibraryTests. Increment B1 (#116): the library owns every Session cell's content (all edits are new revisions of the cell's clip; duplicate scene makes independent clips). Increment B2a: Session cells can share one clip's content (Copy / link > Paste linked here; Make unique; an edit of one linked cell is an edit of all), Song > Make this placement unique gives one placement its own copy in a free cell, and links are saved from format 22. ProjectControllerTests, SessionViewTests. Increment B2b: a Song region can play a placement clip, a MIDI clip of the song's own that no Session cell launches (`Region::clip`). Make this placement unique gives a MIDI placement one without using a cell or adding a scene; copies of it share it, the piano roll edits it (every region that plays it follows), scene changes leave it alone, it is dropped with its last region and a paste brings a cut one back, and it is saved from format 24. Audio placements still get a free cell. ProjectControllerTests, TimelineViewTests. Increment C: every Channel Rack pattern is a library clip whose step lanes are its channels' rows, keyed by stable lane ids (steps with all their values, dormant and hidden steps, and note rows kept exactly); the rack's patterns are that content as the views and engine read it, changed only through `ProjectDocument::setRack`, which makes each changed pattern a new revision and refuses anything it cannot hold exactly; copies are independent clips, moving a channel changes no content, saves are written from the library; no format change. RackLibraryTests, ProjectControllerTests. Increment D1: Session > Rack clips explicitly launches/stops a shared rack source on Beat/Bar without changing editor selection, or opens that source for editing; one composite rack clip plays alongside Session tracks. Transient launch state is not saved. RackLaunchTests, RackIntegrationTests, SessionViewTests. Increment D2: format 26 saves rack clip/lane identifiers, Session rack assignments and explicit Song rack-source references. Linked assignments edit one source; Paste a copy, scene duplication and Song Make unique fork independent bank sources. Used sources are protected from deletion. New readers reject missing/mismatched/mixed-kind references atomically and import older bank slots independently. ProjectControllerTests, RackProjectTests, SessionViewTests, TimelineViewTests. Increment D3 (#144): independent Session-row launches and four Song lane occurrences; same-source/same-pitch voices, delayed hits, probability state and stop/replacement ownership are instance-local within the existing shared 64-voice/pending-hit bounds. New scenes can launch distinct sources; format 28 retains older scenes' duplicate-source launch policy. RackLaunchTests, RackIntegrationTests, RackProjectTests and SessionViewTests cover overlap, scoped stop/cancellation, lifecycle, row indicators, migration and live/export parity. Exact build/test evidence belongs in the feature PR; Windows listening and visual acceptance remain pending. Increment E: document rack ownership uses clip IDs only; stale timeline index caches regenerate from identity and unknown IDs fail atomically. Song bank lookup moves from each sample to snapshot publication. ProjectControllerTests covers cache/ownership refusal and undo; RackIntegrationTests retains cross-publication playback checks and adds an opt-in allocation-checked callback report. TrackCapacityReport includes fixed rack pools; docs/SHARED_PLAYBACK_CAPACITY.md records the measured baseline and T1-04 handoff. Windows cross-view acceptance remains pending. Song regions already share Session clip notes (LIVE-03) |
| TRACK-01 | 1 (T1-04) | Universal tracks: one track type that holds audio, MIDI, instrument and pattern content | Partial | B: persistent TrackId registry and document/view adapters (format 29), checked references and legacy migration; C playback adapters and LIVE-04 resource/limit gates remain. See UNIVERSAL_TRACKS.md |
| PROD-05 | 1 (T1-06) | Piano Roll V1 completion: ghost notes, transpose command, split, editing several clips at once | Implemented | Transpose selected notes by semitone/octave from the piano-roll Edit menu; Shift+Up/Down transposes an octave as one undo step (#110). Split selected notes at the amber edit cursor from the same menu: both halves selected, pitch/velocity/probability preserved, one undo step, all-or-nothing capacity refusal (#115). Ghost notes: Edit > Ghost notes shows the other tracks' MIDI clips in the same scene, or this track's clips in the other scenes, dimmed and read-only behind the open Session clip; shorter clips repeat across its loop, audio/empty/linked cells are skipped, and ghosts are never selected, hit-tested, saved or put in undo. The choice is a view setting for the session (not saved; no format change). PianoRollProcessingTests, PianoRollTransposeTests, PianoRollSplitTests and PianoRollGhostNotesTests; build/test results in the feature PRs, Windows listening and manual menu/shortcut checks pending. Multi-clip editing: Edit > Ghost notes > Edit them with this clip edits the open Session clip together with the shown clips of its loop length (other lengths stay ghosts): their notes are merged into the piano roll, coloured by clip, and each edit is split back into the clip each note came from, keeping its id there; new and pasted notes go to the open clip; linked cells are edited once, frozen and recording clips are left out; one undo step covers every clip an edit touched; Clear clip still clears only the open clip. A view setting (not saved; no format change). PianoRollMultiClipTests. Existing tools: PROD-02 |
| UI-01 | 1 (T1-11) | Saved window layouts and multi-monitor / mixed-DPI support | Partial | Saved local layouts with main/detached geometry, maximized state, active view, device pane and keyboard visibility; save/reset actions and monitor-safe restoration. SongFlowTests covers persistence, malformed settings, simulated display changes and real detachable-host restoration. See WINDOW_LAYOUTS.md. Physical multi-monitor/mixed-DPI acceptance remains pending. |
| QA-03 | 1 (T1-12) | Windows hardware verification pass across Tier 1 features | Ongoing | Results recorded in WINDOWS_AUDIO_CHECKLIST.md with device, driver, buffer size and commit |
| MIX-06 | 1 (later) | Node routing view editing the same routing graph as the mixer | Planned | After MIX-03 to MIX-05 |
| RACK-01 | 2 (T2-02) | Instrument and effect racks with macros | Planned | Builds on the eight-slot FX racks and stock effects |
| CHORD-01 | 2 (T2-04) | Project chord track and arrangement sections | Planned | One chord/scale context for the whole project (feeds INTEL-01); MIDI-06 covers scale awareness |
| LIVE-05 | 2 (T2-05) | Follow actions, scene capture, record performance to the Song, crossfader, live looping | Planned | Starts after ARCH-01 is stable |
| INTEL-01 | 3 | Global musical intelligence: chord track -> piano roll -> patterns -> bass generator -> arpeggiator -> Session Players -> pitch correction -> arrangement | Planned | Every smart feature reads the same project context |
| PLAYER-01 | 4 | Intelligent Session Players | Planned | Needs INTEL-01 |
