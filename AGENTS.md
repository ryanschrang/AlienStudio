# Alien Studio coding agent instructions

Alien Studio is an original, free/open-source Windows 11 DAW written in C++20 with JUCE 8 and CMake.

## Development rules
- Make incremental changes in feature branches and open pull requests; do not push directly to main.
- Preserve the working 16-step prototype and existing CMake configuration.
- Prioritize reliable, real-time-safe audio: no blocking I/O, heap allocation, locks, or logging in audio callbacks.
- Keep MIDI/audio processing separate from the GUI. Use deterministic scheduling and explicit note-off handling.
- Add focused tests for timing, MIDI parsing, voice allocation, and DSP as functionality grows.
- Do not claim runtime functionality solely from a passing compilation.
- Keep all branding, artwork, and code original; check JUCE and other dependency licenses before releases.
- Never commit credentials, tokens, or generated build artifacts.

## Implementation order
1. Polyphonic synth engine: oscillators, velocity, voice stealing, ADSR, filter, and preset scaffolding.
2. MIDI input: device selection, hot-plug handling, note-on/off, velocity, sustain, and safe audio-thread delivery.
3. Drum step sequencer: 16/32/64 steps, tempo-synced transport, kick/snare/hat voices.
4. Piano roll: note creation/move/resize/delete, quantization, velocity, synchronized playback.

## Acceptance criteria
- Windows x64 Debug and Release compile in GitHub Actions.
- New logic has meaningful tests where practical.
- MIDI notes stop correctly and no stuck notes remain after transport stop/device changes.
- Playback is tempo-accurate, with no avoidable real-time thread allocations or locks.
- Pull requests include test instructions and clearly distinguish verified behavior from untested assumptions.
