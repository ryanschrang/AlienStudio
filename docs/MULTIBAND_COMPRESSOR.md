# Alien Multiband Compressor (T2-02 / FX-02)

The stock plugin browser offers **Alien Multiband Compressor** on track, return,
bus and master effect racks. It uses the same bypass, ordering, automation and
saved-state path as the other stock effects, with stable identifier `multiband`.
No project format change is required.

Eight automatable controls:

| Control | Range | Default |
| --- | --- | --- |
| Low/Mid/High threshold | -60 to 0 dBFS, independently | -18 dBFS |
| Ratio | 1:1 to 20:1, shared | 4:1 |
| Attack | 0.1 to 200 ms, shared | 10 ms |
| Release | 5 to 2000 ms, shared | 120 ms |
| Output | -12 to +12 dB on wet path | 0 dB |
| Mix | 0 to 100% | 100% |

The fixed nominal splits are 250 Hz and 2.5 kHz. These are gentle complementary
first-order splits, not brick-wall bands: low is LP250, mid is LP2500 minus LP250,
and high is input minus LP2500. Distant bands are largely independent, but their
responses overlap around the splits. Each band has a separate stereo-linked
peak detector, a 10 ms detector fall time, a fixed 6 dB soft knee and independent
gain reduction. The louder channel drives the same gain in both channels for
each band; audio never crosses between channels. There is no external sidechain.

Threshold, ratio, output and mix changes are smoothed over a 5 ms time constant.
Attack/release change the dynamics time constants without resetting detector or
gain history. At unity band gains and zero output trim the bands reconstruct the
dry signal exactly. Mix zero is exact dry once settled, including when wet Output
is raised. No lookahead or latency is added; this compressor is not a peak limiter.
The filter tail is reported as 100 ms. Reset clears filter, detector and control
history; processing allocates no memory after preparation.

`StockDspTests` checks selective reduction, unity/dry behavior, stereo linking,
attack/release, reset/tail, automation block-size independence and invalid input.
`StockEffectsTests` checks every catalog effect's state, controls, editor layout,
host automation without edit notifications and processing allocation guards.
`ProjectControllerTests` checks Undo/Redo and project state round-tripping.
These do not replace native hardware listening and physical display checks.
