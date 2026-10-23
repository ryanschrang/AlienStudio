#pragma once
// The Arrangement screen: a horizontal timeline where Session clips (MIDI and
// audio) and Channel Rack patterns are placed, moved, trimmed, split, copied and
// grouped. It also shows the loop range and named markers, and starts playback
// from wherever you click. Everything the user does goes through
// ProjectController::editTimeline, so undo, saving and the audio engine need
// nothing from this file; a drag is previewed on a copy and committed once, so
// each gesture is one undo step.
#include "ClipBin.h"
#include "ProjectController.h"
#include "StudioWidgets.h"
#include "Timeline.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

namespace alien {

namespace tlui {
// How far a dragged edge or a placed clip is pulled to the grid.
enum class Snap { automatic, bar, halfBar, beat, eighth, sixteenth, off };
inline constexpr std::array<Snap, 7> allSnaps{Snap::automatic, Snap::bar, Snap::halfBar, Snap::beat, Snap::eighth, Snap::sixteenth, Snap::off};
inline const char* snapName(Snap s) {
    switch (s) {
    case Snap::automatic: return "Auto (follows zoom)"; case Snap::bar: return "1 bar"; case Snap::halfBar: return "1/2 bar";
    case Snap::beat: return "1 beat"; case Snap::eighth: return "1/8 note"; case Snap::sixteenth: return "1/16 note"; case Snap::off: return "Off";
    }
    return "";
}
inline std::int64_t snapSize(Snap s) {
    switch (s) {
    case Snap::bar: return timeline::ticksPerBar; case Snap::halfBar: return timeline::ticksPerBar / 2;
    case Snap::beat: return timeline::ticksPerBeat; case Snap::eighth: return timeline::ticksPerBeat / 2;
    case Snap::sixteenth: return timeline::ticksPerStep; default: return 0;
    }
}
} // namespace tlui

class TimelineCanvas final : public juce::Component, public juce::DragAndDropTarget {
public:
    static constexpr int headerWidth = 156, markerStripHeight = 20, loopStripHeight = 16, rulerHeight = 24;
    static constexpr int topHeight = markerStripHeight + loopStripHeight + rulerHeight;
    static constexpr int rowHeight = 46, edgeGrab = 6, dragThreshold = 3;
    static constexpr double minPixelsPerTick = 0.00001, maxPixelsPerTick = 0.6, defaultPixelsPerTick = 64.0 / 3840.0;

    enum class Zone { none, corner, header, markerStrip, loopStrip, ruler, region, lane };
    enum class Edge { none, left, right };
    struct Hit {
        Zone zone{Zone::none};
        int displayRow{-1};
        std::uint32_t region{0}, marker{0};
        Edge edge{Edge::none};
        enum class LoopPart { none, start, end, body } loopPart{LoopPart::none};
    };

    explicit TimelineCanvas(ProjectController& projectController) : controller(projectController) {
        setName("Arrangement timeline");
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);
    }

    // ---- what the owner hears about ---------------------------------------------------------------
    std::function<void(std::int64_t)> onSeek;                  // the play position was set (a click on the ruler, a marker...)
    std::function<void()> onPlayToggle;                        // Space
    std::function<void(int row, int source)> onOpenSource;     // double-click on a region: edit what it plays
    std::function<void(int track, int scene, bool sceneAdded)> onClipCreated;   // a new empty clip was made for a track (a scene is added when none was free)
    std::function<void(const juce::String&)> onMessage;
    std::function<void()> onViewChanged;                       // zoom or scroll changed
    std::function<void()> onStateChanged;                      // selection, cursor, loop or length changed
    std::function<void(juce::PopupMenu, juce::Rectangle<int>, std::function<void(int)>)> menuLauncher;   // seams: the tests answer for the user
    std::function<void(const juce::String&, const juce::String&, std::function<void(const juce::String&)>)> nameRequester;

    // ---- the document, as the view sees it ------------------------------------------------------------
    const timeline::Timeline& committed() const noexcept { return controller.timeline(); }
    const timeline::Timeline& shown() const noexcept { return preview ? *preview : controller.timeline(); }
    int trackCount() const noexcept { return controller.document().trackCount(); }
    int displayRowCount() const noexcept { return trackCount() + timeline::rackLanes; }
    int timelineRow(int displayRow) const noexcept { return displayRow < trackCount() ? displayRow : timeline::rackRowBase + (displayRow - trackCount()); }
    int displayRowOf(int row) const noexcept { return timeline::isPatternRow(row) ? trackCount() + (row - timeline::rackRowBase) : row; }

    // ---- geometry (public so the tests and the owner share it) --------------------------------------------
    double pixelsPerTick() const noexcept { return ppt; }
    double scrollTicks() const noexcept { return scrollX; }
    int scrollY() const noexcept { return scrollYpx; }
    int laneAreaWidth() const noexcept { return std::max(1, getWidth() - headerWidth); }
    int laneAreaHeight() const noexcept { return std::max(1, getHeight() - topHeight); }
    double visibleTicks() const noexcept { return laneAreaWidth() / ppt; }
    std::int64_t contentTicks() const noexcept {
        const auto& t = committed();
        return std::max<std::int64_t>(t.length(), t.contentEnd()) + timeline::ticksPerBar;
    }
    double maxScrollTicks() const noexcept { return std::max(0.0, static_cast<double>(contentTicks()) - visibleTicks()); }
    int contentHeight() const noexcept { return displayRowCount() * rowHeight; }
    int maxScrollY() const noexcept { return std::max(0, contentHeight() - laneAreaHeight()); }
    double xForTick(double tick) const noexcept { return headerWidth + (tick - scrollX) * ppt; }
    double tickAtX(double x) const noexcept { return scrollX + (x - headerWidth) / ppt; }
    int rowTop(int displayRow) const noexcept { return topHeight + displayRow * rowHeight - scrollYpx; }
    int displayRowAt(int y) const noexcept {
        if (y < topHeight) return -1;
        const int row = (y - topHeight + scrollYpx) / rowHeight;
        return row >= 0 && row < displayRowCount() ? row : -1;
    }
    juce::Rectangle<int> rowBounds(int row) const noexcept { return {0, rowTop(displayRowOf(row)), getWidth(), rowHeight}; }
    juce::Rectangle<float> regionBounds(const timeline::Region& r) const noexcept {
        const float x0 = static_cast<float>(xForTick(static_cast<double>(r.start))), x1 = static_cast<float>(xForTick(static_cast<double>(r.end())));
        return {x0, static_cast<float>(rowTop(displayRowOf(r.row)) + 3), std::max(1.0f, x1 - x0), static_cast<float>(rowHeight - 6)};
    }
    juce::Rectangle<float> regionBounds(std::uint32_t id) const noexcept {
        const auto* r = shown().find(id);
        return r != nullptr ? regionBounds(*r) : juce::Rectangle<float>{};
    }
    juce::Rectangle<float> markerBounds(const timeline::Marker& m) const {
        const float x = static_cast<float>(xForTick(static_cast<double>(m.tick)));
        const float width = std::max(24.0f, 12.0f + 6.2f * static_cast<float>(juce::String::fromUTF8(m.name.c_str()).length()));
        return {x, 2.0f, width, static_cast<float>(markerStripHeight - 4)};
    }
    juce::Rectangle<float> loopBounds() const noexcept {
        const auto& t = shown();
        const float x0 = static_cast<float>(xForTick(static_cast<double>(t.loopStart))), x1 = static_cast<float>(xForTick(static_cast<double>(t.loopEnd)));
        return {x0, static_cast<float>(markerStripHeight), std::max(1.0f, x1 - x0), static_cast<float>(loopStripHeight)};
    }

