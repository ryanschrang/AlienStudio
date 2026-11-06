# Euclidean rhythm generator (T2-04 / MIDI-05 / MIDI-06)

In the Piano Roll tool list choose **Euclidean rhythm**, then open Settings.
Set Steps (1-64), Hits (0-Steps), Rotate right (0 to Steps-1) and Pitch (MIDI 0-127).
Press Apply to insert the rhythm at the amber cursor. Existing notes remain.
No selection is needed; generated notes become the selection. One Undo removes
the whole insertion and one Redo restores it. Repeated Apply inserts more notes.

Each cell is a sixteenth note, regardless of the drawing snap menu. Generated
notes last half a cell, with MIDI velocity 102 and chance 100%. A 3-hit, 8-step
cycle at rotation zero has hits at cells 0, 3 and 6. Rotation one moves these
to 1, 4 and 7. Integer modular distribution spaces cyclic gaps as evenly as
possible; the same settings reproduce the same rhythm without randomness or a
seed. This is a rhythm generator for one pitch, not an arpeggiator.

The whole cycle must fit after the cursor in the current loop. Invalid settings,
loop overflow or the clip note limit refuse the entire insertion. Zero hits
does nothing. Refusal keeps Settings open and preserves notes and redo history.
The generator does not extend clips or Song placements automatically.

**Project scale** defaults off. When enabled, the pitch snaps to the current
shared project scale at Apply, independently of local Piano Roll scale settings.
Undefined project key means chromatic; existing notes remain untouched. Notes
on a drum instrument still map to its MIDI pad numbers, so leave this option off
when exact drum-note mapping matters. Set the project key in Piano Roll > Scale.
Settings remain local to the editor; generated notes save like ordinary notes.

`PianoRollProcessingTests` covers distribution, rotation, preservation, MIDI scale
edges and atomic bounds/capacity checks. `ProjectControllerTests` exercises the
controls and owner history callbacks, empty-clip generation and current project
scale use. Native mouse/DPI use and listening remain separate manual checks.
