#include "PianoRollProcessing.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace alien;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static bool near(double a, double b) { return std::abs(a - b) < 1.0e-8; }
static bool sameNote(const PianoRollNote& a, const PianoRollNote& b) {
    return a.id == b.id && a.pitch == b.pitch && a.startStep == b.startStep
        && a.lengthSteps == b.lengthSteps && a.velocity == b.velocity;
}

static void selectionTests() {
    PianoRollPattern pattern;
    const auto a = pattern.add(60, 0, 1), b = pattern.add(64, 0, 1);
    PianoRollSelection selection;
    require(selection.valid() && !selection.contains(0) && !selection.add(0), "selection rejects zero IDs");
    require(selection.add(a) && selection.add(a) && selection.count == 1, "selection does not duplicate IDs");
    require(selection.toggle(b) && selection.count == 2 && !selection.toggle(a) && selection.count == 1,
            "selection toggles stable IDs");
    selection.add(123456);
    selection.retain(pattern);
    require(selection.count == 1 && selection.contains(b), "stale selection IDs are pruned");
    selection.selectAll(pattern);
    require(selection.count == 2 && selection.contains(a) && selection.contains(b), "select all includes each note");
    pattern.erase(a); selection.retain(pattern);
    require(selection.count == 1 && selection.contains(b), "deletion cannot transfer selection to another index");
    selection.clear(); require(selection.count == 0 && selection.valid(), "selection clears");
    for (std::size_t i = 0; i < PianoRollPattern::maxNotes; ++i)
        require(selection.add(static_cast<std::uint32_t>(i + 1)), "full selection fits");
    require(!selection.add(9999) && selection.valid(), "selection bounds cannot overflow");
    selection.count = PianoRollPattern::maxNotes + 1;
    require(!selection.valid() && !selection.remove(1) && !selection.add(9999), "malformed selection is safely rejected");
}

static void strumTests() {
    PianoRollPattern pattern;
    // Deliberately add a chord in a different order than its pitches.
    const auto high = pattern.add(67, 4, 8, .7f);
    const auto low = pattern.add(60, 4, 8, .7f);
    const auto mid = pattern.add(64, 4, 8, .7f);
    const auto solo = pattern.add(72, 12, 2, .7f);
    const auto other = pattern.add(55, 4, 8, .7f);
    const auto untouched = *pattern.find(other);
    PianoRollSelection selected;
    selected.add(high); selected.add(low); selected.add(mid); selected.add(solo);
    const auto original = pattern;
    StrumSettings settings; settings.delaySteps = .125; settings.velocityVariation = 0;
    auto result = processStrum(pattern, selected, settings);
    require(result.changed() && pattern.valid(), "ascending strum produces a valid edit");
    require(near(pattern.find(low)->startStep, 4) && near(pattern.find(mid)->startStep, 4.125)
        && near(pattern.find(high)->startStep, 4.25), "strum orders each chord by ascending pitch");
    require(near(pattern.find(solo)->startStep, 12), "strum resets delay for a new onset");
    require(sameNote(*pattern.find(other), untouched), "strum leaves unselected notes unchanged");
    require(pattern.find(high)->lengthSteps == 8 && pattern.find(mid)->lengthSteps == 8,
            "strum preserves duration");
    pattern = original; settings.descending = true;
    require(processStrum(pattern, selected, settings).changed()
        && near(pattern.find(high)->startStep, 4) && near(pattern.find(low)->startStep, 4.25),
        "descending strum reverses pitch direction");
    pattern = original; settings.delaySteps = 0;
    require(!processStrum(pattern, selected, settings).changed() && samePianoRollPattern(pattern, original),
            "zero strum amounts do not alter notes or IDs");
    settings.velocityVariation = .2f; settings.seed = 123;
    auto repeat = pattern;
    require(processStrum(pattern, selected, settings).changed()
        && processStrum(repeat, selected, settings).changed() && samePianoRollPattern(pattern, repeat),
        "strum velocities repeat exactly from a seed");
    for (std::size_t i = 0; i < pattern.count; ++i)
        require(std::abs(pattern.notes[i].velocity - original.notes[i].velocity) <= .200001f,
                "strum velocity variation respects amount");

    PianoRollPattern edge;
    const auto edgeLow = edge.add(60, 63, 1), edgeHigh = edge.add(67, 63, 1);
    selected.selectAll(edge); settings.delaySteps = 4; settings.velocityVariation = 0;
    require(!processStrum(edge, selected, settings).changed() && edge.valid()
        && edge.find(edgeLow)->startStep == 63 && edge.find(edgeHigh)->startStep == 63,
        "strum at the final clip edge preserves duration without invalid times");
}

