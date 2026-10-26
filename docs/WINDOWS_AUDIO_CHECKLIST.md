# Windows audio verification

Status: NOT RUN on physical Windows hardware for this change.
Automated offline rendering verifies generated samples, not an audible output,
USB driver behavior, latency, dropout rate, or GUI appearance on Windows.

Record: commit/build, Windows version, audio interface, driver type/version,
sample rate, buffer size, MIDI controller, display scale, and pass/fail notes.

1. Launch with a working default output. Confirm audible sound and correct device.
   Separately test PR #22 with an unavailable output: a useful warning appears;
   close during startup and verify no crash. Never call it verified before merge
   and hardware testing.
2. Create equal-pitch clips on two Session tracks. Choose Lead and Bass, pan them
   left/right, launch together, and confirm distinct sounds in the two speakers.
   Add chords exceeding 16 notes to one track; the other must keep sounding.
3. Change volume/balance during playback. Listen for clicks and dropouts. Mute,
   unmute, solo one track, solo two, and mute a soloed track. Confirm meters follow
   track output. Live keyboard input intentionally bypasses Session mute/solo.
4. Change a track preset: held notes finish with their old patch; new notes use
   the new sound. Stop, switch mode, Alt-Tab while holding keys, disconnect MIDI,
   and restart the audio device. Check for stuck notes and recoverable playback.
5. Import stereo WAV/MP3; confirm balance/mute/solo also affect audio clips. Clips
   are one-shots; they must not be presented as tempo-warped loops.
6. Save/open a v3 project with different settings on all tracks. Open an older
   v1/v2 project; each track inherits its saved global preset. Confirm note data.
7. Save portable into a new project folder. Close the app, move that whole folder,
   temporarily rename the original source-audio directory, and reopen the copy.
   Confirm every clip plays. Save normally and reopen again. Restore original
   directory names afterwards. Do not delete the user's source recordings.
8. Test missing source media and a non-writable destination. Collection failure
   must leave any existing project usable. Test names with spaces/Unicode and
   different source files with identical filenames.
9. Check Mixer and top-bar controls at 960x650 and 100%, 150%, 200% display scales.
   Tab/text entry must work without accidental notes; controls must remain usable.
10. Listen for at least 15 minutes across representative sample rates/buffers,
    noting underruns, CPU use and audible defects. Log failures with exact steps.
11. Missing audio: import a WAV into a Session clip and load a drum-pad sample,
    Save, close, then rename the folder holding those files and reopen. The project
    must open; the clip tile must be dark red with the file name, the status line must
    say audio is offline, and the clip/pad must be silent (no crash, no noise).
    Press Relink audio, choose the renamed folder, and confirm the clip and pad play
    again. Undo once: both return to offline. Save while offline and reopen: the
    original paths are still remembered. Try a folder with two same-named files in
    different sub-folders and confirm the one in the matching folder layout wins.
    Save portable while audio is offline: it must refuse and name the file.
12. Master volume and gain staging: play a busy project and a project saved with
    an earlier build; built-in sounds and audio clips should be about 6 dB louder
    than before and not distorted at unity. Sweep the Master fader 0-400% while
    playing (listen for clicks), double-click it to return to 0 dB, and confirm the
    master meter's red clip light comes on when the mix is pushed hot and that
    the output then sounds soft-clipped rather than harshly chopped. Save, reopen
    and confirm the fader position is restored; Undo/Redo it; export a WAV and
    confirm the file level matches what you heard with the fader at that position.
13. Count-in and metronome: with playback stopped, press Record on an empty clip
    with the default one-bar count-in. Four clicks should sound, the first one
    higher; playback of a clip with a note on beat 1 must then start exactly on the
    beat after the fourth click (no gap, no early start). Repeat with Off and 2
    bars, at 60 and 200 BPM, and cancel with Play/Stop during the count-in. Switch
    the metronome on while a clip plays: clicks follow the tempo, accent every bar,
    ignore the Master fader, and are absent from an exported WAV and from an Audio
    lab take. Try Low/Medium/High.
