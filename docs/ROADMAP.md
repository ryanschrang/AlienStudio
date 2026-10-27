# Alien Studio roadmap

## Priority plan: Alien Studio Core 1.0 (adopted 2026-10-26)

This section replaces the older ordering below as the source of truth for what to work on next.
The sections after it are kept as history.

**Rule: Tier 1 comes first.** Tier 1 is the DAW foundation and is the priority until each row is
complete. After that, expand the Tier 2 items that are only partly done. Tier 3 and Tier 4 are
not scheduled; they must not pull effort away from the foundation. Status for every ID lives in
FEATURE_CHECKLIST.md; this file says what order to do things in and how to split the work.

Until the Tier 1 systems are mature, Alien Studio should not be described as competitive with
established professional DAWs. Logic Pro, Ableton Live 12 and FL Studio set a high baseline:
recording, takes and comping, timing and pitch editing, routing, automation, global tempo and
key, markers, MIDI transformations and generators, MPE, scales and probability.

### Working on this roadmap with several people or agents

- **Claim before starting.** Open a draft PR (or an issue) whose title starts with the
  workstream ID, for example `T1-06: ghost notes in the piano roll`. One claim per workstream
  at a time; check open PRs for the ID before starting.
- **One small PR at a time.** A workstream is a series of PRs, each from its own branch off
  `main`, each with tests, test instructions and a verified / not verified split (AGENTS.md).
- **Stay inside your area.** The "Main area" column lists the files a workstream normally
  touches. Two workstreams that share a file can run at once, but the second to merge must
  merge `main` and rerun the tests. `src/ToneEngine.h`, `src/ProjectController.h`,
  `src/SessionProject.h` and `docs/FEATURE_CHECKLIST.md` are shared hot spots: keep edits to
  them small.
- **Project format numbers.** Only one open PR may claim a new project format number. Say
  the number in the PR title or description (the next free number is 18) and renumber if
  another PR merges first.
- **Update the checklist** row for the ID in the same PR, and never mark something verified on
  CI alone; Windows listening and hardware checks are recorded separately.
- **Dependencies** in the tables are hard: do not start a row before the rows it depends on
  have merged, or agree the shared design in a docs PR first.

### Tier 1: DAW foundation (priority)

Ordered by importance. Rows without dependencies can run in parallel.

