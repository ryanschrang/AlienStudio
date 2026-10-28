#pragma once
// Multi-clip editing (PROD-05): the piano roll edits several Session clips at once. Their notes are
// merged into one pattern for the editor, and each edit is split back into the clips the notes came
// from. A note keeps its clip and its id in that clip, so the engine sees an edit of a playing clip
// as an edit (not a new set of notes); a note added in the editor belongs to the first clip, the one
// that was opened. Only clips with that clip's loop length are merged; clips of another length stay
// ghosts, since a shorter clip repeats and its copies could not be told apart.
//
// No JUCE and no audio code. Message thread only.
#include "PianoRollSequence.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace alien {

struct MultiClipSource {
    int track{}, scene{};
    std::shared_ptr<const PianoRollPattern> pattern;
};

class MultiClipEdit {
public:
    // sources[0] is the clip that was opened. Returns false, and leaves this inactive, unless there are two or
    // more valid clips of the first one's loop length whose notes fit in one pattern (PianoRollPattern::maxNotes).
    bool begin(std::vector<MultiClipSource> sources) {
        end();
        if (sources.size() < 2 || sources[0].pattern == nullptr || !sources[0].pattern->valid()) return false;
        const int loop = sources[0].pattern->loopSteps;
        std::size_t total = 0;
        for (const auto& source : sources) {
            if (source.pattern == nullptr || !source.pattern->valid() || source.pattern->loopSteps != loop) return false;
            total += source.pattern->count;
        }
        if (total > PianoRollPattern::maxNotes) return false;
        auto merged = std::make_shared<PianoRollPattern>();
        merged->loopSteps = loop;
        for (std::size_t s = 0; s < sources.size(); ++s)
            for (std::size_t i = 0; i < sources[s].pattern->count; ++i) {
                auto note = sources[s].pattern->notes[i];
                const auto id = mergedIdBase + static_cast<std::uint32_t>(merged->count);
                owners.push_back({id, static_cast<int>(s), note.id});
                note.id = id;
                merged->notes[merged->count++] = note;
            }
        // Merged notes have ids from mergedIdBase up; a note the editor adds gets a small one (add() counts up from 1),
        // so a new note can never take the id of a note deleted in the same edit and land in that note's clip.
        if (!merged->valid()) { owners.clear(); return false; }
        combinedPattern = std::move(merged);
        clips = std::move(sources);
        return true;
    }
    void end() { clips.clear(); owners.clear(); combinedPattern.reset(); }
    bool active() const noexcept { return clips.size() >= 2 && combinedPattern != nullptr; }
    const std::vector<MultiClipSource>& sources() const noexcept { return clips; }
    // The merged notes, for the editor (empty when inactive).
    const PianoRollPattern& combined() const noexcept { return combinedPattern != nullptr ? *combinedPattern : emptyPianoRoll(); }
    // Which clip (an index into sources()) a note in the editor belongs to: the opened clip for a note it does not know.
    int ownerOf(std::uint32_t id) const noexcept {
        for (const auto& owner : owners) if (owner.combined == id) return owner.source;
        return 0;
    }
    // Splits the editor's notes back into one pattern per clip, in sources() order. A note the editor added gets an
    // id of its own in the opened clip and is remembered, so later edits of it keep that id. Returns false (and
    // changes nothing) when `edited` is not a merged pattern of this edit (another loop length, or invalid).
    bool split(const PianoRollPattern& edited, std::vector<std::shared_ptr<const PianoRollPattern>>& out) {
        if (!active() || !edited.valid() || edited.loopSteps != combinedPattern->loopSteps) return false;
        std::vector<std::shared_ptr<PianoRollPattern>> parts;
        for (std::size_t s = 0; s < clips.size(); ++s) {
            auto part = std::make_shared<PianoRollPattern>();
            part->loopSteps = edited.loopSteps;
            parts.push_back(std::move(part));
        }
        auto known = owners;
        for (std::size_t i = 0; i < edited.count; ++i) {
            auto note = edited.notes[i];
            auto found = std::find_if(known.begin(), known.end(), [&](const Owner& o) { return o.combined == note.id; });
            if (found == known.end()) {
                // New in the editor: a fresh id in the opened clip, unused by its notes and by every other new note.
                std::uint32_t fresh = 0;
                for (const auto& o : known) if (o.source == 0) fresh = std::max(fresh, o.original);
                known.push_back({note.id, 0, fresh + 1});
                found = known.end() - 1;
            }
            auto& part = *parts[static_cast<std::size_t>(found->source)];
            if (part.count == PianoRollPattern::maxNotes) return false;
            note.id = found->original;
            part.notes[part.count++] = note;
        }
        out.clear();
        for (auto& part : parts) {
            if (!part->valid()) return false;
            out.push_back(std::move(part));
        }
        owners = std::move(known);
        // Forget notes the editor no longer has, so the list does not grow with every delete.
        std::erase_if(owners, [&](const Owner& o) { return edited.find(o.combined) == nullptr; });
        return true;
    }

private:
    static constexpr std::uint32_t mergedIdBase = 0x40000000u;
    struct Owner { std::uint32_t combined; int source; std::uint32_t original; };
    std::vector<MultiClipSource> clips;
    std::vector<Owner> owners;
    std::shared_ptr<const PianoRollPattern> combinedPattern;
};

} // namespace alien
