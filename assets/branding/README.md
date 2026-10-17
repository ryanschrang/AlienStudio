# Alien Studio branding assets

`AlienStudio_Icon_Corrected.ico` is the approved corrected Windows icon, supplied
as `1000005180.ico` and renamed without changing its bytes. It contains 16, 24,
32, 48, 64, 128 and 256 pixel images. Keep the entire alien, headphones and neon
rounded border, with the original balanced padding. Do not crop, stretch or
replace these assets with generated art or the older cropped versions.

`AlienStudio_Icon_Corrected.png` is the original 1024 x 1024 high-resolution
branding source supplied as `1000005179.png` inside the uploaded ZIP, reserved
for future branding/splash work. Retain the original supplied file; do not
reconstruct or upscale it from the ICO. Issue #9 allows splash preparation: this
asset directory is that preparation. No startup splash or delay is introduced;
the separate alien/UFO splash artwork is not part of this integration.

## Windows resource integration

`resources/AlienStudioIcons.rc.in` embeds the ICO directly as icon groups 1 and 2.
CMake adds this script only on Windows and preserves JUCE's generated version
resource. JUCE 8's native window class extracts the first two executable icon
groups for its large and small icons, covering the native title bar and taskbar
where Windows supports them. Explorer and new shortcuts use the executable's
first icon group. This project has no installer or Start-menu shortcut generator;
future packaging should use this executable's icon (index 0).

Do not use JUCE's `ICON_BIG`/`ICON_SMALL` generator for this approved ICO: in JUCE
8.0.10 it regenerates only 16, 32, 48 and 256 pixel frames and loses three supplied
sizes. Updating the source should be an intentional replacement with newly
approved files, followed by the asset/resource tests and Windows visual checks.

## Verify on Windows 11

Download and extract the Release artifact from this PR's Windows C++ CI run into
a new directory. Close previous Alien Studio processes before launching it.

1. Inspect the executable in Explorer's Details/Small icons, Medium, Large and
   Extra large icon views (Ctrl + mouse wheel also changes icon size). At small
   sizes check the alien/headphone silhouette and neon border; at large sizes
   confirm all four sides have balanced padding and nothing is cut off.
2. Launch that executable and check the native window/title-bar icon, taskbar
   button and Alt-Tab icon. Check 100%, 150% and 200% display scaling where
   available. Windows chooses the nearest supplied size; exact rendering depends
   on OS theme, title-bar settings and display scaling.
3. Make a fresh desktop/Start-menu shortcut to this executable, or unpin an old
   taskbar shortcut and pin the freshly extracted executable. Existing shortcuts
   may have a different icon location or retain an older cached icon.
4. If Explorer still shows an old icon, first use a new extraction directory and
   reopen the window. Try `ie4uinit.exe -show` from Run/Terminal to refresh icons.
   If necessary, restart Windows Explorer through Task Manager or sign out and
   back in. These are manual cache refresh options; the app does not alter caches.
5. Smoke-test synth presets, mouse piano, computer-keyboard notes with Tab and
   octave changes, Play/Stop and the 16-step sequencer. No audio/input source code
   changes are needed for this icon integration.

Automated checks validate the approved files and, on Windows, the built
executable's icon groups and exact frame payloads. A passing build/resource test
is not a visual Explorer/taskbar check; document manual results separately.
