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
        for (auto* child : browser.getChildren()) if (auto* combo = dynamic_cast<juce::ComboBox*>(child)) combo->setSelectedId(2);
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
        browser.setImportEnabled(true, true); import->onClick(); require(imports == 1, "import requires explicit action");
        search->setText("no match", false); search->onTextChange(); require(browser.dragDescriptionFor(0).isVoid(), "empty filtered list has no stale drag payload"); require(browser.resultCount() == 0 && !import->isEnabled() && !preview->isEnabled(), "empty search clears stale actions");
    }
    require(root.deleteRecursively(), "remove sample fixtures");
}
