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
#include <map>
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
// Removes a recording take from the take list, as one undo step. The comp and the audio file stay.
inline bool forgetTake(ProjectController& controller, std::uint32_t id) {
    if (controller.isBusy() || controller.isRecording()) return false;
    return controller.editTimeline("Forget recording take", [=](timeline::Timeline& t) {
        return std::erase_if(t.takes, [=](const auto& take) { return take.id == id; }) != 0;
    });
}
} // namespace tlui

class TimelineCanvas final : public juce::Component, public juce::DragAndDropTarget {
public:
    static constexpr int headerWidth = 156, markerStripHeight = 20, loopStripHeight = 16, rulerHeight = 24;
    static constexpr int topHeight = markerStripHeight + loopStripHeight + rulerHeight;
    static constexpr int rowHeight = 46, edgeGrab = 6, dragThreshold = 3;
    static constexpr int takeLaneHeight = 30;   // one recording take under its track, when the track shows its take lanes
    static constexpr float fadeHandleHeight = 10.0f;   // the top strip of an audio clip where its fade handles are grabbed
    static constexpr double minPixelsPerTick = 0.00001, maxPixelsPerTick = 0.6, defaultPixelsPerTick = 64.0 / 3840.0;

    enum class Zone { none, corner, header, markerStrip, loopStrip, ruler, region, lane, take };
    enum class Edge { none, left, right };
    struct Hit {
        Zone zone{Zone::none};
        int displayRow{-1};
        std::uint32_t region{0}, marker{0};
        std::int64_t tempo{-1};   // the tick of the tempo change under the pointer on the ruler
        int meterBar{-1};         // the bar of the time signature change under the pointer on the ruler
        Edge edge{Edge::none};
        Edge fade{Edge::none};   // a fade handle at the top of an audio clip: left is the fade in (or crossfade), right the fade out
        enum class LoopPart { none, start, end, body } loopPart{LoopPart::none};
        std::uint32_t take{0};    // a take lane under a track (its label in the header, or the lane itself)
        int takeLane{-1};         // which of the track's take lanes, from 0
    };

