# Alien Studio

**Alien Studio** is an ambitious, free and open-source digital audio workstation (DAW) for Windows 11. Its vision combines the immediacy of FL Studio-inspired pattern production, the flexible clip-based creativity of Ableton Live-inspired workflows, and the recording, editing, and MIDI depth associated with professional DAWs such as Pro Tools and Cubase.

Alien Studio is an **independent, original application**, not affiliated with or endorsed by any of those products. We aim to learn from broadly useful workflows without copying proprietary code, artwork, branding, or protected assets.

> **Project status:** Early music-making prototype with a pitch/velocity step sequencer, piano-roll tools, a four-track/four-scene MIDI/audio Session grid, independent Session synths and mixer controls, USB MIDI input, project save/open, and portable audio collection. Windows Debug/Release builds and automated tests run in GitHub Actions. Arrangement, recording, warping, plugin hosting, and production effects remain planned. Native audio-device behavior still requires hands-on Windows testing; see [the verification checklist](docs/WINDOWS_AUDIO_CHECKLIST.md).

## Product vision: three connected workspaces

1. **Production** — Fast beat making and composition with a channel rack, step sequencer, advanced piano roll, patterns, drum machines, samplers, synths, MIDI tools, and automation.
2. **Arrangement** — A multitrack timeline for audio and MIDI recording, clip editing, comping, fades, routing, automation, mixing, and finishing full songs.
3. **Live Performance** — A clip/session grid for launching loops and scenes, improvising arrangements, syncing tempo, and recording performances into the timeline.

The long-term differentiator is **one shared project and audio engine** across all three workspaces: create a pattern, launch it live, and arrange or edit it on the timeline without rebuilding it.

## Roadmap

### Implemented foundations
- [x] C++20/JUCE/CMake project, Windows Debug/Release CI, and automated tests.
- [x] Polyphonic lead/bass/pluck synth, USB MIDI input/sustain/reconnection, typing and mouse piano.
- [x] Editable 16-step pitch/velocity sequence and four-bar piano roll with processing, clipboard, and local undo/redo.
- [x] Four-track/four-scene clip launching, beat/bar timing, MIDI CC scene mapping, and WAV/MP3 audio clips.
- [x] Independent 16-voice Session synths, per-track preset/volume/balance/mute/solo and output meters.
- [x] Versioned project save/open and portable project copies with collected audio.
- [x] GPL-3.0 license file. Dependency attribution/distribution review remains required before releases.

### Next implementation sequence
1. Drum sampler and sample browser; extend the step workflow for drum channels.
2. Shared arrangement timeline and WAV export; move Session material into complete songs.
3. MIDI recording, metronome, count-in, overdubbing, and explicit input-to-track routing.
4. Audio recording, trimming, fades, take lanes, and comping.
5. Automation, EQ/compression/reverb/delay, buses, sends, sidechain, and latency compensation.
6. VST3 hosting, plugin scan/recovery, state persistence, and freeze/render.
7. Tempo warping, pitch correction, expressive MIDI/articulations, and optional assisted composition/mastering.

Autosave/recovery, global undo, project/media management, broader track/scene capacity,
and Windows device/listening verification accompany these milestones. See
[the delivery roadmap](docs/ROADMAP.md) for acceptance criteria and current boundaries.

These are **goals**, not promises of feature parity or delivery dates. Priorities will change as the audio engine, stability, and contributor capacity evolve.

## Build from source (Windows 11)

Install Visual Studio 2022 with **Desktop development with C++**, Git, and CMake 3.24+.

