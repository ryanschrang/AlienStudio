# Alien Studio

**Alien Studio** is an ambitious, free and open-source digital audio workstation (DAW) for Windows 11. Its vision combines the immediacy of FL Studio-inspired pattern production, the flexible clip-based creativity of Ableton Live-inspired workflows, and the recording, editing, and MIDI depth associated with professional DAWs such as Pro Tools and Cubase.

Alien Studio is an **independent, original application**, not affiliated with or endorsed by any of those products. We aim to learn from broadly useful workflows without copying proprietary code, artwork, branding, or protected assets.

> **Project status:** Integrated early DAW prototype with up to 16 tracks/32 scenes, 12 built-in sounds, drum sample kits/browser, an arrangement timeline (Session clips and Channel Rack patterns with snap, zoom, selection, loop ranges and markers) with WAV export, disk-streamed audio recording and offline tempo matching, seven built-in stock effects (EQ, compressor, saturator, reverb, delay, limiter, filter), an FL Studio-style Channel Rack with a pattern bank, plus initial stereo VST3 instrument/FX hosting. Project v8 unifies these features with saving, portable media collection, undo/redo and recovery. Native Windows device, plugin and listening checks remain required; see [the checklist](docs/WINDOWS_AUDIO_CHECKLIST.md).

## Product vision: three connected workspaces

1. **Production** — Fast beat making and composition with a channel rack, step sequencer, advanced piano roll, patterns, drum machines, samplers, synths, MIDI tools, and automation.
2. **Arrangement** — A multitrack timeline for audio and MIDI recording, clip editing, comping, fades, routing, automation, mixing, and finishing full songs.
3. **Live Performance** — A clip/session grid for launching loops and scenes, improvising arrangements, syncing tempo, and recording performances into the timeline.

The long-term differentiator is **one shared project and audio engine** across all three workspaces: create a pattern, launch it live, and arrange or edit it on the timeline without rebuilding it.

## Roadmap

### Application toolbar

The menu row above the transport groups application commands into **File, Edit,
View, Transport, Options and Help**. Audio settings (device, sample rate and
buffer size) and MIDI input/controller settings also have direct buttons at
the right. **F10** opens the menus; arrow keys navigate and Escape closes them.
**Ctrl+,** opens audio settings.

File includes Save as, portable copies and song/loop WAV export. Edit shows
the current project undo/redo operation. View opens every workspace and can
show the on-screen keyboard for playing notes. It starts hidden to leave more
editing space. Click the **piano icon** beside Audio settings to open or close
the bottom piano panel from Steps, Piano Roll, Session, Song, Mixer, Rack or
any other view; the icon lights while it is open. Switching views keeps the
same piano visibility, and hiding it does not disable external MIDI or the
typing piano. That display choice lasts for the current window.
Transport exposes playback, recording, metronome,
Song mode and the existing recording options. Options also links to the plugin
rack and interrupted-take recovery. Help lists common shortcuts and app details.
Commands that would replace/save a project or change audio devices are disabled
while recording or busy, and are checked again when selected from an open menu.
The existing transport controls and editor shortcuts remain available.

### Detachable workspaces

Select **Steps, Piano Roll, Session, Song, Mixer or Rack**, then click the
**detach icon** beside the piano icon (or **View > Detach current view**).
The editor opens in its own resizable native window, which can be dragged to
another monitor. Drums, Audio lab and Plugins can be detached in the same way.
For example, detach Song and Mixer to keep both visible at once.

Each editor is the same live component connected to the same project and audio
engine; detaching does not create another project or synth engine. Windows share
one transport. Use Song mode when editing patterns alongside song playback.
The original tab shows **Show detached window** and **Dock back here** instead
of a second editor. Selecting that view again raises its existing window.

Use **Dock into main window**, the detached window's close button, or
**View > Dock current view** to return it. **View > Dock all views** gathers all
editors. Global project shortcuts and typing-piano input work in detached views;
text fields retain text entry. All detached windows close with the application.
Window placement is temporary and is not saved in project files. Physical
multi-monitor movement, mixed-DPI layouts and audio-device listening remain
manual checks before release.

### Implemented foundations
- [x] C++20/JUCE/CMake project, Windows Debug/Release CI, and automated tests.
- [x] Polyphonic lead/bass/pluck synth, USB MIDI input/sustain/reconnection, typing and mouse piano.
- [x] Editable 16-step pitch/velocity sequence and four-bar piano roll with processing, clipboard, and local undo/redo.
- [x] Session clip launching with up to 16 tracks and 32 scenes (add, rename, reorder, duplicate, delete), beat/bar timing, MIDI CC scene mapping, and WAV/MP3 audio clips.
- [x] Independent 16-voice Session synths, per-track preset/volume/balance/mute/solo and output meters.
- [x] Versioned project save/open and portable project copies with collected audio.
- [x] Project-wide undo/redo, unsaved-changes prompts, one-minute autosave and crash recovery.
- [x] MIDI recording into Session clips (computer keyboard or USB MIDI keyboard) with count-in, metronome, overdub/replace and latency compensation.
- [x] Stock effects: EQ, compressor, reverb, tempo-synced delay and look-ahead limiter in every track and master FX rack, with custom panels.
- [x] Channel Rack: FL-style drum step sequencer (64 channels, 16/32/64/128 steps, pattern bank, per-step velocity/pan/pitch/chance/gate/nudge/ratchet, swing) routed to the master or a Session track.
- [x] Arrangement timeline: place MIDI clips, audio clips and Channel Rack patterns on a zoomable, scrollable timeline with snap, marquee selection, copy/paste/duplicate/group/split, loop range, named markers, playback from any position and WAV export of the whole song or the loop.
- [x] Song view in the main top bar, and a **Song mode** switch that plays the whole song (drums, synths, audio) from Steps, Piano Roll, Session, Song or Rack.
- [x] GPL-3.0 license file. Dependency attribution/distribution review remains required before releases.

### Next implementation sequence
1. Reliable long-file playback, waveforms and missing-media errors.
2. Non-destructive audio editing: slip, loop, reverse, normalize, fades and crossfades on the timeline.
3. Multitrack recording, monitoring, punch and takes.
4. MIDI recording follow-ups: controller data, quantize-on-record, loop-record takes and punch in/out.
5. Mixer routing and plugin reliability, including sidechains and multi-output routing.
6. Automation lanes for track volume/pan and stock/plugin parameters (on the timeline), tempo maps, tempo-warped audio clips and interactive warp markers.
7. Freeze, stems and final delivery.

Track implementation and verification in [the feature checklist](docs/FEATURE_CHECKLIST.md).
Use [the representative projects](examples/README.md) for production, live and recording checks.
The [feature summary PDF](docs/Alien_Studio_Feature_Summary.pdf) is a dated snapshot;
the checklist is the maintained source of status. The first streaming increment
adds a buffer primitive with focused tests; Session long-file integration remains unfinished.
See [the milestone acceptance plan](docs/LONG_PLAYBACK_PLAN.md).

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
references. Unsaved edits are autosaved once a minute and offered for recovery after an
abnormal exit (see Undo, saving and recovery below).
MIDI recording remains future work. The **Song** view (arrangement playback) is a main view in the top bar; VST3 instruments are under **More...**.
Same-pitch overlaps share one gate until the last overlap ends; velocity changes
to a sounding note apply at its next trigger. Notes end at the clip's loop boundary.
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

