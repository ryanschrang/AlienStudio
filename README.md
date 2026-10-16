# Alien Studio

**Alien Studio** is an ambitious, free and open-source digital audio workstation (DAW) for Windows 11. Its vision combines the immediacy of FL Studio-inspired pattern production, the flexible clip-based creativity of Ableton Live-inspired workflows, and the recording, editing, and MIDI depth associated with professional DAWs such as Pro Tools and Cubase.

Alien Studio is an **independent, original application**, not affiliated with or endorsed by any of those products. We aim to learn from broadly useful workflows without copying proprietary code, artwork, branding, or protected assets.

> **Project status:** Early prototype. The current code implements a basic 16-step tone sequencer with play/stop and tempo controls. Professional DAW features below are **planned**, not yet implemented. Windows CI compilation is being validated; a successful build and runtime behavior have not yet been confirmed.

## Product vision: three connected workspaces

1. **Production** — Fast beat making and composition with a channel rack, step sequencer, advanced piano roll, patterns, drum machines, samplers, synths, MIDI tools, and automation.
2. **Arrangement** — A multitrack timeline for audio and MIDI recording, clip editing, comping, fades, routing, automation, mixing, and finishing full songs.
3. **Live Performance** — A clip/session grid for launching loops and scenes, improvising arrangements, syncing tempo, and recording performances into the timeline.

The long-term differentiator is **one shared project and audio engine** across all three workspaces: create a pattern, launch it live, and arrange or edit it on the timeline without rebuilding it.

## Roadmap

### Phase 0 — Reliable foundation (in progress)
- [x] Establish C++/JUCE/CMake project and Windows GitHub Actions build workflow.
- [x] Implement initial 16-step tone sequencer, transport controls, and BPM control in source.
- [ ] Obtain a passing Windows CI build and test executable on Windows 11.
- [ ] Add automated tests, crash diagnostics, basic settings, and stable audio-device selection.
- [ ] Select and publish an open-source license after checking JUCE and third-party dependency obligations.

### Phase 1 — Usable music sketchpad
- [ ] Project save/open, undo/redo, autosave, and tempo/time signature.
- [ ] Multiple tracks and patterns; editable step sequencer and piano roll.
- [ ] MIDI note editing, quantization, velocity, and basic keyboard input.
- [ ] Built-in drum sampler and at least one playable synthesizer.
- [ ] Audio/MIDI rendering and WAV export.

### Phase 2 — Full arrangement and recording
- [ ] Multitrack arrangement timeline and clip editing.
- [ ] Audio recording, waveform display, trimming, fades, and basic comping.
- [ ] Mixer with buses, sends, metering, EQ, compression, and effects.
- [ ] Automation lanes and non-destructive editing.
- [ ] Robust audio/MIDI device handling, latency compensation, and project recovery.

### Phase 3 — Live creative workflow
- [ ] Session-style clip and scene launching with synchronized playback.
- [ ] Record clip-launch performances into arrangement.
- [ ] Audio time-stretching/warping and loop synchronization.
- [ ] MIDI controller mapping and performance controls.

### Phase 4 — Professional ecosystem
- [ ] VST3 hosting subject to SDK licensing and compatibility review.
- [ ] Advanced MIDI tools, routing, comping, editing, and mixing workflows.
- [ ] Additional instruments, modulation, mastering tools, and performance optimization.
- [ ] Optional AI-assisted features where feasible, with clear privacy and licensing controls.

These are **goals**, not promises of feature parity or delivery dates. Priorities will change as the audio engine, stability, and contributor capacity evolve.

## Build from source (Windows 11)

Install Visual Studio 2022 with **Desktop development with C++**, Git, and CMake 3.24+.

```powershell
git clone https://github.com/ryanschrang/AlienStudio.git
cd AlienStudio
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake fetches JUCE during configuration, so internet access is needed on the first configure. Windows build runs and downloadable artifacts, when available, are tracked in [GitHub Actions](https://github.com/ryanschrang/AlienStudio/actions). **A green CI build is not the same as completed hands-on audio testing.**

## Free and open-source commitment

Our goal is a fully usable, no-subscription DAW with no feature paywalls. Before distributing binaries or accepting outside contributions, the project needs a clear license, attribution and dependency review (including JUCE), and a contribution policy.

## Development principles

- **Original design:** familiar capabilities, distinct interface and identity.
- **Audio reliability first:** low-latency playback, safe real-time code, and recoverable projects.
- **Creative continuity:** patterns, clips, tracks, and timeline all share one project model.
- **Accessible workflow:** efficient keyboard shortcuts, MIDI controllers, and clear UI.
- **Honest milestones:** distinguish working code, passing builds, tested features, and future plans.
