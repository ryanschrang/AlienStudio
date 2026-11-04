# T1-02 completion audit

CMD-01 is **not complete**. The Command Engine currently exposes 26 commands:
five structural track commands, four Session track mix commands, three Session
MIDI creation plus all-type duplication/independence, one Session clip delete
command (claim #186), two clipboard paste commands (claim #189),
one Session MoveClip command (claim #192),
five Session scene commands (claim #181), and CreateRackPattern/AssignRackClip
(claim #198), RemoveClipAudio (claim #203), SetMasterVolume (claim #208), and
ImportClipAudio.

The existing roadmap examples are a target catalog, not proof that every
operation already has a command. Completion requires both a validated command
and adoption by the corresponding project-editing UI. Transport, selection,
audition, and saving a file are separate lifecycle actions, not snapshot edits.

| Family | Delivered | Required before current Tier 1 coverage is complete |
| --- | --- | --- |
| Command contract | Typed arguments, stable names, Execute, Preview, Batch, Undo/Redo, grouping, no-op/redo preservation | Cross-family acceptance with dirty plugin state, asynchronous work and runtime failure; document refusal must precede every live side effect |
| Track structure | Create/delete/move/rename/colour; Session track menus | Audit other entry points for direct mutations and duplicate commits |
| Session scenes | Create/duplicate/move/rename/delete and Session add/menu/name UI | Native interaction/acceptance; shared reducer remains a follow-up to remove controller/Preview duplication |
| Session clips | Empty MIDI creation, all-type independent duplication (claim #196), MIDI make-independent; all-type Session cell delete/clear; PasteClip/PasteLinkedClip and all-type MakeClipUnique; identity-preserving MoveClip and destination menu; CreateRackPattern/AssignRackClip and existing UI adoption (claim #198); RemoveClipAudio and Remove audio button (claim #203); ImportClipAudio with the Import audio button, browser and Explorer drops on Session cells, finished takes and tempo renders (import plan/result boundary, claim #205) | Other rack/channel edit wrappers |
| Mixer parameters/routing | Session/Mixer track volume/pan/mute/solo; SetMasterVolume and master-fader gestures | Bus, return and send parameters; create/rename/delete bus/return; track output, group membership and sidechain routing; UI adoption |
| Devices | Existing host/controller operations and saved plugin instances | Insert/move/remove/bypass devices as commands; noncommitting controller adapters and runtime preparation/restore failure handling |
| Automation/parameters | Existing saved send/plugin lanes and playback | Named lane/point edits and generic parameter targets; Preview must not instantiate or call plugins; account for the older stock-automation cache audit (#179) |
| Tempo/meter | Existing project tempo and Song map editors | Named project-tempo, tempo-map and meter commands; unfreeze/warp-preparation lifecycle, grouping and UI adoption |
| Song/MIDI/rack edits | Existing editor/controller operations | Inventory and route remaining undoable placement, note, pattern and recording-settings edits through the same contract; retain existing gesture grouping |

## Suggested implementation sequence

1. Finish remaining Session clip operations and their UI entry points
   (remaining rack/channel edit wrappers).
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
imports placed on the Song timeline still use `importSampleToSong`; they belong to
the Song/MIDI/rack family below, not this command.
