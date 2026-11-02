#!/usr/bin/env python3
"""Rebuilds content/Starter Pack from pinned, openly licensed sources.

The Starter Pack is a small set of drum one-shots and MIDI clips that ships with Alien
Studio. Nothing in it is original to this project: every file is derived from one of the
sources below, and content/Starter Pack/manifest.csv records exactly which.

  - Virtuosity Drums (Versilian Studios and Karoryfer Samples), CC0-1.0: drum kit and
    hand-percussion one-shots. The kit's close, overhead and room microphones are mixed
    into one stereo hit per sound.
  - Versilian Community Sample Library (VCSL), CC0-1.0: claps.
  - free-midi-chords (Ludovic Drolez), MIT: chord progressions in C major and A minor.
  - Groove MIDI Dataset (Google LLC, Magenta), CC-BY-4.0: drum grooves performed by
    drummers on an electronic kit, cut to their first four bars.

Usage (needs numpy, soundfile and mido):
    python tools/build_starter_pack.py <download cache folder>

Downloads go to the cache folder, never into the repository. Running it again with the
same pins writes the same files.
"""
import csv
import io
import json
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

import mido
import numpy as np
import soundfile as sf

VIRTUOSITY = ("sfzinstruments/virtuosity_drums", "9f04cf9a734527edfbb0a4eee1f674e45bbf71bc")
VCSL = ("sgossner/VCSL", "c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e")
CHORDS_ZIP = "https://github.com/ldrolez/free-midi-chords/releases/download/v0.20260914/free-midi-progressions-20260914.zip"
GROOVE_ZIP = "https://storage.googleapis.com/magentadata/datasets/groove/groove-v1.0.0-midionly.zip"

SOURCES = {
    "virtuosity": ("Virtuosity Drums by Versilian Studios and Karoryfer Samples", "CC0-1.0",
                   f"https://github.com/{VIRTUOSITY[0]}/tree/{VIRTUOSITY[1]}"),
    "vcsl": ("Versilian Community Sample Library by Versilian Studios", "CC0-1.0",
             f"https://github.com/{VCSL[0]}/tree/{VCSL[1]}"),
    "chords": ("free-midi-chords by Ludovic Drolez", "MIT", CHORDS_ZIP),
    "groove": ("Groove MIDI Dataset by Google LLC (Magenta)", "CC-BY-4.0", GROOVE_ZIP),
}

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "content" / "Starter Pack"

# ---- downloads ----------------------------------------------------------------------------------

