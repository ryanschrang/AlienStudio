# Song warp markers (T1-08 / WARP-03)

Right-click an audio region in Song and choose **Enable warp (fit source to region)**.
This maps the remaining source audio to the current region length. Ordinary clips
remain unstretched until warp is enabled. Session launching is unchanged.

Use **Add warp marker here** at an interior musical position. Drag a marker's small
bottom handle to change its musical time while retaining its source time. Grid
snapping applies; Alt bypasses it. Markers cannot cross, and the two endpoints
stay fixed. Right-click an interior handle to remove it. Each completed operation
is one Undo step. The waveform preview follows the source mapping.

**Detect transients as warp markers** replaces interior markers with background
onset suggestions while retaining the endpoints. Detection uses stereo peak
energy, a minimum 80 ms separation, and at most 256 anchors. It is a starting
point for manual edits, especially on sustained or noisy material. It does not
infer BPM, beats, key, or time signatures.

Marker edits and Song/base tempo changes prepare a new pitch-preserving cache
on a worker. Playback pauses while preparing; press Play when ready. Tempo-map
changes inside a region are included, not just the tempo at its start. Escape
cancels preparation/analysis. Cancelled or failed warped regions are silent in
the playback snapshot; the editable markers/source remain intact. After fixing
the cause, use **Rebuild warped audio**, or Undo the edit. **Disable warp** restores
ordinary source playback. Warp editing is refused during recording.

## Rendering and editing contract

- Anchors pair musical content ticks with source seconds. Region content offsets
  advance through that map on trim, split and slip; moving/copying retains it.
- A piecewise-linear source map and the complete Song tempo map drive linked
  stereo WSOLA. Local duration ratios from 1/8 to 8x are admitted; extreme segments
  report an error rather than falling back to pitch-changing resampling.
- Decoded and streamed sources share bounded source-read buffers. Reverse and
  source looping are baked into the cache; crossfade partners include their tail.
- Caches are 32-bit floating-point WAVs, preserving source headroom. The native
  callback only streams prepared samples; no stretch processing, locks, file
  reads or heap allocation are added to it. Offline export uses its existing
  deterministic streaming mode.
- Playback, WAV/stem export and freeze share the same preparation function.
  Exports do not modify source markers or publish partial results on failure.
- Format 30 saves only the anchors and original source references. Earlier files
  default to no warp. Portable projects collect original media; caches regenerate.
- Private cache directories live in the system temp directory. The engine retains
  used caches for its lifetime so queued stream requests remain safe; normal engine
  shutdown removes them. Cache disk use grows with edits and clip duration. A
  process crash can leave temporary directories behind; project/source files are
  unaffected. Rendering long files is bounded in RAM but can take time/disk space.

This is background-prepared Song warping, not continuous live stretching while
turning the tempo control. Higher-quality transient handling and that real-time
mode remain WARP-04 work. The 40 ms grains and 6 ms alignment search can smear or shift attacks; this
version does not promise sample-exact transient pinning. WSOLA may colour sustained material; physical
listening is required before release.

## Validation

Run TimelineTests, SessionProjectTests, TimelineViewTests, TimelinePlaybackTests,
DeterministicStreamedExportTests and LongTempoWarpTests. New checks cover marker
ordering/inverse mapping, edit identity, old/damaged files, actual handle dragging,
Undo/Redo, preparation/cancellation/rebuild, tempo changes, preserved pitch and
stereo phase, float headroom, seeking, callback-size independence, callback
allocation, decoded/streamed export equivalence and read-failure protection.

Before release, listen to drums, vocals and stereo mixes with markers on attacks;
compare Song playback and WAV/stems at 60/120/200 BPM with changes inside a clip.
Exercise trim, split, slip, reverse, looping, fades, crossfades and freeze, then
save/reopen a portable project after moving it. Check long mono/stereo sources,
192 kHz material, cache disk use, low disk space, cancellation, shutdown and reopen.
