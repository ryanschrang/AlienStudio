#pragma once
#include "DrumSamplerPanel.h"

static void drumSliceControllerTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienSliceTest", "", false);
    require(root.createDirectory().wasOk(), "slice fixture directory");
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 2; audio->frames = 83;
    for (int i = 0; i < 83; ++i) { audio->left.push_back((i + 1) / 100.f); audio->right.push_back(-(i + 1) / 100.f); }
    audio->buildWaveform();
    const auto wav = root.getChildFile("source.wav"), project = root.getChildFile("Kit.alienstudio");
    std::atomic<bool> cancel{false}; juce::String error;
    require(alien::writeClipWav(wav, *audio, cancel, error), "write slice source");
    const auto wait = [](alien::ProjectController& c) {
        for (int i = 0; i < 400 && c.isBusy(); ++i) pump(10);
        require(!c.isBusy(), "sampler work completes");
    };
    {
        ToneEngine engine; alien::ProjectController c(engine);
        require(!c.sliceDrum(0, 0) && !c.sliceDrum(-1, 0) && !c.sliceDrum(0, 8), "missing and invalid slice targets refuse");
        c.loadDrum(wav, 0, 0); require(!c.sliceDrum(0, 0), "busy slice refuses"); wait(c);
        const auto whole = c.document().tracks[0].drumAssets[0]; const auto bytes = c.availableAudioBytes();
        engine.recording.phase.store(alien::RecordControls::countIn);
        require(!c.sliceDrum(0, 0), "count-in slice refuses"); engine.recording.phase.store(alien::RecordControls::idle);
        alien::DrumSamplerPanel panel; panel.setSize(1000, 400);
        panel.onSlice = [&](int track, int pad) { require(c.sliceDrum(track, pad), "sampler button slices selected sample"); };
        juce::TextButton* button = nullptr;
        for (int i = 0; i < panel.getNumChildComponents(); ++i) if (panel.getChildComponent(i)->getName() == "Slice sample to 8 pads") button = dynamic_cast<juce::TextButton*>(panel.getChildComponent(i));
        require(button && button->getBounds().getWidth() > 0, "visible slice action exists");
        panel.setBusy(true); require(!button->isEnabled(), "busy panel disables slice action"); panel.setBusy(false);
        button->onClick();
        const auto kit = c.document().tracks[0];
        require(c.availableAudioBytes() == bytes, "slicing adds no PCM memory charge");
        for (int p = 0; p < 8; ++p) {
            require(kit.drumSlices[p] == alien::DrumSlice{83 * p / 8, 83 * (p + 1) / 8}, "uneven source partitions cover every frame");
            require(kit.drumAssets[p]->sliceSource == whole && kit.drumAssets[p]->decodedBytes() == 0, "controller slices share one source");
            require(engine.drumKits[0].samples[p].load() == kit.drumAssets[p].get(), "engine receives slice views");
        }
        c.undo(); require(c.document().tracks[0].drumAssets[0] == whole && !c.document().tracks[0].drumAssets[1], "one Undo restores entire previous kit");
        c.redo(); require(c.document().tracks[0].drumAssets == kit.drumAssets && c.document().tracks[0].drumSlices == kit.drumSlices, "Redo restores slices and shared identities");
        c.clearDrum(0, 1); require(c.document().tracks[0].drumSlices[1].whole() && !c.document().tracks[0].drumAssets[1], "clearing pad clears boundaries"); c.undo();
        c.loadDrum(wav, 0, 1); wait(c); require(c.document().tracks[0].drumSlices[1].whole() && !c.document().tracks[0].drumAssets[1]->sliceSource, "loading new sample resets boundaries"); c.undo();
        const auto sliceWav = root.getChildFile("slice.wav");
        require(alien::writeClipWav(sliceWav, *kit.drumAssets[7], cancel, error), "slice can be written without copying source PCM");
        const auto rendered = alien::decodeSessionAudioClip(sliceWav);
        require(rendered.clip && rendered.clip->frames == kit.drumAssets[7]->frames && std::abs(rendered.clip->left[0] - whole->left[kit.drumSlices[7].start]) < 1e-5, "written slice starts and ends at saved boundaries");
        bool done = false, ok = false;
        c.writeProject(project, false, [&](bool value) { ok = value; done = true; });
        for (int i = 0; i < 400 && !done; ++i) pump(10);
        require(done && ok, "save sliced kit");
    }
    const auto checkKit = [](const alien::ProjectController& c) {
        const auto& kit = c.document().tracks[0];
        const auto source = kit.drumAssets[0]->sliceSource;
        for (int p = 0; p < 8; ++p)
            require(kit.drumAssets[p] && kit.drumAssets[p]->sliceSource == source && kit.drumSlices[p] == alien::DrumSlice{83 * p / 8, 83 * (p + 1) / 8}, "reopened or relinked slices retain shared source and boundaries");
    };
    {
        ToneEngine engine; alien::ProjectController c(engine); const auto bytes = c.availableAudioBytes();
        c.openProject(project); wait(c); checkKit(c);
        require(bytes - c.availableAudioBytes() == audio->decodedBytes(), "reopen decodes and charges shared source once");
        require(c.sliceDrum(0, 7), "a selected slice can itself be subdivided");
        require(c.document().tracks[0].drumSlices[0].start == 72 && c.document().tracks[0].drumSlices[7].end == 83 && !c.document().tracks[0].drumAssets[0]->sliceSource->sliceSource, "reslicing flattens absolute boundaries into original source");
    }
    const auto moved = root.getChildFile("moved"); require(moved.createDirectory().wasOk() && wav.moveFileTo(moved.getChildFile("source.wav")), "move source away");
    {
        ToneEngine engine; alien::ProjectController c(engine); c.openProject(project); wait(c);
        require(!c.document().tracks[0].drumAssets[0] && c.document().tracks[0].drumSlices[0] == alien::DrumSlice{0, 10}, "missing source preserves saved boundaries offline");
        c.relinkMissingAudio(moved); wait(c); checkKit(c);
        c.undo(); require(!c.document().tracks[0].drumAssets[0], "relink is one Undo"); c.redo(); checkKit(c);
    }
    audio->frames = 4; audio->left.resize(4); audio->right.resize(4); audio->buildWaveform();
    require(alien::writeClipWav(moved.getChildFile("source.wav"), *audio, cancel, error), "write shorter relink candidate");
    {
        ToneEngine engine; alien::ProjectController c(engine); c.openProject(project); wait(c);
        const auto bytes = c.availableAudioBytes(); c.relinkMissingAudio(moved); wait(c);
        for (const auto& asset : c.document().tracks[0].drumAssets) require(!asset, "shorter relink cannot silently expand or truncate slices");
        require(c.availableAudioBytes() == bytes, "rejected relink does not retain source audio");
    }
    root.deleteRecursively();
}

