#pragma once
// The Channel Rack screen: one row per sound, a grid of steps for the selected
// pattern, and a graph editor for velocity, pan, pitch and probability. It is
// modelled on FL Studio's rack, in layout and in look: a light, two small knobs
// (pan, volume) and a raised name button with a grip on every row, a small
// "selected" lamp, and raised step buttons in groups of four that light up in
// the channel's colour. Click or drag to paint steps, right-drag to erase, click
// a name to hear it.
// All changes go through ProjectController::editRack, so undo, saving and the
// audio engine need nothing from this file.
#include "ProjectController.h"
#include "StudioWidgets.h"
#include <functional>
#include <optional>

namespace alien {

class ChannelRackGrid final : public juce::Component, public juce::TooltipClient {
public:
    static constexpr int rulerHeight = 20, minCell = 15, maxCell = 44, minRow = 28, maxRow = 38;
    // Where the header's parts are centred (x) or start (nameLeft), in pixels from the left edge. The name column
    // can be resized (drag its right edge), so the parts after it move with it and the steps start after them.
    static constexpr int ledX = 17, panX = 48, volumeX = 82, nameLeft = 104;
    static constexpr int soloAfterName = 16, routeAfterName = 44, lampAfterName = 71, headerAfterName = 86;
    static constexpr int defaultHeaderWidth = nameLeft + rack::defaultNameColumn + headerAfterName;   // 308
    // The edge that resizes the name column takes clicks this many pixels either side of it.
    static constexpr int nameEdgeGrab = 3;

    explicit ChannelRackGrid(ProjectController& projectController) : controller(projectController) {
        setName("Channel rack grid");
        setWantsKeyboardFocus(true);
    }

    std::function<void()> onSelectionChanged;    // another channel was selected
    std::function<void()> onStructureChanged;    // channels were added, removed or moved
    std::function<void(const juce::String&)> onMessage;
    std::function<void(int)> onScrolled;         // horizontal scroll position changed
    std::function<void(int)> onScrolledY;        // vertical scroll position changed
    std::function<void(int)> onOpenPianoRoll;    // "Open in piano roll" was chosen for a channel
    std::function<void()> onLayoutChanged;       // the name column was resized, so the steps start elsewhere
    // How pop-up menus and the rename box are shown. Left empty they use JUCE's
    // real windows; the tests set them to capture what would be shown and to
    // answer for the user (a real window needs a display).
    std::function<void(juce::PopupMenu, juce::Rectangle<int>, std::function<void(int)>)> menuLauncher;
    std::function<void(const juce::String&, const juce::String&, std::function<void(const juce::String&)>)> nameRequester;

    // ---- state ---------------------------------------------------------------
    // The selected channel is the one the graph editor shows. Several channels can be selected as in FL Studio
    // (Ctrl-click adds or removes one, Shift-click a range, Ctrl+A all); the selected channel is always one of
    // them, and the channel commands, knobs, mute and solo of any of them act on all of them as one undo step.
    // The selection belongs to the view, not the project.
    int selectedChannel() const noexcept { return selected; }
    void selectChannel(int channel) {
        const int next = clampRow(channel);
        const auto mask = bit(next);
        if (next == selected && marked == mask) return;
        selected = anchor = next; marked = mask; repaint();
        if (onSelectionChanged) onSelectionChanged();
    }
    // Shows `channel` in the graph editor, keeping the selection when it is part of it (a click on a step or a knob).
    void focusChannel(int channel) {
        if (!isChannelSelected(channel)) { selectChannel(channel); return; }
        if (channel == selected) return;
        selected = channel; repaint();
        if (onSelectionChanged) onSelectionChanged();
    }
    bool isChannelSelected(int row) const noexcept { return row >= 0 && row < project().channelCount && (marked & bit(row)) != 0; }
    std::vector<int> selectedChannels() const {
        std::vector<int> rows;
        for (int r = 0; r < project().channelCount; ++r) if (marked & bit(r)) rows.push_back(r);
        return rows;
    }
    // Ctrl-click: adds a channel to the selection (it becomes the one shown) or takes it out (unless it is the last).
    void toggleChannelSelection(int channel) {
        if (!project().validChannel(channel)) return;
        if (isChannelSelected(channel)) {
            if (marked == bit(channel)) { anchor = channel; return; }
            marked &= ~bit(channel);
            if (selected == channel) selected = selectedChannels().front();
        } else { marked |= bit(channel); selected = channel; }
        anchor = channel; repaint();
        if (onSelectionChanged) onSelectionChanged();
    }
    // Shift-click: every channel from the last one clicked to this one.
    void extendSelection(int channel) {
        if (!project().validChannel(channel)) return;
        const int from = clampRow(anchor);
        marked = 0;
        for (int r = juce::jmin(from, channel); r <= juce::jmax(from, channel); ++r) marked |= bit(r);
        selected = channel; repaint();
        if (onSelectionChanged) onSelectionChanged();
    }
    void selectAllChannels() {
        if (project().channelCount == 0) return;
        marked = 0;
        for (int r = 0; r < project().channelCount; ++r) marked |= bit(r);
        repaint();
        if (onSelectionChanged) onSelectionChanged();
    }
    // Called after the document changed behind the view's back (undo, open, new).
    void refresh() {
        selected = clampRow(selected);
        anchor = clampRow(anchor);
        marked &= project().channelCount >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << project().channelCount) - 1u;
        marked |= bit(selected);
        scrollX = juce::jlimit(0, maxScroll(), scrollX);
        scrollY = juce::jlimit(0, maxScrollY(), scrollY);
        repaint();
    }
    void setPlayhead(int step) {
        if (step == playhead) return;
        const int before = playhead;
        playhead = step;
        repaintColumn(before); repaintColumn(step);
    }
    int scrollPosition() const noexcept { return scrollX; }
    void setScrollPosition(int x) {
        x = juce::jlimit(0, maxScroll(), x);
        if (x == scrollX) return;
        scrollX = x; repaint();
        if (onScrolled) onScrolled(scrollX);
    }

    int scrollPositionY() const noexcept { return scrollY; }
    void setScrollPositionY(int y) {
        y = juce::jlimit(0, maxScrollY(), y);
        if (y == scrollY) return;
        scrollY = y; repaint();
        if (onScrolledY) onScrolledY(scrollY);
    }
    // Brings a row fully into view (after adding or selecting a channel far down the rack).
    void scrollRowIntoView(int row) {
        const int top = row * rowHeight(), bottom = top + rowHeight(), visible = visibleRowsHeight();
        if (top < scrollY) setScrollPositionY(top);
        else if (bottom > scrollY + visible) setScrollPositionY(bottom - visible);
    }

    // ---- geometry (public so the graph editor and the tests share it) ------------
    const rack::Project& project() const noexcept { return controller.channelRack(); }
    int length() const { return project().pattern(project().selectedPattern).length; }
    int nameWidth() const noexcept { return project().nameColumnWidth; }
    int nameRight() const noexcept { return nameLeft + nameWidth(); }
    int soloX() const noexcept { return nameRight() + soloAfterName; }
    int routeX() const noexcept { return nameRight() + routeAfterName; }
    int lampX() const noexcept { return nameRight() + lampAfterName; }
    // Where the steps start: everything left of this is the channel header.
    int headerWidth() const noexcept { return nameRight() + headerAfterName; }
    int stepAreaWidth() const noexcept { return juce::jmax(0, getWidth() - headerWidth()); }
    int cellWidth() const { return juce::jlimit(minCell, maxCell, stepAreaWidth() / juce::jmax(1, length())); }
    int contentWidth() const { return cellWidth() * length(); }
    int maxScroll() const { return juce::jmax(0, contentWidth() - stepAreaWidth()); }
    int rowHeight() const {
        const int rows = juce::jmax(4, project().channelCount);
        return juce::jlimit(minRow, maxRow, (getHeight() - rulerHeight) / rows);
    }
    // The rows scroll under the ruler when 16 channels do not fit the window.
    int visibleRowsHeight() const noexcept { return juce::jmax(0, getHeight() - rulerHeight); }
    int contentHeight() const { return project().channelCount * rowHeight(); }
    int maxScrollY() const { return juce::jmax(0, contentHeight() - visibleRowsHeight()); }
    juce::Rectangle<int> rowBounds(int row) const { return {0, rulerHeight + row * rowHeight() - scrollY, getWidth(), rowHeight()}; }
    juce::Rectangle<int> cellBounds(int row, int step) const {
        return {headerWidth() + step * cellWidth() - scrollX, rulerHeight + row * rowHeight() - scrollY, cellWidth(), rowHeight()};
    }
    // Where a step starts and how wide it is, in this component's coordinates.
    int stepLeft(int step) const { return headerWidth() + step * cellWidth() - scrollX; }
    // The header, left to right as in FL Studio's rack: the light (mute), pan and volume knobs, the channel's name
    // button, solo and output, and the lamp that shows which channel is selected.
    juce::Rectangle<int> muteRect(int row) const { return juce::Rectangle<int>(0, 0, 16, 16).withCentre({ledX, rowBounds(row).getCentreY()}); }
    juce::Rectangle<int> nameRect(int row) const { auto r = rowBounds(row); return {nameLeft, r.getY() + 3, nameWidth(), r.getHeight() - 6}; }
    juce::Rectangle<int> panRect(int row) const { return knobRect(row, panX); }
    juce::Rectangle<int> volumeRect(int row) const { return knobRect(row, volumeX); }
    juce::Rectangle<int> soloRect(int row) const { auto r = rowBounds(row); return juce::Rectangle<int>(0, 0, 24, 18).withCentre({soloX(), r.getCentreY()}); }
    juce::Rectangle<int> routeRect(int row) const { auto r = rowBounds(row); return juce::Rectangle<int>(0, 0, 28, 18).withCentre({routeX(), r.getCentreY()}); }
    juce::Rectangle<int> lampRect(int row) const { return juce::Rectangle<int>(0, 0, 8, 20).withCentre({lampX(), rowBounds(row).getCentreY()}); }
    // The strip along the name column's right edge, from the ruler down past the last row, that resizes the column.
    bool onNameEdge(juce::Point<int> p) const {
        const int rowsBottom = juce::jmin(getHeight(), rulerHeight + contentHeight() - scrollY);
        return std::abs(p.x - nameRight()) <= nameEdgeGrab && p.y >= 0 && p.y < juce::jmax(rulerHeight, rowsBottom);
    }
    // Wide enough for the longest channel name (with the button's padding and grip), within the allowed range.
    int fittedNameWidth() const {
        const auto font = ui::font(13.0f, true);
        float widest = 0.0f;
        for (int r = 0; r < project().channelCount; ++r) {
            const auto& name = project().info[static_cast<std::size_t>(r)].name;
            widest = juce::jmax(widest, juce::GlyphArrangement::getStringWidth(font, juce::String::fromUTF8(name.c_str(), static_cast<int>(name.size()))));
        }
        return juce::jlimit(rack::minNameColumn, rack::maxNameColumn, static_cast<int>(std::ceil(widest)) + 8 + 20 + 4);
    }
    // Resizes the name column (clamped); the steps move to make room. Not an undo step.
    bool setNameColumnWidth(int width) {
        if (!controller.setRackNameColumnWidth(width)) return false;
        scrollX = juce::jlimit(0, maxScroll(), scrollX);
        repaint();
        if (onLayoutChanged) onLayoutChanged();
        return true;
    }
    int rowAt(int y) const {
        if (y < rulerHeight) return -1;
        const int row = (y - rulerHeight + scrollY) / rowHeight();
        return row < project().channelCount ? row : -1;
    }
    // The step under x, or -1 (outside the pattern).
    int stepAtX(int x) const {
        if (x < headerWidth()) return -1;
        const int step = (x - headerWidth() + scrollX) / cellWidth();
        return step < length() ? step : -1;
    }

