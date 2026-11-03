# Command Engine

Roadmap workstream T1-02, checklist ID CMD-01. This note covers structural tracks, volume/pan, mute/solo, Session clips, Session scenes and what later
slices need. See [T1_02_COMPLETION.md](T1_02_COMPLETION.md) for the completion audit; CMD-01 remains partial.

## Why

Every user operation should become a named, undoable command with typed arguments. One command
layer then serves undo/redo, macros, key bindings, MIDI controller scripts, user scripts and,
much later, the Workflow Assistant, which would plan and preview through the same commands
instead of driving the UI.

## Design

Code: `src/CommandEngine.h`, tests: `tests/CommandEngineTests.cpp`.

- **Command.** A `std::variant` of small argument structs (`CreateTrack`, `DeleteTrack`,
  `MoveTrack`, `RenameTrack`, `ColorTrack`, `SetTrackVolume`, `SetTrackPan`, `SetTrackMute`, `SetTrackSolo`, `CreateClip`, `DuplicateClip`, `MakeClipUnique`). `name()` gives the stable name (for example
  `"RenameTrack"`) and `label()` the undo-menu text. `arguments()` and `parse()` convert a command
  to and from its name plus a JSON-style object (`{"track": 1, "name": "Bass"}`), so scripts and
  macros can store and replay commands. `parse()` refuses unknown names, missing or mistyped
  arguments and unknown argument names. Tracks are numbered from zero.
- **Refusal and document edit.** `refusal(doc, command)` says why a command cannot be applied
  (no such track, last track, track limit, blank name, colour outside the palette).
  `applyTo(doc, command)` makes the same change to a bare `ProjectDocument` that the controller's
  operation makes, with no engine involved.
- **Execute.** `CommandEngine::execute(command)` is a batch of one.
- **Batch.** `CommandEngine::batch(commands, label)` first previews the whole batch. If any
  command would be refused, it returns that command's index and reason and touches nothing: not
  the document, the engine, playback or the history (the redo stack survives). Otherwise it runs
  each command through the existing `ProjectController` operation (`addTrack`, `deleteTrack`,
  `moveTrack`, `renameTrack`, `setTrackColour`, `setTrackVolume`, `setTrackPan`, `setTrackMute`, `setTrackSolo`) and calls `commit(label)` once, which makes the
  batch one `DocumentHistory` step. If the controller still refuses a command part way (it should
  not, since the preview accepted it), `ProjectController::revertTo()` restores the document
  captured before the batch and nothing is committed.
- **Preview.** `CommandEngine::preview(commands)` applies the commands to a copy of the document
  and returns the resulting document plus one line per command ("Delete track 1 \"Track 1\"").
  The project, engine and history are never touched. Tests check that preview predicts
  `execute` exactly (`sameContent`, selection included) for every track command.
- **Undo/redo** stay the snapshot history in `DocumentHistory`; `CommandEngine::undo()`/`redo()`
  call the controller. There is no second undo stack.
- A command that changes nothing (rename to the same name, move to the same place) succeeds with
  `changed == false` and records no undo step, as the snapshot history already did.
- `MoveTrack {track, to}` is a real move (the tracks in between shift by one), done as adjacent
  swaps with the controller's `moveTrack(t, +-1)` and recorded as one undo step.

Controller additions are `setTrackColour()` with `trackColourCount`, `revertTo()`,
and the validated, uncommitted setters `setTrackVolume()` / `setTrackPan()` / `setTrackMute()` / `setTrackSolo()`.
The command engine records the final history step. No project format change.

### Track volume and pan

`SetTrackVolume {track, gain}` uses linear gain from 0 to 2: 0 silences the fader,
1 is unity and 2 is twice unity. `SetTrackPan {track, pan}` uses -1 for left,
0 for centre and 1 for right. Tracks are zero-based. Their argument objects are
`{"track": 0, "gain": 0.5}` and `{"track": 0, "pan": -0.25}` respectively.
Numbers may be integers or decimals; strings, booleans, null, missing/unknown
arguments and non-finite values are refused. Ranges are checked before narrowing
to the float values stored by the mixer.

Preview changes only its copied document. Execute writes the document and only
the addressed engine atomic through the controller, preserving the other track
settings. The caller commits once per batch. A repeated value (including a double
that rounds to the same stored float) adds no history entry. A later invalid
command prevents every earlier write; undo and redo restore document and controls.
These commands can follow CreateTrack or MoveTrack in the same batch.

