# Song chord track (T2-04 / CHORD-01, format 36)

The **Chords** strip below Song sections holds up to 256 non-overlapping harmonic
ranges. Set a loop, right-click empty space in the strip and choose **Create ...
Major chord from loop**. The menu shows the exact root to be created: the saved
project root, or C when no project key is defined. Major is an explicit default,
not an inference about the project's scale.

Right-click a chord to choose its Root or Quality, use the current loop as its
range, go to its start, loop it or delete it. Click a chord to seek to its start.
Quality choices reuse the Piano Roll chord vocabulary: Major, Minor, Diminished,
Augmented, Sus2, Sus4, Dominant 7, Major 7 and Minor 7. Root is a pitch class
0-11, displayed with canonical sharp names; there is no octave or inversion in
this harmonic context. Labels and colours derive from root/quality.

Each edit is one Undo/Redo step. Ranges must fit inside the Song and be at least
one sixteenth long; adjacent ranges and gaps are allowed. Overlaps, invalid values
and capacity refuse atomically. No-ops/refusals preserve redo. Busy edits refuse;
deferred actions check project scope, chord identity/content and any captured loop
range. Tempo/meter edits preserve musical ticks, adding Song bars when needed.
The Song cannot be shortened below a chord's end.

`Timeline::chordAt(tick)` is the shared read-only context query. Endpoints are
exclusive; a gap returns no chord. `SongChord` uses the existing `ChordType` enum
so future consumers can use the same quality definitions as the Piano Roll.
The document owns this context; future audio consumers must compile it into their
immutable playback snapshots rather than read a changing document on the callback.

Use **Create MIDI clip on** in a chord's menu to choose a Song track and turn the
chord into an editable MIDI clip. The clip uses the existing Piano Roll voicing
rooted at MIDI 60 plus the pitch class, with notes sustained for the exact chord
range. The clip belongs to the Song and its notes are independent of later chord
edits. One Undo removes the clip.

Creation requires an empty range on the destination track and supports up to
256 sixteenth notes. Recording, busy or frozen targets refuse the operation.
The menu resolves the chosen track by identity, including after track reorder.
Automatic accompaniment, chord-command wrappers and section-driven clip
rearrangement remain follow-ups. Annotation edits are excluded from playback
equality. Choosing Loop this chord explicitly changes the playback loop.

Format 36 saves required `timeline.chords` objects with integer start/exclusive end,
root and quality. Runtime identities regenerate on load like sections/markers.
Strict validation rejects invalid types, roots, qualities, ranges, order, overlaps
or capacity. Versions through 35 load with an empty chord track, retaining their
existing sections. Older applications reject 36 rather than silently drop chords.

TimelineTests covers all roots/qualities, shared-context boundaries, limits,
identity/history equality and tempo/meter behavior. SessionProjectTests covers
canonical persistence, strict field/range rejection and legacy migration.
TimelineViewTests drives real root/quality/context menus, navigation, one-step
history, refusal/redo preservation and stale actions alongside existing UI suites.

## Manual acceptance (not yet run)

- Create a progression from successive loop ranges. Try all root and quality
  menus, adjacent short chords, gaps, scrolling/zoom and 100/150/200% DPI. Existing
  markers, ruler controls, sections, loops and clip rows must remain usable.
- Edit root/quality/range, Undo/Redo, delete and restore, save/reopen. Check chords
  keep their ticks across tempo and meter changes and protect Song end length.
- Navigate and loop a chord during playback. Annotation edits must not retune or
  play notes, stop transport or recreate plugins. Compare playback and WAV export
  before/after metadata-only edits; only explicit loop/navigation actions differ.
- Leave root/quality/create/range menus open, change the chord, loop or project,
  then complete them. Stale actions must leave the current target unchanged.