    // ---- editing (the mouse handlers call these; tests may too) -------------------
    bool setStep(int row, int step, bool on, const juce::String& key) {
        return controller.editRack(on ? "Add steps" : "Remove steps", [&](rack::Project& p) {
            return p.setStepOn(p.selectedPattern, row, step, on); }, key);
    }
    bool changeChannel(int row, const juce::String& label, const std::function<void(rack::Channel&)>& change,
                       const juce::String& key = {}) {
        return controller.editRack(label, [&](rack::Project& p) {
            if (!p.validChannel(row)) return false;
            auto channel = p.channels[static_cast<std::size_t>(row)];
            change(channel);
            return p.setChannel(row, channel);
        }, key);
    }
    // The channels an edit of `row` applies to: the whole selection when `row` is part of it, else just `row`.
    std::vector<int> targetsOf(int row) const {
        if (isChannelSelected(row)) return selectedChannels();
        return project().validChannel(row) ? std::vector<int>{row} : std::vector<int>{};
    }
    // `change` applied to each of `rows`, as one undo step.
    bool changeChannels(const std::vector<int>& rows, const juce::String& label, const std::function<void(int, rack::Channel&)>& change,
                        const juce::String& key = {}) {
        return controller.editRack(label, [&](rack::Project& p) {
            bool any = false;
            for (const int row : rows) {
                if (!p.validChannel(row)) continue;
                auto channel = p.channels[static_cast<std::size_t>(row)];
                change(row, channel);
                any = p.setChannel(row, channel) || any;
            }
            return any;
        }, key);
    }
    // Every target takes the clicked channel's new state, so a mixed selection ends up all muted (or all not).
    void toggleMute(int row) {
        if (!project().validChannel(row)) return;
        const bool on = !project().channels[static_cast<std::size_t>(row)].mute;
        const auto rows = targetsOf(row);
        changeChannels(rows, plural("Mute channel", rows), [on](int, rack::Channel& c) { c.mute = on; }); repaint();
    }
    void toggleSolo(int row) {
        if (!project().validChannel(row)) return;
        const bool on = !project().channels[static_cast<std::size_t>(row)].solo;
        const auto rows = targetsOf(row);
        changeChannels(rows, plural("Solo channel", rows), [on](int, rack::Channel& c) { c.solo = on; }); repaint();
    }
    static juce::String plural(const juce::String& label, const std::vector<int>& rows) {
        return rows.size() > 1 ? label + "s" : label;
    }
    void preview(int row, float velocity = 1.0f) { controller.previewRackChannel(row, velocity); }
    bool sampleOffline(int row) const { return controller.rackChannelOffline(row); }

    // A paint stroke: press on a cell, drag across others. Everything painted in
    // one stroke is a single undo step. A press on an "on" cell erases, on an
    // "off" cell paints; right-button strokes always erase.
    void beginStroke(int row, int step, bool eraseOnly) {
        if (!project().validStep(project().selectedPattern, row, step) || step >= length()) return;
        if (project().isNoteChannel(project().selectedPattern, row)) {
            focusChannel(row);
            if (onOpenPianoRoll) onOpenPianoRoll(row);
            return;
        }
        strokeKey = "rack-stroke-" + juce::String(++strokeCounter);
        paintValue = eraseOnly ? false : !project().pattern(project().selectedPattern).steps[static_cast<std::size_t>(row)][static_cast<std::size_t>(step)].on;
        drag = Drag::steps; lastRow = row; lastStep = step;
        focusChannel(row);
        setStep(row, step, paintValue, strokeKey);
        if (paintValue) preview(row, velocityOf(row, step));
        repaintCell(row, step);
    }
    void continueStroke(int row, int step) {
        if (drag != Drag::steps || project().channelCount == 0) return;
        row = juce::jlimit(0, juce::jmax(0, project().channelCount - 1), row);
        step = juce::jlimit(0, length() - 1, step);
        // Walk the straight line from the last cell so a fast drag leaves no gaps.
        const int dr = row - lastRow, ds = step - lastStep;
        const int n = juce::jmax(std::abs(dr), std::abs(ds));
        for (int i = 1; i <= n; ++i) {
            const int r = lastRow + (dr * i + (dr >= 0 ? n / 2 : -n / 2)) / n;
            const int s = lastStep + (ds * i + (ds >= 0 ? n / 2 : -n / 2)) / n;
            if (setStep(r, s, paintValue, strokeKey)) repaintCell(r, s);
        }
        lastRow = row; lastStep = step;
    }
    void endStroke() { if (drag == Drag::steps) drag = Drag::none; }

    // ---- menus ---------------------------------------------------------------
    enum MenuId { cmdRename = 1, cmdMute, cmdSolo, cmdClearSteps, cmdMoveUp, cmdMoveDown, cmdDelete, cmdSilent, cmdLoadSample,
                  cmdDuplicate = 100, cmdCopyRow, cmdPasteRow, cmdFill2, cmdFill4, cmdFill8,
                  cmdRotateLeft, cmdRotateRight, cmdReverseRow, cmdRepeat4, cmdRepeat8, cmdRepeat16, cmdPianoRoll,
                  synthBase = 200, builtinBase = 20, outputMaster = 40, outputBase = 41, chokeNone = 60, chokeBase = 61 };

