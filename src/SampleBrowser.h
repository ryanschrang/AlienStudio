#pragma once
#include "StudioWidgets.h"
#include "SamplePreviewRender.h"
#include "SampleDrag.h"
#include <functional>

namespace alien {
class SampleBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    SampleBrowser() {
        setName("Local sample browser");
        for (auto* component : std::initializer_list<juce::Component*>{&folder, &search, &list, &preview, &stopButton, &session, &song, &status, &empty, &dragDestination, &matchTempo, &sourceTempo, &tempoLabel}) addAndMakeVisible(component);
        dragDestination.setName("Sample drag destination"); dragDestination.addItem("Drag into Session", 1); dragDestination.addItem("Drag into Song", 2); dragDestination.setSelectedId(1);
        dragDestination.setTooltip("Dragging a sample reveals this editor. Drop it onto an empty Session cell or a free Song lane position.");
        list.setTooltip("Drag a sample to import it. Choose Session or Song above; the editor opens when dragging starts.");
        matchTempo.setButtonText("Match project tempo"); matchTempo.setName("Match sample preview tempo");
        matchTempo.onClick = [this] { stop(); updateButtons(); };
        sourceTempo.setName("Sample source BPM"); sourceTempo.setRange(30, 300, .1); sourceTempo.setValue(120);
        sourceTempo.setSliderStyle(juce::Slider::IncDecButtons); sourceTempo.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 65, 24);
        sourceTempo.setTooltip("Source BPM of the selected sample, entered manually. Only preview is stretched.");
        sourceTempo.onValueChange = [this] { stop(); updateButtons(); };
        tempoLabel.setText("Source BPM   |   Project: 120 BPM", juce::dontSendNotification); tempoLabel.setFont(ui::font(11));
        folder.setButtonText("Choose sample folder..."); folder.onClick = [this] { chooseFolder(); };
        search.setName("Sample search"); search.setTextToShowWhenEmpty("Search WAV / MP3 names and folders", ui::colours::textFaint);
        search.onTextChange = [this] { rebuild(); }; search.onReturnKey = [this] { audition(); }; search.onEscapeKey = [this] { stop(); };
        empty.setText("No matching samples", juce::dontSendNotification); empty.setInterceptsMouseClicks(false, false);
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::inset);
        list.setName("Sample results"); list.setModel(this); list.setRowHeight(26);
        preview.setButtonText("Preview 5s"); preview.onClick = [this] { audition(); };
        stopButton.setButtonText("Stop"); stopButton.onClick = [this] { stop(); };
        session.setButtonText("Import to empty Session cell"); session.onClick = [this] { if (canImport && selectedFile().existsAsFile() && onImport) { stop(); onImport(selectedFile(), false); } };
        song.setButtonText("Import to Song cursor"); song.onClick = [this] { if (canImportSong && selectedFile().existsAsFile() && onImport) { stop(); onImport(selectedFile(), true); } };
        status.setText("Choose a folder to browse local samples.", juce::dontSendNotification);
        updateButtons();
    }
    ~SampleBrowser() override { stop(); cancelled->store(true); pool.removeAllJobs(true, -1); list.setModel(nullptr); }
    std::function<void(bool)> onDragStarted;
    std::function<void(const SessionAudioClip&)> onPreview;
    std::function<void()> onStop;
    std::function<void(const juce::File&, bool)> onImport;
    void setProjectTempo(double bpm) {
        if (bpm == projectTempo) return;
        projectTempo = bpm;
        tempoLabel.setText("Source BPM   |   Project: " + juce::String(bpm, 1) + " BPM", juce::dontSendNotification);
        if (matchTempo.getToggleState()) { stop(); setStatus("Project tempo changed. Press Preview to hear the new tempo."); }
        updateButtons();
    }
    void focusSearch() { search.grabKeyboardFocus(); }
    void setImportEnabled(bool enabled, bool songEnabled) { canImport = enabled; canImportSong = songEnabled; updateButtons(); }
    juce::File selectedFile() const { const int row = list.getSelectedRow(); return row >= 0 && row < static_cast<int>(matches.size()) ? matches[static_cast<std::size_t>(row)] : juce::File{}; }
    juce::var dragDescriptionFor(int row) const { return row >= 0 && row < static_cast<int>(matches.size()) ? sampledrag::encode(matches[static_cast<std::size_t>(row)]) : juce::var{}; }
    int resultCount() const { return static_cast<int>(matches.size()); }
    void setStatus(const juce::String& text) { status.setText(text, juce::dontSendNotification); }
    void stop() { previewCancelled->store(true); ++previewGeneration; if (onStop) onStop(); }
    void visibilityChanged() override { if (!isShowing()) stop(); }
    void resized() override {
        auto r = getLocalBounds(); folder.setBounds(r.removeFromTop(28)); r.removeFromTop(6); search.setBounds(r.removeFromTop(26)); r.removeFromTop(6); dragDestination.setBounds(r.removeFromTop(26)); r.removeFromTop(6);
        tempoLabel.setBounds(r.removeFromTop(18)); auto tempo = r.removeFromTop(26); sourceTempo.setBounds(tempo.removeFromLeft(108)); tempo.removeFromLeft(6); matchTempo.setBounds(tempo); r.removeFromTop(6);
        status.setBounds(r.removeFromBottom(38)); song.setBounds(r.removeFromBottom(28)); r.removeFromBottom(4); session.setBounds(r.removeFromBottom(28)); r.removeFromBottom(6);
        auto buttons = r.removeFromBottom(28); preview.setBounds(buttons.removeFromLeft(100)); buttons.removeFromLeft(6); stopButton.setBounds(buttons.removeFromLeft(60)); r.removeFromBottom(6); list.setBounds(r); empty.setBounds(r.reduced(8));
    }
    void scan(const juce::File& root) {
        stop(); cancelled->store(true); cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancel = cancelled; files.clear(); rebuild(); setStatus("Searching sample folder...");
        juce::Component::SafePointer<SampleBrowser> safe(this);
        pool.addJob([safe, root, cancel] {
            std::vector<juce::File> found; int inspected = 0;
            for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFilesAndDirectories, juce::File::FollowSymlinks::no)) {
                if (cancel->load() || ++inspected > 50000) break;
                const auto file = entry.getFile();
                if (file.existsAsFile() && supported(file)) found.push_back(file);
                if (found.size() >= 5000) break;
            }
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.getFullPathName().compareNatural(b.getFullPathName()) < 0; });
            juce::MessageManager::callAsync([safe, root, cancel, found = std::move(found)]() mutable {
                if (!safe || cancel->load()) return;
                safe->folderRoot = root; safe->files = std::move(found); safe->rebuild();
                safe->setStatus(juce::String(static_cast<int>(safe->files.size())) + " samples (limit 5,000 files / 50,000 entries). Preview is isolated; set source BPM to match the project tempo.");
            });
        });
    }
    static bool supported(const juce::File& file) { return file.hasFileExtension("wav;mp3"); }