CommandEngineTests covers these contracts, argument round trips and boundary
values. Build/test results are recorded in the feature PR. Real hardware listening
and automation interaction remain manual checks.

### Track mute and solo

`SetTrackMute {track, muted}` and `SetTrackSolo {track, soloed}` set explicit states.
JSON arguments are `{"track": 0, "muted": true}` and `{"track": 1, "soloed": false}`.
The state must be a boolean: numeric 0/1, strings, null and missing/unknown arguments
are refused. Solo is additive; it does not clear another track's solo state or change mute.
Preview, batch refusal, no-op detection and undo/redo follow the volume/pan contracts.
Only the addressed document field and live engine control change. Session's left-click
mute light, right-click solo light and S button now execute these named commands. A
successful command sends a refresh-only notification to the mixer, without a second
legacy commit; busy refusal sends no edit notification. Session volume/pan gestures also use named commands (see below).
Master, bus, return and send controls retain their existing direct editing paths.

CommandEngineTests covers true/false JSON round trips, strict types, side-effect-free
preview, live controls, preserved mix/other solo states, busy/invalid-target refusal,
redo preservation and sequential create/reorder batches. Manual listening remains pending.
SessionViewTests drives the actual controls and checks one-step undo/redo, dirty state,
additive solo, busy refusal, refresh notifications and unchanged selection/queued launches.

### Session volume and pan gestures

The Session strip's volume/pan drag, Shift fine adjustment, mouse wheel and
double-click reset execute `SetTrackVolume` / `SetTrackPan`. Each accepted change
updates the document, the addressed live atomic, dirty state and history through
the same command path. The host receives only a refresh notification; Session
controls no longer invoke the Mixer's legacy edit/commit callback. Other mix
controls, selection and queued clip launches retain their values.

`execute` and `batch` accept an optional history grouping key. Ordinary keys use
the existing 1.5-second window, used for repeated wheel edits on the same control.
A drag supplies a unique key with `continuous=true`; adjacent updates merge even
after a pause. Releasing and starting another drag creates a new key. A reset
has no grouping key, so it is a separate command. Unrelated intervening edits
and undo break adjacency: grouping never absorbs someone else's command.

Busy commands and unchanged values write nothing and preserve redo. A drag that
becomes busy or whose row is replaced/reordered stops accepting queued events;
changes already accepted remain undoable. Gestures preserve the existing ranges,
whole-percent rounding, centre/unity snapping and fine movement. No saved format
or engine-capacity change. Master, bus, return and send controls remain later command slices.

CommandEngineTests covers grouping, refusal and interleaved commands.
SessionViewTests drives real handlers, including a pause beyond the timed window,
separate drags, wheel/reset, undo/redo, no-ops, busy transitions and stale rows.
Exact build/test results belong in the PR; listening remains a manual check.

### Known limits of these slices

- The Session track menu, rename box and add-track button use named commands. Commands own
  the undo commit; the view refreshes structure/title without invoking the legacy edit-commit callback.
  Scene edits now use commands too. Master/bus/return/send controls retain their existing controller paths. SessionViewTests covers
  menu add/move/delete, one-step undo/redo, busy refusal and host refresh notifications.
- `applyTo()` repeats the document half of the controller's track operations so a preview can run
  without an engine. The preview-equals-execute tests guard against drift. A later slice could
  move those document edits into one shared place that both use.
- Recording state, frozen tracks and the live drum route are handled exactly as the controller
  handles them today (deleting a track during a take abandons the take).

## Future commands

