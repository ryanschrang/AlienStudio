#pragma once
#include "PluginPresets.h"
#include "StudioWidgets.h"
#include <functional>
#include <vector>

namespace alien {
// Browse a hosted plugin's presets (its programs and .vstpreset files) and load one. Browse first, load explicitly:
// selecting a row changes nothing, double-click, Enter or Load preset loads it. `load` returns why a preset was not
// loaded, or nothing.
class PluginPresetBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    using Loader = std::function<juce::String(const plugin_presets::Preset&)>;
    PluginPresetBrowser(const juce::String& pluginName, plugin_presets::Found found, Loader loader)
        : presets(std::move(found.presets)), truncated(found.truncated), load(std::move(loader)), plugin(pluginName) {
        setName("Plugin preset browser");
        addAndMakeVisible(title); title.setText(plugin + " presets", juce::dontSendNotification); title.setFont(ui::font(13));
        addAndMakeVisible(search); search.setName("Plugin preset search");
        search.setTextToShowWhenEmpty("Search presets (name or folder)", ui::colours::textFaint);
        search.onTextChange = [this] { rebuild(); };
        search.onReturnKey = [this] { loadSelected(); };
        addAndMakeVisible(list); list.setName("Plugin preset results"); list.setModel(this); list.setRowHeight(24);
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::inset);
        addAndMakeVisible(empty); empty.setInterceptsMouseClicks(false, false); empty.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(loadButton); loadButton.setButtonText("Load preset"); loadButton.setName("Load plugin preset");
        loadButton.setTooltip("Load the selected preset into the plugin. Like a change in its own window, it is saved with the project but is not an Undo step.");
        loadButton.onClick = [this] { loadSelected(); };
        addAndMakeVisible(status); status.setFont(ui::font(11));
        status.setText(truncated ? "Showing the first " + juce::String(plugin_presets::maxPresets) + " preset files." : juce::String{}, juce::dontSendNotification);
        rebuild();
        setSize(440, 460);
    }
    ~PluginPresetBrowser() override { list.setModel(nullptr); }

    // Types a search. A text editor reports a change from the message loop, so this rebuilds the list itself.
    void setQuery(const juce::String& text) { search.setText(text, false); rebuild(); }
    void focusSearch() { search.grabKeyboardFocus(); }
    int resultCount() const noexcept { return static_cast<int>(matches.size()); }
    const plugin_presets::Preset* result(int row) const {
        return row >= 0 && row < resultCount() ? &presets[static_cast<std::size_t>(matches[static_cast<std::size_t>(row)])] : nullptr;
    }
    void selectResult(int row) { list.selectRow(row); }
    const plugin_presets::Preset* selected() const { return result(list.getSelectedRow()); }
    juce::String statusText() const { return status.getText(); }
    juce::String emptyText() const { return empty.isVisible() ? empty.getText() : juce::String{}; }
    // Loads the selected preset. True when the plugin took it.
    bool loadSelected() {
        const auto* chosen = selected();
        if (chosen == nullptr || !load) return false;
        const auto why = load(*chosen);
        status.setText(why.isEmpty() ? "Loaded \"" + chosen->name + "\"." : why, juce::dontSendNotification);
        status.setTooltip(status.getText());
        return why.isEmpty();
    }
    void resized() override {
        auto r = getLocalBounds().reduced(10);
        title.setBounds(r.removeFromTop(22)); r.removeFromTop(4);
        search.setBounds(r.removeFromTop(26)); r.removeFromTop(6);
        status.setBounds(r.removeFromBottom(34)); r.removeFromBottom(4);
        loadButton.setBounds(r.removeFromBottom(28).removeFromLeft(120)); r.removeFromBottom(6);
        list.setBounds(r); empty.setBounds(r.reduced(12));
    }
private:
    void rebuild() {
        const auto* previous = selected();
        const auto keep = previous != nullptr ? *previous : plugin_presets::Preset{};
        matches = plugin_presets::filter(presets, search.getText());
        list.updateContent();
        int row = matches.empty() ? -1 : 0;
        for (int i = 0; previous != nullptr && i < resultCount(); ++i) if (*result(i) == keep) row = i;
        if (row < 0) list.deselectAllRows(); else list.selectRow(row);
        empty.setText(presets.empty() ? "No presets found. VST3 preset files go in Documents\\VST3 Presets\\<maker>\\<plugin>."
                                      : juce::String("No matching presets"), juce::dontSendNotification);
        empty.setVisible(matches.empty());
        loadButton.setEnabled(!matches.empty());
        repaint();
    }
    int getNumRows() override { return resultCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected) override {
        const auto* p = result(row);
        if (p == nullptr) return;
        if (isSelected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        const int folderWidth = juce::jmin(160, width / 3);
        g.setColour(ui::colours::text); g.setFont(ui::font(12));
        g.drawText(p->name, 8, 0, width - folderWidth - 16, height, juce::Justification::centredLeft, true);
        g.setColour(ui::colours::textFaint); g.setFont(ui::font(11));
        g.drawText(p->folder, width - folderWidth - 8, 0, folderWidth, height, juce::Justification::centredRight, true);
    }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { loadSelected(); }
    void returnKeyPressed(int) override { loadSelected(); }

    std::vector<plugin_presets::Preset> presets;
    bool truncated{};
    Loader load;
    juce::String plugin;
    std::vector<int> matches;
    juce::Label title, empty, status;
    juce::TextEditor search;
    juce::ListBox list;
    juce::TextButton loadButton;
};
} // namespace alien
