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
- Arrange MIDI, drums and stereo audio with gaps. Compare live playback to exported
  WAV at 60, 120 and 200 BPM. Verify region boundaries, exact ending, Stop/restart,
  mute/solo/pan, cancelled export and unwritable output paths.
- Enable one input, record voice/instrument, Stop & save, launch the captured clip.
  Repeat with stereo inputs carrying distinct signals. Check input meter, source
  channels, saved WAV sample rate and no unrequested monitoring/feedback.
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