    // ---- snapping ---------------------------------------------------------------------------------------------------
    tlui::Snap snap() const noexcept { return snapMode; }
    void setSnap(tlui::Snap s) { snapMode = s; if (onStateChanged) onStateChanged(); }
    // The size of the grid in ticks: what snapping rounds to, and what the arrow keys move by.
    std::int64_t gridTicks() const noexcept {
        if (snapMode != tlui::Snap::automatic) { const auto size = tlui::snapSize(snapMode); if (size > 0) return size; }
        static constexpr std::array<std::int64_t, 9> steps{240, 480, 960, 1920, 3840, 7680, 15360, 30720, 61440};
        for (const auto s : steps) if (static_cast<double>(s) * ppt >= 18.0) return s;
        return steps.back();
    }
    // Rounds to the grid; `bypass` (Alt held) and "Off" keep the exact position, to the tick.
    std::int64_t snapTick(double tick, bool bypass = false) const noexcept {
        if (!std::isfinite(tick)) return 0;
        const auto rounded = static_cast<std::int64_t>(std::llround(std::max(0.0, tick)));
        if (bypass || snapMode == tlui::Snap::off) return std::clamp<std::int64_t>(rounded, 0, timeline::maxLength);
        const auto grid = gridTicks();
        return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(std::max(0.0, tick) / static_cast<double>(grid))) * grid, 0, timeline::maxLength);
    }

    // ---- zoom and scroll ----------------------------------------------------------------------------------------------
    void setScrollTicks(double ticks) {
        if (!std::isfinite(ticks)) return;
        ticks = std::clamp(ticks, 0.0, maxScrollTicks());
        if (ticks == scrollX) return;
        scrollX = ticks; repaint(); notifyView();
    }
    void setScrollY(int y) {
        y = std::clamp(y, 0, maxScrollY());
        if (y == scrollYpx) return;
        scrollYpx = y; repaint(); notifyView();
    }
    // Zooms by `factor` keeping the tick under `anchorX` where it is.
    void zoomBy(double factor, int anchorX) {
        if (!(factor > 0.0) || !std::isfinite(factor)) return;
        const double anchorTick = tickAtX(static_cast<double>(anchorX));
        ppt = std::clamp(ppt * factor, minPixelsPerTick, maxPixelsPerTick);
        scrollX = std::clamp(anchorTick - (static_cast<double>(anchorX) - headerWidth) / ppt, 0.0, maxScrollTicks());
        repaint(); notifyView();
    }
    void zoomBy(double factor) { zoomBy(factor, headerWidth + laneAreaWidth() / 2); }
    // Fits the song (or the work on it) across the window.
    void zoomToFit() {
        const auto& t = committed();
        const double span = static_cast<double>(std::max<std::int64_t>(timeline::ticksPerBar * 4, std::max(t.contentEnd(), static_cast<std::int64_t>(0)))) * 1.02;
        ppt = std::clamp(laneAreaWidth() / span, minPixelsPerTick, maxPixelsPerTick);
        scrollX = 0.0;
        repaint(); notifyView();
    }
    void scrollRowIntoView(int row) {
        const int d = displayRowOf(row), top = d * rowHeight, bottom = top + rowHeight;
        if (top < scrollYpx) setScrollY(top); else if (bottom > scrollYpx + laneAreaHeight()) setScrollY(bottom - laneAreaHeight());
    }

    // ---- selection, cursor, focus -------------------------------------------------------------------------------------------
    const timeline::Ids& selection() const noexcept { return selected; }
    bool isSelected(std::uint32_t id) const noexcept { return timeline::contains(selected, id); }
    void setSelection(const timeline::Ids& ids) {
        selected = committed().expandGroups(ids);
        selectedMarker = 0;
        stateChanged();
    }
    void clearSelection() { if (selected.empty() && selectedMarker == 0) return; selected.clear(); selectedMarker = 0; stateChanged(); }
    void selectAll() {
        selected.clear();
        for (const auto& r : committed().regions) selected.push_back(r.id);
        selectedMarker = 0; stateChanged();
    }
    std::uint32_t selectedMarkerId() const noexcept { return selectedMarker; }
    std::int64_t cursorTick() const noexcept { return cursor; }
    // The play position: where playback starts, or jumps to if it is running.
    void setCursor(std::int64_t tick, bool seek = true) {
        tick = std::clamp<std::int64_t>(tick, 0, committed().length());
        const bool moved = tick != cursor;
        cursor = tick;
        if (seek && onSeek) onSeek(tick);
        if (moved) { repaint(); stateChanged(); }
    }
    int focusRow() const noexcept { return focus; }   // a timeline row
    void setFocusRow(int row) {
        const int d = displayRowOf(row);
        if (d < 0 || d >= displayRowCount() || row == focus) return;
        focus = row; repaint(); stateChanged();
    }
    // The playhead, in ticks (-1 when stopped). With follow on it keeps the playhead in view.
    void setPlayhead(std::int64_t tick) {
        if (tick == playhead) return;
        const auto before = playhead;
        playhead = tick;
        if (follow && tick >= 0 && !dragging()) {
            const double right = scrollX + visibleTicks();
            if (static_cast<double>(tick) < scrollX || static_cast<double>(tick) > right - visibleTicks() * 0.04) {
                const bool jumpedBack = before >= 0 && tick < before;
                setScrollTicks(jumpedBack || static_cast<double>(tick) < scrollX ? static_cast<double>(tick) - visibleTicks() * 0.1 : static_cast<double>(tick) - visibleTicks() * 0.05);
            }
        }
        repaintPlayhead(before); repaintPlayhead(tick);
    }
    std::int64_t playheadTick() const noexcept { return playhead; }
    void setFollow(bool on) { follow = on; }
    bool followsPlayhead() const noexcept { return follow; }

    // Called after the document changed behind the view's back (undo, open, new project, a Session edit).
    void refresh() {
        const auto& t = committed();
        timeline::Ids keep;
        for (const auto id : selected) if (t.find(id) != nullptr) keep.push_back(id);
        selected = keep;
        const bool markerThere = std::any_of(t.markers.begin(), t.markers.end(), [&](const auto& m) { return m.id == selectedMarker; });
        if (!markerThere) selectedMarker = 0;
        cursor = std::clamp<std::int64_t>(cursor, 0, t.length());
        const int d = displayRowOf(focus);
        if (d < 0 || d >= displayRowCount()) focus = timelineRow(0);
        scrollX = std::clamp(scrollX, 0.0, maxScrollTicks());
        scrollYpx = std::clamp(scrollYpx, 0, maxScrollY());
        preview.reset(); drag = {};
        repaint(); notifyView(); stateChanged();
    }

    // ---- commands (the keys, the menus, the toolbar and the tests all come through here) -----------------------------
    bool deleteSelection() {
        if (selectedMarker != 0 && selected.empty()) return removeMarker(selectedMarker);
        if (selected.empty()) return false;
        const auto ids = selected;
        const bool done = edit(ids.size() == 1 ? "Delete clip" : "Delete clips", [&](timeline::Timeline& t) { return t.remove(ids); });
        if (done) { selected.clear(); stateChanged(); }
        return done;
    }
    bool copySelection() {
        if (selected.empty()) return false;
        remember(committed().copy(selected));
        say(juce::String(clipboard().regions.size()) + (clipboard().regions.size() == 1 ? " clip copied." : " clips copied."));
        return true;
    }
    bool cutSelection() {
        if (selected.empty()) return false;
        const auto ids = selected;
        const auto cut = committed().copy(ids);
        const bool done = edit(ids.size() == 1 ? "Cut clip" : "Cut clips", [&](timeline::Timeline& t) { return t.remove(ids); });
        if (done) { remember(cut); selected.clear(); stateChanged(); }
        return done;
    }
    // Pastes at the play position (or `at`); rack patterns land on the focused lane.
    bool paste(std::optional<std::int64_t> at = std::nullopt) {
        const auto clip = pastable();
        if (clip.empty()) { say("Nothing to paste."); return false; }
        int laneDelta = 0;
        int firstLane = timeline::rowCount;
        for (const auto& r : clip.regions) if (r.isPattern()) firstLane = std::min(firstLane, r.row);
        if (firstLane < timeline::rowCount && timeline::isPatternRow(focus)) laneDelta = focus - firstLane;
        const auto where = at.value_or(cursor);
        auto made = std::make_shared<timeline::Ids>();
        const bool done = edit("Paste", [=](timeline::Timeline& t) { made->clear(); return t.paste(clip, where, laneDelta, made.get()); });
        if (done) { selected = *made; selectedMarker = 0; stateChanged(); }
        else say("Those clips do not fit there.");
        return done;
    }
    bool duplicateSelection() {
        if (selected.empty()) return false;
        const auto ids = selected;
        auto made = std::make_shared<timeline::Ids>();
        const bool done = edit("Duplicate", [=](timeline::Timeline& t) { made->clear(); return t.duplicate(ids, made.get()); });
        if (done) { selected = *made; stateChanged(); }
        return done;
    }
    bool groupSelection() {
        const auto ids = selected;
        const bool done = edit("Group clips", [&](timeline::Timeline& t) { return t.group(ids); });
        if (done) { selected = committed().expandGroups(ids); stateChanged(); } else say("Select two or more clips to group them.");
        return done;
    }
    bool ungroupSelection() {
        const auto ids = selected;
        const bool done = edit("Ungroup clips", [&](timeline::Timeline& t) { return t.ungroup(ids); });
        if (done) stateChanged();
        return done;
    }
    // Cuts the selected clips at the play position, or every clip under it when nothing is selected.
    bool splitAtCursor() {
        timeline::Ids ids = selected;
        if (ids.empty()) for (const auto& r : committed().regions) if (cursor > r.start && cursor < r.end()) ids.push_back(r.id);
        if (ids.empty()) { say("Put the play position inside a clip to split it."); return false; }
        const auto tick = cursor;
        auto made = std::make_shared<timeline::Ids>();
        const bool done = edit("Split", [=](timeline::Timeline& t) { made->clear(); return t.split(ids, tick, made.get()); });
        if (done) { for (const auto id : *made) selected.push_back(id); selected = committed().expandGroups(selected); stateChanged(); }
        else say("Put the play position inside a clip to split it.");
        return done;
    }
    bool toggleMuteSelection() {
        if (selected.empty()) return false;
        const auto ids = selected;
        bool allMuted = true;
        for (const auto id : ids) { const auto* r = committed().find(id); allMuted = allMuted && r != nullptr && r->muted; }
        return edit(allMuted ? "Unmute clips" : "Mute clips", [&](timeline::Timeline& t) { return t.setMuted(ids, !allMuted); });
    }
    bool slipSelection(std::int64_t ticks) {
        if (selected.empty()) return false;
        const auto ids = selected;
        return edit(ticks < 0 ? "Slip clips left" : "Slip clips right", [=](timeline::Timeline& t) { return t.slip(ids, ticks); }, "timeline-slip");
    }
    bool selectedStreamedAudio() const {
        const auto& d = controller.document();
        for (const auto id : selected) if (const auto* r = committed().find(id); r != nullptr && !r->isPattern()
            && d.isValidCell(r->row, r->source)) {
            const auto& clip = d.clip(r->row, r->source);
            if (clip.audio != nullptr && clip.audio->streamed()) return true;
        }
        return false;
    }
    bool toggleSourceLoopSelection() {
        if (selected.empty()) return false;
        if (selectedStreamedAudio()) { say("Source looping for streamed clips is not available yet."); return false; }
        bool all = true; for (const auto id : selected) { const auto* r = committed().find(id); all = all && r != nullptr && r->loopSource; }
        const auto ids = selected; return edit(all ? "Disable clip loop" : "Loop clip source", [=](timeline::Timeline& t) { return t.setLoopSource(ids, !all); });
    }
    bool toggleReverseSelection() {
        if (selected.empty()) return false;
        if (selectedStreamedAudio()) { say("Reverse for streamed clips is not available yet."); return false; }
        bool all = true; for (const auto id : selected) { const auto* r = committed().find(id); all = all && r != nullptr && r->reversed; }
        const auto ids = selected; return edit(all ? "Forward clips" : "Reverse clips", [=](timeline::Timeline& t) { return t.setReversed(ids, !all); });
    }
    bool fadeSelection() {
        if (selected.empty()) return false; const auto ids = selected;
        return edit("Fade clips", [=](timeline::Timeline& t) { return t.setFades(ids, timeline::ticksPerBeat, timeline::ticksPerBeat); });
    }
    bool crossfadeSelection() {
        if (selected.size() != 2) { say("Select two touching clips on the same track to crossfade."); return false; }
        const auto* a = committed().find(selected[0]); const auto* b = committed().find(selected[1]);
        if (a == nullptr || b == nullptr) return false; if (b->start < a->start) std::swap(a, b);
        const bool done = edit("Crossfade clips", [=](timeline::Timeline& t) { return t.crossfade(a->id, b->id, timeline::ticksPerBeat); });
        if (!done) say("Crossfade needs two touching clips on the same track."); return done;
    }
    bool normalizeSelection() {
        if (selected.empty()) return false;
        auto next = committed(); bool changed = false;
        const auto& d = controller.document();
        for (const auto id : selected) if (const auto* r = next.find(id); r != nullptr && !r->isPattern() && d.isValidCell(r->row, r->source)) {
            const auto& clip = d.clip(r->row, r->source); if (clip.audio == nullptr || clip.audio->waveformCount == 0) continue;
            float peak = 0.0f; for (std::size_t i = 0; i < clip.audio->waveformCount; ++i) { const auto& p = clip.audio->waveform[i]; peak = std::max({peak, std::abs(p.leftMin), std::abs(p.leftMax), std::abs(p.rightMin), std::abs(p.rightMax)}); }
            if (peak > 1.0e-6f) changed = next.setGain({id}, std::min(64.0, 1.0 / static_cast<double>(peak))) || changed;
        }
        if (!changed) { say("Waveform peaks are still loading or the selected clips are silent."); return false; }
        return edit("Normalize clips", [next = std::move(next)](timeline::Timeline& t) mutable { t = std::move(next); return true; });
    }
    bool toggleLoop() {
        const bool on = !committed().loopEnabled;
        return edit(on ? "Loop on" : "Loop off", [on](timeline::Timeline& t) { return t.setLoopEnabled(on); });
    }
    // Makes the loop cover the selected clips (or the song if none are selected) and switches it on.
    bool loopSelection() {
        std::int64_t from = timeline::maxLength, to = 0;
        for (const auto id : selected) if (const auto* r = committed().find(id)) { from = std::min(from, r->start); to = std::max(to, r->end()); }
        if (to <= from) { from = 0; to = committed().length(); }
        return edit("Set loop", [=](timeline::Timeline& t) { const bool a = t.setLoop(from, to); const bool b = t.setLoopEnabled(true); return a || b; });
    }
    bool setLoopRange(std::int64_t from, std::int64_t to, bool enable = true) {
        return edit("Set loop", [=](timeline::Timeline& t) { const bool a = t.setLoop(from, to); const bool b = enable && t.setLoopEnabled(true); return a || b; });
    }
    bool addMarkerAtCursor(const juce::String& name = {}) {
        const auto tick = cursor;
        const auto text = name.isNotEmpty() ? name : "Marker " + juce::String(committed().markers.size() + 1);
        auto made = std::make_shared<std::uint32_t>(0);
        const bool done = edit("Add marker", [=](timeline::Timeline& t) { *made = t.addMarker(tick, text.toStdString(), static_cast<std::uint8_t>(t.markers.size() % 8)); return *made != 0; });
        if (done) { selected.clear(); selectedMarker = *made; stateChanged(); } else say("No room for another marker there.");
        return done;
    }
    bool removeMarker(std::uint32_t id) {
        const bool done = edit("Delete marker", [=](timeline::Timeline& t) { return t.removeMarker(id); });
        if (done && selectedMarker == id) selectedMarker = 0;
        stateChanged();
        return done;
    }
    bool colourMarker(std::uint32_t id, int colour) {
        return edit("Colour marker", [=](timeline::Timeline& t) { return t.colourMarker(id, static_cast<std::uint8_t>(colour)); });
    }
    void renameMarker(std::uint32_t id) {
        const auto* m = committed().markers.empty() ? nullptr : [&]() -> const timeline::Marker* { for (const auto& x : committed().markers) if (x.id == id) return &x; return nullptr; }();
        if (m == nullptr) return;
        askForName("Rename marker", juce::String::fromUTF8(m->name.c_str()), [this, id](const juce::String& text) {
            edit("Rename marker", [&](timeline::Timeline& t) { return t.renameMarker(id, text.toStdString()); });
        });
    }
    // Arrow keys: moves the selection by the grid (or a bar), or the play position when nothing is selected.
    bool nudge(int direction, bool bar = false) {
        const auto step = (bar ? timeline::ticksPerBar : gridTicks()) * direction;
        if (selected.empty()) { setCursor(cursor + step); return true; }
        const auto ids = selected;
        return edit("Nudge clips", [&](timeline::Timeline& t) { return t.move(ids, step); }, "timeline-nudge");
    }
    bool moveSelectionLanes(int direction) {
        if (selected.empty()) { moveFocus(direction); return true; }
        const auto ids = selected;
        const bool done = edit("Move to another lane", [&](timeline::Timeline& t) { return t.move(ids, 0, direction); });
        return done;
    }
    void moveFocus(int direction) {
        const int d = std::clamp(displayRowOf(focus) + direction, 0, displayRowCount() - 1);
        setFocusRow(timelineRow(d)); scrollRowIntoView(focus);
    }
    void jumpToMarker(bool forward) {
        const auto* m = forward ? committed().markerAfter(cursor) : committed().markerBefore(cursor);
        if (m == nullptr) { say(forward ? "No marker after the play position." : "No marker before the play position."); return; }
        selected.clear(); selectedMarker = m->id;
        setCursor(m->tick);
        if (static_cast<double>(m->tick) < scrollX || static_cast<double>(m->tick) > scrollX + visibleTicks()) setScrollTicks(static_cast<double>(m->tick) - visibleTicks() * 0.1);
        stateChanged();
    }
    // Puts a clip (a Session scene on a track row, or a pattern on a lane) at a position and selects it.
    bool place(int row, int source, std::int64_t tick) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        std::uint32_t id = 0;
        const bool done = controller.placeRegion(row, source, tick, &id);
        if (done) { selected = {id}; selectedMarker = 0; focus = row; afterEdit(); }
        else say("That clip does not fit there.");
        return done;
    }
    // Double-click: the track's selected scene, or the first scene with something in it; on a lane, the selected pattern.
    bool placeDefault(int row, std::int64_t tick) {
        const auto& d = controller.document();
        int source = -1;
        if (timeline::isPatternRow(row)) source = controller.channelRack().selectedPattern;
        else if (d.isValidCell(row, d.selectedScene) && !d.clip(row, d.selectedScene).isEmpty()) source = d.selectedScene;
        else for (int s = 0; s < d.sceneCount() && source < 0; ++s) if (!d.clip(row, s).isEmpty()) source = s;
        // A track with nothing in it yet: start a new, empty clip right here, ready to be opened and filled with notes.
        if (source < 0) return timeline::isPatternRow(row) ? false : newClipHere(row, tick);
        return place(row, source, tick);
    }
    // Makes a new, empty MIDI clip for a track and puts it at `tick`, as one undo step. Double-click it to write notes.
    bool newClipHere(int row, std::int64_t tick) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        std::uint32_t id = 0; int scene = -1; bool sceneAdded = false;
        if (!controller.placeNewClip(row, tick, &id, &scene, &sceneAdded)) { say("A new clip does not fit there: the Session has no room for another scene."); return false; }
        selected = {id}; selectedMarker = 0; focus = row;
        afterEdit();
        if (onClipCreated) onClipCreated(row, scene, sceneAdded);
        const auto& d = controller.document();
        say("New empty clip on " + (row >= 0 && row < d.trackCount() ? d.tracks[static_cast<std::size_t>(row)].name : juce::String("the track"))
            + ". Double-click it to write notes in the Piano Roll.");
        return true;
    }
    // Where a row of the clip list would land: a clip always goes on its own track's lane; a pattern goes on the pattern lane under
    // the pointer (the first lane when the pointer is over a track lane or the ruler). `pointer` is in this component's coordinates.
    struct DropPlan {
        bool valid{false};
        int row{-1}, source{-1};
        std::int64_t tick{0}, length{0};
        juce::String message;   // what to tell the person while dragging
    };
    DropPlan planDrop(const clipbin::Item& item, juce::Point<int> pointer) const {
        DropPlan plan;
        const auto& d = controller.document();
        if (!item.valid()) return plan;
        if (pointer.x < headerWidth) { plan.message = "Drop it on a lane, to the right of the track names."; return plan; }
        plan.tick = snapTick(tickAtX(static_cast<double>(pointer.x)));
        if (item.kind == clipbin::Item::Kind::pattern) {
            const int displayRow = displayRowAt(pointer.y);
            plan.row = displayRow >= 0 && timeline::isPatternRow(timelineRow(displayRow)) ? timelineRow(displayRow) : timeline::rackRowBase;
            plan.source = item.a;
            if (!controller.channelRack().validPattern(plan.source)) { plan.row = -1; plan.message = "That pattern no longer exists."; return plan; }
        } else {
            plan.row = item.a; plan.source = item.b;
            if (!d.isValidCell(plan.row, plan.source)) { plan.row = -1; plan.message = "That clip no longer exists."; return plan; }
        }
        plan.length = controller.naturalLength(plan.row, plan.source);
        plan.valid = plan.length > 0;
        if (plan.valid) {
            timeline::Region named; named.row = plan.row; named.source = plan.source;
            plan.message = "Drop to put " + regionName(named) + " on " + laneName(plan.row) + " at bar " + juce::String(plan.tick / timeline::ticksPerBar + 1)
                         + ", beat " + juce::String((plan.tick % timeline::ticksPerBar) / timeline::ticksPerBeat + 1) + ".";
        }
        return plan;
    }
    juce::String laneName(int row) const {
        const auto& d = controller.document();
        if (timeline::isPatternRow(row)) return "Pattern lane " + juce::String(row - timeline::rackRowBase + 1);
        return row >= 0 && row < d.trackCount() ? d.tracks[static_cast<std::size_t>(row)].name : juce::String("the track");
    }
    // The clip list's double-click: the item at the play position, on the focused pattern lane or the clip's own track.
    bool placeItemAtCursor(const clipbin::Item& item) {
        const int lane = timeline::isPatternRow(focus) ? focus : timeline::rackRowBase;
        const auto pointer = juce::Point<int>(juce::roundToInt(xForTick(static_cast<double>(cursor))), rowTop(displayRowOf(lane)) + rowHeight / 2);
        auto plan = planDrop(item, {std::max(pointer.x, headerWidth), pointer.y});
        if (!plan.valid) { say(plan.message); return false; }
        plan.tick = cursor;   // exactly where the play position is, not the grid point nearest to a pixel
        if (item.kind == clipbin::Item::Kind::pattern) plan.row = lane;
        const bool done = place(plan.row, plan.source, plan.tick);
        if (done) { scrollRowIntoView(plan.row); say("Placed on " + laneName(plan.row) + " at the play position."); }
        return done;
    }

    // ---- dropping from the clip list ---------------------------------------------------------------------------------------
    bool isInterestedInDragSource(const SourceDetails& details) override { return clipbin::decode(details.description).valid(); }
    void itemDragEnter(const SourceDetails& details) override {
        const auto plan = planDrop(clipbin::decode(details.description), details.localPosition);
        if (plan.valid) scrollRowIntoView(plan.row);
        showDropPlan(plan);
    }
    void itemDragMove(const SourceDetails& details) override { showDropPlan(planDrop(clipbin::decode(details.description), details.localPosition)); }
    void itemDragExit(const SourceDetails&) override { showDropPlan({}); say({}); }
    void itemDropped(const SourceDetails& details) override {
        const auto plan = planDrop(clipbin::decode(details.description), details.localPosition);
        showDropPlan({});
        if (!plan.valid) { say(plan.message.isNotEmpty() ? plan.message : "That cannot be placed there."); return; }
        if (place(plan.row, plan.source, plan.tick)) say("Placed on " + laneName(plan.row) + " at bar " + juce::String(plan.tick / timeline::ticksPerBar + 1) + ".");
    }
    void showDropPlan(const DropPlan& plan) {
        dropGhost = plan;
        if (plan.message.isNotEmpty()) say(plan.message);
        repaint();
    }
    const DropPlan& dropPreview() const noexcept { return dropGhost; }
    // The menu of things that can be placed on a row.
    enum MenuId { placeNewClip = 900, placeSceneBase = 1000, placePatternBase = 2000 };
    // The items of the right-click menus.
    enum Command { cmdSplit = 1, cmdMute, cmdDuplicate, cmdCopy, cmdCut, cmdDelete, cmdGroup, cmdUngroup, cmdOpen, cmdPaste, cmdSelectAll, cmdAddMarker,
                   cmdSlipLeft, cmdSlipRight, cmdSourceLoop, cmdReverse, cmdNormalize, cmdFade, cmdCrossfade,
                   cmdRename, cmdRemoveMarker, cmdJump, cmdLoopSelection, cmdColourBase = 100,
                   cmdArm = 200, cmdMonitorBase = 210, cmdInputMonoBase = 300, cmdInputStereoBase = 400 };
    juce::PopupMenu placeMenu(int row) const {
        juce::PopupMenu menu;
        const auto& d = controller.document();
        if (timeline::isPatternRow(row)) {
            const auto& rackProject = controller.channelRack();
            for (int p = 0; p < rackProject.patternCount; ++p)
                menu.addItem(placePatternBase + p, juce::String(p + 1) + "  " + juce::String::fromUTF8(rackProject.patternNames[static_cast<std::size_t>(p)].c_str()));
        } else if (row >= 0 && row < d.trackCount()) {
            menu.addItem(placeNewClip, "New empty clip");
            menu.addSeparator();
            for (int s = 0; s < d.sceneCount(); ++s) {
                const auto& clip = d.clip(row, s);
                const juce::String what = clip.isAudioClip() ? "audio" : clip.notes().count > 0 ? juce::String(static_cast<int>(clip.notes().count)) + " notes" : "empty";
                menu.addItem(placeSceneBase + s, d.scenes[static_cast<std::size_t>(s)].name + ui::utf8("  \xc2\xb7  ") + what, !clip.isEmpty());
            }
        }
        return menu;
    }
    void showPlaceMenu(int row, std::int64_t tick) {
        const auto area = juce::Rectangle<int>(juce::roundToInt(xForTick(static_cast<double>(tick))), rowTop(displayRowOf(row)), 1, rowHeight);
        launch(placeMenu(row), area, [this, row, tick](int id) {
            if (id == placeNewClip) newClipHere(row, tick);
            else if (id >= placePatternBase && id < placePatternBase + 1000) place(row, id - placePatternBase, tick);
            else if (id >= placeSceneBase && id < placeSceneBase + 1000) place(row, id - placeSceneBase, tick);
        });
    }
    void askForName(const juce::String& title, const juce::String& current, std::function<void(const juce::String&)> done) {
        if (nameRequester) { nameRequester(title, current, std::move(done)); return; }
        auto* window = new juce::AlertWindow(title, "", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", current);
        window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TimelineCanvas> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window, done = std::move(done)](int result) {
            if (safe != nullptr && result == 1) done(window->getTextEditorContents("name").trim());
        }), true);
    }

    // The clipboard is shared by every timeline. It also remembers which tracks and scenes (by identity, not
    // position) its rows and sources meant, so a paste after tracks or scenes were deleted or reordered still
    // lands on the right ones and skips the clips whose track or scene is gone.
    static timeline::Clipboard& clipboard() { static timeline::Clipboard shared; return shared; }
    struct ClipboardOrigin { std::vector<std::uint32_t> tracks, scenes; int patterns{0}; };
    static ClipboardOrigin& clipboardOrigin() { static ClipboardOrigin origin; return origin; }
    void remember(timeline::Clipboard clip) {
        clipboard() = std::move(clip);
        auto& origin = clipboardOrigin();
        origin = {};
        for (const auto& track : controller.document().tracks) origin.tracks.push_back(track.id);
        for (const auto& scene : controller.document().scenes) origin.scenes.push_back(scene.id);
        origin.patterns = controller.channelRack().patternCount;
    }
    timeline::Clipboard pastable() const {
        const auto& origin = clipboardOrigin();
        const auto& d = controller.document();
        timeline::Clipboard clip;
        for (auto r : clipboard().regions) {
            if (r.isPattern()) {
                if (origin.patterns != controller.channelRack().patternCount || r.source < 0 || r.source >= origin.patterns) continue;   // patterns were added or removed: their numbers mean something else now
            } else {
                if (r.row < 0 || r.row >= static_cast<int>(origin.tracks.size()) || r.source < 0 || r.source >= static_cast<int>(origin.scenes.size())) continue;
                const auto track = std::find_if(d.tracks.begin(), d.tracks.end(), [&](const auto& t) { return t.id == origin.tracks[static_cast<std::size_t>(r.row)]; });
                const auto scene = std::find_if(d.scenes.begin(), d.scenes.end(), [&](const auto& s) { return s.id == origin.scenes[static_cast<std::size_t>(r.source)]; });
                if (track == d.tracks.end() || scene == d.scenes.end()) continue;
                r.row = static_cast<int>(track - d.tracks.begin());
                r.source = static_cast<int>(scene - d.scenes.begin());
            }
            clip.regions.push_back(r);
        }
        std::int64_t first = timeline::maxLength;
        for (const auto& r : clip.regions) first = std::min(first, r.start);
        for (auto& r : clip.regions) r.start -= first;   // starts are relative to the earliest clip left
        return clip;
    }
    bool dragging() const noexcept { return drag.kind != DragKind::none && drag.active; }

    // ---- hit testing ---------------------------------------------------------------------------------------------------
    Hit hitTest(juce::Point<int> p) const {
        Hit hit;
        if (p.y < topHeight) {
            if (p.x < headerWidth) { hit.zone = Zone::corner; return hit; }
            if (p.y < markerStripHeight) {
                hit.zone = Zone::markerStrip;
                const auto& markers = shown().markers;
                for (auto it = markers.rbegin(); it != markers.rend(); ++it)
                    if (markerBounds(*it).expanded(2.0f, 0.0f).contains(p.toFloat())) { hit.marker = it->id; break; }
                return hit;
            }
            if (p.y < markerStripHeight + loopStripHeight) {
                hit.zone = Zone::loopStrip;
                const auto loop = loopBounds();
                const float x = static_cast<float>(p.x);
                if (std::abs(x - loop.getX()) <= edgeGrab) hit.loopPart = Hit::LoopPart::start;
                else if (std::abs(x - loop.getRight()) <= edgeGrab) hit.loopPart = Hit::LoopPart::end;
                else if (x > loop.getX() && x < loop.getRight()) hit.loopPart = Hit::LoopPart::body;
                return hit;
            }
            hit.zone = Zone::ruler; return hit;
        }
        hit.displayRow = displayRowAt(p.y);
        if (p.x < headerWidth) { hit.zone = hit.displayRow >= 0 ? Zone::header : Zone::none; return hit; }
        if (hit.displayRow < 0) return hit;
        hit.zone = Zone::lane;
        const int row = timelineRow(hit.displayRow);
        const auto& regions = shown().regions;
        for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
            if (it->row != row) continue;
            const auto box = regionBounds(*it);
            if (!box.contains(p.toFloat())) continue;
            hit.zone = Zone::region; hit.region = it->id;
            if (box.getWidth() >= edgeGrab * 3.0f) {
                if (p.x - box.getX() <= edgeGrab) hit.edge = Edge::left;
                else if (box.getRight() - p.x <= edgeGrab) hit.edge = Edge::right;
            }
            break;
        }
        return hit;
    }

    // ---- painting ------------------------------------------------------------------------------------------------------------
    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::surface);
        const auto& t = shown();
        const auto lanes = juce::Rectangle<int>(headerWidth, topHeight, getWidth() - headerWidth, getHeight() - topHeight);
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(lanes);
            paintLanes(g, t, lanes);
        }
        paintHeader(g);
        paintTop(g, t);
    }
    void resized() override {
        scrollX = std::clamp(scrollX, 0.0, maxScrollTicks());
        scrollYpx = std::clamp(scrollYpx, 0, maxScrollY());
        notifyView();
    }

    // ---- mouse ---------------------------------------------------------------------------------------------------------------------
    void mouseMove(const juce::MouseEvent& e) override {
        const auto hit = hitTest(e.getPosition());
        auto cursorType = juce::MouseCursor::NormalCursor;
        if (hit.zone == Zone::region && hit.edge != Edge::none) cursorType = juce::MouseCursor::LeftRightResizeCursor;
        else if (hit.zone == Zone::loopStrip && (hit.loopPart == Hit::LoopPart::start || hit.loopPart == Hit::LoopPart::end)) cursorType = juce::MouseCursor::LeftRightResizeCursor;
        else if (hit.zone == Zone::markerStrip && hit.marker != 0) cursorType = juce::MouseCursor::PointingHandCursor;
        setMouseCursor(cursorType);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (isShowing()) grabKeyboardFocus();
        beginDragAutoRepeat(30);
        drag = {};
        const auto hit = hitTest(e.getPosition());
        const bool bypass = e.mods.isAltDown();
        const bool extend = e.mods.isShiftDown() || e.mods.isCommandDown();
        drag.origin = e.getPosition();
        drag.anchorTick = tickAtX(static_cast<double>(e.x));
        drag.mods = e.mods;
        if (e.mods.isPopupMenu()) { rightClick(hit, e); drag = {}; return; }
        if (hit.displayRow >= 0 && hit.zone != Zone::header) setFocusRow(timelineRow(hit.displayRow));

        switch (hit.zone) {
        case Zone::header:
            if (hit.displayRow >= 0) {
                const int row = timelineRow(hit.displayRow);
                if (!timeline::isPatternRow(row) && armRect(hit.displayRow).expanded(2).contains(e.getPosition())) { toggleArm(row); break; }
                setFocusRow(row); scrollRowIntoView(focus);
            }
            break;
        case Zone::ruler:
            drag.kind = DragKind::scrub; drag.active = true;
            setCursor(snapTick(drag.anchorTick, bypass));
            break;
        case Zone::markerStrip:
            if (hit.marker != 0) {
                selected.clear(); selectedMarker = hit.marker; stateChanged();
                const auto* m = markerById(hit.marker);
                drag.kind = DragKind::marker; drag.grabbed = hit.marker;
                drag.grabOffset = m != nullptr ? static_cast<double>(m->tick) - drag.anchorTick : 0.0;
                drag.base = baseTimeline();
                repaint();
            } else {
                drag.kind = DragKind::scrub; drag.active = true;
                setCursor(snapTick(drag.anchorTick, bypass));
            }
            break;
        case Zone::loopStrip: {
            drag.base = baseTimeline();
            const auto& t = committed();
            if (hit.loopPart == Hit::LoopPart::start) { drag.kind = DragKind::loopStart; }
            else if (hit.loopPart == Hit::LoopPart::end) { drag.kind = DragKind::loopEnd; }
            else if (hit.loopPart == Hit::LoopPart::body) { drag.kind = DragKind::loopMove; drag.grabOffset = static_cast<double>(t.loopStart) - drag.anchorTick; }
            else { drag.kind = DragKind::loopNew; drag.newAnchor = std::min(snapTick(drag.anchorTick, bypass), t.length()); }
            break;
        }
        case Zone::region: {
            const auto* r = committed().find(hit.region);
            if (r == nullptr) break;
            const bool wasSelected = isSelected(hit.region);
            if (hit.edge != Edge::none && !extend) {
                // Resizing is about one clip: select just it.
                if (!wasSelected || selected.size() > 1) setSelection({hit.region});
                drag.kind = hit.edge == Edge::left ? DragKind::trimStart : DragKind::trimEnd;
                drag.grabbed = hit.region; drag.base = baseTimeline();
                drag.edgeTick = hit.edge == Edge::left ? r->start : r->end();
                drag.grabOffset = static_cast<double>(drag.edgeTick) - drag.anchorTick;
                break;
            }
            if (e.mods.isShiftDown()) {
                timeline::Ids ids = selected;
                const auto group = committed().expandGroups({hit.region});
                if (wasSelected) std::erase_if(ids, [&](std::uint32_t id) { return timeline::contains(group, id); });
                else for (const auto id : group) if (!timeline::contains(ids, id)) ids.push_back(id);
                selected = ids; selectedMarker = 0; stateChanged();
                if (wasSelected) break;   // it was just deselected: nothing to drag
            } else if (e.mods.isCommandDown()) {
                // Ctrl-click toggles; Ctrl-drag copies what is dragged. Which of the two it is is known on release.
                drag.pendingToggle = hit.region; drag.wasSelected = wasSelected;
            } else if (!wasSelected) {
                setSelection({hit.region});
            } else if (selected.size() > 1) {
                drag.collapseOnClick = true;   // a plain click on one of several selected clips picks just that one, on release
            }
            drag.kind = e.mods.isCommandDown() ? DragKind::copy : DragKind::move;
            drag.grabbed = hit.region; drag.base = baseTimeline();
            drag.grabOffset = static_cast<double>(r->start) - drag.anchorTick;
            drag.ids = e.mods.isCommandDown() && !wasSelected ? committed().expandGroups({hit.region}) : selected;
            break;
        }
        case Zone::lane:
            drag.kind = DragKind::marquee; drag.additive = extend; drag.previousSelection = selected;
            drag.emptyRow = timelineRow(hit.displayRow);
            break;
        default: break;
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (drag.kind == DragKind::none) return;
        const auto pos = e.getPosition();
        if (!drag.active) {
            if (std::abs(pos.x - drag.origin.x) < dragThreshold && std::abs(pos.y - drag.origin.y) < dragThreshold) return;
            if (drag.kind != DragKind::scrub) drag.active = true;
            if (drag.kind == DragKind::copy && !drag.wasSelected) { selected = drag.ids; selectedMarker = 0; stateChanged(); }
        }
        autoScroll(pos.x);
        const bool bypass = e.mods.isAltDown();
        const double tickNow = tickAtX(static_cast<double>(pos.x));
        switch (drag.kind) {
        case DragKind::scrub: setCursor(snapTick(tickNow, bypass)); break;
        case DragKind::move: case DragKind::copy: {
            const auto* grabbed = drag.base->find(drag.grabbed);
            if (grabbed == nullptr) break;
            const auto newStart = snapTick(tickNow + drag.grabOffset, bypass);
            const auto delta = newStart - grabbed->start;
            const int rows = timeline::isPatternRow(grabbed->row) ? static_cast<int>(std::lround(static_cast<double>(pos.y - drag.origin.y) / rowHeight)) : 0;
            const auto ids = drag.ids;
            if (drag.kind == DragKind::move) {
                drag.label = ids.size() == 1 ? "Move clip" : "Move clips";
                drag.apply = [=](timeline::Timeline& t) { return t.move(ids, delta, rows); };
            } else {
                const auto clip = drag.base->copy(ids);
                std::int64_t first = timeline::maxLength;
                for (const auto id : ids) if (const auto* r = drag.base->find(id)) first = std::min(first, r->start);
                int firstLane = timeline::rowCount;
                for (const auto& r : clip.regions) if (r.isPattern()) firstLane = std::min(firstLane, r.row);
                const int laneDelta = firstLane < timeline::rowCount ? std::clamp(rows, timeline::rackRowBase - firstLane, timeline::rowCount - 1 - firstLane) : 0;
                const auto at = std::max<std::int64_t>(0, first + delta);
                drag.created = std::make_shared<timeline::Ids>();
                auto made = drag.created;
                drag.label = ids.size() == 1 ? "Copy clip" : "Copy clips";
                drag.apply = [=](timeline::Timeline& t) { made->clear(); return t.paste(clip, at, laneDelta, made.get()); };
            }
            updatePreview(); break;
        }
        case DragKind::trimStart: case DragKind::trimEnd: {
            const auto id = drag.grabbed;
            const auto edgeNow = snapTick(tickNow + drag.grabOffset, bypass);
            drag.label = drag.kind == DragKind::trimStart ? "Trim clip start" : "Resize clip";
            if (drag.kind == DragKind::trimStart) drag.apply = [=](timeline::Timeline& t) { return t.trimStart(id, edgeNow); };
            else drag.apply = [=](timeline::Timeline& t) { return t.trimEnd(id, edgeNow); };
            updatePreview(); break;
        }
        case DragKind::marker: {
            const auto id = drag.grabbed;
            const auto to = snapTick(tickNow + drag.grabOffset, bypass);
            drag.label = "Move marker";
            drag.apply = [=](timeline::Timeline& t) { return t.moveMarker(id, to); };
            updatePreview(); break;
        }
        case DragKind::loopNew: case DragKind::loopStart: case DragKind::loopEnd: case DragKind::loopMove: {
            const auto& base = *drag.base;
            const auto snapped = std::min(snapTick(tickNow, bypass), base.length());   // the loop stays inside the song
            std::int64_t from = base.loopStart, to = base.loopEnd;
            if (drag.kind == DragKind::loopNew) { from = std::min(drag.newAnchor, snapped); to = std::max(drag.newAnchor, snapped); }
            else if (drag.kind == DragKind::loopStart) from = std::min(snapped, base.loopEnd - timeline::ticksPerStep);
            else if (drag.kind == DragKind::loopEnd) to = std::max(snapped, base.loopStart + timeline::ticksPerStep);
            else {
                const auto length = base.loopEnd - base.loopStart;
                from = std::clamp<std::int64_t>(snapTick(tickNow + drag.grabOffset, bypass), 0, base.length() - length); to = from + length;
            }
            if (to - from < timeline::ticksPerStep) break;
            drag.label = "Set loop";
            drag.apply = [=](timeline::Timeline& t) { const bool a = t.setLoop(from, to); const bool b = t.setLoopEnabled(true); return a || b; };
            updatePreview(); break;
        }
        case DragKind::marquee: {
            drag.marquee = juce::Rectangle<int>::leftTopRightBottom(std::min(drag.origin.x, pos.x), std::min(drag.origin.y, pos.y),
                                                                     std::max(drag.origin.x, pos.x), std::max(drag.origin.y, pos.y));
            const auto in = marqueeHits(drag.marquee);
            timeline::Ids next = drag.additive ? drag.previousSelection : timeline::Ids{};
            for (const auto id : in) if (!timeline::contains(next, id)) next.push_back(id);
            selected = committed().expandGroups(next); selectedMarker = 0;
            repaint(); break;
        }
        default: break;
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        const auto kind = drag.kind;
        const bool active = drag.active;
        const bool bypass = e.mods.isAltDown();
        auto finished = std::move(drag);
        drag = {};
        const bool hadPreview = preview.has_value();
        preview.reset();
        switch (kind) {
        case DragKind::move: case DragKind::copy: case DragKind::trimStart: case DragKind::trimEnd:
        case DragKind::marker: case DragKind::loopNew: case DragKind::loopStart: case DragKind::loopEnd: case DragKind::loopMove:
            if (active && finished.apply) {
                const bool done = edit(finished.label, finished.apply);
                if (done && kind == DragKind::copy && finished.created) { selected = *finished.created; stateChanged(); }
                if (!done && hadPreview && !controller.isBusy()) say("That does not fit there.");
            } else if (!active) {
                if (kind == DragKind::copy && finished.pendingToggle != 0) {
                    // A Ctrl-click adds a clip to the selection, or takes it out if it was in.
                    const auto group = committed().expandGroups({finished.pendingToggle});
                    if (finished.wasSelected) std::erase_if(selected, [&](std::uint32_t id) { return timeline::contains(group, id); });
                    else for (const auto id : group) if (!isSelected(id)) selected.push_back(id);
                    selectedMarker = 0;
                    stateChanged();
                } else if (kind == DragKind::move && finished.collapseOnClick) {
                    setSelection({finished.grabbed});
                } else if (kind == DragKind::marker && finished.grabbed != 0) {
                    if (const auto* m = markerById(finished.grabbed)) setCursor(m->tick);   // a click on a marker jumps there
                } else if ((kind == DragKind::loopNew || kind == DragKind::loopStart || kind == DragKind::loopEnd || kind == DragKind::loopMove)) {
                    // A click on the loop strip with no drag: nothing changes.
                }
            }
            break;
        case DragKind::marquee:
            if (!active) {
                // A click on an empty spot: it deselects, and puts the play position there.
                if (!finished.additive) { selected.clear(); selectedMarker = 0; stateChanged(); }
                setCursor(snapTick(finished.anchorTick, bypass));
            } else stateChanged();
            break;
        default: break;
        }
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        const auto hit = hitTest(e.getPosition());
        const bool bypass = e.mods.isAltDown();
        const auto tick = snapTick(tickAtX(static_cast<double>(e.x)), bypass);
        if (hit.zone == Zone::region) {
            if (const auto* r = committed().find(hit.region); r != nullptr && onOpenSource) onOpenSource(r->row, r->source);
        } else if (hit.zone == Zone::lane && hit.displayRow >= 0) {
            placeDefault(timelineRow(hit.displayRow), tick);
        } else if (hit.zone == Zone::markerStrip) {
            if (hit.marker != 0) renameMarker(hit.marker);
            else { setCursor(tick, false); addMarkerAtCursor(); }
        } else if (hit.zone == Zone::loopStrip) {
            // Double-click: loop the bar under the pointer.
            const auto bar = static_cast<std::int64_t>(std::floor(tickAtX(static_cast<double>(e.x)) / timeline::ticksPerBar)) * timeline::ticksPerBar;
            setLoopRange(bar, bar + timeline::ticksPerBar);
        }
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const float dy = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
        if (e.mods.isCommandDown()) { zoomBy(std::pow(1.6, static_cast<double>(dy) * 2.0), e.x); return; }
        if (e.mods.isShiftDown() || wheel.deltaX != 0.0f) {
            const double amount = wheel.deltaX != 0.0f ? static_cast<double>(wheel.deltaX) : static_cast<double>(dy);
            setScrollTicks(scrollX - amount * 400.0 / ppt);
            return;
        }
        setScrollY(scrollYpx - juce::roundToInt(dy * 480.0f));
    }
    void mouseMagnify(const juce::MouseEvent& e, float scale) override { zoomBy(static_cast<double>(scale), e.x); }

    // ---- keys -----------------------------------------------------------------------------------------------------------------------------
    bool keyPressed(const juce::KeyPress& key) override {
        const auto mods = key.getModifiers();
        const int code = key.getKeyCode();
        const bool command = mods.isCommandDown(), shift = mods.isShiftDown();
        if (mods.isAltDown()) return false;
        const auto letter = juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(code));
        if (code == juce::KeyPress::escapeKey) {
            if (dragging()) { drag = {}; preview.reset(); repaint(); return true; }
            clearSelection(); return true;
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) return deleteSelection() || !selected.empty();
        if (code == juce::KeyPress::spaceKey && !command) { if (onPlayToggle) onPlayToggle(); return true; }
        if (code == juce::KeyPress::homeKey) { setCursor(0); setScrollTicks(0); return true; }
        if (code == juce::KeyPress::endKey) { setCursor(committed().length()); return true; }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
            const int dir = code == juce::KeyPress::rightKey ? 1 : -1;
            if (command) { jumpToMarker(dir > 0); return true; }
            nudge(dir, shift); return true;
        }
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
            if (command) return false;
            moveSelectionLanes(code == juce::KeyPress::downKey ? 1 : -1); return true;
        }
        if (code == juce::KeyPress::pageDownKey || code == juce::KeyPress::pageUpKey) {
            setScrollTicks(scrollX + (code == juce::KeyPress::pageDownKey ? 1.0 : -1.0) * visibleTicks() * 0.9); return true;
        }
        if (command) {
            if (letter == 'A' && !shift) { selectAll(); return true; }
            if (letter == 'C' && !shift) { copySelection(); return true; }
            if (letter == 'X' && !shift) { cutSelection(); return true; }
            if (letter == 'V' && !shift) { paste(); return true; }
            if (letter == 'D' && !shift) { duplicateSelection(); return true; }
            if (letter == 'G') { if (shift) ungroupSelection(); else groupSelection(); return true; }
            if (letter == 'E' && !shift) { splitAtCursor(); return true; }
            if (code == '0' || code == juce::KeyPress::numberPad0) { zoomToFit(); return true; }
            if (isZoomIn(key)) { zoomBy(1.25); return true; }
            if (isZoomOut(key)) { zoomBy(0.8); return true; }
            return false;   // Ctrl+Z, Ctrl+S, Ctrl+O... belong to the window
        }
        if (isZoomIn(key)) { zoomBy(1.25); return true; }
        if (isZoomOut(key)) { zoomBy(0.8); return true; }
        if (letter == 'S' && !shift) { splitAtCursor(); return true; }
        if (letter == 'M' && !shift) { toggleMuteSelection(); return true; }
        if (letter == 'L') { if (shift) loopSelection(); else toggleLoop(); return true; }
        if (letter == 'K' && !shift) { addMarkerAtCursor(); return true; }
        return false;   // everything else (the rest of the typing piano, for one) is not ours
    }

    static bool isZoomIn(const juce::KeyPress& key) noexcept {
        const int c = key.getKeyCode();
        const auto t = key.getTextCharacter();
        return c == '=' || c == '+' || c == juce::KeyPress::numberPadAdd || t == '+' || t == '=';
    }
    static bool isZoomOut(const juce::KeyPress& key) noexcept {
        const int c = key.getKeyCode();
        const auto t = key.getTextCharacter();
        return c == '-' || c == '_' || c == juce::KeyPress::numberPadSubtract || t == '-' || t == '_';
    }

    // The regions inside a box, in canvas coordinates.
    timeline::Ids marqueeHits(juce::Rectangle<int> box) const {
        timeline::Ids ids;
        const int firstRow = displayRowAt(std::max(box.getY(), topHeight)), lastRow = displayRowAt(std::max(box.getBottom(), topHeight));
        const int rows = displayRowCount();
        const int lo = firstRow < 0 ? (box.getY() < topHeight ? 0 : rows) : firstRow;
        const int hi = lastRow < 0 ? (box.getBottom() >= getHeight() || box.getBottom() > topHeight ? rows - 1 : -1) : lastRow;
        const auto from = static_cast<std::int64_t>(std::floor(tickAtX(static_cast<double>(box.getX()))));
        const auto to = static_cast<std::int64_t>(std::ceil(tickAtX(static_cast<double>(box.getRight()))));
        for (int d = lo; d <= hi && d < rows; ++d) {
            const auto row = timelineRow(d);
            for (const auto& r : committed().regions)
                if (r.row == row && r.start < to && r.end() > from) ids.push_back(r.id);
        }
        return ids;
    }

