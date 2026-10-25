#include "Timeline.h"
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
static void check(bool ok, const char* text) { if (!ok) { std::fprintf(stderr, "%s\n", text); std::exit(1); } }
int main() {
    using namespace alien::timeline;
    Timeline t;
    Region a; a.row = 0; a.source = 0; a.length = ticksPerBar;
    check(t.recordTake(a), "record first pass");
    const auto first = t.takes.front();
    a.source = 1;
    check(t.recordTake(a) && t.takes.size() == 2 && t.regions.size() == 1, "overdub retains both original passes");
    const auto originals = t.takes;
    check(t.compTake(first.id, 960, 1920), "comp middle from original pass");
    check(t.valid() && t.takes == originals && t.regions.size() == 3, "comp is non-destructive");
    const auto* before = t.regionAt(0, 0); const auto* middle = t.regionAt(0, 1000); const auto* after = t.regionAt(0, 2000);
    check(before && middle && after && before->source == 1 && middle->source == 0 && after->source == 1, "only requested range replaced");
    check(std::abs(middle->audioOffset - 0.5) < 1e-12 && std::abs(after->audioOffset - 1.0) < 1e-12, "sample offsets follow trimmed source");
    check(middle->fadeIn > 0 && middle->fadeOut > 0, "comp edges receive short fades");
    check(t.compTake(first.id, 961, 962) && t.valid(), "tiny comp clamps surviving boundary fades");
    const auto saved = t;
    check(!t.compTake(first.id, -1, 100) && !t.compTake(first.id, 0, ticksPerBar + 1)
        && !t.compTake(0, 0, 100) && t == saved, "invalid comp leaves arrangement unchanged");
    auto changed = t; changed.takes[0].gain = 0.5;
    check(!(changed == t) && changed.playbackEquals(t), "take metadata is document state, not simultaneous playback");
    changed = t; changed.tracksSwapped(0, 1); check(changed.takes[0].row == 1, "take follows track reorder");
    changed.trackDeleted(0); check(changed.takes[0].row == 0, "take follows track renumber");
    changed.sceneInserted(0); check(changed.takes[0].source == 1, "take follows scene insertion");
    changed.scenesSwapped(1, 2); check(changed.takes[0].source == 2, "take follows scene reorder");
    changed.sceneDeleted(2); check(changed.takes.size() == 1, "deleting source scene removes matching take");
    changed.trackDeleted(0); check(changed.takes.empty(), "deleting track removes its takes");
    changed = t; changed.takes[1].id = changed.takes[0].id; check(!changed.valid(), "duplicate take IDs rejected");
    changed = t; changed.takes[0].length = std::numeric_limits<std::int64_t>::max(); check(!changed.valid(), "overflowing take bounds rejected");
    Timeline full;
    for (std::size_t i=0; i<maxTakes; ++i) check(full.recordTake(a), "fill take capacity");
    const auto capped = full;
    check(!full.recordTake(a) && full == capped, "full take archive rejects recording transactionally");
    std::puts("Take comp tests passed");
}