    explicit TimelineCanvas(ProjectController& projectController) : controller(projectController) {
        setName("Arrangement timeline");
        setHelpText("Alt-drag an audio clip's body to slip its source without moving its boundaries. "
                    "Drag right to reveal later source audio; drag left to reveal earlier audio. "
                    "Slip is unsnapped. Edges trim and top-corner handles edit fades.");
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);
    }

    // ---- what the owner hears about ---------------------------------------------------------------
    std::function<void(std::int64_t)> onSeek;                  // the play position was set (a click on the ruler, a marker...)
    std::function<void(int)> onFreezeTrack, onUnfreezeTrack;
    std::function<void()> onPlayToggle;                        // Space
    std::function<void(int row, int source)> onOpenSource;     // double-click on a region: edit what it plays
    std::function<void(std::uint64_t clip)> onOpenPlacement;    // ... or, for a placement clip, its notes
    std::function<void(int track, int scene, bool sceneAdded)> onClipCreated;   // a new empty clip was made for a track (a scene is added when none was free)
    std::function<void(const juce::String&)> onMessage;
    std::function<void()> onViewChanged;                       // zoom or scroll changed
    std::function<void()> onStateChanged;                      // selection, cursor, loop or length changed
    std::function<void()> onTakesChanged;                      // a take was comped or forgotten from its lane
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
    int contentHeight() const noexcept { return displayRowCount() * rowHeight + takeLanesAbove(displayRowCount()); }
    int maxScrollY() const noexcept { return std::max(0, contentHeight() - laneAreaHeight()); }
    double xForTick(double tick) const noexcept { return headerWidth + (tick - scrollX) * ppt; }
    double tickAtX(double x) const noexcept { return scrollX + (x - headerWidth) / ppt; }
    // A row is `rowHeight` tall, plus its take lanes under it when its track shows them.
    int rowTop(int displayRow) const noexcept { return topHeight + displayRow * rowHeight + takeLanesAbove(displayRow) - scrollYpx; }
    int blockHeight(int displayRow) const noexcept { return rowHeight + takeLaneCount(timelineRow(displayRow)) * takeLaneHeight; }
    // The row whose block (the row and its take lanes) is at `y`.
    int displayRowAt(int y) const noexcept {
        if (y < topHeight) return -1;
        const int at = y - topHeight + scrollYpx;
        if (laneTracks.empty()) {
            const int row = at / rowHeight;
            return row >= 0 && row < displayRowCount() ? row : -1;
        }
        for (int d = 0, top = 0; d < displayRowCount(); ++d) {
            top += blockHeight(d);
            if (at < top) return d;
        }
        return -1;
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
    // A tempo change is a flag on the lower half of the ruler, labelled with its tempo.
    static juce::String tempoLabel(double bpm) { return juce::String(bpm, std::abs(bpm - std::round(bpm)) < 0.005 ? 0 : 2) + " BPM"; }
    juce::Rectangle<float> tempoBounds(const timeline::TempoChange& c) const {
        const float x = static_cast<float>(xForTick(static_cast<double>(c.tick)));
        const float width = 10.0f + 5.6f * static_cast<float>(tempoLabel(c.bpm).length());
        return {x, static_cast<float>(markerStripHeight + loopStripHeight + 12), width, static_cast<float>(rulerHeight - 13)};
    }
    juce::Rectangle<float> loopBounds() const noexcept {
        const auto& t = shown();
        const float x0 = static_cast<float>(xForTick(static_cast<double>(t.loopStart))), x1 = static_cast<float>(xForTick(static_cast<double>(t.loopEnd)));
        return {x0, static_cast<float>(markerStripHeight), std::max(1.0f, x1 - x0), static_cast<float>(loopStripHeight)};
    }

    // ---- take lanes -----------------------------------------------------------------------------------------------------
    // A track can show its recording takes (every loop pass is one) in lanes under its row, in the order they were
    // recorded. Which tracks show them is a view setting of this window: it is not saved and is not an undo step.
    std::vector<const timeline::Region*> takesOf(int track) const {
        std::vector<const timeline::Region*> list;
        if (timeline::isPatternRow(track)) return list;
        for (const auto& take : committed().takes) if (take.row == track) list.push_back(&take);
        return list;
    }
    const timeline::Region* takeById(std::uint32_t id) const noexcept {
        for (const auto& take : committed().takes) if (take.id == id) return &take;
        return nullptr;
    }
    bool takeLanesShown(int track) const noexcept {
        const auto& d = controller.document();
        if (laneScope != d.identityScope) return false;
        if (track < 0 || track >= d.trackCount()) return false;
        return std::find(laneTracks.begin(), laneTracks.end(), d.trackId(TrackRole::session, track)) != laneTracks.end();
    }
    // How many take lanes are open under a row: none unless its track shows them and has takes.
    int takeLaneCount(int row) const noexcept {
        if (laneTracks.empty() || !takeLanesShown(row)) return 0;
        int count = 0;
        for (const auto& take : committed().takes) count += take.row == row ? 1 : 0;
        return count;
    }
    // The height of the take lanes open above a row.
    int takeLanesAbove(int displayRow) const noexcept {
        if (laneTracks.empty()) return 0;
        int lanes = 0;
        for (int t = 0, tracks = std::min(displayRow, trackCount()); t < tracks; ++t) lanes += takeLaneCount(t);
        return lanes * takeLaneHeight;
    }
    bool setTakeLanesShown(int track, bool show) {
        const auto& d = controller.document();
        if (track < 0 || track >= d.trackCount() || takeLanesShown(track) == show) return false;
        if (laneScope != d.identityScope) { laneTracks.clear(); laneScope = d.identityScope; }
        const auto id = d.trackId(TrackRole::session, track);
        if (show) laneTracks.push_back(id); else std::erase(laneTracks, id);
        scrollYpx = std::clamp(scrollYpx, 0, maxScrollY());
        repaint(); notifyView();
        return true;
    }
    bool toggleTakeLanes(int track) {
        if (!takeLanesShown(track) && takesOf(track).empty()) { say("This track has no recording takes yet."); return false; }
        return setTakeLanesShown(track, !takeLanesShown(track));
    }
    // Lane `lane` under a track, across the header and the lanes.
    juce::Rectangle<int> takeLaneBounds(int track, int lane) const noexcept {
        return {0, rowTop(displayRowOf(track)) + rowHeight + lane * takeLaneHeight, getWidth(), takeLaneHeight};
    }
    // Where a take is drawn in its lane: at its song position, like a clip.
    juce::Rectangle<float> takeBounds(std::uint32_t id) const {
        const auto* take = takeById(id);
        if (take == nullptr) return {};
        const auto list = takesOf(take->row);
        const auto lane = static_cast<int>(std::find(list.begin(), list.end(), take) - list.begin());
        const float x0 = static_cast<float>(xForTick(static_cast<double>(take->start))), x1 = static_cast<float>(xForTick(static_cast<double>(take->end())));
        return {x0, static_cast<float>(takeLaneBounds(take->row, lane).getY() + 2), std::max(1.0f, x1 - x0), static_cast<float>(takeLaneHeight - 4)};
    }
    // The small button in a track's header that shows and hides its take lanes (drawn when it has takes).
    juce::Rectangle<int> takeLanesButtonRect(int displayRow) const { return {headerWidth - 28, rowTop(displayRow) + 28, 20, 11}; }
    std::uint32_t selectedTakeId() const noexcept { return selectedTake; }
    void selectTake(std::uint32_t id) { if (id == selectedTake) return; selectedTake = takeById(id) != nullptr ? id : 0; repaint(); }
    // Whether a take can be comped into the arrangement: a plain pass of an audio clip.
    bool canComp(const timeline::Region& take) const {
        const auto& d = controller.document();
        return !take.reversed && !take.loopSource && d.isValidCell(take.row, take.source) && d.clip(take.row, take.source).isAudioClip()
            && !controller.isBusy() && !controller.isRecording();
    }
    // Plays `from`..`to` of a take on its track (one undo step through the controller); the take itself is kept.
    bool compFromTake(std::uint32_t id, std::int64_t from, std::int64_t to) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        const bool done = controller.compRecordedTake(id, from, to);
        if (done) {
            afterEdit();
            say("Comped bar " + barBeat(from) + " to " + barBeat(to) + " from the take. The take is kept; Undo restores the previous comp.");
            if (onTakesChanged) onTakesChanged();
        }
        return done;
    }
    bool playWholeTake(std::uint32_t id) {
        const auto* take = takeById(id);
        return take != nullptr && compFromTake(id, take->start, take->end());
    }
    bool forgetTake(std::uint32_t id) {
        const bool done = tlui::forgetTake(controller, id);
        if (done) {
            if (selectedTake == id) selectedTake = 0;
            scrollYpx = std::clamp(scrollYpx, 0, maxScrollY());
            afterEdit();
            if (onTakesChanged) onTakesChanged();
        }
        return done;
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
        // A grid of a bar or more snaps to the song's bar lines; a finer one counts from the start of the
        // bar the tick is in, with the beat and the half bar of that bar's time signature.
        const auto map = shown().meterMap();
        const double at = std::max(0.0, tick);
        const auto bar = map.barAt(at);
        const auto grid = gridTicks();
        std::int64_t snapped;
        if (grid >= timeline::ticksPerBar) {
            const auto every = std::max<std::int64_t>(1, grid / timeline::ticksPerBar);
            const auto nearest = bar.index + (at - static_cast<double>(bar.start) >= static_cast<double>(bar.meter.barTicks()) / 2.0 ? 1 : 0);
            snapped = map.tickOfBar(static_cast<std::int64_t>(std::llround(static_cast<double>(nearest) / static_cast<double>(every))) * every);
        } else {
            const auto unit = snapMode == tlui::Snap::beat ? bar.meter.beatTicks() : snapMode == tlui::Snap::halfBar ? bar.meter.barTicks() / 2 : grid;
            snapped = bar.start + static_cast<std::int64_t>(std::llround((at - static_cast<double>(bar.start)) / static_cast<double>(unit))) * unit;
            snapped = std::min(snapped, bar.start + bar.meter.barTicks());
        }
        return std::clamp<std::int64_t>(snapped, 0, timeline::maxLength);
    }
    // "5.3": the bar and beat (of that bar's time signature) a tick falls in, counting from 1.
    juce::String barBeat(std::int64_t tick) const {
        const auto bar = shown().meterMap().barAt(static_cast<double>(std::max<std::int64_t>(0, tick)));
        return juce::String(bar.index + 1) + "." + juce::String((tick - bar.start) / bar.meter.beatTicks() + 1);
    }
    static juce::String meterText(const timeline::Meter& m) { return juce::String(m.numerator) + "/" + juce::String(m.denominator); }
    // A time signature change is a label just after its bar's number on the ruler.
    juce::Rectangle<float> meterBounds(const timeline::MeterChange& c) const {
        const float x = static_cast<float>(xForTick(static_cast<double>(shown().tickOfBar(c.bar))));
        const float numberWidth = 6.4f * static_cast<float>(juce::String(c.bar + 1).length());
        const float width = 8.0f + 6.0f * static_cast<float>(meterText({c.numerator, c.denominator}).length());
        return {x + 6.0f + numberWidth, static_cast<float>(markerStripHeight + loopStripHeight + 1), width, 11.0f};
    }
    // Calls line(tick, kind, bar, beat) for the grid lines from `from` to `to`: kind 2 is a bar line, 1 a
    // beat, 0 a finer line `fine` ticks apart. Bars and beats follow the song's time signatures; when a bar
    // is finer than `fine`, only every few bars get a line.
    template <class Line>
    void forEachGridLine(const timeline::Timeline& t, double from, double to, std::int64_t fine, Line&& line) const {
        const auto map = t.meterMap();
        auto bar = map.barAt(std::max(0.0, from));
        const auto every = std::max<std::int64_t>(1, fine / timeline::ticksPerBar);
        if (every > 1) { const auto first = bar.index / every * every; bar = {first, map.tickOfBar(first), map.meterAtBar(first)}; }
        for (int guard = 0; guard < 200000 && static_cast<double>(bar.start) <= to; ++guard) {
            const auto barTicks = bar.meter.barTicks(), beatTicks = bar.meter.beatTicks();
            line(bar.start, 2, bar.index, std::int64_t{0});
            if (every == 1 && fine < barTicks)
                for (std::int64_t offset = fine; offset < barTicks; offset += fine)
                    line(bar.start + offset, offset % beatTicks == 0 ? 1 : 0, bar.index, offset / beatTicks);
            // The next bar (or the next one with a line), never stepping over a time signature change.
            std::int64_t nextChange = std::numeric_limits<std::int64_t>::max();
            for (const auto& c : t.meterChanges) if (c.bar > bar.index) { nextChange = c.bar; break; }
            const auto step = std::min<std::int64_t>(every, nextChange - bar.index);
            const auto index = bar.index + step;
            bar = {index, bar.start + step * barTicks, index >= nextChange ? map.meterAtBar(index) : bar.meter};
        }
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
        const int d = displayRowOf(row), top = rowTop(d) - topHeight + scrollYpx, bottom = top + blockHeight(d);
        if (top < scrollYpx) setScrollY(top); else if (bottom > scrollYpx + laneAreaHeight()) setScrollY(std::min(top, bottom - laneAreaHeight()));
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
        if (takeById(selectedTake) == nullptr) selectedTake = 0;
        if (laneScope != controller.document().identityScope) { laneTracks.clear(); laneScope = controller.document().identityScope; }
        std::erase_if(laneTracks, [&](TrackId id) { return controller.document().trackSlot(id, TrackRole::session) < 0; });
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
        // Remembered before the edit: cutting the last region of a placement clip drops the clip.
        const auto before = std::make_pair(clipboard(), clipboardOrigin());
        remember(committed().copy(ids));
        const bool done = edit(ids.size() == 1 ? "Cut clip" : "Cut clips", [&](timeline::Timeline& t) { return t.remove(ids); });
        if (done) { selected.clear(); stateChanged(); }
        else { clipboard() = before.first; clipboardOrigin() = before.second; }
        return done;
    }
    // Pastes at the play position (or `at`); rack patterns land on the focused lane.
    bool paste(std::optional<std::int64_t> at = std::nullopt) {
        auto clip = pastable();
        if (clip.empty()) { say("Nothing to paste."); return false; }
        // A pasted placement clip shares the copied one while that still holds what was copied; otherwise
        // (it was cut, or edited since) the paste brings back a clip with the copied notes.
        std::map<std::uint64_t, std::uint64_t> restored;
        for (auto& r : clip.regions) {
            if (!r.ownsClip()) continue;
            const auto& copied = clipboardOrigin().placements[r.clip];
            const auto* now = controller.document().library.clip(library::ClipId{r.clip});
            if (now != nullptr && controller.document().contentOf(library::ClipId{r.clip}).pattern == copied.first) continue;
            auto& id = restored[r.clip];
            if (id == 0) id = controller.restorePlacementClip(copied.first, copied.second);
            if (id == 0) { say("Nothing to paste."); return false; }
            r.clip = id;
        }
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
    // Crosses two selected touching clips, or one selected clip with the clip that ends where it starts.
    // The menu id for giving one fade a shape.
    static int fadeShapeCommand(timeline::FadeEdge edge, timeline::FadeCurve curve) noexcept {
        return cmdFadeShapeBase + static_cast<int>(edge) * 10 + static_cast<int>(curve);
    }
    // Gives the chosen fade of every selected audio clip a shape (one undo step).
    bool setFadeShapeSelection(timeline::FadeEdge edge, timeline::FadeCurve curve) {
        timeline::Ids ids;
        for (const auto id : selected) if (const auto* r = committed().find(id); r != nullptr && isAudioRegion(*r)) ids.push_back(id);
        if (ids.empty()) { say("Select audio clips to shape their fades."); return false; }
        const bool done = edit("Fade shape", [=](timeline::Timeline& t) { return t.setFadeCurve(ids, edge, curve); });
        if (done) say(juce::String(edge == timeline::FadeEdge::in ? "Fade in" : edge == timeline::FadeEdge::out ? "Fade out" : "Crossfade")
                      + ": " + timeline::fadeCurveLabel(curve) + ".");
        return done;
    }
    bool crossfadeSelection() {
        const timeline::Region* a = nullptr; const timeline::Region* b = nullptr;
        if (selected.size() == 1) { b = committed().find(selected[0]); a = committed().crossfadePartner(selected[0]); }
        else if (selected.size() == 2) {
            a = committed().find(selected[0]); b = committed().find(selected[1]);
            if (a != nullptr && b != nullptr && b->start < a->start) std::swap(a, b);
        }
        if (a == nullptr || b == nullptr) { say("Select two touching clips on the same track to crossfade."); return false; }
        const auto left = a->id, right = b->id;
        const bool done = edit("Crossfade clips", [=](timeline::Timeline& t) { return t.crossfade(left, right, timeline::ticksPerBeat); });
        if (!done) say("Crossfade needs two touching audio clips on the same track."); return done;
    }
    bool removeFadesSelection() {
        if (selected.empty()) return false; const auto ids = selected;
        return edit("Remove fades", [=](timeline::Timeline& t) { return t.clearFades(ids); });
    }
    bool selectionHasFades() const {
        for (const auto id : selected) if (const auto* r = committed().find(id); r != nullptr && (r->fadeIn > 0 || r->fadeOut > 0 || r->crossfade > 0)) return true;
        return false;
    }
    bool canCrossfadeSelection() const {
        if (selected.size() == 1) return committed().crossfadePartner(selected[0]) != nullptr;
        if (selected.size() != 2) return false;
        const auto* a = committed().find(selected[0]); const auto* b = committed().find(selected[1]);
        return a != nullptr && b != nullptr && a->row == b->row && !a->isPattern() && (a->end() == b->start || b->end() == a->start);
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
    // Tempo changes go through the controller's tempo edits, which unfreeze frozen tracks first.
    bool tempoEdit(const juce::String& label, const std::function<bool(timeline::Timeline&)>& change) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        if (controller.isRecording()) { say("Stop recording before changing the tempo."); return false; }
        const bool done = controller.editTempoMap(label, change);
        if (done) afterEdit();
        return done;
    }
    // Asks for the tempo from `tick` on. Tick 0 is the project tempo, which the transport sets.
    void askTempo(std::int64_t tick) {
        if (tick <= 0) { say("The song starts at the project tempo; set it in the transport, or add a change later in the song."); return; }
        if (tick > committed().length()) { say("A tempo change must be inside the song."); return; }
        const double now = controller.songTempo().tempoAt(static_cast<double>(tick));
        const juce::String where = "Bar " + barBeat(tick);
        askForName("Tempo from " + where + " (60-200 BPM)", juce::String(now, std::abs(now - std::round(now)) < 0.005 ? 0 : 2),
            [this, tick](const juce::String& text) {
                const auto value = text.trim().upToFirstOccurrenceOf(" ", false, true);
                const double bpm = value.getDoubleValue();
                if (value.isEmpty() || !value.containsOnly("0123456789.") || !timeline::validTempo(bpm)) {
                    say("Enter a tempo from 60 to 200 BPM.");
                    return;
                }
                if (!tempoEdit("Tempo change", [=](timeline::Timeline& t) { return t.setTempoChange(tick, bpm); }))
                    say("The tempo there is already " + tempoLabel(bpm) + ".");
            });
    }
    // Edits what a region plays: its placement clip, or its cell or rack pattern.
    void openRegion(const timeline::Region& r) {
        if (r.ownsClip()) { if (onOpenPlacement) onOpenPlacement(r.clip); }
        else if (onOpenSource) onOpenSource(r.row, r.source);
    }
    // Gives one placement its own copy of what it plays (see ProjectController::makePlacementUnique).
    bool makePlacementUnique(std::uint32_t id) {
        if (controller.isBusy()) { say("Wait for the save, import or export to finish before changing the arrangement."); return false; }
        const int scenesBefore = controller.document().sceneCount();
        if (!controller.makePlacementUnique(id)) { say("That placement already plays content of its own."); return false; }
        afterEdit();
        say("This placement now has its own copy: edit it without changing the others.");
        if (const auto* r = committed().find(id); r != nullptr && r->playsCell() && onClipCreated)   // an audio placement took a cell
            onClipCreated(r->row, r->source, controller.document().sceneCount() != scenesBefore);
        return true;
    }
    bool removeTempoChange(std::int64_t tick) {
        return tempoEdit("Remove tempo change", [=](timeline::Timeline& t) { return t.removeTempoChange(tick); });
    }
    // Asks for the time signature from bar `bar` (counting from 0) on, as "3/4" or "6/8".
    void askMeter(int bar) {
        if (bar < 0 || bar >= committed().lengthBars) { say("A time signature change must be inside the song."); return; }
        const auto now = committed().meterMap().meterAtBar(bar);
        askForName("Time signature from bar " + juce::String(bar + 1) + " (for example 3/4 or 6/8)", meterText(now),
            [this, bar](const juce::String& text) {
                const auto trimmed = text.trim();
                const auto top = trimmed.upToFirstOccurrenceOf("/", false, false).trim(), bottom = trimmed.fromFirstOccurrenceOf("/", false, false).trim();
                const int numerator = top.getIntValue(), denominator = bottom.getIntValue();
                if (!trimmed.contains("/") || top.isEmpty() || bottom.isEmpty() || !top.containsOnly("0123456789") || !bottom.containsOnly("0123456789")
                    || !timeline::validMeter(numerator, denominator)) {
                    say("Enter a time signature such as 3/4 or 6/8: 1 to " + juce::String(timeline::maxMeterNumerator) + " beats of a 2, 4, 8 or 16.");
                    return;
                }
                if (!edit("Time signature change", [=](timeline::Timeline& t) { return t.setMeterChange(bar, numerator, denominator); }))
                    say("Bar " + juce::String(bar + 1) + " is already in " + meterText({numerator, denominator}) + ".");
            });
    }
    bool removeMeterChange(int bar) {
        return edit("Remove time signature change", [=](timeline::Timeline& t) { return t.removeMeterChange(bar); });
    }
    // Arrow keys: moves the selection by the grid (or a bar), or the play position when nothing is selected.
    bool nudge(int direction, bool bar = false) {
        const auto from = selected.empty() ? cursor : [&] { const auto* r = committed().find(selected.front()); return r != nullptr ? r->start : cursor; }();
        const auto step = (bar ? committed().meterMap().barAt(static_cast<double>(from)).meter.barTicks() : gridTicks()) * direction;
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
        plan.length = controller.naturalLength(plan.row, plan.source, plan.tick);
        plan.valid = plan.length > 0;
        if (plan.valid) {
            timeline::Region named; named.row = plan.row; named.source = plan.source;
            const auto at = barBeat(plan.tick);
            plan.message = "Drop to put " + regionName(named) + " on " + laneName(plan.row) + " at bar " + at.upToFirstOccurrenceOf(".", false, false)
                         + ", beat " + at.fromFirstOccurrenceOf(".", false, false) + ".";
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
        if (place(plan.row, plan.source, plan.tick)) say("Placed on " + laneName(plan.row) + " at bar " + barBeat(plan.tick).upToFirstOccurrenceOf(".", false, false) + ".");
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
                   cmdRename, cmdRemoveMarker, cmdJump, cmdLoopSelection, cmdRemoveFades, cmdTempo, cmdRemoveTempo, cmdMeter, cmdRemoveMeter, cmdMakeUnique, cmdColourBase = 100, cmdFadeShapeBase = 500,
                   cmdArm = 200, cmdFreeze = 201, cmdUnfreeze = 202, cmdTakeLanes = 203, cmdPlayTake = 204, cmdPlayTakeLoop = 205, cmdForgetTake = 206,
                   cmdMonitorBase = 210, cmdInputMonoBase = 300, cmdInputStereoBase = 400 };
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
    struct ClipboardOrigin {
        std::uint32_t scope{};
        std::vector<TrackId> tracks;
        std::vector<std::uint32_t> scenes;
        // Placement clips as they were copied, so a paste can bring back one that was cut (and so is gone).
        std::map<std::uint64_t, std::pair<std::shared_ptr<const PianoRollPattern>, juce::String>> placements;
    };
    static ClipboardOrigin& clipboardOrigin() { static ClipboardOrigin origin; return origin; }
    void remember(timeline::Clipboard clip) {
        clipboard() = std::move(clip);
        auto& origin = clipboardOrigin();
        origin = {};
        origin.scope = controller.document().identityScope;
        for (int t = 0; t < controller.document().trackCount(); ++t) origin.tracks.push_back(controller.document().trackId(TrackRole::session, t));
        for (const auto& scene : controller.document().scenes) origin.scenes.push_back(scene.id);
        const auto& d = controller.document();
        for (const auto& r : clipboard().regions)
            if (r.ownsClip()) origin.placements[r.clip] = {d.contentOf(library::ClipId{r.clip}).pattern, d.placementName(r.clip)};
    }
    timeline::Clipboard pastable() const {
        const auto& origin = clipboardOrigin();
        const auto& d = controller.document();
        timeline::Clipboard clip;
        if (origin.scope != d.identityScope) return clip;
        for (auto r : clipboard().regions) {
            if (r.isPattern()) {
                if (r.rackClip == 0) continue; // Clipboard content always comes from an identity-bound document.
                const bool sameDocument = origin.scope == d.identityScope;
                const int source = controller.rackPatternForClip(r.rackClip);
                if (!sameDocument || source < 0) continue;
                r.source = source;
            } else if (r.ownsClip()) {
                if (r.row < 0 || r.row >= static_cast<int>(origin.tracks.size()) || origin.placements.count(r.clip) == 0) continue;
                const int track = d.trackSlot(origin.tracks[static_cast<std::size_t>(r.row)], TrackRole::session);
                if (track < 0) continue;
                r.row = track;
            } else {
                if (r.row < 0 || r.row >= static_cast<int>(origin.tracks.size()) || r.source < 0 || r.source >= static_cast<int>(origin.scenes.size())) continue;
                const int track = d.trackSlot(origin.tracks[static_cast<std::size_t>(r.row)], TrackRole::session);
                const auto scene = std::find_if(d.scenes.begin(), d.scenes.end(), [&](const auto& s) { return s.id == origin.scenes[static_cast<std::size_t>(r.source)]; });
                if (track < 0 || scene == d.scenes.end()) continue;
                r.row = track;
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
            hit.zone = Zone::ruler;
            for (auto it = shown().meterChanges.rbegin(); it != shown().meterChanges.rend(); ++it)
                if (meterBounds(*it).expanded(2.0f, 1.0f).contains(p.toFloat())) { hit.meterBar = it->bar; return hit; }
            const auto& changes = shown().tempoChanges;
            for (auto it = changes.rbegin(); it != changes.rend(); ++it)
                if (tempoBounds(*it).expanded(3.0f, 2.0f).contains(p.toFloat())) { hit.tempo = it->tick; break; }
            return hit;
        }
        hit.displayRow = displayRowAt(p.y);
        if (const int below = hit.displayRow >= 0 ? p.y - rowTop(hit.displayRow) - rowHeight : -1; below >= 0) {
            // Under the row: one of its track's take lanes, in the header (its label) or across the lanes.
            const auto list = takesOf(timelineRow(hit.displayRow));
            const int lane = below / takeLaneHeight;
            if (lane < static_cast<int>(list.size())) { hit.zone = Zone::take; hit.take = list[static_cast<std::size_t>(lane)]->id; hit.takeLane = lane; }
            return hit;
        }
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
            if (isAudioRegion(*it) && box.getWidth() >= edgeGrab * 4.0f && static_cast<float>(p.y) - box.getY() <= fadeHandleHeight) {
                const auto handles = fadeHandles(*it, box);
                const float x = static_cast<float>(p.x);
                // Nearest wins, so the two can be told apart when they meet.
                const float toIn = std::abs(x - handles.first), toOut = std::abs(x - handles.second);
                if (std::min(toIn, toOut) <= edgeGrab) { hit.fade = toIn <= toOut ? Edge::left : Edge::right; break; }
            }
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
        if (hit.zone == Zone::region && (hit.edge != Edge::none || hit.fade != Edge::none)) cursorType = juce::MouseCursor::LeftRightResizeCursor;
        else if (hit.zone == Zone::region && e.mods.isAltDown() && !e.mods.isShiftDown() && !e.mods.isCommandDown()) {
            if (const auto* r = committed().find(hit.region); r != nullptr && isAudioRegion(*r))
                cursorType = juce::MouseCursor::LeftRightResizeCursor;
        }
        else if (hit.zone == Zone::loopStrip && (hit.loopPart == Hit::LoopPart::start || hit.loopPart == Hit::LoopPart::end)) cursorType = juce::MouseCursor::LeftRightResizeCursor;
        else if (hit.zone == Zone::markerStrip && hit.marker != 0) cursorType = juce::MouseCursor::PointingHandCursor;
        else if (hit.zone == Zone::ruler && hit.tempo >= 0) cursorType = juce::MouseCursor::PointingHandCursor;
        else if (hit.zone == Zone::take && e.x >= headerWidth) cursorType = juce::MouseCursor::IBeamCursor;   // drag across a take to comp that range
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
                if (!takesOf(row).empty() && takeLanesButtonRect(hit.displayRow).expanded(2).contains(e.getPosition())) { toggleTakeLanes(row); break; }
                setFocusRow(row); scrollRowIntoView(focus);
            }
            break;
        case Zone::ruler:
            if (hit.tempo >= 0) {
                drag.kind = DragKind::tempo; drag.newAnchor = hit.tempo;
                drag.grabOffset = static_cast<double>(hit.tempo) - drag.anchorTick;
                drag.base = baseTimeline();
                break;
            }
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
            if (hit.fade != Edge::none && !extend) {
                if (!wasSelected || selected.size() > 1) setSelection({hit.region});
                drag.kind = hit.fade == Edge::left ? DragKind::fadeIn : DragKind::fadeOut;
                drag.grabbed = hit.region; drag.base = baseTimeline();
                break;
            }
            if (hit.edge != Edge::none && !extend) {
                // Resizing is about one clip: select just it.
                if (!wasSelected || selected.size() > 1) setSelection({hit.region});
                drag.kind = hit.edge == Edge::left ? DragKind::trimStart : DragKind::trimEnd;
                drag.grabbed = hit.region; drag.base = baseTimeline();
                drag.edgeTick = hit.edge == Edge::left ? r->start : r->end();
                drag.grabOffset = static_cast<double>(drag.edgeTick) - drag.anchorTick;
                break;
            }
            // Alt on an audio body slips the source; edges and fade handles keep
            // their existing gestures. Other modifier combinations keep selection/copy.
            if (bypass && !extend && isAudioRegion(*r)) {
                if (!wasSelected) setSelection({hit.region});
                drag.kind = DragKind::slip;
                drag.grabbed = hit.region; drag.base = baseTimeline();
                for (const auto id : selected)
                    if (const auto* item = committed().find(id); item != nullptr && isAudioRegion(*item)) drag.ids.push_back(id);
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
        case Zone::take:
            // A click picks the take; a drag across it comps that range (on release, one undo step).
            selectTake(hit.take);
            if (e.x >= headerWidth) { drag.kind = DragKind::comp; drag.grabbed = hit.take; drag.base = baseTimeline(); }
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
        case DragKind::slip: {
            // Relative to the original press, never the previous preview. Right
            // reveals later source audio. Alt slips freely at integer-tick resolution.
            const auto delta = static_cast<std::int64_t>(std::llround(tickNow - drag.anchorTick));
            const auto ids = drag.ids;
            drag.label = ids.size() == 1 ? "Slip audio clip" : "Slip audio clips";
            drag.apply = [=](timeline::Timeline& t) { return t.slip(ids, delta); };
            updatePreview(); break;
        }
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
        case DragKind::fadeIn: case DragKind::fadeOut: {
            const auto* r = drag.base->find(drag.grabbed);
            if (r == nullptr) break;
            const auto id = drag.grabbed;
            const auto at = std::clamp<std::int64_t>(snapTick(tickNow, bypass), r->start, r->end());
            if (drag.kind == DragKind::fadeIn) {
                const auto ticks = at - r->start;
                drag.label = r->crossfade > 0 ? "Crossfade" : "Fade in";
                drag.apply = [=](timeline::Timeline& t) { return t.setFadeIn(id, ticks); };
            } else {
                const auto ticks = r->end() - at;
                drag.label = "Fade out";
                drag.apply = [=](timeline::Timeline& t) { return t.setFadeOut(id, ticks); };
            }
            updatePreview(); break;
        }
        case DragKind::marker: {
            const auto id = drag.grabbed;
            const auto to = snapTick(tickNow + drag.grabOffset, bypass);
            drag.label = "Move marker";
            drag.apply = [=](timeline::Timeline& t) { return t.moveMarker(id, to); };
            updatePreview(); break;
        }
        case DragKind::tempo: {
            const auto from = drag.newAnchor;
            const auto to = snapTick(tickNow + drag.grabOffset, bypass);
            drag.label = "Move tempo change";
            drag.apply = [=](timeline::Timeline& t) { return to != from && t.moveTempoChange(from, to); };
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
        case DragKind::comp: {
            const auto* take = takeById(drag.grabbed);
            if (take == nullptr) break;
            auto from = snapTick(drag.anchorTick, bypass), to = snapTick(tickNow, bypass);
            if (to < from) std::swap(from, to);
            from = std::clamp(from, take->start, take->end()); to = std::clamp(to, take->start, take->end());
            drag.newAnchor = from; drag.edgeTick = to;
            if (to <= from) { drag.apply = nullptr; preview.reset(); repaint(); break; }
            const auto id = drag.grabbed;
            drag.label = "Comp recording take";
            drag.apply = [=](timeline::Timeline& t) { return t.compTake(id, from, to); };
            updatePreview(); break;
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
        case DragKind::move: case DragKind::copy: case DragKind::slip: case DragKind::trimStart: case DragKind::trimEnd:
        case DragKind::fadeIn: case DragKind::fadeOut: case DragKind::marker: case DragKind::loopNew: case DragKind::loopStart: case DragKind::loopEnd: case DragKind::loopMove:
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
        case DragKind::tempo:
            if (active && finished.apply) {
                if (!tempoEdit(finished.label, finished.apply) && hadPreview && !controller.isBusy()) say("A tempo change cannot go there.");
            } else if (!active) setCursor(finished.newAnchor);   // a click on a tempo change plays from there
            break;
        case DragKind::comp:
            if (active && finished.apply) {
                if (!compFromTake(finished.grabbed, finished.newAnchor, finished.edgeTick) && !controller.isBusy()) say("That take cannot be comped.");
            } else if (!active) setCursor(snapTick(finished.anchorTick, bypass));   // a click: the take is picked, and plays from there
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
            if (const auto* r = committed().find(hit.region); r != nullptr) openRegion(*r);
        } else if (hit.zone == Zone::lane && hit.displayRow >= 0) {
            placeDefault(timelineRow(hit.displayRow), tick);
        } else if (hit.zone == Zone::markerStrip) {
            if (hit.marker != 0) renameMarker(hit.marker);
            else { setCursor(tick, false); addMarkerAtCursor(); }
        } else if (hit.zone == Zone::ruler && hit.meterBar >= 0) {
            askMeter(hit.meterBar);
        } else if (hit.zone == Zone::ruler) {
            askTempo(hit.tempo >= 0 ? hit.tempo : tick);   // a tempo change, or a new one where the pointer is
        } else if (hit.zone == Zone::loopStrip) {
            // Double-click: loop the bar under the pointer.
            const auto bar = committed().meterMap().barAt(std::max(0.0, tickAtX(static_cast<double>(e.x))));
            setLoopRange(bar.start, bar.start + bar.meter.barTicks());
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
    enum class DragKind { none, scrub, move, copy, slip, trimStart, trimEnd, fadeIn, fadeOut, marker, tempo, loopNew, loopStart, loopEnd, loopMove, marquee, comp };
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
        if (drag.kind == DragKind::marquee || drag.kind == DragKind::move || drag.kind == DragKind::copy || drag.kind == DragKind::slip || drag.kind == DragKind::trimEnd
            || drag.kind == DragKind::loopNew || drag.kind == DragKind::loopEnd || drag.kind == DragKind::scrub || drag.kind == DragKind::comp) {
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
                if (isAudioRegion(*r)) {   // fades act on audio only
                    menu.addItem(cmdFade, "Fade in and out"); menu.addItem(cmdCrossfade, "Crossfade", canCrossfadeSelection());
                    menu.addItem(cmdRemoveFades, "Remove fades", selectionHasFades());
                    // One submenu per fade: its shape (the first selected clip's is ticked).
                    const auto shapes = [&](timeline::FadeEdge edge, const char* title, timeline::FadeCurve current) {
                        juce::PopupMenu sub;
                        for (int c = 0; c < timeline::fadeCurveCount; ++c) {
                            const auto curve = static_cast<timeline::FadeCurve>(c);
                            sub.addItem(fadeShapeCommand(edge, curve), timeline::fadeCurveLabel(curve), true, curve == current);
                        }
                        menu.addSubMenu(title, sub);
                    };
                    shapes(timeline::FadeEdge::in, "Fade-in shape", r->fadeInCurve);
                    shapes(timeline::FadeEdge::out, "Fade-out shape", r->fadeOutCurve);
                    shapes(timeline::FadeEdge::crossfade, "Crossfade shape", r->crossfadeCurve);
                }
            }
            menu.addSeparator();
            menu.addItem(cmdDuplicate, "Duplicate");
            menu.addItem(cmdCopy, "Copy"); menu.addItem(cmdCut, "Cut"); menu.addItem(cmdDelete, "Delete");
            menu.addSeparator();
            menu.addItem(cmdGroup, "Group", selected.size() > 1); menu.addItem(cmdUngroup, "Ungroup", anyGrouped);
            menu.addSeparator();
            menu.addItem(cmdOpen, r != nullptr && r->isPattern() ? "Open in Channel Rack" : "Open clip");
            if (r != nullptr && !r->isPattern()) menu.addItem(cmdMakeUnique, "Make this placement unique", controller.placementShared(hit.region));
            launch(std::move(menu), area, [this, region = hit.region](int id) {
                switch (id) {
                case cmdSplit: splitAtCursor(); break; case cmdMute: toggleMuteSelection(); break;
                case cmdSlipLeft: slipSelection(-timeline::ticksPerStep); break; case cmdSlipRight: slipSelection(timeline::ticksPerStep); break;
                case cmdSourceLoop: toggleSourceLoopSelection(); break; case cmdReverse: toggleReverseSelection(); break;
                case cmdNormalize: normalizeSelection(); break; case cmdFade: fadeSelection(); break; case cmdCrossfade: crossfadeSelection(); break;
                case cmdRemoveFades: removeFadesSelection(); break;
                case cmdFadeShapeBase + 0: case cmdFadeShapeBase + 1: case cmdFadeShapeBase + 2: case cmdFadeShapeBase + 3: case cmdFadeShapeBase + 4:
                case cmdFadeShapeBase + 10: case cmdFadeShapeBase + 11: case cmdFadeShapeBase + 12: case cmdFadeShapeBase + 13: case cmdFadeShapeBase + 14:
                case cmdFadeShapeBase + 20: case cmdFadeShapeBase + 21: case cmdFadeShapeBase + 22: case cmdFadeShapeBase + 23: case cmdFadeShapeBase + 24:
                    setFadeShapeSelection(static_cast<timeline::FadeEdge>((id - cmdFadeShapeBase) / 10), static_cast<timeline::FadeCurve>((id - cmdFadeShapeBase) % 10));
                    break;
                case cmdDuplicate: duplicateSelection(); break; case cmdCopy: copySelection(); break;
                case cmdCut: cutSelection(); break; case cmdDelete: deleteSelection(); break;
                case cmdGroup: groupSelection(); break; case cmdUngroup: ungroupSelection(); break;
                case cmdOpen: if (const auto* r = committed().find(region); r != nullptr) openRegion(*r); break;
                case cmdMakeUnique: makePlacementUnique(region); break;
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
        } else if (hit.zone == Zone::ruler && hit.meterBar >= 0) {
            juce::PopupMenu menu;
            menu.addItem(cmdJump, "Play from here");
            menu.addItem(cmdMeter, "Change time signature...");
            menu.addItem(cmdRemoveMeter, "Delete time signature change");
            launch(std::move(menu), area, [this, bar = hit.meterBar](int id) {
                if (id == cmdJump) setCursor(committed().tickOfBar(bar));
                else if (id == cmdMeter) askMeter(bar);
                else if (id == cmdRemoveMeter) removeMeterChange(bar);
            });
        } else if (hit.zone == Zone::ruler && hit.tempo >= 0) {
            juce::PopupMenu menu;
            menu.addItem(cmdJump, "Play from here");
            menu.addItem(cmdTempo, "Change tempo...");
            menu.addItem(cmdRemoveTempo, "Delete tempo change");
            launch(std::move(menu), area, [this, at = hit.tempo](int id) {
                if (id == cmdJump) setCursor(at);
                else if (id == cmdTempo) askTempo(at);
                else if (id == cmdRemoveTempo) removeTempoChange(at);
            });
        } else if (hit.zone == Zone::markerStrip || hit.zone == Zone::ruler) {
            juce::PopupMenu menu;
            menu.addItem(cmdAddMarker, "Add marker here");
            if (hit.zone == Zone::ruler) {
                menu.addItem(cmdTempo, "Add tempo change here...", tick > 0);
                menu.addItem(cmdRemoveTempo, "Remove all tempo changes", !committed().tempoChanges.empty());
                menu.addSeparator();
                const auto bar = static_cast<int>(committed().meterMap().barAt(static_cast<double>(tick)).index);
                menu.addItem(cmdMeter, "Time signature from bar " + juce::String(bar + 1) + "...", bar < committed().lengthBars);
                menu.addItem(cmdRemoveMeter, "Remove all time signature changes", !committed().meterChanges.empty());
            }
            launch(std::move(menu), area, [this, tick](int id) {
                if (id == cmdAddMarker) { setCursor(tick, false); addMarkerAtCursor(); }
                else if (id == cmdTempo) askTempo(tick);
                else if (id == cmdRemoveTempo) tempoEdit("Remove tempo changes", [](timeline::Timeline& t) { return t.clearTempoChanges(); });
                else if (id == cmdMeter) askMeter(static_cast<int>(committed().meterMap().barAt(static_cast<double>(tick)).index));
                else if (id == cmdRemoveMeter) edit("Remove time signature changes", [](timeline::Timeline& t) { return t.clearMeterChanges(); });
            });
        } else if (hit.zone == Zone::loopStrip) {
            juce::PopupMenu menu;
            menu.addItem(cmdJump, committed().loopEnabled ? "Loop off" : "Loop on");
            menu.addItem(cmdLoopSelection, "Loop the selected clips", !selected.empty());
            launch(std::move(menu), area, [this](int id) { if (id == cmdJump) toggleLoop(); else if (id == cmdLoopSelection) loopSelection(); });
        } else if (hit.zone == Zone::take) {
            selectTake(hit.take);
            const int track = timelineRow(hit.displayRow);
            setFocusRow(track);
            if (e.x >= headerWidth) setCursor(tick, false);
            const auto* take = takeById(hit.take);
            if (take == nullptr) return;
            const auto& t = committed();
            const bool ready = canComp(*take);
            juce::PopupMenu menu;
            menu.addSectionHeader("Take " + juce::String(hit.takeLane + 1));
            menu.addItem(cmdPlayTake, "Play this take", ready);
            menu.addItem(cmdPlayTakeLoop, "Play this take in the loop range", ready && t.loopStart >= take->start && t.loopEnd <= take->end());
            menu.addSeparator();
            menu.addItem(cmdForgetTake, "Forget take", !controller.isBusy() && !controller.isRecording());
            menu.addItem(cmdTakeLanes, "Hide take lanes");
            launch(std::move(menu), area, [this, id = hit.take, track](int chosen) {
                if (chosen == cmdPlayTake) playWholeTake(id);
                else if (chosen == cmdPlayTakeLoop) compFromTake(id, committed().loopStart, committed().loopEnd);
                else if (chosen == cmdForgetTake) forgetTake(id);
                else if (chosen == cmdTakeLanes) setTakeLanesShown(track, false);
            });
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
        if(controller.isTrackFrozen(track))return "Frozen audio";
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
        menu.addItem(cmdArm, s.armed ? "Disarm recording" : "Arm for recording", !s.frozen(), s.armed);
        menu.addItem(s.frozen()?cmdUnfreeze:cmdFreeze,s.frozen()?"Unfreeze track":"Freeze track (render audio)",!controller.isBusy() && !controller.isRecording());
        menu.addSubMenu("Input (" + inputText(track) + ")", inputs,!s.frozen());
        menu.addSubMenu("Monitor input", monitor,!s.frozen());
        menu.addSeparator();
        const auto takes = takesOf(track).size();
        menu.addItem(cmdTakeLanes, takes == 0 ? juce::String("Show take lanes (no takes yet)") : "Show take lanes (" + juce::String(static_cast<int>(takes)) + ")",
                     takes > 0 || takeLanesShown(track), takeLanesShown(track));
        return menu;
    }
    void recordingMenuChosen(int track, int id) {
        if (id == cmdTakeLanes) toggleTakeLanes(track);
        else if(id==cmdFreeze){if(onFreezeTrack)onFreezeTrack(track);}
        else if(id==cmdUnfreeze){if(onUnfreezeTrack)onUnfreezeTrack(track);}
        else if (id == cmdArm) toggleArm(track);
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
        if (r.ownsClip()) {
            const auto name = d.placementName(r.clip);
            return (name.isNotEmpty() ? name : juce::String("Clip")) + ui::utf8("  \xc2\xb7  ") + "Song clip";
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
            if (y + blockHeight(d) < topHeight || y > getHeight()) continue;
            g.setColour(d % 2 == 0 ? col::surface : col::raised.withAlpha(0.55f));
            g.fillRect(lanes.getX(), y, lanes.getWidth(), rowHeight);
            if (d == displayRowOf(focus)) { g.setColour(col::accent.withAlpha(0.05f)); g.fillRect(lanes.getX(), y, lanes.getWidth(), rowHeight); }
            if (d == trackCount()) { g.setColour(col::lineStrong); g.fillRect(lanes.getX(), y, lanes.getWidth(), 2); }
            for (int lane = 0, count = takeLaneCount(timelineRow(d)); lane < count; ++lane) {   // the take lanes: sunk below the row
                const int top = y + rowHeight + lane * takeLaneHeight;
                g.setColour(col::inset); g.fillRect(lanes.getX(), top, lanes.getWidth(), takeLaneHeight);
                g.setColour(col::line); g.fillRect(lanes.getX(), top, lanes.getWidth(), 1);
            }
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
        forEachGridLine(t, scrollX, scrollX + visibleTicks(), fineGrid(), [&](std::int64_t whole, int kind, std::int64_t, std::int64_t) {
            const float x = static_cast<float>(xForTick(static_cast<double>(whole)));
            if (x < headerWidth || x > getWidth()) return;
            g.setColour((kind == 2 ? col::lineStrong : kind == 1 ? col::line.brighter(0.15f) : col::line).withAlpha(kind == 2 ? 1.0f : 0.7f));
            g.fillRect(x, static_cast<float>(topHeight), 1.0f, static_cast<float>(getHeight() - topHeight));
        });
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
        // Takes, in their lanes.
        for (int track = 0; track < trackCount() && !laneTracks.empty(); ++track) {
            if (takeLaneCount(track) == 0) continue;
            for (const auto* take : takesOf(track)) {
                const auto box = takeBounds(take->id);
                if (box.getRight() < headerWidth || box.getX() > getWidth() || box.getBottom() < topHeight || box.getY() > getHeight()) continue;
                paintTake(g, t, *take, box);
            }
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
        if (isAudioRegion(r)) paintFades(g, r, box);
        g.setColour(colour.withAlpha(r.muted ? 0.4f : 1.0f));
        g.fillRoundedRectangle(box.withWidth(std::min(3.0f, box.getWidth())), 1.5f);
        g.setColour(isSel ? juce::Colours::white : colour.withAlpha(0.65f));
        g.drawRoundedRectangle(box, 4.0f, isSel ? 1.6f : 1.0f);
    }

    // A take in its lane: its waveform dim, and bright where the arrangement plays it (the range being dragged is outlined).
    void paintTake(juce::Graphics& g, const timeline::Timeline& t, const timeline::Region& take, juce::Rectangle<float> box) {
        namespace col = ui::colours;
        const auto colour = rowColour(take.row);
        const bool isSel = take.id == selectedTake;
        g.setColour(colour.withAlpha(isSel ? 0.14f : 0.08f)); g.fillRoundedRectangle(box, 3.0f);
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(box.toNearestInt());
            paintContent(g, take, box, colour.withAlpha(0.32f), 2.0f);
        }
        for (const auto& [from, to] : t.takePlays(take.id, controller.songTempo())) {
            const float x0 = std::max(box.getX(), static_cast<float>(xForTick(static_cast<double>(from))));
            const float x1 = std::min(box.getRight(), static_cast<float>(xForTick(static_cast<double>(to))));
            if (x1 - x0 < 0.5f) continue;
            const auto part = juce::Rectangle<float>(x0, box.getY(), x1 - x0, box.getHeight());
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(part.toNearestInt());
            g.setColour(colour.withAlpha(0.30f)); g.fillRect(part);
            paintContent(g, take, box, colour.withAlpha(0.95f), 2.0f);
            g.setColour(colour); g.fillRect(part.withHeight(2.0f));
        }
        if (drag.kind == DragKind::comp && drag.active && drag.grabbed == take.id && drag.edgeTick > drag.newAnchor) {
            const float x0 = static_cast<float>(xForTick(static_cast<double>(drag.newAnchor))), x1 = static_cast<float>(xForTick(static_cast<double>(drag.edgeTick)));
            g.setColour(col::accent.withAlpha(0.18f)); g.fillRect(juce::Rectangle<float>(x0, box.getY(), x1 - x0, box.getHeight()));
            g.setColour(col::accent); g.drawRect(juce::Rectangle<float>(x0, box.getY(), x1 - x0, box.getHeight()), 1.5f);
        }
        g.setColour(isSel ? juce::Colours::white.withAlpha(0.85f) : colour.withAlpha(0.5f));
        g.drawRoundedRectangle(box, 3.0f, isSel ? 1.4f : 1.0f);
    }

    bool isAudioRegion(const timeline::Region& r) const {
        const auto& d = controller.document();
        return !r.isPattern() && d.placementContent(r).audio != nullptr;
    }
    // Where the fade-in (or crossfade) and fade-out handles sit across a clip's box.
    std::pair<float, float> fadeHandles(const timeline::Region& r, juce::Rectangle<float> box) const {
        const auto in = static_cast<float>(static_cast<double>(r.crossfade > 0 ? r.crossfade : r.fadeIn) * ppt);
        const auto out = static_cast<float>(static_cast<double>(r.fadeOut) * ppt);
        return {std::min(box.getX() + in, box.getRight()), std::max(box.getRight() - out, box.getX())};
    }
    // Shades what each fade takes away and draws its curve in the shape it plays (FadeCurve).
    void paintFades(juce::Graphics& g, const timeline::Region& r, juce::Rectangle<float> box) {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(box.toNearestInt());
        const float top = box.getY() + 2.0f, bottom = box.getBottom() - 1.0f;
        const auto [inX, outX] = fadeHandles(r, box);
        auto shade = [&](float from, float to, bool rising, timeline::FadeCurve shape) {
            if (to - from < 1.0f) return;
            juce::Path curve, area;
            const int steps = shape == timeline::FadeCurve::linear ? 1 : 24;
            for (int i = 0; i <= steps; ++i) {
                const float x = static_cast<float>(i) / static_cast<float>(steps);
                const auto level = static_cast<float>(timeline::fadeGain(shape, rising ? x : 1.0f - x));
                const float y = bottom - (bottom - top) * level;
                const float px = from + (to - from) * x;
                if (i == 0) curve.startNewSubPath(px, y); else curve.lineTo(px, y);
            }
            area = curve;
            area.lineTo(to, top); area.lineTo(from, top); area.closeSubPath();   // everything above the curve is faded away
            g.setColour(juce::Colours::black.withAlpha(0.28f)); g.fillPath(area);
            g.setColour(juce::Colours::white.withAlpha(r.muted ? 0.35f : 0.8f)); g.strokePath(curve, juce::PathStrokeType(1.2f));
        };
        if (r.crossfade > 0) {
            shade(box.getX(), inX, true, r.crossfadeCurve);
            // The outgoing clip's falling curve, mirrored across the same span.
            juce::Path falling;
            for (int i = 0; i <= 24; ++i) {
                const float x = static_cast<float>(i) / 24.0f;
                const float px = box.getX() + (inX - box.getX()) * x;
                const float y = bottom - (bottom - top) * static_cast<float>(timeline::fadeGain(r.crossfadeCurve, 1.0f - x));
                if (i == 0) falling.startNewSubPath(px, y); else falling.lineTo(px, y);
            }
            g.setColour(juce::Colours::white.withAlpha(0.45f)); g.strokePath(falling, juce::PathStrokeType(1.0f));
        } else shade(box.getX(), inX, true, r.fadeInCurve);
        shade(outX, box.getRight(), false, r.fadeOutCurve);
        if (box.getWidth() >= edgeGrab * 4.0f) {
            g.setColour(juce::Colours::white.withAlpha(r.muted ? 0.4f : 0.9f));
            for (const float x : {inX, outX})
                g.fillRect(juce::Rectangle<float>(5.0f, 5.0f).withCentre({std::clamp(x, box.getX() + 3.0f, box.getRight() - 3.0f), box.getY() + 4.0f}));
        }
    }

    // What is inside a region: notes, a waveform, or a pattern's steps, repeated for as long as the region lasts.
    void paintContent(juce::Graphics& g, const timeline::Region& r, juce::Rectangle<float> box, juce::Colour ink, float labelSpace = 16.0f) {
        const auto& d = controller.document();
        const auto inner = box.withTrimmedTop(labelSpace).withTrimmedBottom(3.0f).reduced(2.0f, 0.0f);
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
        if (!r.ownsClip() && !d.isValidCell(r.row, r.source)) return;
        const auto clip = d.placementContent(r);
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
            const auto tempo = controller.songTempo();
            const double start = static_cast<double>(r.start);
            for (int px = first; px <= last; ++px) {
                const double tickInRegion = (static_cast<double>(px) - x0) / ppt;
                if (tickInRegion < 0 || tickInRegion >= static_cast<double>(r.length)) continue;
                const double seconds = r.audioOffset + tempo.secondsBetween(start, start + tickInRegion);
                if (seconds >= duration) break;
                const double secondsNext = r.audioOffset + tempo.secondsBetween(start, start + tickInRegion + 1.0 / ppt);
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
            if (y + blockHeight(i) < topHeight || y > getHeight()) continue;
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
            if (!isLane) paintTakeLaneHeaders(g, i, row);
        }
        g.setColour(col::lineStrong); g.fillRect(headerWidth - 1, topHeight, 1, getHeight() - topHeight);
    }

    // The button that shows a track's take lanes (with how many takes it has), and the label of each open lane.
    void paintTakeLaneHeaders(juce::Graphics& g, int displayRow, int track) {
        namespace col = ui::colours;
        const auto list = takesOf(track);
        if (list.empty()) return;
        const bool open = takeLanesShown(track);
        const auto button = takeLanesButtonRect(displayRow).toFloat();
        g.setColour(open ? col::accent.withAlpha(0.8f) : col::control); g.fillRoundedRectangle(button, 2.5f);
        g.setColour(open ? col::accentInk : col::textDim); g.setFont(ui::monoFont(9.0f));
        g.drawText(juce::String(static_cast<int>(list.size())), button.toNearestInt(), juce::Justification::centred, false);
        if (!open) return;
        const auto tempo = controller.songTempo();
        for (std::size_t lane = 0; lane < list.size(); ++lane) {
            const auto area = takeLaneBounds(track, static_cast<int>(lane)).withWidth(headerWidth - 1);
            const bool isSel = list[lane]->id == selectedTake;
            g.setColour(isSel ? col::hover : col::inset); g.fillRect(area);
            g.setColour(rowColour(track).withAlpha(0.6f)); g.fillRect(area.withWidth(4).withTrimmedLeft(2));
            g.setColour(col::line); g.fillRect(area.withHeight(1));
            const bool plays = !shown().takePlays(list[lane]->id, tempo).empty();
            g.setColour(plays ? col::text : col::textDim); g.setFont(ui::font(11.0f, plays));
            g.drawText("Take " + juce::String(static_cast<int>(lane) + 1) + (plays ? "  (playing)" : ""), area.withTrimmedLeft(14), juce::Justification::centredLeft, true);
        }
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
        const int rulerTop = markerStripHeight + loopStripHeight;
        forEachGridLine(t, scrollX, scrollX + visibleTicks(), fineGrid(), [&](std::int64_t whole, int kind, std::int64_t barIndex, std::int64_t beatIndex) {
            const float x = static_cast<float>(xForTick(static_cast<double>(whole)));
            if (x < headerWidth || x > getWidth()) return;
            const bool bar = kind == 2, beat = kind == 1;
            g.setColour(bar ? col::textDim : col::textFaint.withAlpha(0.7f));
            g.fillRect(x, static_cast<float>(rulerTop + (bar ? 4 : beat ? 14 : 18)), 1.0f, static_cast<float>(rulerHeight - (bar ? 4 : beat ? 14 : 18)));
            if (bar && barIndex % labelStep == 0) {
                g.setColour(col::text); g.setFont(ui::monoFont(10.5f));
                g.drawText(juce::String(barIndex + 1), juce::roundToInt(x) + 3, rulerTop, 40, 14, juce::Justification::centredLeft);
            } else if (beat && ppt * static_cast<double>(t.meterMap().meterAtBar(barIndex).beatTicks()) >= 30.0) {
                g.setColour(col::textFaint); g.setFont(ui::monoFont(9.5f));
                g.drawText(juce::String(barIndex + 1) + "." + juce::String(beatIndex + 1), juce::roundToInt(x) + 3, rulerTop + 2, 40, 12, juce::Justification::centredLeft);
            }
        });
        // Time signature changes: a label after the bar number.
        for (const auto& c : t.meterChanges) {
            const auto box = meterBounds(c);
            if (box.getRight() < headerWidth || box.getX() > getWidth()) continue;
            g.setColour(col::accent.withAlpha(0.85f));
            g.fillRoundedRectangle(box, 2.5f);
            g.setColour(col::accentInk); g.setFont(ui::monoFont(9.5f));
            g.drawText(meterText({c.numerator, c.denominator}), box.toNearestInt().reduced(4, 0), juce::Justification::centredLeft, false);
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
        // Tempo changes: a line through the ruler and a flag with the tempo.
        for (const auto& c : t.tempoChanges) {
            const auto box = tempoBounds(c);
            if (box.getRight() < headerWidth || box.getX() > getWidth()) continue;
            g.setColour(col::warning);
            g.fillRect(box.getX(), static_cast<float>(rulerTop), 1.5f, static_cast<float>(rulerHeight));
            g.setColour(col::warning.withAlpha(0.85f));
            g.fillRoundedRectangle(box, 2.5f);
            g.setColour(col::accentInk); g.setFont(ui::monoFont(9.5f));
            g.drawText(tempoLabel(c.bpm), box.toNearestInt().reduced(4, 0), juce::Justification::centredLeft, false);
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
    std::uint32_t laneScope{};
    std::vector<TrackId> laneTracks;   // ids of the tracks showing their take lanes (view only, not saved)
    std::uint32_t selectedTake{0};
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
        refreshSends(); parameterBox.setSelectedId(1);
        curveBox.setName("Automation interpolation");
        curveBox.addItem("Linear", 1); curveBox.addItem("Hold", 2); curveBox.setSelectedId(1);
        enableButton.setButtonText("Read"); enableButton.setName("Automation read"); enableButton.setClickingTogglesState(true);
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
        refreshSends();
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
    // One "Send" item per return that exists (its slot's lane); a removed return's choice falls back to Volume.
    void refreshSends() {
        juce::StringArray items{"Volume", "Pan"};
        juce::Array<int> ids{1, 2};
        const auto& returns = controller.document().returns;
        for (int r = 0; r < maxReturns; ++r) {
            const auto& ret = returns[static_cast<std::size_t>(r)];
            if (!ret.used) continue;
            const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + r));
            items.add("Send " + letter + (ret.name != defaultReturnName(r) ? " (" + ret.name + ")" : juce::String{}));
            ids.add(3 + r);
        }
        bool same = parameterBox.getNumItems() == items.size();
        for (int i = 0; same && i < items.size(); ++i) same = parameterBox.getItemId(i) == ids[i] && parameterBox.getItemText(i) == items[i];
        if (same) return;
        const int selected = parameterBox.getSelectedId();
        parameterBox.clear(juce::dontSendNotification);
        for (int i = 0; i < items.size(); ++i) parameterBox.addItem(items[i], ids[i]);
        parameterBox.setSelectedId(ids.contains(selected) ? selected : 1, juce::dontSendNotification);
    }
    automation::Parameter selectedParameter() const { return static_cast<automation::Parameter>(std::clamp(parameterBox.getSelectedId() - 1, 0, automation::parametersPerTrack - 1)); }
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
    float yFor(float value) const { const auto a = plot(); const float low = automation::minimum(selectedParameter()); return a.getBottom() - (value - low) / (automation::maximum(selectedParameter()) - low) * a.getHeight(); }
    void updatePoint(juce::Point<float> position) {
        auto* lane = editable(*preview); const auto a = plot();
        if (a.getWidth() <= 0 || a.getHeight() <= 0) return;
        if (activeTick >= 0) std::erase_if(lane->points, [&](const automation::Point& p) { return p.tick == activeTick; });
        const double fraction = std::clamp(static_cast<double>((position.x - a.getX()) / a.getWidth()), 0.0, 1.0);
        activeTick = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(fraction * preview->length() / timeline::ticksPerStep)) * timeline::ticksPerStep, 0, preview->length());
        const float low = automation::minimum(selectedParameter());
        const float value = low + (automation::maximum(selectedParameter()) - low) * std::clamp((a.getBottom() - position.y) / a.getHeight(), 0.0f, 1.0f);
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

// Recording passes remain separate from the active arrangement. Comping places
// an ordinary audio region, so live playback and WAV export share the result.
class RecordingTakesPanel final : public juce::Component, private juce::ListBoxModel {
public:
    explicit RecordingTakesPanel(ProjectController& owner) : controller(owner), list("Recording takes", this) {
        setName("Recording takes");
        addAndMakeVisible(list); addAndMakeVisible(whole); addAndMakeVisible(range); addAndMakeVisible(forget); addAndMakeVisible(hint);
        list.setRowHeight(26); list.setMultipleSelectionEnabled(false);
        whole.setButtonText("Use whole take"); whole.setName("Comp whole take");
        range.setButtonText("Use loop range"); range.setName("Comp loop range");
        range.setTooltip("Replace only the loop-marker range on this take's track; the rest of the comp stays.");
        forget.setButtonText("Forget take"); forget.setName("Forget recording take");
        forget.setTooltip("Remove this pass from the take list. Active comp and audio file stay; Undo restores the list entry.");
        forget.onClick = [this] {
            const bool done = tlui::forgetTake(controller, selectedId);
            refresh();
            if (done && onChanged) onChanged();
        };
        whole.onClick = [this] { apply(false); };
        range.onClick = [this] { apply(true); };
        hint.setColour(juce::Label::textColourId, ui::colours::textDim);
        hint.setFont(ui::font(12.0f));
        refresh();
    }
    ~RecordingTakesPanel() override { list.setModel(nullptr); }
    std::function<void()> onChanged;   // a take was comped or forgotten here
    void refresh() {
        const auto old = selectedId;
        ids.clear();
        for (const auto& take : controller.timeline().takes) ids.push_back(take.id);
        list.updateContent();
        auto found = std::find(ids.begin(), ids.end(), old);
        if (!ids.empty()) list.selectRow(found == ids.end() ? static_cast<int>(ids.size()) - 1 : static_cast<int>(found - ids.begin()));
        else { list.deselectAllRows(); selectedId = 0; }
        updateButtons(); list.repaint();
    }
    void resized() override {
        auto area = getLocalBounds().reduced(8);
        list.setBounds(area.removeFromLeft(std::max(160, area.getWidth() / 2)));
        area.removeFromLeft(12);
        auto buttons = area.removeFromTop(28);
        whole.setBounds(buttons.removeFromLeft(124)); buttons.removeFromLeft(6);
        range.setBounds(buttons.removeFromLeft(124));
        forget.setBounds(area.removeFromBottom(26).removeFromLeft(124));
        hint.setBounds(area.reduced(0, 4));
    }
    void paint(juce::Graphics& g) override { g.fillAll(ui::colours::raised); }
private:
    int getNumRows() override { return static_cast<int>(ids.size()); }
    const timeline::Region* selected() const {
        for (const auto& take : controller.timeline().takes) if (take.id == selectedId) return &take;
        return nullptr;
    }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selectedRow) override {
        if (row < 0 || row >= static_cast<int>(ids.size())) return;
        if (selectedRow) g.fillAll(ui::colours::accent.withAlpha(0.2f));
        const auto& takes = controller.timeline().takes;
        const auto found = std::find_if(takes.begin(), takes.end(), [&](const auto& take) { return take.id == ids[static_cast<std::size_t>(row)]; });
        if (found == takes.end() || !controller.document().isValidCell(found->row, found->source)) return;
        const auto& clip = controller.document().clip(found->row, found->source);
        g.setColour(ui::colours::text); g.setFont(ui::font(12.0f));
        g.drawText("Take " + juce::String(row + 1) + " | Track " + juce::String(found->row + 1)
            + " | " + clip.audioFileName, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int row) override {
        selectedId = row >= 0 && row < static_cast<int>(ids.size()) ? ids[static_cast<std::size_t>(row)] : 0;
        updateButtons();
    }
    void updateButtons() {
        const auto* take = selected(); const auto& t = controller.timeline();
        const bool idle = take != nullptr && !controller.isBusy() && !controller.isRecording();
        forget.setEnabled(idle);
        const bool ready = idle && !take->reversed && !take->loopSource
            && controller.document().isValidCell(take->row, take->source)
            && controller.document().clip(take->row, take->source).isAudioClip();
        whole.setEnabled(ready);
        range.setEnabled(ready && t.loopStart >= take->start && t.loopEnd <= take->end());
        hint.setText(ids.empty() ? "Record a pass in Song view to keep a take here."
            : "Choose a pass. Use loop markers to comp a smaller range. Original passes are kept; Undo restores the previous comp.", juce::dontSendNotification);
    }
    void apply(bool loopRange) {
        const auto* take = selected(); if (take == nullptr) return;
        const auto id = take->id;
        const auto from = loopRange ? controller.timeline().loopStart : take->start;
        const auto to = loopRange ? controller.timeline().loopEnd : take->end();
        const bool done = controller.compRecordedTake(id, from, to);
        refresh();
        if (done && onChanged) onChanged();
    }
    ProjectController& controller;
    juce::ListBox list;
    juce::TextButton whole, range, forget;
    juce::Label hint;
    std::vector<std::uint32_t> ids;
    std::uint32_t selectedId{};
};

class TimelineView final : public juce::Component, private juce::ScrollBar::Listener {
public:
    static constexpr int toolbarHeight = 46, scrollBarSize = 12, helpHeight = 22;

    explicit TimelineView(ProjectController& projectController)
        : controller(projectController), canvas(projectController), clipBin(projectController), hBar(false), vBar(true), automationEditor(projectController), takesPanel(projectController) {
        namespace col = ui::colours;
        canvas.onFreezeTrack=[this](int track){if(onFreezeTrack)onFreezeTrack(track);};
        canvas.onUnfreezeTrack=[this](int track){if(onUnfreezeTrack)onUnfreezeTrack(track);};
        setName("Arrangement");
        addChildComponent(takesPanel);
        addAndMakeVisible(takesButton);
        takesButton.setButtonText("Takes"); takesButton.setName("Show recording takes");
        takesButton.setClickingTogglesState(true);
        takesButton.onClick = [this] { takesPanel.refresh(); takesPanel.setVisible(takesButton.getToggleState()); resized(); };
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
        addAndMakeVisible(snapBox); snapBox.setName("Snap"); snapBox.setTooltip("Round dragged clips, edges and the play position to this grid. Alt bypasses snap; Alt-drag on an audio body slips its source.");
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
        defaultHelp = "Double-click a lane to place a clip. Drag to move, Alt-drag audio to slip its source, drag an edge to resize or a top corner to fade. S splits, M mutes, L loops, K adds a marker, Ctrl+wheel zooms.";
        help.setText(defaultHelp, juce::dontSendNotification);
        canvas.onViewChanged = [this] { layoutScroll(); };
        canvas.onStateChanged = [this] { updateControls(); };
        canvas.onMessage = [this](const juce::String& text) { setStatus(text); };
        canvas.onTakesChanged = [this] { takesPanel.refresh(); };
        takesPanel.onChanged = [this] { canvas.refresh(); };
        refresh();
    }
    ~TimelineView() override { hBar.removeListener(this); vBar.removeListener(this); }

    TimelineCanvas& timelineCanvas() noexcept { return canvas; }
    ClipBin& clipList() noexcept { return clipBin; }
    bool clipListShown() const noexcept { return clipBin.isVisible(); }
    std::function<void(int)> onFreezeTrack, onUnfreezeTrack;
    std::function<void(std::int64_t from, std::int64_t to)> onExportStems;
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
        takesPanel.refresh();
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
        takesButton.setBounds(helpRow.removeFromLeft(68)); helpRow.removeFromLeft(8);
        clipsButton.setBounds(helpRow.removeFromRight(70)); helpRow.removeFromRight(8);
        help.setBounds(helpRow);
        if (takesPanel.isVisible()) takesPanel.setBounds(r.removeFromBottom(std::min(150, std::max(100, r.getHeight() / 3))));
        if (automationEditor.isVisible()) automationEditor.setBounds(r.removeFromBottom(std::min(180, std::max(80, r.getHeight() / 3))));
        if (clipBin.isVisible()) clipBin.setBounds(r.removeFromRight(ClipBin::width));
        auto bottom = r.removeFromBottom(scrollBarSize);
        hBar.setBounds(bottom.withTrimmedLeft(TimelineCanvas::headerWidth).withTrimmedRight(scrollBarSize));
        vBar.setBounds(r.removeFromRight(scrollBarSize).withTrimmedTop(TimelineCanvas::topHeight));
        canvas.setBounds(r);
        layoutScroll();
    }

private:
    enum ExportId { exportSong = 1, exportLoop, exportStems, exportLoopStems };
    void showExportMenu() {
        const auto& t = controller.timeline();
        juce::PopupMenu menu;
        menu.addItem(exportSong, "Whole song (" + juce::String(t.lengthBars) + " bars)", t.hasRegions());
        menu.addItem(exportLoop, "Loop range (bars " + juce::String(t.meterMap().barAt(static_cast<double>(t.loopStart)).index + 1) + " to "
                     + juce::String(t.meterMap().barsCovering(t.loopEnd)) + ")", t.hasRegions());
        menu.addSeparator();
        menu.addItem(exportStems, "Track and return stems - whole song", t.hasRegions());
        menu.addItem(exportLoopStems, "Track and return stems - loop range", t.hasRegions());
        juce::Component::SafePointer<TimelineView> safe(this);
        std::function<void(int)> done = [safe](int id) {
            if (safe == nullptr || id == 0) return;
            const auto& tl = safe->controller.timeline();
            if (id == exportStems || id == exportLoopStems) {
                if (safe->onExportStems) safe->onExportStems(id == exportStems ? 0 : tl.loopStart, id == exportStems ? -1 : tl.loopEnd);
            } else if (safe->onExport) {
                if (id == exportSong) safe->onExport(0, -1); else if (id == exportLoop) safe->onExport(tl.loopStart, tl.loopEnd);
            }
        };
        if (menuLauncher) { menuLauncher(std::move(menu), std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton), std::move(done));
    }
    void updateControls() {
        const auto& t = controller.timeline();
        loopButton.setToggleState(t.loopEnabled, juce::dontSendNotification);
        loopButton.setColour(juce::TextButton::buttonColourId, t.loopEnabled ? ui::colours::accent.withAlpha(0.35f) : ui::colours::control);
        const auto tick = canvas.cursorTick();
        const double seconds = controller.songTempo().secondsBetween(0.0, static_cast<double>(tick));
        const int minutes = static_cast<int>(seconds / 60.0);
        const auto bar = t.meterMap().barAt(static_cast<double>(tick));
        juce::String text = "Bar " + juce::String(bar.index + 1) + "." + juce::String((tick - bar.start) / bar.meter.beatTicks() + 1)
            + "  " + TimelineCanvas::meterText(bar.meter) + "  " + juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 2).paddedLeft('0', 5);
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
    RecordingTakesPanel takesPanel;
    juce::TextButton automationButton, clipsButton, takesButton;
    juce::Label snapLabel, lengthLabel, readout, help;
    juce::ComboBox snapBox;
    juce::TextButton zoomOutButton, zoomInButton, fitButton, loopButton, markerButton, placeButton, exportButton;
    juce::ToggleButton followButton;
    juce::Slider lengthSlider;
    juce::String defaultHelp;
    bool updating{}, exporting{};
};

} // namespace alien
