#!/usr/bin/env python3
"""Generate original reference WAVs in bounded blocks; never replace audio."""
import argparse
import math
from pathlib import Path
import struct
import wave

def generate(output, seconds, rate):
    if seconds < 1 or seconds > 3600:
        raise ValueError("Use a duration from 1 to 3600 seconds.")
    if rate not in (8000, 44100, 48000, 96000, 192000):
        raise ValueError("Unsupported reference sample rate.")
    output.mkdir(parents=True, exist_ok=True)
    paths = [output / f"reference-{i+1}.wav" for i in range(4)]
    if any(path.exists() for path in paths):
        raise FileExistsError("Choose a new output directory; existing files are preserved.")
    for index, path in enumerate(paths):
        # Exclusive creation also protects against a collision after the check.
        with path.open("xb") as raw:
            with wave.open(raw, "wb") as writer:
                writer.setnchannels(1)
                writer.setsampwidth(2)
                writer.setframerate(rate)
                for start in range(0, rate * seconds, 4096):
                    samples = []
                    for n in range(start, min(start + 4096, rate * seconds)):
                        phase = (n % (rate // 2)) / (rate // 2)
                        envelope = min(1, phase * 40) * min(1, (1 - phase) * 40)
                        value = .2 * envelope * math.sin(2 * math.pi * [110, 165, 220, 330][index] * n / rate)
                        samples.append(int(value * 32767))
                    writer.writeframesraw(struct.pack("<" + "h" * len(samples), *samples))
        print(path)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=4)
    parser.add_argument("--sample-rate", type=int, default=8000, choices=(8000, 44100, 48000, 96000, 192000))
    args = parser.parse_args()
    generate(args.output, args.seconds, args.sample_rate)
