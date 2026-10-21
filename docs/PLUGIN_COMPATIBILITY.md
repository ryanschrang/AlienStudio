# VST3 plugin compatibility

Status of the work described here: **automated tests pass on Linux, against a real VST3 built for the purpose; nothing in this
file has been run on Windows yet.** Every "NOT RUN" below stays that way until a person has run the check on a Windows
machine and recorded the result. Passing CI says the machinery works against purpose-built test plugins. It says nothing
about whether a particular commercial plugin works, which is what this document is for.

## What Alien Studio does with a plugin

| Moment | What happens | If the plugin misbehaves |
| --- | --- | --- |
| **Scanning** (Plugins > Scan VST3) | Every `.vst3` is loaded in a **separate process** with a time limit. Folders are searched recursively; `.vst3` bundles are not entered; symbolic-link loops are not followed. Results and failures are remembered per file (size + modification time), so unchanged files are not loaded again. | A crash, hang or odd exit is recorded against that file with the stage it reached ("while loading the plugin and reading its class list"), in the scan log. The scan carries on with the next file. **Cancel scan** stops at once, even in the middle of a stuck plugin, and records nothing about that file. **A scan that says OK means the plugin's description could be read.** Plugins that ship a module manifest (`moduleinfo.json`, which JUCE-built and most current plugins do) are described without being created, so the scan cannot tell whether they can be created. The safety check on the next row can. |
| **Loading a project** | The first time a plugin file is seen, the **short compatibility check** (below) runs in a separate process. A plugin that passes is trusted until its file changes. | A plugin that crashes or hangs in the check is **not loaded**. Its slot stays in the project with its saved settings, labelled "NOT LOADED", with the reason. |
| **Creating the plugin** | Inside Alien Studio, under a fault trap. | A crash or exception while creating it, applying its saved state or preparing it leaves a placeholder slot (settings kept) and a message. |
| **Playing** | Every call into the plugin runs under a fault trap, and its output is checked for NaN/infinity. | The plugin is switched off for the rest of the session and is never called again. An effect passes its **unprocessed input** through; an instrument goes silent. The slot shows "FAILED - switched off". The report is written to `plugin-host.log`. |
| **Opening an editor** | Creating and showing the window run under the fault trap. | The slot shows "editor crashed", the editor is not reopened this session, and the plugin keeps playing if it can. |
| **Saving** | A plugin that has failed is not asked for its state again. | The last good saved state stays in the project. |
| **Exporting** | Uses separate plugin instances. | If any plugin fails during the render, the export is reported as having **plugin failures** instead of success. |

### What this cannot protect against

A fault trap is not a sandbox. These still take the whole application down, and **autosave is the only backstop**:

* heap corruption or other damage that surfaces later, somewhere else;
* a crash on a thread the plugin started itself;
* a plugin that never returns while being played or while its editor is open (the scan and the safety check have time limits, live playback does not);
* anything that happens inside an open editor window after it has been shown (mouse, paint and timer callbacks arrive from the operating system, outside the trap).

The safety check exists because of these: a plugin that dies this way in the check dies in a child process, not in your session.
A plugin that passes the check can still fail later; the check samples behaviour, it does not prove it.
Running plugins in a separate process permanently (out-of-process hosting) is a larger project and is not part of this change.

## Running the check on Windows

`AlienPluginCheck.exe` is a console tool built next to Alien Studio. It scans for plugins and runs the full battery on each,
**each plugin in its own process**, so a plugin that crashes or hangs becomes a line in the report instead of ending the run.

1. Get `AlienPluginCheck.exe` from the build artifact `AlienPluginCheck-Windows` of the pull request's *Windows build* run.
2. Open PowerShell in the folder that holds it.
3. Valhalla Supermassive on its own:

   ```powershell
   .\AlienPluginCheck.exe check --name Supermassive
   ```

4. Every plugin in the standard VST3 folders (`C:\Program Files\Common Files\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3`), including subfolders:

   ```powershell
   .\AlienPluginCheck.exe check
   ```

   Other places: `.\AlienPluginCheck.exe check "D:\My Plugins"` (searched recursively) or `check "D:\My Plugins\Some.vst3"`.
5. Send back the two files from the folder it names at the end (`AlienPluginCheck-<date>`): `plugin-check-report.txt` and `plugin-scan.log`.

Useful options: `--quick` (about a second per plugin), `--no-editor` (do not open windows), `--timeout 300` (seconds per plugin),
`--out <folder>`. Exit code: 0 = every plugin compatible, 1 = something failed or crashed, 2 = bad arguments, 3 = nothing found.
Some plugins show a licence or update dialog when loaded; that is reported as a timeout on that plugin ("It may be waiting on a licence check...").
Dismiss nothing: let it time out and send the report.

