# T1-04: universal tracks and capacity foundation

Status: first increment; track architecture proposal and reproducible capacity
report. Universal track migration and higher supported limits are not implemented.
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
