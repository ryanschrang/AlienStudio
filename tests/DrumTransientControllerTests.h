#pragma once
#include "DrumSamplerPanel.h"

static void drumTransientControllerTests() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienAutoSlice", "", false);
    require(root.createDirectory().wasOk(), "auto slice fixture directory");
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 8000; audio->left.resize(8000);
    for (int f : {400, 2000, 4000, 6400}) audio->left[f] = .8f;
    audio->buildWaveform(); std::atomic<bool> cancel{false}; juce::String error;
    const auto file = root.getChildFile("attacks.wav"), silent = root.getChildFile("silence.wav");
    require(alien::writeClipWav(file, *audio, cancel, error), "write auto slice source");
    std::fill(audio->left.begin(), audio->left.end(), 0.f); audio->buildWaveform();
    require(alien::writeClipWav(silent, *audio, cancel, error), "write silent source");
    const auto wait = [](alien::ProjectController& c) {
        for (int i = 0; i < 600 && c.isBusy(); ++i) pump(10);
        require(!c.isBusy(), "auto slice worker finishes");
    };
    {
        ToneEngine engine; alien::ProjectController c(engine);
        require(!c.autoSliceDrum(c.drumSliceTarget(0, 0)), "empty auto slice refuses");
        c.loadDrum(file, 0, 0); wait(c); c.loadDrum(file, 0, 1); wait(c);
        const auto original = c.document().tracks[0]; const auto target = c.drumSliceTarget(0, 0);
        const auto bytes = c.availableAudioBytes();
        engine.recording.phase.store(alien::RecordControls::countIn);
        require(!c.autoSliceDrum(target), "count-in refuses auto slice"); engine.recording.phase.store(alien::RecordControls::idle);
        c.setExternalBusy(true); require(!c.autoSliceDrum(target), "busy auto slice refuses"); c.setExternalBusy(false);
        alien::DrumSamplerPanel panel; panel.setSize(1000, 400);
        std::array<std::array<alien::DrumSliceTarget, alien::drumPadCount>, alien::maxSessionTracks> targets{};
        targets[0][0] = target; targets[0][1] = c.drumSliceTarget(0, 1); panel.setSliceTargets(targets);
        juce::TextButton* button = nullptr; juce::ComboBox* pad = nullptr;
        for (int i = 0; i < panel.getNumChildComponents(); ++i) {
            auto* child = panel.getChildComponent(i);
            if (child->getName() == "Auto slice sample at attacks") button = dynamic_cast<juce::TextButton*>(child);
            if (child->getName() == "Drum pad to load") pad = dynamic_cast<juce::ComboBox*>(child);
        }
        require(button && pad && button->getWidth() > 0 && button->isEnabled(), "auto slice action exists and loaded target enables it");
        panel.setBusy(true); require(!button->isEnabled(), "busy auto slice UI disabled"); panel.setBusy(false);
        pad->setSelectedId(2, juce::sendNotificationSync);
        panel.onAutoSlice = [&](const alien::DrumSliceTarget& selected) {
            require(selected == targets[0][1], "pad selection refreshes captured auto slice target");
            require(c.autoSliceDrum(selected), "UI starts auto slicing");
        };
        button->onClick(); require(c.isBusy() && c.document().tracks[0].drumAssets == original.drumAssets, "worker starts without publishing partial state");
        require(!c.autoSliceDrum(target), "duplicate requests refuse while worker runs"); wait(c);
        const auto kit = c.document().tracks[0];
        for (int p = 0; p < 4; ++p) {
            require(kit.drumAssets[p] && kit.drumAssets[p]->sliceSource == original.drumAssets[1], "auto slices share selected source");
            require(engine.drumKits[0].samples[p].load() == kit.drumAssets[p].get(), "auto slice publication reaches engine");
        }
        require(kit.drumSlices[0] == alien::DrumSlice{0, 2000} && kit.drumSlices[3] == alien::DrumSlice{6400, 8000}, "auto boundaries differ from equal slicing");
        for (int p = 4; p < 8; ++p) require(!kit.drumAssets[p] && kit.drumPaths[p].isEmpty() && kit.drumSlices[p].whole(), "unused pads cleared consistently");
        require(c.availableAudioBytes() == bytes, "auto slices retain no new PCM");
        c.undo(); require(c.document().tracks[0].drumAssets == original.drumAssets, "one Undo restores entire original kit");
        c.redo(); require(c.document().tracks[0].drumAssets == kit.drumAssets, "Redo restores exact auto slice views");
        const auto project = root.getChildFile("Auto.alienstudio"); bool saved = false, ok = false;
        c.writeProject(project, false, [&](bool value) { saved = true; ok = value; });
        for (int i = 0; i < 600 && !saved; ++i) pump(10);
        require(saved && ok, "save auto sliced project");
        c.openProject(project); wait(c);
        require(c.document().tracks[0].drumSlices == kit.drumSlices && c.document().tracks[0].drumAssets[3]->frames == 1600, "auto slice boundaries survive save/reopen");
        require(!c.autoSliceDrum(target), "reopened project refuses old request target");
        c.loadDrum(file, 0, 0); wait(c);
        require(c.editDrumSlice(c.drumSliceTarget(0, 0), {2000, 8000}), "select subrange for detection");
        require(c.autoSliceDrum(c.drumSliceTarget(0, 0)), "auto slice selected view"); wait(c);
        require(c.document().tracks[0].drumSlices[0] == alien::DrumSlice{2000, 4000}
            && c.document().tracks[0].drumSlices[2] == alien::DrumSlice{6400, 8000}, "view-local detections map to original source frames");
        c.loadDrum(silent, 0, 0); wait(c); c.clearDrum(0, 1); c.undo();
        const auto before = c.document().tracks[0]; const auto redo = c.redoLabel();
        require(c.autoSliceDrum(c.drumSliceTarget(0, 0)), "silent analysis can start"); wait(c);
        require(c.document().tracks[0].drumAssets == before.drumAssets && c.redoLabel() == redo, "no useful attacks preserve kit and Redo");
        c.loadDrum(file, 0, 0); wait(c);
        require(c.autoSliceDrum(c.drumSliceTarget(0, 0)), "start stale-kit analysis");
        c.clearDrum(0, 1); const auto changed = c.document().tracks[0]; wait(c);
        require(c.document().tracks[0].drumAssets == changed.drumAssets, "kit change during worker refuses entire result");
        require(c.autoSliceDrum(c.drumSliceTarget(0, 0)), "start recording-transition analysis");
        engine.recording.phase.store(alien::RecordControls::countIn); wait(c);
        require(c.document().tracks[0].drumAssets == changed.drumAssets, "recording transition refuses prepared result");
        engine.recording.phase.store(alien::RecordControls::idle);
        require(c.autoSliceDrum(c.drumSliceTarget(0, 0)), "start project-transition analysis");
        c.newProject(); wait(c);
        require(!c.document().tracks[0].drumAssets[0], "new project cannot receive stale slices");
        pump(100); // drain an already queued completion even if project reset busy
        require(!c.document().tracks[0].drumAssets[0], "queued old result stays refused");
    }
    root.deleteRecursively();
}
