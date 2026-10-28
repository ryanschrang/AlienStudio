// The piano roll's keyboard (PROD-06, part 1): note-name labels, keys lit for selected, sounding and auditioned
// notes in their notes' colours, previews played on the edited track's own instrument, and note-offs that always
// reach the instrument that got the note-on.
#include "PianoRollAudition.h"
#include "PianoRollEditor.h"
#include "ToneEngine.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, p, now, 1, false);
}
// Where pitch's key is, for a 900 x 520 editor showing pitches 48-71 (24 rows from y 68, 326 px tall).
static juce::Point<float> keyOf(int pitch) { return {30.0f, 68.0f + (71 - pitch + 0.5f) * (326.0f / 24.0f)}; }
static juce::Image render(PianoRollEditor& editor) {
    juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true);
    juce::Graphics g(image);
    editor.paintEntireComponent(g, false);
    return image;
}

static void labelsAndLights() {
    PianoRollEditor editor;
    editor.setSize(900, 520);
    PianoRollPattern p; p.loopSteps = 16;
    const auto c = p.add(60, 0, 4), e = p.add(64, 4, 4), g = p.add(67, 8, 4);
    editor.setPattern(p);
    require(editor.getKeyLabels() == PianoRollEditor::KeyLabels::cNotes, "keys are named at every C to start with");
    // More of the keyboard is ink with every key named than with only the Cs.
    const auto ink = [&](const juce::Image& image) {
        int dark = 0;
        for (int y = 68; y < 68 + 326; ++y) for (int x = 40; x < 63; ++x) dark += image.getPixelAt(x, y).getBrightness() < 0.5f ? 1 : 0;
        return dark;
    };
    const int cOnly = ink(render(editor));
    editor.setKeyLabels(PianoRollEditor::KeyLabels::all);
    require(ink(render(editor)) > cOnly + 40, "with All notes every key is named");

    require(editor.keyLight(60).state == PianoRollEditor::KeyState::none, "no key is lit with nothing selected or playing");
    const std::uint32_t chosen[] {e};
    editor.setSelection(chosen);
    require(editor.keyLight(64).state == PianoRollEditor::KeyState::selected && editor.keyLight(60).state == PianoRollEditor::KeyState::none,
            "a selected note lights its key");
    require(editor.keyLight(64).colour == juce::Colour(0xff3fc9a2), "in the note's colour");
    // A note of another clip being edited keeps that clip's colour on its key.
    editor.noteTint = [&](std::uint32_t id) { return id == e ? std::optional<juce::Colour>(juce::Colours::orange) : std::nullopt; };
    require(editor.keyLight(64).colour == juce::Colours::orange, "and in another clip's colour when editing several");
    // Notes sounding under the playhead light their keys, brighter; when it moves on, they go out.
    editor.setPlayheadStep(9.0);
    require(editor.keyLight(67).state == PianoRollEditor::KeyState::sounding && editor.keyLight(60).state == PianoRollEditor::KeyState::none,
            "a note under the playhead lights its key");
    require(editor.keyLight(67).colour.getBrightness() > juce::Colour(0xff3fc9a2).getBrightness(), "brighter than a selected key");
    editor.setPlayheadStep(-1.0);
    require(editor.keyLight(67).state == PianoRollEditor::KeyState::none && editor.keyLight(64).state == PianoRollEditor::KeyState::selected,
            "with the transport stopped only the selection is lit");
    (void)c; (void)g;
    // Holding a key down auditions it, and lights it, until the mouse lets go.
    std::vector<std::pair<int, float>> heard;
    editor.onAudition = [&](int note, float velocity) { heard.emplace_back(note, velocity); };
    editor.mouseDown(mouse(editor, keyOf(60)));
    require(heard.size() == 1 && heard[0].first == 60 && heard[0].second > 0 && editor.keyLight(60).state == PianoRollEditor::KeyState::auditioned,
            "pressing a key plays it and lights it");
    editor.mouseDrag(mouse(editor, keyOf(62)));
    require(heard.size() == 3 && heard[1] == std::make_pair(60, 0.0f) && heard[2].first == 62, "sliding to another key moves the note");
    editor.mouseUp(mouse(editor, keyOf(62)));
    require(heard.size() == 4 && heard[3] == std::make_pair(62, 0.0f) && editor.keyLight(62).state == PianoRollEditor::KeyState::none,
            "letting go stops it");
    editor.mouseDown(mouse(editor, keyOf(55)));
    editor.focusLost(juce::Component::focusChangedDirectly);
    require(heard.back() == std::make_pair(55, 0.0f), "losing focus stops a held key");
    std::puts("PASS: keyboard labels, key lights and holding a key");
}

static void routing() {
    using Target = PianoRollAuditionRouter::Target;
    PianoRollAuditionRouter router;
    std::vector<std::tuple<Target, int, float>> sent;
    const auto send = [&](Target t, int note, float velocity) { sent.emplace_back(t, note, velocity); };
    router.play(60, 0.8f, {false, 2}, send);
    require(sent.size() == 1 && std::get<0>(sent[0]) == Target{false, 2} && router.heldOn(60) == Target{false, 2}, "a preview plays on the edited track");
    // The editor switches to a rack channel while the key is still held: the note-off still goes to track 2.
    router.play(60, 0.0f, {true, 5}, send);
    require(sent.size() == 2 && std::get<0>(sent[1]) == Target{false, 2} && std::get<2>(sent[1]) == 0.0f && router.heldOn(60).index == -1,
            "a note-off goes where its note-on went");
    router.play(62, 0.8f, {true, 5}, send);
    router.play(62, 0.6f, {true, 5}, send);
    require(sent.size() == 5 && std::get<2>(sent[3]) == 0.0f && std::get<2>(sent[4]) == 0.6f, "a note started again is released first, never doubled");
    router.play(64, 0.8f, {false, -1}, send);
    require(sent.size() == 5, "nothing plays when there is no track to play on");
    router.play(128, 0.8f, {false, 1}, send); router.play(-1, 0.8f, {false, 1}, send);
    require(sent.size() == 5, "notes outside MIDI are ignored");
    router.play(65, 0.8f, {false, 3}, send);
    router.releaseAll(send);
    require(router.heldOn(62).index == -1 && router.heldOn(65).index == -1 && std::get<2>(sent.back()) == 0.0f, "release all lets every held note go");
    std::puts("PASS: previews follow the note to where it plays");
}

// A preview pushed for track 2 is heard from track 2's own instrument, with the transport stopped, and stops on release.
static void engine() {
    auto engine = std::make_unique<ToneEngine>();
    engine->prepare(48000);
    engine->activeTracks.store(4);
    std::array<float, 512> left{}, right{};
    const auto render = [&](int blocks) {
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    };
    render(2);
    require(engine->tracks[2].peak.load() == 0.0f, "track 2 starts silent");
    engine->trackAuditions.push(ToneEngine::trackAuditionEvent(2, 60), 0.8f);
    render(8);
    require(engine->tracks[2].peak.load() > 0.01f && engine->tracks[0].peak.load() == 0.0f && engine->tracks[1].peak.load() == 0.0f,
            "the preview sounds on track 2 and nowhere else, with the transport stopped");
    engine->trackAuditions.push(ToneEngine::trackAuditionEvent(2, 60), 0.0f);
    render(200);   // past the release
    require(engine->tracks[2].peak.load() < 1.0e-4f, "releasing it stops it");
    std::puts("PASS: previews play on the track's own instrument");
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    labelsAndLights();
    routing();
    engine();
    return 0;
}
