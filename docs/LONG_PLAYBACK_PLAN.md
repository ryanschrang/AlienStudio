# Milestone 1: reliable long-file playback

Baseline: main 8fbc9f4. Current import still rejects audio above 128 MiB decoded;
the session retains at most 512 MiB. Recording/rendering to disk does not remove
these playback limits. The Arrangement timeline runs to 16,384 bars (18 h 12 min at
60 BPM), so hour-long clips can be placed and exported there as well as in Session.

## First increment in this PR

StreamingAudioRing is a fixed-capacity stereo SPSC buffer, with independent read
and write ownership, release/acquire publication, bounded lookahead and explicit
consume. It neither allocates, accesses files, waits nor logs in its methods.
The default sample ring holds 16,384 stereo frames (128 KiB), excluding atomic
indices and alignment. It is a foundation only, not enabled playback support.
Tests cover full/empty boundaries, partial writes, wrapping, stereo lookahead,
independent instances and 200,000 samples across two concurrent threads.

## Next increment

1. Give each active playback instance its own decoder cursor and ring. Sharing a
   clip in several tracks, scenes, undo states or an offline render must not share
   mutable read positions. Keep immutable file metadata separate from playback.
2. Use a bounded shared worker service, with admission limits and clean shutdown.
   Do not start a reader thread for every loaded clip. Prebuffer before a scheduled
   launch. If not ready at the boundary, apply a documented launch policy and
   report it; never silently drop the beginning of a take.
3. Seek/relaunch uses generation-tagged blocks and consumer acknowledgement.
   Never reset ring indices or overwrite memory while a consumer may read it.
   Stop and device replacement must invalidate stale work without joining or
   freeing on the callback thread. Validate 64-bit frame arithmetic.
4. Add large-file metadata loading to import, project reopen, recording recovery
   and tempo-render installation. Preserve bounded decoded drum samples. Track
   streamed-buffer and waveform memory as well as decoded audio memory.
5. Build waveform peaks off-thread and publish immutable cache snapshots.
6. Surface read failures, missing media and underrun counts in the UI. Define
   silence/drop/stop behavior explicitly and test failures without device stalls.
7. Keep offline export deterministic: it may read synchronously on its worker,
   but must not depend on a real-time disk worker outrunning a faster-than-real-time render.

## Required evidence before completion

- Mono/stereo WAV and supported MP3 at 44.1, 48, 96 and 192 kHz.
- Exact interpolation across ring and EOF boundaries; sample-rate conversion.
- Two simultaneous launches of the same file use independent positions.
- Quantized launch, rapid stop/relaunch, seek, scene change and device restart.
- Large recorded, recovered and warped clips load and play; portable save/reopen.
- Missing/truncated source, slow decoder and injected failure produce useful errors.
- No callback heap operations, locks, blocking I/O, decoder calls or destruction.
- Several one-hour sources show memory bounded by configured readers/peaks, not duration.
- Windows Debug/Release CI plus audible 15-minute multitrack playback.
- Arrangement/export agree for supported timeline lengths; explicitly test length limits.

Use examples/README.md as the repeatable human test entry point. Large audio is
generated on demand; do not commit hours of audio or third-party plugin binaries.
