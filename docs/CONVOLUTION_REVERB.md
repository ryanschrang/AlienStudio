# Alien Convolution Reverb (T2-02 / FX-02)

Choose **Alien Convolution Reverb** in the stock plugin browser. Its default
synthetic stereo chamber combines sparse early reflections with a decorrelated
300 ms diffuse tail. The two channels are processed independently.

In **Plugins**, select the effect slot, then choose **Impulse response →
Load WAV impulse response** (claim #282). Choose a mono or stereo WAV at
8 to 192 kHz and at most one second long. Mono responses are mirrored to both
channels. The project embeds the coefficients, and **Use built-in chamber**
restores the default. Both changes support Undo and preserve the three effect
controls. Changing responses reloads the plugin racks.

| Automatable control | Range | Default |
| --- | --- | --- |
| Tone | Wet low-pass, 500 Hz to 16 kHz; bounded below Nyquist | 10 kHz |
| Wet output | -12 to +12 dB, wet path only | 0 dB |
| Mix | 0 to 100% | 25% |

Controls use 5 ms smoothing. Uniform FFT partitions add **512 samples of latency**
(about 10.7 ms at 48 kHz). The dry signal is delayed by the same amount, even at
Mix zero; the host compensates other tracks. The reported tail includes the
selected FIR's duration, the partition delay and a conservative 50 ms tone-filter allowance.
Reset clears all spectra, overlap, filters and delay history.

Preparation builds the canonical 48 kHz response, integrates coefficient bins to
the current sample rate, computes its partition spectra and allocates history.
The audio callback only processes existing storage; it does no file work,
allocation or locking. The response and controls are deterministic. Stable stock
identifier `convolution` and ordinary plugin state preserve automation, saved
controls and history with no project format change. Existing stock IDs remain.

Tone filters the wet signal. True-stereo cross-channel responses and longer room
libraries remain future work.

The DSP can also prepare an in-memory response while processing is stopped
(claim #273). It accepts up to one second of finite coefficients in [-1, 1]
at 8 to 192 kHz, with one channel mirrored to stereo or two equal-length
channels processed independently. It integrates coefficient bins at the
device rate and rejects invalid data before replacing the current kernel.
The stock effect can persist these coefficients in its existing plugin state
(claim #276). The host restores the state before preparing the kernel, so no
external file is needed at project reopen. Chamber-only legacy state or an
invalid embedded response prepares the chamber.

## Manual acceptance (not yet run)

- Add it to a track, return, bus and Master FX. Listen to drums, vocals and a
  sustained instrument; confirm audible early reflections and a short diffuse
  tail. Check for clicks, metallic ringing, unintended stereo imbalance and
  usable level across 44.1/48/96 kHz. CPU and hardware acceptance remain pending.
- At Mix zero, compare against bypass after latency compensation; raise Wet output
  and verify the dry level stays unchanged. Mix 100% should remove direct dry sound.
- Sweep Tone and Mix and automate all three controls. Save/reopen, Undo/Redo and
  reorder/bypass the plugin. Stop/restart and change audio devices; no old tail
  should return after reset. Compare live playback, Song WAV export and freeze,
  paying attention to aligned transients and retained tails.
- Check the three knobs, typed Hz/kHz/dB/percent values and layout at 100/150/200% DPI.

## Automated evidence

StockDspTests compares FFT output to independent direct FIR convolution at multiple
sample rates, including overlap/history wrap, aligned dry timing, reset/tail,
automation block independence and invalid input. StockEffectsTests covers host
latency alignment, catalog-wide parameter/state/editor behavior, silent automation
and callback allocation guards. ProjectControllerTests covers state Undo/Redo and
project serialization. None of these establishes native listening quality.
