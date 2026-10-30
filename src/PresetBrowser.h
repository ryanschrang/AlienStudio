#pragma once
#include "StudioWidgets.h"
#include "SynthEngine.h"
#include <functional>
#include <vector>

namespace alien {
// Browse first, apply explicitly. Factory indices stay stable in filtered rows.
class PresetBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    PresetBrowser() {
        setName("Factory preset browser");
        addAndMakeVisible(search); search.setName("Factory preset search");
        search.setTextToShowWhenEmpty("Search factory presets (name or type)", ui::colours::textFaint);
        search.onTextChange = [this] { rebuild(); };
        search.onReturnKey = [this] { audition(); }; search.onEscapeKey = [this] { stop(); };
        addAndMakeVisible(list); list.setName("Factory preset results"); list.setModel(this); list.setRowHeight(26);
        list.setTooltip("Select a sound; double-click or Enter to audition. Use preset changes the live/step synth.");
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::inset);
        addAndMakeVisible(empty); empty.setText("No matching presets", juce::dontSendNotification); empty.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(current); current.setFont(ui::font(11));
        for (auto* button : {&preview, &stopButton, &use}) addAndMakeVisible(*button);
        preview.setButtonText("Audition C4"); preview.setName("Audition factory preset"); preview.setTooltip("Play a short C4 preview without changing the project");
        stopButton.setButtonText("Stop"); stopButton.setName("Stop preset audition");
        use.setButtonText("Use preset"); use.setName("Use factory preset"); use.setTooltip("Use this sound for the live keyboard and step sequencer");
        preview.onClick = [this] { audition(); }; stopButton.onClick = [this] { stop(); };
        use.onClick = [this] { if (canUse() && onApply) { stop(); onApply(selectedPreset()); } };
        rebuild(); setCurrentPreset(0);
    }
    ~PresetBrowser() override { stop(); list.setModel(nullptr); }
    std::function<void(int)> onAudition, onApply;
    std::function<void()> onStop;
    void setCurrentPreset(int index) {
        if (index < 0 || index >= presetCount) return;
        currentPreset = index; current.setText("Current: " + juce::String(presetNames[static_cast<std::size_t>(index)]), juce::dontSendNotification); updateButtons();
    }
    void setApplyEnabled(bool enabled) { applyEnabled = enabled; updateButtons(); }
    void focusSearch() { search.grabKeyboardFocus(); }
    void stop() { if (onStop) onStop(); }
    int selectedPreset() const { const int row = list.getSelectedRow(); return row >= 0 && row < static_cast<int>(matches.size()) ? matches[static_cast<std::size_t>(row)] : -1; }
    int resultCount() const noexcept { return static_cast<int>(matches.size()); }
    void resized() override {
        auto r = getLocalBounds(); current.setBounds(r.removeFromBottom(18)); r.removeFromBottom(4);
        auto buttons = r.removeFromBottom(28); preview.setBounds(buttons.removeFromLeft(104)); buttons.removeFromLeft(6);
        stopButton.setBounds(buttons.removeFromLeft(60)); buttons.removeFromLeft(6); use.setBounds(buttons.removeFromLeft(108));
        r.removeFromBottom(6); search.setBounds(r.removeFromTop(26)); r.removeFromTop(6); list.setBounds(r); empty.setBounds(r.reduced(8));
    }
    void visibilityChanged() override { if (!isShowing()) stop(); }
private:
    inline static constexpr std::array<const char*, presetCount> tags{"lead saw", "bass square", "pluck sine", "bass sub sine", "bass acid saw", "pad soft saw", "pad strings saw", "brass saw", "lead square", "keys organ square", "keys electric sine", "bell sine"};
    bool canUse() const { return applyEnabled && selectedPreset() >= 0 && selectedPreset() != currentPreset; }
    void updateButtons() { preview.setEnabled(selectedPreset() >= 0); use.setEnabled(canUse()); empty.setVisible(matches.empty()); }
    void rebuild() {
        const int previous = selectedPreset(); stop(); matches.clear();
        const auto terms = juce::StringArray::fromTokens(search.getText().toLowerCase(), false);
        for (int i = 0; i < presetCount; ++i) {
            const auto text = (juce::String(presetNames[static_cast<std::size_t>(i)]) + " " + tags[static_cast<std::size_t>(i)]).toLowerCase();
            bool match = true; for (const auto& term : terms) if (term.isNotEmpty() && !text.contains(term)) match = false;
            if (match) matches.push_back(i);
        }
        list.updateContent();
        const auto found = std::find(matches.begin(), matches.end(), previous);
        if (matches.empty()) list.deselectAllRows();
        else list.selectRow(found != matches.end() ? static_cast<int>(found - matches.begin()) : 0);
        updateButtons(); repaint();
    }
    void audition() { if (selectedPreset() >= 0 && onAudition) onAudition(selectedPreset()); }
    int getNumRows() override { return resultCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= resultCount()) return;
        if (selected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        g.setColour(ui::colours::text); g.setFont(ui::font(12));
        const int preset = matches[static_cast<std::size_t>(row)];
        g.drawText(presetNames[static_cast<std::size_t>(preset)], 8, 0, width - 16, height, juce::Justification::centredLeft);
    }
    void selectedRowsChanged(int) override { stop(); updateButtons(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { audition(); }
    void returnKeyPressed(int) override { audition(); }
    void deleteKeyPressed(int) override { stop(); }
    juce::TextEditor search;
    juce::ListBox list;
    juce::Label empty, current;
    juce::TextButton preview, stopButton, use;
    std::vector<int> matches;
    int currentPreset{};
    bool applyEnabled{true};
};
} // namespace alien