static void chopTests() {
    PianoRollPattern pattern;
    const auto id = pattern.add(60, 2, 2.5, .6f);
    const auto other = pattern.add(70, 2, 4, .5f);
    const auto untouched = *pattern.find(other);
    PianoRollSelection selected; selected.add(id);
    ChopSettings settings; settings.divisionSteps = 1; settings.gate = 1;
    auto result = processChop(pattern, selected, settings);
    require(result.changed() && result.selection.count == 3 && pattern.count == 4 && pattern.valid(),
            "chop selects all fragments and preserves valid unique IDs");
    require(pattern.find(id) && pattern.find(id)->startStep == 2 && pattern.find(id)->lengthSteps == 1,
            "chop keeps the first fragment's original ID");
    require(pattern.notes[2].startStep == 3 && pattern.notes[2].lengthSteps == 1
        && pattern.notes[3].startStep == 4 && pattern.notes[3].lengthSteps == .5,
        "chop includes the partial final fragment without extending its end");
    require(pattern.notes[3].startStep + pattern.notes[3].lengthSteps == 4.5,
            "full gate preserves original final extent");
    require(sameNote(*pattern.find(other), untouched), "chop preserves unselected notes exactly");
    require(pattern.notes[2].velocity == .6f && pattern.notes[3].pitch == 60,
            "fragments preserve original pitch and velocity");

    PianoRollPattern gated;
    const auto gateId = gated.add(60, 0, 1.25);
    selected.clear(); selected.add(gateId); settings.divisionSteps = .5; settings.gate = .5;
    require(processChop(gated, selected, settings).changed() && gated.count == 3
        && near(gated.notes[0].lengthSteps, .25) && near(gated.notes[1].startStep, .5)
        && near(gated.notes[2].startStep, 1) && near(gated.notes[2].lengthSteps, .125),
        "gate shortens each slice, including the tail, while keeping its onset");
    PianoRollPattern tiny;
    tiny.add(60, 0, 2 * tiny.timeQuantum); selected.selectAll(tiny);
    settings.divisionSteps = tiny.timeQuantum; settings.gate = .05;
    require(processChop(tiny, selected, settings).changed() && tiny.count == 2
        && tiny.notes[0].lengthSteps == tiny.timeQuantum && tiny.notes[1].lengthSteps == tiny.timeQuantum,
        "small gated fragments retain the one-tick minimum");

    PianoRollPattern full;
    for (std::size_t i = 0; i < full.maxNotes; ++i) full.add(static_cast<int>(i), 0, 2);
    selected.clear(); selected.add(full.notes[0].id);
    const auto before = full;
    settings.divisionSteps = 1; settings.gate = .5;
    require(processChop(full, selected, settings).status == ProcessingStatus::capacityExceeded
        && samePianoRollPattern(full, before), "capacity failure leaves every note unchanged, including gate");
    // Failure must not advance the private ID allocator either.
    auto expected = before;
    full.erase(full.notes.back().id); expected.erase(expected.notes.back().id);
    require(full.add(80, 4, 1) == expected.add(80, 4, 1), "failed chop does not consume future note IDs");

    PianoRollPattern huge;
    huge.add(60, 0, 64); selected.selectAll(huge);
    const auto hugeBefore = huge; settings.divisionSteps = huge.timeQuantum;
    require(processChop(huge, selected, settings).status == ProcessingStatus::capacityExceeded
        && samePianoRollPattern(huge, hugeBefore), "tiny subdivision is rejected before partial expansion");
    settings.divisionSteps = 256; settings.gate = 1;
    require(!processChop(huge, selected, settings).changed() && samePianoRollPattern(huge, hugeBefore),
            "division longer than note with full gate is a no-op");
}

