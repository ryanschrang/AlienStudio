# Alien Convolution Reverb (T2-02 / FX-02)

Choose **Alien Convolution Reverb** in the stock plugin browser. It uses a fixed,
original synthetic stereo chamber impulse response: sparse early reflections
and a decorrelated, exponentially decaying diffuse tail, lasting 300 ms. This is
not a recording of a physical room. There are no third-party samples or external
dependencies. The two channels have different responses but do not crossfeed
(dual-mono convolution). Mono input mirrored to stereo produces different tails.

| Automatable control | Range | Default |
| --- | --- | --- |
| Tone | Wet low-pass, 500 Hz to 16 kHz; bounded below Nyquist | 10 kHz |
| Wet output | -12 to +12 dB, wet path only | 0 dB |
| Mix | 0 to 100% | 25% |

Controls use 5 ms smoothing. Uniform FFT partitions add **512 samples of latency**
(about 10.7 ms at 48 kHz). The dry signal is delayed by the same amount, even at
Mix zero; the host compensates other tracks. The reported tail includes the
300 ms FIR, the partition delay and a conservative 50 ms tone-filter allowance.
Reset clears all spectra, overlap, filters and delay history.

Preparation builds the canonical 48 kHz response, integrates coefficient bins to
the current sample rate, computes its partition spectra and allocates history.
The audio callback only processes existing storage; it does no file work,
allocation or locking. The response and controls are deterministic. Stable stock
identifier `convolution` and ordinary plugin state preserve automation, saved
controls and history with no project format change. Existing stock IDs remain.

This first increment has one built-in response. User impulse-response file import,
IR selection, true-stereo cross-channel responses and longer room libraries remain
future work. The response itself is fixed; Tone does not change its decay length.

The DSP can also prepare an in-memory response while processing is stopped
(claim #273). It accepts up to one second of finite coefficients in [-1, 1]
at 8 to 192 kHz, with one channel mirrored to stereo or two equal-length
channels processed independently. It integrates coefficient bins at the
device rate and rejects invalid data before replacing the current kernel.
The stock plugin still selects the chamber. File decoding, an editor control
and project persistence are needed to expose custom responses.

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
