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
