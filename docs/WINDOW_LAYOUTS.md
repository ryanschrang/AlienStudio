# Saved window layouts (T1-11 / UI-01)

The application remembers the last workspace across normal restarts. View > Save
window layout writes it immediately, and normal application shutdown saves the
current state. View > Reset window layout docks all editors, selects Steps, hides
the device page and on-screen keyboard, and centres a normal main window. Reset
also saves that default state. Save/reset menu actions refuse while recording or
busy, including a stale menu selection.

The local preference is `Alien Studio/WindowLayout.json` under JUCE's user
application-data directory. It is not embedded in a song, portable-project copy,
undo history or autosave, and it never changes devices, musical content, plugin
windows, play/record transport state or audio ownership. There is one last-used layout per
local user, not a collection of named presets. Canceling quit does not save or
close the application. Forced termination is not a normal shutdown; Save window
layout can preserve the current arrangement before then.

## What is retained

- Main window normal bounds and maximized state; minimized state is not restored.
- Each detached editor's normal bounds/maximized state, keyed by a stable view
  name rather than display text or the currently selected tab.
- Whether each editor is detached. A docked editor retains its last floating
  bounds so the next detach opens there; closing it still means Dock.
- Active workspace tab, Devices/editor page and on-screen keyboard visibility.

Existing editor components are reparented through their ordinary detachable
hosts. Key listeners and typing-piano roots follow their normal open/close
callbacks. Editors and the audio engine are not cloned. Main geometry is restored
while the startup window is hidden; detached windows wait until the splash
finishes. Shutdown captures the layout before native hosts/editors are destroyed.

## Display changes and damaged preferences

Coordinates are JUCE logical desktop coordinates, not physical pixels. The saved
usable display area accompanies normal bounds. Restoration selects a surviving
display by overlap with that area, or the primary display if it is gone. When the
usable area changes, proportional placement is fitted to it. Windows are clamped
to a single usable area so the title bar stays reachable. On a display smaller
than the usual minimum, the minimum size is lowered to fit. Ordinary maximum
window sizes remain in force; maximized windows are then maximized on that display.

Open main and detached editor windows also check for changed usable display areas twice a second on the message thread. A monitor removal, taskbar/work-area change or changed logical display size reuses the same fitting policy, including native title bars. A topology change leaves an already reachable window on an unchanged display alone. Recovery preserves maximized state: the OS manages the maximized frame while the tracker repairs its normal restore rectangle without restoring/re-maximizing the window. Hidden or minimized windows defer recovery until shown/restored. It never raises a window, changes editor ownership, writes preferences or touches the audio engine. Reconnecting a display does not move a recovered window back to its old monitor (claim #375).

Version 1 requires valid integer rectangles, booleans and known view bindings.
Missing, unsupported, oversized (over 64 KiB), or damaged preferences fall back to
the default layout. Parsing is transactional. Saving validates first and uses a
JUCE TemporaryFile replacement; an invalid state or failed directory creation
leaves the previous preference file intact. A failed manual save reports its path
in the status bar. No file I/O runs in an audio callback.

Typing-piano notes are owned by the window that started them. Hiding or destroying a registered detached host releases its held notes immediately, without waiting for the asynchronous desktop focus callback. Removing a host also releases its notes if its focused child was already destroyed. Held keys must be lifted before they can restart after hiding, and unrelated windows keep their notes. Auxiliary key/component listeners are removed together when a host is removed or the router shuts down (claim #393). This is message-thread input cleanup, not an audio callback operation.

When a focused control is destroyed inside a surviving window, its safe focus pointer may clear before desktop focus dispatch. The router still releases held typing notes on that dispatch; two null focus pointers are not treated as unchanged when an earlier focus target was observed. Native-origin typing that never had a focused JUCE control still supports chords and normal key-up release. Focus recovery does not retrigger a key held through deletion, and deleting an unfocused control leaves the current note alone (claim #405).

Disabling a registered main or detached typing host releases its notes synchronously, before desktop focus dispatch. Disabled targets refuse typing. Re-enabling requires a held key to be lifted before it can start a fresh note, and disabling an unrelated host preserves the source window's notes. This uses the existing message-thread component listener; no audio callback is changed (claim #421).

## Verification

SongFlowTests covers schema round trips, damaged-state refusal, negative monitor
coordinates, a removed monitor, changed logical resolution/origin, offscreen
bounds and displays smaller than the ordinary minimum. It writes/reloads only a
test-created preference file and verifies failure/fallback paths. Native host
tests exercise cached bounds, maximize/normal capture, restoration of the same
editor, default docking and balanced listener lifecycle. Live display-recovery tests inject changed work-area snapshots into a real native host, covering an unrelated display addition, offscreen recovery, unchanged topology, hidden/minimized deferral, maximized/normal geometry and editor identity. These simulated snapshots do not establish physical hot-plug or mixed-DPI behavior. Existing detach/dock,
shared typing piano and menu stale-state tests remain part of the suite. TypingPianoFocusTests includes a regression that failed before the lifecycle fix: destroying the detached source left a note on before focus dispatch. It also covers hide/show repeat suppression, removed focus children, unrelated-window preservation, balanced note-offs and a surviving host after router shutdown.

Build/test results and the tested commit are recorded in the implementation PR.
Physical Windows multi-monitor/mixed-DPI acceptance remains pending. Before marking
UI-01 complete, test 100/150/200% scaling, negative-coordinate secondary displays,
monitor removal and reconnect, taskbar/work-area changes, minimize/maximize,
restart, save/reset, and keyboard/typing-piano focus while playback is active.
