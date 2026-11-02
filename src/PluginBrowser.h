#pragma once
// T1-10: browse everything that can go in a rack, the stock effects and every scanned plugin, by
// name, maker, category or kind. Browse first, add explicitly: a row is only added when the user
// presses Use instrument / Add FX, double-clicks it or presses Enter.
#include "StockEffects.h"
#include "StudioWidgets.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <vector>

namespace alien {

class PluginBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    // Choice ids, as the Plugins panel's add path reads them: a stock effect is stockBase + its catalog
    // index; a scanned plugin is its index in the known plugin list + 1. 0 is no choice.
    static constexpr int stockBase = 1000;
    enum class Kind { all, instruments, effects };
    struct Entry {
        int choice{};
        juce::String name, maker, category, format;
        bool instrument{}, stock{}, blocked{};
        juce::String searchText() const {
            return (name + " " + maker + " " + category + " " + format + (instrument ? " instrument synth" : " effect fx")
                    + (stock ? " stock alien" : " plugin") + (blocked ? " blocked" : "")).toLowerCase();
        }
    };

    // The rows for the stock effects and a known plugin list, stock first, then plugins by name.
    static std::vector<Entry> entriesFor(const juce::KnownPluginList& list, const std::function<bool(const juce::PluginDescription&)>& isBlocked = {}) {
        std::vector<Entry> rows;
        for (std::size_t i = 0; i < stock::catalog.size(); ++i)
            rows.push_back({stockBase + static_cast<int>(i), stock::catalog[i].name, "Alien Studio", "Stock FX", "Built in", false, true, false});
        const auto types = list.getTypes();
        std::vector<Entry> scanned;
        for (int i = 0; i < types.size(); ++i) {
            const auto& d = types.getReference(i);
            // VST3 categories arrive as "Fx|Reverb".
            scanned.push_back({i + 1, d.name, d.manufacturerName, d.category.replace("|", " / "), d.pluginFormatName, d.isInstrument, false, isBlocked && isBlocked(d)});
        }
        std::stable_sort(scanned.begin(), scanned.end(), [](const Entry& a, const Entry& b) { return a.name.compareNatural(b.name) < 0; });
        rows.insert(rows.end(), scanned.begin(), scanned.end());
        return rows;
    }
    // Positions in `rows` that match every word of `query` and the kind filter.
    static std::vector<int> matching(const std::vector<Entry>& rows, const juce::String& query, Kind kind) {
        const auto terms = juce::StringArray::fromTokens(query.toLowerCase(), false);
        std::vector<int> found;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[static_cast<std::size_t>(i)];
            if ((kind == Kind::instruments && !row.instrument) || (kind == Kind::effects && row.instrument)) continue;
            const auto text = row.searchText();
            bool match = true;
            for (const auto& term : terms) if (term.isNotEmpty() && !text.contains(term)) { match = false; break; }
            if (match) found.push_back(i);
        }
        return found;
    }

    PluginBrowser() {
        setName("Plugin browser");
        addAndMakeVisible(search); search.setName("Plugin search");
        search.setTextToShowWhenEmpty("Search plugins (name, maker, category, instrument or FX)", ui::colours::textFaint);
        search.onTextChange = [this] { rebuild(); };
        search.onReturnKey = [this] { choose(); };
        addAndMakeVisible(kind); kind.setName("Plugin kind");
        kind.addItem("All", 1); kind.addItem("Instruments", 2); kind.addItem("Effects", 3);
        kind.setSelectedId(1, juce::dontSendNotification);
        kind.onChange = [this] { rebuild(); };
        addAndMakeVisible(list); list.setName("Plugin results"); list.setModel(this); list.setRowHeight(34);
        list.setTooltip("Double-click or press Enter to add to the selected destination: an instrument to its instrument slot, an effect to its FX rack.");
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::inset);
        addAndMakeVisible(empty); empty.setText("No matching plugins", juce::dontSendNotification); empty.setInterceptsMouseClicks(false, false);
        rebuild();
    }
    ~PluginBrowser() override { list.setModel(nullptr); }

    // Double-click or Enter on a row: its choice id and whether it is an instrument.
    std::function<void(int, bool)> onChoose;
    std::function<void()> onSelectionChanged;

    // Message thread: new scan results or blocklist. The selected plugin stays selected when it is still listed.
    void refresh(const juce::KnownPluginList& known, const std::function<bool(const juce::PluginDescription&)>& isBlocked = {}) {
        rows = entriesFor(known, isBlocked);
        rebuild();
    }
    // The selected row (nullptr when nothing is selected).
    const Entry* selected() const {
        const int row = list.getSelectedRow();
        return row >= 0 && row < static_cast<int>(matches.size()) ? &rows[static_cast<std::size_t>(matches[static_cast<std::size_t>(row)])] : nullptr;
    }
    int selectedChoice() const { const auto* e = selected(); return e != nullptr ? e->choice : 0; }
    // Selects a choice id, clearing the search and filter when they hide it. Returns false when it is not listed.
    bool select(int choice) {
        const auto at = [&] { for (std::size_t i = 0; i < matches.size(); ++i) if (rows[static_cast<std::size_t>(matches[i])].choice == choice) return static_cast<int>(i); return -1; };
        if (at() < 0) { search.setText({}, false); kind.setSelectedId(1, juce::dontSendNotification); rebuild(); }
        const int row = at();
        if (row < 0) return false;
        list.selectRow(row);
        return true;
    }
    // TextEditor posts its change message asynchronously, so a typed-in query rebuilds here, at once.
    void setQuery(const juce::String& text) { search.setText(text, false); rebuild(); }
    void setKind(Kind k) { kind.setSelectedId(static_cast<int>(k) + 1, juce::sendNotificationSync); }
    int resultCount() const noexcept { return static_cast<int>(matches.size()); }
    const Entry& result(int i) const { return rows[static_cast<std::size_t>(matches[static_cast<std::size_t>(i)])]; }
    void focusSearch() { search.grabKeyboardFocus(); }
    void choose() { if (const auto* e = selected(); e != nullptr && onChoose) onChoose(e->choice, e->instrument); }

    void resized() override {
        auto r = getLocalBounds();
        auto top = r.removeFromTop(26);
        kind.setBounds(top.removeFromRight(118)); top.removeFromRight(6); search.setBounds(top);
        r.removeFromTop(6);
        list.setBounds(r); empty.setBounds(r.reduced(8));
    }

