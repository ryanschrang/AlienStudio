# Production feature delivery

Order: effect/instrument automation, take lanes and comping, freeze and stem export.

## Automation

First increment: normalized bounded lanes and stable plugin-instance/parameter
identities, with focused tests. This is model groundwork only. It is not wired
into the editor, saved projects, plugin host or playback. Existing volume/pan
behavior and project versions are unchanged.

Next: persist instance IDs (new on insert/replace/duplicate; retained on reorder
and undo), serialize lanes transactionally, build runtime bindings off the audio
thread, add target selection and point editing, then schedule parameter updates
through a tested realtime-safe path. Do not notify UI/listeners from the audio
callback. Test seek, loop, stop, read bypass, manual-state restoration, missing
plugins, live/export equivalence and callback allocation.

Integrate built-in synth controls after the synth/MIDI branch reaches main.
Add send/return targets after mixer PR #84 reaches main.

## Take lanes and comping

Retain every loop pass independently. Show take lanes and select time ranges into
a non-destructive comp. Include boundary fades, gesture undo, portable media and
save/reopen tests. Reuse recording/count-in/punch/latency paths. Verify original
sources remain intact after comp edits.

## Freeze and stem export

Use final routing, automation and comp playback. Freeze retains original editable
state; unfreeze restores it. Define pre/post-FX and return/master handling, aligned
starts, tails and filenames. Cancellation/failure preserves existing files.
Verify stem sums against the corresponding mix, long files and portability.

## Integration baseline

Main: 80e2fff. #78 merged. #82 merged into the former #78 branch after its main
merge, so synth/MIDI work remains outside main. #84 owns sends/returns. Recheck
before shared-code changes. Separate branches can still need conflict resolution.

## Stem export increment (based on main 2b2fef1)

Sends/returns from #84 are now on main; take-comp PR #88 remains separate.
Song's export menu now offers whole-song and loop-range stems. An offline-only
capture object records post-track-mixer buses, post-return-mixer buses, and the
direct-to-master Channel Rack bus in one render. Dry captures receive the same
return latency compensation as the mix. Every WAV has the same aligned start
and duration. Master FX/gain/final limiting are excluded, and PCM clipping can
prevent exact reconstruction for hot individual stems. Plugin tails retain the
host's existing 30-second cap.

Output is staged in a generated sibling directory and published only after all
writers finish. Cancellation, plugin faults, read errors and write errors discard
the staging directory; existing destinations are refused. No project-format
change is required. Tests cover stem sums, alignment, callback allocations,
master exclusion, names, folder collision, cancellation and source/plugin errors.

Next: freeze/unfreeze needs a separately persisted original track state and a
pre-fader render path, with bypass/substitution that does not apply FX or
volume/pan automation twice. It is not implemented by this stem increment.