14. Latency compensation (the one check that needs your ears): on an empty clip
    with the metronome on, record about eight bars of steady quarter notes on the
    click with a USB MIDI keyboard, once with the typing piano. Open the Piano Roll
    and zoom in: notes should sit on the beat lines within roughly one audio block
    (about 11 ms at 48 kHz). If every note is consistently early or late, use
    Record options > Latency compensation (positive = earlier) and repeat; write
    down the device, driver (WASAPI / ASIO), sample rate, buffer size and the
    offset that was needed. A different device or buffer size needs a new value.
15. Monitoring: while recording, the notes you play should sound through the
    selected track's own instrument (try a synth preset, a drum-sampler track and a
    VST3 instrument track), follow that track's volume/mute, and stop cleanly when
    the take ends, even with a key still held. Outside a take the Device rack synth
    should be used again.
16. Overdub, replace and undo: record a second layer over a clip that has notes
    (existing notes keep playing and stay in the clip), then record in Replace mode
    (old notes silent during the take, gone afterwards) and Undo each: the clip
    returns to exactly how it was in one step. Also: stop playback mid-take, start a
    New project and Open another project mid-take, change tabs mid-take, unplug the
    MIDI keyboard with a key held, record into a clip that is not playing (it should
    launch on the next bar), and record more than 1024 notes (status bar should say
    the clip is full). Nothing should hang, stick, or change the wrong clip.

17. Stock effects, listening: add each stock effect from Plugins > stock FX to a track
    and to Master FX. EQ: sweep each band's gain and frequency with the mouse while
    playing; no clicks or zipper noise; the curve matches what you hear (boost
    a band at a known frequency on a tone). Compressor: with a drum loop, lower the
    threshold until the meter moves; attack/release should change the punch audibly;
    Mix at 50% should keep the transients. Reverb: tail should fade smoothly, Size and
    pre-delay audible, and an exported WAV must include the whole tail. Delay: free time
    and Tempo sync at several BPMs (change tempo while playing); ping-pong alternates
    ears; feedback at maximum stays stable. Limiter: push Gain up on a loud mix; the
    output meter must never pass the ceiling, including in the exported WAV.
18. Stock effects, project and rack behavior: put several effects on a track, Move FX
    up/down, Bypass, Remove and Undo each; Save, close, reopen and confirm every
    knob position returned. Save portable and reopen. A track with a limiter must stay
    in sync with the other tracks (play two identical clicks, one with the limiter);
    exported WAV length and alignment must match what you heard. Open the editor of
    two effects at once and move knobs while playing.
19. Stock effects, load: add eight effects to each of several tracks (reverbs and delays
    are the heaviest) and watch CPU and dropouts at 44.1/48/96 kHz and small buffers.
    Change the audio device's sample rate with effects loaded and confirm the delay
    times, reverb size and limiter latency still sound and line up correctly.
20. Channel Rack, feel: open the Rack view. Paint a four-on-the-floor kick, a clap on 2 and 4
    and eight hats while it plays (Play button). Painting, erasing and dragging across rows must
    feel immediate, the playhead must stay on the beat at 60/120/200 BPM, and clicking a name
    must sound at once even while stopped. Try 16/32/64 steps at 100%/150%/200% display scaling
    and the minimum window size; 16 channels must scroll and nothing may overlap.
21. Channel Rack, sound: compare each built-in sound (kick, snare, clap, closed/open hat, tom, rim,
    cowbell) at velocities 20/64/127; draw velocity, pan, pitch and chance with the graph and
    listen for the right result; chance should vary hit to hit. Set swing to 0/33/66% and
    confirm every second sixteenth moves later. Closed and open hat in the same choke group
    must cut each other; mute, solo and volume 0 must be silent without clicks.
22. Channel Rack, routing: send a channel to Track 1 and confirm that track's fader, pan,
    mute, solo, meter and FX affect it (and the master does not double it); route to the master
    again. Delete or reorder that track and confirm the channel moves with it or falls back
    to the master. Use a VST3 instrument track as the destination and confirm both the
    instrument and the rack are heard.
23. Channel Rack, samples and files: load a WAV and an MP3 into channels (long, mono and
    stereo, 44.1 and 48 kHz) and confirm they play at the right speed. Save, close and reopen
    (also Save portable). Rename or move the sample file, reopen: the channel should be silent
    and labelled Missing; Relink audio should repair it. Undo/redo through steps, knobs,
    patterns, channel moves and sample loads.
