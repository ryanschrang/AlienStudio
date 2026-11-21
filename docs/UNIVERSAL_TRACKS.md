# T1-04: universal tracks and capacity foundation

Status: design/baseline (A) and identity/document adapters (B, format 29)
implemented. Playback adapters (C) are implemented: C1 (the callback processes
only the track slots in use), C2 (Song rack lanes saved apart from track rows,
format 32) and C3 (a build-time capacity setting with a measured, CI-tested
32-track, 64-scene candidate profile). Resource budgets (D) are in progress:
D1 sizes the plugin alignment delay lines to the latency actually reported; D2 reserves stream
voices for the track slots a project uses and sample-preview audio when something is previewed.
Higher supported limits (E) are pending.
Baseline: main `1587c17`, project format 17. Workstream TRACK-01 / LIVE-04 from
[the roadmap in PR #100](https://github.com/ryanschrang/AlienStudio/pull/100).
Dependency: [T1-03 shared clip/pattern design, PR #102](https://github.com/ryanschrang/AlienStudio/pull/102).

## Outcome

A track has one identity across Create, Perform, Arrange and the mixer. MIDI,
audio and an instrument are capabilities of that track, not separate project
models with incompatible identifiers. A Channel Rack row becomes a view/binding
of a regular track, rather than a second kind of track with copied musical data.
Master and existing auxiliary returns use the same identity/lookup contract but
retain their role-specific behavior.

More tracks, scenes and notes become supported only after document storage,
playback resources and routing have explicit capacity budgets. Merely changing
`maxSessionTracks` and `maxSessionScenes` is not this feature's implementation.

## Existing boundaries to migrate

| Area | Baseline contract / constraint | Required treatment |
| --- | --- | --- |
| Session tracks | `SessionTrack` in [ProjectDocument.h](../src/ProjectDocument.h) combines name, mixer settings, instruments/FX, drum media and a process-local identity. | Retain behavior while introducing the persistent `TrackId` from the shared-model design. Do not persist a runtime array slot as identity. |
| Rack channels | `rack::Channel` / `ChannelInfo` in [RackModel.h](../src/RackModel.h) own sample/synth configuration, volume/pan, choke and an output index. | Map each channel to a regular track and preserve its source/device settings and separate processing stage. |
| Rack-to-Session routing | A channel can feed the master or a Session track; channel level/pan can precede that track's level/pan/FX. | Preserve both processing stages. Combining their faders or pan values is not generally equivalent. |
| Returns/master | Four return strips, sends and master processing exist. Plugin runtime slots derive master/return offsets from maximum Session track capacity. | Stable track IDs must remain distinct from compiled plugin slot indices, including when limits change. |
| Arrangement | Rows discriminate Session clips from rack-pattern lanes by index; source scene/pattern indices still participate in playback. | T1-03 supplies explicit source identities. View row order and track identity must become separate concepts. |
| Note playback | `ToneEngine` preallocates one `PianoRollMailbox`, `SessionClipMailbox` and `PianoRollScheduler` per configured Session track, not per active track. | Measure these costs; shared document notes have not removed scheduler/launch copies. |
| Audio processing | Delay lines, MIDI/audio buffers, voices, streamed readers and callback loops also scale with configured capacity. | Profile their allocations and CPU separately before selecting a larger supported profile. |

Session tracks already accept audio or MIDI clips. The migration must preserve
that existing flexibility, not introduce an audio-only versus MIDI-only type
split. Source/instrument choice is a device/binding property.

## Universal track contract

Use the typed, project-scoped `TrackId` and clip identities proposed by T1-03;
do not introduce a competing ID generator or a second registry in T1-04.

| Proposed responsibility | Stored in the project | Compiled for playback |
| --- | --- | --- |
| Identity/presentation | `TrackId`, name, colour, view membership/order and role | Resolved track/slot mapping, independent of view order |
| Role/capabilities | Regular, auxiliary return or master role; permitted launch/record/device operations | Validated capability flags; no dynamic role inference in the callback |
| Content bindings | Launch/placement references and lane-to-track bindings from T1-03 | Resolved clip/pattern revision and bounded playback instances |
| Devices | Existing sample/synth/drum/instrument and effect state; existing plugin instance IDs | Prepared processors/voices and preallocated working buffers |
| Mixer/input | Existing gain, pan, mute/solo, input/monitor/arm, sends and output bindings | Lock-free controls plus validated processing order |
| Freeze | Current cache reference, archived source placements and original devices | Existing frozen-cache playback/suspension behavior |

A regular track can carry notes, audio and an instrument chain. This does not
require a master/return to accept clip launches or recording: role restrictions
are explicit validation rules. Track identity is also independent of plugin
instance identity; automation targets must survive this migration unchanged.

View membership is saved metadata, not a clone: hiding a track from the Rack
view cannot delete it from Perform/Arrange or create another musical source.
Ordering a Rack row or mixer strip changes view order and resolved bindings,
not the track's `TrackId`. Existing launch quantization, per-occurrence note
ownership, input monitoring and explicit stop/panic behavior remain contracts.

### Preserve routing during legacy import

For every legacy rack channel, create a track/device stage holding its original
sample/synth selection, choke group, volume law, pan and mute/solo semantics.
Keep an output binding to its former Session target or the master. Existing
Session destinations remain separate stages, with their existing mixer/FX and
input controls. All four returns and master retain their prior behavior.

Do not normalize rack volume 0.8 to track gain 0.8 by copying the raw value:
the rack's 0.8 default represents unity. Do not reinterpret relative rack pitch
as absolute note pitch, move effects to another stage, double-apply automation,
or consolidate two stages without a tested, explicit user operation.

Track deletion/reorder must update references transactionally. Explicit legacy
track deletion retains the current channel-output fallback to master, and undo
restores the original binding. A malformed saved internal reference must be
rejected rather than bound to a different track that reused an index. General
new groups, arbitrary buses and sidechain graph editing remain T1-05 work; this
migration needs only a validated, behavior-preserving representation of the
existing routes and rejection of invalid/cyclic new bindings.

## Capacity audit: what this increment measures

Build `TrackCapacityReport` with `BUILD_TESTING=ON` (the existing development
configuration) and run it. [The source](../tools/TrackCapacityReport.cpp) uses
`sizeof` on actual compiled types; it never constructs an engine, opens a device
or allocates a proposed large project. Its Markdown output records ABI-dependent
sizes and labels every hypothetical row.

The report measures the three selected per-track note objects and the shallow
source-reference tables in three Song snapshots. The three note objects contain
fixed note/event storage; sharing Session document/snapshot pointers does not
remove them. Profiles beyond 16/32 retain the compiled 1,024-note limit and
current layout. They are cost projections, not supported configurations or
claims that the engine will play those projects.

The report is deliberately not an engine-memory estimator. It excludes dynamic
plugin state/processors, delay sample vectors, decoded media, streamed readers
and buffers, voice pools, document/history data, UI and allocator overhead.
`sizeof(ArrangementPlayback)` and rack object sizes are shallow and exclude
pointees. Reference-table estimates exclude snapshot headers and shared payloads.
Do not add these figures to overlapping measurements or treat them as RSS.

T1-03 E also records the fixed rack pools and real offline callback workloads in
[the shared playback baseline](SHARED_PLAYBACK_CAPACITY.md). Those measurements
cover current supported limits, not hypothetical larger profiles.

See [the recorded Windows baseline](TRACK_CAPACITY_BASELINE.md). Rebuild the tool
when data layouts or capacities change. A note-limit change needs a new build
and scheduler/event measurements; do not assume a track-only projection also
models more notes.

### Measurements still required before increasing limits

1. Record the current full engine's prepared/idle/playing memory, including
   heap-owned buffers and plugin delay allocations; capture peak allocation
   during load, undo/redo, source replacement, freeze, export and snapshot handoff.
2. Measure callback time on representative supported hardware with notes, audio,
   sends/returns and plugins, at several buffer sizes. Record the hardware,
   sample rate and load; compilation or the capacity report is not performance
   evidence. A single CPU percentage is not a general track-count guarantee.
3. Measure loaded versus active track costs and demonstrate no callback
   allocation/locking or final ownership destruction during resource handoff.
4. Define explicit bounded resources for voices, event schedules, stream readers,
   plugin MIDI/audio buffers and delay lines. Report an actionable refusal when
   resources cannot be prepared; never silently drop tracks, notes or audio.
5. Audit every capacity/index boundary: Session grid and scene launch/MIDI scene
   mapping, row classification, rack output types, plugin slot maps, stream
   voices/tails, sends, recording arming, project validation and export buses.

Keep the current supported 16 tracks / 32 scenes / 1,024 notes until those gates
and the shared-model migration pass. A 32/64 profile is a candidate to evaluate,
not an approved new limit. The 500-track report row is explicitly hypothetical;
it is not a promise of 500 active instruments or 500 mixer strips.

## Migration and delivery order

| Increment | Deliverable | Required gate |
| --- | --- | --- |
| A: design and baseline (this PR) | Universal-track contract, existing-route migration rules, compiled capacity diagnostic and recorded results | Source trace, diagnostic Debug/Release build/run, clear measured-versus-unmeasured boundaries |
| B: identity and document adapters | One track registry and persistent TrackIds shared with T1-03; existing Session/rack views adapted without a new engine capacity | Round-trip legacy media/devices/routes, uniqueness/reference validation, transactional failure, reorder/delete/undo; no parallel writable track ownership |
| C: routing/playback adapters | Compile existing track/device/output bindings to current bounded engine slots; retire obsolete model-specific identity paths | Sample-accurate live/export comparison, no stuck notes, route/solo/choke equivalence, sends, returns, latency, freeze, recording and callback safety |
| D: resource allocation and limit candidate | Prepare resources outside the callback; use measured budgets rather than multiplying every large fixed object by the document maximum | Memory/CPU results and safe capacity refusals; old snapshots/resources outlive readers; no device interruption during an unsupported partial transition |
| E: supported limit release | Raise one tested profile and update validation, UI, import/export and documentation together | Save/reopen/portable/undo, launch every supported scene, duplicate/reorder/delete boundary cases, audio/recording/export stress, Windows hardware evidence |

The project file version is chosen when persistent track references ship; this
increment does not reserve or bump it. Earlier readers must refuse a future
schema they cannot preserve. Legacy tracks, channels, returns and master obtain
IDs once; ordinary edits and undo retain them. A load failure leaves the open
document and source file intact. Freeze/take source archives and plugin
parameter targets are included in validation, not just audible clips.

Coordinate B/C with T1-03's library/clip binding adapters. Coordinate named edits
with T1-02 rather than writing a separate undo engine. Coordinate routing
extensions with T1-05. None of these coordination points authorizes increasing
limits before the measured resource gates are satisfied.

## Completion criteria

T1-04 remains partial until:

- a track has one stable identity across all views and the mixer, including
  legacy rack channel content, routing, audio/MIDI capabilities and devices;
- edits, copy/duplicate, deletion/reorder and undo have explicit tested identity
  semantics; save/reopen preserves IDs without confusing them with engine slots;
- stems, Song/Session/rack playback, recording, monitoring, sends/returns, freeze
  and plugin automation bindings preserve their behavior after migration;
- at least one larger documented profile passes the full correctness/resource
  tests and representative Windows device/performance checks; unsupported
  projects fail without truncation or data loss.

Relevant suites include ProjectControllerTests, SessionProjectTests,
RackModelTests, RackEngineTests, RackProjectTests, RackIntegrationTests,
SessionViewTests, TimelineViewTests, ToneEngineTests, TimelinePlaybackTests,
SongRecordingTests, MixerRoutingTests and DeterministicStreamedExportTests.
Future registry/adaptor behavior needs focused tests; this diagnostic alone is
not evidence that universal tracks or higher limits already work.

## Increment B: persistent identity and document adapters (format 29)

`TrackRegistry` is the one project-scoped authority for typed `TrackId`s.
It owns identities and bindings, not a second copy of names, controls, media or
devices. Existing Session tracks, rack channel device stages, master, active
returns and active group buses keep their current document data. Registry owner
adapters resolve those same objects from every view. Session runtime tokens and
rack lane IDs are adapter keys; they are never serialized as track identities.
The saved adapters use stage/slot pairs to bind each owner to its persistent ID.
A rack stage remains distinct from its Session output stage, preserving both
volume laws, pan, choke, instruments and processing order.

Reorder retains IDs; rack duplication gets a fresh ID; deletion retires it.
Undo restores the exact registry and routes. The issuance high-water mark survives
undo/redo so branching edits cannot reissue IDs. Removing and recreating a return
or bus in the same slot creates a new owner. Registry changes are message-thread
work; the callback continues to consume existing bounded slots.

Format 29 requires one nonzero, unique ID per existing owner, a strict hex
allocator watermark, and checked identity references for rack/Session outputs,
Song regions (including take/freeze archives), mixer automation and sidechain
keys. Unknown, duplicate, wrong-stage or mismatched references reject the load
before replacing the open document. Saved slot caches must agree with the
identity references. Plugin automation keeps its existing plugin-instance IDs.
Formats 1-28 obtain a registry on import and retain it on the next save. Earlier
readers refuse format 29. No track, scene, note or audio resource limit changes.

Session/rack deferred rename callbacks resolve the original identity after a
reorder and ignore a deleted owner. Song take-lane state and clipboard track
bindings use TrackIds. A separate unsaved document-scope fence prevents a
project-scoped ID from aliasing an owner after creating/opening another project.

This is the B adapter boundary, not the C playback migration: existing engine
slots and index-shaped document caches are still present and validated. C must
compile routing/playback from the shared identities and retire those caches with
live/export, freeze, recording and resource-safety evidence. No new capabilities
or higher limits are claimed here.

Validation is recorded in the implementation PR. Focused controller tests cover
identity persistence, legacy import, routing, reorder/duplicate/delete, undo/redo,
ID issuance after undo and malformed-file atomicity; rack UI tests cover deferred
rename across reorder, deletion and document replacement. Windows physical-device
listening, commercial plugins and multi-monitor/DPI checks remain separate.

## Increment C1: the callback processes only the track slots in use

No project format or limit change. The engine still prepares one bounded slot
per configured Session track at construction; slot `t` is the track at view
position `t`, and the controller still publishes `activeTracks` after every
structural edit (B). C1 changes what the callback does with that count:

- **Only slots in use are processed.** Each block reads `activeTracks` once.
  Per-track work in `ToneEngine` (launches and schedulers, synth/drum render,
  clips, rack buses, inputs, mixer, solo, sends, Song lanes, take capture) and in
  `PluginRuntime` (instruments, FX, sidechain keys, latency alignment) runs for
  those slots only. Rack routing uses the same per-block count, so a rack
  channel whose output names a slot out of use goes to the master, as before.
- **A slot leaving use** is processed one more block: its scheduler unloads its
  clip and ends every note, its streamed clip and crossfade tail stop, its synth
  and drum voices are cut, a plugin instrument gets all-notes-off and a playing
  rack occurrence is asked to stop at its next launch boundary (as deleting a
  track already did). Afterwards it is skipped, its sidechain keys are zeroed
  and its alignment delay lines *forget* their contents in constant time
  (samples written before the forget read as silence), so no callback zeroes a
  96,001-frame line.
- **A slot coming into use** starts from silence: its instrument gets
  all-notes-off and its meter restarts at zero. A launch published for a slot
  out of use waits in its mailbox and plays once a track uses the slot.
- **Bounds tied to 16 tracks are derived from the limit:** the synchronized
  scene-launch mask is 64-bit (`maxSessionTracks <= 64` is asserted), and stream
  voices are four per slot (`voiceCount = 4 * maxSessionTracks`, asserted in
  `ToneEngine`). On main the fixed 48 voices left Session tracks 9-16 without
  crossfade-tail voices (their binding index ran past the pool); at 16 tracks
  the pool is now 64 voices.

Nothing is allocated, freed, locked or logged on the audio thread for a slot
entering or leaving use.

Tests: TrackCapacityTests (new) runs the controller at the compiled limits (every
track launching the last scene, each strip audible, steady playback allocating
nothing, stop leaving every slot silent), a slot leaving use going silent from
its next block and bringing nothing back when reused, a launch waiting for its
slot, and device restarts (including one while the slot set shrinks) leaving no
note sounding. PluginRuntimeTests checks that a reused slot never plays audio
left in its alignment delay and still aligns new audio. RackIntegrationTests
`--capacity-report` prints the prepared engine heap (now counting over-aligned
allocations) and callback times with the number of tracks in use per row.

C1 does not change identity, persistence or limits. C2 (below) replaces the
`16 + lane` Song rack-lane rows with saved lane numbers; C3 measures and adopts a
larger profile.

## Increment C2: Song rack lanes saved apart from track rows (format 32)

Formats 8-31 saved a Song region on rack lane n as row `16 + n`, right after a fixed 16 track
rows. Raising the track limit would make those numbers collide with track rows. Format 32 saves
a track region's row as before and a rack-lane region as row `-1` plus a separate `"lane"` (0-3),
so a saved row never depends on the compiled track limit (`timeline::savedRow`, `savedLane`,
`rowFromSaved` in `Timeline.h`).

- Formats 8-31 still load: a saved row of 16-19 becomes lane 0-3.
- A format 32 row past the tracks without a lane, a lane out of range or not an integer, and a
  row other than -1 with a lane are all refused, and the loaded project is kept.
- Takes and frozen originals are saved by the same code, so they follow the same rule.
- Readers older than format 32 refuse format 32 projects, as for every format bump.

No track, scene or engine change. Tests: `SessionProjectTests` (`songRowFormatTests`), a
project at the track and scene limits round-trips byte for byte.

## Increment C3: a measured 32-track, 64-scene candidate profile

No project format change, and the supported limits stay 16 tracks and 32 scenes. C3 makes the
capacity a build setting, so that a larger profile can be compiled, tested and measured, and
records what that profile costs. Raising the supported limit is still D and E.

- **Build setting.** `ALIEN_SESSION_TRACKS` (16-64) and `ALIEN_SESSION_SCENES` (32-256) in
  CMake set `maxSessionTracks` and `maxSessionScenes` (`src/SessionCapacity.h`). The defaults
  are 16 and 32, and `isSupportedCapacityProfile` is true only for them. The bounds are asserted:
  scene launches wait on a 64-bit track mask, and formats 8-31 numbered rack lanes after 16 rows.
- **CI.** A `capacity-candidate` job builds the 32/64 profile in Release, runs every test and
  prints both capacity reports. It uploads nothing: that build is for evidence, not release.
- **Saved files do not depend on the profile.** Session cells are saved as nested lists, Song
  rack lanes as their own number (C2), returns and buses by their slot, and plugin automation by
  plugin instance ID. A 32/64 build reads and writes the same format 38 as a 16/32 build.
- **Too many tracks or scenes is refused, not truncated.** A build that opens a project with more
  tracks or scenes than it was compiled for now says how many the project has and how many the
  build supports; the open project is kept (`SessionProjectTests`, `capacityRefusalTests`). A 16/32
  build therefore refuses a project saved with 17 or more tracks by a 32/64 build. Until E ships
  one supported profile, projects past 16/32 are not portable between builds.

### Boundary audit at 32/64

Every use of `maxSessionTracks` and `maxSessionScenes` in `src` was checked. Arrays sized by the
limit (engine slots, plugin racks, mailboxes, schedulers, stems, recording sinks, drum pads),
rack bus numbers, plugin rack indices (master, then returns, then buses, after the tracks), the
Song track and lane rows and the stream voice pool (four per slot) all follow the constant. The
Session grid and the Song view already scroll. Fixed bounds that do not follow it, by design:
`legacySavedRackRowBase` (16, for formats 8-31), the MIDI controller's scene buttons (scenes
1-4, `MidiControllerMappings::sceneCount`) and the 16 MIDI channels a plugin accepts.

The whole suite (78 tests) was run on the 32/64 build. Two failed, neither in the app:
`ProjectControllerTests` assumed 12 added tracks reach the limit (it now adds up to the compiled
limit), and `SessionViewTests` overflowed the 1 MiB default stack, because each test keeps an
engine, controller and view on the stack (MSVC `/analyze` C6262: `layoutChecks` uses 1.16 MB at
32/64). It now links with an 8 MiB stack, like ProjectControllerTests and TimelinePlaybackTests.
The same analysis found no `src` function in that translation unit above 66 KB of stack, and the
app creates `MainComponent` (and so its engine) on the heap.

### Measurements

Recorded 2026-11-17 on Intel Core Ultra 9 288V, 32 GB, Windows 11 x64, MSVC 14.51 (Visual
Studio 18 2026), Release, offline: no audio device, plugins or media, other desktop activity
uncontrolled. One run per profile; reproduce with the commands in
[SHARED_PLAYBACK_CAPACITY.md](SHARED_PLAYBACK_CAPACITY.md) on each build directory.

| Engine memory (RackIntegrationTests `--capacity-report`) | 16 / 32 | 32 / 64 |
| --- | ---: | ---: |
| `sizeof(ToneEngine)`, inline object | 100,008 B | 144,888 B |
| Engine heap after construction and `prepare(48 kHz)` | 89.3 MiB | 130.4 MiB |
| Selected note state (TrackCapacityReport) | 5.28 MiB | 10.56 MiB |
| Three Song source-reference tables | 36,864 B | 147,456 B |
| `ArrangementPlayback`, shallow | 12,616 B | 49,480 B |

Each extra track slot costs about 2.6 MiB of engine heap whether or not it is used. Most of it is
the two 96,001-frame stereo alignment delay lines per slot in `PluginRuntime` (about 0.73 MiB
each), then the note mailboxes and scheduler (0.33 MiB). The heap figure counts C++ `new` only;
it excludes plugins, media, snapshots, document/history, UI and allocator overhead.

| Callback, p95 / block budget (48 kHz) | Tracks in use | 64 frames | 256 frames | 1024 frames |
| --- | ---: | ---: | ---: | ---: |
| 16/32 build: transport, no clips | 16 | 6.6 % | 5.9 % | 5.7 % |
| 32/64 build: transport, no clips | 16 | 6.4 % | 5.7 % | 5.5 % |
| 32/64 build: transport, no clips | 32 | 18.9 % | 15.7 % | 15.8 % |
| 16/32 build: every Session track plays MIDI | 16 | 8.8 % | 12.8 % | 12.0 % |
| 32/64 build: every Session track plays MIDI | 32 | 28.7 % | 24.4 % | 23.3 % |
| 16/32 build: a rack occurrence on every track | 16 | 13.3 % | 12.1 % | 12.9 % |
| 32/64 build: a rack occurrence on every track | 32 | 32.7 % | 30.4 % | 29.5 % |

Every row produced finite, audible output with zero callback allocations. With 16 tracks in
use, the larger build costs about the same as the 16/32 build (C1: slots out of use are
skipped). With all 32 in use the p95 cost is 1.9 to 3.3 times the 16-track cost, more than
linear at 64 frames. One run per profile cannot say whether that is cache pressure or noise;
a repeated, controlled measurement is part of D. These are sparse offline workloads, not
physical-device latency or a supported-limit guarantee.

### What C3 does not do

- It does not raise the supported limit or change saved projects; release builds stay 16/32.
- Per-slot memory is still fixed by the compiled maximum. Before a profile above 16/32 is
  supported (D), the delay lines and note state need budgets that do not multiply by the
  maximum: for example, delay lines sized to the latency actually reported (done in D1), or
  prepared only for slots in use, outside the callback.
- Not measured: physical devices, plugins, media, dense note/ratchet loads, peak memory during
  load/undo/export, Debug timings, and a 64-track profile.

## Increment D1: alignment delay lines sized to reported latency

No project format change; the supported limits stay 16 tracks and 32 scenes.

C3 found that most of each track slot's 2.6 MiB was two 96,001-frame stereo delay lines in
`PluginRuntime`, which line every track up behind the slowest plugin chain. Every line held the
maximum delay, whether or not any plugin had latency. D1 sizes them to what the plugins report.

- **Four stages, sized apart.** The track lines (each track and `live`), the source lines that
  wait for the Channel Rack's plugin instruments, the bus stage (bus chains, sends, the direct
  mix) and the return stage (return chains, the dry mix). Each stage's lines are as long as the
  largest latency that stage must give, rounded up to 1,024 samples. A stage with no latent
  plugin holds one sample per line.