private:
    void rebuild() {
        const int previous = selectedChoice();
        matches = matching(rows, search.getText(), static_cast<Kind>(kind.getSelectedId() - 1));
        list.updateContent();
        int row = -1;
        for (std::size_t i = 0; i < matches.size(); ++i) if (rows[static_cast<std::size_t>(matches[i])].choice == previous) row = static_cast<int>(i);
        if (matches.empty()) list.deselectAllRows();
        else list.selectRow(row >= 0 ? row : 0);
        empty.setVisible(matches.empty());
        if (onSelectionChanged) onSelectionChanged();
        repaint();
    }
    int getNumRows() override { return resultCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected) override {
        if (row < 0 || row >= resultCount()) return;
        const auto& e = result(row);
        if (isSelected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        const auto tag = e.blocked ? juce::String("BLOCKED") : e.stock ? juce::String("STOCK FX") : e.instrument ? juce::String("INSTRUMENT") : juce::String("FX");
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(8, 3);
        auto badge = area.removeFromRight(84);
        g.setColour(e.blocked ? ui::colours::mute : e.instrument ? ui::colours::accent : ui::colours::solo);
        g.setFont(ui::font(9.5f, true));
        g.drawText(tag, badge, juce::Justification::centredRight);
        g.setColour(e.blocked ? ui::colours::textFaint : ui::colours::text);
        g.setFont(ui::font(12.0f));
        g.drawText(e.name, area.removeFromTop(area.getHeight() / 2 + 2), juce::Justification::centredLeft, true);
        juce::StringArray details;
        for (const auto& part : {e.maker, e.category, e.format}) if (part.isNotEmpty()) details.add(part);
        g.setColour(ui::colours::textFaint);
        g.setFont(ui::font(10.5f));
        g.drawText(details.joinIntoString("  -  "), area, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int) override { if (onSelectionChanged) onSelectionChanged(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { choose(); }
    void returnKeyPressed(int) override { choose(); }

    juce::TextEditor search;
    juce::ComboBox kind;
    juce::ListBox list;
    juce::Label empty;
    std::vector<Entry> rows{entriesFor({})};
    std::vector<int> matches;
};

} // namespace alien