24. Channel Rack, transport and safety: switch tabs, stop and start while hits are ringing,
    change patterns and lengths while playing, edit steps while playing, and change the audio
    device and its sample rate. Nothing may stick, click badly, hang or play on after Stop;
    note any one-block glitch when moving channels or changing sounds during playback.
    Confirm that the rack is silent in Song mode and in Export WAV unless patterns have been
    placed on the Arrangement timeline's pattern lanes (items 25 to 34).
25. Arrangement timeline, placing: on the Song tab, double-click empty spots in several track
    lanes and in a pattern lane; right-click an empty spot and pick a clip or pattern; use
    Place... Confirm each lands where clicked, on the grid, with the right colour, name and
    contents (notes, waveform or steps). Place past the end of the song: the song must grow.
    Open the clip by double-click (Piano Roll, Session, Rack) and edit it: every placed copy
    must change with it.
26. Arrangement timeline, mouse editing: move, resize, trim the left edge, Ctrl-drag copy and
    drag a pattern clip between lanes; Alt must ignore the grid. Draw selection boxes in empty
    space (also with Shift held), Shift/Ctrl-click and Ctrl+A. Everything under the pointer must
    follow it without lag at 100%/150%/200% scaling and the minimum window size, and every
    drag must undo and redo as one step.
27. Arrangement timeline, keyboard: Delete, Ctrl+X/C/V/D/A/G, Ctrl+Shift+G, S or Ctrl+E,
    M, L, K, arrows (with and without Shift and Ctrl), Home/End, Space, +/-, Ctrl+0.
    Confirm the typing piano does not sound while the timeline has focus for these keys, that
    typing in a text box (e.g. renaming a marker) is not hijacked, and that Ctrl+Z/Y/S/O/N
    still work.
28. Arrangement timeline, zoom and scrolling: wheel, Shift+wheel, Ctrl+wheel (zoom keeps the
    point under the pointer), +/-, Fit song, scroll bars and trackpad gestures with a 4-bar,
    64-bar and 512-bar song (and one of several thousand bars) and 1 to 16+ tracks. The ruler, clips, markers, loop strip and
    playhead must stay aligned and legible, with no flicker or runaway scrolling. Follow
    must keep the playhead in view during playback and let go when you scroll yourself.
29. Arrangement timeline, playback from anywhere: click the ruler at different positions while
    stopped and while playing; press Play and Stop and Play again. Playback must begin at the
    cursor (stopped) or jump there at once (playing) with no stuck notes, hung hats or
    dropouts. Start in the middle of a held MIDI note, a long audio clip and a pattern clip
    that begins before the cursor: audio and notes must be right (notes already started are
    not expected to sound), and the rack must stay on the beat.
30. Arrangement timeline, loop range: set the loop by dragging, move it, resize each end,
    double-click for one bar, turn it on and off with L and the Loop button while playing.
    The wrap point must be gapless and in time with the metronome at 60/120/200 BPM, with
    notes, audio and rack patterns restarting correctly and nothing hanging over the wrap.
31. Arrangement timeline, markers: add (K, double-click, menu), rename, move, recolour,
    delete, jump with Ctrl+Left/Right and click-to-start. Names must be clipped, never
    overlap the ruler, and survive save and reopen. Add well over 30 markers.
32. Arrangement timeline, rack on the timeline: layer patterns on two and four pattern
    lanes, change a pattern's steps while the song plays, send channels to Tracks, mute/solo
    tracks, and compare live playback to Export WAV (whole song and loop range) at 60/120/200
    BPM. Level, timing, swing, choke groups and tails after the last clip must agree.
33. Arrangement timeline, edits while playing: move, delete, split, paste, nudge, mute, change
    the song length, undo and redo during playback. Playback must continue without a restart
    or a stuck note; note any one-block glitch.
34. Arrangement timeline, projects: save and reopen (also Save portable) a song with every
    kind of clip, a loop and markers. Open a project made with the earlier bar-based
    arrangement (v5 to v7): each old region must appear as a clip at the same bar and sound
    the same. Delete a track or scene that has clips on the timeline, then undo.
