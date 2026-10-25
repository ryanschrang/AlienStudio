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

## Stem export increment (integrated with main 12bb76e)

Sends/returns from #84 and take-comp PR #88 are now on main.
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

## Take comp increment (based on main afbfd9a)

Song recordings now retain their original passes, up to 256 per project, within
existing Session scene/track limits. Open **Song > Takes**, choose a pass, and use
**Use whole take** or **Use loop range**. The loop range must fit entirely inside
that pass. Outside the chosen range, the current arrangement remains intact.
New comp clips receive short 5 ms edge fades; existing Song fade/crossfade tools
can refine the joins. Each comp is one undo step. **Forget take** removes only
the list entry, leaving the active arrangement, Session clip and audio file.

Projects save as format 15. Versions 1–14 open with an empty take list. Earlier
app versions refuse format 15 instead of dropping the archive. Original audio
stays in Session cells, so existing missing-media, relink and portable-save paths
apply. Explicit source-scene/track deletion also removes its take references.

This is a take-list and range-comp workflow, not waveform lanes or continuous
loop capture. The recorder still stops at a loop boundary; record another pass
to add a take. Automatic loop splitting, waveform lane gestures, grouped
multitrack comp selection and listening/physical recording validation remain.
The synth/MIDI recovery (#87) and automation model (#86) are now on main;
automation UI/host scheduling remain unfinished, and #84 still owns mixer sends.

## Session track freeze increment

Based on validated stem-export head 63bc373 (#89); implemented separately so its
branch remains unchanged. Session tracks render one post-FX/pre-fader float32 WAV.
Frozen source placements are archived, the rendered cache replaces the active
track regions, native instances unload, and built-in voices/track FX suspend.
Fader, pan, sends and automation continue after the cache. Undo/unfreeze restores
original clips/settings; tempo changes unfreeze before applying the new tempo.

Format 16 stores the archive, rendered Session scene and original song length.
Scene/track reorder maintains references. Source scenes/cache cells are protected
from deletion/replacement until unfreeze. Portable save/relink use ordinary Session
media handling. Cancellation/failure leaves originals intact. Session-only scope:
tracks receiving Channel Rack channels and cross-track clip groups must be unrouted
or ungrouped first. Cache files remain for undo. Tail rendering uses declared
plugin tails and factory/sample releases, capped at 30 seconds. Manual native
plugin, listening, device and interruption checks are separate from automated tests.
