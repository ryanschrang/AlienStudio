#pragma once
#include "ProjectDocument.h"
#include <cstdint>
#include <limits>
#include <vector>

// Edits to a track's eight drum pads (the ClearDrumPad, SetDrumPadSlice, LoadDrumSample and SliceDrumPad commands).
// Pure: the controller and Preview both run them, and a refusal leaves the document as it was. A slice is a view of
// the pad's decoded source, which holds no audio of its own.
namespace alien::drum_pad {
enum class Kind { clear, range, load, slice };
struct Edit {
    Kind kind{Kind::clear};
    int track{-1}, pad{-1};
    DrumSlice range;                          // range: source frames, {0, 0} = the whole sample
    juce::String path;                        // load
    std::shared_ptr<SessionAudioClip> clip;   // load
    std::vector<std::int64_t> bounds;         // slice: source frames; N + 1 ascending bounds make N pads (2 to 8)
};

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (e.track < 0 || e.track >= d.trackCount() || e.pad < 0 || e.pad >= drumPadCount) return "Choose one of a track's eight drum pads.";
    auto& kit = d.tracks[static_cast<std::size_t>(e.track)];
    if (kit.settings.frozen()) return "Unfreeze the track before changing its samples.";
    const auto pad = static_cast<std::size_t>(e.pad);
    if (e.kind == Kind::clear) {
        kit.drumPaths[pad].clear(); kit.drumAssets[pad].reset(); kit.drumSlices[pad] = {};
        return {};
    }
    if (e.kind == Kind::load) {
        if (e.clip == nullptr) return "There is no decoded audio to load.";
        if (e.path.isEmpty()) return "A drum sample needs the path of its file.";
        kit.drumAssets[pad] = e.clip; kit.drumSlices[pad] = {}; kit.drumPaths[pad] = e.path;
        kit.settings.instrument = 1;
        return {};
    }
    const auto asset = kit.drumAssets[pad];
    if (asset == nullptr) return "Load a sample on the pad first.";
    const auto source = asset->sliceSource ? asset->sliceSource : asset;
    if (source->streamed()) return "A sample that plays from disk cannot be sliced.";
    if (e.kind == Kind::range) {
        auto range = e.range;
        if (!range.valid()) return "A slice starts at frame 0 or later and ends after its start.";
        if (!range.whole() && static_cast<std::size_t>(range.end) > source->frames) return "That slice ends after the sample does.";
        // The whole source is one canonical state, so a no-op stays a no-op.
        if (range.start == 0 && static_cast<std::size_t>(range.end) == source->frames) range = {};
        if (kit.drumSlices[pad] == range) return {};
        auto view = drumSliceView(source, range);
        if (view == nullptr) return "That slice could not be made.";
        kit.drumAssets[pad] = std::move(view); kit.drumSlices[pad] = range;
        return {};
    }
    // slice: the pad's sample is cut at the bounds, one slice to a pad from the first pad on; the other pads are cleared.
    if (kit.drumPaths[pad].isEmpty()) return "Load a sample with at least eight frames before slicing.";
    const auto count = e.bounds.size();
    if (count < 3 || count > static_cast<std::size_t>(drumPadCount) + 1) return "Slicing takes 3 to 9 bounds, which make 2 to 8 pads.";
    for (std::size_t i = 0; i < count; ++i)
        if (e.bounds[i] < 0 || e.bounds[i] > std::numeric_limits<int>::max() || static_cast<std::size_t>(e.bounds[i]) > source->frames || (i > 0 && e.bounds[i] <= e.bounds[i - 1]))
            return "Slice bounds are ascending frames inside the sample.";
    const auto path = kit.drumPaths[pad];
    std::array<std::shared_ptr<const SessionAudioClip>, drumPadCount> views{};
    std::array<DrumSlice, drumPadCount> ranges{};
    std::array<juce::String, drumPadCount> paths{};
    for (std::size_t p = 0; p + 1 < count; ++p) {
        ranges[p] = {static_cast<int>(e.bounds[p]), static_cast<int>(e.bounds[p + 1])};
        views[p] = drumSliceView(source, ranges[p]);
        if (views[p] == nullptr) return "Those slices could not be made.";
        paths[p] = path;
    }
    kit.drumAssets = std::move(views); kit.drumSlices = ranges; kit.drumPaths = std::move(paths);
    kit.settings.instrument = 1;
    return {};
}
} // namespace alien::drum_pad
