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
| `--plugin FILE.vst3` | A VST3 effect to put on every playing track, for the VST3 workload (D36, below); without it that workload is skipped |
| `--instrument FILE.vst3` | A VST3 instrument to play every playing track's MIDI, for the VST3 instrument workload (D37, below); without it that workload is skipped |

**The output is muted by default.** The engine renders everything, and then the device is given
silence, so 16 tracks of test chords do not come out of the speakers. With `--audible` they do.

The default run times every workload at the device's current buffer size, about 6 seconds each
(17 in the 16/32 build, 31 in a 32/64 build, D38). Each extra buffer size adds as much again. Only one
workload plays at a time.

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
- Outside plugins only with `--plugin` and `--instrument` (D36, D37, below), one effect and one
  instrument at a time, loaded in the tool's own process with none of the app's crash protection. Streaming from disk is covered since D32 (below),
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

First result, on the development laptop: Windows Audio, 48 kHz, 480 frames, output muted, input the
built-in microphone array (2 channels), 4 s per row, the temporary folder on the laptop's system drive.

| Workload | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late | Xruns | Allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips, 16 tracks | 119.9 | 156.1 | 187.1 | 276.8 | 1.6 | 0 | 0 | 0 | 0 |
| 16 tracks recording a stereo input to a Song take (queues only) | 199.5 | 241.0 | 261.7 | 402.2 | 2.4 | 0 | 0 | 0 | 0 |
| 16 tracks recording a stereo input to disk | 189.7 | 223.7 | 262.3 | 356.5 | 2.2 | 0 | 0 | 0 | 0 |

The row passed: no writer fell behind, and every take finished with all it recorded written. Writing
16 files did not make the callback slower than recording into queues alone (the two rows are within
the spread between runs), which fits the design: the callback only fills each take's queue, and the
16 writer threads do the disk work. No callback was late or over budget, and the audio thread
allocated nothing. The folder was removed afterwards. One run on one laptop, on its system drive; a
slow or busy drive is where a writer could fall behind.

## A VST3 effect on every track (T1-04 D36)

Workload 15, "16 Session MIDI tracks through a VST3 effect", is workload 9 with an outside plugin in
place of the stock EQ and compressor: one instance of the VST3 given with `--plugin` in each playing
track's first effect slot, prepared at the device's rate like any slot.

- **It needs `--plugin FILE.vst3`**, and is skipped without it. The tool is built with VST3 hosting
  for this; the offline report's build is not, so the report leaves the row out.
- **The plugin runs inside the tool's process.** The app's crash containment and blocklist are not
  involved, so a plugin that crashes takes the tool with it. Use plugins you trust.
