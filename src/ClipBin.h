#pragma once
// The clip list beside the Song: every Channel Rack pattern and every Session clip that has
// something in it, as rows you can drag onto a Song lane (or double-click to put at the play
// position). It only reads the project; placing goes through the Song's own code, so undo,
// saving and playback are unchanged.
#include "ProjectController.h"
#include "StudioWidgets.h"
#include "Timeline.h"
#include <vector>

namespace alien {

namespace clipbin {

// What a dragged row stands for: a Session clip (track, scene) or a Channel Rack pattern (index).
struct Item {
    enum class Kind { none, clip, pattern } kind{Kind::none};
    int a{-1}, b{-1};   // clip: a = track, b = scene; pattern: a = pattern index
    bool valid() const noexcept { return kind == Kind::clip ? a >= 0 && b >= 0 : kind == Kind::pattern && a >= 0; }
    bool operator==(const Item& other) const noexcept { return kind == other.kind && a == other.a && b == other.b; }
};

inline juce::String encode(const Item& item) {
    if (item.kind == Item::Kind::clip) return "alien-song-item clip " + juce::String(item.a) + " " + juce::String(item.b);
    if (item.kind == Item::Kind::pattern) return "alien-song-item pattern " + juce::String(item.a);
    return {};
}

// Anything that is not exactly what encode() makes (another component's drag, a damaged string) is "none".
inline Item decode(const juce::var& description) {
    if (!description.isString()) return {};
    const auto parts = juce::StringArray::fromTokens(description.toString(), " ", "");
    const auto number = [](const juce::String& text, int& out) {
        if (text.isEmpty() || text.length() > 6 || !text.containsOnly("0123456789")) return false;
        out = text.getIntValue();
        return true;
    };
    Item item;
    if (parts.size() == 4 && parts[0] == "alien-song-item" && parts[1] == "clip" && number(parts[2], item.a) && number(parts[3], item.b)) item.kind = Item::Kind::clip;
    else if (parts.size() == 3 && parts[0] == "alien-song-item" && parts[1] == "pattern" && number(parts[2], item.a)) item.kind = Item::Kind::pattern;
    else item = {};
    return item;
}

} // namespace clipbin

class ClipBin final : public juce::Component, private juce::ListBoxModel {
public:
    static constexpr int width = 214, rowHeight = 28, hintHeight = 44;

    struct Row {
        enum class Type { header, item, note } type{Type::note};
        juce::String left, right;
        clipbin::Item item;
        juce::Colour colour;
        int placed{0};   // how many times the Song already holds it
    };

    explicit ClipBin(ProjectController& projectController) : controller(projectController) {
        setName("Clip list");
        addAndMakeVisible(list);
        list.setModel(this);
        list.setRowHeight(rowHeight);
        list.setMultipleSelectionEnabled(false);
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::surface);
        list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        list.setWantsKeyboardFocus(true);
        list.setTooltip("Drag a row onto a lane in the Song. Double-click or press Enter to put it at the play position.");
        refresh();
    }

    // Double-click or Enter on a row: put it at the Song's play position.
    std::function<void(const clipbin::Item&)> onPlace;

    // Re-reads the project. Cheap enough to call whenever the Song is shown or the project changes.
    void refresh() {
        rebuild();
        list.updateContent();
        list.repaint();
        repaint();
    }

    // For the tests: what the list shows, and the row for an item.
    int rowCount() const noexcept { return static_cast<int>(rows.size()); }
    const Row& row(int index) const { return rows[static_cast<std::size_t>(index)]; }
    int indexOf(const clipbin::Item& item) const noexcept {
        for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].type == Row::Type::item && rows[i].item == item) return static_cast<int>(i);
        return -1;
    }
    juce::ListBox& listBox() noexcept { return list; }
    // What a drag from this row carries (void for a heading or a note: those cannot be dragged).
    juce::var dragDescriptionForRow(int index) const { return getDragSourceDescriptionFor(index); }
    void activateRow(int index) { activate(index); }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::surface);
        g.setColour(col::line);
        g.fillRect(0, 0, 1, getHeight());
        g.setColour(col::textFaint);
        g.setFont(ui::font(10.5f));
        g.drawFittedText("Drag a row onto a lane. Double-click puts it at the play position.",
                         getLocalBounds().removeFromTop(hintHeight).reduced(10, 6), juce::Justification::topLeft, 3);
    }
    void resized() override { list.setBounds(getLocalBounds().withTrimmedTop(hintHeight).withTrimmedLeft(1)); }