### What the battery checks

| Step | What it does | Typical findings |
| --- | --- | --- |
| Load | Creates the plugin. | Crash, exception, "could not be created". |
| Bus layout | Applies the stereo layout this host uses and switches extra (sidechain) buses off. Also records the buses it found. | Plugins that refuse stereo; plugins whose saved state asks for another layout. |
| Play at 44.1 / 48 / 96 kHz × 64 / 512 samples | Silence, then noise (effects) or a held note (instruments). Checks every sample is a real number and the level is plausible. | NaN or infinity; silence from an effect or a note-less synth; sound with no input. |
| Odd block sizes (1 to 512) | Awkward block lengths, as hosts and offline renders produce. | Crashes or NaN on short blocks. |
| Latency and tail | Reports where an impulse really appears against the latency the plugin declares, and how long it rings after noise stops against its declared tail. | "Reports latency 256 but sound appears at 0": tracks would be misaligned. "Rings 4 s, declares 0.5 s": exports and freezes would cut tails. |
| State round trip | Sets parameters (switches are set to a valid position), plays a few blocks so the plugin has seen the changes, saves, restores into a second instance, compares parameters. | A plugin that saves nothing, or forgets settings. |
| Parameter changes while playing | Moves parameters to 0, 1, 0.5 and back while audio runs on another thread. | Crashes or NaN from parameter changes. |
| Two instances at once | Two copies side by side; destroys one, the other must carry on. | Plugins with global state. |
| Damaged state is survived | Feeds random bytes of five sizes, then truncated and partly overwritten copies of the plugin's own saved state. | Crashes when a project file is damaged. Plugins built with JUCE's VST3 wrapper check the saved-state container before their own code sees it, so for those this step mostly shows that the wrapper copes. |
| Editor opens, resizes and closes | Opens the window with audio running. Skipped with a stated reason if there is no display. | Editor crashes, nonsensical sizes. |
| Unload and reload three times | Destroys and recreates the plugin. | Crashes on unload; state left behind. |

Verdicts: **COMPATIBLE**, **COMPATIBLE WITH WARNINGS** (it works, something is worth knowing), **NOT COMPATIBLE** (a step failed),
**CRASHED** (a step crashed, or the check process died; the report says in which step). If the process dies without being able
to say why, the report keeps every step that finished before it and names the step that was running.

## Results

| Plugin | Role | Version | Windows build tested | Result | Notes |
| --- | --- | --- | --- | --- | --- |
| **Valhalla Supermassive** (Valhalla DSP) | Stereo FX, reverb/delay | not recorded | none | **NOT RUN** | Needs `AlienPluginCheck check --name Supermassive` on Windows, then the in-app checklist below. |

Add rows from `plugin-check-report.txt`. A representative set should cover each of these, using whatever you have installed:

| Coverage | NOT RUN |
| --- | --- |
| A simple stereo effect (Supermassive above) | NOT RUN |
| A synth with a large or GPU-drawn editor | NOT RUN |
| An effect with a sidechain input (e.g. a compressor) | NOT RUN |
| An instrument with several output buses | NOT RUN |
| A plugin that shows a licence or update prompt | NOT RUN |
| A plugin with a very large saved state (sampler or sample-library instrument) | NOT RUN |
| A resizable editor | NOT RUN |
| A plugin reporting latency (lookahead limiter or linear-phase EQ) | NOT RUN |

Record for every row: plugin name and version, Alien Studio commit, Windows version, the verdict, and anything unexpected.

### In-app checklist (needs a person; the check tool cannot do these)

For each plugin you care about, on Windows 11:

1. **Plugins > Scan VST3 > Scan the standard VST3 folders.** Watch the progress; cancel it once and confirm it stops. Open **Show the last scan log**: every file should have a line, and failures should say what stage they reached.
2. Add the plugin to a track (instrument) or as an FX. Play a pattern. Confirm it sounds right and the meters move.
3. **Open editor.** Close it with the window's close button. Open it again. Resize it if it can be resized.
4. Change settings in the editor, **Save**, close Alien Studio, reopen the project. The settings must be back.
5. Move the FX up/down, bypass it, remove it. Undo/redo.
6. **Export WAV.** Listen to the file: effects present, tails not cut.
7. Look at `%APPDATA%\Alien Studio\Logs\plugin-host.log` and the scan log. Anything unexpected is a finding.
8. Optional, to see containment working in the real application: copy the CI artifact `AlienTestPlugin-Windows` into a VST3 folder, scan it, set the user environment variable `ALIEN_TEST_PLUGIN=crash-process=200` **before** starting Alien Studio, add it as an FX and press play. After a few seconds the slot must read "FAILED - switched off", the audio must carry on, and the log must name it. Remove the plugin and the variable afterwards. Other values are listed at the top of `tests/TestPlugin/TestPluginCore.h`.