| Order | Workstream | Checklist IDs | Status today | Depends on | Main area |
| --- | --- | --- | --- | --- | --- |
| T1-01 | Tempo and time-signature map | TEMPO-01 | Implemented on Song (#104, #114); Windows listening pending | - | `Timeline.h`, `StepClock.h`, `ToneEngine.h`, export |
| T1-02 | Command Engine (see below) | CMD-01 | Partial: structural track commands and track volume/pan, Execute/Undo/Redo/Batch/Preview; Session track menus use commands; remaining operations and other UI adoption pending | - | `CommandEngine.h`, `ProjectController.h`, `DocumentHistory.h` |
| T1-03 | One clip/pattern model shared by Create, Perform and Arrange | ARCH-01, LIVE-03 | Partial: Session library and linked cells implemented (#116, #124); direct region clip IDs and Channel Rack integration remain | Design doc first; T1-02 helps | `RackModel.h`, `SessionProject.h`, `Timeline.h`, `Arrangement.h` |
| T1-04 | Universal tracks and higher limits | TRACK-01, LIVE-04 | Partial: 16 tracks / 32 scenes; rack channels are a separate track kind | T1-03 design | `SessionCapacity.h`, `SessionProject.h`, `ToneEngine.h` |
| T1-05 | Mixer routing: groups, buses, sidechain, flexible returns | MIX-03, MIX-04, MIX-05 | Partial: group buses (#106), stock/VST3 sidechain routing (#119), four fixed returns, pre/post sends and latency compensation; flexible returns remain | - | `TrackMixer.h`, `ToneEngine.h` routing, mixer view |
| T1-06 | Piano Roll V1 completion | PROD-05 | Transpose, cursor split and ghost notes implemented; missing multi-clip editing | - | `PianoRoll*.h` |
| T1-07 | Recording completion | REC-02 | Done in code: loop-pass recording (#107) and waveform take lanes (#121); Windows listening checks pending | - | `SongRecording.h`, `TimelineView.h` |
| T1-08 | Audio editing completion | EDIT-01, WARP-03 | Partial: trim, split, menu/Alt-drag slip, gain, fades, crossfades; missing selectable fade curves and warp markers | Warp markers need T1-01 | `Timeline.h`, `TimelineView.h`, `TempoWarp.h` |
| T1-09 | Plugin reliability and coverage | PLUG-01, PLUG-02, INST-10, AUTO-03 | Partial: VST3 hosting and crash containment; missing scans, rack VST3 instruments, plugin automation playback, commercial plugin checks | - | `Plugin*.h`, `RackEngine.h` |
| T1-10 | Browser for samples, presets and plugins | BROWSE-01, INST-08 | Not started | - | new browser view; `ClipBin.h` |
| T1-11 | UI and workspace | UI-01 | Mostly done: detachable windows, menus, theme; missing saved layouts, multi-monitor/DPI checks | - | `DetachableView.h`, `MainComponent.h` |
| T1-12 | Windows hardware verification | QA-03 | Ongoing; needs people with audio interfaces, MIDI controllers and plugins | - | `docs/WINDOWS_AUDIO_CHECKLIST.md` |

**Good first parallel set:** T1-01, T1-02, T1-05, T1-06, T1-07, T1-09, T1-10 and T1-12 touch mostly
separate files. T1-03 and T1-04 should start with a shared design doc, because they change the
project model everyone else builds on.

#### Milestone A: project engine (T1-01 to T1-04)

Prove the skeleton: Project -> universal tracks -> clips -> patterns -> devices -> routing ->
automation -> tempo -> undo/redo -> save/load.

#### The Command Engine (T1-02)

Do not build the Workflow Assistant yet; build the Command Engine. Every user operation should
eventually be a named, undoable command, for example:

```
CreateTrack  DeleteTrack  MoveTrack  RenameTrack  ColorTrack
CreateClip   DuplicateClip  MakeClipUnique
SetParameter  AutomateParameter
CreateBus  RouteTrack  CreateSend
InsertDevice  MoveDevice  RemoveDevice
CreateGroup  AddToGroup
SetTempo  SetChord  SetArrangementSection
```

with **Execute, Undo, Redo, Batch and Preview**. This is not only for AI: it gives reliable
undo/redo, macros, keyboard actions, MIDI controller scripting, user scripting, batch processing
and workflow automation. The eventual assistant becomes a natural-language front end to the same
commands. The implemented first slice wraps the existing undoable track operations, with tests
that a batch is one undo step. Track volume/pan commands extend this foundation; remaining
operations and adoption by the existing UI follow in later slices.

#### Milestone B: MIDI production (T1-06, then Tier 2)

The target is the best compositional environment, not "has a piano roll".

- **Piano Roll V1:** draw, paint, delete, move, resize, split, duplicate, velocity, quantize,
  transpose, scale highlighting, ghost notes, multi-select, copy/paste, undo/redo.
- **Piano Roll V2:** probability, per-note expression, MPE, editing several clips at once,
  humanization, chord detection, chord tools.
- **Piano Roll V3:** generators, Euclidean rhythm, mutation, groove extraction, voice leading,
  harmonization, bass generation.

#### Milestone C: audio engine (T1-07, T1-08)

Audio tracks, recording, monitoring, input routing, waveform rendering, clip trimming, fades,
gain, crossfades, stretching, looping, punch recording, comping, multichannel recording and
latency compensation. When this works well, add warp-style processing.

#### Milestone D: mixer (T1-05)

Track -> insert chain -> sends -> bus/group -> master, plus sidechains, parallel routing,
groups, sends/returns, delay compensation, metering and automation. Later: a traditional mixer
and a node routing view (MIX-06) that edit exactly the same routing graph.

#### Milestone E: Create + Arrange (T1-03)

A pattern made in the Create views is the same object that launches in Perform (Session) and is
placed in Arrange (Song). Editing any representation updates every one of them. This is the
architectural feature to guard most carefully: it unifies the three workflows instead of
cloning them.

### Tier 2: what makes Alien Studio competitive (after Tier 1)

Expand the partly done items first. Emphasis on T2-01 and T2-02: the sampler and racks must be
genuinely integrated rather than checkbox features.

| Order | Workstream | Checklist IDs | Status today |
| --- | --- | --- | --- |
| T2-01 | Integrated sampler: audio clip -> quick sample -> slice -> multisample -> granular/resynthesis, one audio object throughout (Milestone G) | INST-07, PROD-03 | Partial: eight-pad drum sampler and sample channels |
| T2-02 | Instrument and effect racks with macros; remaining stock effects | RACK-01, FX-01, FX-02 | Partial: eight-slot FX rack per track and return, seven stock effects |
| T2-03 | Mixing and mastering environment | FINISH-02 | Partial: stock EQ, compressor, limiter, saturator, filter, reverb, delay |
| T2-04 | Chord and arrangement system: chord track, project-wide scale, arrangement sections (Milestone H foundation) | CHORD-01, MIDI-06 | Partial: chord builder and scales inside the Piano Roll |
| T2-05 | Live performance: follow actions, scene capture, record performance to the Song, controller mapping, crossfader, live looping (Milestone F) | LIVE-05, LIVE-02 | Partial: clip and scene launch on the beat or bar, MIDI CC mapping |

Milestone F (Perform) starts only after the shared clip model (T1-03) is stable.

### Tier 3: what could make Alien Studio better than competitors (not scheduled)

- **Source conversion pipeline (CONV-01 to CONV-03):** audio -> stems -> MIDI, chords and
  groove -> sampler or instrument -> editable pattern. Stem separation alone no longer
  differentiates (Ableton Live 12 has it); the pipeline does.
- **Global musical intelligence (INTEL-01):** one chord and scale context in the project
  model feeding the Piano Roll, patterns, bass generator, arpeggiator, Session Players, pitch
  correction and the arrangement, instead of eight disconnected "smart" features. Protect this
  direction in every Tier 1 and Tier 2 design.

### Tier 4: frontier features (not scheduled)

Large native instrument collection (INST-03 to INST-06, INST-09), advanced source separation,
intelligent Session Players (PLAYER-01), the Workflow Assistant (AI-01) and cloud collaboration
and version history (CLOUD-01).

**Advanced audio (Milestone I),** after the above: pitch editing (PITCH-01), spectral editing,
audio to MIDI, stem separation, transient/sustain decomposition, resynthesis and polyphonic pitch.

**Workflow Assistant (Milestone J), explicitly postponed** until the Command Engine is mature. It
then needs no special-purpose hacks: Interpret -> Plan -> Preview -> Execute -> single undo,
through the same commands everything else uses. Add an Explain mode that analyses and suggests
without changing anything: Analyze -> Explain -> Suggest -> Preview -> Apply -> Undo.

## Earlier roadmap notes

The integration branch combines the redesigned project/controller UI with drum kits,
arrangement/WAV export, disk-streamed recording and offline tempo matching. Project v6
also stores initial VST3 instrument/FX racks. See README for the supported workflow
and WINDOWS_AUDIO_CHECKLIST.md for unverified hardware/plugin checks.

The Song view (the arrangement tab, now a main view with a Song mode switch that plays the whole song from any view) is a full arrangement timeline (Session clips and Channel Rack patterns, snap/zoom/selection/loop/markers, WAV export of the song or loop; project v8).

Clips on the timeline have fade handles and equal-power crossfades (project v11).

Remaining work: Windows plugin/device listening tests; plugin/send automation and tempo-warped audio clips on the timeline; long-file streaming Session playback; waveform editing; live warp markers;
plugin automation, sidechains/multi-output, freeze and runtime crash isolation.

Tracks can be armed and recorded onto the Song with count-in, punch and latency compensation (REC-02); take lanes and comping for loop recording remain.

## Next: step sequencer, limits and chords (planned October 2026)

Planned, not implemented. Each item is its own branch and pull request, with tests and the Windows
checks it needs; order may change as work lands.

1. **Finish the Channel Rack "Next" items.** Done: multi-channel selection (#67), opening a channel
   straight into the piano roll (#68) and resizable name columns (#71).
2. **Step sequencer v2, together with raising the limits.** Part 1 is in review: project format 12
   adds per-step gate, nudge and ratchets, and the rack grows to 64 channels and 128 steps. The
   Session limits did not need a format change after all (tracks, scenes and notes are already
   variable-length lists in the file); they need new clip storage instead (see *Limits*). The rest:
   - *Per-step expression:* gate (note length), micro-timing nudge, ratchets/retriggers (2-4 hits
     inside a step) and trigger conditions (every Nth pass, first pass only, fill, not first) on
     top of the existing velocity, pan, pitch and chance.
   - *Per-channel timing:* each channel's own length (polymeter) and rate (1/8, 1/16, 1/32,
     triplets), and swing per channel as well as for the rack.
   - *Generators and tools:* Euclidean rhythms, seeded random fill with density, humanise,
     shift row, an undoable "variation" command, step entry from MIDI pads and quantized live
     recording into the rack.
   - *Channels that play notes:* a channel can drive the built-in synth or a VST instrument, with
     its own piano roll and scale-aware pitch steps (FL Studio's channel piano roll).
   - *Sampler controls per channel:* envelope, sample start, reverse, filter and fine tune, also
     drawable per step as graph-editor lanes.
   - *Limits:* rack channels and steps are raised (64 and 128). Still to do: more notes per clip
     than 1,024, more tracks/scenes than 16/32, and MIDI scene mapping beyond scenes 1-4. Every
     clip holds a fixed 1,024-note array (32 KiB), and the song snapshot copies one per track and
     scene (about 16 MiB, three of them in flight), so raising them as they are to, say, 64 tracks,
     128 scenes and 4,096 notes would make each snapshot about 1 GiB. Clips need shared,
     immutable, right-sized note storage (as rack patterns already have) before the limits can
     rise; memory, scheduling cost and real-time safety need their own tests.
   - *Ableton-style extras, later:* a 4x4 pad view of the same channels, note repeat, per-pad
     effects and sends (sends depend on MIX-02 bus routing), A/B pattern variations, pattern
     chaining and "split by channel" when placing a pattern on the Song.
3. **Chord production.** The piano roll already has a single-chord stamp, strum, inversion,
   arpeggiate and scale guides; the new work is about progressions:
   - *Chord pads:* choose a key and scale and get its diatonic chords (I-vii) as pads, playable
     from the mouse, the computer keyboard or MIDI, with voicing controls (spread, octave and
     automatic voice-leading).
   - *Progressions:* common progressions to audition and drop into a clip or onto the Song as
     one undo step.
   - *Chord effect on a track:* one played note sounds a chord.
   - *Chord track on the Song,* followed by other clips and by the rack's pitch steps (last: it
     touches playback, editing and export).
4. **Take lanes and comping** for loop recording (the rest of REC-02).

Historical milestone notes below describe the earlier stacked branches.

# Alien Studio delivery roadmap

Status: first track/mixer and portable-project implementation, pending PR review.
These are incremental milestones, not promises of full commercial DAW parity.
Every milestone retains one shared track/clip/project model and real-time-safe
processing. Each needs automated coverage and Windows listening/device testing.

## Reviewed existing work

PR #22 captures JUCE audio initialization failures and shows a deferred warning
through a SafePointer. It has no review findings and its Windows checks passed.
No confirmed defect was found in this review; it remains a separate unmerged PR.
Do not duplicate its change here or assume it is already on main.

## Implemented in this change

- Independent 16-voice Session synths with per-track presets and basic mixing.
- Project v3 persistence with v1/v2 preset migration and validated mixer values.
- Portable project copies with deduplicated audio and relative media references.
- Corrected README claims and a concrete Windows audio verification checklist.

Remaining boundaries: up to 16 tracks and 32 scenes (the engine pre-allocates
this capacity; raise maxSessionTracks/maxSessionScenes to grow it), 1,024 notes
per clip, 12 factory synth presets, MIDI CC mapping for scenes 1-4 only, live
input on a separate Device rack synth, and no recording or warping. Reordering
or deleting tracks/scenes stops Session playback at the next launch boundary. Audio clips play once at their original duration. A
quantized launch does not tempo-sync their audio. Media is still decoded into RAM
under the existing limits; collection is not streaming.

## Ordered feature milestones

1. **Drum sampler and browser.** Load original/licensed samples into pads, audition
   with velocity, map MIDI notes, and sequence distinct drum voices. Save all pad
   mappings and collect their media. Test polyphony, retrigger/choke, missing
   assets, stop/panic, and bounded rendering. Extend step lengths separately.
2. **Arrangement and WAV export.** Put existing audio/MIDI clips on a common
   timeline; move/duplicate/trim/loop with undo. Capture Session launches into
   arrangement. Render a defined song range and tails to WAV using the same DSP.
   Test timeline boundaries, sample-rate/block-size consistency, cancellation,
   and saved arrangements. Finish a three-minute example song as acceptance.
3. **MIDI recording.** Explicit track input/monitor selection, metronome/count-in,
   timestamped capture, overdub/replace, quantize, sustain, and undo. Preserve held
   note ownership when changing track; test disconnect/stop/loop boundaries.
4. **Audio recording and editing.** Input channels, arm/monitor, disk writer queue,
   latency alignment, trim/fade/crossfade, take lanes and comping. Test disk-full,
   underrun, cancellation, long recordings, and device changes without losing
   existing takes. Never perform filesystem work in the audio callback.
5. **Production mixer.** Parameter automation, insert effects, buses, sends,
   sidechain and delay compensation. Validate routing cycles, bypass/state recall,
   sample-accurate automation, overload behavior, and output/stem rendering.
6. **Plugin hosting.** VST3 SDK/license review, external scan worker, failed-plugin
   recovery, instrument/effect state, automation, latency reporting, and freeze.
   Verify real plugins on Windows; a passing mock host is insufficient.
7. **Advanced editing and assistance.** Quality-evaluated time stretch/warp and
   pitch correction, expressive MIDI/articulations, chord-aware accompaniment,
   stem separation and mastering assistance. Assess algorithm/model licensing,
   CPU cost, audible artifacts, and optional downloads before committing a design.

## Reliability alongside every milestone

Project-wide undo/redo, autosave/recovery and unsaved-changes prompts are in
place; extend them to each new feature (recording takes, arrangement edits,
plugin state). Grow track capacity with bounded audio scheduling. Add hardware
results to WINDOWS_AUDIO_CHECKLIST.md. Keep actual test results distinct from
planned tests, and never label hardware performance verified from CI alone.

## Drum sampler milestone update

Implemented: eight sample pads per track, local file browser, velocity-sensitive
one-shots, fixed 16-voice polyphony, hat choke pair, external MIDI routing,
piano-roll sequencing, v4 kit persistence and portable collection. Focused DSP
and integration tests cover reset, source isolation and allocation-free operation.
Still later: dedicated multi-lane 16/32/64-step drum editor, sample trimming,
streaming, factory sample packs, and more elaborate pad modulation.

## October 2026 implementation update

Implemented foundations now include per-track eight-pad sample kits/browser,
arrangement timeline with WAV export, disk-streamed mono/stereo input takes,
and offline fixed-tempo WSOLA rendering. Audio lab adds explicit input settings,
record state/meter, destination display and a separate tempo-render workflow.
Full recording/editing and warping milestones remain open: streaming playback,
latency compensation, count-in/punch/overdub, trimming/fades, take lanes/comping,
automatic BPM, warp markers and transient-aware algorithms. Windows listening and
visual checks remain separate gates from the passing offline tests.

## T1-03 design entry point

The numbered Core 1.0 priority plan is proposed in PR #100. Start T1-03
(ARCH-01 / LIVE-03) with [the shared clip/pattern model design](SHARED_CLIP_MODEL.md).
The design defines source identity, linked/independent copies, lossless rack data,
real-time publication, project migration and incremental acceptance gates.
It is a proposal; the common model and cross-view migration are not implemented yet.