def fetch(url, cache):
    target = cache / re.sub(r"[^A-Za-z0-9._-]+", "_", url.split("://", 1)[1])
    if not target.exists():
        print("download", url)
        request = urllib.request.Request(url, headers={"User-Agent": "AlienStudio-build-starter-pack"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read()
        target.write_bytes(data)
    return target


def raw(repo, path, cache):
    owner_repo, sha = repo
    return fetch(f"https://raw.githubusercontent.com/{owner_repo}/{sha}/{urllib.request.quote(path)}", cache)

# ---- audio ---------------------------------------------------------------------------------------

def stereo(data):
    return np.repeat(data[:, None], 2, axis=1) if data.ndim == 1 else data[:, :2]


def mix(parts, cache):
    """Sums (repo, path, gain) sources sample-aligned, as the instrument's own mixer would."""
    rate, total = None, None
    for repo, path, gain in parts:
        data, r = sf.read(raw(repo, path, cache), dtype="float64", always_2d=False)
        if rate is None:
            rate = r
        if r != rate:
            raise SystemExit(f"{path}: rate {r} differs from {rate}")
        data = stereo(data) * gain
        if total is None:
            total = data
        else:
            n = max(len(total), len(data))
            total = np.pad(total, ((0, n - len(total)), (0, 0))) + np.pad(data, ((0, n - len(data)), (0, 0)))
    return total, rate


def finish(data, rate, longest):
    """Trims silence, caps the length with a fade, normalizes to -1 dBFS and dithers to 16 bits."""
    level = np.max(np.abs(data), axis=1)
    peak = level.max()
    if peak <= 0:
        raise SystemExit("silent sample")
    start = max(0, int(np.argmax(level > peak * 0.01)) - int(rate * 0.002))
    loud = np.nonzero(level > peak * 0.001)[0]
    end = min(len(data), loud[-1] + int(rate * 0.02), start + int(rate * longest))
    data = data[start:end].copy()
    fade = min(len(data), int(rate * 0.03))
    data[-fade:] *= np.linspace(1.0, 0.0, fade)[:, None]
    data *= (10 ** (-1 / 20)) / np.max(np.abs(data))
    rng = np.random.default_rng(len(data))   # deterministic dither: same input, same file
    dither = (rng.random(data.shape) - rng.random(data.shape)) / 32768.0
    return np.clip(np.round((data + dither) * 32767.0), -32768, 32767).astype(np.int16)

# ---- what goes in ------------------------------------------------------------------------------

KIT_MICS = (("kickmic", 1.0), ("snaremic", 1.0), ("oh", 1.0), ("room", 0.5))

# (output, folder, articulation, layer as a fraction of the loudest, longest seconds)
KIT = [
    ("Kicks/Kick.wav", "kick", "kick_snoff", 0.85, 1.5),
    ("Kicks/Kick Soft.wav", "kick", "kick_snoff", 0.45, 1.5),
    ("Kicks/Kick Snares On.wav", "kick", "kick_snon", 0.85, 1.5),
    ("Snares/Snare.wav", "snare", "snare_center", 0.85, 1.5),
    ("Snares/Snare Soft.wav", "snare", "snare_center", 0.45, 1.5),
    ("Snares/Snare Rimshot.wav", "snare", "snare_rimshot", 0.85, 1.5),
    ("Snares/Snare Cross-stick.wav", "snare", "snare_crossstick", 0.85, 1.0),
    ("Snares/Snare Flam.wav", "snare", "snare_flam", 0.85, 1.5),
    ("Snares/Snare Muted.wav", "snare", "snare_muted", 0.85, 1.0),
    ("Snares/Snare Snares Off.wav", "snareoff", "snareoff_center", 0.85, 1.5),
    ("Hats/Hat Closed.wav", "hh", "hh_closed", 0.85, 0.6),
    ("Hats/Hat Closed Soft.wav", "hh", "hh_closed", 0.4, 0.6),
    ("Hats/Hat Half Open.wav", "hh", "hh_half", 0.85, 1.2),
    ("Hats/Hat Open.wav", "hh", "hh_open", 0.85, 2.0),
    ("Hats/Hat Pedal.wav", "hh", "hh_pedal", 0.85, 0.6),
    ("Toms/Tom High.wav", "htom", "htom_center", 0.85, 2.0),
    ("Toms/Tom Low.wav", "ltom", "ltom_center", 0.85, 2.5),
    ("Toms/Tom Low Rimshot.wav", "ltom", "ltom_rimshot", 0.85, 2.5),
    ("Cymbals/Ride.wav", "ride", "ride_ride", 0.85, 4.0),
    ("Cymbals/Ride Bell.wav", "ride", "ride_bell", 0.85, 4.0),
    ("Cymbals/Crash.wav", "crash", "crash_crash", 0.85, 4.0),
    ("Cymbals/Crash Sizzle.wav", "crash", "crash_sizzle", 0.85, 4.0),
]

# (output, source folder, file name pattern; the loudest layer, first round robin, is used)
PERCUSSION = [
    ("Percussion/Shaker.wav", "shaker", r"LShaker_Shake1D"),
    ("Percussion/Tambourine.wav", "tambourine", r"Tamb1_Shake"),
    ("Percussion/Cowbell.wav", "cowbell", r"Cowbell1_Normal"),
    ("Percussion/Claves.wav", "claves", r"Claves1_Hit"),
    ("Percussion/Conga.wav", "conga", r"Conga_22_HitN"),
    ("Percussion/Tumba.wav", "tumba", r"Tumba_24_HitN"),
    ("Percussion/Bongo High.wav", "bongoh", r"BongoH_Hit1"),
    ("Percussion/Bongo Low.wav", "bongol", r"BongoL_Hit1"),
    ("Percussion/Agogo High.wav", "agogo", r"Agogo_High"),
    ("Percussion/Agogo Low.wav", "agogo", r"Agogo_Low"),
    ("Percussion/Triangle.wav", "triangle", r"Triangle1_Hit"),
    ("Percussion/Cabasa.wav", "cabasa", r"Cabasa1_Rub"),
    ("Percussion/Vibraslap.wav", "vibraslap", r"Vibraslap1_1_Hit"),
]

CLAPS = [
    ("Claps/Clap.wav", "Idiophones/Struck Idiophones/Claps/Clap_rr1.wav"),
    ("Claps/Clap Alt.wav", "Idiophones/Struck Idiophones/Claps/Clap_rr4.wav"),
    ("Claps/Solo Clap.wav", "Idiophones/Struck Idiophones/Claps/SoloClap_vl3.wav"),
]

CHORDS = {
    "Major": ["C - I V vi IV - Hopeful Romantic", "C - I IV V - Joyful Excited", "C - vi IV iii V - Hopeful Romantic",
              "C - I vi IV V - Romantic Hopeful", "C - IV I V vi - Joyful Romantic", "C - I iii vi IV - Romantic Hopeful",
              "C - IV vi I V - Hopeful Romantic", "C - I IV ii V - Joyful Triumphant", "C - ii V I - Triumphant",
              "C - Isus2 I vi7 visus4 - Playful Romantic"],
    "Minor": ["A - i VI III VII - Nostalgic Hopeful", "A - i iv v - Sad Lonely", "A - i VII VI III iv VI VII i - Mysterious Surprised",
              "A - VI VII i III - Triumphant Nostalgic", "A - i iv VI v - Sad Hopeful", "A - VI i v III - Hopeful Nostalgic",
              "A - i v iv VII - Sad Rebellious", "A - im7 ivsus4 v7 isus4 - Mysterious Tender", "A - i VII III VI - Rebellious Triumphant",
              "A - iv i v VI - Nostalgic Hopeful"],
}

# Styles to take one 4/4 groove of each (the first in the dataset's own order), named by style.
GROOVES = [("rock", "Rock"), ("funk", "Funk"), ("hiphop", "Hip-Hop"), ("soul", "Soul"), ("pop", "Pop"),
           ("jazz-swing", "Jazz Swing"), ("afrobeat", "Afrobeat"), ("latin", "Latin"), ("reggae", "Reggae"),
           ("dance-disco", "Disco"), ("dance-breakbeat", "Breakbeat"), ("neworleans-funk", "New Orleans Funk"),
           ("latin-brazilian-samba", "Samba"), ("latin-brazilian-bossa", "Bossa Nova"), ("blues-shuffle", "Blues Shuffle"),
           ("punk", "Punk"), ("rock-halftime", "Rock Halftime"), ("latin-reggaeton", "Reggaeton")]
GROOVE_BARS = 4


def tree(repo):
    url = f"https://api.github.com/repos/{repo[0]}/git/trees/{repo[1]}?recursive=1"
    with urllib.request.urlopen(url, timeout=60) as response:
        return [item["path"] for item in json.load(response)["tree"] if item["type"] == "blob"]


def layer(paths, pattern, fraction):
    found = {}
    for p in paths:
        m = re.search(pattern + r"_v[l]?(\d+)(?:_rr(\d+))?", p)
        if m and (m.group(2) in (None, "1")):
            found[int(m.group(1))] = p
    if not found:
        raise SystemExit(f"nothing matches {pattern}")
    layers = sorted(found)
    return found[layers[min(len(layers) - 1, max(0, round(fraction * len(layers)) - 1))]]


def loudest(paths, pattern):
    """Percussion names its layers as _v<n>, as a velocity range _<low>_<high>, or not at all."""
    best = None
    for p in paths:
        m = re.search(pattern + r"(?:_v(\d+)|_\d+_(\d+))?_rr1_", p)
        if m:
            key = int(m.group(1) or m.group(2) or 0)
            if best is None or key > best[0]:
                best = (key, p)
    if best is None:
        raise SystemExit(f"nothing matches {pattern}")
    return best[1]


def cut_groove(data, bars):
    """The first `bars` bars of a 4/4 performance as notes that each end after they start.

    The recordings hold drum hits whose note-off shares the note-on's tick, which some
    readers order off-before-on (a stuck note), so every note lasts at least a 128th note.
    Notes still sounding at the end are closed there."""
    song = mido.MidiFile(file=io.BytesIO(data))
    ticks = song.ticks_per_beat
    end = ticks * 4 * bars
    shortest = max(1, ticks // 32)
    out = mido.MidiFile(type=0, ticks_per_beat=ticks)
    track = mido.MidiTrack()
    out.tracks.append(track)
    meta, notes, held = [], [], {}
    now = 0
    for message in mido.merge_tracks(song.tracks):
        now += message.time
        if now >= end:
            break
        if message.is_meta:
            if message.type in ("set_tempo", "time_signature"):
                meta.append((now, message))
        elif message.type == "note_on" and message.velocity > 0:
            key = (message.channel, message.note)
            if key in held:   # struck again before it ended: end the first hit here
                notes.append((*held.pop(key), now, now))
            held[key] = (now, message.channel, message.note, message.velocity)
        elif message.type in ("note_off", "note_on") and (message.channel, message.note) in held:
            notes.append((*held.pop((message.channel, message.note)), now, end))
    notes += [(*start, end, end) for start in held.values()]
    # (tick, order, message): at one tick, meta first, then note-offs, then note-ons.
    events = [(at, 0, m) for at, m in meta]
    for start, channel, note, velocity, stop, limit in notes:   # never past the next hit of the same note
        stop = min(limit, max(stop, start + shortest))
        if stop <= start:   # struck twice on one tick: one hit
            continue
        events.append((start, 2, mido.Message("note_on", channel=channel, note=note, velocity=velocity)))
        events.append((stop, 1, mido.Message("note_off", channel=channel, note=note, velocity=0)))
    last = 0
    for at, _, message in sorted(events, key=lambda e: (e[0], e[1], getattr(e[2], "note", -1))):
        track.append(message.copy(time=at - last))
        last = at
    track.append(mido.MetaMessage("end_of_track", time=end - last))
    buffer = io.BytesIO()
    out.save(file=buffer)
    return buffer.getvalue()

# ---- build ---------------------------------------------------------------------------------------

def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    cache = Path(sys.argv[1])
    cache.mkdir(parents=True, exist_ok=True)
    rows = []

    def write_wav(name, data, rate, source, origin):
        target = OUT / "Drums" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        sf.write(target, data, rate, subtype="PCM_16")
        rows.append(("Drums/" + name, source, SOURCES[source][1], origin))

    kit_paths = tree(VIRTUOSITY)
    for name, folder, articulation, fraction, longest in KIT:
        first = layer([p for p in kit_paths if p.startswith(f"Samples/oh/{folder}/")], f"oh_{articulation}", fraction)
        parts = [(VIRTUOSITY, first.replace("/oh/", f"/{mic}/").replace("/oh_", f"/{mic}_"), gain) for mic, gain in KIT_MICS]
        data, rate = mix(parts, cache)
        write_wav(name, finish(data, rate, longest), rate, "virtuosity", " + ".join(p for _, p, _ in parts))
    for name, folder, pattern in PERCUSSION:
        path = loudest([p for p in kit_paths if p.startswith(f"Samples/perc/close/{folder}/")], pattern)
        data, rate = mix([(VIRTUOSITY, path, 1.0)], cache)
        write_wav(name, finish(data, rate, 2.0), rate, "virtuosity", path)
    for name, path in CLAPS:
        data, rate = mix([(VCSL, path, 1.0)], cache)
        write_wav(name, finish(data, rate, 1.5), rate, "vcsl", path)

    chords = zipfile.ZipFile(fetch(CHORDS_ZIP, cache))
    for mode, names in CHORDS.items():
        for name in names:
            target = OUT / "MIDI" / "Chord Progressions" / mode / (name + ".mid")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(chords.read(f"{mode}/{name}.mid"))
            rows.append((target.relative_to(OUT).as_posix(), "chords", "MIT", f"{mode}/{name}.mid"))

    grooves = zipfile.ZipFile(fetch(GROOVE_ZIP, cache))
    info = list(csv.DictReader(io.TextIOWrapper(grooves.open("groove/info.csv"), encoding="utf-8")))
    for style, title in GROOVES:
        candidates = [r for r in info if r["style"].replace("/", "-") == style and r["beat_type"] == "beat"
                      and r["time_signature"] == "4-4" and float(r["duration"]) > 20]
        # A tempo people commonly play the style at, when the dataset has one.
        match = next((r for r in candidates if 70 <= int(r["bpm"]) <= 180), candidates[0] if candidates else None)
        if match is None:
            raise SystemExit(f"no groove for {style}")
        target = OUT / "MIDI" / "Drum Grooves" / f"{title} {match['bpm']} BPM.mid"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(cut_groove(grooves.read("groove/" + match["midi_filename"]), GROOVE_BARS))
        rows.append((target.relative_to(OUT).as_posix(), "groove", "CC-BY-4.0",
                     f"{match['midi_filename']} (first {GROOVE_BARS} bars)"))

    licenses = OUT / "LICENSES"
    licenses.mkdir(parents=True, exist_ok=True)
    (licenses / "CC0-1.0.txt").write_bytes(raw(VIRTUOSITY, "LICENSE", cache).read_bytes())
    (licenses / "MIT-free-midi-chords.txt").write_bytes(chords.read("LICENSE"))
    (licenses / "CC-BY-4.0.txt").write_bytes(fetch("https://creativecommons.org/licenses/by/4.0/legalcode.txt", cache).read_bytes())

    with open(OUT / "manifest.csv", "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(("file", "source", "license", "origin"))
        writer.writerows(sorted(rows))
    print(len(rows), "files")


if __name__ == "__main__":
    main()
