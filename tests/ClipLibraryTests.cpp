// The clip library (T1-03 increment A): ids, revisions, linked and independent copies,
// stale edits, invalid references and all-or-nothing operations. No JUCE: runs in a moment.
#include "ClipLibrary.h"
#include <cstdio>
#include <memory>
#include <string>

using namespace alien::library;

static int failures = 0;
static void check(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message.c_str()); }
}

static std::shared_ptr<const PatternData> notesPayload(int pitch, int loopSteps = 16) {
    auto notes = std::make_shared<alien::PianoRollPattern>();
    notes->loopSteps = loopSteps;
    if (notes->add(pitch, 0, 2, 0.8f) == 0) check(false, "fixture: a note");
    auto data = std::make_shared<PatternData>();
    data->lengthTicks = static_cast<std::int64_t>(loopSteps) * alien::PianoRollPattern::ticksPerStep;
    data->lanes.push_back({LaneId{1}, NoteLane{std::move(notes)}});
    return data;
}

static void idTests() {
    check(toHex(std::uint64_t{0x1a2b}) == "0000000000001a2b" && toHex(~std::uint64_t{0}) == "ffffffffffffffff", "ids are saved as 16 hex digits");
    check(parseId<ClipKind>("0000000000001A2B").value_or(ClipId{}).value == 0x1a2b, "either case reads back");
    check(!parseId<ClipKind>("0000000000000000") && !parseId<ClipKind>("1a2b") && !parseId<ClipKind>("00000000000001a2b")
          && !parseId<ClipKind>("000000000000001g") && !parseId<ClipKind>(" 000000000001a2b"), "zero, the wrong length or a stray character is refused");
    for (const std::uint64_t value : {std::uint64_t{1}, std::uint64_t{0x0123456789abcdef}, ~std::uint64_t{0}})
        check(parseId<PatternKind>(toHex(value)).value_or(PatternId{}).value == value, "an id survives saving: " + toHex(value));
    check(!PatternId{} && PatternId{3} && PatternId{3} < PatternId{4}, "zero is no id; ids order by value");

    Library library;
    const auto a = library.addClip(notesPayload(60)), b = library.addClip(notesPayload(62));
    const auto pa = library.clip(a)->pattern, pb = library.clip(b)->pattern;
    check(a && b && a != b && pa != pb && pa.value != a.value && pa.value != b.value && pb.value != a.value, "every new id is new, across kinds too");
    check(library.valid() && library.patternCount() == 2 && library.clipCount() == 2, "two clips, two patterns");
}

static void copyAndLinkTests() {
    Library library;
    const auto verse = library.addClip(notesPayload(60), {"Verse", 2});
    const auto pattern = library.clip(verse)->pattern;
    // An independent copy starts with the same payload but is its own pattern.
    const auto copy = library.duplicateClip(verse);
    check(copy && library.clip(copy)->pattern != pattern && library.content(copy) == library.content(verse)
          && library.clip(copy)->info == library.clip(verse)->info, "a duplicate has its own pattern with the same content and name");
    check(!library.linked(verse, copy) && library.usesOf(pattern) == 1, "and is not linked");
    const auto before = library.content(verse);
    check(library.editPattern(library.clip(copy)->pattern, 1, notesPayload(64)) == EditResult::done
          && library.content(verse) == before && library.content(copy) != before, "editing the copy leaves the original alone");
    // A linked copy shares the pattern: an edit reaches both.
    const auto chorus = library.linkClip(verse);
    check(chorus && library.linked(verse, chorus) && library.usesOf(pattern) == 2, "a linked duplicate uses the same pattern");
    check(library.setClipInfo(chorus, {"Chorus", 5}) && library.clip(verse)->info.name == "Verse", "its name and colour are its own");
    check(library.editPattern(pattern, 1, notesPayload(67)) == EditResult::done && library.content(verse) == library.content(chorus)
          && library.pattern(pattern)->revision == 2, "an edit of the pattern reaches every linked clip, as a new revision");
    // Make unique: the clip gets its own pattern holding what it plays now; the other keeps the old one.
    const auto now = library.content(chorus);
    check(library.makeClipUnique(chorus) && !library.linked(verse, chorus) && library.clip(verse)->pattern == pattern
          && library.content(chorus) == now && library.pattern(library.clip(chorus)->pattern)->revision == 1,
          "Make unique forks the clip onto a new pattern with its current content");
    check(!library.makeClipUnique(chorus) && !library.makeClipUnique(verse) && !library.makeClipUnique(ClipId{999}),
          "a clip that shares nothing, or no clip at all, is already unique");
    check(library.editPattern(library.clip(chorus)->pattern, 1, notesPayload(48)) == EditResult::done
          && library.content(verse) == now, "after it, edits stay apart");
    check(library.valid(), "the library stays valid");
}

static void identityIsNotContentTests() {
    Library library;
    // One immutable empty payload behind two patterns does not make them one pattern.
    const auto a = library.addClip(emptyPayload()), b = library.addClip(emptyPayload());
    check(library.content(a) == library.content(b) && !library.linked(a, b), "two empty clips sharing a payload are not linked");
    check(library.editPattern(library.clip(a)->pattern, 1, notesPayload(60)) == EditResult::done
          && library.content(b) == emptyPayload(), "filling one leaves the other empty");
    // Equal notes in separate patterns are separate too.
    const auto c = library.addClip(notesPayload(72)), d = library.addClip(notesPayload(72));
    check(!library.linked(c, d) && library.clip(c)->pattern != library.clip(d)->pattern, "equal notes are not shared identity");
}