private:
    enum class DragKind { none, scrub, move, copy, trimStart, trimEnd, marker, loopNew, loopStart, loopEnd, loopMove, marquee };
    struct Drag {
        DragKind kind{DragKind::none};
        bool active{};
        juce::Point<int> origin;
        juce::ModifierKeys mods;
        double anchorTick{}, grabOffset{};
        std::int64_t newAnchor{}, edgeTick{};
        std::uint32_t grabbed{}, pendingToggle{};
        bool wasSelected{}, additive{}, collapseOnClick{};
        int emptyRow{};
        timeline::Ids ids, previousSelection;
        std::optional<timeline::Timeline> base;
        std::function<bool(timeline::Timeline&)> apply;
        std::shared_ptr<timeline::Ids> created;
        juce::String label;
        juce::Rectangle<int> marquee;
    };

    std::optional<timeline::Timeline> baseTimeline() const {
        auto t = committed();
        t.tempo = controller.document().bpm;
        return t;
    }
    void updatePreview() {
        if (!drag.base || !drag.apply) return;
        auto t = *drag.base;
        if (drag.apply(t) && t.valid(trackCount(), controller.document().sceneCount(), controller.channelRack().patternCount)) preview = std::move(t);
        else preview.reset();
        repaint();
    }
    bool edit(const juce::String& label, const std::function<bool(timeline::Timeline&)>& change, const juce::String& key = {}) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        const bool done = controller.editTimeline(label, change, key);
        if (done) afterEdit();
        return done;
    }
    void afterEdit() {
        timeline::Ids keep;
        for (const auto id : selected) if (committed().find(id) != nullptr) keep.push_back(id);
        selected = keep;
        cursor = std::clamp<std::int64_t>(cursor, 0, committed().length());
        scrollX = std::clamp(scrollX, 0.0, maxScrollTicks());
        repaint(); notifyView(); stateChanged();
    }
    void say(const juce::String& text) { if (onMessage) onMessage(text); }
    void stateChanged() { repaint(); if (onStateChanged) onStateChanged(); }
    void notifyView() { if (onViewChanged) onViewChanged(); }
    const timeline::Marker* markerById(std::uint32_t id) const {
        for (const auto& m : committed().markers) if (m.id == id) return &m;
        return nullptr;
    }
    void launch(juce::PopupMenu menu, juce::Rectangle<int> area, std::function<void(int)> chosen) {
        juce::Component::SafePointer<TimelineCanvas> safe(this);
        std::function<void(int)> done = [safe, chosen = std::move(chosen)](int id) { if (safe != nullptr && id != 0) chosen(id); };
        if (menuLauncher) { menuLauncher(std::move(menu), area, std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(area)), std::move(done));
    }
    void autoScroll(int x) {
        if (drag.kind == DragKind::marquee || drag.kind == DragKind::move || drag.kind == DragKind::copy || drag.kind == DragKind::trimEnd
            || drag.kind == DragKind::loopNew || drag.kind == DragKind::loopEnd || drag.kind == DragKind::scrub) {
            const int right = getWidth() - 24;
            if (x > right) setScrollTicks(scrollX + std::min(40, x - right) * 0.6 / ppt);
            else if (x < headerWidth + 24 && scrollX > 0) setScrollTicks(scrollX - std::min(40, headerWidth + 24 - x) * 0.6 / ppt);
        }
    }

    // Right-click: a menu for what is under the pointer.
    void rightClick(const Hit& hit, const juce::MouseEvent& e) {
        const auto tick = snapTick(tickAtX(static_cast<double>(e.x)), e.mods.isAltDown());
        const auto area = juce::Rectangle<int>(e.x, e.y, 1, 1);
        if (hit.zone == Zone::region) {
            if (!isSelected(hit.region)) setSelection({hit.region});
            setCursor(tick, false);
            const auto* r = committed().find(hit.region);
            const bool anyGrouped = std::any_of(selected.begin(), selected.end(), [&](std::uint32_t id) { const auto* x = committed().find(id); return x != nullptr && x->group != 0; });
            bool allMuted = true;
            for (const auto id : selected) { const auto* x = committed().find(id); allMuted = allMuted && x != nullptr && x->muted; }
            juce::PopupMenu menu;
            menu.addItem(cmdSplit, "Split at play position", r != nullptr && cursor > r->start && cursor < r->end());
            menu.addItem(cmdMute, allMuted ? "Unmute" : "Mute");
            if (r != nullptr && !r->isPattern()) {
                menu.addSeparator();
                menu.addItem(cmdSlipLeft, "Slip source left"); menu.addItem(cmdSlipRight, "Slip source right");
                menu.addItem(cmdSourceLoop, "Loop source"); menu.addItem(cmdReverse, "Reverse"); menu.addItem(cmdNormalize, "Normalize");
                menu.addItem(cmdFade, "Fade in and out"); menu.addItem(cmdCrossfade, "Crossfade", selected.size() == 2);
            }
            menu.addSeparator();
            menu.addItem(cmdDuplicate, "Duplicate");
            menu.addItem(cmdCopy, "Copy"); menu.addItem(cmdCut, "Cut"); menu.addItem(cmdDelete, "Delete");
            menu.addSeparator();
            menu.addItem(cmdGroup, "Group", selected.size() > 1); menu.addItem(cmdUngroup, "Ungroup", anyGrouped);
            menu.addSeparator();
            menu.addItem(cmdOpen, r != nullptr && r->isPattern() ? "Open in Channel Rack" : "Open clip");
            launch(std::move(menu), area, [this, region = hit.region](int id) {
                switch (id) {
                case cmdSplit: splitAtCursor(); break; case cmdMute: toggleMuteSelection(); break;
                case cmdSlipLeft: slipSelection(-timeline::ticksPerStep); break; case cmdSlipRight: slipSelection(timeline::ticksPerStep); break;
                case cmdSourceLoop: toggleSourceLoopSelection(); break; case cmdReverse: toggleReverseSelection(); break;
                case cmdNormalize: normalizeSelection(); break; case cmdFade: fadeSelection(); break; case cmdCrossfade: crossfadeSelection(); break;
                case cmdDuplicate: duplicateSelection(); break; case cmdCopy: copySelection(); break;
                case cmdCut: cutSelection(); break; case cmdDelete: deleteSelection(); break;
                case cmdGroup: groupSelection(); break; case cmdUngroup: ungroupSelection(); break;
                case cmdOpen: if (const auto* r = committed().find(region); r != nullptr && onOpenSource) onOpenSource(r->row, r->source); break;
                default: break;
                }
            });
        } else if (hit.zone == Zone::lane && hit.displayRow >= 0) {
            const int row = timelineRow(hit.displayRow);
            setFocusRow(row); setCursor(tick, false);
            auto menu = placeMenu(row);
            juce::PopupMenu full;
            full.addSectionHeader(timeline::isPatternRow(row) ? "Place pattern here" : "Place clip here");
            full.addSeparator();
            juce::PopupMenu::MenuItemIterator it(menu, true);
            while (it.next()) full.addItem(it.getItem());
            full.addSeparator();
            full.addItem(cmdPaste, "Paste", !pastable().empty());
            full.addItem(cmdSelectAll, "Select all");
            full.addItem(cmdAddMarker, "Add marker here");
            launch(std::move(full), area, [this, row, tick](int id) {
                if (id == placeNewClip) newClipHere(row, tick);
                else if (id >= placePatternBase && id < placePatternBase + 1000) place(row, id - placePatternBase, tick);
                else if (id >= placeSceneBase && id < placeSceneBase + 1000) place(row, id - placeSceneBase, tick);
                else if (id == cmdPaste) paste(tick);
                else if (id == cmdSelectAll) selectAll();
                else if (id == cmdAddMarker) { setCursor(tick, false); addMarkerAtCursor(); }
            });
        } else if (hit.zone == Zone::markerStrip && hit.marker != 0) {
            selected.clear(); selectedMarker = hit.marker; stateChanged();
            juce::PopupMenu menu, colours;
            for (int c = 0; c < 8; ++c) colours.addColouredItem(cmdColourBase + c, "Colour " + juce::String(c + 1), ui::trackColour(c));
            menu.addItem(cmdJump, "Play from here");
            menu.addItem(cmdRename, "Rename...");
            menu.addSubMenu("Colour", colours);
            menu.addItem(cmdRemoveMarker, "Delete marker");
            launch(std::move(menu), area, [this, marker = hit.marker](int id) {
                if (id == cmdJump) { if (const auto* m = markerById(marker)) setCursor(m->tick); }
                else if (id == cmdRename) renameMarker(marker);
                else if (id == cmdRemoveMarker) removeMarker(marker);
                else if (id >= cmdColourBase && id < cmdColourBase + 8) colourMarker(marker, id - cmdColourBase);
            });
        } else if (hit.zone == Zone::markerStrip || hit.zone == Zone::ruler) {
            juce::PopupMenu menu;
            menu.addItem(cmdAddMarker, "Add marker here");
            launch(std::move(menu), area, [this, tick](int id) { if (id == cmdAddMarker) { setCursor(tick, false); addMarkerAtCursor(); } });
        } else if (hit.zone == Zone::loopStrip) {
            juce::PopupMenu menu;
            menu.addItem(cmdJump, committed().loopEnabled ? "Loop off" : "Loop on");
            menu.addItem(cmdLoopSelection, "Loop the selected clips", !selected.empty());
            launch(std::move(menu), area, [this](int id) { if (id == cmdJump) toggleLoop(); else if (id == cmdLoopSelection) loopSelection(); });
        } else if (hit.zone == Zone::header && hit.displayRow >= 0 && !timeline::isPatternRow(timelineRow(hit.displayRow))) {
            const int track = timelineRow(hit.displayRow);
            launch(recordingMenu(track), area, [this, track](int id) { recordingMenuChosen(track, id); });
        }
    }

    // ---- recording settings of a track (header) -------------------------------------------------------------------