- **The row fails if any plugin faulted** (the runtime's fault count), as every row does.

First result, on the development laptop (Windows Audio, 48 kHz, 480 frames, muted, 4 s per row), with
the repository's own test plugin (`Alien Test Plugin`, a simple gain/tone/mix effect built with the tests):

| Workload | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late | Allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips, 16 tracks | 110.5 | 176.7 | 244.3 | 347.2 | 1.8 | 0 | 0 | 0 |
| 16 Session MIDI tracks | 309.0 | 503.4 | 549.6 | 672.9 | 5.0 | 0 | 0 | 0 |
| 16 Session MIDI tracks, stock EQ + compressor | 545.1 | 745.9 | 871.8 | 1040.5 | 7.5 | 0 | 0 | 0 |
| 16 Session MIDI tracks through a VST3 effect | 402.0 | 576.7 | 645.0 | 760.7 | 5.8 | 0 | 0 | 0 |

16 instances of the test plugin added about 93 us a callback at p50 over the same tracks without it,
about 6 us an instance per 480-frame block, and no plugin faulted. The zero allocations are the tool's
own: the plugin's DLL allocates through its own runtime, which the count does not see (D37). The test plugin does almost
nothing, so this is the cost of hosting a VST3 (its bus and parameter handling), not of a real effect.
Not shown: real third-party plugins, instruments, plugins reporting latency, more than one plugin per
track.

## A VST3 instrument on every track (T1-04 D37)

Workload 16, "16 Session MIDI tracks playing a VST3 instrument", is workload 5 with an outside instrument
in place of the built-in synth: one instance of the VST3 given with `--instrument` in each playing
track's instrument slot, playing that track's Session clip (a held three-note chord).

- **It needs `--instrument FILE.vst3`**, and is skipped without it. The tool loads the first plugin in
  the file that says it is an instrument, and refuses a file with none.
- **The plugin runs inside the tool's process**, as with `--plugin`: no crash containment.
- **Callback allocations count the tool's own allocations only.** A plugin's DLL allocates through its
  own C runtime, which the count does not see, so 0 here says nothing about the plugin.
- **The row fails if any plugin faulted**, as every row does. It does not check that each instance
  sounded.

First results, on the development laptop (Windows Audio, 48 kHz, 480 frames, muted, 4 s per row), each
instrument with the patch it loads with:

| Workload | p50 us | p95 us | p99 us | max us | p95 / budget % | Over budget | Late |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips, 16 tracks | 101.6 | 163.9 | 203.9 | 212.8 | 1.6 | 0 | 0 |
| 16 Session MIDI tracks (built-in synth) | 290.1 | 431.4 | 509.9 | 547.3 | 4.3 | 0 | 0 |
| 16 tracks playing Surge XT | 1277.5 | 1586.2 | 1862.9 | 5600.8 | 15.9 | 0 | 0 |
| 16 tracks playing Vital | 2981.7 | 3609.0 | 4117.8 | 4383.9 | 36.1 | 0 | 0 |

16 instances of a real synth cost 4 to 10 times the built-in synth's whole callback on the same tracks:
about 62 us (Surge XT) and 168 us (Vital) an instance per 480-frame block, with three notes held. Surge
XT's longest callback (5.6 ms, still inside the 10 ms period) was a single spike. Not shown: other
patches, more notes, instruments reporting latency, an audio interface, the 32/64 build.

## The 32/64 candidate on a device (T1-04 D38)

A build with more than 16 tracks lists every workload twice: the 16-track rows (numbers unchanged),
then each row that puts work on every track again at the build's track count. In the 32/64 build those
are workloads 17-30; 26-28 (monitoring and recording) need `--inputs`.

```powershell
cmake -S . -B build-32x64 -DALIEN_SESSION_TRACKS=32 -DALIEN_SESSION_SCENES=64
cmake --build build-32x64 --config Release --target AlienDeviceTiming
build-32x64/AlienDeviceTiming_artefacts/Release/AlienDeviceTiming.exe --plugin FILE.vst3 --instrument SYNTH.vst3 > device-timing-32.md
```

First results, development laptop, 32/64 build (Windows Audio, 48 kHz, 480 frames, muted, 4 s per row),
with the repository's test plugin as the effect and Surge XT and Vital (default patches) as the
instrument:

| Workload | 16 tracks p50 us | 16 tracks p95 us | 32 tracks p50 us | 32 tracks p95 us | 32 tracks p95 / budget % | Ratio (p50) | Over budget (32) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips | 97.8 | 127.1 | 164.7 | 204.6 | 2.0 | 1.68 | 0 |
| Session MIDI tracks | 291.1 | 435.5 | 532.5 | 797.9 | 8.0 | 1.83 | 0 |
| MIDI, sends to 4 returns | 351.5 | 498.0 | 638.9 | 911.1 | 9.1 | 1.82 | 0 |
| Session audio clips | 194.5 | 238.7 | 340.4 | 402.2 | 4.0 | 1.75 | 0 |
| MIDI, stock EQ + compressor | 512.3 | 648.3 | 965.7 | 1239.2 | 12.4 | 1.89 | 0 |
| Session drum tracks | 173.8 | 229.9 | 317.1 | 399.6 | 4.0 | 1.82 | 0 |
| Audio clips streamed from disk | 219.9 | 277.6 | 427.6 | 501.6 | 5.0 | 1.94 | 0 |
| MIDI through a VST3 effect (test plugin) | 389.4 | 520.3 | 701.6 | 989.7 | 9.9 | 1.80 | 0 |
| MIDI playing Surge XT | 1259.9 | 1519.0 | 2445.3 | 2857.2 | 28.6 | 1.94 | **1** |
| MIDI playing Vital | 2868.6 | 3441.2 | 5859.8 | 6610.3 | 66.1 | 2.04 | 0 |

Every row played without a fault or a late callback, and no stream ran dry. Also at 32 tracks: every
rack occurrence p50 518 / p95 617 us, and one MIDI track playing among 32 p50 180 / p95 218 us.

- **Cost grows about linearly with tracks.** 32 tracks cost 1.7 to 2.0 times 16 in the same build;
  the fixed part of a block (the master, returns, the device) keeps idle rows under 2.
- **One over-budget callback**: 32 Surge XT instances, max 10,455 us against a 10,000 us period. Its
  p99 was 3.4 ms, so this was a single spike (Surge XT also spiked to 5.2 ms at 16 tracks), but on this
  laptop at 480 frames 32 instances of it can drop out.
- **Vital at 32 tracks** used two thirds of the period at p95; its longest gap between callbacks was
  13 ms (1.3 periods, under the 1.5-period "late" mark).
- The 16-track rows of the 32/64 build cost about what the 16/32 build's do (D36, D37 above), as C1 found
  offline.

The input rows at 32 tracks, run by the user with `--inputs` (the laptop's microphone array, two
channels, 5 s per row); the 16-track figures are the 16/32 build's from D33-D35 above, a different run:

| Workload | 16 tracks p50 us | 32 tracks p50 us | 32 tracks p95 us | 32 tracks max us | 32 tracks p95 / budget % | Ratio (p50) | Over budget | Late |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Monitoring a stereo input | 193 | 328.8 | 423.5 | 542.9 | 4.2 | 1.70 | 0 | 0 |
| Recording to a Song take | 198 | 349.6 | 424.5 | 626.5 | 4.2 | 1.77 | 0 | 0 |
| Recording to disk (32 files) | 190 | 347.2 | 493.1 | 736.2 | 4.9 | 1.83 | 0 | 0 |

No take overran its queue, every disk take wrote every frame it recorded, and the driver counted no
xruns.

Not shown: other buffer sizes, an audio interface, real effects that do work, allocations inside
plugins (see D37).

## Other buffer sizes, a real effect, and the Surge XT spike (T1-04 D39)

D38 left three device questions open: other buffer sizes, real effects that do work, and the one over-budget
callback with 32 Surge XT instances (#460). These runs answer what this machine can: the development laptop
(Intel Core Ultra 9 288V), its built-in Realtek output, muted, at commit c7062fa. TAL-Reverb-4 is the effect
(it does real work: a stereo plate on every track) and Surge XT (default patch) the instrument.

**Buffer sizes.** The built-in Realtek driver opens only at 480 frames in shared and Low Latency mode, whatever size
is asked for; Windows Audio (Exclusive Mode) also offers 256 and 512, at 44.1 kHz. Asked for 64 or 128 frames, every
mode opened at 480, so smaller buffers need another audio device. The 16/32 build, every workload that needs no
input (`--type "Windows Audio (Exclusive Mode)" --buffers 128,256,512 --seconds 4`), microseconds:

| Workload (16 tracks) | 256 p50 | 256 p95 | 256 max | 480 p50 | 480 p95 | 480 max | 512 p50 | 512 p95 | 512 max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Transport, no clips | 57.3 | 123.8 | 214.4 | 100.9 | 153.2 | 218.4 | 106.1 | 152.6 | 233.5 |
| Session MIDI tracks | 157.5 | 254.1 | 372.0 | 281.6 | 423.9 | 568.6 | 301.8 | 450.5 | 618.5 |
| MIDI, sends to 4 returns | 190.1 | 283.9 | 390.9 | 341.6 | 484.4 | 618.4 | 398.2 | 561.5 | 704.1 |
| MIDI, stock EQ + compressor | 286.2 | 366.8 | 473.2 | 503.4 | 625.4 | 846.2 | 540.8 | 686.7 | 928.5 |
| Audio clips streamed from disk | 118.7 | 161.0 | 281.0 | 206.4 | 248.7 | 299.7 | 222.3 | 280.7 | 353.1 |
| MIDI through TAL-Reverb-4 | 641.6 | 749.2 | 886.4 | 1167.8 | 1322.7 | 1521.1 | 1238.6 | 1410.2 | 1563.1 |
| MIDI playing Surge XT | 685.9 | 854.6 | 5059.6 | 1212.8 | 1416.4 | 5438.1 | 1276.4 | 1486.6 | 5226.1 |

Every row at every size played with no over-budget or late callback, no xrun and no allocation in the callback; the
budget is 5,805 us at 256 frames, 10,884 at 480 and 11,610 at 512. Cost per callback falls about in proportion to the
buffer, so the share of the period used stays about the same (the heaviest, Surge XT, 14.7 % at p95 at 256 frames).
The other rows (drums, rack occurrences, Session audio) behaved the same; the full output is in the PR.

**A real effect at 32 tracks.** In the 32/64 build, 32 instances of TAL-Reverb-4 (workload 29), three runs of 8 s at
the shared 480 frames and 48 kHz: p50 2,394-2,420 us, p95 2,641-2,721, max at most 3,371 (27 % of the period at p95),
never over budget; at 256 frames in exclusive mode p50 1,243, p95 1,435, max 1,750.

**The Surge XT spike is a one-off, not a steady cost.** 32 Surge XT instances (workload 30) went over budget exactly
once in each of three 8 s runs at 480 frames (max 10,363-10,884 us against a 10,000 us period), while p99 stayed at
3,408-3,780 us; at 256 frames three callbacks went over and one was late (max 8,917 us). The timing tool now says which
callback was longest and which went over (on stderr; `CallbackTimingTests` checks it). That puts the spike at the same
moment every time: about 4.0 s after playback starts, with 1 s or 3 s of warm-up, for 16 instances (about 5.2 ms) and 32
(about 10.8 ms). Over 12 s of recording it does not come back. The test MIDI repeats every bar (2 s), so the spike is not
a cost of the notes; it is a one-time event inside Surge XT, about 0.3 ms per instance, landing in one callback. Neither
TAL-Reverb-4 nor Vital (D38) shows it.

What this means for the 32/64 profile: steady 32-instance Surge XT playback uses about 30 % of a 480-frame period at
p95, and a project that starts 32 instances at once can drop one block about 4 s after playback starts. That is
Surge XT's behaviour rather than the engine's; the app cannot remove it, short of starting the instruments before
playback.

Not shown: buffers under 256 frames, an audio interface or ASIO (the app is not built with ASIO), and a second machine.