static void randomizeTests() {
    PianoRollPattern pattern;
    const auto a = pattern.add(60, 8, 3, .5f);
    const auto b = pattern.add(65, 12, 2, .5f);
    const auto other = pattern.add(70, 16, 4, .5f);
    PianoRollSelection selected; selected.add(a); selected.add(b);
    const auto original = pattern;
    RandomizeSettings settings; settings.timingSteps = .5; settings.velocityVariation = .2f;
    settings.pitchSemitones = 5; settings.seed = 12345;
    auto repeated = pattern;
    require(processRandomize(pattern, selected, settings).changed()
        && processRandomize(repeated, selected, settings).changed() && samePianoRollPattern(pattern, repeated),
        "randomizer is exactly repeatable from the same input and seed");
    require(sameNote(*pattern.find(other), *original.find(other)), "randomizer only alters selected notes");
    require(pattern.find(a)->pitch == 62 && pattern.find(b)->pitch == 68,
            "portable seeded pitch sequence is stable");
    for (const auto id : {a, b}) {
        const auto& note = *pattern.find(id); const auto& old = *original.find(id);
        require(std::abs(note.startStep - old.startStep) <= .5 && note.lengthSteps == old.lengthSteps
            && std::abs(note.velocity - old.velocity) <= .200001f && std::abs(note.pitch - old.pitch) <= 5,
            "random dimensions stay within requested ranges and preserve duration");
    }
    auto anotherSeed = original; ++settings.seed;
    require(processRandomize(anotherSeed, selected, settings).changed() && !samePianoRollPattern(pattern, anotherSeed),
            "different seed changes generated notes");
    pattern = original; settings.timingSteps = 0; settings.velocityVariation = 0; settings.pitchSemitones = 0;
    require(!processRandomize(pattern, selected, settings).changed() && samePianoRollPattern(pattern, original),
            "zero randomizer ranges are a true no-op");

    PianoRollPattern edge;
    edge.add(0, 0, 64, 1.0f / 127.0f); edge.add(127, 63, 1, 1);
    selected.selectAll(edge); settings.timingSteps = 4; settings.velocityVariation = 1; settings.pitchSemitones = 24;
    for (std::uint32_t seed = 0; seed < 100; ++seed) {
        auto changed = edge; settings.seed = seed;
        processRandomize(changed, selected, settings);
        require(changed.valid() && changed.notes[0].startStep == 0 && changed.notes[0].lengthSteps == 64,
                "randomize clamps pitch, velocity and timing at all clip limits");
    }
}

static void quantizeTests() {
    PianoRollPattern pattern;
    const auto a = pattern.add(60, 1.4, 2), b = pattern.add(64, 3.5, 2);
    const auto other = pattern.add(70, 5.4, 2);
    PianoRollSelection selected; selected.add(a); selected.add(b);
    const auto original = pattern;
    QuantizeSettings settings; settings.gridSteps = 1;
    require(processQuantize(pattern, selected, settings).changed() && pattern.find(a)->startStep == 1
        && pattern.find(b)->startStep == 3 && sameNote(*pattern.find(other), *original.find(other)),
        "straight quantize selects nearest grid with earlier tie and leaves other notes alone");
    require(pattern.find(a)->lengthSteps == 2 && pattern.find(a)->pitch == 60
        && pattern.find(a)->velocity == original.find(a)->velocity, "quantize preserves duration, pitch and velocity");
    require(!processQuantize(pattern, selected, settings).changed(), "already quantized notes do not create a change");
    pattern = original; settings.strength = .5;
    require(processQuantize(pattern, selected, settings).changed() && near(pattern.find(a)->startStep, 1.2)
        && near(pattern.find(b)->startStep, 3.25), "partial strength blends toward grid on stored ticks");
    pattern = original; settings.strength = 0; settings.swing = .5;
    require(!processQuantize(pattern, selected, settings).changed() && samePianoRollPattern(pattern, original),
            "zero quantize strength is a true no-op even with swing");

    PianoRollPattern swung;
    const auto early = swung.add(60, .6, 1), late = swung.add(64, 1.4, 1), even = swung.add(67, 1.9, 1);
    selected.selectAll(swung); settings.strength = 1; settings.swing = .75;
    require(processQuantize(swung, selected, settings).changed()
        && swung.find(early)->startStep == 0 && swung.find(late)->startStep == 1.75
        && swung.find(even)->startStep == 2, "quantize finds nearest actual swung point, including earlier neighbor");

    PianoRollPattern edge;
    const auto end = edge.add(60, 62.5, 1.5);
    const auto longId = edge.add(64, 0, 64);
    selected.selectAll(edge); settings.swing = .75; settings.gridSteps = 4;
    require(edge.valid() && processQuantize(edge, selected, settings).status != ProcessingStatus::invalidInput
        && edge.valid() && edge.find(end)->lengthSteps == 1.5 && edge.find(longId)->startStep == 0,
        "swung targets at clip end preserve duration and remain inside clip");
    PianoRollPattern nearEdge;
    const auto nearEnd = nearEdge.add(60, 60.5, 3.5);
    selected.selectAll(nearEdge); settings.swing = 0;
    require(processQuantize(nearEdge, selected, settings).changed()
        && nearEdge.find(nearEnd)->startStep == 60 && nearEdge.find(nearEnd)->lengthSteps == 3.5,
        "quantize chooses a real grid point before the final duration clamp");
}