public:
    // Enabled audio inputs of the current device, in the order the audio callback receives them. Set by the owner.
    std::function<juce::StringArray()> inputNames;
    juce::Rectangle<int> armRect(int displayRow) const { return {headerWidth - 28, rowTop(displayRow) + 6, 20, 20}; }
    bool toggleArm(int track) {
        const auto& d = controller.document();
        if (track < 0 || track >= d.trackCount()) return false;
        const bool arm = !d.tracks[static_cast<std::size_t>(track)].settings.armed;
        const bool done = controller.editTrackRecording(track, (arm ? "Arm " : "Disarm ") + d.tracks[static_cast<std::size_t>(track)].name,
                                                       [arm](TrackSettings& s) { s.armed = arm; });
        if (done && arm && !inputAvailable(track)) say("Track armed, but its input is not enabled. Enable it in Audio settings or pick another input (right-click the track name).");
        repaint(0, topHeight, headerWidth, getHeight() - topHeight);
        return done;
    }
    bool setTrackInput(int track, int first, bool stereo) {
        const bool done = controller.editTrackRecording(track, "Change track input", [=](TrackSettings& s) { s.input = first; s.stereoInput = stereo; });
        repaint(0, topHeight, headerWidth, getHeight() - topHeight);
        return done;
    }
    bool setTrackMonitor(int track, InputMonitor mode) {
        const bool done = controller.editTrackRecording(track, "Change input monitoring", [=](TrackSettings& s) { s.monitor = static_cast<int>(mode); });
        repaint(0, topHeight, headerWidth, getHeight() - topHeight);
        return done;
    }
    // "In 1", "In 1+2" (the device's channel names in the menu), and whether the device has that input enabled.
    juce::String inputText(int track) const {
        const auto& s = controller.document().tracks[static_cast<std::size_t>(track)].settings;
        return "In " + juce::String(s.input + 1) + (s.stereoInput ? "+" + juce::String(s.input + 2) : juce::String());
    }
    bool inputAvailable(int track) const {
        const auto& s = controller.document().tracks[static_cast<std::size_t>(track)].settings;
        const int count = inputNames ? inputNames().size() : 0;
        return s.input + (s.stereoInput ? 1 : 0) < count;
    }
    // Called a few times a second: reads each track's input level and repaints the headers whose meters moved.
    void updateInputMeters() {
        const auto& d = controller.document();
        inputLevels.resize(static_cast<std::size_t>(d.trackCount()), 0.0f);
        bool moved = false;
        for (int t = 0; t < d.trackCount(); ++t) {
            auto& shown = inputLevels[static_cast<std::size_t>(t)];
            const float next = std::max(controller.takeInputLevel(t), shown * 0.7f);
            const float level = next < 0.001f ? 0.0f : next;
            moved = moved || std::abs(level - shown) > 0.002f;
            shown = level;
        }
        if (moved) repaint(0, topHeight, headerWidth, getHeight() - topHeight);
    }
    float inputLevel(int track) const { return track >= 0 && track < static_cast<int>(inputLevels.size()) ? inputLevels[static_cast<std::size_t>(track)] : 0.0f; }
