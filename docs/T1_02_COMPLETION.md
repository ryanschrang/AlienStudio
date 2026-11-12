# T1-02 completion audit

CMD-01 is **not complete**. The Command Engine currently exposes 135 commands:
five structural track commands, four Session track mix commands, three Session
MIDI creation plus all-type duplication/independence, one Session clip delete
command (claim #186), two clipboard paste commands (claim #189),
one Session MoveClip command (claim #192),
five Session scene commands (claim #181), and CreateRackPattern/AssignRackClip
(claim #198), RemoveClipAudio (claim #203), SetMasterVolume (claim #208),
ImportClipAudio, five send and return commands (claim #223), and ten return strip
and bus commands (claim #226), SetTrackOutput and SetSidechain (claim #234), four device commands (claim #242), eight tempo and time signature commands (claim #250), twelve rack channel commands, thirteen rack step, pattern and swing commands, five rack sound and paste commands, three rack notes, groove and sample commands, six Song marker and loop commands, twenty Song clip edit commands, six Song section and chord commands, SetSongLength, CompSongTake and ForgetSongTake, four Song warp marker commands, four automation lane commands, PlaceSongClip, PasteSongClips, and SetClipNotes and SetPlacementClipNotes.

The existing roadmap examples are a target catalog, not proof that every
operation already has a command. Completion requires both a validated command
and adoption by the corresponding project-editing UI. Transport, selection,
audition, and saving a file are separate lifecycle actions, not snapshot edits.

| Family | Delivered | Required before current Tier 1 coverage is complete |
| --- | --- | --- |
| Command contract | Typed arguments, stable names, Execute, Preview, Batch, Undo/Redo, grouping, no-op/redo preservation | Cross-family acceptance with dirty plugin state, asynchronous work and runtime failure; document refusal must precede every live side effect |
| Track structure | Create/delete/move/rename/colour; Session track menus | Audit other entry points for direct mutations and duplicate commits |
| Session scenes | Create/duplicate/move/rename/delete and Session add/menu/name UI | Native interaction/acceptance; shared reducer remains a follow-up to remove controller/Preview duplication |
| Session clips | Empty MIDI creation, all-type independent duplication (claim #196), MIDI make-independent; all-type Session cell delete/clear; PasteClip/PasteLinkedClip and all-type MakeClipUnique; identity-preserving MoveClip and destination menu; CreateRackPattern/AssignRackClip and existing UI adoption (claim #198); RemoveClipAudio and Remove audio button (claim #203); ImportClipAudio with the Import audio button, browser and Explorer drops on Session cells, finished takes and tempo renders (import plan/result boundary, claim #205); AddRackChannel, AddRackSynthChannel, RemoveRackChannel, MoveRackChannel, DuplicateRackChannel, RenameRackChannel and SetRackChannelVolume/Pan/Mute/Solo/Output/Choke with the Channel Rack menus, buttons and knobs; SetRackStep, SetRackStepValue, FillRackSteps, RotateRackSteps, ReverseRackSteps, RepeatRackSteps, ClearRackSteps, SetRackPatternLength, SetRackSwing, CloneRackPattern, ClearRackPattern, RenameRackPattern and DeleteRackPattern with the step grid, value graph, length and swing controls and menus; SetRackChannelSound, SilenceRackChannel, SetRackChannelSynth, SetRackChannelPlugin and PasteRackSteps with the channel menu | Native interaction/acceptance; the old rack controller methods go in the final route cleanup |
| Mixer parameters/routing | Session/Mixer track volume/pan/mute/solo; SetMasterVolume and master-fader gestures; SetTrackSend/SetTrackSendMode with the Mixer send knobs and switches; CreateReturn/RenameReturn/DeleteReturn with the Add return button and return menu (claim #223); SetReturnVolume/SetReturnPan/SetReturnMute and SetBusVolume/SetBusPan/SetBusMute/SetBusSolo with the Mixer strips; CreateBus/RenameBus/DeleteBus with the Add bus button and bus menu (claim #226); SetTrackOutput with the Mixer output menu (a track's bus is also its group) and SetSidechain with the Plugins sidechain menu (claim #234) | Audit other entry points for direct mutations; native acceptance |
| Devices | InsertDevice/RemoveDevice/MoveDevice/SetDeviceBypass with the Plugins Add, Remove, Move and Bypass buttons and a shared capture, execute and rebuild flow (claim #242) | Runtime load failure stays a status message, not a refusal; rack-channel instrument commands; native acceptance |
| Automation/parameters | Existing saved send/plugin lanes and playback | Named lane/point edits and generic parameter targets; Preview must not instantiate or call plugins; account for the older stock-automation cache audit (#179) |
| Tempo/meter | SetTempo with the transport control (a drag is an audition until its release); SetTempoChange, MoveTempoChange, RemoveTempoChange and ClearTempoChanges, SetMeterChange, RemoveMeterChange and ClearMeterChanges with the Song ruler editors; the unfreeze is predicted by Preview and shares the Undo step (claim #250) | Native acceptance; remove the unused direct setBpm and editTempoMap routes in the final route cleanup |
| Song/MIDI/rack edits | AddSongMarker, MoveSongMarker, RenameSongMarker, ColourSongMarker, RemoveSongMarker and SetSongLoop with the Song marker strip, menus and keys, the loop strip, Loop button and L / Shift+L; MoveSongClips, TrimSongClipStart/End, SlipSongClips, DeleteSongClips, SetSongClipsMuted, SetSongClipGain, SetSongClipFades, SetSongClipFadeIn/Out, SetSongClipFadeShape, RemoveSongClipFades, CrossfadeSongClips, GroupSongClips, UngroupSongClips, LoopSongClips, ReverseSongClips, SplitSongClips, DuplicateSongClips and CopySongClips with the Song clip drags (move, copy, trim, slip, fade handles), Delete/Cut, nudge and lane keys, mute, group, fade, crossfade, normalize, loop-source, reverse, Split and Duplicate actions; AddSongSection, EditSongSection, RemoveSongSection, AddSongChord, EditSongChord and RemoveSongChord with the section and chord strip menus; SetSongLength with the Song length control; CompSongTake and ForgetSongTake with the takes panel, take lanes, comp drag and take menu; SetSongClipWarp, AddSongWarpMarker, MoveSongWarpMarker and RemoveSongWarpMarker with the audio clip warp menu, transient detection and the marker handle drag; SetAutomationRead, AddAutomationPoint, MoveAutomationPoint and RemoveAutomationPoint with the automation editor's Read button, point clicks and drags, and right-click delete; PlaceSongClip with the Song double-click and drop placements and the Session "Place in Song" menu item; PasteSongClips with Ctrl+V and the Song Paste menu item, bringing back a cut placement clip inside its own Undo step; SetClipNotes and SetPlacementClipNotes with the piano roll's note edits to Session MIDI clips (one or several at once) and Song placement clips, one Undo step per gesture | Native interaction/acceptance |

## Suggested implementation sequence

1. Finish remaining Session clip operations and their UI entry points.
2. Add mixer parameter and routing families using pure document reducers and
   validated, noncommitting controller adapters.
3. Add device/parameter/automation plans with explicit runtime failure behavior.
4. Add tempo/meter and remaining Song/MIDI/rack editor adapters.
5. Run cross-family Batch/Preview/Undo/Redo acceptance and remove obsolete direct
   edit/commit routes. Record native Windows acceptance separately from CI.

Each family is a reviewable increment on its own branch, with one active T1-02
claim/PR at a time under ROADMAP.md. Do not mark CMD-01 implemented when only a
command struct/parser exists or while its UI still bypasses the command layer.

`SetChord` and new arrangement-section features depend on T2-04 product/model
work. They are future command consumers, not authorization to build those
features inside a Tier 1 command migration. Existing Song marker/region edits
still belong in the current coverage audit.

## Identity safety

Preview uses a scoped, local runtime-object identity stream and initializes its
result from a document copy. It does not consume process-global track/scene or
project-scope identities. Immediate execution predicts the same runtime IDs;
an intervening real edit requires a fresh Preview. Persistent clip and track
registries continue to allocate in the copied document. No project format changes.

## Scene acceptance evidence

CommandEngineTests covers sequential scene batches, Preview/live agreement,
runtime identity isolation, rack duplication, invalid-later-command refusal,
recording/count-in and busy guards, no-op/redo preservation, strict argument
roundtrips, frozen/last-scene protection and scene limits. SessionViewTests
drives menu actions, one-step Undo, refresh-only host notifications, stale menu
refusal and deferred rename identity. Exact build/test results belong in the PR.

## Session delete acceptance

`DeleteClip {track, scene}` removes one Session cell with shared pure document
validation and reduction. The Clear clip button executes it, then asks the host
to reload the editor without publishing a piano-roll edit or another commit.
MIDI/audio/rack assignments and explicitly allocated empty clips are supported;
an absent cell is a no-op that preserves redo. Linked cells and shared rack
patterns retain their content. Existing cell-based Song placements fall silent;
independent placement clips and rack-pattern placements retain their sources.
Frozen tracks, busy state and recording/count-in refuse before live edits.
CommandEngineTests and SessionViewTests cover the command and actual Clear button;
exact build/test results belong in the PR. Native listening remains separate.

## Prepared Session audio import boundary

The Session file importer now captures project scope, track/scene identity, cell
identity and immutable content/media before worker decoding. Completion resolves
that same destination after reorder and refuses changed content, deleted targets,
project replacement, frozen linked content, rack assignments, busy state and
recording/count-in before media retention, engine publication or history edits.
Selection changes do not redirect the result. The prepared reducer builds a pure
candidate for command Preview.

`ImportClipAudio` is the named command over that boundary. The Import audio button,
browser and Explorer drops on Session cells, finished takes and tempo renders all
run it, so each is one Import audio undo step with the same refusals. A drop only
fills an empty cell, including when something arrived there during decoding. Sample
imports placed on the Song timeline run their own command, `ImportSongAudio`, through
`command::importSampleToSong`.
