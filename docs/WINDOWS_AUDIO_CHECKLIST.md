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