private:
    std::vector<float> inputLevels;
    juce::PopupMenu recordingMenu(int track) const {
        const auto& s = controller.document().tracks[static_cast<std::size_t>(track)].settings;
        const auto names = inputNames ? inputNames() : juce::StringArray();
        juce::PopupMenu menu, inputs, monitor;
        if (names.isEmpty()) inputs.addItem(cmdInputMonoBase, "No inputs are enabled (Audio settings)", false);
        for (int i = 0; i < names.size() && i < maxTrackInputs; ++i)
            inputs.addItem(cmdInputMonoBase + i, "Mono: " + juce::String(i + 1) + "  " + names[i], true, !s.stereoInput && s.input == i);
        if (names.size() > 1) inputs.addSeparator();
        for (int i = 0; i + 1 < names.size() && i + 1 < maxTrackInputs; i += 2)
            inputs.addItem(cmdInputStereoBase + i, "Stereo: " + juce::String(i + 1) + "+" + juce::String(i + 2) + "  " + names[i] + " / " + names[i + 1],
                           true, s.stereoInput && s.input == i);
        monitor.addItem(cmdMonitorBase + static_cast<int>(InputMonitor::automatic), "Auto (while armed)", true, s.monitor == static_cast<int>(InputMonitor::automatic));
        monitor.addItem(cmdMonitorBase + static_cast<int>(InputMonitor::always), "In (always)", true, s.monitor == static_cast<int>(InputMonitor::always));
        monitor.addItem(cmdMonitorBase + static_cast<int>(InputMonitor::off), "Off", true, s.monitor == static_cast<int>(InputMonitor::off));
        menu.addItem(cmdArm, s.armed ? "Disarm recording" : "Arm for recording", true, s.armed);
        menu.addSubMenu("Input (" + inputText(track) + ")", inputs);
        menu.addSubMenu("Monitor input", monitor);
        return menu;
    }
    void recordingMenuChosen(int track, int id) {
        if (id == cmdArm) toggleArm(track);
        else if (id >= cmdMonitorBase && id <= cmdMonitorBase + 2) setTrackMonitor(track, static_cast<InputMonitor>(id - cmdMonitorBase));
        else if (id >= cmdInputMonoBase && id < cmdInputMonoBase + maxTrackInputs) setTrackInput(track, id - cmdInputMonoBase, false);
        else if (id >= cmdInputStereoBase && id < cmdInputStereoBase + maxTrackInputs) setTrackInput(track, id - cmdInputStereoBase, true);
    }

    // ---- painting pieces ----------------------------------------------------------------------------------------
    void repaintPlayhead(std::int64_t tick) {
        if (tick < 0) return;
        const int x = juce::roundToInt(xForTick(static_cast<double>(tick)));
        if (x >= headerWidth - 2 && x <= getWidth() + 2) repaint(x - 3, 0, 7, getHeight());
    }
    // The finest grid worth drawing at this zoom, in ticks.
    std::int64_t fineGrid() const noexcept {
        static constexpr std::array<std::int64_t, 10> steps{240, 480, 960, 1920, 3840, 7680, 15360, 30720, 61440, 122880};
        for (const auto s : steps) if (static_cast<double>(s) * ppt >= 9.0) return s;
        return steps.back();
    }
    int barLabelStep() const noexcept {
        for (const int s : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096}) if (static_cast<double>(s) * timeline::ticksPerBar * ppt >= 50.0) return s;
        return 8192;
    }
    juce::Colour rowColour(int row) const {
        if (timeline::isPatternRow(row)) return ui::colours::warning;
        const auto& d = controller.document();
        return row >= 0 && row < d.trackCount() ? ui::trackColour(d.tracks[static_cast<std::size_t>(row)].colour) : ui::colours::accent;
    }
    juce::String regionName(const timeline::Region& r) const {
        const auto& d = controller.document();
        if (r.isPattern()) {
            const auto& p = controller.channelRack();
            return p.validPattern(r.source) ? juce::String::fromUTF8(p.patternNames[static_cast<std::size_t>(r.source)].c_str()) : juce::String("Pattern");
        }
        if (!d.isValidCell(r.row, r.source)) return "Clip";
        const auto& clip = d.clip(r.row, r.source);
        const auto scene = d.scenes[static_cast<std::size_t>(r.source)].name;
        return clip.isAudioClip() && clip.audioFileName.isNotEmpty() ? scene + ui::utf8("  \xc2\xb7  ") + clip.audioFileName : scene;
    }

    void paintLanes(juce::Graphics& g, const timeline::Timeline& t, juce::Rectangle<int> lanes) {
        namespace col = ui::colours;
        const int rows = displayRowCount();
        for (int d = 0; d < rows; ++d) {
            const int y = rowTop(d);
            if (y + rowHeight < topHeight || y > getHeight()) continue;
            g.setColour(d % 2 == 0 ? col::surface : col::raised.withAlpha(0.55f));
            g.fillRect(lanes.getX(), y, lanes.getWidth(), rowHeight);
            if (d == displayRowOf(focus)) { g.setColour(col::accent.withAlpha(0.05f)); g.fillRect(lanes.getX(), y, lanes.getWidth(), rowHeight); }
            if (d == trackCount()) { g.setColour(col::lineStrong); g.fillRect(lanes.getX(), y, lanes.getWidth(), 2); }
        }
        // The song's end, and the loop.
        const float endX = static_cast<float>(xForTick(static_cast<double>(t.length())));
        if (endX < getWidth()) { g.setColour(col::window.withAlpha(0.55f)); g.fillRect(juce::Rectangle<float>(std::max(endX, static_cast<float>(headerWidth)), static_cast<float>(topHeight), getWidth() - endX, static_cast<float>(getHeight() - topHeight))); }
        if (t.loopEnabled) {
            const auto loop = loopBounds();
            g.setColour(col::accent.withAlpha(0.07f));
            g.fillRect(juce::Rectangle<float>(loop.getX(), static_cast<float>(topHeight), loop.getWidth(), static_cast<float>(getHeight() - topHeight)));
        }
        // Grid lines.
        const auto fine = fineGrid();
        const double firstTick = std::floor(scrollX / static_cast<double>(fine)) * static_cast<double>(fine);
        for (double tick = firstTick; tick <= scrollX + visibleTicks() + static_cast<double>(fine); tick += static_cast<double>(fine)) {
            const auto whole = static_cast<std::int64_t>(std::llround(tick));
            const float x = static_cast<float>(xForTick(static_cast<double>(whole)));
            if (x < headerWidth || x > getWidth()) continue;
            const bool bar = whole % timeline::ticksPerBar == 0, beat = whole % timeline::ticksPerBeat == 0;
            g.setColour((bar ? col::lineStrong : beat ? col::line.brighter(0.15f) : col::line).withAlpha(bar ? 1.0f : 0.7f));
            g.fillRect(x, static_cast<float>(topHeight), 1.0f, static_cast<float>(getHeight() - topHeight));
        }
        // Markers run down through the lanes.
        for (const auto& m : t.markers) {
            const float x = static_cast<float>(xForTick(static_cast<double>(m.tick)));
            if (x < headerWidth || x > getWidth()) continue;
            g.setColour(ui::trackColour(m.colour).withAlpha(m.id == selectedMarker ? 0.7f : 0.3f));
            g.fillRect(x, static_cast<float>(topHeight), 1.0f, static_cast<float>(getHeight() - topHeight));
        }
        // Regions.
        for (const auto& r : t.regions) {
            const auto box = regionBounds(r);
            if (box.getRight() < headerWidth || box.getX() > getWidth() || box.getBottom() < topHeight || box.getY() > getHeight()) continue;
            paintRegion(g, r, box, isSelected(r.id));
        }
        if (preview && drag.base && (drag.kind == DragKind::move || drag.kind == DragKind::copy)) {
            // A guide where the grabbed clip will start.
            if (const auto* r = preview->find(drag.grabbed)) {
                const float x = static_cast<float>(xForTick(static_cast<double>(r->start)));
                g.setColour(col::accent.withAlpha(0.6f)); g.fillRect(x, static_cast<float>(topHeight), 1.0f, static_cast<float>(getHeight() - topHeight));
            }
        }
        if (dropGhost.valid) {
            // A row from the clip list is being dragged over: show the lane and where its clip would sit.
            const int displayRow = displayRowOf(dropGhost.row);
            g.setColour(col::accent.withAlpha(0.10f));
            g.fillRect(lanes.getX(), rowTop(displayRow), lanes.getWidth(), rowHeight);
            timeline::Region ghost; ghost.row = dropGhost.row; ghost.source = dropGhost.source; ghost.start = dropGhost.tick; ghost.length = dropGhost.length;
            const auto box = regionBounds(ghost);
            g.setColour(rowColour(ghost.row).withAlpha(0.35f)); g.fillRoundedRectangle(box, 4.0f);
            g.setColour(col::accent); g.drawRoundedRectangle(box, 4.0f, 1.6f);
            const float x = static_cast<float>(xForTick(static_cast<double>(dropGhost.tick)));
            g.fillRect(x, static_cast<float>(topHeight), 1.0f, static_cast<float>(getHeight() - topHeight));
        }
        // The play position, then the playhead.
        const float cx = static_cast<float>(xForTick(static_cast<double>(cursor)));
        if (cx >= headerWidth && cx <= getWidth()) { g.setColour(col::solo.withAlpha(0.9f)); g.fillRect(cx, static_cast<float>(topHeight), 1.5f, static_cast<float>(getHeight() - topHeight)); }
        if (playhead >= 0) {
            const float px = static_cast<float>(xForTick(static_cast<double>(playhead)));
            if (px >= headerWidth && px <= getWidth()) { g.setColour(col::warning); g.fillRect(px, static_cast<float>(topHeight), 2.0f, static_cast<float>(getHeight() - topHeight)); }
        }
        if (drag.kind == DragKind::marquee && drag.active) {
            g.setColour(col::accent.withAlpha(0.12f)); g.fillRect(drag.marquee);
            g.setColour(col::accent.withAlpha(0.8f)); g.drawRect(drag.marquee, 1);
        }
    }

    void paintRegion(juce::Graphics& g, const timeline::Region& r, juce::Rectangle<float> box, bool isSel) {
        namespace col = ui::colours;
        const auto colour = rowColour(r.row);
        const float alpha = r.muted ? 0.10f : isSel ? 0.46f : 0.26f;
        g.setColour(colour.withAlpha(alpha));
        g.fillRoundedRectangle(box, 4.0f);
        {
            juce::Graphics::ScopedSaveState state(g);
            juce::Path clip; clip.addRoundedRectangle(box, 4.0f);
            g.reduceClipRegion(box.toNearestInt());
            g.setColour(colour.withAlpha(r.muted ? 0.35f : 0.95f));
            paintContent(g, r, box, colour.withAlpha(r.muted ? 0.35f : 0.9f));
            if (r.muted) {
                g.setColour(col::textFaint.withAlpha(0.35f));
                for (float x = box.getX() - box.getHeight(); x < box.getRight(); x += 9.0f) g.drawLine(x, box.getBottom(), x + box.getHeight(), box.getY(), 1.0f);
            }
            if (box.getWidth() > 24.0f) {
                g.setColour(col::text.withAlpha(r.muted ? 0.5f : 0.95f));
                g.setFont(ui::font(11.0f, true));
                g.drawText(regionName(r), box.withTrimmedLeft(6.0f).withTrimmedRight(14.0f).withHeight(15.0f).toNearestInt(), juce::Justification::centredLeft, true);
            }
            if (r.group != 0 && box.getWidth() > 16.0f) {
                g.setColour(col::text.withAlpha(0.8f));
                const auto link = juce::Rectangle<float>(box.getRight() - 14.0f, box.getY() + 4.0f, 9.0f, 6.0f);
                g.drawRoundedRectangle(link, 2.5f, 1.2f);
            }
        }
        g.setColour(colour.withAlpha(r.muted ? 0.4f : 1.0f));
        g.fillRoundedRectangle(box.withWidth(std::min(3.0f, box.getWidth())), 1.5f);
        g.setColour(isSel ? juce::Colours::white : colour.withAlpha(0.65f));
        g.drawRoundedRectangle(box, 4.0f, isSel ? 1.6f : 1.0f);
    }

    // What is inside a region: notes, a waveform, or a pattern's steps, repeated for as long as the region lasts.
    void paintContent(juce::Graphics& g, const timeline::Region& r, juce::Rectangle<float> box, juce::Colour ink) {
        const auto& d = controller.document();
        const auto inner = box.withTrimmedTop(16.0f).withTrimmedBottom(3.0f).reduced(2.0f, 0.0f);
        if (inner.getHeight() < 4.0f || box.getWidth() < 12.0f) return;
        const float visibleLeft = std::max(inner.getX(), static_cast<float>(headerWidth)), visibleRight = std::min(inner.getRight(), static_cast<float>(getWidth()));
        const double x0 = xForTick(static_cast<double>(r.start));
        if (r.isPattern()) {
            const auto& p = controller.channelRack();
            if (!p.validPattern(r.source)) return;
            const auto& pattern = p.pattern(r.source);
            const std::int64_t length = static_cast<std::int64_t>(pattern.length) * timeline::ticksPerStep;
            int used = 0;
            std::array<int, rack::maxChannels> lane{};
            for (int c = 0; c < p.channelCount; ++c) {
                bool any = false;
                for (int s = 0; s < pattern.length; ++s) any = any || pattern.steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)].on;
                if (any) lane[static_cast<std::size_t>(c)] = used++;
                else lane[static_cast<std::size_t>(c)] = -1;
            }
            if (used == 0) return;
            const float laneH = inner.getHeight() / static_cast<float>(used);
            const std::int64_t firstCycle = r.offset / length, lastCycle = (r.offset + r.length - 1) / length;
            if (lastCycle - firstCycle > 400) return;
            g.setColour(ink);
            for (std::int64_t cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const std::int64_t cycleStart = cycle * length - r.offset;   // tick within the region where this pass begins
                if (cycle > firstCycle) { g.setColour(ink.withAlpha(0.25f)); g.fillRect(static_cast<float>(x0 + cycleStart * ppt), inner.getY(), 1.0f, inner.getHeight()); g.setColour(ink); }
                for (int c = 0; c < p.channelCount; ++c) {
                    if (lane[static_cast<std::size_t>(c)] < 0) continue;
                    for (int s = 0; s < pattern.length; ++s) {
                        if (!pattern.steps[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)].on) continue;
                        const std::int64_t at = cycleStart + static_cast<std::int64_t>(s) * timeline::ticksPerStep;
                        if (at < 0 || at >= r.length) continue;
                        const float x = static_cast<float>(x0 + at * ppt);
                        if (x + 2.0f < visibleLeft || x > visibleRight) continue;
                        const float w = std::max(1.5f, static_cast<float>(timeline::ticksPerStep * ppt) - 1.0f);
                        g.fillRect(x, inner.getY() + laneH * static_cast<float>(lane[static_cast<std::size_t>(c)]) + 0.5f, w, std::max(1.5f, laneH - 1.0f));
                    }
                }
            }
            return;
        }
        if (!d.isValidCell(r.row, r.source)) return;
        const auto& clip = d.clip(r.row, r.source);
        if (clip.audio != nullptr) {
            const auto& audio = *clip.audio;
            if (audio.streamed() && audio.waveformCount == 0) {   // too long to keep in memory: its waveform is in the clip detail view
                g.setColour(ink.withAlpha(0.45f));
                g.fillRect(inner.getX(), inner.getCentreY(), inner.getWidth(), 1.0f);
                g.setFont(ui::font(10.0f));
                g.drawText("streamed from disk", inner.toNearestInt(), juce::Justification::bottomLeft, true);
                return;
            }
            if (audio.waveformCount == 0 || audio.frames == 0) return;
            const double duration = static_cast<double>(audio.frames) / audio.sampleRate;
            const float mid = inner.getCentreY(), half = inner.getHeight() * 0.5f;
            g.setColour(ink);
            const int first = static_cast<int>(std::floor(visibleLeft)), last = static_cast<int>(std::ceil(visibleRight));
            for (int px = first; px <= last; ++px) {
                const double tickInRegion = (static_cast<double>(px) - x0) / ppt;
                if (tickInRegion < 0 || tickInRegion >= static_cast<double>(r.length)) continue;
                const double seconds = r.audioOffset + timeline::secondsForTicks(tickInRegion, controller.document().bpm);
                if (seconds >= duration) break;
                const double secondsNext = r.audioOffset + timeline::secondsForTicks(tickInRegion + 1.0 / ppt, controller.document().bpm);
                const auto b0 = std::min(audio.waveformCount - 1, static_cast<std::size_t>(seconds / duration * static_cast<double>(audio.waveformCount)));
                const auto b1 = std::min(audio.waveformCount - 1, std::max(b0, static_cast<std::size_t>(std::min(secondsNext, duration) / duration * static_cast<double>(audio.waveformCount))));
                float amp = 0.0f;
                for (auto b = b0; b <= b1; ++b) {
                    const auto& peak = audio.waveform[b];
                    amp = std::max({amp, std::abs(peak.leftMin), std::abs(peak.leftMax), std::abs(peak.rightMin), std::abs(peak.rightMax)});
                }
                const float h = std::max(0.5f, std::min(1.0f, amp) * half);
                g.fillRect(static_cast<float>(px), mid - h, 1.0f, h * 2.0f);
            }
            return;
        }
        if (clip.audioOffline()) {
            g.setColour(ui::colours::danger.withAlpha(0.8f)); g.setFont(ui::font(10.5f));
            g.drawText("audio file missing", inner.toNearestInt(), juce::Justification::centredLeft, true);
            return;
        }
        const auto& notes = clip.notes();
        if (notes.count == 0) return;
        const std::int64_t length = static_cast<std::int64_t>(notes.loopSteps) * timeline::ticksPerStep;
        int low = 127, high = 0;
        for (std::size_t i = 0; i < notes.count; ++i) { low = std::min(low, notes.notes[i].pitch); high = std::max(high, notes.notes[i].pitch); }
        const float noteH = std::clamp(inner.getHeight() / static_cast<float>(high - low + 1), 1.5f, 5.0f);
        const float span = inner.getHeight() - noteH;
        const std::int64_t firstCycle = r.offset / length, lastCycle = (r.offset + r.length - 1) / length;
        if (lastCycle - firstCycle > 400) return;
        for (std::int64_t cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            const std::int64_t cycleStart = cycle * length - r.offset;
            if (cycle > firstCycle) { g.setColour(ink.withAlpha(0.25f)); g.fillRect(static_cast<float>(x0 + cycleStart * ppt), inner.getY(), 1.0f, inner.getHeight()); }
            g.setColour(ink);
            for (std::size_t i = 0; i < notes.count; ++i) {
                const auto& n = notes.notes[i];
                std::int64_t a = cycleStart + static_cast<std::int64_t>(std::llround(n.startStep * timeline::ticksPerStep));
                std::int64_t b = a + static_cast<std::int64_t>(std::llround(n.lengthSteps * timeline::ticksPerStep));
                a = std::max<std::int64_t>(a, 0); b = std::min<std::int64_t>(b, r.length);
                if (b <= a) continue;
                const float x = static_cast<float>(x0 + a * ppt), w = std::max(1.5f, static_cast<float>((b - a) * ppt));
                if (x + w < visibleLeft || x > visibleRight) continue;
                const float y = inner.getY() + (high == low ? span * 0.5f : span * static_cast<float>(high - n.pitch) / static_cast<float>(high - low));
                g.fillRect(x, y, w, noteH);
            }
        }
    }

    void paintHeader(juce::Graphics& g) {
        namespace col = ui::colours;
        const auto& d = controller.document();
        const auto area = juce::Rectangle<int>(0, topHeight, headerWidth, getHeight() - topHeight);
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(area);
        g.setColour(col::raised); g.fillRect(area);
        for (int i = 0; i < displayRowCount(); ++i) {
            const int y = rowTop(i);
            if (y + rowHeight < topHeight || y > getHeight()) continue;
            const int row = timelineRow(i);
            const bool isLane = timeline::isPatternRow(row);
            g.setColour(i == displayRowOf(focus) ? col::hover : (i % 2 == 0 ? col::raised : col::control.withAlpha(0.5f)));
            g.fillRect(0, y, headerWidth - 1, rowHeight);
            g.setColour(rowColour(row)); g.fillRect(0, y, 4, rowHeight);
            g.setColour(col::text); g.setFont(ui::font(12.0f, true));
            const juce::String name = isLane ? "Pattern lane " + juce::String(row - timeline::rackRowBase + 1) : d.tracks[static_cast<std::size_t>(row)].name;
            const int textWidth = headerWidth - (isLane ? 20 : 48);
            g.drawText(name, 12, y + 7, textWidth, 16, juce::Justification::centredLeft, true);
            g.setColour(col::textFaint); g.setFont(ui::font(10.5f));
            if (isLane) { g.drawText("Channel Rack", 12, y + 24, textWidth, 14, juce::Justification::centredLeft, true); }
            else paintRecordingControls(g, i, row);
            g.setColour(col::line); g.fillRect(0, y + rowHeight - 1, headerWidth, 1);
        }
        g.setColour(col::lineStrong); g.fillRect(headerWidth - 1, topHeight, 1, getHeight() - topHeight);
    }

    // The second line of a track's header (its input, and whether it monitors), its arm button and its input meter.
    void paintRecordingControls(juce::Graphics& g, int displayRow, int track) {
        namespace col = ui::colours;
        const auto& s = controller.document().tracks[static_cast<std::size_t>(track)].settings;
        const int y = rowTop(displayRow);
        const bool available = inputAvailable(track);
        juce::String line = inputText(track);
        if (s.monitoring()) line << "  ·  monitoring";
        else if (s.monitor == static_cast<int>(InputMonitor::off)) line << "  ·  monitor off";
        g.setColour(!available && (s.armed || s.monitoring()) ? col::warning : col::textFaint);
        g.setFont(ui::font(10.5f));
        g.drawText(available || !(s.armed || s.monitoring()) ? line : inputText(track) + "  ·  input not enabled", 12, y + 24, headerWidth - 48, 14,
                   juce::Justification::centredLeft, true);
        const auto arm = armRect(displayRow).toFloat();
        const auto recordRed = juce::Colour(0xffe5484d);
        g.setColour(s.armed ? recordRed : col::control);
        g.fillEllipse(arm);
        g.setColour(s.armed ? recordRed.brighter(0.4f) : col::textFaint);
        g.drawEllipse(arm.reduced(0.5f), 1.0f);
        g.setColour(s.armed ? juce::Colours::white : col::textFaint);
        g.fillEllipse(arm.withSizeKeepingCentre(7.0f, 7.0f));
        if (s.armed || s.monitoring()) {   // input level along the bottom of the row
            const float level = std::min(1.0f, inputLevel(track));
            const auto meter = juce::Rectangle<float>(12.0f, static_cast<float>(y + rowHeight - 5), static_cast<float>(headerWidth - 24), 2.0f);
            g.setColour(col::inset); g.fillRect(meter);
            g.setColour(level > 0.95f ? recordRed : col::accent);
            g.fillRect(meter.withWidth(meter.getWidth() * level));
        }
    }

    void paintTop(juce::Graphics& g, const timeline::Timeline& t) {
        namespace col = ui::colours;
        g.setColour(col::raised); g.fillRect(0, 0, getWidth(), topHeight);
        g.setColour(col::textFaint); g.setFont(ui::font(10.5f, true));
        g.drawText("MARKERS", 12, 0, headerWidth - 16, markerStripHeight, juce::Justification::centredLeft);
        g.drawText("LOOP", 12, markerStripHeight, headerWidth - 16, loopStripHeight, juce::Justification::centredLeft);
        g.drawText("BARS", 12, markerStripHeight + loopStripHeight, headerWidth - 16, rulerHeight, juce::Justification::centredLeft);
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(headerWidth, 0, getWidth() - headerWidth, topHeight);
        // Loop strip.
        {
            const auto loop = loopBounds();
            g.setColour(col::inset); g.fillRect(headerWidth, markerStripHeight, getWidth() - headerWidth, loopStripHeight);
            g.setColour(t.loopEnabled ? col::accent.withAlpha(0.55f) : col::textFaint.withAlpha(0.3f));
            g.fillRect(loop.reduced(0.0f, 2.0f));
            g.setColour(t.loopEnabled ? col::accent : col::textFaint);
            g.fillRect(loop.getX(), loop.getY() + 1.0f, 2.0f, loop.getHeight() - 2.0f);
            g.fillRect(loop.getRight() - 2.0f, loop.getY() + 1.0f, 2.0f, loop.getHeight() - 2.0f);
        }
        // Ruler: a number every few bars, ticks at the bars, beats and sixteenths that fit.
        const int labelStep = barLabelStep();
        const auto fine = fineGrid();
        const double firstTick = std::floor(scrollX / static_cast<double>(fine)) * static_cast<double>(fine);
        const int rulerTop = markerStripHeight + loopStripHeight;
        for (double tick = firstTick; tick <= scrollX + visibleTicks() + static_cast<double>(fine); tick += static_cast<double>(fine)) {
            const auto whole = static_cast<std::int64_t>(std::llround(tick));
            const float x = static_cast<float>(xForTick(static_cast<double>(whole)));
            if (x < headerWidth || x > getWidth()) continue;
            const bool bar = whole % timeline::ticksPerBar == 0, beat = whole % timeline::ticksPerBeat == 0;
            g.setColour(bar ? col::textDim : col::textFaint.withAlpha(0.7f));
            g.fillRect(x, static_cast<float>(rulerTop + (bar ? 4 : beat ? 14 : 18)), 1.0f, static_cast<float>(rulerHeight - (bar ? 4 : beat ? 14 : 18)));
            if (bar && (whole / timeline::ticksPerBar) % labelStep == 0) {
                g.setColour(col::text); g.setFont(ui::monoFont(10.5f));
                g.drawText(juce::String(whole / timeline::ticksPerBar + 1), juce::roundToInt(x) + 3, rulerTop, 40, 14, juce::Justification::centredLeft);
            } else if (beat && ppt * timeline::ticksPerBeat >= 30.0 && !bar) {
                g.setColour(col::textFaint); g.setFont(ui::monoFont(9.5f));
                g.drawText(juce::String(whole / timeline::ticksPerBar + 1) + "." + juce::String((whole % timeline::ticksPerBar) / timeline::ticksPerBeat + 1), juce::roundToInt(x) + 3, rulerTop + 2, 40, 12, juce::Justification::centredLeft);
            }
        }
        g.setColour(col::line); g.fillRect(headerWidth, topHeight - 1, getWidth() - headerWidth, 1);
        // Markers.
        for (const auto& m : t.markers) {
            const auto box = markerBounds(m);
            if (box.getRight() < headerWidth || box.getX() > getWidth()) continue;
            const auto colour = ui::trackColour(m.colour);
            g.setColour(colour.withAlpha(m.id == selectedMarker ? 1.0f : 0.8f));
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(col::accentInk); g.setFont(ui::font(10.5f, true));
            g.drawText(juce::String::fromUTF8(m.name.c_str()), box.toNearestInt().reduced(5, 0), juce::Justification::centredLeft, true);
            if (m.id == selectedMarker) { g.setColour(juce::Colours::white); g.drawRoundedRectangle(box, 3.0f, 1.5f); }
        }
        // The play position and the playhead on the ruler.
        const float cx = static_cast<float>(xForTick(static_cast<double>(cursor)));
        if (cx >= headerWidth && cx <= getWidth()) {
            g.setColour(col::solo);
            juce::Path flag; flag.addTriangle(cx - 5.0f, static_cast<float>(rulerTop), cx + 5.0f, static_cast<float>(rulerTop), cx, static_cast<float>(rulerTop) + 8.0f);
            g.fillPath(flag);
        }
        if (playhead >= 0) {
            const float px = static_cast<float>(xForTick(static_cast<double>(playhead)));
            if (px >= headerWidth && px <= getWidth()) { g.setColour(col::warning); g.fillRect(px, static_cast<float>(rulerTop), 2.0f, static_cast<float>(rulerHeight)); }
        }
    }

    ProjectController& controller;
    double ppt{defaultPixelsPerTick}, scrollX{0.0};
    int scrollYpx{0};
    tlui::Snap snapMode{tlui::Snap::automatic};
    timeline::Ids selected;
    std::uint32_t selectedMarker{0};
    std::int64_t cursor{0}, playhead{-1};
    int focus{0};
    bool follow{true};
    std::optional<timeline::Timeline> preview;
    DropPlan dropGhost;   // where a row dragged from the clip list would land, while it is over the lanes
    Drag drag;
};