The **Session** tab is a grid of independent MIDI or audio clips laid out like FL Studio's
Performance mode, with a few touches from Ableton Live: tracks are rows down the left and
scenes are columns across the top. It starts with four tracks and four scenes; use **+ Track**
and **+ Scene** to add more (up to 16 tracks and 32 scenes), and right-click a track header or
scene button to rename, move, duplicate or delete it. The grid scrolls when it is larger than
the window; the track headers stay at the left and the scene buttons stay at the top. Clip
columns stop growing at 220 px in a wide window.

Each track header reads left to right: a round **light** (lit when the track will sound, ringed
when it is muted; click to mute, right-click to solo), a **pan** and a **volume** knob, the
track's **name button** (click to select the track, double-click to rename, right-click for the
track menu; the track's synth preset is in its hover text), an **S** solo chip, and a square
**stop** button that stops just that track at the selected launch timing (it is lit while the
track is playing a clip). The light, knobs and S chip are the Mixer tab's own controls for the
same track: moving one here moves it there, and a change is one undo step, like a Mixer edit.
Drag a knob to change it (hold Shift for fine control); double-click it to return pan to centre
or volume to unity; the mouse wheel nudges it.

Clip tiles are raised buttons: an occupied tile takes its track's colour (brighter while it is
playing) and has a darker launch button with a play triangle at its left; an empty slot is a
dark tile with a dim triangle. Left-clicking a tile queues that clip for the selected boundary
and the scene buttons launch a whole column; **Stop all** (top-left corner) stops every track.
Each clip holds up to 1,024 notes. Use **Launch at** to choose the next beat or bar boundary
(bar is the default). Right-clicking a tile opens it in the piano roll without changing
playback. **Edit selected clip** provides the same editing path for keyboard
and touch users. Launching a scene queues that column across all tracks for the
same beat or bar. The grid shows each track's selected, queued, and playing
clip. **Stop all** follows the selected launch timing. Inside the piano
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
never automatically deleted. Audio is decoded again on open. Project reads,
writes and audio decoding run off the audio callback.

**Missing audio.** A project only stores the *location* of each audio file, so a
file that was moved, renamed, deleted or sits on a drive that is not connected
cannot be loaded. The project still opens: every other clip, note, mixer setting
and plugin loads normally, and the affected audio clips (and drum pads) stay
silent and keep their saved path. Offline clips show a dark-red tile with the file
name, the status line says how many files are offline, and the tooltip lists
them. Press **Relink audio** (it appears whenever something is offline) and
choose a folder: it is searched, including sub-folders, for files with the same
names. If several match, the one whose parent folders best match the original
location is preferred, and a file that will not decode is skipped. Everything
found is relinked as one undoable step labelled *Relink audio*; anything not
found stays offline and can be searched for again in another folder. Saving
keeps the original path of anything still offline, so nothing is lost. **Save
portable** cannot copy a file that does not exist, so it stops and names the
missing file instead of writing a project with a hole in it. Opening an
autosaved recovery copy follows the same rules. Drum pads whose sample is
missing are not marked on the pads themselves; they are reported in the status
line and relinked with the same button. Not yet verified on Windows.

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
tools edit MIDI clips; the separate Plugins view assigns VST3 instruments and effects.


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
icon-cache refresh instructions. The approved artwork is used by the startup splash.


### Undo, saving and recovery

**Undo** and **Redo** (the arrow buttons left of **New**, or Ctrl+Z and
Ctrl+Shift+Z / Ctrl+Y) cover the whole project: notes, step sequencer, tempo,
mixer, live sound, launch timing, audio import/removal, Clear clip, and adding,
renaming, moving, duplicating or deleting tracks and scenes. Hover the buttons
to see what will be undone. Undoing a clip edit selects that clip again.
Rapid repeats of one control (mouse-wheel tempo or fader changes) merge into a
single step. Up to 200 steps are kept. Inside the piano roll, Ctrl+Z still
uses the piano roll's own note history; those changes also join the project
history. Undoing a track or scene move or deletion stops Session playback at
the next launch boundary.

Other shortcuts: Ctrl+N new session, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S
save as. The window title shows the project name and starts with `*` while
there are unsaved changes; the status-bar dot turns hollow amber.

New, Open and closing the window ask **Save / Don't Save / Cancel** when there
are unsaved changes. While a session has unsaved changes, Alien Studio writes
an autosave copy once a minute to `%APPDATA%\Alien Studio\Recovery` (it never
overwrites your project file). If Alien Studio does not close normally, the next
launch offers to recover that work; recovered sessions remain unsaved until you
save them. Saving, Don't Save on exit, New and Open remove the autosave.

### Session mixer

Open **Mixer** to choose one of 12 factory sounds separately for each track (the strip row scrolls when there are many tracks).
Each track has 16 voices; a busy track cannot steal another track's voices.
Volume is 0–200%, with 100% preserving the previous level. Balance ranges from
left to right, with the centre preserving both stereo channels. Mute overrides
Solo; multiple soloed tracks can play together. Gain changes use a short ramp to
reduce clicks. Meters show the post-control track peak.

**Sends and return tracks.** Between the tracks and the master are four return
tracks, **A** to **D**, for effects that several tracks share, such as one reverb
or delay for the whole mix. Every track strip has a **SENDS** row: one knob per
return (off to 0 dB; double-click turns a send off) and, under it, a letter
button that switches that send between post-fader (unlit, the default: it
follows the track's fader, pan and volume automation) and pre-fader (lit: it
ignores the fader and pan, so you can pull a track down and keep its reverb).
Either way a muted track, or one silenced by another track's solo, sends
nothing. A return's own strip has pan, mute, a volume fader (0-200 %, like a
track) and a meter; track solo never silences a return, so a soloed track is
still heard through its reverb. Put effects on a return in **Plugins** by
choosing **Return A FX** (to **D**) as the destination: any stock effect or VST3
effect works there, with up to eight per return and no instrument slot. Sends,
return strips and return FX are saved with the project (format v15; older
projects open with every send off and the returns at 0 dB with no effects),
are undoable, and are included in WAV export.

Plugin delay is compensated through the returns as well: each return waits for
the slowest return's effects, and the dry tracks wait just as long, so a send
always lines up with the track it came from. The extra delay is added to the
latency Alien Studio reports. Moving a send knob glides over a few
milliseconds, so it does not click; switching a send between pre and post
fader while it plays jumps at once and can click.

