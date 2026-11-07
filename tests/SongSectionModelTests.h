#pragma once
static void sectionModelTests() {
    Timeline t; const auto initial = t;
    const auto a = t.addSection(0, bar * 2, " Intro ", 3), b = t.addSection(bar * 2, bar * 4, "Verse", 5);
    check(a && b && t.valid() && t.sections[0].name == "Intro", "named sections accept adjacent ranges and canonical names");
    check(t.playbackEquals(initial) && t != initial, "sections affect document history but not playback content");
    const auto before = t;
    check(!t.addSection(bar, bar * 3, "Overlap") && !t.addSection(0, 1, "Tiny") && !t.addSection(-1, bar, "Negative")
        && !t.addSection(0, t.length() + 1, "Outside") && !t.addSection(bar * 4, bar * 5, "  "), "invalid section ranges and blank names refuse");
    check(t == before, "section refusals are atomic");
    auto edit = *t.findSection(b); edit.name = "Chorus"; edit.colour = 7;
    check(t.setSection(edit) && t.findSection(b)->name == "Chorus" && !t.setSection(edit), "section name/colour edits preserve identity and no-op behavior");
    edit.start = bar; check(!t.setSection(edit) && t.valid(), "section resize cannot overlap another range");
    edit = *t.findSection(b); edit.end = std::numeric_limits<std::int64_t>::min();
    check(!t.setSection(edit), "extreme invalid range refuses without signed overflow");
    check(t.setLengthBars(1) && t.lengthBars == 4, "section end prevents shrinking the song below its content");
    const auto ranges = t.sections;
    check(t.setMeterChange(0, 3, 4) && t.sections == ranges && t.valid(), "meter changes retain section musical ticks and enough song length");
    check(t.setTempoChange(bar, 90) && t.sections == ranges, "tempo edits do not rewrite section ranges");
    check(t.removeSection(a) && !t.findSection(a) && !t.removeSection(a), "section deletion is identity based");
    Timeline full;
    for (std::size_t i = 0; i < maxSections; ++i) check(full.addSection(i * ticksPerStep, (i + 1) * ticksPerStep, "Part") != 0, "section capacity fixture fits");
    check(!full.addSection(maxSections * ticksPerStep, (maxSections + 1) * ticksPerStep, "Extra"), "section count is bounded");
    auto bad = full; bad.sections[1].id = bad.sections[0].id; check(!bad.valid(), "duplicate section IDs refuse");
    bad = full; std::swap(bad.sections[0], bad.sections[1]); check(!bad.valid(), "unsorted section ranges refuse");
}
