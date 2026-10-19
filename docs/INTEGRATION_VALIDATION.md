# Integrated DAW and VST3 host validation

## Local results

- Linux Release application target built with CMake and JUCE 8.0.10, VST3 hosting enabled.
- All 19 C++ test executables compiled and passed. Local execution reused common JUCE module objects; the standard CMake/CTest definitions remain the Windows CI path.
- The focus/UI test passed under a virtual X11 display with standard window-manager atoms populated.
- Branding source-asset tests passed; compiled Windows icon-resource check was not run locally.
- New Drums, Arrangement, Audio lab and Plugins workspaces rendered for inspection at 1280x800, with compact Plugin Rack rendering at 960x650.
- `git diff --check` passed.

The tests include the actual engine callback, zero counted C++ heap allocations in its tested processing paths, MIDI offsets across 512-sample internal block boundaries, instrument/track/master gain order, transport note release, FX bypass, track latency alignment, plugin state round-trip and missing-plugin preservation. DSP test processors are original deterministic fixtures, not third-party plugins.

Controller regressions cover discarding while an autosave is queued/in flight, collected audio paths surviving undo/redo after the original file is deleted, both historical v4 project dialects, v5 arrangements, track/scene structure, undo/redo, autosave/recovery and saving. Existing drum, arrangement/WAV, recording/tempo-warp, synth, MIDI, piano-roll and focus tests also passed.

## Not verified

- Physical Windows audio/MIDI devices, input capture, latency and audible performance.
- Compatibility or native-editor behavior with real third-party VST3 binaries; none were available in the local environment.
- Crash/timeout behavior with a genuinely crashing third-party plugin. The child-process scanning path is implemented, but this manual case remains on the Windows checklist.
- Production readiness, long recording sessions, live warp editing, automation and advanced routing.

Windows Debug/Release CI results must be reviewed on the latest PR commit. Keep the PR draft until those checks and appropriate plugin/device checks have been reviewed.

## Normal repository checks

On Windows with the documented C++ build prerequisites:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Repeat with Debug. Use `docs/WINDOWS_AUDIO_CHECKLIST.md` for manual device and plugin checks. Recording and tempo-warp source input remain capped at 60 seconds.