| Command | Maps onto today | Needs new work |
| --- | --- | --- |
| CreateTrack, DeleteTrack, MoveTrack, RenameTrack, ColorTrack | Done in this slice | - |
| CreateClip | Implemented for an empty Session MIDI cell | Song placement creation and asynchronous audio import remain |
| DuplicateClip | Implemented for independent MIDI/audio/offline/rack Session copies with explicit source/destination cells | Song placement command duplication remains |
| MakeClipUnique | Implemented for MIDI/audio/offline/rack Session content | Song placement command wrappers remain |
| SetTrackVolume, SetTrackPan | Implemented: command setters write document then the addressed engine control | Session and Mixer track UI adopted; other parameter targets remain |
| SetParameter (other targets) | Mixer edits via `trackSettingsEdited`, `returnSettingsEdited`, `masterGainEdited` with coalesced `commit` | Remaining targets need command wrappers |
| AutomateParameter | `editTimeline` on `parameterLanes`, plugin automation targets | Command wrapper over lane edits |
| CreateSend | Up to eight dynamic returns with per-track send level and pre/post (`TrackSettings`) | Command wrappers for return creation and send settings |
| CreateBus, RouteTrack, CreateGroup, AddToGroup | Existing group buses and track output routing (T1-05) | Pure Preview and noncommitting command/controller adapters; UI adoption |
| InsertDevice, MoveDevice, RemoveDevice | `setPluginRacks` records rack edits after `PluginHost` changes the live instances | Plugin instances are runtime state; needs a host-side command path |
| SetTempo | `setBpm` (whole BPM, unfreezes frozen tracks) | Tempo map (T1-01, TEMPO-01) |
| SetChord | Piano roll chord stamping on clip notes only | Chord track |
| SetArrangementSection | Timeline markers (`addMarker`, `moveMarker`, `removeMarker`) | Arrangement sections |

### Mixer track controls

Mixer track volume, pan, mute and solo execute the same named commands as Session.
Each slider drag uses a unique continuous history key, so even a paused drag is
one Undo step; a later drag or discrete edit starts another step. The command
updates document and engine together. The host notification only refreshes the
other view. Busy projects refuse these edits, and queued events from a strip
whose track identity no longer matches its row are ignored. Instrument, send,
master, return and bus controls retain their existing edit paths.

## Session clip creation and duplication commands

Use the Session **Copy / link** menu to create an empty MIDI clip, duplicate an existing MIDI/audio/rack clip
into the next free cell on its track (wrapping through existing scenes), or make
linked content independent. A full track needs a free cell/new scene before
duplicating; these actions never overwrite occupied cells or silently add scenes.
Allocated empty MIDI clips are displayed as clips and survive save/reopen and
clipboard copying through the existing saved pattern-identity field. No project
format version changes. Ordinary unallocated empty cells remain empty.

| Name | Arguments (zero-based) | Behavior |
| --- | --- | --- |
| CreateClip | `track`, `scene` | Allocate a default-length, empty MIDI clip in an unused cell |
| DuplicateClip | `track`, `scene`, `toTrack`, `toScene` | Independent copy of the current MIDI/audio/rack source into an unused cell |
| MakeClipUnique | `track`, `scene` | Detach shared MIDI/audio content (already-independent is a no-op), or copy a rack source |

Preview and execution use pure document operations in MidiClipCommands.h (creation)
and SessionClipCommands.h (all-type duplication).
Previews do not consume live identities. Create/duplicate select the destination;
Undo restores the selection before the batch, while Redo selects the final
destination. The history step has optional separate Undo focus, leaving all
existing commands' focus behavior intact. Ordinary independence keeps selection and
playing voices unchanged. The host refreshes without making another commit.

Busy projects, recording/count-in, frozen destinations, invalid addresses and
occupied destinations are refused. DuplicateClip and MakeClipUnique support audio/offline-audio
and rack cells; duplication is described below and independence uses the clipboard reducer. A stale menu checks captured track/scene/clip identities before
acting. CommandEngineTests and SessionViewTests cover Preview/live agreement,
sequential batches, atomic refusal, no-op/redo preservation, link independence,
empty-clip persistence, focus and actual menu actions. Audio imports, Song
placement command wrappers and remaining rack/channel edits remain later work.

## Session scene commands

`CreateScene {}`, `DuplicateScene {scene}`, `MoveScene {scene, to}`,
`RenameScene {scene, name}` and `DeleteScene {scene}` use zero-based addresses.
Move is an absolute destination. Duplication inserts an independent copy after
its source, including audio/MIDI clips and independent Channel Rack sources.
Scene limits, last-scene protection and frozen original references are checked
before execution. Recording/count-in and busy state refuse the entire batch.

Session's add-scene button, scene menu and rename prompt use the commands;
notifications refresh the host without a second commit. Deferred menus reject
replaced/reordered rows; a pending rename follows its captured scene identity.
Previews allocate runtime object IDs only in a local scoped stream, including
existing track-creation previews. They no longer consume global UI identities.

The document reducer is in SceneCommands.h; live execution retains existing
controller playback/structure handling. The completion audit records the
remaining command families and UI migrations. No project-format change.

## Session clip deletion