```powershell
git clone https://github.com/ryanschrang/AlienStudio.git
cd AlienStudio
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake fetches JUCE during configuration, so internet access is needed on the first configure. Windows build runs and downloadable artifacts, when available, are tracked in [GitHub Actions](https://github.com/ryanschrang/AlienStudio/actions). **A green CI build is not the same as completed hands-on audio testing.**

## Free and open-source commitment

Our goal is a fully usable, no-subscription DAW with no feature paywalls. The repository includes GPL-3.0; dependency attribution and distribution review (including JUCE), and a contribution policy remain release work.

## Development principles

- **Original design:** familiar capabilities, distinct interface and identity.
- **Audio reliability first:** low-latency playback, safe real-time code, and recoverable projects.
- **Creative continuity:** patterns, clips, tracks, and timeline all share one project model.
- **Accessible workflow:** efficient keyboard shortcuts, MIDI controllers, and clear UI.
- **Honest milestones:** distinguish working code, passing builds, tested features, and future plans.

## Synthesizer prototype (Issue #3)

Select **Lead (saw)**, **Bass (square)**, or **Pluck (sine)**, then click the
on-screen keyboard. Vertical position controls velocity; computer-keyboard
notes also work outside text fields and preset selectors. Multiple held keys sound together.
The original 16 pads sequence their selected pitches and velocities at sixteenth-note
intervals, with half-step note gates (A3/102 are the initial defaults). Stop silences voices and resets the sequence to step 1. The keyboard
can be played while the transport is stopped. Device restarts clear synth voices.
External MIDI input is available from the MIDI selector described below.

The audio-thread-owned DSP has 16 fixed voices, linear ADSR envelopes, velocity
gain, PolyBLEP saw/square oscillators, sine oscillators, and a one-pole low-pass
filter per voice. Presets affect new notes; held notes retain their patch.
Allocation uses idle voices first, then the oldest released voice, then the oldest
held voice. Repeated note-ons may overlap; note-off releases all matching notes
from that source. Sequencer and keyboard notes have independent ownership.
Stealing resets the selected voice immediately; listening tests should assess
clicks under sustained overload. Fixed 1/16 output scaling provides chord headroom.

GUI notes cross a bounded single-producer/single-consumer queue. Queue overflow
requests all-sound-off, preventing a dropped note-off from leaving stuck voices.
DSP and audio callbacks contain no heap allocation, locks, logging, or blocking I/O.

After building, run automated tests in either Windows configuration:

```powershell
ctest --test-dir build -C Debug --output-on-failure --no-tests=error
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Tests cover sample-counted ADSR boundaries and early release, note lifecycle,
repeated notes, velocity gain, all presets, pitch at multiple sample rates,
voice stealing, source isolation, panic/device reset, fractional step timing,
queue overflow/wraparound, finite bounded rendering, and DSP heap allocation.
Windows CI runs tests in both configurations.

Hands-on testing still required on Windows 11: listen to all three presets,
play chords beyond 16 notes, test short/long key presses and rapid retriggers,
change presets during held notes, stop/restart transport, change/restart the
audio device, and confirm silence without stuck notes or objectionable artifacts.
Physical MIDI input still requires the controller checks below.
Compilation, synthetic DSP tests, and a GUI smoke test do not verify audible
output, hardware MIDI, or real-time performance on Windows.

### USB MIDI keyboard input (Issue #8)

Connect a Windows-recognized USB MIDI keyboard/controller and choose its friendly
name from the **MIDI** selector in the top row. Ports with identical names have
separate port labels; selection follows their identifiers, not list positions.
The default is **None (off)**. All 16 MIDI channels play the current synth preset,
with velocity 1–127, note-on/off (including note-on velocity zero), and per-channel
CC64 sustain (values 64–127 on, 0–63 off). Repeated note-ons retrigger the current
pitch on that channel; a single note-off releases it, subject to sustain.

MIDI, typing/mouse piano, the step sequencer and piano roll have independent note
ownership. Releasing a MIDI note does not stop an equal-pitch typing/clip note.
Live MIDI, typing/mouse input and the legacy step sequencer share a 16-voice synth.
Each Session track has its own 16-voice synth; voice stealing stays within that engine.
Live input still uses the Device rack preset and is not routed through the Session mixer. MIDI highlights appear on the bottom keyboard, including notes
held by sustain. **MIDI activity** flashes when supported messages arrive; it
does not by itself verify an audible audio-device output.

Inputs are rescanned about once per second. Disconnecting releases the selected
device's notes/pedal and keeps its name marked **disconnected**. Reconnecting the
same identifier resumes automatically. If Windows assigns a different port ID,
select the new entry. **Refresh** rescans and reopens the selected port, releasing
its held notes; use it after an open failure or a driver that keeps a stale port
listed. Close other applications using an exclusive MIDI driver and retry if the
status says **Open failed**. Selecting None stops input. Device choice is currently
session-only and is not saved across launches.

