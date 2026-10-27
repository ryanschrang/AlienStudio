#pragma once
// The Channel Rack's patterns as clip library content (T1-03 increment C; see docs/SHARED_CLIP_MODEL.md).
//
// A rack pattern slot is one clip. Its pattern holds a step lane for every channel row that holds
// anything: steps that are on, step values someone drew (on steps or off ones, inside the pattern's
// length or past it), or a note sequence, which is then authoritative while the steps stay dormant.
// A lane is keyed by its channel's stable lane id, not the channel's position, so moving a channel
// changes nothing in the content and a new channel never picks up a deleted one's row. Both
// directions are exact: rebuilding a pattern from its content gives back the same steps and notes.
//
// No JUCE and no audio code. Message thread only.
#include "ClipLibrary.h"
#include "RackModel.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace alien::rack {

inline bool defaultRow(const std::array<Step, maxSteps>& row) noexcept {
    return std::all_of(row.begin(), row.end(), [](const Step& s) { return s == Step{}; });
}

// One pattern slot as library content.
inline std::shared_ptr<const library::PatternData> toPatternData(const Project& project, int slot) {
    if (!project.validPattern(slot)) return nullptr;
    const auto& pattern = project.pattern(slot);
    auto data = std::make_shared<library::PatternData>();
    data->lengthTicks = static_cast<std::int64_t>(pattern.length) * PianoRollPattern::ticksPerStep;
    for (int c = 0; c < project.channelCount; ++c) {
        const auto i = static_cast<std::size_t>(c);
        if (pattern.notes[i] == nullptr && defaultRow(pattern.steps[i])) continue;   // nothing to keep
        data->lanes.push_back({library::LaneId{project.lanes[i]}, library::StepLane{pattern.steps[i], pattern.notes[i]}});
    }
    std::sort(data->lanes.begin(), data->lanes.end(), [](const auto& a, const auto& b) { return a.id.value < b.id.value; });
    return data;
}

// The slot's pattern rebuilt from library content and the project's lane bindings. A note row that is
// the same sequence as in `previous` keeps its revision, so queued notes of an unchanged row are not
// cancelled. Null when the content does not fit the project (a lane no channel is bound to, a lane that
// is not a step lane, an invalid length).
inline std::shared_ptr<const Pattern> fromPatternData(const library::PatternData& data, const Project& project, const Pattern* previous) {
    if (data.lengthTicks % PianoRollPattern::ticksPerStep != 0) return nullptr;
    const auto length = static_cast<int>(data.lengthTicks / PianoRollPattern::ticksPerStep);
    if (!validLength(length)) return nullptr;
    auto pattern = std::make_shared<Pattern>();
    pattern->length = length;
    std::size_t bound = 0;
    for (int c = 0; c < project.channelCount; ++c) {
        const auto i = static_cast<std::size_t>(c);
        const auto lane = std::find_if(data.lanes.begin(), data.lanes.end(), [&](const auto& l) { return l.id.value == project.lanes[i]; });
        if (lane == data.lanes.end()) continue;
        const auto* steps = std::get_if<library::StepLane>(&lane->content);
        if (steps == nullptr) return nullptr;
        ++bound;
        pattern->steps[i] = steps->steps;
        pattern->notes[i] = steps->notes;
        if (steps->notes != nullptr)
            pattern->noteRevisions[i] = previous != nullptr && previous->notes[i] == steps->notes ? previous->noteRevisions[i] : nextNoteRevision();
    }
    if (bound != data.lanes.size()) return nullptr;   // content for a channel that is not there
    return pattern;
}

// Whether two pieces of rack content are the same: same length, lanes, steps and notes.
inline bool samePatternData(const library::PatternData& a, const library::PatternData& b) {
    if (a.lengthTicks != b.lengthTicks || a.lanes.size() != b.lanes.size()) return false;
    for (std::size_t i = 0; i < a.lanes.size(); ++i) {
        const auto* x = std::get_if<library::StepLane>(&a.lanes[i].content);
        const auto* y = std::get_if<library::StepLane>(&b.lanes[i].content);
        if (a.lanes[i].id != b.lanes[i].id || x == nullptr || y == nullptr || x->steps != y->steps) return false;
        if (x->notes != y->notes && (x->notes == nullptr || y->notes == nullptr || !Pattern::sameNotes(*x->notes, *y->notes))) return false;
    }
    return true;
}

} // namespace alien::rack