The **Master** strip at the right edge of the mixer has a volume fader
(0–400%, i.e. −∞ to +12 dB, double-click for 0 dB) and an output meter. The
master volume is applied after the master FX chain, is saved with the project
(older projects open at 0 dB), is an undoable edit, and is included in WAV
export. The meter reads the signal *before* the safety limiter, so its red clip
light at the top (held for about 1.5 s) means the mix reached full scale and
the limiter is working. The
limiter is a soft clipper: it leaves everything below 80% of full scale
untouched and rounds off peaks above that so the output can never exceed full
scale; it replaces the old hard clamp, which flattened peaks abruptly. It is a
safety net, not a mastering limiter.

**Gain staging change.** Built-in sounds and imported audio clips used to sit
quietly (a full 16-voice track was scaled by 1/16 and audio clips by 25%), so a
project had little usable headroom. Audio clips are now 6 dB louder and
synthesised voices are 6 dB louder, with the master fader giving 12 dB more if
needed. Projects saved by earlier versions will therefore sound about 6 dB
louder for built-in sounds and audio clips than before; hosted VST3 plugins are
unchanged. The values were chosen by calculation and tests, not by ear, and
need a listening check on Windows. Lower the master fader to restore the old
level of an existing project.

Preset changes affect newly triggered notes; held notes retain their original
patch until release. Mixer settings also affect imported audio on that track.
Live MIDI/typing/mouse input and the legacy step sequencer remain on their separate
Device rack synth; Session mute/solo does not mute live monitoring. Project format
v8 stores the Arrangement timeline, v7 the Channel Rack and v6 added track/scene names, drum kits, the bar-based arrangement and plugin state; v3 stores
mixer settings for the original four tracks. Versions 1 and 2 load with the old
global preset copied to all four tracks, centred balance, unity volume, and
mute/solo off. Older Alien Studio versions cannot open v8 projects, and a v8 project is refused (with a reason) if its timeline or Channel Rack is damaged. Both historical v4 variants and the older bar-based arrangements (v5 to v7) migrate on load: each old region becomes a clip on the timeline at the same bar.


### Recording MIDI: count-in, metronome, overdub and replace

**To record,** select an empty or MIDI clip (a Session cell, or the clip open in the
Piano Roll) and press the red **Record** button in the top bar. Play the notes on
the computer keyboard piano or a USB MIDI keyboard. Press **Record** again, or
**Stop**, to end the take. Record works from the Piano Roll and Session views; from
any other view it switches to the Piano Roll first. A clip that holds audio cannot
be recorded into, and the status bar says so.

- **Count-in.** When playback is stopped, recording starts with a count-in (one bar
  by default; Off, 1 or 2 bars in the options menu). The count-in always clicks, the
  first beat of the bar higher and louder. Playback starts on the exact sample the
  count-in ends, so the clip's first beat lines up with the first beat after the
  count-in. Press **Play/Stop** during the count-in to cancel without recording.
- **Already playing.** If the transport is running, recording starts straight away
  with no count-in and without restarting the loop. If the chosen clip is not the
  one playing on its track it launches at the next launch boundary (a bar by
  default), and only notes played after it starts are recorded.
- **Overdub or replace.** By default new notes are added to whatever the clip
  already holds (overdub), and you hear the existing notes while you play. In the
  options menu, **Replace** plays the clip without its old notes while you record and
  keeps only the new ones. A replace take in which you play nothing leaves the old
  notes alone. The loop length of the clip is kept; a clip is 64 steps (four bars)
  unless you changed it in the Piano Roll.
- **What you hear.** While a take is running, the keys you play sound through the
  chosen track's own instrument and mixer strip (its synth sound, drum sampler or VST3
  instrument, with that track's volume, balance and mute), not the Device rack synth.
  Notes you have already recorded loop back on the next pass, so you can hear a take
  build up. Outside a take nothing changes: live playing still uses the Device rack
  synth.
- **Metronome.** The metronome button next to Record clicks on every beat while the
  transport runs (also when not recording). Its volume is Low, Medium or High in the
  options menu. The click is added to the speaker output after the master fader, so it
  is not changed by the master volume and is **not** part of WAV export or Audio lab
  recordings.
- **One undo step.** A finished take is a single **Record MIDI** step that undo
  removes entirely (and redo restores). While a take runs, undo/redo and edits to
  the clip being recorded wait until it ends. **New**, **Open**, and removing or
  reordering tracks/scenes discard the take in progress. Stopping playback,
  switching to a view that does not play clips, or losing the audio device ends
  the take and keeps what was played so far.
- **Recording settings** (count-in, metronome on/off and volume, overdub/replace,
  latency offset) are saved in the project as an optional `recording` object (the
  project format version is unchanged; projects without it open with the defaults).
  Changing them is an undoable edit.

**Timing and latency.** Notes are stamped when the audio engine picks up the key
press, which happens once per audio block (at most 512 samples, about 10.7 ms at
48 kHz). The engine moves each note earlier by the audio device's reported output
latency plus your **Latency compensation** (-100 to +100 ms, in the options menu),
because you play against sound that reaches you late. The compensation is a
calculation, not a measurement: drivers report their latency differently, so
check it by ear on your own interface (see the Windows checklist) and nudge the
offset in 5 ms steps if recorded notes sit consistently early or late. Pressing a
key for a very short time still records a short note (at least 1/10 of a 16th note), a
key still held when the take ends is closed at that moment, and a note that
runs past the end of the loop is cut there. There is no quantize-on-record yet.

**Limits.** A clip holds at most 1024 notes; if a take fills it, the extra notes
are left out and the status bar says so. Only notes and their velocity are recorded:
pitch bend, mod wheel, sustain and other controllers are not stored. The take is
kept in memory until it ends and is written to the project (and autosave) only then,
so a crash in the middle of a take loses that take.

### Stock effects: EQ, compressor, saturator, reverb, delay, limiter, filter

Open **More... > Plugins**, pick a destination (a track or **Master FX**), choose one of the
**[stock FX]** entries at the top of the list and press **Add FX**. They need no scanning and
work in the same eight FX slots per track and on Master as VST3 effects, so everything
described under *Integrated workspaces and VST3 hosting* applies to them: order (Move FX
up/down), **Bypass / enable**, **Remove**, undo/redo, saving, Save portable, and WAV export
(which renders them like any other effect). **Open editor** shows the effect's panel.