    juce::PopupMenu channelMenu(int row) const {
        juce::PopupMenu menu;
        if (!project().validChannel(row)) return menu;
        const auto& channel = project().channels[static_cast<std::size_t>(row)];
        const auto& info = project().info[static_cast<std::size_t>(row)];
        menu.addItem(cmdPianoRoll, "Open in piano roll", onOpenPianoRoll != nullptr);
        menu.addItem(cmdRename, "Rename...");
        menu.addSeparator();
        juce::PopupMenu sound;
        for (std::size_t i = 0; i < rack::builtinSounds.size(); ++i)
            sound.addItem(builtinBase + static_cast<int>(i), rack::builtinSounds[i].name, true, info.sample == std::string("builtin:") + rack::builtinSounds[i].id);
        juce::PopupMenu synths;
        for (int preset = 0; preset < alien::presetCount; ++preset)
            synths.addItem(synthBase + preset, alien::presetNames[static_cast<std::size_t>(preset)], true, channel.instrumentPreset == preset);
        sound.addSubMenu("Built-in synth", synths);
        sound.addSeparator();
        sound.addItem(cmdLoadSample, "Load sample file...");
        sound.addItem(cmdSilent, "Silent (no sound)", true, info.sample.empty() && !channel.isSynth());
        menu.addSubMenu("Sound", sound);
        juce::PopupMenu output;
        output.addItem(outputMaster, "Master", true, channel.output == rack::masterOutput);
        const auto& doc = controller.document();
        for (int t = 0; t < doc.trackCount(); ++t)
            output.addItem(outputBase + t, "Track " + juce::String(t + 1) + " - " + doc.tracks[static_cast<std::size_t>(t)].name, true, channel.output == t);
        menu.addSubMenu("Output", output);
        juce::PopupMenu choke;
        choke.addItem(chokeNone, "None", true, channel.chokeGroup == 0);
        for (int g = 1; g <= rack::maxChokeGroup; ++g) choke.addItem(chokeBase + g - 1, "Group " + juce::String(g), true, channel.chokeGroup == g);
        menu.addSubMenu("Choke group (cuts other sounds in the group)", choke);
        menu.addSeparator();
        const auto rows = targetsOf(row);
        const int n = static_cast<int>(rows.size());
        menu.addItem(cmdMute, "Mute", true, channel.mute);
        menu.addItem(cmdSolo, "Solo", true, channel.solo);
        menu.addItem(cmdDuplicate, n > 1 ? "Duplicate " + juce::String(n) + " channels" : juce::String("Duplicate channel"),
                     project().channelCount + n <= rack::maxChannels);
        const bool stepsOnly = std::none_of(rows.begin(), rows.end(), [&](int r) { return project().isNoteChannel(project().selectedPattern, r); });
        menu.addItem(cmdCopyRow, "Copy steps", stepsOnly); menu.addItem(cmdPasteRow, "Paste steps", stepsOnly && rowClipboard.has_value());
        juce::PopupMenu tools;
        tools.addItem(cmdFill2, "Fill every 2 steps"); tools.addItem(cmdFill4, "Fill every 4 steps"); tools.addItem(cmdFill8, "Fill every 8 steps");
        tools.addSeparator(); tools.addItem(cmdRotateLeft, "Rotate left"); tools.addItem(cmdRotateRight, "Rotate right");
        tools.addItem(cmdReverseRow, "Reverse steps");
        tools.addItem(cmdRepeat4, "Repeat first 4 steps"); tools.addItem(cmdRepeat8, "Repeat first 8 steps");
        tools.addItem(cmdRepeat16, "Repeat first 16 steps", length() > 16);
        menu.addSubMenu("Pattern tools", tools, stepsOnly);
        menu.addItem(cmdClearSteps, n > 1 ? "Clear notes and steps of " + juce::String(n) + " channels" : juce::String("Clear this channel (notes and steps)"));
        menu.addSeparator();
        menu.addItem(cmdMoveUp, "Move up", rows.front() > 0);
        menu.addItem(cmdMoveDown, "Move down", rows.back() + 1 < project().channelCount);
        menu.addItem(cmdDelete, n > 1 ? "Delete " + juce::String(n) + " channels" : juce::String("Delete channel"), project().channelCount > 0);
        return menu;
    }
    juce::PopupMenu outputMenu(int row) const {
        juce::PopupMenu menu;
        if (!project().validChannel(row)) return menu;
        const auto& channel = project().channels[static_cast<std::size_t>(row)];
        menu.addSectionHeader("Send this channel to");
        menu.addItem(outputMaster, "Master", true, channel.output == rack::masterOutput);
        const auto& doc = controller.document();
        for (int t = 0; t < doc.trackCount(); ++t)
            menu.addItem(outputBase + t, "Track " + juce::String(t + 1) + " - " + doc.tracks[static_cast<std::size_t>(t)].name, true, channel.output == t);
        return menu;
    }
    void showChannelMenu(int row) { launchMenu(channelMenu(row), nameRect(row), row); }
    void showOutputMenu(int row) { launchMenu(outputMenu(row), routeRect(row), row); }
    // Runs a command of `row`'s menu. When `row` is one of several selected channels the command acts on all of
    // them as one undo step, except Rename, Load sample and Copy steps, which only make sense for one.
    void runChannelCommand(int row, int id) {
        if (!project().validChannel(row)) return;
        if (id == cmdPianoRoll) { selectChannel(row); if (onOpenPianoRoll) onOpenPianoRoll(row); return; }
        const auto rows = targetsOf(row);
        // Each target in turn, as one undo step: true when anything changed.
        const auto forEach = [&](const juce::String& label, const std::function<bool(rack::Project&, int)>& edit) {
            return controller.editRack(label, [&](rack::Project& p) {
                bool any = false;
                for (const int r : rows) any = edit(p, r) || any;
                return any;
            });
        };
        if (id == cmdCopyRow && project().isNoteChannel(project().selectedPattern, row)) return;
        if (id == cmdCopyRow) { rowClipboard = project().pattern(project().selectedPattern).steps[static_cast<std::size_t>(row)]; return; }
        if (id == cmdPasteRow && rowClipboard) {
            const auto copied = *rowClipboard;
            forEach("Paste steps", [&](rack::Project& p, int r) { return p.pasteRow(p.selectedPattern, r, copied); }); changed(); return;
        }
        if (id == cmdDuplicate) {
            // From the bottom up, so each copy lands under its original; the copies become the selection.
            const int n = static_cast<int>(rows.size());
            if (project().channelCount + n > rack::maxChannels) return;
            const bool done = controller.editRack(plural("Duplicate channel", rows), [&](rack::Project& p) {
                for (auto r = rows.rbegin(); r != rows.rend(); ++r) if (!p.duplicateChannel(*r)) return false;
                return true;
            });
            if (done) {
                marked = 0;
                for (int i = 0; i < n; ++i) marked |= bit(rows[static_cast<std::size_t>(i)] + i + 1);
                const auto at = std::find(rows.begin(), rows.end(), row) - rows.begin();
                selected = anchor = row + static_cast<int>(at) + 1;
            }
            changed(); return;
        }
        if (id >= cmdFill2 && id <= cmdRepeat16) {
            forEach("Edit channel steps", [=](rack::Project& p, int r) {
                if (id == cmdFill2 || id == cmdFill4 || id == cmdFill8) return p.fillEvery(p.selectedPattern, r, id == cmdFill2 ? 2 : id == cmdFill4 ? 4 : 8);
                if (id == cmdRotateLeft || id == cmdRotateRight) return p.rotateRow(p.selectedPattern, r, id == cmdRotateLeft ? -1 : 1);
                if (id == cmdReverseRow) return p.reverseRow(p.selectedPattern, r);
                return p.repeatRow(p.selectedPattern, r, id == cmdRepeat4 ? 4 : id == cmdRepeat8 ? 8 : 16);
            }); changed(); return;
        }
        if (id == cmdRename) { askForName("Rename channel", project().info[static_cast<std::size_t>(row)].name,
                                       [this, row](const juce::String& name) {
                                           controller.editRack("Rename channel", [&](rack::Project& p) { return p.renameChannel(row, name.toStdString()); });
                                           repaint(); if (onStructureChanged) onStructureChanged(); });
        } else if (id == cmdMute) toggleMute(row);
        else if (id == cmdSolo) toggleSolo(row);
        else if (id == cmdClearSteps) {
            forEach(plural("Clear channel", rows), [](rack::Project& p, int r) { return p.clearChannelSteps(p.selectedPattern, r); }); repaint();
        } else if (id == cmdMoveUp || id == cmdMoveDown) {
            // The selected channels move together, keeping their order and the gaps between them.
            const int delta = id == cmdMoveUp ? -1 : 1;
            if ((delta < 0 && rows.front() == 0) || (delta > 0 && rows.back() + 1 >= project().channelCount)) return;
            const bool done = controller.editRack(plural("Move channel", rows), [&](rack::Project& p) {
                if (delta < 0) { for (const int r : rows) if (!p.moveChannel(r, -1)) return false; }
                else for (auto r = rows.rbegin(); r != rows.rend(); ++r) if (!p.moveChannel(*r, 1)) return false;
                return true;
            });
            if (done && isChannelSelected(row)) {
                marked = delta < 0 ? marked >> 1 : marked << 1;
                selected = row + delta; anchor = clampRow(anchor + delta);
            } else if (done) selectChannel(row + delta);
            changed();
        } else if (id == cmdDelete) {
            const bool done = controller.editRack(plural("Delete channel", rows), [&](rack::Project& p) {
                for (auto r = rows.rbegin(); r != rows.rend(); ++r) if (!p.removeChannel(*r)) return false;
                return true;
            });
            if (done) {
                // Deleting another channel keeps the selected one; deleting the selection selects the channel
                // that took the place of the first one deleted.
                const bool other = rows.size() == 1 && rows.front() != selected;
                const int next = other ? selected - (rows.front() < selected ? 1 : 0) : rows.front();
                selected = -1; marked = 0;
                selectChannel(next);
            }
            changed();
        } else if (id == cmdSilent) { forEach(plural("Silence channel", rows), [](rack::Project& p, int r) { return p.setSample(r, ""); }); changed(); }
        else if (id == cmdLoadSample) chooseSample(row);
        else if (id >= synthBase && id < synthBase + alien::presetCount) {
            const int preset = id - synthBase;
            forEach("Change channel instrument", [&](rack::Project& p, int r) {
                const bool done = p.setInstrumentPreset(r, preset);
                if (done) p.renameChannel(r, alien::presetNames[static_cast<std::size_t>(preset)]);
                return done;
            }); changed(); preview(row);
        }
        else if (id >= builtinBase && id < builtinBase + static_cast<int>(rack::builtinSounds.size())) {
            const auto& sound = rack::builtinSounds[static_cast<std::size_t>(id - builtinBase)];
            if (rows.size() == 1) controller.setRackBuiltin(row, sound.id);
            else forEach("Change channel sounds", [&](rack::Project& p, int r) {
                const bool changedSound = p.setSample(r, std::string("builtin:") + sound.id);
                return p.renameChannel(r, sound.name) || changedSound;
            });
            changed(); preview(row);
        } else if (id == outputMaster || (id >= outputBase && id < outputBase + rack::maxOutputTrack + 1)) {
            const int output = id == outputMaster ? rack::masterOutput : id - outputBase;
            changeChannels(rows, plural("Route channel", rows), [output](int, rack::Channel& c) { c.output = static_cast<std::int8_t>(output); });
            repaint();
        } else if (id == chokeNone || (id >= chokeBase && id < chokeBase + rack::maxChokeGroup)) {
            const int group = id == chokeNone ? 0 : id - chokeBase + 1;
            changeChannels(rows, "Change choke group", [group](int, rack::Channel& c) { c.chokeGroup = static_cast<std::uint8_t>(group); });
            repaint();
        }
    }
    void addChannelWithSound(const juce::String& soundId) {
        if (project().channelCount >= rack::maxChannels) return;
        const auto* name = rack::builtinDisplayName(soundId.toStdString());
        if (name == nullptr) return;
        if (controller.editRack("Add channel", [&](rack::Project& p) { return p.addChannel(name, "builtin:" + soundId.toStdString()); })) {
            selected = anchor = project().channelCount - 1; marked = bit(selected);
            preview(selected);
        }
        changed();
    }
    void addSynthChannel(int preset) {
        if (project().channelCount >= rack::maxChannels || preset < 0 || preset >= alien::presetCount) return;
        if (controller.editRack("Add synth channel", [&](rack::Project& p) {
            if (!p.addChannel(alien::presetNames[static_cast<std::size_t>(preset)], "")) return false;
            const int channel = p.channelCount - 1;
            p.setInstrumentPreset(channel, preset);
            PianoRollPattern notes; notes.loopSteps = p.pattern(p.selectedPattern).length;
            return p.setChannelNotes(p.selectedPattern, channel, notes);
        })) {
            selected = anchor = project().channelCount - 1; marked = bit(selected);
            changed();
            if (onOpenPianoRoll) onOpenPianoRoll(selected);
        }
    }
    void addChannelWithFile() {
        if (project().channelCount >= rack::maxChannels) return;
        if (controller.editRack("Add channel", [&](rack::Project& p) { return p.addChannel("Sampler", ""); })) {
            selected = anchor = project().channelCount - 1; marked = bit(selected);
            changed();
            chooseSample(selected);
        }
    }
    void chooseSample(int row) {
        chooser = std::make_unique<juce::FileChooser>("Load a sample into this channel", juce::File{}, "*.wav;*.mp3");
        juce::Component::SafePointer<ChannelRackGrid> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe, row](const juce::FileChooser& result) {
                if (safe != nullptr && result.getResult().existsAsFile()) safe->controller.loadRackSample(result.getResult(), row);
            });
    }
    void askForName(const juce::String& title, const juce::String& current, std::function<void(const juce::String&)> done) {
        if (nameRequester) { nameRequester(title, current, std::move(done)); return; }
        auto* window = new juce::AlertWindow(title, "", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", current);
        window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ChannelRackGrid> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window, done = std::move(done)](int result) {
            // The window still exists while its callback runs; the grid may not.
            if (safe != nullptr && result == 1) done(window->getTextEditorContents("name").trim());
        }), true);
    }

    // ---- painting ------------------------------------------------------------------
    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::surface);
        const auto& p = project();
        const auto& pattern = p.pattern(p.selectedPattern);
        const int cell = cellWidth(), rows = p.channelCount, rh = rowHeight();
        const int len = pattern.length;

        // Persistent column labels make the compact channel controls discoverable.
        g.setColour(col::textDim); g.setFont(ui::font(9.0f, true));
        g.drawText("CHANNEL", nameLeft, 0, nameWidth(), rulerHeight, juce::Justification::centred);
        g.drawText("PAN", panX - 14, 0, 28, rulerHeight, juce::Justification::centred);
        g.drawText("VOL", volumeX - 14, 0, 28, rulerHeight, juce::Justification::centred);
        g.drawText("SOLO", soloX() - 18, 0, 36, rulerHeight, juce::Justification::centred);
        g.drawText("OUT", routeX() - 14, 0, 28, rulerHeight, juce::Justification::centred);
        // Ruler: a number at every beat, a stronger line at every bar.
        g.setColour(col::raised);
        g.fillRect(juce::Rectangle<int>(headerWidth(), 0, getWidth() - headerWidth(), rulerHeight));
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(headerWidth(), 0, getWidth() - headerWidth(), getHeight());
            g.setFont(ui::monoFont(10.5f));
            for (int s = 0; s < len; ++s) {
                const int x = stepLeft(s);
                if (x + cell < headerWidth() || x > getWidth()) continue;
                if (s % 4 == 0) {
                    g.setColour(s % 16 == 0 ? col::textDim : col::textFaint);
                    g.drawText(juce::String(s / 4 + 1), x + 3, 0, cell * 4 - 3, rulerHeight, juce::Justification::centredLeft, false);
                }
                if (s == playhead) { g.setColour(col::warning.withAlpha(0.9f)); g.fillRect(x + 1, rulerHeight - 3, cell - 2, 3); }
            }
        }
        // Rows (scrolled; the ruler stays put).
        juce::Graphics::ScopedSaveState rowsState(g);
        g.reduceClipRegion(0, rulerHeight, getWidth(), juce::jmax(0, getHeight() - rulerHeight));
        for (int row = 0; row < rows; ++row) {
            const auto bounds = rowBounds(row);
            if (bounds.getBottom() <= rulerHeight || bounds.getY() >= getHeight()) continue;
            const auto& channel = p.channels[static_cast<std::size_t>(row)];
            const bool audible = p.audible(row);
            const auto colour = ui::trackColour(row);
            g.setColour(row == selected ? col::raised : isChannelSelected(row) ? col::raised.withAlpha(0.7f)
                                                       : (row % 2 == 0 ? col::surface : col::raised.withAlpha(0.35f)));
            g.fillRect(bounds);
            if (row == selected) { g.setColour(colour); g.fillRect(0, bounds.getY(), 3, bounds.getHeight()); }
            g.setColour(col::line);
            g.drawHorizontalLine(bounds.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
            paintHeader(g, row, channel, audible, colour);
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(headerWidth(), bounds.getY(), getWidth() - headerWidth(), rh);
            if (const auto& sequence = pattern.notes[static_cast<std::size_t>(row)]) {
                int low = 127, high = 0;
                for (std::size_t n = 0; n < sequence->count; ++n) { low = std::min(low, sequence->notes[n].pitch); high = std::max(high, sequence->notes[n].pitch); }
                g.setColour(colour.withAlpha(audible ? 0.9f : 0.35f));
                for (std::size_t n = 0; n < sequence->count; ++n) {
                    const auto& note = sequence->notes[n];
                    const float x = static_cast<float>(stepLeft(0) + note.startStep * cellWidth());
                    const float y = static_cast<float>(bounds.getBottom() - 6)
                        - static_cast<float>(note.pitch - low) / static_cast<float>(std::max(1, high - low)) * static_cast<float>(rh - 14);
                    g.fillRoundedRectangle(x, y, std::max(2.0f, static_cast<float>(note.lengthSteps * cellWidth()) - 1), 3.0f, 1.0f);
                }
                if (sequence->count == 0) g.drawText("Piano roll (empty) - click to edit", bounds.withTrimmedLeft(headerWidth() + 8), juce::Justification::centredLeft);
                continue;
            }
            for (int s = 0; s < len; ++s) {
                const auto r = cellBounds(row, s);
                if (r.getRight() < headerWidth() || r.getX() > getWidth()) continue;
                paintCell(g, r.toFloat().withTrimmedLeft(s % 4 == 0 ? 3.5f : 1.5f).withTrimmedRight(1.5f).reduced(0.0f, 3.5f), pattern.steps[static_cast<std::size_t>(row)][static_cast<std::size_t>(s)],
                          s, colour, audible, s == playhead);
            }
        }
        if (rows == 0) {
            g.setColour(col::textFaint);
            g.setFont(ui::font(14.0f));
            g.drawText("This rack has no channels. Use + Channel to add one.", getLocalBounds().withTrimmedTop(rulerHeight), juce::Justification::centred);
        }
        // The pattern ends here: gap past the last step is the rack's background.
        g.setColour(col::line);
        g.drawVerticalLine(headerWidth() - 1, static_cast<float>(rulerHeight), static_cast<float>(getHeight()));
        if (drag == Drag::knob) paintKnobReadout(g);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        const auto mods = key.getModifiers();
        if (!mods.isCommandDown() || mods.isAltDown()) return false;
        const auto code = key.getKeyCode();
        if (code == 'C' || code == 'c') { runChannelCommand(selected, cmdCopyRow); return true; }
        if (code == 'V' || code == 'v') { runChannelCommand(selected, cmdPasteRow); return true; }
        if (code == 'D' || code == 'd') { runChannelCommand(selected, cmdDuplicate); return true; }
        if (code == 'P' || code == 'p') { runChannelCommand(selected, cmdPianoRoll); return true; }
        if (code == 'A' || code == 'a') { selectAllChannels(); return true; }
        return false;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        const auto point = e.getPosition();
        drag = Drag::none;
        if (onNameEdge(point) && !e.mods.isPopupMenu()) {
            if (e.getNumberOfClicks() >= 2) { setNameColumnWidth(fittedNameWidth()); return; }
            drag = Drag::nameColumn; nameStartWidth = nameWidth(); nameStartX = point.x;
            return;
        }
        const int row = rowAt(point.y);
        if (row < 0) return;
        if (point.x < headerWidth()) {
            // Ctrl-click and Shift-click on the name or the lamp change which channels are selected.
            const bool selecting = nameRect(row).contains(point) || lampRect(row).expanded(3).contains(point);
            if (selecting && !e.mods.isPopupMenu() && e.mods.isCommandDown()) { toggleChannelSelection(row); return; }
            if (selecting && !e.mods.isPopupMenu() && e.mods.isShiftDown()) { extendSelection(row); return; }
            // The controls of a selected channel act on the whole selection, so they keep it.
            const bool control = muteRect(row).expanded(4).contains(point) || soloRect(row).contains(point) || routeRect(row).contains(point)
                || panRect(row).contains(point) || volumeRect(row).contains(point) || e.mods.isPopupMenu();
            if (control) focusChannel(row); else selectChannel(row);
            if (muteRect(row).expanded(4).contains(point)) { if (e.mods.isPopupMenu()) toggleSolo(row); else toggleMute(row); return; }
            if (soloRect(row).contains(point)) { toggleSolo(row); return; }
            if (routeRect(row).contains(point)) { showOutputMenu(row); return; }
            if (panRect(row).contains(point) || volumeRect(row).contains(point)) {
                const bool pan = panRect(row).contains(point);
                if (e.getNumberOfClicks() >= 2) { resetKnob(row, pan); return; }
                if (e.mods.isPopupMenu()) { showChannelMenu(row); return; }
                beginKnob(row, pan, point.y);
                return;
            }
            if (e.mods.isPopupMenu()) { showChannelMenu(row); return; }
            if (e.getNumberOfClicks() >= 2) { runChannelCommand(row, cmdRename); return; }
            if (nameRect(row).contains(point)) preview(row);
            return;
        }
        const int step = stepAtX(point.x);
        if (step >= 0) beginStroke(row, step, e.mods.isPopupMenu());
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (drag == Drag::steps) {
            const auto point = e.getPosition();
            const int row = point.y < rulerHeight ? 0 : (point.y - rulerHeight + scrollY) / rowHeight();
            int step = (point.x - headerWidth() + scrollX) / cellWidth();
            if (point.x < headerWidth()) step = 0;
            continueStroke(row, step);
        } else if (drag == Drag::knob) {
            dragKnob(e.getPosition().y, e.mods.isShiftDown());
        } else if (drag == Drag::nameColumn) {
            setNameColumnWidth(nameStartWidth + e.getPosition().x - nameStartX);
        }
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (drag == Drag::knob) repaint();
        drag = Drag::none;
    }
    void mouseMove(const juce::MouseEvent& e) override {
        hover = e.getPosition();
        setMouseCursor(onNameEdge(hover) ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit(const juce::MouseEvent&) override { hover = {-1, -1}; }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const int row = rowAt(e.getPosition().y);
        if (row >= 0 && e.getPosition().x < headerWidth() && (panRect(row).contains(e.getPosition()) || volumeRect(row).contains(e.getPosition()))) {
            const bool pan = panRect(row).contains(e.getPosition());
            nudgeKnob(row, pan, wheel.deltaY * (pan ? 0.4f : 0.25f));
            return;
        }
        const float amount = wheel.deltaX != 0.0f ? wheel.deltaX : (e.mods.isShiftDown() ? wheel.deltaY : 0.0f);
        if (amount != 0.0f && maxScroll() > 0) setScrollPosition(scrollX - static_cast<int>(amount * 400.0f));
        else if (amount == 0.0f && wheel.deltaY != 0.0f && maxScrollY() > 0) setScrollPositionY(scrollY - static_cast<int>(wheel.deltaY * 400.0f));
        else juce::Component::mouseWheelMove(e, wheel);
    }
    void resized() override {
        scrollX = juce::jlimit(0, maxScroll(), scrollX);
        scrollY = juce::jlimit(0, maxScrollY(), scrollY);
    }

    juce::String getTooltip() override {
        const int row = rowAt(hover.y);
        if (onNameEdge(hover)) return "Drag to resize the name column; double-click to fit the longest name";
        if (row < 0) return hover.y >= 0 && hover.y < rulerHeight && hover.x >= headerWidth() ? juce::String("Step numbers: one per beat") : juce::String();
        if (hover.x >= headerWidth()) {
            if (project().isNoteChannel(project().selectedPattern, row)) return "Piano-roll notes: click to edit lengths, chords and timing. Clear channel returns this row to steps.";
            const int step = stepAtX(hover.x);
            if (step < 0) return {};
            const auto& s = project().pattern(project().selectedPattern).steps[static_cast<std::size_t>(row)][static_cast<std::size_t>(step)];
            return s.on ? "Step " + juce::String(step + 1) + ": velocity " + juce::String(s.velocity) + ", pan " + panText(s.pan)
                          + ", pitch " + juce::String(s.pitch) + ", chance " + juce::String(s.probability) + "%. Right-drag to erase."
                        : "Click or drag to paint steps; right-drag to erase";
        }
        if (muteRect(row).expanded(4).contains(hover)) return "Mute (left click) or solo (right click) this channel";
        if (soloRect(row).contains(hover)) return "Solo this channel";
        if (routeRect(row).contains(hover)) return "Output: click to send this channel to the master or to a Session track";
        if (panRect(row).contains(hover)) return "Channel pan: drag up or down, double-click to centre";
        if (volumeRect(row).contains(hover)) return "Channel volume: drag up or down, double-click to reset";
        if (lampRect(row).expanded(3).contains(hover))
            return "Lit for the selected channels. Ctrl-click to add or remove one, Shift-click for a range, Ctrl+A for all; the graph editor edits the one marked in the row's colour";
        if (sampleOffline(row)) return "The sample file " + sampleFileName(row) + " is missing. Use Relink audio on the Session tab, or right-click to load another.";
        return soundDescription(row) + ". Click to hear and select this sound, Ctrl-click or Shift-click to select several. Double-click to rename, right-click for more.";
    }

private:
    enum class Drag { none, steps, knob, nameColumn };

    // Shows `menu` next to `area` and runs the chosen command on `row`.
    void launchMenu(juce::PopupMenu menu, juce::Rectangle<int> area, int row) {
        juce::Component::SafePointer<ChannelRackGrid> safe(this);
        std::function<void(int)> done = [safe, row](int id) { if (safe != nullptr && id != 0) safe->runChannelCommand(row, id); };
        if (menuLauncher) { menuLauncher(std::move(menu), area, std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(area)), std::move(done));
    }

    juce::Rectangle<int> knobRect(int row, int centreX) const {
        return juce::Rectangle<int>(0, 0, 28, 28).withCentre({centreX, rowBounds(row).getCentreY()});
    }
    static juce::String panText(int pan) { return pan == 0 ? juce::String("centre") : (pan < 0 ? "L" : "R") + juce::String(std::abs(pan)); }
    float velocityOf(int row, int step) const {
        return project().pattern(project().selectedPattern).steps[static_cast<std::size_t>(row)][static_cast<std::size_t>(step)].velocity / 127.0f;
    }
    void changed() {
        repaint();
        if (onStructureChanged) onStructureChanged();
        if (onSelectionChanged) onSelectionChanged();
        scrollRowIntoView(selected);
    }
    void repaintCell(int row, int step) { repaint(cellBounds(row, step).expanded(1)); }
    void repaintColumn(int step) {
        if (step < 0 || step >= length()) return;
        const auto r = cellBounds(0, step);
        repaint(r.getX(), 0, r.getWidth(), getHeight());
    }

    // A raised, bevelled button face: light on top, dark at the bottom, a bright edge along the top and a dark outline.
    static void paintRaised(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour face, float radius, float lift = 0.22f, float sink = 0.30f) {
        g.setGradientFill(juce::ColourGradient(face.brighter(lift), r.getX(), r.getY(), face.darker(sink), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle(r, radius);
        g.setColour(juce::Colours::white.withAlpha(0.13f));
        g.drawLine(r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f, 1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.drawLine(r.getX() + radius, r.getBottom() - 1.5f, r.getRight() - radius, r.getBottom() - 1.5f, 1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.65f));
        g.drawRoundedRectangle(r, radius, 1.0f);
    }

    // One step button. Off: a raised dark button, the second and fourth group of four tinted warm as in FL. On: the
    // channel's colour, dimmer for a softer hit, with a glow. The playhead brightens the button it is on.
    void paintCell(juce::Graphics& g, juce::Rectangle<float> r, const rack::Step& step, int index, juce::Colour colour,
                   bool audible, bool current) const {
        namespace col = ui::colours;
        const bool alternate = (index / 4) % 2 == 1;
        constexpr float radius = 3.0f;
        juce::Colour face = step.on ? colour.interpolatedWith(juce::Colour(0xff101114), 0.62f * (1.0f - static_cast<float>(step.velocity) / 127.0f))
                                    : juce::Colour(alternate ? 0xff3e3230 : 0xff353940);
        if (step.on && !audible) face = face.withMultipliedSaturation(0.2f).withMultipliedBrightness(0.7f);
        if (current) face = face.brighter(step.on ? 0.3f : 0.18f);
        if (step.on && audible) {
            for (int i = 3; i >= 1; --i) {
                g.setColour(face.withAlpha(0.07f));
                g.fillRoundedRectangle(r.expanded(static_cast<float>(i) * 1.1f), radius + static_cast<float>(i));
            }
        }
        paintRaised(g, r, face, radius, step.on ? 0.38f : 0.18f, step.on ? 0.22f : 0.34f);
        if (!step.on) return;
        // Marks for anything that is not the default, so a glance shows what was drawn.
        const auto ink = col::accentInk.withAlpha(0.85f);
        if (step.probability < 100) { g.setColour(col::warning); g.fillEllipse(r.getRight() - 7.0f, r.getY() + 2.0f, 4.5f, 4.5f); }
        if (step.pitch != 0) { g.setColour(ink); g.fillRect(r.getX() + 3.0f, step.pitch > 0 ? r.getY() + 3.0f : r.getBottom() - 5.0f, 5.0f, 2.0f); }
        if (step.pan != 0) {
            g.setColour(ink);
            const float half = r.getWidth() * 0.5f, tick = juce::jmax(2.0f, std::abs(static_cast<float>(step.pan)) / 63.0f * (half - 3.0f));
            g.fillRect(step.pan < 0 ? r.getCentreX() - tick : r.getCentreX(), r.getBottom() - 4.0f, tick, 2.0f);
        }
    }

    juce::String soundDescription(int row) const {
        const auto& info = project().info[static_cast<std::size_t>(row)];
        const auto name = juce::String::fromUTF8(info.name.c_str(), static_cast<int>(info.name.size()));
        if (project().channels[static_cast<std::size_t>(row)].isSynth()) return name + " (built-in synth)";
        if (info.sample.empty()) return name + " (no sound)";
        if (info.isBuiltin()) return name + " (built-in drum)";
        return name + " (" + sampleFileName(row) + ")";
    }

    juce::String sampleFileName(int row) const {
        const auto& info = project().info[static_cast<std::size_t>(row)];
        return juce::File(juce::String::fromUTF8(info.sample.c_str(), static_cast<int>(info.sample.size()))).getFileName();
    }

    void paintHeader(juce::Graphics& g, int row, const rack::Channel& channel, bool audible, juce::Colour colour) const {
        namespace col = ui::colours;
        const auto& info = project().info[static_cast<std::size_t>(row)];
        const bool isSelected = isChannelSelected(row);
        // The light: lit when the channel will sound, ringed when it is muted.
        const auto led = muteRect(row).toFloat().withSizeKeepingCentre(10.0f, 10.0f);
        g.setColour(col::inset);
        g.fillEllipse(led.expanded(1.5f));
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawEllipse(led.expanded(1.5f), 1.0f);
        if (audible) {
            g.setColour(colour.withAlpha(0.13f)); g.fillEllipse(led.expanded(4.5f));
            g.setColour(colour.withAlpha(0.20f)); g.fillEllipse(led.expanded(2.5f));
            g.setGradientFill(juce::ColourGradient(colour.brighter(0.55f), led.getCentreX() - 2.0f, led.getY(), colour.darker(0.25f), led.getCentreX(), led.getBottom(), false));
            g.fillEllipse(led);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.fillEllipse(led.getX() + 2.2f, led.getY() + 1.4f, 3.2f, 2.2f);
        } else {
            g.setColour(col::lineStrong.darker(0.4f));
            g.fillEllipse(led);
        }
        if (channel.mute) { g.setColour(col::mute.withAlpha(0.9f)); g.drawEllipse(led.expanded(3.5f), 1.4f); }
        paintKnob(g, panRect(row).toFloat(), channel.pan, true, colour, audible);
        paintKnob(g, volumeRect(row).toFloat(), channel.volume, false, colour, audible);
        // The channel's button: its name, with a grip at the right end. A sample that cannot be found shows in red.
        const auto name = nameRect(row).toFloat();
        const bool offline = sampleOffline(row);
        paintRaised(g, name, isSelected ? juce::Colour(0xff4d535d) : juce::Colour(0xff3f444c), 3.0f);
        for (int i = 0; i < 4; ++i) {
            const float x = name.getRight() - 15.0f + static_cast<float>(i) * 3.0f;
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRect(x, name.getY() + 4.0f, 1.0f, name.getHeight() - 8.0f);
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRect(x + 1.0f, name.getY() + 4.0f, 1.0f, name.getHeight() - 8.0f);
        }
        g.setColour(offline ? col::danger : (audible ? col::text : col::textDim));
        g.setFont(ui::font(13.0f, true));
        g.drawText(juce::String::fromUTF8(info.name.c_str(), static_cast<int>(info.name.size())),
                   name.toNearestInt().withTrimmedLeft(8).withTrimmedRight(20), juce::Justification::centred, true);
        // Solo and output.
        auto chip = [&](juce::Rectangle<float> r, juce::Colour face, juce::Colour ink, const juce::String& text) {
            paintRaised(g, r, face, 3.0f);
            g.setColour(ink);
            g.setFont(ui::font(10.5f, true));
            g.drawText(text, r.toNearestInt(), juce::Justification::centred);
        };
        chip(soloRect(row).toFloat(), channel.solo ? col::solo : juce::Colour(0xff353940), channel.solo ? col::accentInk : col::textDim, "S");
        chip(routeRect(row).toFloat(), juce::Colour(0xff353940), channel.output == rack::masterOutput ? col::textDim : ui::trackColour(channel.output),
             channel.output == rack::masterOutput ? juce::String("M") : "T" + juce::String(channel.output + 1));
        // The lamp beside the steps: lit for the selected channel (the one the graph editor shows).
        const auto lamp = lampRect(row).toFloat();
        if (isSelected) { g.setColour(col::accent.withAlpha(0.16f)); g.fillRoundedRectangle(lamp.expanded(2.5f), 6.0f); }
        g.setColour(col::inset);
        g.fillRoundedRectangle(lamp, 3.5f);
        g.setColour(isSelected ? col::accent : col::lineStrong);
        g.drawRoundedRectangle(lamp.reduced(0.5f), 3.5f, 1.0f);
        if (isSelected) {
            g.setGradientFill(juce::ColourGradient(col::accent.brighter(0.3f), lamp.getX(), lamp.getY(), col::accent.darker(0.2f), lamp.getX(), lamp.getBottom(), false));
            g.fillRoundedRectangle(lamp.reduced(2.5f), 1.8f);
        }
    }

    // A small round knob like FL's: shaded like a rounded cap, with a pointer. Same sweep and meaning as before
    // (7 o'clock to 5 o'clock; pan is centred at 12 o'clock).
    static void paintKnob(juce::Graphics& g, juce::Rectangle<float> r, float value, bool bipolar, juce::Colour colour, bool audible) {
        namespace col = ui::colours;
        const float start = juce::MathConstants<float>::pi * 1.25f, range = juce::MathConstants<float>::pi * 1.5f;
        const auto cap = r.withSizeKeepingCentre(20.0f, 20.0f);
        const auto centre = cap.getCentre();
        const float amount = bipolar ? (value + 1.0f) * 0.5f : value;
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(cap.translated(0.0f, 1.5f));
        g.setGradientFill(juce::ColourGradient(juce::Colour(audible ? 0xff646b75 : 0xff4d525a), centre.x - 4.0f, centre.y - 6.0f,
                                               juce::Colour(0xff22252a), centre.x + 5.0f, centre.y + 9.0f, true));
        g.fillEllipse(cap);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawEllipse(cap, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.drawEllipse(cap.reduced(1.3f), 0.8f);
        const float angle = start + range * amount;
        const float dx = std::sin(angle), dy = -std::cos(angle);
        g.setColour(audible ? col::text : col::textFaint);
        g.drawLine(centre.x + dx * 2.5f, centre.y + dy * 2.5f, centre.x + dx * 8.2f, centre.y + dy * 8.2f, 2.0f);
        // A dot in the channel's colour where the pointer ends shows at a glance that a knob is off its default.
        const bool movedFromDefault = bipolar ? std::abs(value) > 0.02f : std::abs(value - rack::defaultChannelVolume) > 0.01f;
        if (movedFromDefault && audible) { g.setColour(colour); g.fillEllipse(centre.x + dx * 8.2f - 1.6f, centre.y + dy * 8.2f - 1.6f, 3.2f, 3.2f); }
    }

    // Knobs ----------------------------------------------------------------------
    // A knob of a selected channel turns that knob on every selected channel by the same amount, so their
    // balance is kept (each stops at its own end of the range).
public:
    void beginKnob(int row, bool pan, int y) {
        drag = Drag::knob; knobRow = row; knobPan = pan; knobStartY = y;
        knobStarts.clear();
        for (const int r : targetsOf(row)) {
            const auto& c = project().channels[static_cast<std::size_t>(r)];
            knobStarts.emplace_back(r, pan ? c.pan : c.volume);
        }
        knobKey = "rack-knob-" + juce::String(++strokeCounter);
    }
    void dragKnob(int y, bool fine) {
        const float scale = (fine ? 0.0015f : 0.006f) * (knobPan ? 2.0f : 1.0f);
        moveKnobs(knobStarts, knobPan, static_cast<float>(knobStartY - y) * scale, knobKey);
    }
private:
    static float knobValue(bool pan, float value) {
        return pan ? juce::jlimit(-1.0f, 1.0f, std::abs(value) < 0.02f ? 0.0f : value) : juce::jlimit(0.0f, 1.0f, value);
    }
    // Each (channel, start value) moves by `delta`, as one undo step (merged with others of the same key).
    void moveKnobs(const std::vector<std::pair<int, float>>& starts, bool pan, float delta, const juce::String& key) {
        std::vector<int> rows;
        for (const auto& s : starts) rows.push_back(s.first);
        changeChannels(rows, plural(pan ? "Change channel pan" : "Change channel volume", rows), [&](int r, rack::Channel& c) {
            for (const auto& s : starts) if (s.first == r) (pan ? c.pan : c.volume) = knobValue(pan, s.second + delta);
        }, key);
        repaint(juce::Rectangle<int>(0, 0, headerWidth(), getHeight()));
    }
    void nudgeKnob(int row, bool pan, float delta) {
        std::vector<std::pair<int, float>> starts;
        for (const int r : targetsOf(row)) {
            const auto& c = project().channels[static_cast<std::size_t>(r)];
            starts.emplace_back(r, pan ? c.pan : c.volume);
        }
        moveKnobs(starts, pan, delta, "rack-knob-wheel-" + juce::String(row) + (pan ? "p" : "v"));
    }
    void resetKnob(int row, bool pan) {
        const auto rows = targetsOf(row);
        changeChannels(rows, plural(pan ? "Change channel pan" : "Change channel volume", rows),
                       [pan](int, rack::Channel& c) { (pan ? c.pan : c.volume) = pan ? 0.0f : rack::defaultChannelVolume; });
        repaint(juce::Rectangle<int>(0, 0, headerWidth(), getHeight()));
    }
    void paintKnobReadout(juce::Graphics& g) const {
        if (!project().validChannel(knobRow)) return;
        const auto& c = project().channels[static_cast<std::size_t>(knobRow)];
        const juce::String text = knobPan ? "Pan " + (std::abs(c.pan) < 0.005f ? juce::String("centre") : (c.pan < 0 ? "L " : "R ") + juce::String(juce::roundToInt(std::abs(c.pan) * 100)))
                                          : "Volume " + juce::String(juce::roundToInt(c.volume / rack::defaultChannelVolume * 100.0f)) + "%";
        const auto anchor = knobPan ? panRect(knobRow) : volumeRect(knobRow);
        const auto box = juce::Rectangle<int>(96, 20).withCentre({anchor.getCentreX(), anchor.getY() - 12}).constrainedWithin(getLocalBounds());
        g.setColour(ui::colours::inset.withAlpha(0.95f));
        g.fillRoundedRectangle(box.toFloat(), 4.0f);
        g.setColour(ui::colours::text);
        g.setFont(ui::font(12.0f, true));
        g.drawText(text, box, juce::Justification::centred);
    }

    static_assert(rack::maxChannels <= 64, "the selection is a 64-bit mask");
    static std::uint64_t bit(int row) noexcept { return row >= 0 && row < 64 ? std::uint64_t{1} << static_cast<unsigned>(row) : 0u; }
    int clampRow(int row) const noexcept { return project().channelCount > 0 ? juce::jlimit(0, project().channelCount - 1, row) : 0; }

    std::optional<rack::Project::StepRow> rowClipboard;
    ProjectController& controller;
    int selected{}, anchor{}, playhead{-1}, scrollX{}, scrollY{};
    std::uint64_t marked{1};   // the selected channels, one bit per row; always includes `selected`
    std::vector<std::pair<int, float>> knobStarts;   // each knob target and its value when the drag began
    Drag drag{Drag::none};
    bool paintValue{};
    int lastRow{}, lastStep{}, knobRow{}, knobStartY{}, nameStartWidth{}, nameStartX{};
    bool knobPan{};

    std::uint32_t strokeCounter{};
    juce::String strokeKey, knobKey;
    juce::Point<int> hover{-1, -1};
    std::unique_ptr<juce::FileChooser> chooser;
};

// Draws one property of every step of the selected channel as bars lined up
// under the grid's columns (FL's graph editor). Only steps that are on can be
// edited; drag across bars to draw, right-click a bar to reset it.
class ChannelRackGraph final : public juce::Component {
public:
    ChannelRackGraph(ProjectController& projectController, ChannelRackGrid& rackGrid) : controller(projectController), grid(rackGrid) {
        setName("Step graph editor");
        const char* names[]{"Velocity", "Pan", "Pitch", "Chance", "Gate", "Nudge", "Ratchet"};
        static_assert(std::size(names) == rack::stepFieldCount);
        for (int i = 0; i < rack::stepFieldCount; ++i) {
            auto& button = fieldButtons[static_cast<std::size_t>(i)];
            addAndMakeVisible(button);
            button.setButtonText(names[i]);
            button.setName(juce::String("Graph shows ") + names[i]);
            button.setClickingTogglesState(true);
            button.setRadioGroupId(2002);
            button.setColour(juce::TextButton::buttonOnColourId, ui::colours::accent);
            button.setColour(juce::TextButton::textColourOnId, ui::colours::accentInk);
            button.setTooltip(fieldTooltip(static_cast<rack::StepField>(i)));
            button.onClick = [this, i] { setField(static_cast<rack::StepField>(i)); };
        }
        fieldButtons[0].setToggleState(true, juce::dontSendNotification);
    }

    rack::StepField field() const noexcept { return currentField; }
    void setField(rack::StepField f) {
        currentField = f;
        fieldButtons[static_cast<std::size_t>(f)].setToggleState(true, juce::dontSendNotification);
        repaint();
    }

    // Geometry shared with the tests.
    juce::Rectangle<int> plot() const { return {grid.headerWidth(), 8, getWidth() - grid.headerWidth(), getHeight() - 16}; }
    int valueAtY(int y) const {
        const auto area = plot();
        const double t = 1.0 - juce::jlimit(0.0, 1.0, static_cast<double>(y - area.getY()) / juce::jmax(1, area.getHeight()));
        const int low = rack::fieldMinimum(currentField), high = rack::fieldMaximum(currentField);
        int value = low + juce::roundToInt(t * (high - low));
        if (currentField == rack::StepField::pan && std::abs(value) <= 2) value = 0;   // a little room to land on centre
        return value;
    }
    int yForValue(int value) const {
        const auto area = plot();
        const int low = rack::fieldMinimum(currentField), high = rack::fieldMaximum(currentField);
        return area.getBottom() - juce::roundToInt(static_cast<double>(value - low) / (high - low) * area.getHeight());
    }
    // Draws or erases nothing: sets one property of one step. Steps that are off are left alone.
    bool setValue(int step, int value, const juce::String& key) {
        const int channel = grid.selectedChannel();
        const auto field = currentField;
        const auto& p = controller.channelRack();
        if (!p.validStep(p.selectedPattern, channel, step) || step >= grid.length()) return false;
        if (!p.pattern(p.selectedPattern).steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)].on) return false;
        return controller.editRack("Edit step " + juce::String(fieldLabel()), [&](rack::Project& q) {
            return q.setStepField(q.selectedPattern, channel, step, field, value); }, key);
    }

    void resized() override {
        const int w = grid.headerWidth() - 20;
        const int bw = (w - 3 * 4) / 4;
        for (int i = 0; i < rack::stepFieldCount; ++i)
            fieldButtons[static_cast<std::size_t>(i)].setBounds(10 + (i % 4) * (bw + 4), 32 + (i / 4) * 28, bw, 24);
    }

    // What a step's value in the shown lane means, as the hover readout says it.
    juce::String valueText(const rack::Step& step) const {
        const int v = rack::getField(step, currentField);
        switch (currentField) {
        case rack::StepField::probability: return juce::String(v) + "%";
        case rack::StepField::gate: return v >= rack::fullGate ? juce::String("full") : juce::String(v) + (step.ratchet > 1 ? "% of each hit" : "% of the step");
        case rack::StepField::nudge: return v == 0 ? juce::String("on the grid") : juce::String(v) + "% of a step late";
        case rack::StepField::ratchet: return v == 1 ? juce::String("1 hit") : juce::String(v) + " hits";
        default: return juce::String(v);
        }
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::window);
        const auto& p = controller.channelRack();
        const int channel = grid.selectedChannel();
        ui::drawCaption(g, "Graph editor", juce::Rectangle<int>(10, 8, 120, 18));
        if (p.validChannel(channel)) {
            g.setColour(col::textDim);
            g.setFont(ui::font(12.0f));
            const auto& info = p.info[static_cast<std::size_t>(channel)];
            g.drawText(juce::String::fromUTF8(info.name.c_str(), static_cast<int>(info.name.size())), juce::Rectangle<int>(110, 8, grid.headerWidth() - 120, 18), juce::Justification::centredRight, true);
        }
        g.setColour(col::textFaint);
        g.setFont(ui::font(10.5f));
        g.drawFittedText("Drag across a row of bars to draw. Right-click a bar to reset it.", juce::Rectangle<int>(10, 90, grid.headerWidth() - 20, 40), juce::Justification::topLeft, 3);
        const auto area = plot();
        g.setColour(col::surface);
        g.fillRect(area);
        if (!p.validChannel(channel)) return;
        const auto& pattern = p.pattern(p.selectedPattern);
        if (p.isNoteChannel(p.selectedPattern, channel)) {
            g.setColour(col::textDim);
            g.drawText("This row uses piano-roll notes. Open Piano Roll to edit velocity, pitch and chance.", area.reduced(12), juce::Justification::centredLeft, true);
            return;
        }
        const int len = pattern.length, cell = grid.cellWidth();
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(area);
        const bool bipolar = currentField == rack::StepField::pan || currentField == rack::StepField::pitch;
        const int zeroY = yForValue(bipolar ? 0 : rack::fieldMinimum(currentField));
        g.setColour(col::line);
        g.drawHorizontalLine(zeroY, static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
        for (int s = 0; s < len; ++s) {
            const int x = grid.stepLeft(s);
            if (x + cell < area.getX() || x > area.getRight()) continue;
            const auto& step = pattern.steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(s)];
            if ((s / 4) % 2 == 1) { g.setColour(col::raised.withAlpha(0.5f)); g.fillRect(x, area.getY(), cell, area.getHeight()); }
            if (!step.on) { g.setColour(col::line); g.fillRect(x + cell / 2 - 1, zeroY - 1, 2, 2); continue; }
            const int y = yForValue(rack::getField(step, currentField));
            g.setColour(ui::trackColour(channel).withAlpha(s == hoverStep ? 1.0f : 0.85f));
            g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x + 3), static_cast<float>(juce::jmin(y, zeroY)), static_cast<float>(cell - 6), static_cast<float>(juce::jmax(2, std::abs(zeroY - y)))), 2.0f);
            g.setColour(col::text);
            g.fillRect(x + 3, y - 1, cell - 6, 2);
        }
        if (hoverStep >= 0 && hoverStep < len) {
            const auto& step = pattern.steps[static_cast<std::size_t>(channel)][static_cast<std::size_t>(hoverStep)];
            if (step.on) {
                g.setColour(col::text);
                g.setFont(ui::monoFont(12.0f, true));
                g.drawText(juce::String(fieldLabel()) + " " + valueText(step), area.withHeight(18).reduced(6, 0), juce::Justification::centredRight);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        stroke = "rack-graph-" + juce::String(++strokeCounter);
        lastStep = -1;
        if (e.mods.isPopupMenu()) {
            const int step = grid.stepAtX(e.x);
            if (step >= 0) { setValue(step, rack::fieldDefault(currentField), stroke); repaint(); }
            lastStep = -2;   // a right-click never draws
            return;
        }
        draw(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (lastStep != -2) draw(e); }
    void mouseMove(const juce::MouseEvent& e) override { setHover(grid.stepAtX(e.x)); }
    void mouseExit(const juce::MouseEvent&) override { setHover(-1); }

private:
    const char* fieldLabel() const {
        static const char* names[]{"velocity", "pan", "pitch", "chance", "gate", "nudge", "ratchet"};
        return names[static_cast<int>(currentField)];
    }
    static juce::String fieldTooltip(rack::StepField f) {
        switch (f) {
        case rack::StepField::velocity: return "Draw each step's velocity";
        case rack::StepField::pan: return "Draw each step's pan";
        case rack::StepField::pitch: return "Draw each step's pitch, in semitones";
        case rack::StepField::probability: return "Draw each step's chance of playing";
        case rack::StepField::gate: return "Draw how long each step sounds: the top plays the whole sample, lower cuts it off sooner";
        case rack::StepField::nudge: return "Draw how late each step plays, up to one step after the grid";
        case rack::StepField::ratchet: return "Draw how many times each step plays (1 to 4 hits inside the step)";
        }
        return {};
    }
    void setHover(int step) { if (hoverStep != step) { hoverStep = step; repaint(); } }
    void draw(const juce::MouseEvent& e) {
        const int step = grid.stepAtX(e.x);
        if (step < 0) return;
        setHover(step);
        const int value = valueAtY(e.y);
        // Fill the steps between the last one and this one so a quick drag draws a line.
        const int from = lastStep >= 0 ? lastStep : step;
        const int dir = step >= from ? 1 : -1;
        for (int s = from; s != step + dir; s += dir) {
            const double t = step == from ? 1.0 : static_cast<double>(s - from) / (step - from);
            const int v = lastStep >= 0 ? juce::roundToInt(lastValue + (value - lastValue) * t) : value;
            setValue(s, v, stroke);
        }
        lastStep = step; lastValue = value;
        repaint();
    }

    ProjectController& controller;
    ChannelRackGrid& grid;
    std::array<juce::TextButton, rack::stepFieldCount> fieldButtons;
    rack::StepField currentField{rack::StepField::velocity};
    int hoverStep{-1}, lastStep{-1}, lastValue{};
    std::uint32_t strokeCounter{};
    juce::String stroke;
};

// The whole screen: pattern and swing controls on top, the rack, then the graph.
class ChannelRackView final : public juce::Component, private juce::ScrollBar::Listener {
public:
    static constexpr int toolbarHeight = 52, scrollBarHeight = 12, verticalBarWidth = 12;

    explicit ChannelRackView(ProjectController& projectController)
        : controller(projectController), grid(projectController), graph(projectController, grid), scrollBar(false), verticalBar(true) {
        namespace col = ui::colours;
        setName("Channel rack");
        addAndMakeVisible(grid); addAndMakeVisible(graph); addAndMakeVisible(scrollBar); addChildComponent(verticalBar);
        scrollBar.addListener(this); verticalBar.addListener(this);
        grid.onLayoutChanged = [this] { resized(); };
        scrollBar.setAutoHide(true);
        verticalBar.setAutoHide(false);
        for (auto* label : {&patternLabel, &lengthLabel, &swingLabel}) {
            addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true));
            label->setColour(juce::Label::textColourId, col::textFaint);
        }
        patternLabel.setText("PATTERN", juce::dontSendNotification);
        lengthLabel.setText("STEPS", juce::dontSendNotification);
        swingLabel.setText("SWING", juce::dontSendNotification);
        addAndMakeVisible(patternBox); patternBox.setName("Pattern");
        patternBox.setTooltip("Choose the pattern to edit and play");
        patternBox.onChange = [this] {
            if (updating) return;
            controller.selectRackPattern(patternBox.getSelectedId() - 1);
            refresh();
        };
        for (auto* button : {&newPatternButton, &patternMenuButton, &addChannelButton}) addAndMakeVisible(button);
        newPatternButton.setButtonText("+"); newPatternButton.setName("New pattern"); newPatternButton.setTooltip("Add an empty pattern");
        newPatternButton.onClick = [this] { patternCommand(newPattern); };
        patternMenuButton.setButtonText("..."); patternMenuButton.setName("Pattern menu"); patternMenuButton.setTooltip("Clone, rename, clear or delete this pattern");
        patternMenuButton.onClick = [this] {
            showMenu(patternMenu(), patternMenuButton, [this](int id) { patternCommand(id); });
        };
        addChannelButton.setButtonText("+ Channel"); addChannelButton.setName("Add channel"); addChannelButton.setTooltip("Add a drum channel or a sampler");
        addChannelButton.onClick = [this] {
            showMenu(addChannelMenu(), addChannelButton, [this](int id) { addChannelCommand(id); });
        };
        addAndMakeVisible(lengthBox); lengthBox.setName("Pattern length");
        lengthBox.setTooltip("Number of sixteenth-note steps in this pattern");
        lengthBox.addItem("16 steps (1 bar)", 16); lengthBox.addItem("32 steps (2 bars)", 32); lengthBox.addItem("64 steps (4 bars)", 64);
        lengthBox.addItem("128 steps (8 bars)", 128);
        lengthBox.onChange = [this] {
            if (updating) return;
            const int length = lengthBox.getSelectedId();
            const bool changed = controller.editRack("Change pattern length", [&](rack::Project& p) { return p.setPatternLength(p.selectedPattern, length); });
            refresh();
            hint.setText(changed ? "Pattern length changed." : "Length unchanged: move or shorten notes that extend past the chosen end.", juce::dontSendNotification);
        };
        addAndMakeVisible(swing); swing.setName("Swing");
        swing.setTooltip("Delays every second sixteenth. 0% is straight; higher is more shuffled. Applies to all patterns.");
        swing.setRange(0.0, 100.0, 1.0);
        swing.setTextValueSuffix("%");
        swing.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 24);
        swing.setDoubleClickReturnValue(true, 0.0);
        swing.onValueChange = [this] {
            if (updating) return;
            const float amount = static_cast<float>(swing.getValue() / 100.0);
            controller.editRack("Change swing", [&](rack::Project& p) { return p.setSwing(amount); }, "rack-swing");
            if (onTitleChanged) onTitleChanged();
        };
        addAndMakeVisible(hint);
        hint.setText("Click or drag to paint steps, right-drag to erase. Click a name to hear it, double-click to rename, right-click for more.",
                     juce::dontSendNotification);
        hint.setFont(ui::font(12.0f));
        hint.setColour(juce::Label::textColourId, col::textFaint);
        hint.setJustificationType(juce::Justification::centredLeft);
        grid.onSelectionChanged = [this] { graph.repaint(); };
        grid.onStructureChanged = [this] { refresh(); };
        grid.onScrolled = [this](int x) { scrollBar.setCurrentRangeStart(x); graph.repaint(); };
        grid.onScrolledY = [this](int y) { verticalBar.setCurrentRangeStart(y); };
        refresh();
    }
    ~ChannelRackView() override { scrollBar.removeListener(this); verticalBar.removeListener(this); }

    std::function<void()> onTitleChanged;
    // Like ChannelRackGrid::menuLauncher: lets the tests answer for the user.
    std::function<void(juce::PopupMenu, std::function<void(int)>)> menuLauncher;

    // The "..." menu next to the pattern chooser, and the "+ Channel" menu.
    enum AddChannelId { addSampler = 99, addSoundBase = 100, addSynthBase = 200 };
    juce::PopupMenu patternMenu() const {
        juce::PopupMenu menu;
        menu.addItem(newPattern, "New pattern");
        menu.addItem(clonePattern, "Clone pattern");
        menu.addItem(renamePattern, "Rename pattern...");
        menu.addSeparator();
        menu.addItem(clearPattern, "Clear pattern");
        menu.addItem(deletePattern, "Delete pattern", controller.channelRack().patternCount > 1);
        return menu;
    }
    juce::PopupMenu addChannelMenu() const {
        juce::PopupMenu menu;
        for (std::size_t i = 0; i < rack::builtinSounds.size(); ++i) menu.addItem(addSoundBase + static_cast<int>(i), rack::builtinSounds[i].name);
        juce::PopupMenu synths;
        for (int preset = 0; preset < alien::presetCount; ++preset)
            synths.addItem(addSynthBase + preset, alien::presetNames[static_cast<std::size_t>(preset)], controller.channelRack().channelCount < rack::maxChannels);
        menu.addSubMenu("Built-in synth", synths);
        menu.addSeparator();
        menu.addItem(addSampler, "Sampler (load a file)...", controller.channelRack().channelCount < rack::maxChannels);
        return menu;
    }
    void addChannelCommand(int id) {
        if (id == addSampler) grid.addChannelWithFile();
        else if (id >= addSynthBase && id < addSynthBase + alien::presetCount) grid.addSynthChannel(id - addSynthBase);
        else if (id >= addSoundBase && id < addSoundBase + static_cast<int>(rack::builtinSounds.size()))
            grid.addChannelWithSound(rack::builtinSounds[static_cast<std::size_t>(id - addSoundBase)].id);
    }

    ChannelRackGrid& rackGrid() noexcept { return grid; }
    ChannelRackGraph& graphEditor() noexcept { return graph; }

    juce::String subtitle() const {
        const auto& p = controller.channelRack();
        const auto dot = juce::String(juce::CharPointer_UTF8("  \xc2\xb7  "));
        return juce::String::fromUTF8(p.patternNames[static_cast<std::size_t>(p.selectedPattern)].c_str()) + dot
             + juce::String(p.pattern(p.selectedPattern).length) + " steps" + dot + "swing " + juce::String(juce::roundToInt(p.swing * 100.0f)) + "%";
    }

    void setPlayhead(int step) { grid.setPlayhead(step); graph.repaint(); }

    // Re-read the document (after undo, open, new project, or an edit made here).
    void refresh() {
        const auto& p = controller.channelRack();
        updating = true;
        juce::StringArray names;
        for (int i = 0; i < p.patternCount; ++i)
            names.add(juce::String(i + 1) + "  " + juce::String::fromUTF8(p.patternNames[static_cast<std::size_t>(i)].c_str()));
        if (names != patternNames) {   // not while the user is choosing from the list
            patternNames = names;
            patternBox.clear(juce::dontSendNotification);
            for (int i = 0; i < names.size(); ++i) patternBox.addItem(names[i], i + 1);
        }
        patternBox.setSelectedId(p.selectedPattern + 1, juce::dontSendNotification);
        lengthBox.setSelectedId(p.pattern(p.selectedPattern).length, juce::dontSendNotification);
        swing.setValue(p.swing * 100.0, juce::dontSendNotification);
        addChannelButton.setEnabled(p.channelCount < rack::maxChannels);
        updating = false;
        // The rows (and the graph below them) share the height, and the name column sets where the steps start.
        if (p.channelCount != laidOutChannels || p.nameColumnWidth != laidOutNameWidth) resized();
        grid.refresh();
        layoutScroll();
        graph.repaint();
        repaint();
        if (onTitleChanged) onTitleChanged();
    }

    // Pattern commands, shared by the buttons, the menu and the tests.
    enum PatternCommand { newPattern = 1, clonePattern, renamePattern, clearPattern, deletePattern };
    void patternCommand(int id) {
        const int current = controller.channelRack().selectedPattern;
        if (id == newPattern) controller.editRack("New pattern", [](rack::Project& p) { return p.addPattern(); });
        else if (id == clonePattern) controller.editRack("Clone pattern", [&](rack::Project& p) { return p.addPattern(current); });
        else if (id == clearPattern) controller.editRack("Clear pattern", [](rack::Project& p) { return p.clearPattern(p.selectedPattern); });
        else if (id == deletePattern) controller.deleteRackPattern(current);
        else if (id == renamePattern) {
            const auto name = juce::String::fromUTF8(controller.channelRack().patternNames[static_cast<std::size_t>(current)].c_str());
            grid.askForName("Rename pattern", name, [this, current](const juce::String& text) {
                controller.editRack("Rename pattern", [&](rack::Project& p) { return p.renamePattern(current, text.toStdString()); });
                refresh();
            });
            return;
        }
        refresh();
    }

    void paint(juce::Graphics& g) override { g.fillAll(ui::colours::surface); }

    void resized() override {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop(toolbarHeight).reduced(14, 8);
        auto column = [&](juce::Label& label, int width) {
            auto c = bar.removeFromLeft(width);
            label.setBounds(c.removeFromTop(14));
            return c;
        };
        auto pattern = column(patternLabel, 220);
        patternBox.setBounds(pattern.removeFromLeft(142));
        pattern.removeFromLeft(4);
        newPatternButton.setBounds(pattern.removeFromLeft(34));
        pattern.removeFromLeft(4);
        patternMenuButton.setBounds(pattern.removeFromLeft(34));
        bar.removeFromLeft(14);
        lengthBox.setBounds(column(lengthLabel, 150));
        bar.removeFromLeft(14);
        swing.setBounds(column(swingLabel, 220));
        addChannelButton.setBounds(bar.removeFromRight(110).withTrimmedTop(14));
        hint.setBounds(r.removeFromBottom(24).reduced(14, 0));
        laidOutChannels = controller.channelRack().channelCount;
        laidOutNameWidth = controller.channelRack().nameColumnWidth;
        const int rows = juce::jmax(4, laidOutChannels);
        const int wanted = ChannelRackGrid::rulerHeight + rows * ChannelRackGrid::minRow;
        const int graphHeight = juce::jlimit(90, 150, r.getHeight() - wanted - scrollBarHeight);
        graph.setBounds(r.removeFromBottom(graphHeight));
        auto bar2 = r.removeFromBottom(scrollBarHeight);
        // More channels than the window has room for: the rows scroll.
        const bool scrolls = ChannelRackGrid::rulerHeight + laidOutChannels * ChannelRackGrid::minRow > r.getHeight();
        scrollBar.setBounds(bar2.withTrimmedLeft(grid.headerWidth()).withTrimmedRight(scrolls ? verticalBarWidth : 0));
        verticalBar.setVisible(scrolls);
        if (scrolls) verticalBar.setBounds(r.removeFromRight(verticalBarWidth).withTrimmedTop(ChannelRackGrid::rulerHeight));
        grid.setBounds(r);
        graph.resized();   // its buttons fill the header, which may have changed width without the graph changing size
        graph.repaint();
        layoutScroll();
    }