`DeleteClip {track, scene}` uses zero-based Session cell addresses. The existing
**Clear clip** button invokes this command for MIDI, online/offline audio, rack
assignments and explicitly empty MIDI clips. It removes the addressed cell, not
the shared content of linked cells or the rack pattern. Existing cell-based Song
placements become silent; independent placement/rack sources are preserved.
Selection and unrelated queued launches/transport remain unchanged. The addressed
live ordinary cell receives an empty launch through the existing mailbox. Clearing
a rack assignment does not stop the shared rack source already playing.

Preview and execution share SessionClipCommands.h; Preview has no engine/history
side effects. Batch validates every command before live effects and records one
Undo/Redo step. Undo restores clip identities, links and immutable media. An absent
cell succeeds without a history entry or discarding redo. Invalid addresses,
frozen tracks, busy state and recording/count-in refuse live deletion. No project
format change. The host reloads its piano roll after success without emitting a
second edit; the button also enables clearing allocated clips with zero notes.

## Clipboard paste and all-type independence

`PasteClip {track, scene}` and `PasteLinkedClip {track, scene}` replace a Session
cell. They use the clipboard captured at the start of each Preview/Batch call;
serialized arguments contain the destination address, not the clipboard or media.
Replaying a parsed paste uses the current clipboard. Copy is a non-undoable
clipboard action; it is not part of a command batch. Preview/live comparison
requires unchanged clipboard/document input between calls. Pure `refusal` and
`applyTo` accept an explicit Clipboard context; missing context refuses paste.

PasteClip uses ordinary MIDI/audio content as it was when copied, even if the
original cell was edited or removed. PasteLinkedClip resolves the remembered
source identity in the sequential document and uses its current shared content;
a deleted/pruned source refuses. Explicit empty clips and offline audio are kept.
Rack paste retains existing behavior: it copies or links the current shared rack
source by identity, rather than storing a rack-pattern snapshot in the clipboard.
MakeClipUnique supports MIDI, audio and rack cells. Independent ordinary content
is a no-op; rack independence creates a new rack source. Rack capacity and Song
cell-binding failures roll back the pure reducer, so Preview refuses before any
live publication. Paste preserves selection and unrelated playback requests.

All three menu actions use commands and refresh-only host callbacks. Stale
track/scene/cell identities refuse; busy state, recording/count-in and frozen
destinations refuse. No project format changes. New audio imports and remaining rack/channel edit wrappers remain follow-ups.

## Session clip movement

`MoveClip {track, scene, toTrack, toScene}` moves a Session cell into an empty
cell and selects its destination. The Copy / link menu offers **Move clip to**,
grouped by destination track and scene. MIDI (including explicitly empty clips),
audio/offline audio and rack assignments retain the exact clip identity, linked
content and media. The shared rack pattern is not copied or deleted. No project
format change. Moves use the same pure reducer for Preview and live execution.
Batch applies sequential moves as one Undo/Redo step, including before/after
selection. A same-cell move is a no-op preserving redo.

Occupied or invalid destinations, empty sources, frozen source/destination tracks,
busy state and recording/count-in refuse. Moving does not rewrite Song: source
or destination cell references in Song regions, takes or frozen originals refuse;
make those placements independent first. Independent placement clips and rack
placements keep their sources. The popup validates both captured source and
destination track/scene/cell identities before applying its command.

Moving never launches the destination automatically. An already-requested ordinary
source is republished empty; an already-requested ordinary destination receives
its moved content. Another scene's queued request or pending stop is preserved.
Shared rack playback continues by source identity; moving an assignment does not
transfer running rack voices between tracks. A requested ordinary destination
receives a stop if its new assignment is rack content. Native listening and held
notes still require manual Windows acceptance separate from automated tests.

## All-type DuplicateClip

`DuplicateClip {track, scene, toTrack, toScene}` keeps its stable arguments and now
supports audio/offline audio and rack clips as well as MIDI. **Duplicate clip to
free cell** in Copy / link chooses the next eligible empty cell on the same track,
wrapping through existing scenes. It does not add scenes, overwrite cells, launch
the copy or change the clipboard. The destination is selected. Undo restores the
prior selection; Redo selects the final destination of a batch.

