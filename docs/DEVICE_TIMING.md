# Timing the playback callback on a real audio device (T1-04 D31)

`AlienDeviceTiming` is a command-line tool built with the app. It plays the capacity report's
playback workloads one at a time through a real audio output device, times every callback, and
prints a Markdown table. It is the tool for measurement gate 2 in
[UNIVERSAL_TRACKS.md](UNIVERSAL_TRACKS.md) ("Measure callback time on representative supported
hardware ... at several buffer sizes"), which the offline capacity report
(`RackIntegrationTests --capacity-report`) cannot satisfy on its own.

## Running it

The tool is built next to the app, in `build/AlienDeviceTiming_artefacts/Release/`. Close other audio
apps first, and keep the PC otherwise idle while it runs.

```powershell
build-rel/AlienDeviceTiming_artefacts/Release/AlienDeviceTiming.exe --list
```

```powershell
build-rel/AlienDeviceTiming_artefacts/Release/AlienDeviceTiming.exe > device-timing.md
```

```powershell
build-rel/AlienDeviceTiming_artefacts/Release/AlienDeviceTiming.exe --type "Windows Audio (Exclusive Mode)" --buffers 128,256,512 > device-timing.md
```

| Option | Meaning |
| --- | --- |
| `--list` | List the device types and output devices, then exit |
| `--type NAME` | Device type, as `--list` prints it, for example `ASIO` (default: the system default) |
| `--device NAME` | Output device name (default: the type's default) |
| `--rate HZ` | Sample rate (default: the device's current rate) |
| `--buffers 64,128,...` | Buffer sizes to time, one after another (default: the device's current size) |
| `--seconds S` | Seconds recorded per workload and buffer size (default 5) |
| `--warmup S` | Seconds played before recording starts (default 1) |
| `--workloads 0,1,...` | Which workloads, by number; an unknown option prints the numbered list |
| `--audible` | Send the rendered audio to the device |
| `--inputs` | Also open the first two inputs of the type's default input device, for the input-monitoring and recording workloads (D33-D35, below); without it those workloads are skipped |

**The output is muted by default.** The engine renders everything, and then the device is given
silence, so 16 tracks of test chords do not come out of the speakers. With `--audible` they do.

The default run times all 11 workloads at the device's current buffer size: about 70 seconds. Each
extra buffer size adds as much again. Only one workload plays at a time.

The exit code is 0 when every row was timed, 1 when the device could not be opened or a row failed
(no callbacks arrived, or the workload stopped playing early), and 2 for unknown options.

## What it measures

For each workload and buffer size the tool:

1. builds a fresh engine with the workload set up exactly as the capacity report does
   (`src/CapacityWorkloads.h`, shared by both);
2. attaches it to the open device, which prepares it at the device's rate, as the app does, and
   starts the transport;
3. plays for the warm-up, then records every callback for `--seconds`;
4. checks that everything the row names is still playing and no plugin failed, then detaches it.

| Column | Meaning |
| --- | --- |
| Callbacks | Callbacks recorded |
| p50 / p95 / p99 / max us | The engine's whole callback, timed inside the device callback (nearest rank) |
| p95 / budget % | p95 as a share of one buffer period |
| Over budget | Callbacks that took longer than one buffer period |
| Late | Callbacks that started more than 1.5 buffer periods after the previous one |
| Longest gap us | The longest time between two callback starts |
| Xruns | The driver's own count, where it reports one (`n/a` otherwise; Windows Audio does not) |
| Callback allocations | Heap allocations on the audio thread while recording (must be 0) |

The recorder (`src/CallbackTiming.h`) writes into storage reserved before recording starts: no
allocation, lock or I/O in the callback. Timing adds two clock reads per callback.

If a device does not take a buffer size, the tool says which size it used instead, and times each
size only once.

## What it does not show

- One run is an observation on one machine with whatever else the PC was doing. It is not a
  guarantee, and not a track count anyone can rely on.
- Long gaps and late callbacks include the driver's and Windows' own scheduling, not only the
  engine's work. A long gap without a long callback points at the system, not the engine.
- No plugins except the stock EQ and compressor. Streaming from disk is covered since D32 (below),
  from one file on whatever drive holds the temporary folder. Input monitoring (D33) and recording
  (D34, D35) are covered only with `--inputs`; without it the device is opened with no inputs.

## First results (the development PC, not representative hardware)

Intel Core Ultra 9 288V laptop, Realtek speakers, Windows 11, 16/32 build, 2 seconds per row, muted.
These only show that the tool works; they are not the hardware evidence gate 2 asks for.

| Device type | Rate | Buffer | Workload | p50 us | p95 us | max us | p95 / budget % | Late | Allocations |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows Audio | 48 kHz | 480 | Transport, no clips, 1 track | 61.3 | 88.1 | 219.8 | 0.9 | 0 | 0 |
| Windows Audio | 48 kHz | 480 | Transport, no clips, 16 tracks | 254.2 | 314.7 | 437.1 | 3.1 | 0 | 0 |
| Windows Audio | 48 kHz | 480 | 16 Session MIDI tracks | 376.1 | 509.8 | 700.8 | 5.1 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 256 | Transport, no clips, 16 tracks | 136.3 | 239.1 | 388.9 | 4.1 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 256 | 16 MIDI tracks, stock EQ + compressor | 320.5 | 420.5 | 518.5 | 7.2 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 480 | 16 MIDI tracks, stock EQ + compressor | 586.4 | 732.1 | 856.6 | 6.7 | 0 | 0 |

On this laptop the shared and low-latency modes kept a 480-frame buffer whatever was asked, and
exclusive mode took 256 but not 128. Per frame, 16 idle tracks cost about a third more on the device
(about 0.53 us per frame) than in the offline report on the same machine (about 0.39 us per frame
at 256 and 1,024 frames, D28). The device callback runs on a different thread from the report's,
which the laptop's power management may treat differently; this was not investigated.

## Audio streamed from disk (T1-04 D32)

Workload 11, "16 Session audio clips, streamed from disk", plays the way a long import does: every one
of the 16 tracks launches a Session clip that streams from a WAV file, each through its own decoder,
fed by the engine's disk-stream worker while the device plays.

- **The file** is written by the tool before the first row, into the temporary folder: a quiet
  220 Hz stereo tone, 16-bit, 48 kHz, as long as the warm-up, the recording and 5 s to spare
  (8 s, about 1.5 MiB, with the defaults). It is removed when the tool ends. On a device running at
  another rate it is resampled as it plays, as an import would be.
- **Recording waits for the clips.** A streamed clip starts once its first audio has been read, so
  after the warm-up the tool waits (up to 3 s) until every clip in the row is playing. Every row
  does this; for the others it is already true.
- **The row fails if a stream ran dry or could not be read** on any track (the engine's per-track
  underrun and failure counts, D12), since the clip then played silence where it has audio.
- **The offline report leaves this row out.** It plays blocks back to back, far faster than real
  time, which no disk stream keeps up with; only a device paces callbacks as playback does.

First results, same laptop as above, muted, 4 s per row:

| Device type | Rate | Buffer | Workload | p50 us | p95 us | max us | p95 / budget % | Late | Allocations |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows Audio | 48 kHz | 480 | Transport, no clips, 16 tracks | 109.8 | 168.6 | 273.2 | 1.7 | 0 | 0 |
| Windows Audio | 48 kHz | 480 | 16 Session audio clips (in memory) | 204.2 | 264.8 | 336.7 | 2.6 | 0 | 0 |
| Windows Audio | 48 kHz | 480 | 16 Session audio clips, streamed from disk | 219.1 | 278.4 | 328.0 | 2.8 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 256 | Transport, no clips, 16 tracks | 61.7 | 101.1 | 203.9 | 1.7 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 256 | 16 Session audio clips (in memory) | 105.4 | 135.3 | 277.0 | 2.3 | 0 | 0 |
| Windows Audio (Exclusive Mode) | 44.1 kHz | 256 | 16 Session audio clips, streamed from disk | 118.8 | 159.5 | 249.5 | 2.7 | 0 | 0 |

No stream ran dry or failed, and the audio thread allocated nothing, in either run.

What this shows, on this machine only:
- **Streaming costs the callback a little more than playing the same clips from memory**: about
  7-13 % at p50 (219 vs 204 us at 480 frames, 119 vs 105 us at 256), since each sample is read from a
  voice's ring instead of from the decoded clip. Reading and decoding the file happens on the worker,
  not in the callback.
- **16 concurrent streams kept up** at 256 frames in exclusive mode, the smaller of the two buffers.

What it does not show: a slow, busy or nearly full drive; other files being read at the same time;
more than one file; MP3 sources; rates other than 44.1 and 48 kHz; the 32/64 build. Idle rows ran
at lower absolute times than in the D31 table above, on the same laptop and settings; one run per row
is not enough to say why.

## Input monitoring (T1-04 D33)

Workload 12, "16 tracks monitoring a stereo input", sets every track to monitor inputs 1 and 2 with
the monitor mode "always" (so no track is armed), which plays the input through each track's own
effects and fader as the app does.

- **It needs `--inputs`.** The tool opens no input device unless asked: on most PCs the default input
  is a microphone. With `--inputs` it opens the first two channels of the device type's default input;
  without it the row is skipped with a note on stderr and nothing is counted as failed.
- **With `--audible` as well, the input is played out loud.** A microphone near the speakers will feed
  back; use headphones, or leave the output muted (the default).
- **The row fails if any track metered no input**, which is also what happens when the input device
  has fewer than two channels or delivers only silence.
- **The offline report times it too**, with a 330 Hz test signal on both inputs, since nothing about it
  depends on real time.

First result, on the development laptop: Windows Audio, 48 kHz, 480 frames, output muted, input the
built-in microphone array (2 channels), 4 s per row.

| Workload | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late | Xruns | Allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips, 16 tracks | 115.0 | 163.6 | 196.9 | 222.9 | 1.6 | 0 | 0 | 0 | 0 |
| 16 tracks monitoring a stereo input | 193.3 | 259.1 | 350.4 | 407.5 | 2.6 | 0 | 0 | 0 | 0 |

Every track metered input (the row passed), no callback was late or over budget, and the audio thread
allocated nothing. Monitoring 16 inputs cost about 1.7 times an idle 16-track callback at p50, close to
the offline ratio (about 1.5). With an input open, Windows Audio reported an xrun count (0) where the
output-only runs above report none. One run on one laptop with its built-in microphone; not an audio
interface, and not representative hardware.

## Recording a take (T1-04 D34)

Workload 13, "16 tracks recording a stereo input to a Song take", records the way the Record button
does in the Song view with the song playing: every track is armed on inputs 1 and 2 (monitor
"automatic", so each also plays its input), and the take's audio goes into one queue per track, the
fixed-size lock-free queue the app's recording thread empties to disk.

- **It needs `--inputs`**, like workload 12, and is skipped without it.
- **The queues are emptied every 10 ms** on the tool's main thread, while the device plays, and the
  audio is dropped: what is timed is the callback putting the input into the queues, not the disk.
  A queue holds about 5.5 s at 48 kHz, so a run of any length never fills one.
- **The row fails if a queue overran** (the take would have been cut short), if any track recorded
  nothing, or if the take stopped before the row ended.
- **The offline report times it too**, with the same 330 Hz test signal as workload 12.

First result, on the development laptop: Windows Audio, 48 kHz, 480 frames, output muted, input the
built-in microphone array (2 channels), 4 s per row.

| Workload | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late | Xruns | Allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips, 16 tracks | 111.2 | 146.6 | 197.2 | 236.3 | 1.5 | 0 | 0 | 0 | 0 |
| 16 tracks monitoring a stereo input | 190.0 | 226.8 | 265.1 | 358.0 | 2.3 | 0 | 0 | 0 | 0 |
| 16 tracks recording a stereo input to a Song take | 197.8 | 233.1 | 258.9 | 299.6 | 2.3 | 0 | 0 | 0 | 0 |

The row passed: every track recorded, no queue overran and the take was still recording at the end.
Recording cost about the same as monitoring (p50 4 % more, p95 3 %), no callback was late or over
budget, and the audio thread allocated nothing. The longest gap between callbacks was 12.5 ms against
a 10 ms period, without a late callback (under 1.5 periods); the system's scheduling, not the
callback, which took at most 0.3 ms. One run on one laptop with its built-in microphone.

## Recording a take to disk (T1-04 D35)

Workload 14, "16 tracks recording a stereo input to disk", is workload 13 with each track's take
written to a WAV file as it records, by the app's own recording writer (`DiskRecording`: one queue and
one writer thread per take, with a recovery journal, as the app records). So the callback is timed
while 16 files are being written.

- **It needs `--inputs`**, and is skipped without it.
- **The takes go into a folder in the temporary folder** ("AlienDeviceTiming takes"), with their
  recovery journals in a "Recovery" folder inside it, never the app's own recovery folder. The folder
  and everything in it are removed when the tool ends. At 48 kHz the 16 takes come to about 4.4 MiB per
  second recorded (24-bit stereo WAV, as the app writes them).
- **The row fails if a writer fell behind** (its queue filled and ended the take), if a track recorded
  or wrote nothing, if the take stopped, or if, once the device has stopped calling the engine, a take
  does not finish with everything it recorded written.
- **The offline report leaves this row out**: a writer cannot keep up with blocks played faster than
  real time.

Not measured on a device yet: run it with `--inputs` (muted, or with headphones).
