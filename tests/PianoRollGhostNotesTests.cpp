#include "PianoRollEditor.h"
#include "PianoRollGhostSources.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p, juce::ModifierKeys mods) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &c, &c, now, p, now, 1, false);
}

static std::shared_ptr<const PianoRollPattern> notes(int loopSteps, std::initializer_list<std::pair<int, double>> pitchStarts,
                                                     double length = 2) {
    auto p = std::make_shared<PianoRollPattern>();
    p->loopSteps = loopSteps;
    for (const auto& [pitch, start] : pitchStarts) p->add(pitch, start, length);
    return p;
}

static std::vector<PianoRollGhostNote> placed(std::span<const PianoRollGhostClip> clips, int loopSteps, std::size_t limit = 8192) {
    std::vector<PianoRollGhostNote> out;
    forEachGhostNote(clips, loopSteps, [&](const PianoRollGhostNote& n) { out.push_back(n); }, limit);
    return out;
}

static juce::Image render(PianoRollEditor& editor) {
    juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true);
    juce::Graphics g(image);
    editor.paintEntireComponent(g, false);
    return image;
}

static void placementTests() {
    // A one-bar clip repeats across a 40-step loop; the last repeat is cut at the loop end.
    const std::vector<PianoRollGhostClip> bar { { notes(16, {{60, 0}, {64, 6}}, 4), 2, 1 } };
    const auto tiled = placed(bar, 40);
    require(tiled.size() == 6 && tiled[0].startStep == 0 && tiled[2].startStep == 16 && tiled[4].startStep == 32
            && tiled[0].track == 2 && tiled[0].scene == 1, "a shorter ghost clip repeats across the edited loop");
    require(tiled[3].startStep == 22 && tiled[3].lengthSteps == 4 && tiled[5].startStep == 38 && tiled[5].lengthSteps == 2,
            "a repeated note keeps its length but ends at the edited loop's end");
    const std::vector<PianoRollGhostClip> longer { { notes(128, {{60, 10}, {62, 70}}), 0, 0 } };
    const auto cut = placed(longer, 64);
    require(cut.size() == 1 && cut[0].pitch == 60, "notes past the edited loop are not shown");
    require(placed(bar, 40, 3).size() == 3, "ghost placement is bounded");
    const std::vector<PianoRollGhostClip> none { { nullptr, 0, 0 } };
    require(placed(none, 64).empty() && placed(bar, 0).empty(), "missing patterns and empty loops place nothing");
}

static void sourceTests() {
    auto doc = ProjectDocument::createDefault(4, 3);
    const auto put = [&](int t, int s, std::shared_ptr<const PianoRollPattern> p, juce::String audio = {}) {
        SessionClip c;
        c.pattern = std::move(p);
        c.audioFilePath = audio;
        doc.setClip(t, s, c);
    };
    put(0, 0, notes(64, {{60, 0}}));          // the edited clip
    put(1, 0, notes(64, {{67, 4}}));          // another track, same scene
    put(2, 0, notes(64, {{70, 8}}), "missing.wav"); // an audio clip (offline)
    put(0, 1, notes(32, {{55, 2}}));          // the same track, another scene
    put(3, 1, notes(64, {{80, 0}}));          // neither
    doc.cells[2][3] = doc.library.linkClip(doc.cellClip(0, 0));   // linked to the edited clip
    doc.cells[0][3] = doc.library.linkClip(doc.cellClip(0, 0));
    const auto before = doc;

    const auto scene = ghostClipsFor(doc, 0, 0, GhostNoteSource::sameScene);
    require(scene.size() == 1 && scene[0].track == 1 && scene[0].scene == 0 && scene[0].pattern == doc.clip(1, 0).pattern,
            "same-scene ghosts share the other tracks' MIDI clips, skipping audio, empty and linked cells");
    const auto track = ghostClipsFor(doc, 0, 0, GhostNoteSource::sameTrack);
    require(track.size() == 1 && track[0].track == 0 && track[0].scene == 1,
            "same-track ghosts are this track's clips in the other scenes");
    require(ghostClipsFor(doc, 0, 0, GhostNoteSource::off).empty(), "off shows no ghosts");
    require(ghostClipsFor(doc, 9, 0, GhostNoteSource::sameScene).empty(), "an invalid cell has no ghosts");
    require(ghostClipsFor(doc, 1, 2, GhostNoteSource::sameScene).size() == 1,
            "an empty edited cell still shows the clips beside it");
    require(sameContent(before, doc) && doc.linkedCells(0, 0) == 3, "collecting ghosts does not change the project");
}

