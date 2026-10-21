# Representative Alien Studio projects

These original reference sessions are repeatable starting points, not evidence
that every DAW workflow is complete. Open the .alienstudio files from this checkout.
They require no third-party plugins. All playback/listening results are NOT RUN
for this draft until recorded in results/TEMPLATE.md on the exact build.

| Project | Purpose | Expected baseline |
| --- | --- | --- |
| production/Production.alienstudio | 120 BPM, four tracks, four scenes, 16-bar song | Built-in bass, chords, lead and pulse; arrange and export |
| live/LivePerformance.alienstudio | 128 BPM, four scenes, beat launches | Launch Build/Full/Break/Finish without old notes sticking |
| recording/MultitrackReference.alienstudio | Four original mono reference WAVs, 120 BPM | Two-bar simultaneous playback; additional empty take slots |

## Production

Open Production, select Arrangement and Play. Compare all 16 bars to WAV export;
check track mute/solo/pan, stop/restart, note timing and final silence. Save a new
copy, change notes and region order, undo/redo, then reopen. Use Session to audition
the four scenes. MIDI patterns loop; audio clips elsewhere remain one-shots.

## Live performance

Open LivePerformance and select Session. Start transport and launch scenes using
the scene launch controls. Build omits lead; Break omits bass/pulse. Switch Beat
to Bar and confirm launch boundaries, including empty tracks. Exercise Stop,
rapid scene changes and device restart. Test MIDI CC scene mapping separately
with a physical controller. Record controller model and driver in the results.

## Recording reference

Open MultitrackReference and select Arrangement/Play. Four distinct synthesized
reference signals should play together for four seconds. Solo each and check
its start/end. They are generated test audio, not an acoustic band recording.
Open Session and select a slot in Record Here before recording with Audio lab;
save a fresh WAV, then launch that slot. Select a slot in Alternate Take to render
an offline tempo match. Use Save portable into a new folder, move it, reopen and
verify media. Do not overwrite the original fixture.

Current audio recording is mono/stereo from Audio lab. Simultaneous independently
armed recording tracks, punch, comping and monitoring are future milestones; this
fixture must not be described as a passing multitrack recording test.

## Long playback extension (expected to remain blocked on baseline)

Generate long sources with:
`python tools/generate_reference_audio.py --output <NEW_FOLDER> --seconds 3600 --sample-rate 48000`

The generator uses bounded blocks and refuses to replace files. At 48 kHz,
one hour of mono audio exceeds the decoded clip budget; the small committed
references remain four seconds at 8 kHz. Do not import
generated hours expecting baseline support: 128 MiB decoded-clip and 512 MiB
retained-session limits still apply. Use these sources when STREAM-02/03 land,
then test several tracks, duplicate-source launches, failures and memory use.
The Arrangement timeline runs to 16,384 bars (18 hours at 60 BPM), so long sources can be placed and exported there as well as launched in Session.

## Automated validation

RepresentativeProjectTests parses all three fixtures with the native project
parser, round-trips serialization and opens each real reference WAV through JUCE.
StreamingAudioRingTests exercises the buffer foundation independently.
Neither test establishes audible output, Windows device behavior or full DAW parity.
