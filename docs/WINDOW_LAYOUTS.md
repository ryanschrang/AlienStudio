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

Version 1 requires valid integer rectangles, booleans and known view bindings.
Missing, unsupported, oversized (over 64 KiB), or damaged preferences fall back to
the default layout. Parsing is transactional. Saving validates first and uses a
JUCE TemporaryFile replacement; an invalid state or failed directory creation
leaves the previous preference file intact. A failed manual save reports its path
in the status bar. No file I/O runs in an audio callback.

## Verification

SongFlowTests covers schema round trips, damaged-state refusal, negative monitor
coordinates, a removed monitor, changed logical resolution/origin, offscreen
bounds and displays smaller than the ordinary minimum. It writes/reloads only a
test-created preference file and verifies failure/fallback paths. Native host
tests exercise cached bounds, maximize/normal capture, restoration of the same
editor, default docking and balanced listener lifecycle. Existing detach/dock,
shared typing piano and menu stale-state tests remain part of the suite.

Build/test results and the tested commit are recorded in the implementation PR.
Physical Windows multi-monitor/mixed-DPI acceptance remains pending. Before marking
UI-01 complete, test 100/150/200% scaling, negative-coordinate secondary displays,
monitor removal and reconnect, taskbar/work-area changes, minimize/maximize,
restart, save/reset, and keyboard/typing-piano focus while playback is active.