static void editorTests() {
    PianoRollEditor editor;
    editor.setSize(1000, 500);
    PianoRollPattern own;
    const auto ownId = own.add(60, 0, 4);
    editor.setPattern(own);
    int changes = 0, commits = 0, sourceChanges = 0;
    editor.onChange = [&] { ++changes; };
    editor.onEditCommitted = [&] { ++commits; };
    editor.onGhostSourceChanged = [&](GhostNoteSource) { ++sourceChanges; };

    const std::vector<PianoRollGhostClip> clips { { notes(64, {{64, 8}}, 4), 1, 0 } };
    editor.setGhostClips(clips);
    require(editor.getGhostClips().empty() && editor.ghostNoteCount() == 0, "ghosts stay hidden while the toggle is off");
    const auto plain = render(editor);

    editor.setGhostSource(GhostNoteSource::sameScene);
    editor.setGhostSource(GhostNoteSource::sameScene);
    require(sourceChanges == 1 && editor.getGhostSource() == GhostNoteSource::sameScene,
            "choosing a ghost source asks the owner once for ghost clips");
    editor.setGhostClips(clips);
    require(editor.ghostNoteCount() == 1 && changes == 0 && commits == 0 && !editor.canUndo(),
            "showing ghost notes is not an edit and records no undo step");

    // Geometry at 1000 x 500: grid x 64..974, y 68..374, 24 rows from C5 (71) down to C3 (48).
    const float step = 910.0f / 64.0f, row = 306.0f / 24.0f;
    const juce::Point<float> ghostPoint { 64.0f + 9.0f * step, 68.0f + (71 - 64) * row + row / 2 };
    const auto withGhost = render(editor);
    const auto px = ghostPoint.roundToInt();
    require(withGhost.getPixelAt(px.x, px.y) != plain.getPixelAt(px.x, px.y), "a ghost note is painted");
    const juce::Point<float> ownPoint { 64.0f + 1.5f * step, 68.0f + (71 - 60) * row + row / 2 };
    const auto op = ownPoint.roundToInt();
    require(withGhost.getPixelAt(op.x, op.y) == plain.getPixelAt(op.x, op.y), "the edited clip's notes paint as before");
    require(withGhost.getPixelAt(px.x, px.y) != withGhost.getPixelAt(op.x, op.y), "ghosts look different from real notes");

    // A Shift-drag marquee and right-click erase see only the edited clip's notes.
    const juce::ModifierKeys shiftLeft(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    editor.mouseDown(mouse(editor, ghostPoint.translated(-30.0f, -8.0f), shiftLeft));
    editor.mouseDrag(mouse(editor, ghostPoint.translated(40.0f, 8.0f), shiftLeft));
    editor.mouseUp(mouse(editor, ghostPoint.translated(40.0f, 8.0f), shiftLeft));
    require(editor.getSelectedIds().empty() && editor.getPattern().count == 1, "a marquee over ghosts selects nothing");
    const std::array everything { ownId };
    editor.setSelection(everything);
    require(editor.getSelectedIds().size() == 1 && editor.getSelectedIds()[0] == ownId, "only the open clip's notes can be selected");
    editor.setSelection({});
    editor.mouseDown(mouse(editor, ghostPoint, juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier)));
    editor.mouseUp(mouse(editor, ghostPoint, juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier)));
    require(editor.getPattern().count == 1 && changes == 0 && commits == 0, "right-clicking a ghost erases nothing");
    // A left click on a ghost is a click on empty grid: it draws a new note of the edited clip.
    editor.mouseDown(mouse(editor, ghostPoint, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier)));
    editor.mouseUp(mouse(editor, ghostPoint, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier)));
    require(editor.getPattern().count == 2 && editor.getSelectedIds().size() == 1
            && editor.getPattern().find(editor.getSelectedIds()[0])->pitch == 64 && commits == 1
            && editor.ghostNoteCount() == 1 && editor.getGhostClips()[0].pattern == clips[0].pattern
            && clips[0].pattern->count == 1, "clicking a ghost edits only the open clip; the ghost clip is untouched");
    editor.undo();
    require(editor.getPattern().count == 1 && editor.ghostNoteCount() == 1, "undo leaves ghosts in place");

    // Ghosts follow the editor's loop, and loading a pattern keeps the owner's ghosts.
    PianoRollPattern shortLoop;
    shortLoop.loopSteps = 4;
    editor.setPattern(shortLoop);
    require(editor.ghostNoteCount() == 0, "ghosts outside a shorter edited loop are not shown");

    const auto commitsBefore = commits;
    editor.setGhostSource(GhostNoteSource::off);
    require(sourceChanges == 2 && editor.getGhostClips().empty() && commits == commitsBefore && !editor.canUndo(),
            "turning ghosts off drops them without an edit");
    editor.setGhostClips(clips);
    require(editor.getGhostClips().empty(), "an owner cannot show ghosts while the toggle is off");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    placementTests();
    sourceTests();
    editorTests();
    std::cout << "Piano-roll ghost note tests passed\n";
}