private:
    void showMenu(juce::PopupMenu menu, juce::Component& target, std::function<void(int)> chosen) {
        juce::Component::SafePointer<ChannelRackView> safe(this);
        std::function<void(int)> done = [safe, chosen = std::move(chosen)](int id) { if (safe != nullptr && id != 0) chosen(id); };
        if (menuLauncher) { menuLauncher(std::move(menu), std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target), std::move(done));
    }
    void layoutScroll() {
        const int visible = grid.stepAreaWidth(), total = grid.contentWidth();
        scrollBar.setRangeLimits(0.0, static_cast<double>(juce::jmax(total, visible)));
        scrollBar.setCurrentRange(grid.scrollPosition(), visible);
        const int shown = grid.visibleRowsHeight();
        verticalBar.setRangeLimits(0.0, static_cast<double>(juce::jmax(grid.contentHeight(), shown)));
        verticalBar.setCurrentRange(grid.scrollPositionY(), shown);
    }
    void scrollBarMoved(juce::ScrollBar* bar, double start) override {
        if (bar == &verticalBar) { grid.setScrollPositionY(static_cast<int>(start)); return; }
        grid.setScrollPosition(static_cast<int>(start));
        graph.repaint();
    }

    ProjectController& controller;
    ChannelRackGrid grid;
    ChannelRackGraph graph;
    juce::ScrollBar scrollBar, verticalBar;
    int laidOutChannels{-1}, laidOutNameWidth{-1};
    juce::Label patternLabel, lengthLabel, swingLabel, hint;
    juce::ComboBox patternBox, lengthBox;
    juce::StringArray patternNames;
    juce::TextButton newPatternButton, patternMenuButton, addChannelButton;
    juce::Slider swing{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    bool updating{};
};

} // namespace alien
