// Chord detection from MIDI (CONV-03): naming chords in windows, bars and half bars, a Song region's notes, and
// how detected chords replace the chord track's chords in a range.
#include "ChordDetect.h"
#include <iostream>
#include <vector>

using namespace alien;
using namespace alien::chord_detect;
namespace tl = alien::timeline;

static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
static constexpr std::int64_t bar = tl::ticksPerBar, beat = tl::ticksPerBeat;

static void chordAt(std::vector<TimedNote>& notes, std::int64_t from, std::int64_t to, std::initializer_list<int> pitches, float velocity = 0.8f) {
    for (const int p : pitches) notes.push_back({from, to, p, velocity});
}
static bool is(const std::optional<Guess>& g, int root, ChordType quality) { return g && g->root == root && g->quality == quality; }

static void everyQuality() {
    struct Case { std::initializer_list<int> pitches; int root; ChordType quality; const char* what; };
    const Case cases[] = {
        {{48, 52, 55}, 0, ChordType::major, "C major"},
        {{57, 60, 64}, 9, ChordType::minor, "A minor"},
        {{59, 62, 65}, 11, ChordType::diminished, "B diminished"},
        {{60, 64, 68}, 0, ChordType::augmented, "C augmented"},
        {{50, 52, 57}, 2, ChordType::suspended2, "D sus2"},
        {{50, 55, 57}, 2, ChordType::suspended4, "D sus4"},
        {{43, 59, 62, 65}, 7, ChordType::dominant7, "G dominant 7"},
        {{53, 57, 60, 64}, 5, ChordType::major7, "F major 7"},
        {{50, 53, 57, 60}, 2, ChordType::minor7, "D minor 7"},
        {{40, 55, 60}, 0, ChordType::major, "C major over E (first inversion) is still C major"},
        {{66, 69, 74, 78}, 2, ChordType::major, "a doubled D major voicing"},
    };
    for (const auto& c : cases) {
        std::vector<TimedNote> notes;
        chordAt(notes, 0, bar, c.pitches);
        check(is(detect(notes, 0, bar), c.root, c.quality), c.what);
    }
}

static void ambiguityAndNoise() {
    std::vector<TimedNote> none;
    check(!detect(none, 0, bar), "silence names no chord");
    std::vector<TimedNote> one;
    chordAt(one, 0, bar, {60});
    check(!detect(one, 0, bar), "a single note names no chord");
    std::vector<TimedNote> fifth;
    chordAt(fifth, 0, bar, {48, 55});
    check(is(detect(fifth, 0, bar), 0, ChordType::major), "a bare fifth is read as the simplest chord on its root");
    // C, F and G are both C sus4 and F sus2: the bass decides.
    std::vector<TimedNote> sus;
    chordAt(sus, 0, bar, {48, 65, 67});
    check(is(detect(sus, 0, bar), 0, ChordType::suspended4), "C in the bass makes C, F, G a C sus4");
    std::vector<TimedNote> susF;
    chordAt(susF, 0, bar, {41, 60, 67});
    check(is(detect(susF, 0, bar), 5, ChordType::suspended2), "F in the bass makes it an F sus2");
    // A quick passing tone does not change the chord, and a grace note low down does not become the bass.
    std::vector<TimedNote> melody;
    chordAt(melody, 0, bar, {48, 52, 55});
    melody.push_back({beat, beat + tl::ticksPerStep, 74, 0.8f});
    melody.push_back({0, 30, 38, 0.8f});
    check(is(detect(melody, 0, bar), 0, ChordType::major), "passing and grace notes leave the chord alone");
    // No chord in a cluster that has no clear root.
    std::vector<TimedNote> cluster;
    chordAt(cluster, 0, bar, {60, 61, 62, 63, 64, 65, 66});
    check(!detect(cluster, 0, bar), "a chromatic cluster names no chord");
    // The same notes always give the same answer.
    const auto a = detect(melody, 0, bar), b = detect(melody, 0, bar);
    check(a && b && a->root == b->root && a->quality == b->quality && a->score == b->score, "detection is deterministic");
}

