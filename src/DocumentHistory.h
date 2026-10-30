#pragma once
#include "ProjectDocument.h"
#include <juce_core/juce_core.h>
#include <cstdint>
#include <vector>

// Project-wide undo/redo. Every committed edit stores a complete snapshot of
// the ProjectDocument. Snapshots share their (immutable) clip contents, so a
// snapshot costs roughly the size of the track/scene/clip tables, not the
// notes or audio. Message thread only.
namespace alien {

class DocumentHistory {
public:
    static constexpr std::size_t limit = 200;
    // Rapid repeats of the same continuous edit (mouse wheel on a fader,
    // typing a value) merge into one undo step within this window.
    static constexpr juce::uint32 coalesceMilliseconds = 1500;

    struct Step {
        juce::String label;
        int focusTrack{-1}, focusScene{-1}; // Clip to show when this step is undone/redone.
    };

    void reset(const ProjectDocument& document) {
        entries.clear();
        entries.push_back({document, {}, {}, nextId++, 0});
        position = 0;
    }

    // Record the document as it is after an edit. A continuous gesture supplies
    // a unique key so its updates merge even after a pause. Other edits or an
    // undo still break adjacency; a later gesture must use a new key.
    void record(const ProjectDocument& after, const juce::String& label, int focusTrack = -1, int focusScene = -1,
                const juce::String& coalesceKey = {}, bool continuous = false) {
        if (entries.empty()) { reset(after); return; }
        // Nothing actually changed (e.g. a click that did not move a control).
        if (sameContent(entries[position].document, after)) return;
        const auto now = juce::Time::getMillisecondCounter();
        auto& top = entries[position];
        if (coalesceKey.isNotEmpty() && position + 1 == entries.size() && position > 0
            && top.coalesceKey == coalesceKey && (continuous || now - top.time < coalesceMilliseconds)) {
            top.document = after;
            top.time = now;
            top.id = nextId++;
            return;
        }
        entries.resize(position + 1);
        entries.push_back({after, {label, focusTrack, focusScene}, coalesceKey, nextId++, now});
        if (entries.size() > limit + 1) entries.erase(entries.begin());
        position = entries.size() - 1;
    }

    // Media relocation is storage metadata, not an undoable edit. Apply it to
    // both undo and redo states without changing their content identities.
    template<class Fn> void relocateMedia(Fn&& relocate) {
        for (auto& entry : entries) relocate(entry.document);
    }

    bool canUndo() const noexcept { return position > 0; }
    bool canRedo() const noexcept { return position + 1 < entries.size(); }
    juce::String undoLabel() const { return canUndo() ? entries[position].step.label : juce::String{}; }
    juce::String redoLabel() const { return canRedo() ? entries[position + 1].step.label : juce::String{}; }

    // Move back one step. Returns the document to restore and the step undone.
    const ProjectDocument* undo(Step& undone) {
        if (!canUndo()) return nullptr;
        undone = entries[position].step;
        --position;
        return &entries[position].document;
    }

    const ProjectDocument* redo(Step& redone) {
        if (!canRedo()) return nullptr;
        ++position;
        redone = entries[position].step;
        return &entries[position].document;
    }

    void breakCoalescing() { if (!entries.empty()) entries[position].coalesceKey.clear(); }

    // Identifies the current state; equal ids mean identical documents.
    std::uint64_t currentId() const noexcept { return entries.empty() ? 0 : entries[position].id; }
    std::size_t size() const noexcept { return entries.size(); }

private:
    struct Entry {
        ProjectDocument document;
        Step step;
        juce::String coalesceKey;
        std::uint64_t id{};
        juce::uint32 time{};
    };
    std::vector<Entry> entries;
    std::size_t position{};
    std::uint64_t nextId{1};
};

} // namespace alien