// The whole screen: the toolbar, the timeline, its scroll bars and a line of help.

// One editable automation lane at a time. Edits stay in a local preview during a
// gesture and commit once on mouse-up, so a drag is one undo transaction.
class TrackAutomationEditor final : public juce::Component {
public:
    explicit TrackAutomationEditor(ProjectController& owner) : controller(owner) {
        setName("Track automation");
        addAndMakeVisible(trackBox); addAndMakeVisible(parameterBox); addAndMakeVisible(curveBox);
        addAndMakeVisible(enableButton); addAndMakeVisible(hint);
        trackBox.setName("Automation track"); parameterBox.setName("Automation parameter");
        parameterBox.addItem("Volume", 1); parameterBox.addItem("Pan", 2); parameterBox.setSelectedId(1);
        curveBox.setName("Automation interpolation");
        curveBox.addItem("Linear", 1); curveBox.addItem("Hold", 2); curveBox.setSelectedId(1);
        enableButton.setButtonText("Read"); enableButton.setClickingTogglesState(true);
        enableButton.setTooltip("Enable playback of this automation lane");
        hint.setText("Click to add; drag to move; right-click a point to delete. Values are absolute.", juce::dontSendNotification);
        hint.setColour(juce::Label::textColourId, ui::colours::textDim); hint.setFont(ui::font(11.0f));
        trackBox.onChange = parameterBox.onChange = [this] { preview.reset(); base.reset(); refresh(); };
        enableButton.onClick = [this] {
            const int track = selectedTrack(); const auto parameter = selectedParameter();
            controller.editTimeline("Toggle automation read", [=](timeline::Timeline& t) {
                for (auto& lane : t.automationLanes) if (lane.track == track && lane.parameter == parameter) {
                    lane.enabled = !lane.enabled; return true;
                }
                return false;
            });
            refresh();
        };
        refresh();
    }
    void refresh() {
        const auto& d = controller.document();
        const int previous = trackBox.getSelectedId();
        trackBox.clear(juce::dontSendNotification);
        for (int t = 0; t < d.trackCount(); ++t) trackBox.addItem("Track " + juce::String(t + 1), t + 1);
        trackBox.setSelectedId(juce::jlimit(1, d.trackCount(), previous > 0 ? previous : 1), juce::dontSendNotification);
        const auto* lane = find(controller.timeline());
        enableButton.setToggleState(lane != nullptr && lane->enabled, juce::dontSendNotification);
        enableButton.setEnabled(lane != nullptr && !controller.isBusy());
        repaint();
    }
    void resized() override {
        auto bar = getLocalBounds().removeFromTop(30).reduced(6, 3);
        trackBox.setBounds(bar.removeFromLeft(110)); bar.removeFromLeft(6);
        parameterBox.setBounds(bar.removeFromLeft(110)); bar.removeFromLeft(6);
        curveBox.setBounds(bar.removeFromLeft(95)); bar.removeFromLeft(6);
        enableButton.setBounds(bar.removeFromLeft(60)); hint.setBounds(bar);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::raised);
        const auto area = plot(); if (area.isEmpty()) return;
        g.setColour(ui::colours::window); g.fillRect(area);
        const auto& t = preview ? *preview : controller.timeline();
        const auto* lane = find(t);
        const bool volume = selectedParameter() == automation::Parameter::volume;
        const float baseline = volume ? 1.0f : 0.0f;
        const float baselineY = yFor(baseline);
        g.setColour(ui::colours::line); g.drawHorizontalLine(juce::roundToInt(baselineY), area.getX(), area.getRight());
        if (lane == nullptr || lane->points.empty()) return;
        g.setColour(lane->enabled ? ui::colours::accent : ui::colours::textDim);
        juce::Path path; path.startNewSubPath(area.getX(), yFor(lane->points.front().value));
        for (std::size_t i = 0; i < lane->points.size(); ++i) {
            const auto& p = lane->points[i]; const float x = xFor(p.tick), y = yFor(p.value);
            if (i > 0 && lane->points[i - 1].curve == automation::Curve::hold) path.lineTo(x, yFor(lane->points[i - 1].value));
            path.lineTo(x, y);
        }
        path.lineTo(area.getRight(), yFor(lane->points.back().value));
        g.strokePath(path, juce::PathStrokeType(1.5f));
        for (const auto& p : lane->points) g.fillEllipse(xFor(p.tick) - 4.0f, yFor(p.value) - 4.0f, 8.0f, 8.0f);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (controller.isBusy() || !plot().contains(e.position)) return;
        base = controller.timeline(); preview = *base;
        auto* lane = editable(*preview);
        int nearest = -1; float distance = 100.0f;
        for (std::size_t i = 0; i < lane->points.size(); ++i) {
            const float dx = xFor(lane->points[i].tick) - e.position.x, dy = yFor(lane->points[i].value) - e.position.y;
            const float d = dx * dx + dy * dy;
            if (d < distance) { distance = d; nearest = static_cast<int>(i); }
        }
        if (e.mods.isPopupMenu()) {
            if (nearest >= 0) lane->points.erase(lane->points.begin() + nearest);
            finish("Delete automation point"); return;
        }
        activeTick = nearest >= 0 ? lane->points[static_cast<std::size_t>(nearest)].tick : -1;
        updatePoint(e.position); repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (preview && !e.mods.isPopupMenu()) { updatePoint(e.position); repaint(); } }
    void mouseUp(const juce::MouseEvent&) override { if (preview) finish("Edit automation point"); }
