# Command Engine

Roadmap workstream T1-02, checklist ID CMD-01. This note covers structural tracks, volume/pan, mute/solo and what later
slices need.

## Why

Every user operation should become a named, undoable command with typed arguments. One command
layer then serves undo/redo, macros, key bindings, MIDI controller scripts, user scripts and,
much later, the Workflow Assistant, which would plan and preview through the same commands
instead of driving the UI.

## Design

Code: `src/CommandEngine.h`, tests: `tests/CommandEngineTests.cpp`.

- **Command.** A `std::variant` of small argument structs (`CreateTrack`, `DeleteTrack`,
  `MoveTrack`, `RenameTrack`, `ColorTrack`, `SetTrackVolume`, `SetTrackPan`, `SetTrackMute`, `SetTrackSolo`). `name()` gives the stable name (for example
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
Only the addressed document field and live engine control change. Existing mixer and
Session mute/solo gestures retain their current direct editing paths.

CommandEngineTests covers true/false JSON round trips, strict types, side-effect-free
preview, live controls, preserved mix/other solo states, busy/invalid-target refusal,
redo preservation and sequential create/reorder batches. Manual listening remains pending.

### Known limits of these slices

- The Session track menu, rename box and add-track button use named commands. Commands own
  the undo commit; the view refreshes structure/title without invoking the legacy edit-commit callback.
  Scene edits and mixer gestures retain their existing controller paths. SessionViewTests covers
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
| CreateClip | `ProjectController::placeNewClip`, `setClipPattern`, `importAudio` | Async audio import needs a command that completes later |
| DuplicateClip | `editTimeline` with `Timeline::duplicate`; `duplicateScene` | Command wrapper only |
| MakeClipUnique | Song regions share Session clip notes | New operation (T1-03 shared clip model) |
| SetTrackVolume, SetTrackPan | Implemented: command setters write document then the addressed engine control | Existing mixer UI still uses its direct editing path |
| SetParameter (other targets) | Mixer edits via `trackSettingsEdited`, `returnSettingsEdited`, `masterGainEdited` with coalesced `commit` | Remaining targets need command wrappers |
| AutomateParameter | `editTimeline` on `parameterLanes`, plugin automation targets | Command wrapper over lane edits |
| CreateSend | Four fixed returns with per-track send level and pre/post (`TrackSettings`) | "Create" means new return tracks: T1-05 routing |
| CreateBus, RouteTrack, CreateGroup, AddToGroup | None | T1-05 mixer routing (MIX-03, MIX-04, MIX-05) |
| InsertDevice, MoveDevice, RemoveDevice | `setPluginRacks` records rack edits after `PluginHost` changes the live instances | Plugin instances are runtime state; needs a host-side command path |
| SetTempo | `setBpm` (whole BPM, unfreezes frozen tracks) | Tempo map (T1-01, TEMPO-01) |
| SetChord | Piano roll chord stamping on clip notes only | Chord track |
| SetArrangementSection | Timeline markers (`addMarker`, `moveMarker`, `removeMarker`) | Arrangement sections |