## What is verified, and where

| Claim | Evidence | Platform |
| --- | --- | --- |
| A crash inside a guarded call is caught and reported (access violation, illegal instruction, divide by zero, C++ exception); unguarded faults still crash; earlier handlers keep working | `FaultGuardTests`, with real faults, 80,000 repeats, 4 threads, nested guards, forked child processes | Linux (POSIX signals). **The Windows path (structured exceptions, stack overflow recovery) is compiled and run only by Windows CI.** |
| A failing plugin is switched off, never called again, passes its input through, does not allocate or lock on the audio thread | `PluginFaultTests`, `PluginRuntimeTests` (allocation counting) | Linux |
| Recursive discovery, architecture rejection, crash/hang/odd-exit classification, scan database, incremental rescans, retry, cancel, persistence; the child is really killed on timeout | `PluginScanTests` against real crashing, hanging and flooding child processes | Linux |
| The battery names the step of a crash, NaN, a lie about latency, lost state, editor failures; the safety check blocks only crashes and hangs | `PluginCheckTests` | Linux (editor steps under a virtual display) |
| A real VST3 binary is scanned, checked, hosted, and survives crashes, hangs, NaN and a crash on its own thread, through the scanner, the tool and the host | `PluginEndToEndTests` with the `AlienTestPlugin` VST3 (a plugin that misbehaves on request) | Linux. The same test is built and run by Windows CI, which has not run it yet |
| A plugin that is refused by the safety check is never created, keeps its settings in the project, and the panel says why; a later successful load clears the note | `PluginFaultTests` | Linux |
| Cancelling a scan returns promptly, kills the stuck child, records nothing about the file it cut off, and the next scan does that file again; the scan log text is complete | `PluginScanTests` (fake and real child processes) | Linux |
| Slot labels carry `NOT LOADED` / `FAILED` / `editor crashed`; the scan buttons and menu commands reach their handlers | `PluginPanelTests` | Linux (no window; the pop-up menu itself is not shown) |
| Valhalla Supermassive, or any commercial plugin, works | nothing | **NOT RUN** |
| The Plugins panel (menus, progress, health text) looks right and is usable | nothing automated beyond construction tests | **NOT RUN on Windows** |

## What the real-plugin tests taught us

These came out of running the machinery against a real VST3 binary rather than against in-process fakes, and they shaped the design:

* **Scanning does not necessarily create the plugin.** JUCE's VST3 host reads the class list from `moduleinfo.json` when a plugin ships one, without running the plugin's constructor. A plugin that crashes in its constructor therefore scans as OK. This is why the safety check exists and why it runs the plugin's code in a separate process before the first load.
* **A VST3 plugin must be prepared on the message thread.** JUCE's VST3 host takes the message-manager lock inside `prepareToPlay`, so preparing a plugin from a worker thread while the main thread waits for it deadlocks. The check battery therefore runs every control call (create, prepare, release, destroy, state) on the main thread and only audio processing on its own audio thread, as the application does.
* **Parameter changes reach a VST3 plugin's processor with the next audio block.** A state round trip that saves immediately after changing parameters sees the old values. The battery plays a few blocks first.
* **Asking a VST3 plugin whether it has an editor creates the editor.** An editor that crashes therefore shows up while loading, not when the window opens. The report names "asking the plugin for its name, editor..." instead of blaming the constructor.
* **JUCE's VST3 wrapper validates a plugin's saved state before the plugin sees it.** Random bytes never reach a JUCE plugin's own parser, so the damaged-state step cannot make such a plugin crash. It still matters for plugins built another way, and `PluginCheckTests` covers that code path in-process.
* **Two checks that start together must not share a result file.** Picking "a file name that does not exist yet" is not atomic, and the child process creates its result file only when it finishes. Checks started at the same moment chose the same name, deleted each other's progress markers, and reported healthy plugins as crashed (2 of 4 wrong in the reproduction). This matters when a scan and a project load overlap, with two copies of the app, and with two runs of the tool. Every result file now has a unique name, and `PluginEndToEndTests` starts four checks at once.
* **A child process can close its output a moment before it has exited.** Reading the exit code straight after reading the output returned 0 for a plugin that exited with 1. The tool's tests (and the scanner) wait for the process to finish first.