private:
    int getNumRows() override { return static_cast<int>(rows.size()); }

    void paintListBoxItem(int index, juce::Graphics& g, int w, int h, bool selected) override {
        namespace col = ui::colours;
        if (index < 0 || index >= static_cast<int>(rows.size())) return;
        const auto& r = rows[static_cast<std::size_t>(index)];
        const auto area = juce::Rectangle<int>(0, 0, w, h);
        if (r.type == Row::Type::header) {
            g.setColour(col::textFaint);
            g.setFont(ui::font(10.5f, true));
            g.drawText(r.left, area.reduced(10, 0).withTrimmedTop(6), juce::Justification::centredLeft, true);
            return;
        }
        if (r.type == Row::Type::note) {
            g.setColour(col::textFaint);
            g.setFont(ui::font(11.0f));
            g.drawFittedText(r.left, area.reduced(10, 2), juce::Justification::centredLeft, 2);
            return;
        }
        const auto inner = area.reduced(6, 2);
        g.setColour(selected ? col::accent.withAlpha(0.18f) : col::raised.withAlpha(0.6f));
        g.fillRoundedRectangle(inner.toFloat(), 4.0f);
        g.setColour(r.colour);
        g.fillRoundedRectangle(inner.withWidth(3).toFloat(), 1.5f);
        auto text = inner.withTrimmedLeft(10).withTrimmedRight(6);
        if (r.placed > 0) {
            g.setColour(col::accent);
            g.fillEllipse(static_cast<float>(text.getRight() - 6), static_cast<float>(text.getCentreY() - 3), 6.0f, 6.0f);
            text.removeFromRight(12);
        }
        const int rightWidth = juce::jmin(text.getWidth() / 2, 64);
        g.setColour(col::textDim);
        g.setFont(ui::font(10.5f));
        g.drawText(r.right, text.removeFromRight(rightWidth), juce::Justification::centredRight, true);
        g.setColour(col::text);
        g.setFont(ui::font(11.5f, true));
        g.drawText(r.left, text, juce::Justification::centredLeft, true);
    }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override {
        return selectedRows.size() == 1 ? getDragSourceDescriptionFor(selectedRows[0]) : juce::var();
    }
    juce::var getDragSourceDescriptionFor(int index) const {
        if (index < 0 || index >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(index)].type != Row::Type::item) return {};
        return clipbin::encode(rows[static_cast<std::size_t>(index)].item);
    }
    void listBoxItemDoubleClicked(int index, const juce::MouseEvent&) override { activate(index); }
    void returnKeyPressed(int index) override { activate(index); }
    juce::String getTooltipForRow(int index) override {
        if (index < 0 || index >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(index)].type != Row::Type::item) return {};
        const auto& r = rows[static_cast<std::size_t>(index)];
        return r.left + (r.placed > 0 ? ": placed " + juce::String(r.placed) + (r.placed == 1 ? " time" : " times") + " in the Song" : ": not in the Song yet")
             + ". Drag it onto a lane, or double-click to put it at the play position.";
    }

    void activate(int index) {
        if (index < 0 || index >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(index)].type != Row::Type::item) return;
        if (onPlace) onPlace(rows[static_cast<std::size_t>(index)].item);
    }

    void rebuild() {
        rows.clear();
        const auto& d = controller.document();
        const auto& t = controller.timeline();
        const auto& rack = controller.channelRack();
        const auto placedClips = [&](int track, int scene) {
            int n = 0;
            for (const auto& r : t.regions) if (!r.isPattern() && r.row == track && r.source == scene) ++n;
            return n;
        };
        const auto placedPatterns = [&](int pattern) {
            int n = 0;
            for (const auto& r : t.regions) if (r.isPattern() && r.source == pattern) ++n;
            return n;
        };
        Row header; header.type = Row::Type::header;
        header.left = "PATTERNS  (Channel Rack)";
        rows.push_back(header);
        for (int p = 0; p < rack.patternCount; ++p) {
            Row r; r.type = Row::Type::item;
            r.item = {clipbin::Item::Kind::pattern, p, -1};
            r.left = juce::String(p + 1) + "  " + juce::String::fromUTF8(rack.patternNames[static_cast<std::size_t>(p)].c_str());
            r.right = juce::String(rack.pattern(p).length) + " steps";
            r.colour = ui::colours::warning;
            r.placed = placedPatterns(p);
            rows.push_back(r);
        }
        header.left = "CLIPS  (Session)";
        rows.push_back(header);
        bool any = false;
        for (int track = 0; track < d.trackCount(); ++track)
            for (int scene = 0; scene < d.sceneCount(); ++scene) {
                const auto& clip = d.clip(track, scene);
                if (clip.isEmpty()) continue;
                any = true;
                Row r; r.type = Row::Type::item;
                r.item = {clipbin::Item::Kind::clip, track, scene};
                r.left = d.tracks[static_cast<std::size_t>(track)].name + ui::utf8("  \xc2\xb7  ") + d.scenes[static_cast<std::size_t>(scene)].name;
                r.right = clip.isAudioClip() ? juce::String("audio") : juce::String(static_cast<int>(clip.notes().count)) + (clip.notes().count == 1 ? " note" : " notes");
                r.colour = ui::trackColour(d.tracks[static_cast<std::size_t>(track)].colour);
                r.placed = placedClips(track, scene);
                rows.push_back(r);
            }
        if (!any) {
            Row note; note.type = Row::Type::note;
            note.left = "No clips yet. Double-click a track's lane in the Song to start one.";
            rows.push_back(note);
        }
    }

    ProjectController& controller;
    juce::ListBox list{"Clips"};
    std::vector<Row> rows;
};

} // namespace alien
