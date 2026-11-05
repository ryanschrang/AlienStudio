# Project key and scale (T2-04 / MIDI-06)

Project format 34 adds one `ProjectMusicalScale` to the project document and saved
project data: `defined`, root pitch class 0-11 and one of the existing ten ScaleType
values. Highlight and snap flags remain local view preferences. Future consumers
should read this shared context rather than introduce independent project keys.

In Piano Roll > Scale, choose a local Root and Scale, then **Use as project scale**.
This saves the context as one Undo step and enables **Follow project scale** in
that editor. A following editor uses the shared root/type for highlighting, Scale
rows and pitch snapping. Turn following off to restore its prior local key; turn
it on again to use the latest project key. To choose a different project key,
turn following off, edit Root/Scale, then publish it again. Changes never transpose
existing notes or retune audio. Notes outside the scale remain visible in Scale rows.

Executing a metadata edit sends a dedicated UI-context notification; it does not
republish audio or recreate plugin instances. Undo/Redo and reopening restore the
context through the existing document lifecycle. Busy, recording/count-in, invalid
values and stale project scopes refuse before metadata/history changes. No-op
edits preserve redo.

Files before version 34 load with an undefined project scale. Following an undefined
context falls back to the local settings. Older applications reject version 34 files
instead of silently dropping the new metadata. Follow mode is a local editor preference,
not a saved musical property.

Delivered: shared model/persistence and Piano Roll guide/fold/snap adoption. Channel
Rack and generator adoption, chord track, arrangement sections and automatic key
inference remain future increments. This does not complete MIDI-06 or CHORD-01.

Verification: SessionProjectTests covers strict format validation and migration;
ProjectControllerTests drives the actual Scale controls and history/scoped guards;
TypingPianoFocusTests retains existing guide and focus coverage. Native listening,
physical keyboard/DPI checks and hardware acceptance are recorded separately.
