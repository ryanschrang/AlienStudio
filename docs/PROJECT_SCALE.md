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

Delivered: shared model/persistence, Piano Roll guide/fold/snap adoption, and optional
Channel Rack pitch-graph snapping. In Rack > Pitch graph, turn on **Scale** to snap
newly drawn pitches to the saved context. Undefined context leaves drawing chromatic.
The graph uses the existing MIDI-60 unity reference and searches inside its -24..24
semitone range, with ties downward. Samples retain that existing unity reference;
this does not infer a loaded sample's acoustic key. Right-click still resets to zero,
even when that pitch is outside the key. Existing steps are not rewritten, and playback
never resnaps them when the project key changes. The toggle is a local view preference.
A changed project, scale, row, pattern or pattern content cancels a stale draw gesture.

Piano Roll > Euclidean rhythm has its own opt-in **Project scale** toggle. It snaps
the requested MIDI pitch to the nearest tone of the current shared context when
Apply is pressed (ties downward, bounded to MIDI 0-127). This option is independent
of the Scale tool's local settings and follow/snap toggles. An undefined project
key leaves the pitch chromatic. Changing the key later never retunes generated
notes. Generator settings are local and do not change the project format.

Note-based rack rows can use the Piano Roll's existing follow option. Song sections
and the editable chord-track context are documented in SONG_SECTIONS.md and
SONG_CHORD_TRACK.md. Chord creation seeds its root from the saved key, but its
explicit quality and subsequent edits remain independent of that key. Other
generator adoption, automatic accompaniment and section-driven clip rearrangement
remain future increments. This does not complete MIDI-06 or CHORD-01.

Verification: SessionProjectTests covers strict format validation and migration;
ProjectControllerTests drives the actual Scale controls and history/scoped guards;
TypingPianoFocusTests retains existing guide and focus coverage. Native listening,
physical keyboard/DPI checks and hardware acceptance are recorded separately.
