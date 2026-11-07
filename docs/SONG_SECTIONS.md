# Song arrangement sections (T2-04 / CHORD-01, format 35)

The Song has a **Sections** strip below its bar ruler. Sections are named,
coloured annotations over musical tick ranges, separate from point markers.
Up to 64 sections can exist; they may touch but cannot overlap. Each range must
fit inside the song and be at least one sixteenth long. Names are trimmed and
bounded to 32 UTF-8 bytes, matching the point-marker name limit.

Set the loop range, then right-click empty space in the Sections strip and choose
**Create section from loop**. It starts with a numbered name. Right-click a section
to rename, colour or delete it, or choose **Use current loop as section range**
to change its boundaries. Existing clips and point markers stay untouched.
Clicking a section seeks to its start. **Loop this section** sets and enables
that exact loop range as one undoable edit; **Go to section start** only seeks.
Other section edits each have one Undo/Redo step. Busy edits refuse and stale
menus/rename dialogs cannot change a replaced project or changed section.

Sections keep musical ticks across tempo and meter changes. They prevent the
song from being shortened past their ends. Metadata changes do not retime clips
or affect playback; setting the playback loop remains a separate action. This
increment does not move/copy the clips inside a section, reorder arrangements,
or implement a chord track. CHORD-01 remains partial.

Project format 35 adds a required `timeline.sections` array of objects with
integer `start`/exclusive `end`, string `name` and byte `colour`. Runtime identities
are regenerated on load like marker identities. Invalid types, ranges, overlaps,
names, ordering or capacity refuse the entire file. Version 34 and earlier load
with no sections. Older applications reject version 35 rather than drop sections.

TimelineTests covers bounds/overlap, capacity, equality, length and tempo/meter
behavior. SessionProjectTests covers canonical persistence and strict migration.
TimelineViewTests drives actual context menus, navigation, scope guards and history
alongside the existing marker/loop/layout suites.

## Manual acceptance (not yet run)

- At 100/150/200% DPI, check the new strip, clipped labels, short adjacent sections,
  zoom/scroll and interactions near the header. Existing markers, loop handles,
  tempo/meter ruler controls and clip rows should retain usable hit targets.
- Create Intro/Verse/Chorus sections from different loop ranges. Rename, colour,
  resize through the current loop and delete them. Test single-step Undo/Redo,
  save/reopen, blank names, overlap refusal and the 64-section bound.
- Click a section during playback and choose Loop this section. Check seek and
  loop boundaries with MIDI and audio at different tempos/meters. Metadata edits
  should not interrupt playback or recreate plugins. Compare Song WAV export
  with the same loop before adding annotation-only sections.
- Leave a section menu or rename dialog open, change its section/project, then
  complete it: the old action must leave the new target untouched.
