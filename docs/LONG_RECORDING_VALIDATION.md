# Disk recording validation

Live Audio lab recording now uses a fixed-memory SPSC capture FIFO and a separate disk writer. The earlier bounded in-memory AudioCapture helper remains for offline tools/tests; it is not the live recorder.

## Local checks

LongRecordingTests passed with the current JUCE 8.0.10 implementation:

- 125 seconds of 48 kHz stereo audio recorded faster than real time, finalized to 24-bit WAV and loaded by the existing Session decoder. Header length and final stereo samples matched the input.
- A simulated one-hour, 8 kHz capture exercised FIFO draining and periodic checkpoints with constant 1 MiB mono buffer storage. Stereo storage stayed at 2 MiB. This uses accelerated input, not a physical hour-long device session.
- The actual engine callback captured with transport stopped, no monitoring, and zero counted C++ heap allocations on the calling thread.
- FIFO wrapping, non-finite sample sanitation, missing-input silence and stopped-device sealing passed.
- Injected writer stalls, write failures and checkpoint failures were surfaced. Overruns stopped at a continuous accepted prefix instead of silently dropping a gap.
- Real partial WAVs and journals retained after injected IO failure were recovered with exact prefix frame counts.
- A handle-owned exclusive lease prevented recovery while a take was active, then allowed recovery after the writer closed. Zero-byte lock files remain to prevent lock-inode races.
- Stale-header recovery, filename collisions preserving existing destinations and odd-byte mono padding passed.
- RF64 header fields and reading a virtual 6 GB WAV through JUCE passed without allocating/writing a multi-gigabyte file. Physical files over 4 GiB remain a manual check.

Existing AudioToolsTests, ToneEngineTests, SessionProjectTests and ArrangementTests also passed after the capture-interface change. Main.cpp passed syntax compilation and the application was linked locally with optimized code and the existing JUCE module objects.

## Boundaries

Recording has no one-minute cap. Disk throughput, free space and filesystem limits still apply. The Session player remains RAM-based: takes beyond 128 MiB decoded audio, or the remaining 512 MiB session budget, stay saved on disk and produce a clear UI message instead of being loaded. Streaming long-file playback is separate work. Tempo-warp input now streams from disk without a 60-second limit; see LONG_TEMPO_WARP_VALIDATION.md.

No physical Windows input/output, multi-hour device recording, real disk-full hardware test or power-loss durability claim is made. Run the updated Windows checklist before treating this milestone as verified on hardware. Recoverable payload can exclude FIFO-resident, partially written or OS-cached samples.

## Repository test

Build normally using README, then run:

```powershell
ctest --test-dir build -C Release -R "LongRecordingTests|AudioToolsTests|ToneEngineTests|SessionProjectTests|ArrangementTests" --output-on-failure
```

Repeat with Debug and review Windows CI on the current pull-request commit.
