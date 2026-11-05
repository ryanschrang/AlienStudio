#pragma once
#include "StudioWidgets.h"
#include "AudioClipImport.h"
#include "StarterPack.h"
#include "SamplePreviewRender.h"
#include "SampleTempoDetect.h"
#include "SampleDrag.h"
#include "MidiClipDrag.h"
#include <functional>

namespace alien {
class SampleBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    SampleBrowser() {
        setName("Local sample browser");
        for (auto* component : std::initializer_list<juce::Component*>{&folder, &starter, &search, &list, &preview, &stopButton, &session, &song, &status, &empty, &dragDestination, &matchTempo, &sourceTempo, &detectTempo, &tempoLabel, &matchKey, &sourceKey, &targetKey}) addAndMakeVisible(component);
        dragDestination.setName("Sample drag destination"); dragDestination.addItem("Drag into Session", 1); dragDestination.addItem("Drag into Song", 2); dragDestination.setSelectedId(1);
        dragDestination.setTooltip("Dragging a sample reveals this editor. Drop it onto an empty Session cell or a free Song lane position.");
        list.setTooltip("Drag a sample to import it. Choose Session or Song above; the editor opens when dragging starts. Drag a MIDI clip into the piano roll, or a drum groove onto the Channel Rack.");
        matchTempo.setButtonText("Match project tempo"); matchTempo.setName("Match sample preview tempo");
        matchTempo.onClick = [this] { stop(); updateButtons(); };
        sourceTempo.setName("Sample source BPM"); sourceTempo.setRange(30, 300, .1); sourceTempo.setValue(120);
        sourceTempo.setSliderStyle(juce::Slider::IncDecButtons); sourceTempo.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 65, 24);
        sourceTempo.setTooltip("Source BPM of the selected sample. Type it, or press Detect BPM. Only preview is stretched.");
        sourceTempo.onValueChange = [this] { stop(); updateButtons(); };
        detectTempo.setButtonText("Detect BPM"); detectTempo.setName("Detect sample BPM");
        detectTempo.setTooltip("Estimate the selected sample's tempo from its first minute and fill in Source BPM. It can be off by half or double, so check it by ear.");
        detectTempo.onClick = [this] { detect(); };
        tempoLabel.setText("Source BPM   |   Project: 120 BPM", juce::dontSendNotification); tempoLabel.setFont(ui::font(11));
        for (auto* key : {&sourceKey, &targetKey}) {
            for (int i = 0; i < 12; ++i) key->addItem(previewKeyNames[i], i + 1);
            key->setSelectedId(1, juce::dontSendNotification); key->onChange = [this] { stop(); updateButtons(); };
        }
        sourceKey.setName("Sample source key"); sourceKey.setTooltip("Root key of the selected sample, entered manually.");
        targetKey.setName("Sample preview target key"); targetKey.setTooltip("Key to hear the preview in. It shifts by at most six semitones, the nearer way round.");
        matchKey.setButtonText("Match key"); matchKey.setName("Match sample preview key");
        matchKey.setTooltip("Shift the preview's pitch from the source key to the target key. Only preview is changed.");
        matchKey.onClick = [this] { stop(); updateButtons(); };
        folder.setButtonText("Choose sample folder..."); folder.onClick = [this] { chooseFolder(); };
        starter.setButtonText("Starter Pack"); starter.setTooltip("Drum sounds that come with Alien Studio");
        starter.onClick = [this] { showStarterPack(); };
        search.setName("Sample search"); search.setTextToShowWhenEmpty("Search WAV / MP3 / MIDI names and folders", ui::colours::textFaint);
        search.onTextChange = [this] { rebuild(); }; search.onReturnKey = [this] { audition(); }; search.onEscapeKey = [this] { stop(); };
        empty.setText("No matching samples", juce::dontSendNotification); empty.setInterceptsMouseClicks(false, false);
        list.setColour(juce::ListBox::backgroundColourId, ui::colours::inset);
        list.setName("Sample results"); list.setModel(this); list.setRowHeight(26);
        preview.setButtonText("Preview 5s"); preview.onClick = [this] { audition(); };
        stopButton.setButtonText("Stop"); stopButton.onClick = [this] { stop(); };
        session.setButtonText("Import to empty Session cell"); session.onClick = [this] { if (canImport && selectedIsSample() && onImport) { stop(); onImport(selectedFile(), false); } };
        song.setButtonText("Import to Song cursor"); song.onClick = [this] { if (canImportSong && selectedIsSample() && onImport) { stop(); onImport(selectedFile(), true); } };
        status.setText("Choose a folder to browse local samples.", juce::dontSendNotification);
        updateButtons();
        // Open on the Starter Pack, so there is something to browse before any folder is chosen.
        if (starterFolder.isDirectory()) scan(starterFolder);
    }
    ~SampleBrowser() override { stop(); cancelled->store(true); pool.removeAllJobs(true, -1); list.setModel(nullptr); }
    std::function<void(bool)> onDragStarted;
    // A MIDI clip started dragging: the owner shows where it can be dropped (the piano roll, or the Channel Rack for a groove).
    std::function<void(const juce::File&)> onMidiDragStarted;
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
    // Browses the Starter Pack that ships with Alien Studio; tests point it at another folder.
    void setStarterFolder(const juce::File& root) { starterFolder = root; updateButtons(); }
    void showStarterPack() { if (starterFolder.isDirectory()) scan(starterFolder); }
    void setImportEnabled(bool enabled, bool songEnabled) { canImport = enabled; canImportSong = songEnabled; updateButtons(); }
    juce::File selectedFile() const { const int row = list.getSelectedRow(); return row >= 0 && row < static_cast<int>(matches.size()) ? matches[static_cast<std::size_t>(row)] : juce::File{}; }
    juce::var dragDescriptionFor(int row) const {
        if (row < 0 || row >= static_cast<int>(matches.size())) return {};
        const auto& file = matches[static_cast<std::size_t>(row)];
        return mididrag::supported(file) ? mididrag::encode(file) : sampledrag::encode(file);
    }
    // The selected row is an audio sample that can be previewed and imported; a MIDI clip is only dragged.
    bool selectedIsSample() const { const auto file = selectedFile(); return file.existsAsFile() && !mididrag::supported(file); }
    int resultCount() const { return static_cast<int>(matches.size()); }
    void setStatus(const juce::String& text) { status.setText(text, juce::dontSendNotification); }
    void stop() {
        previewCancelled->store(true); ++previewGeneration; if (onStop) onStop();
        if (detecting) { detecting = false; setStatus("Tempo detection stopped."); updateButtons(); }
    }
    void visibilityChanged() override { if (!isShowing()) stop(); }
    void resized() override {
        auto r = getLocalBounds(); auto top = r.removeFromTop(28); starter.setBounds(top.removeFromRight(110)); top.removeFromRight(6); folder.setBounds(top); r.removeFromTop(6); search.setBounds(r.removeFromTop(26)); r.removeFromTop(6); dragDestination.setBounds(r.removeFromTop(26)); r.removeFromTop(6);
        tempoLabel.setBounds(r.removeFromTop(18)); auto tempo = r.removeFromTop(26); sourceTempo.setBounds(tempo.removeFromLeft(108)); tempo.removeFromLeft(6); detectTempo.setBounds(tempo.removeFromLeft(84)); tempo.removeFromLeft(6); matchTempo.setBounds(tempo); r.removeFromTop(4);
        auto key = r.removeFromTop(26); sourceKey.setBounds(key.removeFromLeft(64)); key.removeFromLeft(6); targetKey.setBounds(key.removeFromLeft(64)); key.removeFromLeft(6); matchKey.setBounds(key); r.removeFromTop(6);
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
                const auto clips = static_cast<int>(std::count_if(safe->files.begin(), safe->files.end(), [](const auto& f) { return mididrag::supported(f); }));
                safe->setStatus(juce::String(static_cast<int>(safe->files.size()) - clips) + " samples" + (clips > 0 ? " and " + juce::String(clips) + " MIDI clips" : juce::String())
                                + " (limit 5,000 files / 50,000 entries). Preview is isolated; set source BPM to match the project tempo.");
            });
        });
    }
    static bool supported(const juce::File& file) { return file.hasFileExtension("wav;mp3") || mididrag::supported(file); }
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
    void updateButtons() { const bool selected = selectedIsSample(); starter.setEnabled(starterFolder.isDirectory()); empty.setVisible(matches.empty()); const bool valid = validPreviewSettings(matchTempo.getToggleState(), sourceTempo.getValue(), projectTempo, keyShift());
        preview.setEnabled(selected && valid); detectTempo.setEnabled(selected && !detecting);
        preview.setTooltip(valid ? "Preview up to five seconds without changing the project" : matchTempo.getToggleState() && !validPreviewTempoRatio(sourceTempo.getValue(), projectTempo)
            ? "Use a source/project tempo ratio between 0.5 and 2." : "This key shift and tempo ratio together stretch the sample too far."); session.setEnabled(selected && canImport); song.setEnabled(selected && canImportSong); }
    // One job at a time with preview: it shares the cancel flag and generation, so a new
    // selection, a typed BPM, Stop or leaving the browser all discard the result.
    void detect() {
        const auto file = selectedFile(); stop(); if (!selectedIsSample()) return;
        previewCancelled = std::make_shared<std::atomic<bool>>(false); const auto cancel = previewCancelled;
        const auto generation = previewGeneration; detecting = true; updateButtons(); setStatus("Detecting tempo...");
        juce::Component::SafePointer<SampleBrowser> safe(this);
        pool.addJob([safe, file, generation, cancel] {
            if (cancel->load()) return;
            auto result = std::make_shared<SampleTempoResult>(detectSampleTempo(file, *cancel));
            juce::MessageManager::callAsync([safe, generation, result] {
                if (!safe || generation != safe->previewGeneration) return;
                safe->detecting = false;
                if (result->tempo) {
                    safe->sourceTempo.setValue(result->tempo->bpm, juce::dontSendNotification);
                    safe->setStatus("Estimated " + juce::String(result->tempo->bpm, 1) + " BPM. It can be off by half or double, so check it by ear.");
                } else safe->setStatus(result->message);
                safe->updateButtons();
            });
        });
    }
    void audition() {
        const auto file = selectedFile(); stop(); if (file == juce::File{}) return;
        if (mididrag::supported(file)) { setStatus(midiHint); return; }
        previewCancelled = std::make_shared<std::atomic<bool>>(false); const auto cancel = previewCancelled;
        const bool sync = matchTempo.getToggleState(); const double sourceBpm = sourceTempo.getValue(), targetBpm = projectTempo;
        if (sync && !validPreviewTempoRatio(sourceBpm, targetBpm)) { setStatus("Use a source/project tempo ratio between 0.5 and 2."); return; }
        const int semitones = keyShift();
        if (!validPreviewSettings(sync, sourceBpm, targetBpm, semitones)) { setStatus("This key shift and tempo ratio together stretch the sample too far."); return; }
        const auto generation = previewGeneration; setStatus(sync || semitones != 0 ? "Matching preview tempo and key..." : "Loading preview...");
        juce::String keyText;
        if (semitones != 0) keyText = juce::String(previewKeyNames[sourceKey.getSelectedId() - 1]) + " to " + previewKeyNames[targetKey.getSelectedId() - 1]
                                      + " (" + (semitones > 0 ? "+" : "") + juce::String(semitones) + " semitones)";
        juce::Component::SafePointer<SampleBrowser> safe(this);
        pool.addJob([safe, file, generation, cancel, sync, sourceBpm, targetBpm, semitones, keyText] {
            if (cancel->load()) return;
            auto result = std::make_shared<AudioClipImportResult>(renderSamplePreview(file, sync, sourceBpm, targetBpm, *cancel, semitones));
            juce::MessageManager::callAsync([safe, generation, result, sync, sourceBpm, targetBpm, keyText] {
                if (!safe || generation != safe->previewGeneration || !safe->isShowing()) return;
                if (!result->clip) { safe->setStatus(result->error); return; }
                const auto tempoText = sync ? juce::String(sourceBpm, 1) + " to " + juce::String(targetBpm, 1) + " BPM" : juce::String("original tempo");
                safe->setStatus(keyText.isNotEmpty() ? "Preview: " + tempoText + "; key " + keyText + " (up to 5s)."
                    : sync ? "Preview: " + tempoText + "; pitch preserved (up to 5s)." : "Preview: first five seconds at original pitch and tempo.");
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
            if (const auto clip = mididrag::decode(description); clip != juce::File{})
                juce::MessageManager::callAsync([safe, clip] { if (safe && safe->onMidiDragStarted) safe->onMidiDragStarted(clip); });
            else juce::MessageManager::callAsync([safe, song] { if (safe && safe->onDragStarted) safe->onDragStarted(song); });
        }
        return description;
    }
private:
    int getNumRows() override { return resultCount(); }
    void selectedRowsChanged(int) override { stop(); updateButtons(); if (mididrag::supported(selectedFile())) setStatus(midiHint); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { audition(); }
    void returnKeyPressed(int) override { audition(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= resultCount()) return;
        if (selected) g.fillAll(ui::colours::accent.withAlpha(.2f));
        g.setColour(ui::colours::text); g.setFont(ui::font(12)); g.drawText(matches[static_cast<std::size_t>(row)].getRelativePathFrom(folderRoot), 6, 0, width - 12, height, juce::Justification::centredLeft);
    }
    juce::TextButton folder, starter, preview, stopButton, session, song, detectTempo;
    juce::ToggleButton matchTempo; juce::Slider sourceTempo; juce::Label tempoLabel;
    juce::ToggleButton matchKey; juce::ComboBox sourceKey, targetKey;
    int keyShift() const { return matchKey.getToggleState() ? previewKeyShift(sourceKey.getSelectedId() - 1, targetKey.getSelectedId() - 1) : 0; }
    double projectTempo{120};
    juce::ComboBox dragDestination;
    static constexpr const char* midiHint = "MIDI clip: drag it into the piano roll to add its notes at the drop point, or a drum groove onto the Channel Rack.";
    juce::TextEditor search; juce::ListBox list; juce::Label status, empty;
    juce::File folderRoot, starterFolder{starterPack::folder()}; std::vector<juce::File> files, matches;
    std::unique_ptr<juce::FileChooser> chooser; juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> previewCancelled{std::make_shared<std::atomic<bool>>(false)};
    unsigned previewGeneration{}; bool canImport{}, canImportSong{}, detecting{};
};
} // namespace alien
