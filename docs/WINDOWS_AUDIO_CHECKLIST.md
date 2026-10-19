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
- Let a take hit 60 seconds; disconnect/reconfigure the input during capture; close
  the main window during capture. Each must stop safely and save captured frames.
  Test an unwritable destination: take stays in memory and Record take retries Save.
- Warp a known-BPM drum loop, vocal and stereo mix to slower/faster project tempi.
  Compare pitch, stereo image, transients and exact duration against the original.
  Exercise cancellation, invalid ratios, 60-second input limit, silence and near-full
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
- Repeat recording/warp/listening checks. Recording and warp input remain capped at 60 seconds.