- **Allocated off the callback, adopted at the block start.** `reserveAlignment` (message
  thread) allocates a stage's new storage and publishes it through an atomic pointer; the
  callback takes it at its next `begin()` and hands the old storage back through a second one,
  which the message thread frees. No allocation, lock or free in the callback.
- **When it grows.** After each plugin slot is prepared (grow only), after a whole set of plugins
  is restored (fit exactly, so removing a latent plugin frees its memory) and on every plugin
  host poll (grow only, so a plugin whose latency wobbles does not reallocate).
- **A plugin that reports more latency while playing.** Until the next poll grows its stage, the
  callback delays by as much as the line holds and counts a shortfall block
  (`alignmentShortfalls`). The stage that grows starts empty, so it loses at most the audio it was
  delaying, at a moment the latency changed anyway; the other stages are untouched. An export
  that sees a shortfall fails with a message instead of writing tracks that are out of step.
- **Not changed.** The Channel Rack's per-instrument lines keep their full length while an
  instrument is loaded (they were already allocated only then). `maxDelay` is still the most any
  chain can be compensated for.

`PluginRuntimeTests` covers it: an idle runtime reserves no alignment audio; a 4-sample plugin's
stage is one granule and the others stay empty; a plugin that jumps to 3,000 samples while
playing is clamped and counted once per block with no callback allocation; the host's poll grows
only the track stage, the next block adopts it without allocating, and an impulse on the other
track then comes out at exactly sample 3,000; polling never shrinks, a restore does.