private:
    void chooseFolder() {
        chooser = std::make_unique<juce::FileChooser>("Choose sample folder", folderRoot);
        juce::Component::SafePointer<SampleBrowser> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe](const auto& choice) { if (safe && choice.getResult().isDirectory()) safe->scan(choice.getResult()); });
    }
    void rebuild() {
        const auto selected = selectedFile(); stop(); matches.clear();
        const auto terms = juce::StringArray::fromTokens(search.getText().toLowerCase(), false);
        for (const auto& file : files) {
            const auto name = file.getRelativePathFrom(folderRoot).toLowerCase(); bool match = true;
            for (const auto& term : terms) if (term.isNotEmpty() && !name.contains(term)) match = false;
            if (match) matches.push_back(file);
        }
        list.updateContent(); list.deselectAllRows();
        if (!matches.empty()) { const auto found = std::find(matches.begin(), matches.end(), selected); list.selectRow(found == matches.end() ? 0 : static_cast<int>(found - matches.begin())); }
        updateButtons(); repaint();
    }
    void updateButtons() { const bool selected = selectedFile() != juce::File{}; empty.setVisible(matches.empty()); preview.setEnabled(selected && (!matchTempo.getToggleState() || validPreviewTempoRatio(sourceTempo.getValue(), projectTempo)));
        preview.setTooltip(matchTempo.getToggleState() && !validPreviewTempoRatio(sourceTempo.getValue(), projectTempo) ? "Use a source/project tempo ratio between 0.5 and 2." : "Preview up to five seconds without changing the project"); session.setEnabled(selected && canImport); song.setEnabled(selected && canImportSong); }
    void audition() {
        const auto file = selectedFile(); stop(); if (file == juce::File{}) return;
        previewCancelled = std::make_shared<std::atomic<bool>>(false); const auto cancel = previewCancelled;
        const bool sync = matchTempo.getToggleState(); const double sourceBpm = sourceTempo.getValue(), targetBpm = projectTempo;
        if (sync && !validPreviewTempoRatio(sourceBpm, targetBpm)) { setStatus("Use a source/project tempo ratio between 0.5 and 2."); return; }
        const auto generation = previewGeneration; setStatus(sync ? "Matching preview tempo..." : "Loading preview...");
        juce::Component::SafePointer<SampleBrowser> safe(this);
        pool.addJob([safe, file, generation, cancel, sync, sourceBpm, targetBpm] {
            if (cancel->load()) return;
            auto result = std::make_shared<AudioClipImportResult>(renderSamplePreview(file, sync, sourceBpm, targetBpm, *cancel));
            juce::MessageManager::callAsync([safe, generation, result, sync, sourceBpm, targetBpm] {
                if (!safe || generation != safe->previewGeneration || !safe->isShowing()) return;
                if (!result->clip) { safe->setStatus(result->error); return; }
                safe->setStatus(sync ? "Preview: " + juce::String(sourceBpm, 1) + " to " + juce::String(targetBpm, 1) + " BPM; pitch preserved (up to 5s)."
                    : "Preview: first five seconds at original pitch and tempo.");
                if (safe->onPreview) safe->onPreview(*result->clip);
            });
        });
    }