| Effect | Controls | Notes |
| --- | --- | --- |
| **Alien EQ** | Low shelf, two bells (frequency, gain, Q) and high shelf, output level | The curve shows the combined response; drag a numbered handle to set frequency and gain, mouse wheel over a bell changes its width, double-click a handle to flatten it. A flat EQ is bit-exact pass-through. |
| **Alien Compressor** | Threshold, ratio, attack, release, knee, make-up, mix | Stereo-linked peak detector, gain reduction smoothed in dB, soft knee. The meter shows the reduction; **Mix** gives parallel compression. |
| **Alien Saturator** | Drive, character (soft, warm, hard), tone, mix, output level | Soft is a smooth tanh curve, Warm adds even harmonics like a tube stage, Hard clips. All three are clean at low levels, so Drive alone sets how hard the sound is pushed; the curve shows the result. It runs at 4x the sample rate so the harmonics it creates do not fold back as harsh aliases, and the 40-sample oversampling delay is reported as latency (the host delays every other track to match, and the dry part of Mix is delayed with it). Tone is a low-pass on the saturated sound (Open for none). Changing character crossfades over a few milliseconds. Peaks can go slightly above the clip level after the oversampling filter (about 10 % on a hard-clipped tone), so put a limiter after it if the next stage cannot take that. |
| **Alien Reverb** | Size, damping, width, pre-delay, mix | A classic room (Freeverb-style) reverb. Its reported tail follows Size and pre-delay, so exported WAVs include the decay. |
| **Alien Delay** | Time, tempo sync and division, feedback, ping-pong, tone, mix | With **Tempo sync** the time follows the project tempo (1/16 to one bar); ping-pong bounces the repeats left and right; Tone darkens the repeats (set to Open for none). Up to 2 s free, 4 s when synced. |
| **Alien Limiter** | Gain, ceiling, release | A 2 ms look-ahead limiter. The output never exceeds the ceiling (measured on sample peaks; it is not an inter-sample/true-peak limiter). The look-ahead is reported as latency and the host delays every other track to match. |
| **Alien Filter** | Mode (low-pass, high-pass, band-pass, notch), slope (12 or 24 dB/oct), cutoff, resonance, mix, output level | Drag the handle on the curve sideways for the cutoff and up or down for the resonance (or use the mouse wheel); double-click it to reset both. At the default resonance (0.71) both slopes are Butterworth: -3 dB at the cutoff. Changing mode or slope fades the filtered sound out and back in over a few milliseconds, so it never clicks. The reported tail follows cutoff and resonance, so a ringing filter is not cut off in WAV export. Mix at 0 % is bit-exact pass-through. |

All controls are ordinary host parameters: they can be typed ("1.5k", "250 ms", "-6"),
double-clicked to reset, and are what the upcoming automation lanes will drive. Their
settings are stored in the project inside the rack entry (a `description` with format
`AlienStock` plus the state), so no project-format version change was needed; a project
that names a stock effect this build does not know loads with that slot as a placeholder
and keeps its saved state. Older Alien Studio builds treat the entry as a missing plugin.

Settings changes are smoothed (no zipper noise or clicks), non-finite input (NaN/infinity from
a misbehaving plugin) is replaced by silence so feedback paths cannot lock up, and the
audio path never allocates, locks or blocks. The effects are stereo in and stereo out;
there is no sidechain input yet, and parameter changes made while playing are not yet
recorded as automation.

### Channel Rack: FL-style step sequencer and pattern bank

Press **Rack** in the top bar (it is the fifth view button; the older tabs keep their
positions). The old **Steps** tab, with its single 16-step pattern for the live synth,
is unchanged. The rack is a separate, richer sequencer for drums and samples.

**Channels.** One row per sound, up to 16. A new project starts with Kick, Clap, Hat and
Snare, all built in (synthesized by Alien Studio, no sample files). Each row is laid out
like FL Studio's channel rack, left to right:

- a **light** that is lit when the channel will sound (ringed when muted): click to mute,
  right-click to solo;
- a **pan** knob and a **volume** knob (drag up/down, Shift for fine, mouse wheel, double-click
  to reset; volume 80% is unity, the top is about +1.9 dB). The pointer shows the value, and a
  small dot appears on a knob that is not at its default;
- the **name** button: click to hear the sound, double-click to rename, right-click for the
  channel menu (rename, choose a built-in sound or **Load sample file...**, **Silent**, output,
  choke group, mute, solo, clear this channel's steps, move up/down, delete). Hover it to see
  what the channel plays (a built-in drum, or the sample file);
- **S** (solo) and an **output** chip: **M** sends the channel to the master, **T1** to **T16**
  into that Session track's mixer strip, so its volume/pan/mute/solo, effects and meter apply.
  A channel routed to a track that is later deleted falls back to the master;
- a small **selection lamp**, lit for the selected channels. Clicking it selects the channel
  without playing its sound.

**Selecting several channels.** As in FL Studio, **Ctrl-click** a name or lamp to add a channel
to the selection or take it out, **Shift-click** to select every channel between it and the last
one clicked, and **Ctrl+A** to select them all; a plain click selects just that one. The graph
editor shows the last one clicked (its row is marked in its colour). With several selected, the
light, solo, knobs and channel menu of any of them act on all of them, as one undo step: mute
and solo set them all the same way, a knob turns them all by the same amount, and the menu's
sound, output, choke group, pattern tools, paste, clear, duplicate, move and delete apply to each
(the menu says how many). Rename, Load sample file and Copy steps stay with the clicked channel.
The selection is part of the screen, not of the project.

**Resizing the name column.** Drag the right edge of the name buttons (on any row, or on the
**CHANNEL** label above them) to make the column wider or narrower, between 64 and 320 pixels;
double-click the edge to fit the longest channel name. Solo, output, the lamp, the steps and the
graph editor move to make room. The width is saved with the project but is not an undo step, like
choosing a pattern; older projects open at the usual width.

**A channel in the piano roll.** Choose **Open in piano roll** in a channel's menu (or press
**Ctrl+P** with the rack focused) to edit that channel's steps in the selected pattern as notes,
like FL Studio's channel piano roll. Each lit step is a one-step note: its height is the step's
pitch (middle C is no change, two octaves either way), and its velocity and chance are the step's.
Drawing, moving and deleting notes rewrites the steps, one undo step per gesture, while the
rack's pattern plays. A note snaps to the nearest whole step and lasts one step (its length does
not set the step's gate yet), a step holds one note (the last one placed wins), and the status bar
says when a note was adjusted. Gate, nudge and ratchet are kept as they are. Picking a Session clip puts the piano roll back on clips.