Preset/source/transport changes, window deactivation, audio-device restarts and
app shutdown clear MIDI gates and sustain along with the existing panic path.
Play fresh notes after a reset; notes already held are not reconstructed.
CC120 silences only its MIDI channel, CC123 releases its keys while honoring
sustain, and CC121 releases sustain while preserving keys still down. MIDI clock,
program changes, pitch bend/aftertouch, recording and MIDI output are outside this
input feature; incoming notes play on the next audio callback without grid snap.

MIDI callbacks write validated three-byte events to fixed storage. They never
touch GUI components or the GUI note queue. The audio consumer never allocates,
waits on producer synchronization, opens devices or logs. Contention/queue
overflow requests a MIDI-only reset and discards older events, protecting dropped
note-offs without silencing other live sources. Device session tokens reject
late callbacks after switching/disconnect; reset revisions reject pre-panic data.

Without a controller, select **Test input (no controller)**. Enable **Sustain**,
press/release **Test C4**, confirm activity/highlight and sustained sound, then
disable Sustain to release it. Test C4 sends note 60 at velocity 100 through the
same input pipe/state/synth path as hardware. None, preset changes and Stop clear
the test. This verifies application routing, not Windows USB driver behavior.

Manual Windows hardware plan:

1. Select a controller; play individual notes, chords, repeated notes and soft/
   hard velocities on each preset, with transport stopped and during playback.
2. Hold sustain, release keys, then release the pedal. Test multiple MIDI channels
   on the same pitch; release one while typing or mouse piano holds that pitch.
3. Hold notes/pedal and disconnect, select another port, choose None, Refresh,
   change preset, Stop, Alt-Tab, restart audio or close the app. Check silence and
   cleared highlights. Reconnect and play fresh notes without restarting the app.
4. Reorder devices and test identical port names; force an exclusive-driver open
   failure if available, close the other app and Refresh. Check the status.
5. Verify the original 16 pads, piano-roll editing/tools/clipboard, typing piano,
   Z/X octave changes, text entry and Tab navigation. Check minimum window size.

`MidiInputTests` uses fake devices and concurrent producers to test translation,
sustain, velocity-zero releases, retriggers, device changes, stale callbacks,
reconnect/errors/retry, overflow, source-local reset and allocation avoidance.
GUI tests cover selection, test sustain/activity, device lifecycle/status and
accessible layout with a fake backend. Offline actual audio callback tests verify
velocity response, sustain, same-pitch source/channel isolation, disconnect,
panic/preset/device reset and silence after release. CI needs no MIDI hardware;
physical controller/driver and audible Windows output remain manual checks.

### Four-bar piano roll

Choose the **Piano roll** tab to edit a separate four-bar synth clip. The original
**16-step sequencer** remains available. Both share Play / Stop and tempo. The step sequencer uses the Device rack preset;
Session MIDI clips use their track preset in Mixer. Switching between step mode
and Session mode resets playback; switching between Piano roll, Session and Mixer
keeps Session playback running. The window resizes to fit
smaller displays.

- Left-click an empty grid cell to create a note. New notes reuse the duration
  and velocity of the last edited note; before the first edit, duration follows
  the selected snap and velocity starts at 102. Drag a note body to move it, or
  its right edge to change its duration. Right-click or select and press Delete
  to erase it.
- Choose 1/16, 1/8 or 1/4 snapping. Selected-note arrow keys move time/pitch;
  Shift+Left/Right changes duration. Page Up/Down or the wheel/scrollbar changes
  the visible pitch range (all 128 MIDI pitches, C−1–G9).
- Drag a velocity stem, or use the selected-note Velocity control (1–127).
  Notes at the same start time can be selected individually in the grid before
  changing their velocity.
- Use the selected-note **Chance** control (0–100%) for repeatable per-loop note
  probability. Lower-probability notes draw more faintly; skipped notes never
  acquire a playback gate or emit an unmatched note-off.
- Click the piano at the left to audition pitches. The bottom mouse piano and
  computer keyboard still work; Tab navigation and text entry keep their keys.
- **Load example** adds an original melody to an empty clip. **Clear** removes the
  clip's notes. Start Play to see the playhead and hear the loop.

