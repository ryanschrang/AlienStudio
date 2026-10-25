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

## Send automation increment

Based on main 0cbf576: send/return routing is merged, and Send A–D curves now
extend the track automation editor, validation, persistence and sample-position
playback/export. Manual knob values remain unchanged; Read bypass and leaving
Song restore those values through the existing send glide. Pre/post mode stays
manual. Version 15 retains its layout; older readers reject the new parameter
IDs rather than discarding automation. No duplicate work on stem export (#89),
freeze (#93), or saturator (#92) is included.

Plugin-parameter automation still needs persistent instance IDs, lane storage,
editor targets and runtime scheduling. Send automation does not complete it.
