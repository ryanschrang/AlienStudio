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
- No plugins except the stock EQ and compressor, no streaming from disk, and no recording or input
  monitoring (the device is opened with no inputs).

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