Save the session before closing to retain MIDI patterns, track settings, and audio
references. Unsaved edits are lost; autosave/recovery is not yet implemented.
MIDI recording, arrangement playback, and external/plugin instruments remain future work.
Same-pitch overlaps share one gate until the last overlap ends; velocity changes
to a sounding note apply at its next trigger. Notes end at the four-bar boundary.
Edits publish at audio-block boundaries; stop, source changes, preset changes and
device restarts clear the scheduler's held notes. Preset/blur panic restarts a
running clip at the beginning, matching the safe all-notes-off path.

`PianoRollSequenceTests` checks fractional-tempo timing, loop boundaries, stop and
restart, live edits, same-pitch overlap ownership, snapshot concurrency, source
isolation and allocation avoidance. `TypingPianoFocusTests` also checks GUI note
creation/movement/resizing/deletion, velocity, snap, audition cleanup and focus
routing. `ToneEngineTests` renders the actual audio callback offline to verify
sound, playhead progress, safe stop/restart, edits and source changes without
audio hardware. Windows CI runs both Debug and Release. Linux GUI checks use a virtual
display; native Windows hardware sound still needs the manual check below.

Manual Windows test: open Piano roll, Load example, Play and change tempo. Add a
chord; move, resize and delete notes during playback, then change velocity and
snap. Check the loop boundary and Stop for hanging notes. Switch between tabs,
change presets, audition piano keys, and Alt-Tab while holding typing/mouse notes.
Return and play again. Confirm the original 16 pads and typing octave controls
still work; verify the UI at the minimum window size and normal display scaling.

### Highlight, copy, paste and duplicate notes

Shift-drag a rectangle from empty grid space to highlight notes. Ctrl+click
toggles individual notes; Ctrl+Shift-drag adds to the highlighted group.
**Ctrl+A** selects everything. Selected notes and their velocity stems use a brighter highlight.
The piano roll's **Edit** menu also offers Select all, Copy, Paste and Duplicate.

- **Ctrl+C — Copy:** remembers selected notes, including their pitches, lengths,
  relative timing and velocities. Copy leaves the clip and undo history alone.
- **Ctrl+V — Paste:** inserts the copied phrase at the amber insertion marker.
  Click the time ruler to position it without changing the selection. Creating
  a note on empty grid space also moves the marker. Placement follows the current snap setting. The earliest
  copied note starts at the marker; relative offsets stay intact.
- **Ctrl+D — Duplicate:** copies the highlighted phrase after its latest note
  ends, aligned to the next grid boundary. The new notes become selected so
  repeated duplication extends the phrase. Duplicate keeps the copied clipboard
  phrase available for later Paste.

Paste selects the new notes and advances the marker to the next grid boundary
after the pasted phrase, ready for another Paste. Each Paste or Duplicate is one
undo step. Ctrl+Z / Ctrl+Y restore the notes, selection and insertion marker.
Copy remains available after deleting the original notes or clearing the clip.

This is an internal note clipboard for the current editor session; it does not
overwrite the Windows text clipboard or exchange MIDI with other applications.
Text fields keep their normal Copy/Paste shortcuts. If the whole phrase will not
fit inside the four-bar loop or the 128-note capacity, nothing is pasted and the
editor explains why. Move the marker earlier, select fewer notes, or free space.

Clipboard regression tests check relative timing, velocities, unique IDs,
repeated operations, capacity/loop-edge rejection and failed-operation atomicity.
GUI tests cover highlights, cursor placement, shortcuts, undo/redo, retained
clipboard data and text-field isolation; offline callback tests cover adding and
undoing copied notes during playback without hanging notes.

Manual Windows check: highlight a chord or melody, Copy, click a later ruler
position and Paste. Check note positions/velocities, repeat Paste and Duplicate,
then undo/redo once per operation. Copy, delete the originals and Paste again.
Try pasting at the loop end and into a full clip. Check the Edit menu, numeric
text-field Copy/Paste, Tab navigation, typing piano, and Stop during playback.

### Piano-roll processing tools

Select notes before processing: click a note, Ctrl+click to toggle individual
notes, Shift-drag an empty area to select a rectangle, or Ctrl+A to select the clip.
Ctrl+Shift-drag adds a rectangle to the selection. Dragging a selected note moves the
group; Delete and the Velocity control also apply to the selection. The grid's
snap setting controls ordinary move/resize gestures.