private:
    int selectedTrack() const { return std::max(0, trackBox.getSelectedId() - 1); }
    automation::Parameter selectedParameter() const { return parameterBox.getSelectedId() == 2 ? automation::Parameter::pan : automation::Parameter::volume; }
    const automation::Lane* find(const timeline::Timeline& t) const {
        for (const auto& lane : t.automationLanes) if (lane.track == selectedTrack() && lane.parameter == selectedParameter()) return &lane;
        return nullptr;
    }
    automation::Lane* editable(timeline::Timeline& t) {
        for (auto& lane : t.automationLanes) if (lane.track == selectedTrack() && lane.parameter == selectedParameter()) return &lane;
        t.automationLanes.push_back({selectedTrack(), selectedParameter(), true, {}});
        return &t.automationLanes.back();
    }
    juce::Rectangle<float> plot() const { return getLocalBounds().withTrimmedTop(34).reduced(12, 8).toFloat(); }
    float xFor(std::int64_t tick) const { const auto a = plot(); return a.getX() + a.getWidth() * static_cast<float>(static_cast<double>(tick) / controller.timeline().length()); }
    float yFor(float value) const { const auto a = plot(); const float low = selectedParameter() == automation::Parameter::volume ? 0.0f : -1.0f; return a.getBottom() - (value - low) * 0.5f * a.getHeight(); }
    void updatePoint(juce::Point<float> position) {
        auto* lane = editable(*preview); const auto a = plot();
        if (a.getWidth() <= 0 || a.getHeight() <= 0) return;
        if (activeTick >= 0) std::erase_if(lane->points, [&](const automation::Point& p) { return p.tick == activeTick; });
        const double fraction = std::clamp(static_cast<double>((position.x - a.getX()) / a.getWidth()), 0.0, 1.0);
        activeTick = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(fraction * preview->length() / timeline::ticksPerStep)) * timeline::ticksPerStep, 0, preview->length());
        const float low = selectedParameter() == automation::Parameter::volume ? 0.0f : -1.0f;
        const float value = low + 2.0f * std::clamp((a.getBottom() - position.y) / a.getHeight(), 0.0f, 1.0f);
        lane->put({activeTick, value, curveBox.getSelectedId() == 2 ? automation::Curve::hold : automation::Curve::linear});
    }
    void finish(const juce::String& label) {
        // A load, undo or edit elsewhere during the gesture must not be overwritten.
        if (base && preview && controller.timeline() == *base) {
            const auto result = *preview;
            controller.editTimeline(label, [result](timeline::Timeline& t) { t = result; return true; });
        }
        preview.reset(); base.reset(); activeTick = -1; refresh();
    }
    ProjectController& controller;
    juce::ComboBox trackBox, parameterBox, curveBox;
    juce::TextButton enableButton;
    juce::Label hint;
    std::optional<timeline::Timeline> preview, base;
    std::int64_t activeTick{-1};
};