public:
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override {
        const auto description = rows.size() == 1 ? dragDescriptionFor(rows[0]) : juce::var{};
        if (!description.isVoid()) {
            stop(); const bool song = dragDestination.getSelectedId() == 2;
            juce::Component::SafePointer<SampleBrowser> safe(this);
            juce::MessageManager::callAsync([safe, song] { if (safe && safe->onDragStarted) safe->onDragStarted(song); });
        }
        return description;
    }
private:
    int getNumRows() override { return resultCount(); }
    void selectedRowsChanged(int) override { stop(); updateButtons(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { audition(); }
    void returnKeyPressed(int) override { audition(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= resultCount()) return;
        if (selected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        g.setColour(ui::colours::text); g.setFont(ui::font(12)); g.drawText(matches[static_cast<std::size_t>(row)].getRelativePathFrom(folderRoot), 6, 0, width - 12, height, juce::Justification::centredLeft);
    }
    juce::TextButton folder, preview, stopButton, session, song;
    juce::ToggleButton matchTempo; juce::Slider sourceTempo; juce::Label tempoLabel;
    double projectTempo{120};
    juce::ComboBox dragDestination;
    juce::TextEditor search; juce::ListBox list; juce::Label status, empty;
    juce::File folderRoot; std::vector<juce::File> files, matches;
    std::unique_ptr<juce::FileChooser> chooser; juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> previewCancelled{std::make_shared<std::atomic<bool>>(false)};
    unsigned previewGeneration{}; bool canImport{}, canImportSong{};
};
} // namespace alien
