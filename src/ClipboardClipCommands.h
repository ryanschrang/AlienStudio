#pragma once
#include "ProjectDocument.h"
#include <optional>

namespace alien::clipboard_clip {
struct Clipboard { library::ClipId source{}; std::optional<SessionClip> content; };
enum class Kind { paste, link, unique };
struct Edit { Kind kind{}; int track{-1}, scene{-1}; };
inline juce::String refusal(const ProjectDocument& d, const Edit& e, const Clipboard* clipboard) {
    if (!d.isValidCell(e.track, e.scene)) return "The Session cell does not exist.";
    if (d.tracks[static_cast<std::size_t>(e.track)].settings.frozen()) return "Unfreeze the destination track before editing its clips.";
    if (e.kind == Kind::unique) return d.cellClip(e.track, e.scene) ? juce::String{} : "Choose an existing Session clip to make independent.";
    if (!clipboard || !clipboard->content) return "Copy a Session clip before pasting.";
    if (e.kind == Kind::link && !d.library.clip(clipboard->source)) return "The copied source no longer exists; copy it again before linking.";
    if (clipboard->content->rackClip && d.rackPatternForClip(clipboard->content->rackClip) < 0) return "The copied rack source no longer exists.";
    return {};
}
// Pure reducer; the caller supplies a clipboard snapshot captured before the
// batch. A failed rack allocation/binding restores the whole document.
inline juce::String apply(ProjectDocument& d, const Edit& e, const Clipboard* clipboard) {
    if (const auto why = refusal(d, e, clipboard); why.isNotEmpty()) return why;
    const bool unique = e.kind == Kind::unique;
    const auto content = unique ? d.clip(e.track, e.scene) : *clipboard->content;
    if (content.rackClip) {
        if (e.kind == Kind::link && d.cellClip(e.track, e.scene).value == content.rackClip) return {};
        const auto before = d;
        auto fail = [&]() -> juce::String { d = before; return "The rack source could not be copied or assigned (capacity or Song cell reference)."; };
        auto id = content.rackClip;
        if (e.kind != Kind::link) {
            auto next = d.channelRack; const int slot = next.patternCount;
            if (!next.addPattern(d.rackPatternForClip(id))) return fail();
            next.selectedPattern = d.channelRack.selectedPattern;
            if (!d.setRack(std::move(next))) return fail();
            id = d.channelRack.clips[static_cast<std::size_t>(slot)];
        }
        return d.bindRackClip(e.track, e.scene, id) ? juce::String{} : fail();
    }
    if (unique) {
        const auto id = d.cellClip(e.track, e.scene); const auto* clip = d.library.clip(id);
        return clip && (d.library.usesOf(clip->pattern) < 2 || d.library.makeClipUnique(id))
            ? juce::String{} : "The clip could not be made independent.";
    }
    if (e.kind == Kind::link) {
        const auto* source = d.library.clip(clipboard->source);
        if (d.linkOf(e.track, e.scene) == source->pattern.value) return {};
        const auto id = d.library.linkClip(clipboard->source);
        if (!id) return "The linked clip could not be allocated.";
        d.cells[static_cast<std::size_t>(e.scene)][static_cast<std::size_t>(e.track)] = id; d.prune();
    } else {
        d.clearClip(e.track, e.scene);
        d.setClip(e.track, e.scene, content, true);
    }
    return {};
}
} // namespace alien::clipboard_clip
