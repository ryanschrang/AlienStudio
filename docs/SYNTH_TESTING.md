# Polyphonic synthesizer milestone (issue #3)

## Behavior

The JUCE audio-device callback now owns a 16-voice synth. The portable DSP core is separate from the GUI and shares the same processing path with headless tests. It uses sine and PolyBLEP saw/square oscillators, per-voice velocity and ADSR, a basic one-pole preset low-pass filter, and bounded output saturation. Lead, Bass and Pluck settings are captured at note-on; changing presets leaves existing voices on their original settings.

Allocation order is existing key (retrigger), first free voice, oldest released voice, then oldest held voice. Retriggering the same channel/key replaces its previous gate rather than stacking note-ons. A 32-sample residual fade softens steals; it is not a guarantee of inaudible stealing. A pluck held after decay remains allocated until note-off or stealing.

The original 16-step sequencer still runs at quarter-beat intervals. Enabled steps trigger A3 (220 Hz) on MIDI channel 1 with an explicit half-step gate. Audition keys use channel 2; the C-major chord uses channel 3. These inputs do not release each other's notes. Transport Stop immediately silences all voices, clears pending notes and resets the next start to step 1. The All notes off button also clears voices; an active sequencer resumes triggering at subsequent steps. Device stop/reinitialization clears stale voices and commands.

Hold an on-screen note or **Hold C major** to audition while stopped or playing. Release the mouse to send note-off. The velocity slider affects subsequent notes. Hardware MIDI devices, sustain pedal, pitch bend, sample-accurate external MIDI timestamps and computer-keyboard performance mapping are intentionally deferred to the MIDI-input milestone. The engine accepts MIDI note-on/off, zero-velocity note-on, CC120 and CC123; this is not a complete MIDI implementation.

## Real-time ownership

- GUI writes lock-free bool/int atomics and a fixed-capacity single-producer/single-consumer ring. Only the message thread may produce ring events. A future hardware MIDI callback must have its own safe delivery mechanism; it must not become a second producer.
- The audio thread drains at most 128 commands per block and owns all oscillator, envelope, voice and transport state. Queue overflow discards pending commands and panics, preventing a dropped note-off from leaving a stuck note.
- No heap allocation, locks, file access or logging occurs in application callback code. Static assertions require lock-free control atomics. A headless allocation guard exercises rendering, triggering, stealing and release. This does not prove OS/driver real-time behavior or catch allocations internal to external audio drivers.
- Sequencer timing carries fractional sample positions across blocks. Tempo changes take effect for newly scheduled steps. GUI commands are applied at block boundaries, so audition latency depends on device buffer size.

## Automated tests

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

For headless DSP development without downloading JUCE, configure with `-DALIEN_BUILD_APP=OFF`. `BUILD_TESTING` defaults to ON. The existing Windows CI matrix builds the app and runs these tests in both configurations. Test checks remain enabled in Release; they do not rely on `assert`.

Seven focused test groups cover ADSR transitions/early release, 16-voice allocation/stealing, MIDI parsing/channel isolation, waveform frequency/bounds, preset velocity response, concurrent ring publication/overflow, transport stop/restart/device reset, exact output across block sizes and application callback heap allocation.

## Hands-on testing still required

Passing tests and CI builds do not confirm audio-device behavior or listening quality. On a Windows 11 machine:

1. Launch Debug and Release builds with the default output device; confirm startup errors are shown if initialization fails.
2. Play the default four active steps. Change tempo and toggle steps; verify the 16-step pattern and displayed position. Stop during a note, then restart at step 1.
3. Select each preset and hold/release every audition key and the chord. Compare low/high velocity. Listen for stable pitch, expected envelope tails, acceptable aliasing/clicks and output level.
4. Audition while the sequencer is running. Stop while holding a note, release it and play again. Check All notes off during playback and while stopped. No note should remain stuck.
5. Test 44.1/48/96 kHz and several buffer sizes using the system/device configuration. Disconnect/reconnect or change the default output device while sounding notes; verify recovery and no stale voices. This prototype does not yet expose device selection.
6. Run a prolonged session and measure underruns/CPU using suitable audio tools. Check rapid GUI interaction and high-DPI layout.
7. Hardware MIDI input cannot yet be tested end-to-end: there is no device input UI/backend in this issue. Repeat note-on/off, repeated-key, channel and device-loss tests with real MIDI hardware when that stage is integrated.

Local environment: no C++ compiler/CMake is available in the current task environment. Build and automated-test evidence must come from the PR's Windows CI; hands-on audio/MIDI testing remains unverified.