Ordinary audio/MIDI copies get independent library clip/pattern identities and
preserve clip metadata, while immutable decoded/streamed media is shared. Rack
copies allocate a new independent pattern/source while retaining current rack
editor selection and channel routing. The pure reducer is shared by Preview and
execution. Sequential batches check rack capacity before live effects, and
invalid later commands refuse the whole batch. Rack destinations referenced by
cell-based Song regions/takes/frozen originals refuse (as rack assignment does).
Occupied/invalid/frozen destinations, missing sources, busy and recording/count-in
states refuse. Reading a frozen source into another unfrozen track is allowed.
No project format change. Another scene's queued request and pending stops remain
untouched; an already-requested destination receives its new ordinary content or
an ordinary stop for a rack assignment. Rack playback does not start automatically.

CommandEngineTests and SessionViewTests cover pure/live identity agreement,
clipboard isolation, media sharing, offline copies, rack capacity, selection and
history, stale source menus, guards and actual audio/rack duplicate menu actions.
Manual Windows listening and streamed-media acceptance remain separate from CI.

## Rack pattern creation and Session assignment

`CreateRackPattern {}` appends and selects an empty Channel Rack pattern.
The Channel Rack New pattern button/menu invokes this named command. It keeps
Session selection and channel/plugin routing unchanged. It does not instantiate
plugins, issue a rack-launch request or add a Session cell.

`AssignRackClip {track, scene, clip}` binds an existing rack source to a Session
cell, replacing its old cell reference without rewriting other linked cells.
The Copy / link assignment submenu invokes the command with a stable source ID,
so a missing source refuses instead of resolving another pattern by slot. JSON
`clip` is a positive canonical decimal **string**, including for small identities,
to preserve all 64 bits in scripts. Numeric, signed, padded and overflowing values
refuse parsing. Source existence is validated against the sequential document.

Both commands share RackCommands.h for pure Preview and live execution. Batch
validates all commands before live effects and records one Undo/Redo step.
Preview neither consumes live IDs nor creates plugin instances. Assignment to
the same source is a no-op preserving redo. Assign preserves Session selection
and clipboard; it stops only the addressed ordinary track as the existing UI
assignment did. Shared rack voices are not launched or transferred automatically.
The source and destination popup identities are revalidated before assignment.

Busy projects and recording/count-in refuse both commands. Creation refuses at
the rack pattern limit or when a rack channel routes to a frozen track, retaining
the existing editRack protection. Assignment refuses frozen/missing destinations,
missing source IDs and cell-based Song/take/frozen-original references. Undo
restores the pattern bank, editor pattern selection and replaced cell content.
No project format change. Clone/rename/delete/step/channel command migration and
asynchronous audio import plans remain unfinished; T1-02 is still partial.

## Remove Session clip audio

`RemoveClipAudio {track, scene}` migrates the existing Session **Remove audio**
button to the Command Engine. Preview uses the same pure document edit as live
execution; sequential Batch is one Undo/Redo step and missing audio is a no-op
preserving redo. Online and offline audio use the same path without decoding or
file access. Host notifications refresh the piano roll/UI without another commit.

Existing linked behavior is preserved: remaining MIDI notes/custom loop content
edits the shared clip pattern, removing audio from its links while retaining notes
and identities. A default empty remainder instead clears only the addressed cell;
other linked audio cells keep their content. Undo restores exact media references
and sharing. Invalid/frozen cells and busy/recording/count-in edits refuse. If a
shared pattern would be edited, frozen linked tracks also refuse before mutation.
Only affected currently requested cells are republished; other queued launches,
pending stops and transport are preserved. No project format change. Asynchronous
imports still need a separate prepared-result boundary and remain unfinished.

## Prepared Session audio import results

`SessionAudioImportPlan.h` provides a pure destination plan and decoded-result
boundary for the existing Session file importer (claim #205). Planning records
project scope, stable track/scene IDs, the addressed cell identity, immutable
library content and prior media reference. It does no file I/O and touches no
engine/history. Decoding stays on the controller worker pool.

Completion resolves the stable destination and validates its current content
before retaining media or publishing anything live. It follows track/scene reorder
and ignores current selection; project replacement, removed targets or edits to
shared content invalidate the plan. Invalid decoded assets, frozen targets/linked
content, rack assignments and recording/count-in/busy states refuse. A pure
`apply` builds a candidate document; the controller retains only a validated asset,
installs that candidate and records one Import audio undo step. Other queued
launches and pending stops remain unchanged; affected requested links refresh.

The plan holds runtime immutable pointers and is not a persisted/JSON command.
A named import command consuming prepared results, browser import and processed
Audio Tools entry points remain unfinished. They must reuse this validation model
rather than decode or call plugins from Preview. No project format change.
