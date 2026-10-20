# Disk-stream worker increment

Stacked on PR #40. This increment implements the decoder/voice service behind
STREAM-02; it does not yet enable large Session imports or replace the current
player. STREAM-03 and launch integration remain required before users can play
clips above the decoded-memory budget.

## Ownership and bounds

DiskAudioFile is immutable sample-rate/channel/frame metadata. JuceDiskAudioFile
inspects WAV/MP3 headers and opens a fresh JUCE decoder for every playback voice.
No decoded whole-file vectors or threads are created per imported source.

DiskAudioStreamService has one worker, 32 admitted endpoints (two available per
track for active/pending playback), and fixed 8192-frame tagged rings. Each tag
contains a 64-bit generation and source-frame position. On typical x64 builds
sample rings total 6 MiB; reader/codec internals and retained source metadata are
additional. Reads are limited to 1024 frames and readiness requires 2048 frames
or source EOF. One service is intended per engine, not per loaded clip.

The control thread calls retain() before publishing source pointers. The service
owns retained sources through worker shutdown. It releases readers before source
metadata. Destruction joins the worker and belongs on a control thread after
callback detachment. Retain is control-thread-only and must not race destruction.

Each endpoint has one audio-side owner, and the worker alone owns its reader.
Request commands use a three-buffer SPSC mailbox. Neither side resets ring
indices. A consumer can release unread old frames on request; subsequently
published obsolete frames carry their old generation and are safely discarded.
Old decoding results are also checked against the latest request before publication.
A new request supports 64-bit seek positions. Two voices sharing a source retain
independent reader positions and cancellation/restart state.

request(), ready(), failed(), sample() and releaseBefore() never perform decoding,
file I/O, allocation, waits or locking. sample returns false without modifying
output when data is unavailable. It never advances song time. The upcoming Session
integration must choose an explicit late-launch and underrun policy; it must not
silently skip the start of a file. Null requests stop a voice. Invalid positions,
open failures, failed reads and exceptions set a generation-specific failure flag.

## Verification added

DiskAudioStreamTests covers:
- independent concurrent playbacks of one virtual source;
- stereo interpolation, bounded decoder reads, repeated ring wrapping;
- zero caller-thread allocations in request/sample/release;
- source lengths beyond 32-bit counters and seeking to a five-billion-frame tail;
- superseded requests, rapid restart, stop and invalid seek;
- open/read failure, decoder exceptions, recovery on a fresh generation;
- one-frame mono EOF and non-finite sample sanitation.

DiskAudioFileTests writes and opens real stereo PCM WAV data, checks independent
decoders at different positions, validates sample order and read bounds, then
removes the file and checks missing/malformed source errors. A real MP3 decode,
Windows codec behavior, slow-disk fairness, listening tests and shutdown during a
blocked OS read still require platform validation. A decoder that blocks in OS
I/O can delay all voices in this single-worker increment; do not call that case
verified or claim deadline guarantees.

Local builds were unavailable because the execution environment was disconnected.
Check Windows Debug/Release CI on the exact PR commit. Existing ring, project,
audio and engine tests remain part of the suite.

## Next integration

Bind two endpoints to each SessionAudioPlayer; prebuffer a pending clip while the
current one continues, then swap at an accepted launch boundary. Register immutable
file sources off-thread, load large clips without whole-file allocations, create
background waveform caches and surface errors in the UI. Offline export must use
an explicit deterministic read path rather than depending on real-time prebuffer
speed. Current Session import and arrangement duration limits remain unchanged.
