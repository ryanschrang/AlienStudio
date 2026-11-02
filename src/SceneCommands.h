#pragma once
#include "ProjectDocument.h"
#include <map>

namespace alien::scene_edit {
enum class Kind { create, duplicate, move, rename, remove };
struct Edit { Kind kind{}; int scene{-1}, to{-1}; juce::String name; };
inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    if (e.kind == Kind::create) return d.canAddScene() ? juce::String{} : "The Session has reached its scene limit.";
    if (e.scene < 0 || e.scene >= d.sceneCount()) return "That scene does not exist.";
    if (e.kind == Kind::duplicate && !d.canAddScene()) return "The Session has reached its scene limit.";
    if (e.kind == Kind::move && (e.to < 0 || e.to >= d.sceneCount())) return "The destination scene does not exist.";
    if (e.kind == Kind::rename && e.name.trim().isEmpty()) return "A scene name cannot be blank.";
    if (e.kind == Kind::remove) {
        if (d.sceneCount() <= 1) return "The last scene cannot be deleted.";
        for (const auto& track : d.tracks) if (track.settings.freezeScene == e.scene) return "Unfreeze the track before deleting its frozen scene.";
        for (const auto& region : d.timeline.frozenOriginals) if (region.source == e.scene) return "A frozen original still uses this scene.";
    }
    return {};
}
// Same document semantics as the controller, with no playback/history callbacks.
inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (e.kind == Kind::rename) { d.scenes[static_cast<std::size_t>(e.scene)].name = e.name.trim().substring(0, 64); return {}; }
    if (e.kind == Kind::move) {
        const int delta = e.to > e.scene ? 1 : -1;
        for (int s = e.scene; s != e.to; s += delta) {
            const int to = s + delta; d.timeline.scenesSwapped(s, to);
            for (auto& track : d.tracks) { auto& frozen = track.settings.freezeScene; if (frozen == s) frozen = to; else if (frozen == to) frozen = s; }
            std::swap(d.scenes[static_cast<std::size_t>(s)], d.scenes[static_cast<std::size_t>(to)]);
            std::swap(d.cells[static_cast<std::size_t>(s)], d.cells[static_cast<std::size_t>(to)]);
            if (d.selectedScene == s) d.selectedScene = to; else if (d.selectedScene == to) d.selectedScene = s;
        }
        return {};
    }
    if (e.kind == Kind::remove) {
        d.timeline.sceneDeleted(e.scene);
        for (auto& track : d.tracks) if (track.settings.freezeScene > e.scene) --track.settings.freezeScene;
        d.scenes.erase(d.scenes.begin() + e.scene); d.cells.erase(d.cells.begin() + e.scene); d.prune();
        if (d.selectedScene > e.scene) --d.selectedScene; d.clampSelection(); return {};
    }
    const bool copy = e.kind == Kind::duplicate; const int index = copy ? e.scene + 1 : d.sceneCount();
    std::vector<library::ClipId> row(static_cast<std::size_t>(d.trackCount()));
    if (copy) {
        const auto source = d.cells[static_cast<std::size_t>(e.scene)]; auto next = d.channelRack;
        std::map<std::uint64_t, int> copies;
        for (const auto id : source) {
            const int pattern = d.rackPatternForClip(id.value);
            if (pattern >= 0 && !copies.contains(id.value)) {
                const int slot = next.patternCount;
                if (!next.addPattern(pattern)) return "The Channel Rack has no room for an independent scene copy.";
                copies[id.value] = slot;
            }
        }
        if (!copies.empty()) {
            next.selectedPattern = d.channelRack.selectedPattern;
            if (!d.setRack(std::move(next))) return "The scene's rack content could not be copied.";
            for (std::size_t t = 0; t < row.size(); ++t) if (const auto found = copies.find(source[t].value); found != copies.end())
                row[t] = library::ClipId{d.channelRack.clips[static_cast<std::size_t>(found->second)]};
        }
        for (std::size_t t = 0; t < row.size(); ++t) if (!row[t] && source[t]) {
            row[t] = d.library.duplicateClip(source[t]); if (!row[t]) return "The scene's clip identity could not be allocated.";
        }
    }
    const auto name = copy ? d.scenes[static_cast<std::size_t>(e.scene)].name + " copy" : d.nextSceneName();
    d.timeline.sceneInserted(index);
    for (auto& track : d.tracks) if (track.settings.freezeScene >= index) ++track.settings.freezeScene;
    d.scenes.insert(d.scenes.begin() + index, SessionScene{name}); d.cells.insert(d.cells.begin() + index, std::move(row));
    if (d.selectedScene >= index && index < d.sceneCount() - 1) ++d.selectedScene;
    return {};
}
} // namespace alien::scene_edit