### Measurements

2026-11-19, the same machine as C3, Release, 16/32, offline. Run back to back with the build of
main before this change, alternating, two runs each:

| `RackIntegrationTests --capacity-report` | Before | D1 |
| --- | ---: | ---: |
| Engine heap after construction and `prepare(48 kHz)` | 89.4 MiB | 45.4 MiB |
| p95 / budget, 16 tracks, transport, 64 frames | 8.1-9.3 % | 9.1-9.9 % |
| p95 / budget, 16 Session MIDI tracks, 64 frames | 17.3-18.8 % | 15.5-16.2 % |
| p95 / budget, 16 Session rack occurrences, 64 frames | 20.2-21.0 % | 19.9-20.1 % |

The callback cost is the same within run-to-run noise. Every run on this day was slower than the
2026-11-17 C3 run of the same code, so compare rows within one table only. 44 MiB is
the 60 track, source, bus and return lines at 0.73 MiB each; per extra track slot the fixed cost
falls from about 2.6 MiB to about 1.1 MiB. The 32/64 build was not re-measured. (D2 found that
"the rest" was mostly the four stream voices per slot, 0.75 MiB, and that 22 MiB of the remaining
heap was the sample preview, not a per-slot cost.)

Not done in D1: note state and mailboxes still scale with the compiled maximum, peak memory during
load/undo/export is not measured, and no latent third-party plugin was tested; the test plugin
reports a latency it is told to.

