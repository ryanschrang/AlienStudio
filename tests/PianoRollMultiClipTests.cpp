// Multi-clip editing (PROD-05): several Session clips merged into one piano-roll pattern and each edit split
// back into the clips its notes came from, which clips take part, and the editor's switch and colours.
#include "PianoRollEditor.h"
#include "PianoRollGhostSources.h"
#include "PianoRollMultiClip.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

static std::shared_ptr<const PianoRollPattern> notes(int loopSteps, std::initializer_list<std::pair<int, double>> pitchStarts) {
    auto p = std::make_shared<PianoRollPattern>();
    p->loopSteps = loopSteps;
    for (const auto& [pitch, start] : pitchStarts) p->add(pitch, start, 2);
    return p;
}
static bool same(const PianoRollPattern& a, const PianoRollPattern& b) {
    if (a.count != b.count || a.loopSteps != b.loopSteps) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        const auto& x = a.notes[i]; const auto* y = b.find(x.id);
        if (y == nullptr || y->pitch != x.pitch || y->startStep != x.startStep || y->lengthSteps != x.lengthSteps
            || y->velocity != x.velocity || y->probability != x.probability) return false;
    }
    return true;
}
static std::uint32_t idOf(const PianoRollPattern& p, int pitch) {
    for (std::size_t i = 0; i < p.count; ++i) if (p.notes[i].pitch == pitch) return p.notes[i].id;
    return 0;
}

static void mergeAndSplit() {
    const auto bass = notes(16, {{40, 0}, {43, 8}});
    const auto chords = notes(16, {{60, 0}, {64, 0}, {67, 0}});
    const auto lead = notes(16, {{72, 4}});
    MultiClipEdit edit;
    require(edit.begin({{0, 0, bass}, {1, 0, chords}, {2, 0, lead}}) && edit.active(), "three clips of one length merge");
    const auto& merged = edit.combined();
    require(merged.count == 6 && merged.loopSteps == 16 && merged.valid(), "every note is in the merged pattern");
    require(edit.ownerOf(idOf(merged, 40)) == 0 && edit.ownerOf(idOf(merged, 64)) == 1 && edit.ownerOf(idOf(merged, 72)) == 2,
            "each note knows its clip");

    std::vector<std::shared_ptr<const PianoRollPattern>> parts;
    require(edit.split(merged, parts) && parts.size() == 3, "splitting gives one pattern per clip");
    require(same(*parts[0], *bass) && same(*parts[1], *chords) && same(*parts[2], *lead), "unedited, each clip is exactly as it was, ids and all");

    // Move a chord note, delete the lead note, add a note: only the clips concerned change.
    auto edited = merged;
    require(edited.move(idOf(edited, 64), 65, 2), "move a note of the second clip");
    require(edited.erase(idOf(edited, 72)), "delete the third clip's note");
    const auto added = edited.add(48, 12, 2);
    require(added != 0, "add a note");
    require(edit.split(edited, parts), "split the edit");
    require(same(*parts[0], *bass) == false && parts[0]->count == 3 && same(*parts[2], *notes(16, {})) , "the new note joins the opened clip; the third clip is empty");
    const auto* moved = parts[1]->find(chords->find(idOf(*chords, 64))->id);
    require(moved != nullptr && moved->pitch == 65 && moved->startStep == 2 && parts[1]->count == 3, "the moved note keeps its id in its own clip");
    const auto newId = idOf(*parts[0], 48);
    require(newId != 0 && bass->find(newId) == nullptr, "the new note has an id of its own in the opened clip");
    // A later edit of the new note keeps that id.
    require(edited.setVelocity(added, 0.4f) && edit.split(edited, parts) && idOf(*parts[0], 48) == newId
            && parts[0]->find(newId)->velocity == 0.4f, "and keeps it through later edits");
    require(edit.ownerOf(added) == 0, "a note the editor added belongs to the opened clip");

    // Refusals.
    auto other = edited; other.setLoopSteps(32);
    require(!edit.split(other, parts), "a pattern of another length is not split");
    MultiClipEdit refused;
    require(!refused.begin({{0, 0, bass}}) && !refused.active(), "one clip is not a multi-clip edit");
    require(!refused.begin({{0, 0, bass}, {1, 0, notes(32, {{60, 0}})}}), "clips of another length are not merged");
    auto full = std::make_shared<PianoRollPattern>(); full->loopSteps = 16;
    for (int i = 0; i < 1024; ++i) full->add(i % 128, (i / 128) * 2.0, 1);
    require(full->count == 1024 && !refused.begin({{0, 0, full}, {1, 0, bass}}), "more notes than one pattern holds is refused");
    edit.end();
    require(!edit.active() && edit.combined().count == 0, "ending leaves nothing behind");
    std::puts("PASS: merging clips and splitting edits back");
}

