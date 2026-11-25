# T1-04: universal tracks and capacity foundation

Status: design/baseline (A) and identity/document adapters (B, format 29)
implemented. Playback adapters (C) are implemented: C1 (the callback processes
only the track slots in use), C2 (Song rack lanes saved apart from track rows,
format 32) and C3 (a build-time capacity setting with a measured, CI-tested
32-track, 64-scene candidate profile). Resource budgets (D) are in progress:
D1 sizes the plugin alignment delay lines to the latency actually reported; D2 reserves stream
voices for the track slots a project uses and sample-preview audio when something is previewed;
D3 reserves each slot's note mailboxes and scheduler with it; D4 measures peak memory during
save, open, undo/redo and export; D5 frees the audio of a replaced project once the engine lets go of it; D6 frees old Song and Channel
Rack snapshots one audio block after the engine leaves them; D7 refuses an open or added track whose
engine slots cannot be prepared, with a reason, and changes nothing; D8 reserves the Channel Rack's
pending-hit queue for the rack channels a project has, instead of all 64; D9 keeps a block's Channel
Rack plugin notes from losing a note-off when there are more notes than the block's list holds; D10
tells the user, in the status bar and after an export or freeze, when Channel Rack hits were not played;
D11 bounds the MIDI a Song or Session track's instrument is given each block, counts what is dropped
and reports it the same way; D12 reports, by track, streamed audio that dropped out or could not be
read during playback; D13 reports plugin latency that put tracks out of step while playing.
D14 frees the Channel Rack samples, slice views and key maps a replaced project used.
D15 sizes each Channel Rack plugin instrument's delay line to the latency it must wait for.
D16 frees, during a session, Session and Song audio that no undo step or the clipboard can bring back.
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
  instrument is loaded (they were already allocated only then). D15 sizes them too. `maxDelay` is still the most any
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

### Comparing both profiles after D2

The Windows CI capacity job runs the Release capacity reports for 16/32 and
32/64 on the same runner at the same commit. The 32/64 full test suite runs
first. The two reports record engine allocation and offline callback cost
using the existing workloads. Compare timing within a job and repeat locally
for release decisions. The hosted runner does not exercise physical audio hardware.

To reproduce locally, configure separate x64 build directories with
`ALIEN_SESSION_TRACKS` and `ALIEN_SESSION_SCENES` set to 16 and 32, then 32 and
64. Build the Release `TrackCapacityReport` and `RackIntegrationTests` targets
in both directories. Run `TrackCapacityReport.exe` and
`RackIntegrationTests.exe --capacity-report` for each profile on the same machine.

## Increment D3: note state reserved with the track slot

No project format change; the supported limits stay 16 tracks and 32 scenes.

After D2 every compiled slot still carried its note state from construction: the note mailbox
(three 1,024-note buffers, 96 KiB), the launch mailbox (96 KiB) and the note scheduler (146 KiB),
0.33 MiB per slot (TrackCapacityReport) whether a project used the slot or not.

What D3 changes:

- **Note state exists once a slot is used.** A `SlotStore` holds one object per slot behind an
  atomic pointer. `ToneEngine::setActiveTracks(n)` (control thread) reserves the note mailbox,
  launch mailbox and scheduler of every slot below `n` together with its stream voices, and only
  then publishes the count. Objects are never freed while the engine lives; a smaller project keeps
  them.
- **The callback reads only adopted slots.** At the start of a block it adopts the slots reserved
  since the last one (`boundSlots`), and the track count it processes is limited to those slots, so
  a count that arrives before its slots are adopted, or one stored without `setActiveTracks`, never
  reaches a missing object. Loops that used to stop every compiled slot's notes (device start/stop,
  transport stop, panic, a timeline seek) now cover the adopted slots; the others never played.
- **GUI publishes never meet a missing mailbox.** Indexing `pianoRollNotes` or `pianoRollLaunches`
  from the control thread creates that slot's mailbox if needed, so a clip published to a slot
  before the project grows waits there, as before, and plays once the slot is in use.