**Steps.** Click or drag to paint steps; a drag across several channels paints each cell it
crosses without gaps. A press on a lit step erases along the drag instead; right-drag always
erases. Painting plays the sound. A whole stroke is one undo step. Ruler numbers mark each
beat, and every group of four steps alternates in shade, as in FL Studio. Steps are raised
buttons; a lit step takes the channel's colour and glows, and one in a channel that will not
sound (muted, or silenced by another channel's solo) is greyed. In a very wide window the
buttons stop growing. A **choke group** cuts the other channels of the same group when one hits
(closed and open hat).

**Graph editor.** Under the rack, the **Velocity / Pan / Pitch / Chance / Gate / Nudge / Ratchet**
buttons choose what the bars show for the selected channel. Click or drag across the bars to draw
(a fast drag draws a straight line between the bars it crosses); right-click a bar to reset it.
Only lit steps can be edited. Pan runs -63 to +63 (the middle snaps to centre), pitch is +-24
semitones, chance is the probability (0 to 100%) that the step plays.
- **Gate** is how long a hit sounds: a full bar (the default) plays the whole sample, a lower bar
  cuts it off after that percent of the step, with a short fade.
- **Nudge** plays the step later, up to one whole step after the grid (FL Studio's shift), for a
  laid-back hit or a flam. It never plays early.
- **Ratchet** plays the step 1 to 4 times, evenly spaced inside it, for hat rolls and stutters.
  The gate then applies to each hit's share of the step, and the nudge moves every hit. One chance
  roll decides the whole step: all its hits play, or none. Marks on a lit step show anything
that is not the default: a dot for chance below 100%, a bar for pitch, a tick for pan.

**Patterns.** The **Pattern** chooser holds up to 64 patterns. **+** adds an empty one;
**...** clones, renames, clears or deletes the current one. **Steps** sets the pattern's
length to 16, 32, 64 or 128 steps (one, two, four or eight bars); longer patterns scroll sideways
with the scroll bar or Shift + mouse wheel. The rack holds up to 64 channels, which scroll with the
mouse wheel when they do not fit.
**Swing** delays every second sixteenth (0% is straight) for all patterns. The Play
button plays the selected pattern from its first step, looping; switching patterns while it
plays takes effect at the next block, not on a bar line.

**Saving.** The rack is part of the project (format v7; gate, nudge and ratchet from v12) and of
undo/redo. Projects saved before v12 open with every step at a full gate, on the grid, with one hit.
Earlier versions of Alien Studio cannot open v12 projects. Built-in sounds are
saved by name; sample files by path, relative when they sit inside the project folder, and
**Save portable** copies them next to the project. A missing sample opens as a silent,
red-labelled channel that **Relink audio** can repair, like a missing clip.

**Limits (honest list).**
- The rack plays by itself **only while the Rack tab is shown**, unless **Song mode** is on (top bar).
  To hear patterns in a song, place them on the Song view's four pattern lanes (see below): they then
  play in the Song and are in **Export WAV**.
- Pitch is set per step (+-24 semitones, which also changes the sample's speed). There is no
  per-channel root note, sample envelope or start/end trim, and no
  per-step note length, flam or sample offset yet; every hit is a one-shot.
- Channel routing reaches Session mixer strips (volume/pan/mute/solo/FX/meters). Instrument-plugin
  tracks keep their own audio; the rack is mixed into the same strip afterwards.
- Changing the order or the sounds of channels while it plays can glitch for one audio block.
- Loaded sample files stay in memory for the whole session and count towards the 512 MiB audio limit.
- Verified by automated tests only on a Linux build (model, engine rendering, project file, undo,
  mouse gestures on the screen's components). Not yet verified on Windows hardware: see the
  checklist.

### Song mode: hear everything together

By default each view plays only its own thing: Steps plays the step sequencer, Piano Roll and
Session play clips, Rack plays the selected pattern, and the **Song** view plays the arrangement.
Press **Song mode** (the button next to **More...** in the top bar) and **Play** plays the whole Song
from Steps, Piano Roll, Session, Song and Rack, so the drums, synths and vocals you have placed
can be heard together while you work on any of them. Mixer, Drums, Audio lab and Plugins play
nothing of their own, so they do not change what is playing.

- If the Song has nothing on it, Play says so in the status bar instead of running silently.
- Recording MIDI into a clip needs the clip view, so pressing Record turns Song mode off (and says so).
- While the Song plays, the clock at the top shows the Song's own bar, beat and 16th.
- Song mode is not saved in the project; it starts off each time the program starts.

### Arrangement timeline: clips, patterns, loop and markers

Open it from the **Song** button in the top bar (between Session and Mixer). The screen is a ruler of bars and beats above one lane per
Session track and four **pattern lanes** for the Channel Rack. A strip above the ruler holds the
**loop range**, and one above that the named **markers**.

**What can be placed.** MIDI clips and audio clips from the Session grid (on their own track's lane)
and Channel Rack patterns (on any of the four pattern lanes; patterns on different lanes play at
the same time). A placement is a *reference*, not a copy: edit the notes in the Piano Roll or the
steps in the Channel Rack and every place that clip or pattern sits on the timeline changes with
it. A clip repeats for as long as it is stretched. Nothing overlaps on one lane: what you drop,
paste, place or resize onto is replaced or shortened. The song is 1 to 16,384 bars long (18 hours at 60 BPM) and grows when
you place something past its end (the **Song length** box shortens it again, but never below the last clip).

**Placing.** The **clip list** on the right (the **Clips** button hides it) shows every Channel Rack
pattern and every Session clip that has something in it. **Drag a row onto the lanes** to place it: a
clip always lands on its own track's lane, a pattern on the pattern lane under the pointer (the first
lane if the pointer is over a track), at the snapped position under the pointer; a ghost and the help
line show where it will go. **Double-click a row** (or press Enter on it) to put it at the play
position instead. A dot on a row means the Song already holds it.

Double-click an empty spot in a track's lane to put that track's selected scene there
(or its first scene that holds a clip); double-click in a pattern lane to put the Channel
Rack's selected pattern there. **A track with no clip yet gets a new, empty clip** of four bars
instead: it takes a free cell of that track in the Session (adding a scene when every cell is
taken), it is a clip of its own (not another reference to one you already placed), and it is one
undo step. Double-click it to open the Piano Roll and write notes. **New empty clip** is also on the
right-click menu and the **Place...** button for any track lane, when you want one on a track
that already has clips. Right-click an empty spot to choose which clip or pattern, or use the
**Place...** button to put one at the play position on the selected row. Double-click a clip to open
what it plays (Piano Roll, Session, or the Channel Rack for a pattern).

**Selecting.** Click a clip; Shift- or Ctrl-click adds or removes; drag across empty space to draw a
selection box (it takes every clip it touches; hold Shift to add to the selection); Ctrl+A selects
everything; Esc clears. Grouped clips (Ctrl+G) select and move together.

**Editing.** Drag a clip to move it (pattern clips can also change lane); drag its right edge to
resize it; drag its left edge to hide or reveal the start of its music without moving the music. Ctrl-drag
copies. **Alt-drag an audio clip's body** to slip its source without moving or resizing it:
right reveals later audio, left reveals earlier audio, stopping at the source start. This is
unsnapped, at the timeline's tick resolution. In a mixed selection only audio clips slip.
Audio edges and fade handles keep their usual gestures. For other drags, hold **Alt** to
ignore the grid; to move audio freely, start a normal drag and then press Alt. Each drag is
one undo step; Esc cancels the preview.

**Arming tracks and listening to an input.** Each track's header has a round arm button (it turns
red when armed) and shows the track's input under its name. Right-click the track name to arm it,
choose its **Input** (one channel, or a stereo pair, from the inputs enabled in **Audio settings**,
which now lists up to 32) and how it **monitors**: **Auto** plays the input while the track is
armed, **In** always plays it, **Off** never does. What you hear goes through the track's effects,
volume, pan, mute and solo, and a thin meter along the bottom of the header shows the input level.
If the chosen input is not enabled on the device the header says so. Arm, input and monitoring are
saved with the project and each change is one undo step. Use headphones when monitoring a
microphone, or turn monitoring off, to avoid feedback.

**Recording onto the Song.** With the Song view showing (or Song mode on), press **Record**: every
armed track whose input is enabled records to its own new WAV (24-bit, at the device's rate) while
the song plays. When stopped, the count-in clicks first and the song starts after it; when already
playing, recording starts at once. Press **Record** or **Stop** to finish. Each take becomes an audio
clip on its track, in a free Session cell (a scene is added if there is none), placed where it was
played: the device's input and output latency is taken off, plus the **Audio input latency** offset
in the record options menu (use it to line up takes if your interface reports its latency wrongly).
All the takes of one pass are one undo step, and a take replaces whatever was under it on its lane.
Turn on **Punch in/out at the loop range** in the same menu to record only between the loop markers:
start before them for a pre-roll, and the take stops by itself at the loop's end. Without punch, a
take also stops where the loop wraps or the playhead jumps, because each take is one piece (take
lanes for loop recording come later). Takes are saved in a "*project* takes" folder next to a saved
project, or in *Documents/Alien Studio takes* for one not yet saved.

**Fades and crossfades (audio clips).** An audio clip has two small handles in its top corners: drag
the left one in for a fade in and the right one in for a fade out (straight-line fades, shown
shaded). To crossfade, put two audio clips end to end on a track, select the second (or both) and
choose **Crossfade** from the right-click menu: the first clip plays on past its end while the second
comes in, on equal-power curves, over one beat. Drag the second clip's left handle to lengthen or
shorten it. **Remove fades** clears a clip's fades and crossfade. None of this changes the audio
file, and playback and WAV export sound the same. The right-click menu also has slip, source loop,
reverse, normalize and **Fade in and out** (one beat each).

| Key | Does |
|---|---|
| Space | play / stop from the play position |
| Delete, Ctrl+X, Ctrl+C, Ctrl+V | delete, cut, copy, paste at the play position (pattern clips paste to the selected lane) |
| Ctrl+D | duplicate: copies go just after the selection |
| Ctrl+G, Ctrl+Shift+G | group, ungroup |
| S or Ctrl+E | split the selected clips (or every clip under it) at the play position |
| M | mute / unmute the selected clips |
| Left / Right | nudge the selection by one grid step; Shift = one bar; with nothing selected, move the play position |
| Up / Down | move a selected pattern to another lane; with nothing selected, move the selected row |
| Home / End | play position to the start / end of the song |
| Page Up / Page Down | scroll back / forward by almost a screen |
| L, Shift+L | loop on/off; loop the selected clips (or the whole song) |
| K | add a marker at the play position |
| Ctrl+Left / Ctrl+Right | jump to the previous / next marker |
| + / -, Ctrl+wheel, Ctrl+0 | zoom in / out around the pointer; zoom to fit |

The wheel scrolls the lanes, Shift+wheel (or a trackpad) scrolls sideways, and the scroll bars
work as usual. **Fit** zooms to show all your clips (at least four bars) and **Follow** keeps the moving playhead in view. **Snap** can be 1 bar, 1/2 bar, 1 beat,
1/8, 1/16, **Auto** (follows the zoom) or **Off**. S is also a typing-piano key; on this screen it splits clips instead.

**Playing from anywhere.** Click the ruler (or drag along it) to put the play position there;
press Play and the song starts from it, or jumps to it at once if it is already playing. Clicking
an empty spot in a lane does the same. **Loop range:** drag in the loop strip to set it (the ends
and the middle can be dragged; double-click loops one bar); with the loop on, playback repeats it.
Edits made while the song plays do not restart it.
**Markers:** double-click the marker strip, press K, use the **+ Marker** button or right-click; click a marker to start from it,
drag it to move, double-click to rename; the right-click menu changes its colour or deletes it. There
can be 128 markers.

**Export WAV** (button at the top right) renders the whole song, or only the loop range, to a
stereo 48 kHz/24-bit WAV, including the Channel Rack patterns.

**Limits (honest list).**
- Audio is not tempo-warped on the timeline: audio plays at its original speed, so changing the
  project tempo changes where a placed audio clip ends but not how fast it plays.
- Fades are straight lines and crossfades equal-power; there is no choice of curve. A crossfade
  needs the clips to touch; the outgoing clip is silent in it if its file ends at the join. Reverse
  and source looping are not available for streamed (very long) clips.
- At most 1,024 clips, 128 markers, 16,384 bars (18 h 12 min at 60 BPM) and four pattern lanes. A Session clip lives on its
  own track's lane; to move music to another track, copy it there from the Session grid.
- Automation covers track volume and pan only; there is one loop range and no per-clip pitch.
- The timeline is controlled with the mouse and keyboard only: no MIDI-controller mapping.

### Integrated workspaces and VST3 hosting

Use **More...** in the top bar for **Drums**, **Audio lab** and **Plugins**. The Steps, Piano Roll,
Session, Song, Mixer and Rack views have buttons of their own.

- Drums: load WAV/MP3 samples into eight pads per track, audition and sequence MIDI
  notes 36–43. Select **Drum kit** in Mixer. The MIDI routing selector in Drums
  also routes external MIDI to a track's VST3 instrument; typing/mouse piano
  and the legacy step sequencer continue to use the live synth.
- Audio lab: configure device inputs and record mono/stereo takes to a new WAV;
  recording streams 24-bit PCM directly to disk with **no 60-second duration cap**.
  A fixed 262,144-frame FIFO (1 MiB mono / 2 MiB stereo) feeds a dedicated writer.
  On Stop, the FIFO drains and the file is finalized off the audio thread. A disk
  overrun stops recording and reports the continuous prefix; write/flush failures
  retain the unfinished WAV and its recovery journal. **Recover takes...** repairs
  an interrupted file from its complete audio frames and publishes it without
  overwriting an existing destination.
  Takes are loaded into Session when they fit the existing 128 MiB decoded-clip
  and 512 MiB session budgets. Larger takes remain safely saved at the selected
  WAV destination, with an explicit message; long-file streaming playback is a
  separate milestone. Full waveform editing, recording latency compensation and
  live warp markers remain future work.
  Tempo matching reads a chosen WAV/MP3 source in bounded blocks and streams a
  separate WAV with no 60-second input cap, a 0.5–2x duration ratio and linked-stereo
  WSOLA processing. Sources too large for Session can be selected directly. Large
  results stay on disk when they exceed Session memory budgets; Cancel removes
  unpublished render output, and IO failures retain recoverable partial output.
- Plugins: **Scan VST3...** opens a menu: scan the standard VST3 folders
  (`C:\Program Files\Common Files\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3`), scan any
  folder, or add a single plugin file. Folders are searched recursively (to 12 levels;
  `.vst3` bundles are not entered and symbolic-link loops are not followed). Every file is
  loaded in a **separate process** with a 60-second limit, so a plugin that crashes or hangs
  is recorded against that file, with the stage it reached, and the scan carries on. Results
  are remembered per file (size and modification time): unchanged files are not loaded again,
  **Retry plugins that failed** and **Rescan everything** override that, and **Cancel scan**
  stops immediately, even in the middle of a stuck plugin. The list is saved after every file.
  **Show the last scan log** lists every file and why each failure happened; the running
  logs are `plugin-scan.log` and `plugin-host.log` in `%APPDATA%\Alien Studio\Logs`.
  A scan that says OK means the plugin's description could be read. Plugins that ship a module
  manifest are described without being created, so a scan cannot tell whether they can be
  created; the safety check below does.

Plugin crash containment, from the point of view of a user:

- **Safety check.** The first time a plugin file is loaded into a project, a short
  compatibility check runs in a separate process (load, bus layout, a short run, a state
  round trip, unload). A plugin that crashes or hangs in it is **not loaded**: its slot stays
  in the project with its saved settings and reads `NOT LOADED - blocked by the safety check`.
  A plugin that passes is trusted until its file changes. **Scan VST3 > Re-check plugins that
  were blocked** tries again; the same menu can switch the check off.
- **Fault trapping.** Every call into a plugin made by Alien Studio (create, prepare, process,
  save and restore state, open its editor) runs under a fault trap. A plugin that crashes,
  throws or produces NaN/infinity while playing is switched off for the rest of the session and
  never called again: an effect passes its unprocessed input through, an instrument goes silent,
  the slot reads `FAILED - switched off`, and the failure is written to the problem log.
  A plugin that fails during WAV export makes the export report **plugin failures** instead of
  success. A plugin whose editor fails to open reads `editor crashed` and keeps playing.
- **What this cannot catch.** A fault trap is not a sandbox. Heap corruption that surfaces
  later, a crash on a thread the plugin started itself, a plugin that never returns while being
  played or while its editor is open, and anything that happens in an editor window after it has
  opened still take the application down; autosave is the backstop. Running plugins in a separate
  process permanently is not part of this release.
- **Checking a plugin on your own machine.** `AlienPluginCheck` (a console program built with
  Alien Studio) scans for plugins and runs the full battery on each, one process per plugin, and
  writes a report. See [docs/PLUGIN_COMPATIBILITY.md](docs/PLUGIN_COMPATIBILITY.md) for how to run it,
  what it checks, and which plugins have actually been tested (no commercial plugin has been
  verified yet).

Each Session track has one instrument slot (replacing its built-in synth/kit)
plus eight ordered FX slots. Master has eight FX slots. Select a target, choose
an installed plugin (or a stock effect), and use **Use instrument** or **Add FX**. Select a rack slot
for **Open editor**, **Bypass / enable**, **Move FX up/down**, or **Remove**.
Bypassed instruments are silent; bypassed effects pass audio through. Native
editors have a generic parameter-editor fallback. Missing or unsupported plugins
preserve their description, opaque settings and bypass state in the project;
missing instruments stay silent rather than substituting a different sound.

The engine renders in preallocated chunks of at most 512 samples and timestamps
Session MIDI within each chunk. Plugin playheads receive tempo, PPQ, 4/4 meter,
play/stop and sample time. Track latency is aligned up to 96,000 samples per
rack; changes in reported latency during playback can produce a discontinuity.
WAV export uses isolated plugin instances, trims initial reported processing
latency, and includes reported effect tails up to 30 seconds. Export refuses
missing/unsupported plugins instead of silently omitting them.

Initial scope is stereo-output VST3 instruments and stereo FX without sidechains.
No VST2, AU, CLAP, multi-output routing, plugin automation lanes, MIDI output from
plugins, freeze, or bridging of 32-bit plugins is provided. Running processors
are in-process: the safety check and fault trapping described above contain the common
failures, but not heap corruption, plugin-created threads or hangs. Rack changes and project loads detach/restart the audio callback; native
parameter adjustments themselves remain live. Plugin state is only read back (which
also detaches the callback) when it can matter: Save captures it whenever playback is
stopped, or while playing only if a plugin was edited; undo, redo and track/scene edits
capture it only if a plugin was edited; autosave never interrupts playback, so a
minute-old recovery copy may lack plugin edits made while the transport was running.
Renaming a track or scene, or editing notes, does not rebuild plugin instances. Plugin binaries/licenses and plugin-internal sample libraries are
not copied by Save portable. Keep those installed on a destination computer.

Recovery cleanup is fenced against in-flight autosaves. Portable-media relocation
updates the active document and undo/redo history for clips and drum samples.
Project v6 loads both prior v4 dialects, v5 arrangements and versions 1–3.


### Long recording files and recovery

The Audio lab shows accepted recording time and time written to disk. Recording
uses the selected mono/stereo device inputs, 8–192 kHz, and preserves the existing
monitoring-off behavior. There is no duration field or one-minute stop. Capacity
now depends on disk throughput/space and filesystem limits. RIFF WAV headers have
a reserved chunk that switches to RF64 when the file passes 4 GiB; third-party
RF64 compatibility and physical multi-hour recording remain manual checks.

An in-progress take has an adjacent `*.recording-<id>.partial.wav` file. Its journal
is in `%APPDATA%\Alien Studio\Recording Recovery` on Windows. WAV headers are
checkpointed after approximately every second of written audio. Recover takes
can rebuild a stale header from the payload and discard an incomplete final
sample frame. A crash may lose queued/in-memory or OS-cached audio; recovery does
not guarantee survival of every sample after sudden power loss.

If the intended destination already exists when finalizing, it is preserved and
the unfinished take remains recoverable. Recovery chooses a new sibling filename
when needed. Keep unfinished WAVs and journals together until recovery succeeds;
never remove original source recordings to make space automatically. The
tempo-warp renderer also streams from disk with no one-minute input cap.

### Full Channel Rack piano-roll notes

Open a rack channel with **Open in piano roll** or **Ctrl+P**. Editing now stores a full note sequence instead of converting it back to steps: lengths, fractional starts, overlapping notes, chords, velocity, chance and MIDI pitches 0-127 are retained. This works with the rack's existing sample channels; playback ends each note with a short release, and samples still end naturally if they are shorter than the note. Sustaining samples by looping, synth channels and VST3 instrument channels are later milestones.

A melodic row displays a miniature note preview; clicking it opens Piano Roll. Drum rows retain gate/nudge/ratchet step sequencing. Step painting and row step tools do not overwrite melodic rows. **Clear this channel (notes and steps)** explicitly removes its notes and returns the row to step mode. Duplicate, move, delete, undo, pattern copies and save/open retain melodic data. Shortening a pattern is refused if it would cut an existing note; move/shorten those notes first. The initial import of a legacy drum row uses one-step notes, as before. Keep a row in step mode for gate/nudge/ratchet programming; the legacy import does not translate those controls into melodic notes.

Projects with melodic rack rows use format 13; older projects still load as step rows. The sample player has 64 voices and a fixed scheduling queue, with no callback allocation. Song/WAV-export scheduling uses the same note path, clips note endings to region boundaries, and plays future fractional note-ons after a partial-step trim/seek. It does not chase notes whose starts have already passed.

### Channel Rack built-in synth instruments

Use **+ Channel > Built-in synth** and choose one of Alien Studio's 12 factory sounds (Lead, Bass, Pluck, Sub Bass, Acid Bass, Soft Pad, Strings, Brass, Square Lead, Organ, Electric Keys or Bell). A new instrument opens its empty piano roll. Draw long notes or chords; the synth sustains and uses its preset's release envelope when each note ends. Overlapping notes of the same pitch release independently. The rack has a shared 64-voice pool across samples and synths.

Change the preset with **channel menu > Sound > Built-in synth**. Pan, volume, mute/solo and Output routing use the rack's existing controls. Choosing a drum, loading a sample, or choosing Silent switches the sound source; the channel's note data stays intact. Instrument selection, duplication and edits support undo/redo. Project format 14 saves the preset; older channels load as sample channels. Song playback and WAV export use the same synth path.

Clicking a channel name gives a timed preview. Piano-roll key/chord previews can sustain until released; stop, app deactivation and panic release voices. On step-mode synth rows, full gate means one step (or one ratchet interval); sample full gate still means play the sample to its end. Live hardware MIDI and the global typing/on-screen keyboard keep their existing Session routing in this milestone. VST3 rack instruments, custom synth-patch editing and per-instrument MIDI input routing remain later work.

### Live MIDI for Session instruments

In **Plugins**, scan/select a VST3 instrument, choose a destination track and click **Use instrument**. Set **Live MIDI** to that track to play it from a hardware controller, the typing keyboard or the on-screen piano while stopped or playing. The same selector also plays Session built-in synths and drum kits. **Standalone synth** restores the separate live synth. The Drums input selector shares this route; plugin destination and live destination are independent.

Hardware note channels, velocity, CC messages and pitch bend reach the selected VST3 instrument. Recording continues to monitor the armed recording track. Changing the live destination releases the previous instrument, and device reset/panic releases hardware notes. This route is a current-session control, not saved project data. Channel Rack VST3 slots and direct live input to Channel Rack channels remain a separate integration step.

### Chord progression builder

In Piano Roll, choose **Chord progression** in the processing tools list and click **Edit...**. Set a tonic MIDI note (48 = C3), Major or Natural minor, and four scale degrees. The initial 1-5-6-4 sequence becomes I-V-vi-IV in major; each triad's quality comes from the chosen scale. Change individual degrees to write your own sequence.

**Steps / chord** uses sixteenth notes: 16 steps is one 4/4 bar. Apply inserts all four chords at the insertion cursor and selects their notes for further editing, strumming or arpeggiation. Existing notes remain. The sequence must fit inside the current loop and note limit; failures leave the clip unchanged. Undo/redo treats insertion as one edit. Use the normal transport to hear the inserted sequence. This tool does not copy another DAW's UI or include third-party chord content.


### Track and return stem export

In **Song > Export WAV**, choose **Track and return stems** for the whole song
or loop range, then choose a parent folder. Export creates a new numbered folder
with 48 kHz / 24-bit stereo files: one per Session track, Return A–D, and Channel
Rack channels routed directly to master. Track stems include track FX, faders,
pan and volume/pan automation. Returns are separate, so shared effects are not
duplicated into each track. Project mute/solo states apply.

Files share a start and duration, including plugin tails (the host's existing
30-second cap). Master effects, master gain and final output limiting are excluded.
Summing the files reconstructs the pre-master bus subject to PCM quantization
and clipping of individual stems above full scale. Cancellation or a plugin/write
failure discards the staging folder. Existing folders are not replaced. Freeze
and unfreeze are not implemented by this export increment.

### Recording takes and range comps

Recording in Song view keeps original passes in **Takes**. Choose a pass and
**Use whole take**, or set loop markers inside the pass and choose **Use loop
range** to replace just that part of the active arrangement. Undo restores the
previous comp. Original files remain unchanged. **Forget take** removes a list
entry without deleting its active comp or audio file. Projects use format 17.
Recording still stops at loop boundaries; continuous loop takes and waveform
lanes are not included in this increment.

### Send automation

In **Song > Automation**, choose a track and **Send A**, **Send B**, **Send C** or
**Send D**. Click to add a point, drag to move it, and right-click a point to
remove it. The top of the graph is unity (0 dB); the bottom is off. Linear and
Hold curves use the Song playhead, including seeks and loop wraps. Each gesture
is one undo step. **Read** disables the curve and glides back to the manual send
knob, which automation never overwrites. Leaving Song playback also returns to
the manual level.

Sends use the mixer's existing pre/post-fader switch. Pre-fader sends ignore
volume/pan but obey mute/solo; post-fader sends follow the processed track.
WAV export uses the same curves. Send automation is saved in the existing
version-15 automation list (parameter IDs 2–5). Earlier builds reject projects
containing these unknown targets instead of silently dropping the curves;
projects without send lanes remain compatible with the earlier format-15 parser.
This does not automate return faders, pre/post switches or plugin parameters.


### Freeze and unfreeze Session tracks

Right-click a Session track header in **Song** and choose **Freeze track (render
audio)**. Freeze renders its arranged clips and track effects into a 48 kHz,
32-bit floating-point cache, before fader, pan, sends and volume/pan automation.
These mixer controls and shared return effects remain live. Original instrument
and effect instances unload while frozen; saved plugin settings and source clips
are retained. Choose **Unfreeze track** to restore them. Both actions undo/redo.

The header reads **Frozen audio**. Source edits, recording, instrument/preset
changes and edits to the cached Song clip are blocked until unfreeze. Changing
tempo automatically unfreezes tracks, so caches never play at an incompatible
tempo. Original Session cells stay intact; while frozen, Session playback uses
only the new rendered cache cell. Unfreeze restores the original song length
unless other content requires a longer song.

This increment supports Session tracks without Channel Rack inputs. Reroute rack
channels and ungroup clips shared with other tracks before freezing. Factory
release envelopes, drum sample tails and reported plugin tails render up to the
existing 30-second host cap. Native plugin behavior and joins still need listening
checks. Rebuilding plugin instances can briefly interrupt playback.

Projects use **format 17** and retain the cache reference and original regions.
Formats 1–15 open unfrozen; format 16 retains its freeze state. Older app versions refuse format 17. **Save portable**
collects cached and original media. Missing caches can be relinked, or unfreeze can
restore the original clips. Caches live in the application's FreezeAudio folder;
unfreeze keeps the file for Undo, and does not delete original media.

### Plugin parameter automation persistence

Project format 17 saves a persistent identity for every instrument/effect and
normalized parameter automation lanes on the timeline. Identities follow FX
reordering and undo; newly inserted or replacement plugins receive fresh IDs.
Older projects acquire IDs on load and preserve them on subsequent saves.
Missing/removed plugins retain orphan lanes so they cannot control a replacement.
The host can enumerate stable parameter-ID bindings for loaded plugins on the
message thread with audio detached. Parameters without persistent IDs are omitted.
This increment provides persistence and bindings; the parameter lane editor,
and playback application remain to be implemented. Send automation is on main through #95.

Formats 1–16 remain supported, including frozen caches and original regions
from format 16. Older readers refuse format 17 rather than discarding targets.
