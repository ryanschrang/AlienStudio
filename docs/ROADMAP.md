# Alien Studio roadmap

The integration branch combines the redesigned project/controller UI with drum kits,
arrangement/WAV export, bounded recording and offline tempo matching. Project v6
also stores initial VST3 instrument/FX racks. See README for the supported workflow
and WINDOWS_AUDIO_CHECKLIST.md for unverified hardware/plugin checks.

Remaining work: Windows plugin/device listening tests; disk-streamed recording longer
than 60 seconds; MIDI recording/count-in/overdub; waveform editing; live warp markers;
plugin automation, sidechains/multi-output, freeze and runtime crash isolation.

Historical milestone notes below describe the earlier stacked branches.

# Alien Studio delivery roadmap

Status: first track/mixer and portable-project implementation, pending PR review.
These are incremental milestones, not promises of full commercial DAW parity.
Every milestone retains one shared track/clip/project model and real-time-safe
processing. Each needs automated coverage and Windows listening/device testing.

## Reviewed existing work

PR #22 captures JUCE audio initialization failures and shows a deferred warning
through a SafePointer. It has no review findings and its Windows checks passed.
No confirmed defect was found in this review; it remains a separate unmerged PR.
Do not duplicate its change here or assume it is already on main.

## Implemented in this change

- Independent 16-voice Session synths with per-track presets and basic mixing.
- Project v3 persistence with v1/v2 preset migration and validated mixer values.
- Portable project copies with deduplicated audio and relative media references.
- Corrected README claims and a concrete Windows audio verification checklist.

Remaining boundaries: up to 16 tracks and 32 scenes (the engine pre-allocates
this capacity; raise maxSessionTracks/maxSessionScenes to grow it), 1,024 notes
per clip, 12 factory synth presets, MIDI CC mapping for scenes 1-4 only, live
input on a separate Device rack synth, and no recording or warping. Reordering
or deleting tracks/scenes stops Session playback at the next launch boundary. Audio clips play once at their original duration. A
quantized launch does not tempo-sync their audio. Media is still decoded into RAM
under the existing limits; collection is not streaming.

## Ordered feature milestones

1. **Drum sampler and browser.** Load original/licensed samples into pads, audition
   with velocity, map MIDI notes, and sequence distinct drum voices. Save all pad
   mappings and collect their media. Test polyphony, retrigger/choke, missing
   assets, stop/panic, and bounded rendering. Extend step lengths separately.
2. **Arrangement and WAV export.** Put existing audio/MIDI clips on a common
   timeline; move/duplicate/trim/loop with undo. Capture Session launches into
   arrangement. Render a defined song range and tails to WAV using the same DSP.
   Test timeline boundaries, sample-rate/block-size consistency, cancellation,
   and saved arrangements. Finish a three-minute example song as acceptance.
3. **MIDI recording.** Explicit track input/monitor selection, metronome/count-in,
   timestamped capture, overdub/replace, quantize, sustain, and undo. Preserve held
   note ownership when changing track; test disconnect/stop/loop boundaries.
4. **Audio recording and editing.** Input channels, arm/monitor, disk writer queue,
   latency alignment, trim/fade/crossfade, take lanes and comping. Test disk-full,
   underrun, cancellation, long recordings, and device changes without losing
   existing takes. Never perform filesystem work in the audio callback.
5. **Production mixer.** Parameter automation, insert effects, buses, sends,
   sidechain and delay compensation. Validate routing cycles, bypass/state recall,
   sample-accurate automation, overload behavior, and output/stem rendering.
6. **Plugin hosting.** VST3 SDK/license review, external scan worker, failed-plugin
   recovery, instrument/effect state, automation, latency reporting, and freeze.
   Verify real plugins on Windows; a passing mock host is insufficient.
7. **Advanced editing and assistance.** Quality-evaluated time stretch/warp and
   pitch correction, expressive MIDI/articulations, chord-aware accompaniment,
   stem separation and mastering assistance. Assess algorithm/model licensing,
   CPU cost, audible artifacts, and optional downloads before committing a design.

## Reliability alongside every milestone

Project-wide undo/redo, autosave/recovery and unsaved-changes prompts are in
place; extend them to each new feature (recording takes, arrangement edits,
plugin state). Grow track capacity with bounded audio scheduling. Add hardware
results to WINDOWS_AUDIO_CHECKLIST.md. Keep actual test results distinct from
planned tests, and never label hardware performance verified from CI alone.

## Drum sampler milestone update

Implemented: eight sample pads per track, local file browser, velocity-sensitive
one-shots, fixed 16-voice polyphony, hat choke pair, external MIDI routing,
piano-roll sequencing, v4 kit persistence and portable collection. Focused DSP
and integration tests cover reset, source isolation and allocation-free operation.
Still later: dedicated multi-lane 16/32/64-step drum editor, sample trimming,
streaming, factory sample packs, and more elaborate pad modulation.

## October 2026 implementation update

Implemented foundations now include per-track eight-pad sample kits/browser,
bar-based arrangement with WAV export, bounded 60-second mono/stereo input takes,
and offline fixed-tempo WSOLA rendering. Audio lab adds explicit input settings,
record state/meter, destination display and a separate tempo-render workflow.
Full recording/editing and warping milestones remain open: streaming takes,
latency compensation, count-in/punch/overdub, trimming/fades, take lanes/comping,
automatic BPM, warp markers and transient-aware algorithms. Windows listening and
visual checks remain separate gates from the passing offline tests.
