# Alien Studio

**Alien Studio** is an ambitious, free and open-source digital audio workstation (DAW) for Windows 11. Its vision combines the immediacy of FL Studio-inspired pattern production, the flexible clip-based creativity of Ableton Live-inspired workflows, and the recording, editing, and MIDI depth associated with professional DAWs such as Pro Tools and Cubase.

Alien Studio is an **independent, original application**, not affiliated with or endorsed by any of those products. We aim to learn from broadly useful workflows without copying proprietary code, artwork, branding, or protected assets.

> **Project status:** Early prototype. The current code implements a 16-step synthesizer sequencer with play/stop and tempo controls, a 16-voice synth, an on-screen keyboard, and lead/bass/pluck presets. Professional DAW features below are **planned**, not yet implemented. Windows CI compilation is being validated; a successful build and runtime behavior have not yet been confirmed.

## Product vision: three connected workspaces

1. **Production** — Fast beat making and composition with a channel rack, step sequencer, advanced piano roll, patterns, drum machines, samplers, synths, MIDI tools, and automation.
2. **Arrangement** — A multitrack timeline for audio and MIDI recording, clip editing, comping, fades, routing, automation, mixing, and finishing full songs.
3. **Live Performance** — A clip/session grid for launching loops and scenes, improvising arrangements, syncing tempo, and recording performances into the timeline.

The long-term differentiator is **one shared project and audio engine** across all three workspaces: create a pattern, launch it live, and arrange or edit it on the timeline without rebuilding it.

## Roadmap

### Phase 0 — Reliable foundation (in progress)
- [x] Establish C++/JUCE/CMake project and Windows GitHub Actions build workflow.
- [x] Implement initial 16-step tone sequencer, transport controls, and BPM control in source.
- [ ] Obtain a passing Windows CI build and test executable on Windows 11.
- [ ] Add automated tests, crash diagnostics, basic settings, and stable audio-device selection.
- [ ] Select and publish an open-source license after checking JUCE and third-party dependency obligations.

### Phase 1 — Usable music sketchpad
- [ ] Project save/open, undo/redo, autosave, and tempo/time signature.
- [ ] Multiple tracks and patterns; editable step sequencer and piano roll.
- [ ] MIDI note editing, quantization, velocity, and basic keyboard input.
- [ ] Built-in drum sampler and at least one playable synthesizer.
- [ ] Audio/MIDI rendering and WAV export.

### Phase 2 — Full arrangement and recording
- [ ] Multitrack arrangement timeline and clip editing.
- [ ] Audio recording, waveform display, trimming, fades, and basic comping.
- [ ] Mixer with buses, sends, metering, EQ, compression, and effects.
- [ ] Automation lanes and non-destructive editing.
- [ ] Robust audio/MIDI device handling, latency compensation, and project recovery.

### Phase 3 — Live creative workflow
- [ ] Session-style clip and scene launching with synchronized playback.
- [ ] Record clip-launch performances into arrangement.
- [ ] Audio time-stretching/warping and loop synchronization.
- [ ] MIDI controller mapping and performance controls.

### Phase 4 — Professional ecosystem
- [ ] VST3 hosting subject to SDK licensing and compatibility review.
- [ ] Publish an Alien Studio Plugin SDK with versioned APIs, documentation, and example synth, audio-effect, and MIDI-tool projects.
- [ ] Design safe extension boundaries for third-party plugins, including validation, crash isolation where feasible, and permission controls for non-audio extensions.
- [ ] Provide a community plugin manager for discovering, installing, updating, and disabling extensions, with clear provenance and trust warnings.
- [ ] Advanced MIDI tools, routing, comping, editing, and mixing workflows.
- [ ] Additional instruments, modulation, mastering tools, and performance optimization.
- [ ] Optional AI-assisted features where feasible, with clear privacy and licensing controls.

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

Our goal is a fully usable, no-subscription DAW with no feature paywalls. Before distributing binaries or accepting outside contributions, the project needs a clear license, attribution and dependency review (including JUCE), and a contribution policy.

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
The original 16 pads still sequence A3 at sixteenth-note intervals, with half-step
note gates. Stop silences voices and resets the sequence to step 1. The keyboard
can be played while the transport is stopped. Device restarts clear synth voices.
External MIDI device selection/input is a separate roadmap task.

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
Physical MIDI input testing will be required when device input is implemented.
Compilation, synthetic DSP tests, and a GUI smoke test do not verify audible
output, hardware MIDI, or real-time performance on Windows.

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
side effect. New windows start with focus on Play / Stop. Held keys are released on focus changes; keys held through a focus
change or panic must be lifted before they can play again. There is no persistent
keyboard enable/disable setting, and the listener is removed on destruction.

`TypingPianoFocusTests` uses real JUCE components, focus traversal and peer event
dispatch with simulated physical key state. It checks Tab/Shift-Tab, notes from
ordinary controls, octave-button activation without focus stealing, octave repeat
suppression, native letter-code casing, editor/combo/shortcut exclusion, chords,
blur release and listener
teardown/recreation. On Linux, run the GUI test with a display and window manager (or initialize
the usual WM atoms in a bare Xvfb session), then run
`ctest --test-dir build --output-on-failure`. Windows CI runs all tests
in both Debug and Release. These are event/focus simulation tests, not native
Windows key injection or an audible hardware playback test.

Manual Windows check: play A/S/D/F, Tab to a button, then play again. Repeat using
Shift-Tab; both directions must move focus and notes must still work on ordinary
controls. Tab to Octave Up and activate it with Space: focus stays on the button,
and the next A uses the new octave. Type in the tempo field and preset selector:
no notes should play there. Tab back to a button and play a chord. While holding
it, Tab or Alt-Tab away; notes/highlights must clear. Release the keys, return and
play again. Repeat after closing/reopening the app, and check mouse notes,
presets, transport and the 16-step sequencer.