Choose a tool in the toolbar and open its settings, or use these shortcuts while
focus is inside the piano roll:

| Shortcut | Tool | Controls |
| --- | --- | --- |
| Alt+S | Strum | Delay between chord pitches, up/down direction, velocity variation and seed. Each chord is grouped by its original start time. |
| Alt+U | Chop | Slice duration and gate percentage. The final fragment stays within the original note; all fragments become selected. |
| Alt+R | Randomize | Symmetric timing, velocity and semitone variation, with a repeatable seed. |
| Alt+Q | Quantize | Grid, strength and swing. Swing delays alternate grid points; strength blends toward the nearest point on that swung grid. |
| Alt+C | Chord stamp | Root MIDI note, chord type, inversion, length and velocity. Preview auditions the voicing before Apply adds it at the amber ruler cursor. |
| Alt+L | Scale guides | Root and scale choices plus independent background highlighting and pitch snapping. Changes apply immediately. |
| Alt+I / Alt+Shift+I | Invert up/down | Raises the lowest or lowers the highest tone of each selected simultaneous chord by one octave. |
| Alt+V | Reverse | Mirrors selected note extents in time while preserving pitch, length, velocity and probability. |
| Alt+A | Arpeggiate | Sequences each selected simultaneous chord by pitch with rate, gate and direction controls. |
| Shift+Tab | Editor / devices | Switches the central workspace between the pattern editor and compact Synth & MIDI device rack. |

The **Session** tab provides a four-track by four-scene grid of independent MIDI
or audio clips. Use **Launch at** to choose the next beat or bar boundary
(bar is the default). Left-clicking a track cell queues that clip for the
selected boundary; right-clicking opens it in the piano roll without changing
playback. **Edit selected clip** provides the same editing path for keyboard
and touch users. Launching a scene queues that row across all tracks for the
same beat or bar. The grid shows each track's selected, queued, and playing
clip. **Stop all tracks** follows the selected launch timing. Inside the piano
roll, left-click adds notes and right-click removes them. Active clips keep
playing while the Session and Piano roll tabs are changed. Separate clip states
are held for this session until saved to a project file.

Use **Save** and **Open** in the top bar to store and restore a project. Project
files preserve the 16-step sequence, tempo, live/step synth preset, independent track synth/mixer settings, all four-by-four MIDI
clip patterns, the selected clip, and beat/bar launch timing. MIDI notes retain
their pitch, timing, velocity and probability. Normal **Save** retains external audio references. **Save portable** writes a project
copy and collects referenced audio into an adjacent uniquely named media folder.
Move the project file and its media folder together (or put both inside one folder).
Collected paths are relative, so moving that folder does not break them; normal
saves preserve those relative references. Repeated source files are copied once.
Collection failure leaves an existing project intact; earlier media folders are
never automatically deleted. Audio is decoded again on open. If a referenced
file cannot be found or decoded, opening fails without replacing the current
session. Project reads, writes and audio decoding run off the audio callback.

To map a hardware MIDI CC to a Session action, select **Scene 1–4** or
**Stop all**, click **Learn CC**, then move the desired controller. The learned
channel and CC number appear beside the controls. Pressing the controller
launches the scene or queues Stop all using the current beat/bar launch timing;
release it before pressing again. **Clear** removes the selected mapping, and
learning an already mapped CC moves it to the newly selected action. Mappings
last for the current app session. MIDI note input, sustain and note-off handling
continue to work independently.

To add a file to a Session clip, select the clip, choose **Import WAV/MP3**, and
pick a mono or stereo file. Alien Studio decodes it on a background worker,
shows a waveform, and starts the one-shot clip at the selected beat/bar boundary.
MP3 decoding uses the Windows media decoder on Windows; builds without an MP3
decoder report an import error. Decoded audio is limited to 128 MiB per clip
and 512 MiB retained per application session. **Remove audio** switches that
clip back to its MIDI pattern. Right-clicking an audio
clip opens its waveform view; right-clicking a MIDI clip opens the piano roll.
File access and decoding stay off the audio callback.