class TimelineView final : public juce::Component, private juce::ScrollBar::Listener {
public:
    static constexpr int toolbarHeight = 46, scrollBarSize = 12, helpHeight = 22;

    explicit TimelineView(ProjectController& projectController)
        : controller(projectController), canvas(projectController), clipBin(projectController), hBar(false), vBar(true), automationEditor(projectController) {
        namespace col = ui::colours;
        setName("Arrangement");
        addChildComponent(automationEditor);
        addAndMakeVisible(automationButton);
        automationButton.setButtonText("Automation"); automationButton.setClickingTogglesState(true);
        automationButton.setName("Show automation lanes");
        automationButton.onClick = [this] { automationEditor.setVisible(automationButton.getToggleState()); resized(); };
        addAndMakeVisible(canvas); addAndMakeVisible(hBar); addAndMakeVisible(vBar); addAndMakeVisible(clipBin);
        addAndMakeVisible(clipsButton);
        clipsButton.setButtonText("Clips"); clipsButton.setName("Show clip list"); clipsButton.setClickingTogglesState(true);
        clipsButton.setToggleState(true, juce::dontSendNotification);
        clipsButton.setTooltip("Show or hide the list of patterns and clips. Drag a row onto a lane to place it.");
        clipsButton.onClick = [this] { clipBin.setVisible(clipsButton.getToggleState()); resized(); };
        clipBin.onPlace = [this](const clipbin::Item& item) { canvas.placeItemAtCursor(item); };
        hBar.addListener(this); vBar.addListener(this);
        hBar.setAutoHide(false); vBar.setAutoHide(true);
        for (auto* label : {&snapLabel, &lengthLabel}) {
            addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true)); label->setColour(juce::Label::textColourId, col::textFaint);
        }
        snapLabel.setText("SNAP", juce::dontSendNotification); lengthLabel.setText("SONG BARS", juce::dontSendNotification);
        addAndMakeVisible(snapBox); snapBox.setName("Snap"); snapBox.setTooltip("Round dragged clips, edges and the play position to this grid. Hold Alt to ignore it.");
        for (std::size_t i = 0; i < tlui::allSnaps.size(); ++i) snapBox.addItem(tlui::snapName(tlui::allSnaps[i]), static_cast<int>(i) + 1);
        snapBox.setSelectedId(1, juce::dontSendNotification);
        snapBox.onChange = [this] { canvas.setSnap(tlui::allSnaps[static_cast<std::size_t>(snapBox.getSelectedId() - 1)]); };
        for (auto* b : {&zoomOutButton, &zoomInButton, &fitButton, &loopButton, &markerButton, &placeButton, &exportButton}) addAndMakeVisible(b);
        followButton.setButtonText("Follow"); followButton.setName("Follow playhead"); followButton.setToggleState(true, juce::dontSendNotification);
        followButton.setClickingTogglesState(true); followButton.setTooltip("Scroll with the playhead while it plays");
        followButton.onClick = [this] { canvas.setFollow(followButton.getToggleState()); };
        addAndMakeVisible(followButton);
        zoomOutButton.setButtonText("-"); zoomOutButton.setName("Zoom out"); zoomOutButton.setTooltip("Zoom out (Ctrl+wheel, or the - key)");
        zoomOutButton.onClick = [this] { canvas.zoomBy(0.8); };
        zoomInButton.setButtonText("+"); zoomInButton.setName("Zoom in"); zoomInButton.setTooltip("Zoom in (Ctrl+wheel, or the + key)");
        zoomInButton.onClick = [this] { canvas.zoomBy(1.25); };
        fitButton.setButtonText("Fit"); fitButton.setName("Fit song"); fitButton.setTooltip("Zoom to show the whole song (Ctrl+0)");
        fitButton.onClick = [this] { canvas.zoomToFit(); };
        loopButton.setButtonText("Loop"); loopButton.setName("Loop"); loopButton.setClickingTogglesState(false);
        loopButton.setTooltip("Repeat the loop range while playing (L). Drag in the strip above the bars to set the range.");
        loopButton.onClick = [this] { canvas.toggleLoop(); };
        markerButton.setButtonText("+ Marker"); markerButton.setName("Add marker"); markerButton.setTooltip("Drop a marker at the play position (K)");
        markerButton.onClick = [this] { canvas.addMarkerAtCursor(); };
        placeButton.setButtonText("Place..."); placeButton.setName("Place clip"); placeButton.setTooltip("Put a clip or pattern at the play position, on the selected row");
        placeButton.onClick = [this] { canvas.showPlaceMenu(canvas.focusRow(), canvas.cursorTick()); };
        exportButton.setButtonText("Export WAV"); exportButton.setName("Export WAV"); exportButton.setTooltip("Render the song, or just the loop range, to a WAV file");
        exportButton.onClick = [this] { if (exporting) { if (onExport) onExport(0, -1); } else showExportMenu(); };
        addAndMakeVisible(lengthSlider);
        lengthSlider.setName("Song length"); lengthSlider.setSliderStyle(juce::Slider::IncDecButtons);
        lengthSlider.setRange(1, timeline::maxBars, 1); lengthSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 24);
        lengthSlider.setTooltip("How many bars the song lasts. It cannot be shorter than the last clip.");
        lengthSlider.onValueChange = [this] {
            if (updating) return;
            const int bars = static_cast<int>(lengthSlider.getValue());
            controller.editTimeline("Change song length", [bars](timeline::Timeline& t) { return t.setLengthBars(bars); }, "timeline-length");
            refresh();
        };
        addAndMakeVisible(readout); addAndMakeVisible(help);
        readout.setFont(ui::monoFont(11.5f)); readout.setColour(juce::Label::textColourId, col::textDim);
        readout.setJustificationType(juce::Justification::centredRight);
        help.setFont(ui::font(12.0f)); help.setColour(juce::Label::textColourId, col::textFaint);
        help.setJustificationType(juce::Justification::centredLeft);
        defaultHelp = "Double-click a lane to place a clip. Drag to move, drag an edge to resize, drag empty space to select. S splits, M mutes, L loops, K adds a marker, Ctrl+wheel zooms.";
        help.setText(defaultHelp, juce::dontSendNotification);
        canvas.onViewChanged = [this] { layoutScroll(); };
        canvas.onStateChanged = [this] { updateControls(); };
        canvas.onMessage = [this](const juce::String& text) { setStatus(text); };
        refresh();
    }
    ~TimelineView() override { hBar.removeListener(this); vBar.removeListener(this); }

    TimelineCanvas& timelineCanvas() noexcept { return canvas; }
    ClipBin& clipList() noexcept { return clipBin; }
    bool clipListShown() const noexcept { return clipBin.isVisible(); }
    std::function<void(std::int64_t from, std::int64_t to)> onExport;   // to == -1: through the end of the song; while exporting, the button asks to cancel with (0, -1)
    std::function<void()> onTitleChanged;                                // the subtitle (length, clips, loop) changed
    std::function<void(juce::PopupMenu, std::function<void(int)>)> menuLauncher;   // lets the tests answer for the user

    void setStatus(const juce::String& text) { help.setText(text.isEmpty() ? defaultHelp : text, juce::dontSendNotification); help.setTooltip(text); }
    void setPlayhead(std::int64_t tick) { canvas.setPlayhead(tick); }
    // While a render is running the Export button becomes a cancel button.
    void setExporting(bool on) {
        exporting = on;
        exportButton.setButtonText(on ? "Cancel export" : "Export WAV");
        exportButton.setTooltip(on ? "Stop rendering" : "Render the song, or just the loop range, to a WAV file");
    }

    juce::String subtitle() const {
        const auto& t = controller.timeline();
        const auto dot = juce::String(juce::CharPointer_UTF8("  \xc2\xb7  "));
        return juce::String(t.lengthBars) + " bars" + dot + juce::String(static_cast<int>(t.regions.size())) + " clips" + dot
             + (t.loopEnabled ? "loop on" : "loop off");
    }

    // Re-read the document (after undo, open, new project, or an edit made elsewhere).
    void refresh() {
        const auto& t = controller.timeline();
        updating = true;
        lengthSlider.setValue(t.lengthBars, juce::dontSendNotification);
        updating = false;
        canvas.refresh();
        clipBin.refresh();
        automationEditor.refresh();
        updateControls();
        layoutScroll();
    }
    // The clip list reads the project, which may have changed while another view was showing.
    void visibilityChanged() override { if (isVisible()) clipBin.refresh(); }

    void paint(juce::Graphics& g) override { g.fillAll(ui::colours::surface); }
    void resized() override {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop(toolbarHeight).reduced(14, 6);
        auto column = [&](juce::Label& label, int width) {
            auto c = bar.removeFromLeft(width);
            label.setBounds(c.removeFromTop(14));
            return c;
        };
        snapBox.setBounds(column(snapLabel, 168)); bar.removeFromLeft(12);
        auto zoom = bar.removeFromLeft(142).withTrimmedTop(14);
        zoomOutButton.setBounds(zoom.removeFromLeft(34)); zoom.removeFromLeft(4);
        zoomInButton.setBounds(zoom.removeFromLeft(34)); zoom.removeFromLeft(4);
        fitButton.setBounds(zoom.removeFromLeft(54)); bar.removeFromLeft(4);
        followButton.setBounds(bar.removeFromLeft(74).withTrimmedTop(14)); bar.removeFromLeft(8);
        loopButton.setBounds(bar.removeFromLeft(64).withTrimmedTop(14)); bar.removeFromLeft(8);
        markerButton.setBounds(bar.removeFromLeft(92).withTrimmedTop(14)); bar.removeFromLeft(8);
        placeButton.setBounds(bar.removeFromLeft(88).withTrimmedTop(14)); bar.removeFromLeft(14);
        lengthSlider.setBounds(column(lengthLabel, 120));
        exportButton.setBounds(bar.removeFromRight(110).withTrimmedTop(14)); bar.removeFromRight(10);
        readout.setBounds(bar.withTrimmedTop(14));
        auto helpRow = r.removeFromBottom(helpHeight).reduced(14, 0);
        automationButton.setBounds(helpRow.removeFromLeft(110)); helpRow.removeFromLeft(8);
        clipsButton.setBounds(helpRow.removeFromRight(70)); helpRow.removeFromRight(8);
        help.setBounds(helpRow);
        if (automationEditor.isVisible()) automationEditor.setBounds(r.removeFromBottom(std::min(180, std::max(80, r.getHeight() / 3))));
        if (clipBin.isVisible()) clipBin.setBounds(r.removeFromRight(ClipBin::width));
        auto bottom = r.removeFromBottom(scrollBarSize);
        hBar.setBounds(bottom.withTrimmedLeft(TimelineCanvas::headerWidth).withTrimmedRight(scrollBarSize));
        vBar.setBounds(r.removeFromRight(scrollBarSize).withTrimmedTop(TimelineCanvas::topHeight));
        canvas.setBounds(r);
        layoutScroll();
    }

private:
    enum ExportId { exportSong = 1, exportLoop };
    void showExportMenu() {
        const auto& t = controller.timeline();
        juce::PopupMenu menu;
        menu.addItem(exportSong, "Whole song (" + juce::String(t.lengthBars) + " bars)", t.hasRegions());
        menu.addItem(exportLoop, "Loop range (bars " + juce::String(t.loopStart / timeline::ticksPerBar + 1) + " to "
                     + juce::String((t.loopEnd + timeline::ticksPerBar - 1) / timeline::ticksPerBar) + ")", t.hasRegions());
        juce::Component::SafePointer<TimelineView> safe(this);
        std::function<void(int)> done = [safe](int id) {
            if (safe == nullptr || id == 0 || !safe->onExport) return;
            const auto& tl = safe->controller.timeline();
            if (id == exportSong) safe->onExport(0, -1); else if (id == exportLoop) safe->onExport(tl.loopStart, tl.loopEnd);
        };
        if (menuLauncher) { menuLauncher(std::move(menu), std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton), std::move(done));
    }
    void updateControls() {
        const auto& t = controller.timeline();
        loopButton.setToggleState(t.loopEnabled, juce::dontSendNotification);
        loopButton.setColour(juce::TextButton::buttonColourId, t.loopEnabled ? ui::colours::accent.withAlpha(0.35f) : ui::colours::control);
        const auto tick = canvas.cursorTick();
        const double seconds = timeline::secondsForTicks(static_cast<double>(tick), controller.document().bpm);
        const int minutes = static_cast<int>(seconds / 60.0);
        juce::String text = "Bar " + juce::String(tick / timeline::ticksPerBar + 1) + "." + juce::String((tick % timeline::ticksPerBar) / timeline::ticksPerBeat + 1)
            + "  " + juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 2).paddedLeft('0', 5);
        const auto count = canvas.selection().size();
        if (count > 0) text += "   " + juce::String(static_cast<int>(count)) + (count == 1 ? " clip selected" : " clips selected");
        readout.setText(text, juce::dontSendNotification);
        lengthSlider.setValue(t.lengthBars, juce::dontSendNotification);
        repaint();
        if (onTitleChanged) onTitleChanged();
    }
    void layoutScroll() {
        if (canvas.getWidth() <= 0) return;
        const double total = static_cast<double>(canvas.contentTicks()), visible = canvas.visibleTicks();
        hBar.setRangeLimits(0.0, std::max(total, visible));
        hBar.setCurrentRange(canvas.scrollTicks(), visible);
        hBar.setSingleStepSize(visible * 0.1);
        vBar.setRangeLimits(0.0, static_cast<double>(std::max(canvas.contentHeight(), canvas.laneAreaHeight())));
        vBar.setCurrentRange(canvas.scrollY(), canvas.laneAreaHeight());
    }
    void scrollBarMoved(juce::ScrollBar* bar, double start) override {
        if (bar == &vBar) canvas.setScrollY(static_cast<int>(start));
        else canvas.setScrollTicks(start);
    }

    ProjectController& controller;
    TimelineCanvas canvas;
    ClipBin clipBin;
    juce::ScrollBar hBar, vBar;
    TrackAutomationEditor automationEditor;
    juce::TextButton automationButton, clipsButton;
    juce::Label snapLabel, lengthLabel, readout, help;
    juce::ComboBox snapBox;
    juce::TextButton zoomOutButton, zoomInButton, fitButton, loopButton, markerButton, placeButton, exportButton;
    juce::ToggleButton followButton;
    juce::Slider lengthSlider;
    juce::String defaultHelp;
    bool updating{}, exporting{};
};

} // namespace alien
