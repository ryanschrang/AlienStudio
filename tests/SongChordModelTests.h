#pragma once
static void chordModelTests() {
    using alien::ChordType;
    Timeline t; const auto empty = t;
    const auto a = t.addChord(0, bar, 0, ChordType::major), b = t.addChord(bar, 2 * bar, 7, ChordType::dominant7);
    const auto c = t.addChord(3 * bar, 4 * bar, 9, ChordType::minor);
    check(a && b && c && t.valid(), "chord track accepts adjacent ranges and gaps");
    check(t.chordAt(0)->id == a && t.chordAt(bar - 1)->id == a && t.chordAt(bar)->id == b
        && !t.chordAt(2 * bar) && !t.chordAt(-1) && !t.chordAt(4 * bar), "shared chord context uses exclusive ends and has no implicit chord in gaps");
    check(t != empty && t.playbackEquals(empty), "chord context affects history but does not play or retune notes");
    const auto before = t;
    check(!t.addChord(bar / 2, bar * 2, 2, ChordType::minor) && !t.addChord(-1, bar, 0, ChordType::major)
        && !t.addChord(2 * bar, 2 * bar + 1, 0, ChordType::major) && !t.addChord(0, t.length() + 1, 0, ChordType::major), "invalid chord ranges and overlaps refuse");
    check(t == before, "chord refusal is atomic");
    for (int root = 0; root < 12; ++root) for (int q = 0; q < 9; ++q) {
        Timeline one; const auto id = one.addChord(0, bar, root, static_cast<ChordType>(q));
        check(id && one.valid() && one.chordAt(0)->root == root, "all supported roots and qualities are valid");
        alien::ChordStampSettings stamp; stamp.root = 48 + root; stamp.type = one.chordAt(0)->quality;
        check(alien::chordPitches(stamp).count == (q < 6 ? 3u : 4u), "shared chord quality matches existing Piano Roll voicing vocabulary");
    }
    auto edited = *t.findChord(b); edited.root = 6; edited.quality = ChordType::minor7;
    check(t.setChord(edited) && t.findChord(b)->root == 6 && !t.setChord(edited), "chord edit preserves identity and no-op behavior");
    for (int badRoot : {-1, 12}) { auto bad = edited; bad.root = badRoot; check(!t.setChord(bad), "invalid chord root refuses"); }
    for (int badQuality : {-1, 9}) { auto bad = edited; bad.quality = static_cast<ChordType>(badQuality); check(!t.setChord(bad), "invalid chord quality refuses"); }
    auto bad = edited; bad.start = 0; check(!t.setChord(bad), "chord resize cannot overlap its neighbour");
    bad = edited; bad.end = std::numeric_limits<std::int64_t>::min(); check(!t.setChord(bad), "extreme chord end refuses without signed overflow");
    check(t.setLengthBars(1) && t.lengthBars == 4, "chord end prevents shortening Song below its content");
    const auto ranges = t.chords;
    check(t.setMeterChange(0, 3, 4) && t.chords == ranges && t.valid(), "meter edits preserve chord ticks and enough Song length");
    check(t.setTempoChange(bar, 90) && t.chords == ranges, "tempo changes do not rewrite harmonic context");
    check(t.removeChord(a) && !t.findChord(a) && !t.removeChord(a), "chord deletion is identity based");
    Timeline full; full.lengthBars = 32;
    for (std::size_t i = 0; i < maxChords; ++i) check(full.addChord(i * ticksPerStep, (i + 1) * ticksPerStep, 0, ChordType::major) != 0, "bounded chord capacity fixture fits");
    check(!full.addChord(maxChords * ticksPerStep, (maxChords + 1) * ticksPerStep, 0, ChordType::major), "chord capacity refuses without partial content");
    auto duplicate = full; duplicate.chords[1].id = duplicate.chords[0].id; check(!duplicate.valid(), "duplicate chord IDs refuse");
    duplicate = full; std::swap(duplicate.chords[0], duplicate.chords[1]); check(!duplicate.valid(), "unsorted chord ranges refuse");
}
