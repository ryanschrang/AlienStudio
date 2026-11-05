# Automatic drum-pad slicing (T2-01 / INST-07)

In Drums, select a loaded pad and press **Auto slice**. This replaces the entire
eight-pad kit with up to eight chronological slices of that pad's selected range.
Unused pads are cleared. One Undo restores the old kit; Redo restores the slices.
Pads 3 and 4 retain their existing hat choke behavior.

Analysis runs on a worker and accepts decoded ranges of at most one minute. It
uses 5 ms energy windows, comparing each window against the preceding 20 ms,
with a relative noise floor and 30 ms minimum attack separation. Stereo energy
uses the louder channel, so opposite-polarity channels cannot cancel. At least
two attacks are required. Silence, steady audio or a single detected attack
leaves the kit and undo history unchanged; equal slicing and frame editing are
still available.

The first slice retains the lead-in before the first detected attack. Up to seven
strongest later attacks become boundaries, sorted in time; the final slice
retains the tail. Longer material is refused rather than partially scanned.
This is an energy heuristic, not beat or instrument recognition. Very soft hits,
closely spaced rolls, sustained sounds and noisy recordings can be misdetected.
There is no zero-crossing adjustment or automatic fade; inspect the saved Start
and exclusive End frame fields and listen for clicks.

Results are checked against the captured project, track identity and entire kit
before publication, including sample replacement, frozen and recording state.
Detection on an existing slice stays inside that selected range and maps its
boundaries back to the original source frames. Views share the original PCM;
the existing format 33 slice metadata saves and reloads them without a new format.

`DrumSamplerTests` covers synthetic attacks at multiple sample rates, stereo,
mono, deterministic selection, silence, steady audio and view ranges.
`ProjectControllerTests` covers the panel action, shared publication, one-step
Undo/Redo, persistence, subranges and stale-result refusal. These tests do not
establish detection quality on real recordings or native listening acceptance.