Tests: `ToneEngineTests` (a new engine holds note state for the default four tracks; a launch
published to slot 16 before it is reserved creates its mailbox only; a track count stored without
reserving processes nothing beyond the adopted slots; after growing, the waiting clip plays with no
callback allocation; shrinking ends its notes and keeps the state); `DeterministicStreamedExportTests`
(growing to seven tracks reserves seven slots' note state and shrinking keeps it).

The callback now starts with no slot in use. It used to start as if every compiled slot had been,
so its first block ended the notes of slots 5-16 although they had never played; with D3 that would
read note state that does not exist.

### Measurements

2026-11-21, same machine, Release, 16/32, offline (`RackIntegrationTests --capacity-report`):

| Engine heap (C++ `new`) | D2 | D3 |
| --- | ---: | ---: |
| After construction and `prepare(48 kHz)` (four tracks reserved) | 14.5 MiB | 10.5 MiB |
| With every slot reserved (`setActiveTracks(16)`) | 23.5 MiB | 23.5 MiB |

The 4.0 MiB saved is the note state of slots 5-16 (12 x 0.33 MiB). It is a saving for projects
with fewer tracks than the build allows; a project that uses every slot holds the same memory as
before. Callback p95 on this run: 16 tracks transport 7.6 % at 64 frames, 16 Session MIDI tracks
9.6 % (64 and 256), 16 Session rack occurrences 14.7 % (64), with zero callback allocations. This
is a different day from D2's run, so it is not a before/after comparison.

What still scales with the compiled maximum: the per-slot mixer, synth, sampler and send objects,
and the resources of slots a project once used. Not measured: the 32/64 build, peak memory during
load/undo/export, and a physical device.

## Increment D4: peak memory during save, open, undo/redo and export

No product change; this increment measures. The supported limits stay 16 tracks and 32 scenes.

Measurement gate 1 asks for peak allocation during load, undo/redo, freeze, export and snapshot
handoff, not only the steady engine. `MemoryPeakTests` replaces every C++ `operator new` and
`operator delete` (plain, array, aligned, nothrow) with versions that record each block's size,
so the live heap and its high-water mark are known on every thread, including the load and export
workers. It builds a full project at the compiled limits (every track, 32 scenes, 64 notes in every
MIDI cell, four ten-second stereo 48 kHz audio clips, an 8-bar Song with two clips per track, no
plugins). Then it saves it, makes 20 edits and undoes and redoes them, exports the Song the way
Export WAV does, and opens it into a new engine and over itself. After each open it runs a few
callbacks, as a device would, so the engine hands back what it replaced. `--report` prints the
table below; without it the checks run in CTest.

Checks:
- an export returns its render engine and project copy when it ends;
- opening the same project into a new engine twice holds the same memory;
- once settled, opening over an open project keeps nothing but the replaced decoded audio;
- closing everything returns the memory.

### Measurements

2026-11-21, Release, 16/32, D3 engine (`MemoryPeakTests --report`):

| Step | Live before (MiB) | Peak (MiB) | Peak above before (MiB) | Live after (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Build the full project | 10.6 | 61.5 | 50.9 | 59.3 |
| Save | 59.3 | 67.2 | 7.8 | 59.4 |
| 20 edits, 20 undo, 20 redo | 59.4 | 64.9 | 5.5 | 62.3 |
| Export the 8-bar Song (its own render engine) | 62.3 | 85.9 | 23.6 | 62.3 |
| Open into a new engine and controller | 10.9 | 55.4 | 44.5 | 55.3 |
| Open over itself | 55.3 | 86.7 | 31.4 | 86.0 |
| Open over itself, fourth time | 131.3 | 162.7 | 31.4 | 146.0 |

What this shows:
- **Export roughly doubles a small engine's share for its duration:** a second engine (10.5 MiB)
  and a copy of the project, all returned when it ends.
- **An open costs about the project again at its peak,** because the loaded project is built
  beside the open one before it replaces it.
- **Opening over an open project grows memory every time.** `ProjectController` keeps every
  decoded clip it has loaded for as long as it lives (`retained`), because the engine may still
  point at one; the replaced audio, 14.6 MiB here, is never freed. The growth stops only when
  the 512 MiB `maxSessionAudioBytes` limit refuses the next open with "Close and restart Alien
  Studio to release audio from older sessions". The first two opens also keep about one replaced
  document each (about 16 MiB, mostly 32 KiB note patterns per cell), and then that stops growing.
  This is consistent with the engine's three-slot Song snapshot mailbox, whose old snapshots share
  the document's patterns; it was not traced further.

Not measured: freeze, stems, source replacement, plugins and their state, malloc-based storage
(JUCE `HeapBlock`), the 32/64 build, and anything but this one synthetic project. A next step for
D is to release retained audio of a replaced project once the engine no longer refers to it.

## Increment D5: the audio of a replaced project is freed

No project format change; the supported limits stay 16 tracks and 32 scenes.

D4 found that each open over an open project kept the audio it replaced for the controller's
lifetime. `ProjectController::retained` exists because the engine holds raw pointers into decoded
audio, so nothing it might still read may be freed; with no way to know when it had stopped, the
controller kept everything until `maxSessionAudioBytes` (512 MiB) refused the next open.

What D5 changes:

- **The engine answers a release request.** `ToneEngine::audioReleaseRequest` / `audioReleaseDone`
  are a pair of counters. The callback reads the request before it takes anything from its
  mailboxes, so the block that sees a new number has adopted everything the controller published
  before asking: the new Song snapshot, Session launches and drum pads. After the drum pads are
  brought up to date it stops and clears what could still point into old audio (clip players and
  crossfade tails, every adopted slot's timeline lane, drum voices, the rack player), refreshes
  every slot's drum pads, and stores the same number in `audioReleaseDone`. A playing Song picks
  its lanes up again from the new snapshot. Nothing is allocated or freed on the audio thread.
- **The controller sets replaced audio aside.** After an open (or File > New) has put the new
  project, its history and its published Song in place, every clip that only `retained` still owns
  is moved to a waiting list, and the controller asks the engine to let go. Nothing else can hand
  such a clip out again: not the document, history, the clipboard, the rack caches or a running
  job. A clip with any other owner stays retained, as before; a drum slice source goes with its
  views when they are its only other owners.
- **Freed on the message thread once answered.** `releaseReplacedAudio()` (called by the app's
  timer and at the next open) frees the waiting list once `audioReleaseDone` has reached the
  request, and takes its bytes off the 512 MiB budget. With no device running nothing is freed,
  as before. It also waits while warped audio is being prepared, because the old Song can still be
  the published one then.

Tests (`MemoryPeakTests`):
- with no callback since the open, the replaced audio waits;
- one callback answers the request;
- then the audio is freed (its last `weak_ptr` expires), including a clip that was playing;
- a clip on the clipboard is not freed and still pastes;
- File > New frees the replaced audio too;
- on the full project, once settled, an open over an open project keeps nothing, and the audio
  budget does not shrink.

### Measurements

2026-11-21, Release, 16/32 (`MemoryPeakTests --report`), same project as D4:

| Step | D4: live after (MiB) | D5: live after (MiB) |
| --- | ---: | ---: |
| Open over itself | 86.0 | 71.3 |
| Open over itself, fourth time | 146.0 | 87.2 |
| Growth per open once settled | 14.6 (until 512 MiB refuses) | 0.0 |

Live memory now settles about 32 MiB above a single open: two replaced documents' note patterns,
consistent with the engine's three-slot Song snapshot mailbox, which keeps older snapshots until it
overwrites them. That is bounded and was not traced further (D6 traced and fixed it).

Not done: audio replaced by edits inside one session (removing or replacing a clip's audio) is still
kept, since undo can bring it back; rack sample caches and disk-stream sources are still kept for the
controller's or engine's lifetime; plugins were not part of the test.

## Increment D6: old Song and Channel Rack snapshots freed once the engine leaves them

No project format change; the supported limits stay 16 tracks and 32 scenes.

D5 left live memory about 32 MiB above a single open after repeated opens. Tracing it: the Song
mailbox (`ArrangementMailbox`) is a three-slot buffer, and a slot is only reassigned when a later
publish needs it. After the audio thread moves to a new snapshot, the snapshot it left and the one
before stay in the other two slots until two more publishes overwrite them. Each Song snapshot
shares every Session cell's note pattern with the document it came from, so after an open the two
replaced documents' patterns stayed alive: 16 MiB each on the D4 project (512 cells of 32 KiB). At
32/64 the same project shape would hold four times that. The Channel Rack mailbox had the same
structure.

The old rule was also only safe by delay: the slot holding the snapshot the audio thread had just
left could be overwritten by the second publish after it, with nothing checking that the block that
left it had finished.

What D6 changes:

- **One mailbox for both.** `RetiringMailbox<T>` (new, `src/RetiringMailbox.h`) is the same triple
  buffer with an explicit release rule; `ArrangementMailbox` keeps its validation on top, and
  `rack::SnapshotMailbox` wraps it.
- **The audio thread stamps what it leaves.** Every `consume()` (once per block) counts a visit; when
  it moves to a newer snapshot it stamps the old slot with that visit, before the exchange that
  publishes the move. It does nothing else: no allocation, no free, no lock.
- **Producers release only finished snapshots.** A snapshot may be freed once the audio thread never
  read it, or has begun a later visit than the one that left it: one audio thread runs blocks in
  order, so that block is over. A publish that needs the slot of an unfinished one moves it to a
  waiting list instead of overwriting it. `collect()` frees what is finished; producers call it on
  every publish, and `ProjectController::releaseReplacedAudio()` (the app's timer) calls it for both
  mailboxes, so an idle project lets go too.
- **The current snapshot is never touched.** The audio thread holds `front`; producers touch only the
  back slot and the middle slot when it is not marked new, which only a producer can change.

Tests:
- `ArrangementTests`: a snapshot the reader has just left is kept, also through a `collect()` in the
  same block; after the reader's next block it is freed and nothing else is held; the current
  snapshot is never freed; publishes that reuse a just-left snapshot's slot keep it until the
  reader's next block; superseded snapshots that were never read are freed by the publishing thread.
- `RackModelTests`: left snapshots are freed after the reader's next block; the existing concurrent
  producer/consumer check (40,000 publishes against a reading thread, no torn or empty snapshot) runs
  against the new mailbox.
- `MemoryPeakTests`: every open over an open project, once settled, holds what a single open holds
  (within 1 MiB), not only from the fourth open on.

### Measurements

2026-11-21, Release, 16/32 (`MemoryPeakTests --report`), same project as D4 and D5:

| Step | D5: live after (MiB) | D6: live after (MiB) |
| --- | ---: | ---: |
| Open into a new engine and controller | 55.3 | 55.3 |
| Open over itself | 71.3 | 55.4 |
| Open over itself, fourth time | 87.2 | 55.4 |
| Most kept by one open over an open project, once settled | 16 (first two opens), then 0.0 | 0.1 |

Peaks are unchanged (an open still builds the loaded project beside the open one: 86.7 MiB at its
peak here). The other D4 rows (build, save, undo/redo, export) are within 0.1 MiB of D5.

Not done: the per-slot mixer, synth and sampler objects were measured for this increment and are
small (`SynthEngine` 4.4 KiB, `DrumSampler` 2.3 KiB, `TrackMixer` 20 bytes, `SessionAudioPlayer`
120 bytes per slot, plus what they allocate when prepared), so reserving them with the slot would
save little; they stay fixed. Audio replaced by edits inside one session, rack sample caches,
disk-stream sources, plugins and the 32/64 build are as after D5.

## Increment D7: a refusal when track slots cannot be prepared

No project format change; the supported limits stay 16 tracks and 32 scenes.

Measurement gate 4 asks for an actionable refusal when resources cannot be prepared, never a silent
drop. Since D2 and D3, a track slot's stream voices and note state are allocated when a project first
uses the slot, on the control thread. If memory ran out there, `setActiveTracks` threw after the
document had already changed: the open or added track was half applied, and the exception reached
whatever had called it.

What D7 changes:

- **The engine prepares, then says whether it could.** `ToneEngine::prepareTracks(n)` reserves the
  slots below `n` and answers false if an allocation fails, keeping what it made; a slot counts as
  prepared (`preparedTracks()`) only when all of it exists, so a later call carries on. Nothing is
  freed. `setActiveTracks` uses it and, if it fails, publishes only the slots that exist, so the
  callback never processes a missing one.
- **Prepared before anything is committed.** An open prepares the loaded project's track count right
  after its existing checks, before it stops playback or replaces the document; `addTrack` (and with
  it the Command Engine's CreateTrack) prepares one more slot before it adds the track. On failure the
  status bar says what failed and how many tracks could be prepared ("Open failed: not enough memory
  for 8 tracks"; "Close other programs or open a project with fewer tracks"), and the open project, its
  history and the engine are as they were. Undo and redo only return to track counts that were
  prepared before, and prepared slots are never freed, so they need no check.
- **Export.** The WAV export's render engine reports "Not enough memory to prepare N tracks for the
  export" instead of throwing.
- **An open no longer creates launch mailboxes for unused slots.** Stopping playback before an open
  published a stop to every compiled slot, which created a launch mailbox (96 KiB) for slots the
  project never used, undoing part of D3 on every open. Slots without a mailbox never played, so they
  are now skipped.

Tests (`MemoryPeakTests`, whose allocator can now fail every allocation of one chosen size, here a
slot's note mailbox):
- the engine refuses to grow from 4 to 8 slots, keeps 4 prepared and published, still runs a block,
  and grows once memory is available;
- an open of an 8-track project is refused with the reason in the status bar and leaves the open
  4-track project (same track identity) and the engine's track count unchanged; the same open then
  succeeds;
- adding a ninth track is refused with a reason and the project keeps 8; it then succeeds;
- after an open of a 4-track project, only 4 slots have launch mailboxes (checked to fail without the
  fix).

Not tested: the export refusal (its render engine prepares four slots when it is built, so this
failure injection stops it earlier, in construction); running out of memory anywhere other than a
track slot, such as decoding audio or building the loaded document, which the loader already reports
as a failed open; plugins.

## Increment D8: the Channel Rack's hit queue reserved per channel

No project format change; the supported limits stay 16 tracks and 32 scenes.

Measuring what a new engine (10.9 MiB) still held after D7: the
per-slot mixer, synth, sampler and send objects that D3 listed as "still scaling with the compiled
maximum" are small (about 7 KiB a slot). The largest single object was the Channel Rack player's
pending-hit queue, the nudged, ratcheted and between-step hits waiting for their sample: room for
every one of the 64 rack channels' worst case (1,032 hits each, 72 bytes a hit), 4.5 MiB in every
engine, including each export's render engine, whatever the project used. When it was full, a hit
was silently dropped.

What D8 changes:

- **The queue is reserved a channel at a time.** `RackPlayer` keeps it as one 72.6 KiB chunk per
  reserved channel. `reserveChannels(n)` (control thread) makes the chunks below `n` and then
  publishes the count; it keeps what it made if memory runs out and never frees a chunk while the
  player lives. The callback adopts newly reserved chunks at the start of a block, without
  allocating, and the heap now indexes across chunks (same ordering: earliest first, ties in the
  order they were queued). A player made without a count reserves all 64, as before.
- **Reserved before a snapshot is published.** The engine's rack mailbox reserves the snapshot's
  channels before publishing it, so every caller (the controller, export, tests) gets room first.
  A new engine reserves the default rack's four channels.
- **An open is refused** if its rack channels cannot be reserved, like D7's track slots: "Open
  failed: not enough memory for N Channel Rack channels", with how many could be prepared; nothing
  changes. **An export** reports it instead of rendering. A **rack edit** that adds a channel has
  already happened when the snapshot is published, so it is not undone; the status bar says
  hits on channels past the reserved ones may not play.
- **Dropped hits are counted.** A hit the queue has no room for increments `droppedHits()`
  (`ToneEngine::droppedRackHits()`) rather than vanishing. With the queue reserved for every
  channel the bound is the same as before, so this only happens after a failed reservation. The
  count is not shown in the UI yet.

Tests:
- `RackEngineTests`: two plugin channels with 700 notes each inside one step queue 1,400 hits at
  once, past one channel's room, and every note-on comes out on its exact sample in order (checked to
  fail with a broken heap); with one channel reserved, 1,032 play and the other 368 are counted; after
  reserving a second channel all 1,400 play; the full-rack ratchet stress test drops nothing.
- `MemoryPeakTests`: a new engine reserves the default four channels and holds under 8 MiB;
  publishing a 12-channel rack reserves 12 and the next callback allocates nothing; an open of a
  10-channel project whose queue cannot be reserved is refused with the reason and leaves the open
  project as it was, then succeeds; a rack edit whose channel cannot be reserved still happens, says
  so, and the next edit reserves what was missing.

### Measurements

2026-11-22, Release, 16/32, offline:

| Engine heap (C++ `new`) | D7 | D8 |
| --- | ---: | ---: |
| After construction and `prepare(48 kHz)` (four tracks, four rack channels) | 10.9 MiB | 6.2 MiB |
| With every track slot reserved (`setActiveTracks(16)`) | 23.5 MiB (D3) | 19.2 MiB |

`MemoryPeakTests --report`: live memory with one engine, controller and the full project 59.3 ->
55.1 MiB; the export's peak above its starting point 23.6 -> 19.3 MiB (its render engine is smaller
by the same amount); open and reopen peaks 86.8 -> 82.5 MiB, and reopening still keeps nothing. A
project with all 64 rack channels holds the same queue as before. Callback p95 stayed in the same
range as D3 (16 Session rack occurrences 13.3 % at 64 frames), with zero callback allocations.

Not tested: the export refusal message, a real low-memory condition, the running app, and the 32/64 build locally (CI builds
it). The dropped-hit count is not surfaced in the UI.

## Increment D9: no lost note-offs for the Channel Rack's plugin instruments

No project format change; the supported limits stay 16 tracks and 32 scenes.

Gate 4 asks that notes are never silently dropped. Looking for what was still dropped after D8: the
notes the rack player gives its plugin instruments each block went through two fixed lists, and
both cut off whatever came last when full, note-offs included:

- the rack player's list for the block (1,024 notes, every channel together), and
- each channel's MIDI for its instrument in `PluginRuntime::processRackChannel` (the first 512).

One step can hold far more than that: a piano-roll pattern has up to 1,024 notes, and once the 64 rack
voices are busy every new plugin note steals one, which costs a note-off as well as a note-on. A
dense step therefore cut off note-offs, and the instrument kept those notes held: a stuck note.

What D9 changes:

- **The rack player keeps room for every note-off it owes.** It counts the plugin voices playing.
  Each one still needs a note-off. A plugin hit is played only if the block's list keeps room for its
  note-on, its own note-off and every note-off already owed. Otherwise it is not played and changes
  nothing, not even a choke. It is counted in `droppedHits()` with the hits a full pending queue
  could not keep (D8). With 64 voices the list can always hold all of their note-offs and as many
  new notes (checked at compile time), so stopping or muting always fits.
- **A channel's instrument takes the whole list.** The per-channel cap is now the rack player's list
  size (a compile-time check ties the two together), and each channel's MIDI storage is sized for it,
  plus the 16-channel panic and the 256 live events: 11.5 KiB a channel, down from 16 KiB.

Tests:
- `RackEngineTests`: 1,000 plugin notes on one tick play 512 notes and count 488 they had no room
  for. Every note that starts ends. Stopping in the middle of that block ends every note that was
  playing.
- `RackPluginInstrumentTests`: the same step through the engine reaches a test instrument with every
  note-off. No pitch is left held, the 488 are counted in `droppedRackHits()`, and nothing
  allocates.
- Both tests fail if the player stops keeping room, and the engine test fails if the old 512-note
  channel cap comes back.

Not tested: real VST3 instruments, the running app, and the 32/64 build locally (CI builds it).
Hits left unplayed are still not shown in the UI. The Session tracks' plugin MIDI (64 KiB per track,
not cut off by count) was not part of this increment.

## Increment D10: unplayed Channel Rack hits are reported

No project format change; the supported limits stay 16 tracks and 32 scenes.

D8 and D9 count the Channel Rack hits the engine has no room for, but the count was only readable in
code, so to the user they were still silently dropped (gate 4). D10 shows them:

- **The status bar.** The app's timer calls `ProjectController::reportDroppedRackHits()`. When the
  engine's count has grown it says "N Channel Rack hits were not played", and the tooltip explains
  why and what to do. It reports only hits new since the last report, at most once a second, so a
  long run of them does not flood the status bar.
- **Export and freeze.** Each render has its own engine. If it could not play some hits, the export
  summary ends with "WARNING: N Channel Rack hits were not played; the file is missing them". A
  freeze's status says the frozen audio is missing them.

Tests: `RackPluginInstrumentTests` (the dense block from D9):
- nothing is reported before any hit is dropped;
- the block's 488 unplayed hits are reported once, with the reason, and not repeated;
- 488 more from playing the step again wait until a second has passed and are then reported alone.

Not tested: the status bar in the running app, and the export and freeze warnings (they run on a
worker and are not covered by an automated test). The export warning is not shown when plugins also
failed during the render; that message takes its place.

## Increment D11: track-instrument MIDI bounded, counted and reported

No project format change; the supported limits stay 16 tracks and 32 scenes.

D9 left the MIDI for Song and Session track instruments out of scope. Each track's MIDI storage is
64 KiB, reserved once. Notes were capped at 4,095 a block: past that a note was dropped, and the next
block told every instrument to end its notes, so a dropped note-off could not leave one held. But:

- the dropped notes were not counted, so the user heard notes cut off with no reason given;
- live controller and pitch-bend messages had no cap, so a flood of them could outgrow the storage
  and make the audio thread allocate.

What D11 changes:

- **One limit for every event a track instrument is given.** `PluginRuntime::trackEventsPerBlock`
  (4,096) now covers notes, controllers and pitch bend alike, through one check (`roomFor`).
  - An event past the limit is dropped and counted (`droppedTrackEvents()`, or
    `ToneEngine::droppedTrackMidi()`), and the next block ends every note, as before.
  - A compile-time check makes sure the limit, its panics and some silences fit in the storage,
    so a flood never reallocates.
- **Reported with the Channel Rack's drops (D10).**
  - `ProjectController::reportDroppedRackHits()` is now `reportDroppedNotes()`. It says "N MIDI events
    for track instruments were not sent, and their notes were stopped", with its own tooltip reason,
    beside or instead of the Channel Rack line, still at most once a second.
  - An export's summary and a freeze's status name both kinds.

Tests:
- `PluginRuntimeTests`:
  - a block keeps exactly 4,096 events for a track's instrument;
  - a note-off, a controller and a pitch bend past the limit are counted, not kept;
  - the note whose note-off was dropped ends the next block;
  - the following block has room again;
  - nothing allocates.
- `RackPluginInstrumentTests`: five events past the limit are reported with the track reason and
  without the Channel Rack's; both kinds read together in one line.

Not tested: a Song arrangement that actually reaches 4,096 notes in one block for one track (it needs
stacked regions), the status bar in the running app, and the export and freeze warnings. The limit
is per track and per block, so it is reached only by very dense stacked material or a flood of live
MIDI.

## Increment D12: streamed audio that drops out or cannot be read is reported

No project format change; the supported limits stay 16 tracks and 32 scenes.

Long audio clips stream from disk. When a stream was not buffered in time, the player output silence
and counted an underrun for each silent sample. When a stream could not be read, the clip stopped and
a flag was set, but the next clip cleared it. Only export checked either one: it refuses a render that
hit one. During live playback, Session or Song, the user heard a gap or a missing clip with no reason
given (gate 4).

What D12 changes:

- **Failures are counted.** `SessionAudioPlayer::failureCount()` counts streams that could not be
  read, whether at launch or while playing. Queueing the next clip does not reset it. A stream that
  fails while playing now counts once as a failure; before, every silent sample after it counted as an
  underrun. `ToneEngine::sessionStreamFailures(track)` sums a track's player and crossfade-tail player.
  The audio thread only adds to an atomic.
- **Reported by track.** `ProjectController::reportStreamProblems()`, called by the app's timer, says
  "Streamed audio dropped out on track 1: the disk could not keep up" and/or "Streamed audio on
  track 3 could not be read, so it stopped". Each comes with its own tooltip (disk too slow; file
  moved, deleted or disconnected, and Search for missing audio). It lists only what is new since the
  last report, at most once a second. The timer shows D10/D11's dropped-note message first, then this
  one on a later tick, so neither hides the other.

Tests:
- `StreamedSessionPlaybackTests`:
  - a failed launch is one failure, not an underrun, and the count survives the next queue;
  - a stream that fails while playing is one failure, and its silence is not an underrun;
  - a stream whose disk stalls underruns without failing.
- `RackPluginInstrumentTests`: a real engine plays a Song with a stalled stream on track 1 and an
  unreadable one on track 3.
  - The engine counts the underruns and the single failure on the right tracks.
  - The status bar names both tracks with both reasons; more of the same waits a second, then only
    what is new is reported.

Not covered:
- a Song region whose stream arrives late and starts part-way in. That is by design, keeps time, and
  is not reported; reporting it would be noisy, since playing from the middle of a region always waits
  briefly.
- the running app and a real slow disk.

## Increment D13: plugin latency that puts tracks out of step is reported

No project format change; the supported limits stay 16 tracks and 32 scenes.

D1 lines every chain up behind the slowest one, with delay lines sized to the latency the plugins
report, up to `PluginRuntime::maxDelay` (96,000 samples). A plugin set whose latency is already past
that is refused when it loads. Two cases while playing were silent:

- **A plugin's latency grows.** Until the host's next poll grows the delay lines, the tracks are out
  of step and the callback counts a shortfall block (D1). Only an export looked at that count.
- **A chain's latency grows past 96,000 samples.** Each chain's latency is clamped to the limit, so
  the others wait 96,000 samples and that chain plays late by the rest. Nothing was counted at all.

What D13 changes:

- `PluginRuntime::reportedLatency(rack)` gives a chain's latency (a track, the master, a bus, a
  return, or a Channel Rack instrument) before the clamp. Bypassed, failed and frozen plugins add
  none, as for alignment.
- `PluginHost::latencyProblems()`, called by `poll()` on the app's timer when `onLatencyProblem` is
  set:
  - says once when new shortfall blocks were counted: "A plugin's latency grew while playing, so the
    tracks were out of step for a moment until they were lined up again";
  - names each chain past the limit once: "The plugins on track 2 report 120,000 samples of latency,
    more than the 96,000 that can be lined up, so track 2 plays 24,000 samples late. Bypass or remove
    a high-latency plugin there." It is named again only after it has come back within the limit.
- The app shows these in the status bar as warnings, not plugin failures: the plugins keep running.
- An export collects the same report after its render and adds a chain past the limit to its summary
  as a warning. (A shortfall during an export already fails it, as in D1.)

Nothing changes on the audio thread.

Tests (`PluginRuntimeTests`):
- a shortfall while playing is reported once, and a poll with nothing new reports nothing;
- a track whose two plugins report 60,000 samples each is named with its latency and how late it plays;
  the in-limit track and Channel Rack instrument are not;
- polling again does not repeat it; a Channel Rack instrument that then reports 100,000 samples is
  named on its own;
- bypassing one plugin brings the track back within the limit; past it again, it is named again;
  a frozen track is not named;
- without the callback, `poll()` leaves the report for an export to collect.

Mutation checks: with the "named once" guard removed, or the shortfall report disabled, the test fails.

Not covered: the status bar in the running app, the export summary and a real latent third-party
plugin (the test plugin reports the latency it is told to). Freezing a track does not collect the
report (D18 adds that).

## Increment D14: a replaced project's Channel Rack samples are freed

No project format change; the supported limits stay 16 tracks and 32 scenes.

D5 frees the audio of a project that an open or File > New replaces, once the engine has let go of
it. It left out the Channel Rack's caches, which were kept for the controller's lifetime:

- `rackAudio`, the decoded rack sample files by path;
- `rackSliceViews` and `retainedSliceViews`, the views sliced channels play;
- `rackKeyMaps`, the key maps channels with key zones play.

So each project opened with different rack samples left the last one's decoded audio in memory and
counted against the 512 MiB audio budget, until an open was refused. Reopening the same project freed
the whole file, but a sliced or key-zoned sample stayed, held by its old views and key map.

What D14 changes:

- **`forgetUnusedRackAudio()`.** After an open or File > New has published the new project and reset
  its history, the caches keep only what its channels use. A cache entry made from a file that is
  no longer the current decode goes too. Everything it lets go of is handed to the D5 release
  rather than freed on the spot, because the engine may still hold raw pointers to it.
- **The D5 release follows chains.** A clip is set aside when every reference to it comes from:
  - `retained`;
  - what the caches let go of;
  - another clip being set aside: a slice view's source, or a key map's zones.

  So a key map, its slice views and the file under them go together. Before, only a slice view's
  direct source was followed. Anything something else still owns, such as a copied cell, stays.

Nothing changes on the audio thread; freeing still waits for the engine's answer, as in D5.

Tests (`MemoryPeakTests`):
- A project whose 10-second rack sample is sliced into channels and put on key zones is reopened over
  itself three times. The audio budget counts the sample once, and live memory stays within 1 MiB.
- Opening a different project with a 1-second sample:
  - before a callback, the old sample waits;
  - after it, the budget counts only the new sample;
  - live memory falls by about the old sample's size;
  - the new sample stays loaded.
- File > New returns the budget to empty.

Mutation checks: the test fails when an open doesn't clear the caches, and when it clears them but
keeps the old slice views.

Not covered: the running app, and rack samples loaded or relinked inside one session and then
replaced by an edit. Undo can bring those back, so they stay until the next open, as Session audio
does in D5.

## Increment D15: Channel Rack instrument delay lines sized to latency

No project format change; the supported limits stay 16 tracks and 32 scenes.

Each Channel Rack plugin instrument waits for the slowest one, so all of them come out together. D1
left these per-instrument lines at full length: every loaded instrument held 96,001 stereo samples
(0.73 MiB), whatever latency it reported. That is up to 47 MiB with 64 instruments.

What D15 changes (`PluginRuntime`):

- **Sized like D1's stages.** Each loaded instrument's line is as long as the slowest rack
  instrument's latency, rounded up to whole 1,024-sample granules. With no latent instrument it holds
  one sample. A channel that never had an instrument holds no line.
- **Allocated off the callback.** `reserveAlignment` allocates a channel's storage and publishes it;
  the callback takes it at its next `begin()` and hands back the old one. That's the same handoff as
  D1, with no allocation, lock or free in the callback. It grows when an instrument is prepared and
  on the host's poll, and fits exactly on a restore, which frees the line of a channel whose
  instrument went.
- **New and empty for each instrument.** Preparing an instrument on a channel always publishes a new
  line, so nothing the previous instrument left in it plays.
- **Shortfalls.** A line too short for the delay asked of it (a latency that grew since the last
  poll) goes through the same `align()` as the other stages. The source stage is sized from the same
  latency and taken up in the same block, so it is short in exactly those blocks and already counts
  them in `alignmentShortfalls()`: an export still fails, as before.

`alignmentBytes()` includes these lines; `channelAlignmentCapacity(c)` reports one.

Tests (`PluginRuntimeTests`):
- two rack instruments without latency hold one-sample lines: under 4 KiB, where it was 1.5 MiB;
- the latency grows to 3,000 samples while playing:
  - the too-short line is counted once per block, with no callback allocation;
  - the poll grows both loaded lines to 3,072 and the empty channel gets none;
  - the next block takes them up without allocating;
  - a note on the faster instrument then comes out exactly 3,000 samples late;
- a newly loaded instrument's line plays nothing the old one left in it;
- a restore shrinks the lines back, including the line of a channel whose instrument was removed.

Mutation checks: the test fails when a new instrument keeps the old line, and when the lines are
full length again. Counting the channel line itself changes no count the test can see, as the
source stage counts the same blocks.

Not covered: the running app, a real latent third-party instrument, and an engine-wide heap figure
with rack instruments loaded (the capacity report loads none).

## Increment D16: audio no edit can bring back is freed during a session

No project format change; the supported limits stay 16 tracks and 32 scenes.

D5 frees audio only when an open or File > New replaces the project. Within a session, every clip
the controller had loaded stayed in `retained` and counted against the 512 MiB audio budget for as
long as the project was open, even once nothing could hand it out again. For example:

- an imported clip whose import was undone and whose redo step was then replaced by another edit;
- a deleted clip whose last undo step fell off the 200-step history;
- a freeze render after Unfreeze, once no undo step holds the frozen state;
- a recording take that was undone and recorded over;
- a Channel Rack sample's previous decode, after the file it names was decoded again in place (no
  undo step).

A long session could reach the budget and be refused more audio, though the project used far less.

What D16 changes:

- **`ProjectController::releaseUnreachableAudio()`**, called by the app's timer. At most once a
  second, and only while the transport is stopped and nothing is loading, recording or warping, it
  runs D5/D14's `setAsideRetainedAudio()` on its own. A clip is set aside when nothing but
  `retained` reaches it:
  - not the document;
  - not an undo or redo step;
  - not the clipboard or the Channel Rack caches;
  - not a Song snapshot or a running job.
- **A lighter engine release.** D5's release stops everything, so the callback answers it right
  after an open. That would cut off a performance if it ran after an ordinary edit. D16 adds
  `editedAudioRequest`/`editedAudioDone`:
  - The callback answers only in a block where the transport is stopped. While playing, the request
    waits.
  - It stops what could still point into audio: clip players and crossfade tails (already silent
    once stopped), Song lanes, drum-pad voices and Channel Rack sample voices.
  - It leaves synth notes played live alone.
  - The timer call after the answer frees the clips and gives their bytes back to the budget.

Nothing on the audio thread allocates, locks or frees. The callback only stops voices and stores a
number, as in D5.

Tests (`MemoryPeakTests`, a 2-second stereo clip at 48 kHz):
- audio in the document, or reachable by undo or redo, is not set aside;
- after an undone import's redo step is replaced, the clip is set aside:
  - not while playing;
  - not freed until the engine answers;
  - the engine doesn't answer while playing;
  - once a stopped callback answers, it is freed, live heap falls by its size and the budget is
    back where it started;
- a clip on the clipboard stays and still pastes;
- a deleted clip stays while its undo step exists, and is freed after 201 more edits push it off
  the history;
- a rack sample in use stays; once it is decoded again in place, the old decode is freed and the
  new one is what the channel plays.

Mutation checks: the test fails when the callback answers while playing, when no sweep runs, and
when clips are freed without waiting for the engine's answer. Stopping the Channel Rack player in
the lighter release isn't checked by a test, since nothing rings in it.

Not covered:
- **Freeze renders and recording takes** use the same `retained` list and the same sweep, but
  aren't exercised by this test.
- **A Channel Rack channel given a different file** keeps the old file's decode as before (D14).
  The cache is keyed by path, and undo can bring the old file back. (D17 frees it once no undo step
  can.)
- **The running app.** Freeing happens on the app's timer while stopped. A drum pad still ringing at
  that moment, or a Channel Rack sample auditioned from the keyboard, is cut off.

## Increment D17: a Channel Rack file switched away from is freed

No project format change; the supported limits stay 16 tracks and 32 scenes.

After D16, a Channel Rack channel given a different file (Load sample, a drop, a relink to another
file) still kept the old file's decode in `rackAudio` until the next open or File > New (D14). The
cache is keyed by path, and an undo can name the old path again, so it could not simply be dropped.
In a long session, each file auditioned on a channel stayed decoded and counted against the 512 MiB
audio budget.

What D17 changes:

- **`DocumentHistory::forEachDocument()`** visits every state an undo or redo can return to.
- **The D16 sweep forgets rack files no state names.** Before `setAsideRetainedAudio()`, the
  stopped-transport sweep collects the file of every channel in every history state and calls D14's
  `forgetUnusedRackAudio()` with that set, on top of the open document's files. A file no state
  names leaves the cache, with the slice views and key maps made from it, and D14's chain rule sets
  them aside together.
- Freeing still waits for D16's engine answer, which only comes in a stopped block.

An undo can therefore never land on a channel whose file has been forgotten: while any state names
the file it stays decoded, so the channel is never shown as offline because of the sweep.

Nothing changes on the audio thread.

Tests (`MemoryPeakTests`, after the D16 checks, a 1-second rack sample replaced by a 2-second one on
the same channel):
- both files are counted while the switch can be undone, and a sweep frees neither;
- undo plays the first file again (not offline), and a sweep then keeps it; redo plays the second;
- after 201 more edits push the switch off the history, a sweep frees the first file's bytes:
  - live memory falls by about its size;
  - the budget gets them back;
  - the channel still plays the second file, and the clipboard clip stays;
- a second sweep frees nothing more.

Not covered:
- the running app;
- a sliced or key-zoned channel switched to another file (the views and maps go by D14's rule,
  which `rackCacheTests` covers for an open, not for this sweep).

## Increment D18: a freeze collects its render's plugin report

No project format change; the supported limits stay 16 tracks and 32 scenes.

A freeze renders the track offline with its own engine and plugin host, then installs the result.
Unlike an export, it never asked that host what happened during the render:

- **A plugin that failed while rendering** was switched off (its fault contained, as always), so
  the frozen audio was missing its sound. The freeze still replaced the live track, said "Track
  frozen" and gave no sign of it.
- **A chain past the latency limit** (D13) plays late in the frozen audio too, with no warning.

What D18 changes:

- **`PluginHost::renderReport()`** collects, once the render has finished, the failures `poll()`
  reports and the D13 latency report. It leaves the host's own callbacks in place and doesn't call
  them; each failure and each chain past the limit is reported once, as by `poll()`.
- **`PluginHost::freezeOutcome(report, droppedNotes)`** decides what the freeze does:
  - a plugin failed: the freeze stops, the rendered file is deleted, the track is left as it was,
    and the status bar says "Freeze stopped: a plugin failed while the track was rendering ...
    Original track unchanged." with the plugin's report;
  - otherwise the track freezes, and the status bar adds a warning for notes the render had no
    room for (D10, D11, as before) and for a chain past the latency limit, on one line.
- The app's freeze job uses both on the message thread, before running the `FreezeTrack` command.

Nothing changes on the audio thread.

Tests:
- `PluginFaultTests`:
  - a healthy render reports nothing and freezes with no warning, or with the dropped-note warning;
  - a plugin that crashes during the render is named with its slot, and the freeze stops with
    "Original track unchanged";
  - the host's own problem callback stays set and is not called;
  - a second report doesn't repeat the failure.
- `PluginRuntimeTests`: a host whose chains are past the latency limit gives a render report that
  names track 2 and the Channel Rack instrument. The freeze installs with the warning on one line,
  the host's latency callback is untouched, and the next report doesn't repeat it.

Not covered: the running app's freeze job itself (it calls these two functions) and a real
third-party plugin failing during a freeze.