static void revisionTests() {
    Library library;
    const auto clip = library.addClip(notesPayload(60));
    const auto pattern = library.clip(clip)->pattern;
    const auto snapshot = library;
    check(library.editPattern(pattern, 1, notesPayload(62)) == EditResult::done && library.pattern(pattern)->revision == 2, "an edit makes revision 2");
    const auto edited = library;
    check(library.editPattern(pattern, 1, notesPayload(64)) == EditResult::stale && library == edited,
          "an edit based on an old revision is refused and changes nothing");
    check(library.editPattern(pattern, 2, library.pattern(pattern)->data) == EditResult::unchanged && library == edited,
          "the same content again is no new revision");
    check(library.editPattern(PatternId{12345}, 1, notesPayload(60)) == EditResult::missing && library == edited, "an unknown pattern");
    check(library.editPattern(pattern, 2, nullptr) == EditResult::invalid && library == edited, "no content");
    library = snapshot;
    check(library.pattern(pattern)->revision == 1 && library.content(clip) == snapshot.content(clip), "a copy of the library is an undo snapshot");
}

static void validityTests() {
    Library library;
    auto bad = std::make_shared<PatternData>(*notesPayload(60));
    bad->lengthTicks += 1;
    check(!library.addClip(bad) && !library.addPattern(nullptr) && library.patternCount() == 0 && library.clipCount() == 0,
          "content whose length does not match its notes, or no content, adds nothing");
    auto lanes = std::make_shared<PatternData>(*notesPayload(60));
    lanes->lanes.push_back(lanes->lanes.front());
    check(!lanes->valid(), "two lanes with one id are invalid");
    lanes->lanes.back().id = LaneId{};
    check(!lanes->valid(), "a lane without an id is invalid");
    check(!library.addClip(PatternId{77}), "a clip needs an existing pattern");
    // A Channel Rack row keeps every step value and its authoritative notes.
    auto rack = std::make_shared<PatternData>();
    rack->lengthTicks = 32 * alien::PianoRollPattern::ticksPerStep;
    StepLane kick; kick.steps[0].on = true; kick.steps[100].velocity = 7;   // a dormant value past the length
    StepLane bass; bass.notes = std::get<NoteLane>(notesPayload(36, 32)->lanes[0].content).notes;   // a row that plays notes
    rack->lanes = {{LaneId{1}, kick}, {LaneId{2}, bass}, {LaneId{3}, AudioLane{MediaId{9}}}};
    check(rack->valid(), "rack rows with steps, dormant values and notes, and an audio lane, are valid content");
    const auto clip = library.addClip(rack);
    check(clip && std::get<StepLane>(library.content(clip)->lanes[0].content).steps[100].velocity == 7, "nothing is flattened away");
    auto wrong = std::make_shared<PatternData>(*rack);
    wrong->lengthTicks = 48 * alien::PianoRollPattern::ticksPerStep;
    check(!wrong->valid(), "a rack row needs a rack length (16, 32, 64 or 128 steps) that matches its notes");
    wrong = std::make_shared<PatternData>(*rack);
    std::get<StepLane>(wrong->lanes[0].content).steps[3].velocity = 0;
    check(!wrong->valid(), "an invalid step value is invalid content");
    wrong = std::make_shared<PatternData>(*rack);
    wrong->lanes[2].content = AudioLane{};
    check(!wrong->valid(), "audio needs a media id");
    check(library.valid(), "the library is valid");
}

static void removalTests() {
    Library library;
    const auto a = library.addClip(notesPayload(60));
    const auto linked = library.linkClip(a);
    const auto other = library.addClip(notesPayload(62));
    const auto shared = library.clip(a)->pattern;
    check(library.removeClip(a) && !library.removeClip(a) && library.pattern(shared) != nullptr && library.content(linked) != nullptr,
          "removing a clip keeps its pattern for the clips (and undo) still using it");
    check(library.prune({linked, ClipId{4242}}) == 2 && library.clip(other) == nullptr && library.clipCount() == 1
          && library.patternCount() == 1 && library.pattern(shared) != nullptr, "pruning keeps the roots and what they use");
    check(library.prune({}) == 2 && library.patternCount() == 0 && library.clipCount() == 0 && library.valid(), "no roots: nothing left");
    // Removed ids are never handed out again.
    const auto fresh = library.addClip(notesPayload(60));
    check(fresh.value > other.value, "ids are not reused after removal");
}

int main() {
    {
        Library live;
        auto restored = live;
        const auto issued = live.addClip(notesPayload(60));
        restored.retainIssuedIdsFrom(live);
        const auto fresh = restored.addClip(notesPayload(62));
        check(fresh.value > issued.value && restored.clip(issued) == nullptr && restored.valid(),
              "restoring an older snapshot cannot reissue an identity held by playback");
    }
    idTests();
    copyAndLinkTests();
    identityIsNotContentTests();
    revisionTests();
    validityTests();
    removalTests();
    if (failures == 0) std::puts("ClipLibraryTests passed");
    return failures == 0 ? 0 : 1;
}
