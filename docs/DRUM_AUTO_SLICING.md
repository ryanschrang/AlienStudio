# Automatic drum-pad slicing (T2-01 / INST-07)

The Drums panel shows the selected pad's original waveform and highlights its
applied frame range. The right edge marks the exclusive end frame. Apply a
range to see it in the preview, or use Undo to see the restored range.

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

## Channel Rack sample to channels

Open a Channel Rack channel that plays an audio file and choose **Slice sample > Into 4, 8 or 16 channels**. Each
equal slice becomes a new channel after it, named after the channel with the slice's number, with the channel's
volume, pan, output and choke group and no steps or notes. The channel itself stays. One Undo removes all the new
channels. The slices share the file's decoded audio: a slice channel saves the file's path and its frame range (format
37), so it relinks and reloads with the file. A slice channel's menu offers **Slice this slice**, which stays inside its
range. Loading another sound into a slice channel plays that sound whole.

Slicing needs the file decoded here (an offline file must be relinked first), and the rack holds 64 channels. It is
the `SliceRackChannel {channel, bounds}` command, whose bounds are the slices' edges in the file's frames; scripts can
give uneven edges. As with pads, there is no zero-crossing adjustment or fade, so listen for clicks at slice edges.

**Slice sample > At attacks** uses the pads' attack detector on the channel's audio (its own range, for a slice
channel) and runs the same command with the edges it finds, so it is one Undo step like the equal slices. The first
slice keeps the lead-in before the first attack and the last keeps the tail. It makes as many channels as the rack has
room for, 16 at most; with more attacks than that, the strongest become the edges. The search runs on a worker for a
decoded range of at most one minute. Silence, a steady sound or a single attack changes nothing and makes no Undo
step, and the status line says so. A result is dropped if the project, the channel or the recording state changed
while searching. `ChannelRackViewTests` covers the menu, the worker, slices of a slice, the room limit and the dropped
results with synthetic clicks; how well it finds the hits in real recordings has not been checked by ear.

## Channel Rack slices on keys

**Slices on keys** in the same menu keeps the slices on the one channel instead of adding channels: the first slice
plays on C5 (note 60, the pitch a plain step plays), and each next slice on the next key up. Choose **At attacks** (up
to 16 keys) or **4, 8 or 16 equal slices**. A note in the channel's piano roll, or a step's pitch, then picks the
slice; each slice plays at its own pitch, and keys without a slice are silent. **Play the whole sample on every key**
goes back. Each choice is one Undo step, and none needs room in the rack.

It is the `SetRackChannelKeySlices {channel, bounds, firstKey}` command; empty bounds plays the sample whole again.
The channel saves its key zones (format 38): for each, the file's frame range, its lowest and highest key and its root
key. The command puts each slice on a single key at its own root; the saved form and the engine also take a zone that
spans several keys and is pitched from its root; a multisample (below) makes those. A new sound on the channel, or
slicing it into channels, plays whole on every key.

`CommandEngineTests`, `RackEngineTests` and `ChannelRackViewTests` cover the command, the shared audio, saving,
playback of each zone, the menu and the attack search. It has not been listened to in the app.

## Channel Rack multisamples from several files

**Sound > Multisample from files...** on a channel puts 2 to 16 separate recordings on that channel's keys. When every
file's name says the note it was recorded on (`Piano_C4`, `Strings Eb2 soft`, `pad_F#3`; C4 is note 60, as the app
names notes), each file plays at its own pitch on that note and the keys between two notes are split halfway, the lowest
file reaching down to note 0 and the highest up to 127, so the whole keyboard plays. When any name says no note, each
file gets one key at its own pitch from C4 up, in natural name order, like a kit. Two files on one note are refused.
The channel is named for what the files' names share ("Piano"), and plays the lowest file as its sample. It is one
Undo step, and the status bar says how the files were laid out. Files are decoded on a worker first; a file that
cannot be decoded, or is too long to keep in memory, refuses the whole load with its name.

It is the `LoadRackMultisample {channel, paths, name, clips}` command (decoded audio, so no script form), laid out by
`rack_channel::multisampleLayout`. A key zone may now name a file of its own (format 39): the zone's `file`, absent for
the channel's own sample, with frames {0, 0} for the whole file. Each file is decoded once and counted against the
audio limit; each zone plays its own file at that file's sample rate. Every place a channel's sample path is followed
also follows its zones' files: opening a project (each file is resolved and decoded), the missing-audio list, Relink
missing audio, Save with samples (each file is collected), and the caches a project open lets go of. A zone whose file
is missing stays in the project and is listed as missing, while the channel's other zones still play.

Relinking used to give the channel its file back through the same edit as loading a new sound, which also dropped the
channel's slice range and key zones; it now moves the path and keeps them, for slice channels and key slices too.

`CommandEngineTests` (note names, the layout, the command, Undo, saving, moved files, refusals), `MemoryPeakTests`
(open, a missing zone file, relink, Save with samples, memory counted) and `ChannelRackViewTests` (the menu) cover it.
It has not been listened to in the app, and the note-name rule has only been tried on made-up names.

## Session audio to drum pads

Open **Copy / link** on a loaded, decoded Session audio clip and choose
**Slice audio to 8 drum pads (replace track kit)**. This switches the clip’s
track to Drums, replaces its eight pads with equal frame ranges and keeps
the original Session clip in place. Undo restores the previous kit and
instrument. The pad views share the decoded source with the Session clip,
including after saving and reopening. Streamed and offline clips are ineligible.