Time controls use ticks: **240 ticks = one 1/16 note**, 120 = 1/32,
60 = 1/64, and 30 = 1/128. At 120 BPM, 30 ticks is 15.625 ms.
Strum and Randomize preserve note lengths while keeping them inside the loop.
Pitch and velocity stay inside MIDI bounds. A fixed seed and the same source
notes/settings produce the same random result; applying again processes the
already edited notes, so Undo first when comparing settings from the same source.

Chord stamp includes major, minor, diminished, augmented, suspended second,
suspended fourth, dominant seventh, major seventh and minor seventh voicings.
It does not require selected notes. Click the ruler to set its start, choose an
inversion, and use Preview to audition every tone. Apply releases the preview,
adds the complete chord, and selects its notes. Invalid pitches, loop overflow,
or the 128-note limit reject the complete chord without partial insertion.

Scale guides include major, natural/harmonic/melodic minor, Dorian,
Mixolydian, major/minor pentatonic, blues and chromatic scales in every root.
Highlighting colours scale rows and emphasizes the tonic without changing any
notes. Optional pitch snapping affects newly drawn notes and deliberate vertical
mouse or arrow-key moves. Directional moves advance to the next scale tone;
clicked pitches choose the nearest tone with a stable lower tie. Existing notes,
clipboard phrases, chord stamps, external MIDI and typing-piano input remain
unchanged. Alt+L reopens the current session settings.

Probability decisions are deterministic for each note and loop pass, restart
from the same sequence when transport restarts, and run in the fixed-size audio
scheduler without allocation or locks. Copy, paste, duplicate, chop, inversion,
reverse and arpeggiation preserve probability. Inversion, reverse and each
Arpeggiate Apply are single undo steps.

**Apply** commits the result; **Cancel** or Escape closes settings without changing
notes. Each Apply is one undo step, and each mouse gesture is one step. Use the
Undo/Redo buttons, Ctrl+Z and Ctrl+Y (or Ctrl+Shift+Z). Up to 64 edits are retained
for this session. Selection is restored with each edit. A new edit discards the
redo branch. No-op tools do not add history. Settings fields retain their normal
text editing and Tab/Shift+Tab navigation.

The clip has a 128-note limit. If Chop or Chord stamp would exceed it, the whole operation is
rejected with an explanation; no notes are lost. Processing affects selected
notes only. Edits and undo/redo publish through the same audio snapshot path as
ordinary editing, releasing removed/moved notes during playback. Same-pitch
overlaps retain the shared-gate behavior described above.

Regression coverage includes deterministic selected-note transforms, chopped
fragment IDs/capacity handling, swing/strength, sub-sixteenth sample timing,
loop-edge releases, processing during offline callback playback, selection,
gesture undo/redo, Cancel, and shortcut/text-field/Tab routing. Native Windows
listening remains a manual check:

1. Create a chord and a separate unselected melody. Strum the chord in both
   directions, vary velocity, then undo/redo. Confirm the melody is unchanged.
2. Chop a long note at 60 ticks with a 50% gate. Listen to individual attacks;
   undo once to restore the full note. Try a full clip and confirm the capacity
   message leaves all notes intact.
3. Randomize timing/velocity/pitch, undo, and reapply with the same seed. Compare
   results. Quantize partially, then fully, with swing; check loop-edge notes.
4. Apply, undo, delete and clear while playing; Stop/restart and switch tabs or
   presets. Confirm no hanging notes. Test typing piano and octave controls after
   Tab, after closing tool settings, and after Alt-Tab.
5. Open Scale with Alt+L, choose D natural minor, and toggle Highlight. Enable
   Snap, draw on C#, then move the note upward and downward. Confirm pitches land
   on scale tones; disable Snap and confirm chromatic drawing returns.

Ghost notes from other tracks, additional instrument types, per-note slides/expression
and riff generation are separate future additions. These processing
tools operate on the existing synth clip and do not add plugin hosting.


### Computer keyboard piano

Play chromatic notes while the piano or an ordinary control has focus with
**A W S E D F T G Y H U J** (C through B); hold several keys for chords.
**Z / X** or the Octave − / + buttons change the starting octave (C0–C8,
initially C4). The help line shows the current octave. Held notes retain their
original pitch when switching octaves, and the piano highlights held pitches.
Mouse and typing can hold the same pitch independently.