static void ranges() {
    std::vector<tl::MeterChange> fourFour;
    const tl::MeterMap meters{fourFour};
    std::vector<TimedNote> notes;
    chordAt(notes, 0, 2 * bar, {48, 52, 55});            // C for two bars: one chord
    chordAt(notes, 2 * bar, 2 * bar + 2 * beat, {45, 57, 60, 64});   // A minor, then
    chordAt(notes, 2 * bar + 2 * beat, 3 * bar, {41, 57, 60, 65});   // F major in the same bar
    chordAt(notes, 4 * bar, 5 * bar, {43, 59, 62, 65});   // bar 4 is silent; G7 in bar 5
    const auto chords = detectRange(notes, 0, 5 * bar, meters);
    check(chords.size() == 4, "four chords: C, Am, F and G7");
    if (chords.size() == 4) {
        check(chords[0].start == 0 && chords[0].end == 2 * bar && chords[0].root == 0, "two bars of C are one chord");
        check(chords[1].start == 2 * bar && chords[1].end == 2 * bar + 2 * beat && chords[1].quality == ChordType::minor, "a change in mid-bar splits it");
        check(chords[2].start == 2 * bar + 2 * beat && chords[2].end == 3 * bar && chords[2].root == 5, "the second half has its own chord");
        check(chords[3].start == 4 * bar && chords[3].end == 5 * bar && chords[3].quality == ChordType::dominant7, "a silent bar is left without a chord");
    }
    // A range that starts and ends inside bars cuts the chords there; a sliver under a sixteenth is dropped.
    const auto cut = detectRange(notes, bar / 2, 2 * bar - 100, meters);
    check(cut.size() == 1 && cut[0].start == bar / 2 && cut[0].end == 2 * bar - 100, "chords are cut to the range");
    std::vector<TimedNote> edge;
    chordAt(edge, 0, bar, {48, 52, 55});
    chordAt(edge, bar, 2 * bar, {43, 59, 62});
    const auto sliver = detectRange(edge, bar - 100, 2 * bar, meters);
    check(sliver.size() == 1 && sliver[0].start == bar && sliver[0].root == 7, "a sliver of a bar shorter than a sixteenth gets no chord");
    // A sustained F major 7 stays one chord: its halves are the same chord.
    std::vector<TimedNote> seventh;
    chordAt(seventh, 0, bar, {41, 57, 60, 64});
    const auto held = detectRange(seventh, 0, bar, meters);
    check(held.size() == 1 && held[0].root == 5 && held[0].quality == ChordType::major7, "a held seventh chord is not split");
    // A melody moving over a held C triad does not split the bar.
    std::vector<TimedNote> moving;
    chordAt(moving, 0, bar, {48, 52, 55});
    moving.push_back({0, 2 * beat, 72, 0.6f}); moving.push_back({2 * beat, bar, 74, 0.6f});
    const auto over = detectRange(moving, 0, bar, meters);
    check(over.size() == 1 && over[0].root == 0 && over[0].quality == ChordType::major, "a melody over a held chord leaves one chord");
    // 3/4: bars of three beats, never split in half.
    std::vector<tl::MeterChange> waltz{{0, 3, 4}};
    const tl::MeterMap three{waltz};
    std::vector<TimedNote> w;
    chordAt(w, 0, 3 * beat, {48, 52, 55});
    chordAt(w, 3 * beat, 6 * beat, {43, 59, 62});
    const auto inThree = detectRange(w, 0, 6 * beat, three);
    check(inThree.size() == 2 && inThree[0].end == 3 * beat && inThree[1].root == 7, "bars follow the song's time signature");
}

