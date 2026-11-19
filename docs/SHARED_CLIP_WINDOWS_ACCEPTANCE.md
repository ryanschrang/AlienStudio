# Shared clip Windows acceptance

Workstream T1-03 / T1-12, ARCH-01 / QA-03. Related issue #144.

## Scope

Verify shared content across Create, Perform and Arrange, independent overlapping rack occurrences, note ownership, persistence, Undo/Redo, and live-render versus WAV parity.

## Baseline and environment

Baseline is main at 9ae3643 (T1-04 C3 merged). Windows x64 with Visual Studio 18 2026 and MSVC 14.51.36231. A separate exported source snapshot is used for builds because the workspace sandbox prevents creating branch refs in the primary checkout. JUCE 8.0.10 is reused from the existing dependency checkout.

## Automated verification

A fresh Debug build and focused regression tests are in progress. Results will be recorded here with commands before review.

## Windows interactive verification

Pending. Launch through the computer-use integration returned `Computer Use app approval timed out`. No app interaction or visual behavior has been verified. The device inventory query was refused, so interface, driver, sample rate, buffer size and MIDI-controller identity are unverified.

The manual D1/D2/D3 procedures remain in `docs/WINDOWS_AUDIO_CHECKLIST.md`. Record an exact build commit and the device configuration before performing them.

- Launch distinct sources in one scene with Beat and Bar quantization.
- Launch the same source and pitch on two rows, stop one, and confirm the other continues.
- Exercise nudges, ratchets, fractional notes, choke groups and sample voices during replacement.
- Edit linked content from Create, Perform and Arrange, then Undo/Redo. Verify independent copies and Make unique.
- Change the editor selection while playing and confirm the playing source stays selected for playback.
- Overlap Song rack lanes, trim/move/mute/remove/replace one occurrence, seek and loop, and compare a WAV export with playback.
- Save/reopen new and format 26/27 projects, preserving legacy duplicate-source scene behavior.
- Hold notes through stop/panic, transport restart, document replacement and MIDI/audio-device changes.
- Inspect queued/playing row indicators at minimum window size and 150/200% display scaling.

ARCH-01 and QA-03 remain pending hardware and listening acceptance.