35. Arrangement timeline, long streamed audio: place a clip too long to decode (over
    128 MiB, e.g. a one-hour WAV) on the timeline, split it, trim its front and place it more than
    once. Press Play at the start, from the middle of a region and just before one begins, and
    jump around with the ruler. Audio must start on time and in sync with other tracks, with no
    dropouts or stuck audio; stop and restart often. Then Export WAV (whole song and a loop range)
    and compare it with live playback; cancel an export; rename the source file and export again
    (it must stop with a message before writing anything).
36. Song tab and Song mode: check the top bar at 960x650 and at 100/150/200% scaling: six view
    buttons (Steps, Piano Roll, Session, Song, Mixer, Rack), Song mode and More... all readable,
    the clock still shows bar, beat and tempo. Place a drum pattern, a MIDI clip and an audio
    clip in the Song. Turn Song mode on and press Play from Steps, Piano Roll, Session, Rack and
    Song: the same song must play from each, in time, and the clock must follow the song. Switch
    views and toggle Song mode while playing: no stuck notes or hung hats. Turn Song mode off:
    each view plays its own pattern or clips again. With an empty Song, press Play in Song mode
    and confirm the status bar explains why it is silent. Press Record with Song mode on: it must
    turn off and the take must record as before.
37. Song clip list and new clips: open the Song. Drag a Channel Rack pattern from the list to a
    pattern lane, and a Session clip onto the lanes (it must land on its own track whichever lane
    the pointer is over), at several zoom levels and with Snap on and off; watch the ghost and the
    help line. Drop outside the lanes and press Esc during a drag: nothing is placed. Double-click
    a row: it goes to the play position. Double-click a lane of a track with no clips: a new empty
    four-bar clip appears; double-click it, write notes in the Piano Roll, return to the Song and
    play it. Make clips until the Session grows a scene (look at the Session view), then undo: the
    clip and the scene must go together. Hide and show the list with Clips at 960 x 650.

## Results

No physical-device results supplied yet. Fill in the environment and actual
observations above before marking this milestone hardware-verified.

## Drum sampler addition (hardware results still NOT RUN)

Load samples into two tracks, audition all eight pads, and sequence pitches 36–43
at different velocities. Confirm pads 3/4 choke one another, repeated hits overlap,
and empty pads remain silent. Route a USB controller to each kit, release notes,
change route, disconnect, Stop and panic; no old sample should remain playing.
Save portable, move its folder, reopen, and verify kit samples and instrument
selection. Check the browser and pad buttons at the minimum window size.

## Arrangement and Audio lab (hardware/UI results still NOT RUN)

- Check track-colored regions and Audio lab cards at 960x650 and 100/150/200%
  scaling. Check focus, text readability, selected pad/region states and meter.
- Arrange (on the Song tab) MIDI, drums and stereo audio with gaps. Compare live playback to exported
  WAV at 60, 120 and 200 BPM. Verify region boundaries, exact ending, Stop/restart,
  mute/solo/pan, cancelled export and unwritable output paths.
- Enable one input, record voice/instrument, Stop & save, launch the captured clip.
  Repeat with stereo inputs carrying distinct signals. Check input meter, source
  channels, saved WAV sample rate and no unrequested monitoring/feedback.
- Song view: enable several inputs in Audio settings (an interface with 4+ inputs if available). Arm tracks on
  different mono inputs and a stereo pair; confirm each header meter follows only its own input and that
  Auto/In/Off monitoring behave as labelled. Measure round-trip monitoring latency at 64, 256 and 1024-sample
  buffers. Monitor a microphone through speakers at low volume only: check that Off and disarming stop it at once.
  Disable an input a track uses and confirm the header warns and the track stays silent.
- Song view takes: arm two tracks (one mono, one stereo) and press Record from stop with a one-bar count-in;
  play along with a click-heavy part, stop, and confirm both takes appear on their lanes in time with the song
  (play them back against the original). Measure the offset of a loopback cable (output to input) take at 64,
  256 and 1024-sample buffers; it should be within a millisecond or two, otherwise note the reported device
  latency and the Audio input latency offset that fixes it. One Undo removes both takes; Redo brings them back.
  Turn on punch, set the loop markers, start two bars earlier and confirm only the loop range is recorded and
  the take stops at the punch-out while the song plays on. Change the audio device during a take and close
  the app during a take: the captured audio must be kept (placed, or left on disk for Recover takes).