Typing input follows focus within the window, with no modifiers. Tab and
Shift-Tab navigate normally, including the octave buttons; moving to an ordinary
button does not disable notes. Tempo text entry, preset combo boxes, popup menus
and application shortcuts retain their keys. Returning from an editor to an
ordinary control restores note input without clicking an octave button.
Focus loss, window deactivation, preset changes and Play / Stop release notes.

Manual test plan (Windows Release artifact from the PR's Windows C++ CI run):

1. Click piano, hold A/D/G for a chord, release individually; check highlights
   and sound stop. Hold A through OS key repeat; it should not retrigger.
2. Hold A, press X, then release A: the original C must stop. Press A again
   for the higher C. Check Z/X and octave buttons at both octave limits.
3. Hold a chord and Alt-Tab, click tempo/preset, change preset, or stop transport;
   check no lingering highlights or sound. Return and play again.
4. Type in the tempo field and use modified shortcuts; no piano notes should play.
5. Test mouse press/drag/release, then hold a mouse note and the matching typing
   note: releasing either source must leave the other held until its release.
6. Check all three presets and the 16-step sequencer still play correctly.

Automated mapping/lifecycle/polyphony tests run with `ctest --test-dir build
-C Debug --output-on-failure` (or `-C Release`). GUI and audio-device behavior
still require the manual checks above on Windows hardware.


### Keyboard focus regression checks (Issue #10)

PR #7 handled typing only in the piano component. Tab moved key events to other
controls, while octave updates called `grabKeyboardFocus()` and appeared to repair
input. The fix follows window focus with one listener and removes that focus
side effect. Held keys are released on focus changes; keys held through a focus
change or panic must be lifted before they can play again. There is no persistent
keyboard enable/disable setting, and the listener follows the native window hierarchy and is removed on destruction.

`TypingPianoFocusTests` uses real JUCE components, focus traversal and peer event
dispatch with simulated physical key state. It checks Tab/Shift-Tab, notes from
ordinary controls, octave-button activation without focus stealing, octave repeat
suppression, native-window focus/letter-code casing, reparenting,
editor/combo/shortcut exclusion, chords,
blur release and listener
teardown/recreation. On Linux, run the GUI test with a display and window manager (or initialize
the usual WM atoms in a bare Xvfb session), then run
`ctest --test-dir build --output-on-failure`. Windows CI runs all tests
in both Debug and Release. These are event/focus simulation tests, not native
Windows key injection or an audible hardware playback test.

Manual Windows check: play A/S/D/F, Tab to a button, then play again. Repeat using
Shift-Tab; both directions must move focus and notes must still work on ordinary
controls. Tab to Octave Up and activate it with Enter: focus stays on the button,
and the next A uses the new octave. Type in the tempo field and preset selector:
no notes should play there. Tab back to a button and play a chord. While holding
it, Tab or Alt-Tab away; notes/highlights must clear. Release the keys, return and
play again. Repeat after closing/reopening the app, and check mouse notes,
presets, transport and the 16-step sequencer.

### Windows branding

The corrected official icon lives in [`assets/branding`](assets/branding/README.md).
Windows builds embed the supplied multi-resolution ICO directly, retaining all
seven sizes and the complete padded artwork. See that directory's README for
asset updates, Explorer/title-bar/taskbar checks at different Windows sizes, and
icon-cache refresh instructions. No startup splash is introduced.


### Session mixer

Open **Mixer** to choose Lead, Bass, or Pluck separately for each of the four tracks.
Each track has 16 voices; a busy track cannot steal another track's voices.
Volume is 0–200%, with 100% preserving the previous level. Balance ranges from
left to right, with the centre preserving both stereo channels. Mute overrides
Solo; multiple soloed tracks can play together. Gain changes use a short ramp to
reduce clicks. Meters show the post-control track peak; final output is clamped
for protection, not professionally limited or mastered.

Preset changes affect newly triggered notes; held notes retain their original
patch until release. Mixer settings also affect imported audio on that track.
Live MIDI/typing/mouse input and the legacy step sequencer remain on their separate
Device rack synth; Session mute/solo does not mute live monitoring. Project format
v3 stores mixer settings. Versions 1 and 2 load with the old global preset copied
to all four tracks, centred balance, unity volume, and mute/solo off.