static void drumSliceBoundaryTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienSliceBoundaryTest", "", false);
    require(root.createDirectory().wasOk(), "boundary fixture directory");
    auto audio = std::make_shared<alien::SessionAudioClip>(); audio->sampleRate = 8000; audio->channels = 2; audio->frames = 83;
    for (int i = 0; i < 83; ++i) { audio->left.push_back(i / 100.f); audio->right.push_back(-i / 100.f); } audio->buildWaveform();
    const auto file = root.getChildFile("sample.wav"), project = root.getChildFile("Edited.alienstudio");
    std::atomic<bool> cancel{false}; juce::String error;
    require(alien::writeClipWav(file, *audio, cancel, error), "write boundary source");
    {
        ToneEngine engine; alien::ProjectController c(engine);
        const auto wait = [&] { for (int i = 0; i < 400 && c.isBusy(); ++i) pump(10); require(!c.isBusy(), "boundary worker finishes"); };
        require(!c.editDrumSlice(c.drumSliceTarget(0, 0), {1, 2}), "offline boundary edits refuse");
        c.loadDrum(file, 0, 0); wait(); c.loadDrum(file, 0, 1); wait();
        const auto original = c.document().tracks[0].drumAssets[0], neighbour = c.document().tracks[0].drumAssets[1];
        const auto target = c.drumSliceTarget(0, 0); const auto bytes = c.availableAudioBytes(); const auto history = c.undoLabel();
        for (auto bad : {alien::DrumSlice{-1, 2}, {2, 2}, {3, 1}, {1, 0}, {0, 84}}) require(!c.editDrumSlice(target, bad), "invalid boundary edits refuse");
        require(c.undoLabel() == history && c.document().tracks[0].drumAssets[0] == original, "refusal precedes history and engine changes");
        c.setExternalBusy(true); require(!c.editDrumSlice(target, {5, 19}), "busy range refuses"); c.setExternalBusy(false);
        engine.recording.phase.store(alien::RecordControls::countIn); require(!c.editDrumSlice(target, {5, 19}), "recording count-in range refuses"); engine.recording.phase.store(alien::RecordControls::idle);
        alien::DrumSamplerPanel panel; panel.setSize(1000, 400);
        const auto refresh = [&] {
            std::array<std::array<juce::String, alien::drumPadCount>, alien::maxSessionTracks> paths{};
            std::array<std::array<alien::DrumSlice, alien::drumPadCount>, alien::maxSessionTracks> ranges{};
            std::array<std::array<alien::DrumSliceTarget, alien::drumPadCount>, alien::maxSessionTracks> targets{};
            juce::StringArray names;
            for (int t = 0; t < c.document().trackCount(); ++t) {
                names.add(c.document().tracks[t].name); paths[t] = c.document().tracks[t].drumPaths; ranges[t] = c.document().tracks[t].drumSlices;
                for (int p = 0; p < alien::drumPadCount; ++p) targets[t][p] = c.drumSliceTarget(t, p);
            }
            panel.setTrackNames(names); panel.setPaths(paths, ranges); panel.setSliceTargets(targets);
        };
        c.onDocumentRestored = refresh; refresh();
        int calls = 0;
        panel.onApplySlice = [&](const alien::DrumSliceTarget& expected, alien::DrumSlice range) { ++calls; return c.editDrumSlice(expected, range); };
        const auto child = [&](const char* name) -> juce::Component* {
            for (int i = 0; i < panel.getNumChildComponents(); ++i) if (panel.getChildComponent(i)->getName() == name) return panel.getChildComponent(i);
            require(false, "boundary control exists"); return nullptr;
        };
        auto* start = dynamic_cast<juce::TextEditor*>(child("Slice start frame")); auto* end = dynamic_cast<juce::TextEditor*>(child("Slice end frame"));
        auto* apply = dynamic_cast<juce::TextButton*>(child("Apply drum slice range")); auto* whole = dynamic_cast<juce::TextButton*>(child("Restore whole drum sample"));
        auto* waveform = child("Drum slice waveform");
        require(panel.getLocalBounds().contains(waveform->getBounds()) && waveform->getHeight() > 40,
            "the waveform fits in the drum panel");
        const auto highlightPixel = [&](double position) {
            const auto image = waveform->createComponentSnapshot(waveform->getLocalBounds());
            const int x = 6 + static_cast<int>(position * (image.getWidth() - 12));
            return image.getPixelAt(x, image.getHeight() - 6);
        };
        const auto wholeHighlight = highlightPixel(.8);
        require(start && end && apply && whole && apply->isEnabled() && start->getText() == "0" && end->getText() == "83", "loaded range controls show source frames");
        for (auto* control : std::array<juce::Component*, 4>{start, end, apply, whole}) require(panel.getLocalBounds().contains(control->getBounds()), "boundary controls laid out within panel");
        for (const char* invalid : {"", "-1", "3.5", "text", "9999999999999999"}) {
            start->setText(invalid, false); end->setText("19", false); apply->onClick(); require(calls == 0, "invalid numeric text cannot submit an edit");
        }
        start->setText("0", false); end->setText("0", false); apply->onClick(); require(calls == 0, "empty interval is not a whole-sample action");
        start->setText("5", false); end->setText("19", false); refresh();
        require(start->getText() == "5" && end->getText() == "19", "unrelated refresh preserves range draft");
        panel.setBusy(true); require(!apply->isEnabled() && !whole->isEnabled(), "busy disables range controls"); apply->onClick(); require(calls == 0, "busy action cannot submit"); panel.setBusy(false);
        apply->onClick();
        const auto edited = c.document().tracks[0].drumAssets[0];
        require(calls == 1 && c.document().tracks[0].drumSlices[0] == alien::DrumSlice{5, 19} && edited->sliceSource == original && edited->frames == 14, "Apply edits selected pad using shared original source");
        require(highlightPixel(.14) == wholeHighlight && highlightPixel(.8) == alien::ui::colours::inset,
            "the applied slice highlights its original source interval");
        require(c.document().tracks[0].drumAssets[1] == neighbour && c.availableAudioBytes() == bytes && engine.drumKits[0].samples[0].load() == edited.get(), "other pads and PCM budget stay unchanged while engine gets edited view");
        require(!c.editDrumSlice(target, {6, 20}), "replaced view refuses stale target");
        c.undo(); require(c.document().tracks[0].drumAssets[0] == original, "one Undo restores previous range");
        require(highlightPixel(.8) == wholeHighlight, "Undo restores the full waveform highlight");
        const auto redo = c.redoLabel();
        require(c.editDrumSlice(c.drumSliceTarget(0, 0), {0, 83}) && c.redoLabel() == redo && c.document().tracks[0].drumSlices[0].whole(), "full-source no-op preserves redo and canonical whole state");
        c.redo(); require(c.document().tracks[0].drumAssets[0] == edited, "Redo restores edited range identity");
        whole->onClick(); require(c.document().tracks[0].drumAssets[0] == original && c.document().tracks[0].drumSlices[0].whole(), "Whole sample restores original source without copying"); c.undo();
        require(c.document().tracks[0].drumSlices[0] == alien::DrumSlice{5, 19}, "Undo whole action restores edited boundaries");
        require(c.editDrumSlice(c.drumSliceTarget(0, 0), {1, 82}) && c.document().tracks[0].drumAssets[0]->frames == 81, "range can expand beyond its prior view using original source"); c.undo();
        require(c.editDrumSlice(c.drumSliceTarget(0, 0), {82, 83}) && c.document().tracks[0].drumAssets[0]->frames == 1, "one-frame slice at source end is valid"); c.undo();
        const auto output = root.getChildFile("edited-slice.wav");
        require(alien::writeClipWav(output, *c.document().tracks[0].drumAssets[0], cancel, error), "write manually edited view");
        const auto decoded = alien::decodeSessionAudioClip(output);
        require(decoded.clip && decoded.clip->frames == 14 && std::abs(decoded.clip->left.front() - .05f) < 1e-5 && std::abs(decoded.clip->right.back() + .18f) < 1e-5, "edited source-frame range controls exported stereo PCM");
        c.onDocumentRestored = {}; c.loadDrum(file, 0, 0); wait();
        const auto replacement = c.document().tracks[0].drumAssets[0]; const auto beforeStale = c.undoLabel();
        start->setText("6", false); end->setText("20", false); apply->onClick();
        require(c.document().tracks[0].drumAssets[0] == replacement && c.undoLabel() == beforeStale, "stale panel refuses replacement sample without history edits");
        c.onDocumentRestored = refresh; c.undo();
        bool done = false, ok = false; c.writeProject(project, false, [&](bool value) { done = true; ok = value; });
        for (int i = 0; i < 400 && !done; ++i) pump(10); require(done && ok, "save edited boundaries");
        const auto oldTarget = c.drumSliceTarget(0, 0); c.openProject(project); wait();
        require(c.document().tracks[0].drumSlices[0] == alien::DrumSlice{5, 19} && c.document().tracks[0].drumAssets[0]->frames == 14, "reopen restores edited boundaries and playable view");
        require(!c.editDrumSlice(oldTarget, {6, 20}), "project replacement refuses old editing target");
        const auto movedTarget = c.drumSliceTarget(0, 0); require(c.moveTrack(0, 1), "move target track");
        require(c.editDrumSlice(movedTarget, {6, 20}) && c.document().tracks[1].drumSlices[0] == alien::DrumSlice{6, 20}, "stable target follows track reorder");
        const auto deletedTarget = c.drumSliceTarget(1, 0); require(c.deleteTrack(1) && !c.editDrumSlice(deletedTarget, {7, 21}), "deleted target refuses");
        c.onDocumentRestored = {};
    }
    {
        ToneEngine engine; alien::ProjectController frozen(engine);
        frozen.loadDrum(file, 0, 0); for (int i = 0; i < 400 && frozen.isBusy(); ++i) pump(10);
        require(!frozen.isBusy(), "freeze fixture sample loaded");
        frozen.setClipPattern(0, 0, patternWith(36), false);
        require(frozen.editTimeline("Place freeze source", [](alien::timeline::Timeline& timeline) { return timeline.placeBars(0, 0, 0, 1); }), "place freeze fixture");
        const auto before = frozen.drumSliceTarget(0, 0); const auto expected = frozen.timeline();
        require(frozen.installFrozenTrack(0, file, audio, expected), "freeze sampler track");
        require(!frozen.editDrumSlice(before, {1, 20}), "frozen track refuses formerly editable target");
        require(!frozen.autoSliceDrum(before), "frozen track refuses auto slicing");
        alien::DrumSamplerPanel panel;
        std::array<std::array<alien::DrumSliceTarget, alien::drumPadCount>, alien::maxSessionTracks> targets{};
        targets[0][0] = frozen.drumSliceTarget(0, 0); panel.setSliceTargets(targets);
        bool disabled = false;
        for (int i = 0; i < panel.getNumChildComponents(); ++i) if (panel.getChildComponent(i)->getName() == "Apply drum slice range") disabled = !panel.getChildComponent(i)->isEnabled();
        require(disabled, "frozen panel disables boundary action");
        for (int i = 0; i < panel.getNumChildComponents(); ++i) if (panel.getChildComponent(i)->getName() == "Auto slice sample at attacks")
            require(!panel.getChildComponent(i)->isEnabled(), "frozen panel disables auto slicing");
    }
    root.deleteRecursively();
}
