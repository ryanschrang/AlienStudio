#pragma once
#include "SampleBrowser.h"
#include "AudioFileWrite.h"

static void sampleBrowserChecks() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienSampleBrowser", "", false);
    require(root.createDirectory().wasOk(), "sample browser fixture directory");
    alien::SessionAudioClip clip; clip.sampleRate = 8000; clip.channels = 1; clip.frames = 8000 * 6; clip.left.assign(clip.frames, .2f); clip.buildWaveform();
    std::atomic<bool> cancel{false}; juce::String error;
    const auto wav = root.getChildFile("Deep/Kick.wav"); require(wav.getParentDirectory().createDirectory().wasOk(), "sample subfolder");
    require(alien::writeClipWav(wav, clip, cancel, error), "write browser audio fixture");
    root.getChildFile("ignore.txt").replaceWithText("ignored");
    require(alien::SampleBrowser::supported(juce::File("C:/Snare.MP3")) && !alien::SampleBrowser::supported(root.getChildFile("ignore.txt")), "WAV and MP3 extension filtering");
    require(alien::SampleBrowser::supported(juce::File("C:/Groove.MID")) && alien::SampleBrowser::supported(juce::File("C:/Chords.midi")), "MIDI clips are listed too");
    require(alien::mididrag::decode(alien::mididrag::encode(juce::File("C:/My Clips/Groove.mid"))) == juce::File("C:/My Clips/Groove.mid"), "a MIDI drag keeps its full path");
    for (const juce::var bad : {juce::var(3), juce::var("alien-midi-clip\nrelative.mid"), juce::var("alien-midi-clip\nC:/song.wav"), juce::var("alien-local-sample\nC:/clip.mid"),
                                juce::var("alien-midi-clip\nC:/one.mid\nC:/two.mid")})
        require(alien::mididrag::decode(bad) == juce::File{}, "malformed MIDI clip payload refused");
    require(alien::sampledrag::encode(juce::File("C:/clip.mid")).isVoid() && alien::sampledrag::decode(alien::mididrag::encode(juce::File("C:/clip.mid"))) == juce::File{},
            "a MIDI clip is never a sample drag, so Session and Song ignore it");
    {
        alien::SampleBrowser browser; browser.setSize(360, 400); browser.addToDesktop(0); browser.setVisible(true); browser.scan(root);
        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(10); };
        for (int i = 0; i < 400 && browser.resultCount() == 0; ++i) pump();
        require(browser.resultCount() == 1 && browser.selectedFile() == wav, "recursive scan selects WAV and excludes other files");
        juce::TextEditor* search = nullptr; juce::Button* preview = nullptr; juce::Button* import = nullptr;
        for (auto* c : browser.getChildren()) {
            if (auto* editor = dynamic_cast<juce::TextEditor*>(c)) search = editor;
            if (auto* button = dynamic_cast<juce::Button*>(c)) {
                if (button->getButtonText() == "Preview 5s") preview = button;
                if (button->getButtonText() == "Import to empty Session cell") import = button;
            }
            require(browser.getLocalBounds().contains(c->getBounds()), "sample controls fit compact layout");
        }
        require(search && preview && import, "sample browser controls available");
        require(alien::sampledrag::decode(browser.dragDescriptionFor(0)) == wav, "drag payload preserves selected full path with spaces");
        require(browser.dragDescriptionFor(-1).isVoid() && browser.dragDescriptionFor(9).isVoid(), "invalid browser drag rows refused");
        for (const juce::var bad : {juce::var(3), juce::var("alien-local-sample\nrelative.wav"), juce::var("alien-local-sample\nC:/sample.txt"), juce::var("alien-local-sample\nC:/one.wav\nC:/two.wav")})
            require(alien::sampledrag::decode(bad) == juce::File{}, "malformed sample payload refused");
        int dragViews = 0; browser.onDragStarted = [&](bool song) { require(song, "drag destination selector chooses Song"); ++dragViews; };
        for (auto* child : browser.getChildren()) if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && child->getName() == "Sample drag destination") combo->setSelectedId(2);
        juce::SparseSet<int> rows; rows.addRange({0, 1});
        require(alien::sampledrag::decode(browser.getDragSourceDescription(rows)) == wav, "list model supplies sample drag description");
        pump(); require(dragViews == 1, "drag start asynchronously reveals selected destination");
        int imports = 0, previews = 0; browser.onImport = [&](const juce::File& file, bool song) { require(file == wav && !song, "explicit Session target"); ++imports; };
        browser.onPreview = [&](const alien::SessionAudioClip& sample) { require(sample.frames == 40000, "preview decoding capped at five seconds"); ++previews; };
        search->setText("DEEP kick", false); search->onTextChange(); require(browser.resultCount() == 1, "search matches case-insensitive path terms");
        require(!import->isEnabled() && preview->isEnabled(), "busy imports do not block browsing");
        preview->onClick(); for (int i = 0; i < 400 && previews == 0; ++i) pump();
        require(previews == 1 && imports == 0, "preview does not import");
        preview->onClick(); browser.stop(); for (int i = 0; i < 10; ++i) pump(); require(previews == 1, "stop invalidates pending decode");
        juce::Slider* sourceBpm = nullptr; juce::ToggleButton* sync = nullptr;
        for (auto* child : browser.getChildren()) {
            if (child->getName() == "Sample source BPM") sourceBpm = dynamic_cast<juce::Slider*>(child);
            if (child->getName() == "Match sample preview tempo") sync = dynamic_cast<juce::ToggleButton*>(child);
        }
        require(sourceBpm && sync, "manual source BPM and tempo-match controls available");
        sync->setToggleState(true, juce::dontSendNotification); sync->onClick(); sourceBpm->setValue(60, juce::dontSendNotification); sourceBpm->onValueChange();
        browser.onPreview = [&](const alien::SessionAudioClip& sample) { require(sample.frames == 20000, "faster preview uses source/project tempo ratio"); ++previews; };
        preview->onClick(); for (int i = 0; i < 400 && previews == 1; ++i) pump(); require(previews == 2 && imports == 0, "synced preview does not import");
        sourceBpm->setValue(240, juce::dontSendNotification); sourceBpm->onValueChange();
        browser.onPreview = [&](const alien::SessionAudioClip& sample) { require(sample.frames == 40000, "slower preview stays within five-second playback cap"); ++previews; };
        preview->onClick(); for (int i = 0; i < 400 && previews == 2; ++i) pump(); require(previews == 3, "slower preview rendered");
        preview->onClick(); browser.setProjectTempo(140); for (int i = 0; i < 10; ++i) pump(); require(previews == 3, "project tempo change cancels pending preview");
        sourceBpm->setValue(30, juce::dontSendNotification); sourceBpm->onValueChange(); require(!preview->isEnabled(), "unsupported BPM ratio disables preview");
        sync->setToggleState(false, juce::dontSendNotification); sync->onClick(); require(preview->isEnabled(), "original-tempo preview remains available");
        juce::ToggleButton* matchKey = nullptr; juce::ComboBox* sourceKey = nullptr; juce::ComboBox* targetKey = nullptr;
        for (auto* child : browser.getChildren()) {
            if (child->getName() == "Match sample preview key") matchKey = dynamic_cast<juce::ToggleButton*>(child);
            if (child->getName() == "Sample source key") sourceKey = dynamic_cast<juce::ComboBox*>(child);
            if (child->getName() == "Sample preview target key") targetKey = dynamic_cast<juce::ComboBox*>(child);
        }
        require(matchKey && sourceKey && targetKey && sourceKey->getNumItems() == 12 && sourceKey->getSelectedId() == 1 && targetKey->getSelectedId() == 1, "source and target key controls available, both starting at C");
        targetKey->setSelectedId(3, juce::sendNotificationSync); require(preview->isEnabled() && !matchKey->getToggleState(), "key controls change nothing until Match key is on");
        matchKey->setToggleState(true, juce::dontSendNotification); matchKey->onClick();
        browser.onPreview = [&](const alien::SessionAudioClip& sample) { require(sample.frames == 40000, "key-shifted preview keeps its length"); ++previews; };
        preview->onClick(); for (int i = 0; i < 400 && previews == 3; ++i) pump(); require(previews == 4 && imports == 0, "key-shifted preview rendered without importing");
        bool keyStatus = false;
        for (auto* child : browser.getChildren()) if (auto* l = dynamic_cast<juce::Label*>(child); l && l->getText().contains("key C to D (+2 semitones)")) keyStatus = true;
        require(keyStatus, "status line names the key shift");
        sync->setToggleState(true, juce::dontSendNotification); sync->onClick(); sourceBpm->setValue(70, juce::dontSendNotification); sourceBpm->onValueChange();
        require(preview->isEnabled(), "half-tempo preview shifted up stays in range");
        targetKey->setSelectedId(11, juce::sendNotificationSync);
        require(!preview->isEnabled() && preview->getTooltip().contains("key shift"), "half-tempo preview shifted down is refused with a reason");
        matchKey->setToggleState(false, juce::dontSendNotification); matchKey->onClick(); require(preview->isEnabled(), "turning Match key off allows the tempo-only preview");
        sync->setToggleState(false, juce::dontSendNotification); sync->onClick();
        browser.setImportEnabled(true, true); import->onClick(); require(imports == 1, "import requires explicit action");
        search->setText("no match", false); search->onTextChange(); require(browser.dragDescriptionFor(0).isVoid(), "empty filtered list has no stale drag payload"); require(browser.resultCount() == 0 && !import->isEnabled() && !preview->isEnabled(), "empty search clears stale actions");
    }
    {
        // The Starter Pack: shown at once from its button, with its samples and its MIDI clips.
        alien::SampleBrowser browser; browser.setSize(360, 400); browser.addToDesktop(0); browser.setVisible(true);
        const juce::File pack{juce::String(ALIEN_STARTER_PACK_DIR)};
        browser.setStarterFolder(pack); browser.showStarterPack();
        for (int i = 0; i < 400 && browser.resultCount() == 0; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        const auto clips = alien::starterPack::midiClips(pack);
        require(browser.resultCount() == static_cast<int>(alien::starterPack::drumSamples(pack).size() + clips.size()), "the Starter Pack button browses the pack's samples and MIDI clips");
        // A MIDI clip row drags as a MIDI clip; it cannot be previewed or imported as audio.
        int midiRow = -1;
        for (int row = 0; row < browser.resultCount() && midiRow < 0; ++row)
            if (alien::mididrag::decode(browser.dragDescriptionFor(row)) == clips.back().file) midiRow = row;
        require(midiRow >= 0, "the last Starter Pack clip is a MIDI drag source");
        juce::ListBox* list = nullptr; juce::Button* preview = nullptr; juce::Button* import = nullptr; juce::Label* status = nullptr;
        for (auto* c : browser.getChildren()) {
            if (auto* l = dynamic_cast<juce::ListBox*>(c)) list = l;
            if (auto* b = dynamic_cast<juce::Button*>(c); b && b->getButtonText() == "Preview 5s") preview = b;
            if (auto* b = dynamic_cast<juce::Button*>(c); b && b->getButtonText() == "Import to empty Session cell") import = b;
            if (auto* l = dynamic_cast<juce::Label*>(c); l && l->getText().contains("samples and")) status = l;
        }
        require(list && preview && import && status, "the status line counts samples and MIDI clips");
        browser.setImportEnabled(true, true);
        list->selectRow(midiRow);
        require(!preview->isEnabled() && !import->isEnabled() && !browser.selectedIsSample(), "a MIDI clip cannot be previewed or imported as audio");
        require(status->getText().startsWith("MIDI clip: drag it into the piano roll"), "and the status line says how to use it");
        juce::File dragged; int sampleDrags = 0;
        browser.onMidiDragStarted = [&](const juce::File& file) { dragged = file; };
        browser.onDragStarted = [&](bool) { ++sampleDrags; };
        juce::SparseSet<int> rows; rows.addRange({midiRow, midiRow + 1});
        require(alien::mididrag::decode(browser.getDragSourceDescription(rows)) == clips.back().file, "the list supplies the MIDI drag");
        for (int i = 0; i < 5; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(dragged == clips.back().file && sampleDrags == 0, "dragging a MIDI clip asks the owner to show where it can land, not the sample destination");
        juce::Button* starter = nullptr;
        for (auto* c : browser.getChildren()) if (auto* b = dynamic_cast<juce::Button*>(c); b && b->getButtonText() == "Starter Pack") starter = b;
        require(starter && starter->isEnabled(), "the Starter Pack button is offered");
        browser.setStarterFolder(pack.getChildFile("missing")); require(!starter->isEnabled(), "and disabled when the pack is missing");
    }
    require(root.deleteRecursively(), "remove sample fixtures");
}
