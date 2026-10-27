// The Channel Rack's patterns as clip library content (T1-03 increment C): the conversion both ways
// is exact, lanes follow channels by identity, and content that does not fit is refused.
#include "RackLibrary.h"
#include <cstdlib>
#include <iostream>

using namespace alien;
using namespace alien::rack;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

// Three channels; pattern 0 has steps with drawn values (some off, some past the length) and a note row.
static Project sample() {
    auto p = Project::createDefault();
    p.removeChannel(3);
    require(p.setPatternLength(0, 32), "a 32-step pattern");
    Step loud{true, 120, -20, 5, 80, 60, 25, 3};
    require(p.setStep(0, 0, 0, loud), "a step with every value drawn");
    Step dormant = loud; dormant.on = false;
    require(p.setStep(0, 0, 7, dormant), "an off step keeps its values");
    require(p.setStep(0, 0, 100, loud), "a step past the length is kept");
    PianoRollPattern notes; notes.loopSteps = 32;
    require(notes.add(60, 0, 2, 0.9f) != 0 && notes.add(64, 0, 2, 0.7f) != 0 && notes.add(67, 4.5, 1.25, 0.5f, 0.6f) != 0, "a chord and an off-grid note");
    require(p.setChannelNotes(0, 1, notes), "channel 2 plays notes");
    return p;
}

static void parity() {
    const auto p = sample();
    const auto data = toPatternData(p, 0);
    require(data != nullptr && data->valid() && data->lengthTicks == 32 * PianoRollPattern::ticksPerStep, "the slot becomes valid library content");
    require(data->lanes.size() == 2, "only rows that hold something become lanes (channel 3 is untouched)");
    const auto back = fromPatternData(*data, p, &p.pattern(0));
    require(back != nullptr && *back == p.pattern(0), "the pattern comes back exactly");
    require(back->steps[0][7].velocity == 120 && !back->steps[0][7].on && back->steps[0][100].ratchet == 3, "off steps and hidden steps keep every value");
    require(back->notes[1] == p.pattern(0).notes[1] && back->noteRevisions[1] == p.pattern(0).noteRevisions[1],
            "the note row is shared, not copied, and keeps its revision");
    const auto fresh = fromPatternData(*data, p, nullptr);
    require(fresh != nullptr && fresh->noteRevisions[1] != 0 && fresh->noteRevisions[1] != p.pattern(0).noteRevisions[1],
            "without the previous pattern a note row gets a new revision");
    std::puts("PASS: rack content round trip is exact");
}

static void lanesFollowChannels() {
    auto p = sample();
    const auto before = toPatternData(p, 0);
    auto moved = p;
    require(moved.moveChannel(0, 1), "move channel 1 down");
    const auto after = toPatternData(moved, 0);
    require(samePatternData(*before, *after), "moving a channel does not change the content");
    require(*fromPatternData(*after, moved, nullptr) == moved.pattern(0), "and the moved rows come back in their new places");
    auto removed = p;
    require(removed.removeChannel(0), "remove channel 1");
    require(!samePatternData(*before, *toPatternData(removed, 0)), "removing a channel removes its lane");
    require(removed.addChannel("New", "builtin:kick") && removed.lanes[2] != p.lanes[0], "a new channel gets a lane of its own");
    require(fromPatternData(*before, removed, nullptr) == nullptr, "content for a channel that is gone does not fit");
    auto copied = p;
    require(copied.duplicateChannel(1) && copied.lanes[2] != copied.lanes[1], "a duplicated channel gets its own lane");
    require(toPatternData(copied, 0)->lanes.size() == 3 && copied.pattern(0).notes[2] == copied.pattern(0).notes[1], "with a copy of the row");
    std::puts("PASS: lanes follow channels by identity");
}

static void refusals() {
    const auto p = sample();
    auto data = *toPatternData(p, 0);
    auto odd = data; odd.lengthTicks = 20 * PianoRollPattern::ticksPerStep;
    require(fromPatternData(odd, p, nullptr) == nullptr, "a length the rack cannot play is refused");
    auto audio = data; audio.lanes[0].content = library::AudioLane{library::MediaId{3}};
    require(fromPatternData(audio, p, nullptr) == nullptr, "a lane that is not a step lane is refused");
    auto other = data; std::get<library::StepLane>(other.lanes[0].content).steps[1].on = true;
    require(!samePatternData(data, other), "a changed step is a different content");
    require(toPatternData(p, 5) == nullptr, "a slot that does not exist has no content");
    std::puts("PASS: content that does not fit is refused");
}

int main() {
    parity();
    lanesFollowChannels();
    refusals();
    return 0;
}