- Record past 60 seconds and manually stop; disconnect/reconfigure the input during capture; close
  the main window during capture. Each must stop safely and save captured frames.
  Test an unwritable destination: preparation fails before capture. A mid-take write failure retains the partial WAV for Recover takes.
- Warp a known-BPM drum loop, vocal and stereo mix to slower/faster project tempi.
  Compare pitch, stereo image, transients and exact duration against the original.
  Exercise cancellation, invalid ratios, long source files, silence and near-full
  session memory. Reimport the original before comparing another ratio.
- Save portable with recorded/warped clips, move folder, reopen and export.
  Changing project tempo must not claim to live-warp already rendered audio.


## Integrated VST3 hosting (manual checks still required)
- Scan one installed x64 instrument and one stereo FX; verify the cached list on restart.
- Scan an invalid/crashing bundle; verify the main application survives and logs the failure.
- Sequence the instrument from Session and Arrangement; route external MIDI from Drums.
- Open/close native editors, move/bypass/remove FX, test master FX and transport stop.
- Save/reopen including native parameter changes; test missing-plugin placeholders and undo.
- Compare live playback with exported WAV, including latency-reporting FX and reverb tails.
- Test 44.1/48/96 kHz, varying device buffer sizes, and input/output device changes.
- Save portable, remove original source audio, undo/redo, save/reopen; verify collected media.
- Start an autosave then New/discard/quit; verify discarded recovery does not reappear.
- Repeat recording/warp/listening checks. Recording has no one-minute cap; tempo warping also streams long source files from disk.


## Long disk recording
- Record mono/stereo for at least 5 and 30 minutes at 44.1/48/96 kHz; check duration and beginning/end audio in another editor.
- Monitor process memory: capture storage should stay constant while the WAV grows.
- Test disk full, permission failure, a slow/removable drive and input-device stop; inspect the saved prefix or retained partial file and error message.
- Interrupt the app during a test take, restart, choose Audio lab > Recover takes; inspect recovered duration and channels. Test an existing destination collision without replacing it.
- Record beyond the 128 MiB decoded-clip budget; verify the WAV stays saved and the UI clearly reports that it was not loaded into Session.
- Test a take beyond 4 GiB on a suitable filesystem and verify RF64 compatibility. This physical long-duration case has not been verified locally.

## Long tempo warping
- Choose a recording longer than one minute directly in Audio lab > Render tempo match; render at 0.5x, 1x and 2x duration ratios. Check duration, pitch, stereo image and grain-boundary clicks.
- Render a source/result beyond Session decoded-memory budgets. Confirm the WAV saves with a clear disk-only message and capture/render working memory stays bounded.
- Cancel midway; confirm no final WAV or abandoned partial/journal remains. Try an existing target, a missing source, and a full/slow drive; preserve all source and existing destination audio.
- Confirm RF64 long sources/results work in Windows and an external editor. Physical multi-hour rendering and listening remain manual checks.

## Song view fades and crossfades
- Drag the fade handles of an audio clip with a real mouse at 100% and 150% display scaling; check the handles are easy to grab and do not get in the way of trimming.
- Listen to fade ins/outs and to a one-beat and a one-bar crossfade between two different recordings: no click, dip or level jump at the join. Repeat with a long (streamed) recording as the first clip.
- Start playback inside a crossfade, loop over one, and export a WAV containing one; compare the export with what was heard.

## Song tempo changes
- In the Song view, double-click the ruler at bar 5 and enter 90; add another change at bar 9 (140). Play from bar 1 with the metronome on: the click and every MIDI, drum and Channel Rack part change speed exactly on those bars, with no stuck or doubled notes.
- Place a recorded vocal or long audio clip across a change: it plays at its own speed with no jump, click or pitch change; a clip placed after the change starts on its bar.
- Loop a range that contains a change and listen to several passes; seek and start playback inside the faster and slower sections.
- Record a take across a change with a count-in; check it lines up with what was played.
- Export a WAV and stems of a song with tempo changes and compare them with playback; freeze a track, then add a tempo change and check it is unfrozen.
- Load a VST3 tempo-synced effect (a synced delay or LFO) and check it follows the new tempo after each change.
