#pragma once
#include "StudioWidgets.h"
#include "AudioClipImport.h"
#include "StarterPack.h"
#include <functional>

namespace alien {
class SampleBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    SampleBrowser() {
        setName("Local sample browser");
        for (auto* component : std::initializer_list<juce::Component*>{&folder, &starter, &search, &list, &preview, &stopButton, &session, &song, &status, &empty}) addAndMakeVisible(component);
        folder.setButtonText("Choose sample folder..."); folder.onClick = [this] { chooseFolder(); };
        starter.setButtonText("Starter Pack"); starter.setTooltip("Drum sounds that come with Alien Studio");
        starter.onClick = [this] { showStarterPack(); };
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
        // Open on the Starter Pack, so there is something to browse before any folder is chosen.
        if (starterFolder.isDirectory()) scan(starterFolder);
    }
    ~SampleBrowser() override { stop(); cancelled->store(true); pool.removeAllJobs(true, -1); list.setModel(nullptr); }
    std::function<void(const SessionAudioClip&)> onPreview;
    std::function<void()> onStop;
    std::function<void(const juce::File&, bool)> onImport;
    void focusSearch() { search.grabKeyboardFocus(); }
    // Browses the Starter Pack that ships with Alien Studio; tests point it at another folder.
    void setStarterFolder(const juce::File& root) { starterFolder = root; updateButtons(); }
    void showStarterPack() { if (starterFolder.isDirectory()) scan(starterFolder); }
    void setImportEnabled(bool enabled, bool songEnabled) { canImport = enabled; canImportSong = songEnabled; updateButtons(); }
    juce::File selectedFile() const { const int row = list.getSelectedRow(); return row >= 0 && row < static_cast<int>(matches.size()) ? matches[static_cast<std::size_t>(row)] : juce::File{}; }
    int resultCount() const { return static_cast<int>(matches.size()); }
    void setStatus(const juce::String& text) { status.setText(text, juce::dontSendNotification); }
    void stop() { previewCancelled->store(true); ++previewGeneration; if (onStop) onStop(); }
    void visibilityChanged() override { if (!isShowing()) stop(); }
    void resized() override {
        auto r = getLocalBounds(); auto top = r.removeFromTop(28); starter.setBounds(top.removeFromRight(110)); top.removeFromRight(6); folder.setBounds(top); r.removeFromTop(6); search.setBounds(r.removeFromTop(26)); r.removeFromTop(6);
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
                safe->setStatus(juce::String(static_cast<int>(safe->files.size())) + " samples (limit 5,000 files / 50,000 entries). Preview uses original pitch and tempo.");
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
    void updateButtons() { const bool selected = selectedFile() != juce::File{}; starter.setEnabled(starterFolder.isDirectory()); empty.setVisible(matches.empty()); preview.setEnabled(selected); session.setEnabled(selected && canImport); song.setEnabled(selected && canImportSong); }
    void audition() {
        const auto file = selectedFile(); stop(); if (file == juce::File{}) return;
        previewCancelled = std::make_shared<std::atomic<bool>>(false); const auto cancel = previewCancelled;
        const auto generation = previewGeneration; setStatus("Loading preview...");
        juce::Component::SafePointer<SampleBrowser> safe(this);
        pool.addJob([safe, file, generation, cancel] {
            if (cancel->load()) return;
            // Read only the first five seconds, even for multi-hour source files.
            juce::AudioFormatManager formats; formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            auto clip = std::make_shared<SessionAudioClip>(); juce::String error;
            if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 192000 || reader->lengthInSamples <= 0) error = "Cannot preview this audio file.";
            else {
                clip->sampleRate = reader->sampleRate; clip->channels = static_cast<int>(reader->numChannels);
                clip->frames = static_cast<std::size_t>(std::min(reader->lengthInSamples, static_cast<juce::int64>(reader->sampleRate * 5)));
                clip->left.resize(clip->frames); if (clip->channels == 2) clip->right.resize(clip->frames);
                float* channels[]{clip->left.data(), clip->channels == 2 ? clip->right.data() : nullptr};
                if (!reader->read(channels, clip->channels, 0, static_cast<int>(clip->frames))) error = "Could not read sample preview.";
            }
            juce::MessageManager::callAsync([safe, generation, clip, error] {
                if (!safe || generation != safe->previewGeneration || !safe->isShowing()) return;
                if (error.isNotEmpty()) { safe->setStatus(error); return; }
                safe->setStatus("Preview: first five seconds at original pitch and tempo.");
                if (safe->onPreview) safe->onPreview(*clip);
            });
        });
    }
    int getNumRows() override { return resultCount(); }
    void selectedRowsChanged(int) override { stop(); updateButtons(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { audition(); }
    void returnKeyPressed(int) override { audition(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= resultCount()) return;
        if (selected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        g.setColour(ui::colours::text); g.setFont(ui::font(12)); g.drawText(matches[static_cast<std::size_t>(row)].getRelativePathFrom(folderRoot), 6, 0, width - 12, height, juce::Justification::centredLeft);
    }
    juce::TextButton folder, starter, preview, stopButton, session, song;
    juce::TextEditor search; juce::ListBox list; juce::Label status, empty;
    juce::File folderRoot, starterFolder{starterPack::folder()}; std::vector<juce::File> files, matches;
    std::unique_ptr<juce::FileChooser> chooser; juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> previewCancelled{std::make_shared<std::atomic<bool>>(false)};
    unsigned previewGeneration{}; bool canImport{}, canImportSong{};
};
} // namespace alien