static void whichClips() {
    auto doc = ProjectDocument::createDefault(4, 2);
    const auto put = [&](int t, int s, std::shared_ptr<const PianoRollPattern> p) { SessionClip c; c.pattern = std::move(p); doc.setClip(t, s, c); };
    put(0, 0, notes(16, {{40, 0}}));
    put(1, 0, notes(16, {{60, 0}}));
    put(2, 0, notes(32, {{72, 0}}));   // another length: stays a ghost
    put(3, 0, notes(16, {{50, 0}}));
    put(0, 1, notes(16, {{41, 0}}));
    // In scene 1, track 2's cell shares track 1's content: one content, edited once.
    put(1, 1, notes(16, {{61, 0}}));
    doc.cells[1][2] = doc.library.linkClip(doc.cellClip(1, 1));
    const auto all = [](int, int) { return true; };
    auto sources = multiClipSourcesFor(doc, 0, 0, GhostNoteSource::sameScene, all);
    require(sources.size() == 3 && sources[0].track == 0 && sources[1].track == 1 && sources[2].track == 3, "the opened clip first, then the scene's clips of its length");
    sources = multiClipSourcesFor(doc, 0, 0, GhostNoteSource::sameScene, [](int track, int) { return track != 1; });
    require(sources.size() == 2 && sources[1].track == 3, "a clip the owner cannot edit (frozen, recording) is left out");
    sources = multiClipSourcesFor(doc, 0, 0, GhostNoteSource::sameTrack, all);
    require(sources.size() == 2 && sources[1].scene == 1, "or this track's clips in other scenes");
    sources = multiClipSourcesFor(doc, 0, 1, GhostNoteSource::sameScene, all);
    require(sources.size() == 2 && sources[1].track == 1, "two linked cells are one content: it is edited once");
    sources = multiClipSourcesFor(doc, 1, 1, GhostNoteSource::sameScene, all);
    require(sources.size() == 2 && sources[1].track == 0, "and a cell linked to the opened one is not a second copy of its notes");
    std::puts("PASS: which clips are edited together");
}

static void editorSwitch() {
    PianoRollEditor editor;
    editor.setSize(900, 520);
    std::vector<bool> changes;
    editor.onMultiClipChanged = [&](bool on) { changes.push_back(on); };
    require(!editor.isMultiClipEditing(), "multi-clip editing starts off");
    editor.setMultiClipEditing(true);
    editor.setMultiClipEditing(true);
    require(editor.isMultiClipEditing() && changes == std::vector<bool>{true}, "turning it on tells the owner once");
    editor.setEditingClipCount(3);
    require(editor.editingClipCount() == 3, "the editor knows how many clips it edits");
    MultiClipEdit edit;
    require(edit.begin({{0, 0, notes(16, {{40, 0}})}, {1, 0, notes(16, {{60, 0}})}}), "two clips");
    editor.setPattern(edit.combined());
    std::vector<std::uint32_t> tinted;
    editor.noteTint = [&](std::uint32_t id) -> std::optional<juce::Colour> {
        tinted.push_back(id);
        return edit.ownerOf(id) > 0 ? std::optional<juce::Colour>(juce::Colours::orange) : std::nullopt;
    };
    juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true);
    juce::Graphics g(image);
    editor.paintEntireComponent(g, false);
    require(!tinted.empty(), "painting asks which clip each note belongs to");
    editor.setMultiClipEditing(false);
    require(changes == std::vector<bool>{true, false}, "and turning it off tells the owner too");
    std::puts("PASS: the editor's multi-clip switch and note colours");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    mergeAndSplit();
    whichClips();
    editorSwitch();
    return 0;
}