## Increment D2: stream voices and sample preview reserved as they are used

No project format change; the supported limits stay 16 tracks and 32 scenes.

After D1 the engine still requested 45.4 MiB at construction. Measuring it allocation by
allocation (C++ `new`, grouped by object) found two fixed costs that did not depend on the project:

- **Stream voices.** `DiskAudioStreamService` built all 64 voices (four per compiled track slot:
  two for a slot's clips, two for a crossfade's outgoing clip) in one block. Each voice is an
  8,192-frame ring of 24-byte frames, about 0.19 MiB, so 0.75 MiB per slot whether the slot was
  ever used.
- **Sample preview.** The browser's preview kept three slots of five seconds of stereo at
  192 kHz each: 22 MiB, allocated even if nothing was ever previewed.

What D2 changes:

- **A voice exists once a slot is used.** `DiskAudioStreamService::reserve(index)` (control
  thread) allocates one voice and publishes it through an atomic pointer; the worker skips voices
  not reserved. Voices are never freed while the service runs, so a pointer once handed out stays
  valid. `ToneEngine::setActiveTracks(n)` reserves the four voices of every slot below `n`, then
  publishes the track count; at its next block the callback binds those slots' players (pointer
  assignment only). `ProjectController` and the WAV export set the count this way, and so do the
  tests. A new engine reserves the default project's four tracks. Voices are kept when a project
  shrinks, so the cost is the most track slots the project has used.