static void invalidAndEmptyTests() {
    PianoRollPattern pattern; const auto id = pattern.add(60, 1, 4);
    const auto before = pattern; PianoRollSelection selected; selected.add(id);
    StrumSettings strum; strum.delaySteps = std::numeric_limits<double>::quiet_NaN();
    ChopSettings chop; chop.gate = 0;
    RandomizeSettings random; random.pitchSemitones = 25;
    QuantizeSettings quantize; quantize.gridSteps = 0;
    require(processStrum(pattern, selected, strum).status == ProcessingStatus::invalidInput
        && processChop(pattern, selected, chop).status == ProcessingStatus::invalidInput
        && processRandomize(pattern, selected, random).status == ProcessingStatus::invalidInput
        && processQuantize(pattern, selected, quantize).status == ProcessingStatus::invalidInput
        && samePianoRollPattern(pattern, before), "invalid processing options are rejected without edits");
    strum = {}; strum.velocityVariation = std::numeric_limits<float>::infinity();
    chop = {}; chop.divisionSteps = pattern.timeQuantum / 2;
    random = {}; random.timingSteps = -1;
    quantize = {}; quantize.swing = .751;
    require(processStrum(pattern, selected, strum).status == ProcessingStatus::invalidInput
        && processChop(pattern, selected, chop).status == ProcessingStatus::invalidInput
        && processRandomize(pattern, selected, random).status == ProcessingStatus::invalidInput
        && processQuantize(pattern, selected, quantize).status == ProcessingStatus::invalidInput,
        "out-of-range and non-finite dimensions are rejected");
    PianoRollSelection empty;
    require(!processStrum(pattern, empty).changed() && !processChop(pattern, empty).changed()
        && !processRandomize(pattern, empty).changed() && !processQuantize(pattern, empty).changed()
        && samePianoRollPattern(pattern, before), "empty selection never processes all notes implicitly");
    empty.add(99999);
    require(processChop(pattern, empty).selection.count == 0 && samePianoRollPattern(pattern, before),
            "stale-only selections are harmless and pruned");
    selected.ids[1] = selected.ids[0]; selected.count = 2;
    require(processChop(pattern, selected).status == ProcessingStatus::invalidInput
        && samePianoRollPattern(pattern, before), "duplicate selection IDs cannot process notes twice");
    auto invalid = before; invalid.count = invalid.maxNotes + 1;
    require(processRandomize(invalid, empty).status == ProcessingStatus::invalidInput,
            "malformed patterns are rejected without reading past storage");
}

int main() {
    selectionTests(); strumTests(); chopTests(); randomizeTests(); quantizeTests(); invalidAndEmptyTests();
    std::cout << "Piano-roll selected-note processing tests passed\n";
}