static void regionProjection() {
    PianoRollPattern pattern;   // one bar: C at the start, held two beats
    pattern.loopSteps = 16;
    pattern.add(60, 0, 8, 0.7f);
    tl::Region r;
    r.start = 4 * bar; r.length = 3 * bar; r.offset = bar / 2;   // starts half way through the clip
    const auto notes = regionNotes(r, pattern);
    check(notes.size() == 3, "the clip repeats across the region");
    if (notes.size() == 3) {
        check(notes[0].start == 4 * bar + bar / 2 && notes[0].end == 4 * bar + bar, "the offset moves the loop; the first note is the next repeat's");
        check(notes[2].start == 6 * bar + bar / 2 && notes[2].end == 7 * bar && notes[2].velocity == 0.7f, "notes are cut at the region's end");
    }
    PianoRollPattern crossing;
    crossing.loopSteps = 16;
    crossing.add(62, 4, 12);
    tl::Region trimmed;
    trimmed.start = 0; trimmed.length = bar; trimmed.offset = bar / 2;
    const auto clipped = regionNotes(trimmed, crossing);
    check(!clipped.empty() && clipped[0].start == 0 && clipped[0].end == bar / 2, "a note that started before the region's left edge is cut there");
    PianoRollPattern dense;
    dense.loopSteps = 1;
    for (int i = 0; i < 64; ++i) dense.add(40 + i, 0, 1);
    tl::Region huge;
    huge.length = 16384 * bar;
    check(regionNotes(huge, dense).empty(), "a projection past the note limit is refused");
    check(regionNotes(r, PianoRollPattern{}).empty(), "an empty clip has no notes");
}

static void replacing() {
    const std::vector<Chord> detected{{2 * bar, 3 * bar, 0, ChordType::major}, {3 * bar, 4 * bar, 7, ChordType::major}};
    std::vector<tl::SongChord> existing{
        {1, 0, 2 * bar + bar / 2, 9, ChordType::minor},         // crosses the left edge: cut
        {2, 3 * bar, 3 * bar + beat, 4, ChordType::minor},      // inside: removed
        {3, 3 * bar + 2 * beat, 6 * bar, 5, ChordType::major},  // crosses the right edge: cut
        {4, 7 * bar, 8 * bar, 2, ChordType::minor},              // outside: left alone
    };
    // existing must not overlap: chord 2 ends before chord 3 starts.
    const auto p = plan(existing, 2 * bar, 4 * bar, detected);
    check(p.removed.size() == 1 && p.removed[0] == 2, "a chord inside the range is removed");
    check(p.edited.size() == 2 && p.edited[0].id == 1 && p.edited[0].end == 2 * bar && p.edited[1].id == 3 && p.edited[1].start == 4 * bar,
          "chords that cross the edges are cut there and keep their ids");
    check(p.added.size() == 2 && p.added[0].start == 2 * bar, "the detected chords are added");

    const std::vector<tl::SongChord> spanning{{9, 0, 8 * bar, 2, ChordType::minor7}};
    const auto split = plan(spanning, 2 * bar, 4 * bar, detected);
    check(split.edited.size() == 1 && split.edited[0].end == 2 * bar && split.added.size() == 3
          && split.added[0].start == 4 * bar && split.added[0].end == 8 * bar && split.added[0].quality == ChordType::minor7,
          "a chord across the whole range keeps both outer parts");

    const std::vector<tl::SongChord> slivers{{5, 2 * bar - 100, 2 * bar + beat, 2, ChordType::major}};
    const auto gone = plan(slivers, 2 * bar, 4 * bar, detected);
    check(gone.removed.size() == 1 && gone.edited.empty(), "a part shorter than a sixteenth is removed, not kept");

    const std::vector<tl::SongChord> same{{6, 2 * bar, 3 * bar, 0, ChordType::major}, {7, 3 * bar, 4 * bar, 7, ChordType::major}};
    check(!plan(same, 2 * bar, 4 * bar, detected).changes(), "detecting what is already there changes nothing");
}

int main() {
    everyQuality();
    ambiguityAndNoise();
    ranges();
    regionProjection();
    replacing();
    if (failures != 0) { std::cerr << failures << " chord detection checks failed\n"; return 1; }
    std::cout << "Chord detection: qualities, bass, noise, bars and half bars, meters, region notes and replacement passed\n";
}