- **A slot without voices** has no streamed clip to play: its player already refuses one cleanly
  (`queue` answers false). The voices of a slot are reserved before the track count that uses it is
  published, so this does not happen in the app.
- **Deterministic export.** `setDeterministic` now also applies to voices reserved afterwards, so a
  render whose project grows after it starts is still served on the rendering thread.
- **Preview audio grows on the GUI thread.** A preview slot's storage is resized in `request`,
  while the GUI owns the slot (state `writing`); the audio thread never reads a slot it has handed
  back as free. A slot keeps the length of the longest preview it has held, up to the same five
  seconds at 192 kHz. Nothing is reserved until the first preview.

Tests: `DiskAudioStreamTests` (no voice before `reserve`, reserving twice returns the same voice);
`DeterministicStreamedExportTests` (a new engine holds 16 voices; growing to seven tracks reserves
28 and shrinking keeps them; a streamed clip on track 7, whose voices were reserved after
construction, plays live within 3e-7 of its export, with no stream failure or underrun);
`ToneEngineTests` (an idle preview holds no audio; each preview reserves its own length in one
slot; only the slot that needs more grows; the grown slot plays its whole preview; the length
limit is unchanged; the existing allocation-free callback check still passes).

### Measurements

2026-11-20, same machine, Release, 16/32, offline (`RackIntegrationTests --capacity-report`):

| Engine heap (C++ `new`) | After D1 | D2 |
| --- | ---: | ---: |
| After construction and `prepare(48 kHz)` (four tracks reserved) | 45.4 MiB | 14.5 MiB |
| With every slot reserved (`setActiveTracks(16)`) | 45.4 MiB | 23.5 MiB |

The 9.0 MiB between the two D2 rows is the 48 voices of slots 5-16. Callback p95 on this run: 16
tracks transport 6.3 % at 64 frames, 16 Session MIDI tracks 8.3 % (64) and 9.0 % (256), 16 Session
rack occurrences 13.6 % (64), with zero callback allocations; this machine was faster today than
in the D1 run, so these are not a before/after comparison. JUCE `HeapBlock` storage (such as the
plugin MIDI buffers) is allocated with `malloc` and is not in these figures.

What still scales with the compiled maximum: the note mailboxes and schedulers (0.33 MiB per slot,
TrackCapacityReport), the per-slot mixer, synth and sampler objects, and the voices of slots a
project once used. Not measured: the 32/64 build, peak memory during load/undo/export, and a
physical device.
