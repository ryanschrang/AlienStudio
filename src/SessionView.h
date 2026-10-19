#pragma once
#include "ProjectController.h"
#include "SessionClipButton.h"
#include "AudioClipWaveform.h"
#include "StudioWidgets.h"
#include "SynthEngine.h"
#include <functional>
#include <memory>
#include <vector>

namespace alien {

// Session (clip launcher) page: track headers, the clip grid, scene buttons,
// clip actions, MIDI CC mapping and the selected clip's detail view.
class SessionView final : public juce::Component {
public:
    SessionView(ProjectController& projectController, ToneEngine& toneEngine)
        : controller(projectController), engine(toneEngine) {
        namespace col = ui::colours;
        addAndMakeVisible(sessionHelp);
        sessionHelp.setText("Click a clip to launch it, right-click to edit. Launch a scene to switch every track together.",
                            juce::dontSendNotification);
        sessionHelp.setFont(ui::font(12.5f));
        sessionHelp.setColour(juce::Label::textColourId, col::textFaint);
        addAndMakeVisible(launchTimingLabel);
        launchTimingLabel.setText("LAUNCH", juce::dontSendNotification);
        launchTimingLabel.setFont(ui::font(10.5f, true));
        launchTimingLabel.setColour(juce::Label::textColourId, col::textFaint);
        launchTimingLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(launchTiming);
        launchTiming.setName("Session launch quantization");
        launchTiming.setTooltip("Choose whether queued clips and scenes launch on the next beat or bar");
        launchTiming.addItem("Beat", 1);
        launchTiming.addItem("Bar", 2);
        launchTiming.onChange = [this] {
            controller.setLaunchQuantization(launchTiming.getSelectedId() == 1 ? 4 : 16);
            if (onTitleChanged) onTitleChanged();
            if (onEdited) onEdited();
        };

        addAndMakeVisible(controllerTarget);
        controllerTarget.setName("MIDI controller mapping action");
        controllerTarget.setTooltip("Choose a Session action to assign to a MIDI CC");
        for (int i = 0; i < MidiControllerMappings::sceneCount; ++i)
            controllerTarget.addItem("Scene " + juce::String(i + 1), i + 1);
        controllerTarget.addItem("Stop all", MidiControllerMappings::stopAllAction + 1);
        controllerTarget.setSelectedId(1, juce::dontSendNotification);
        controllerTarget.onChange = [this] { updateControllerMappingLabel(); };
        addAndMakeVisible(learnController);
        learnController.setButtonText("Learn CC");
        learnController.setGlyph(ui::Glyph::learn);
        learnController.setName("Learn MIDI controller");
        learnController.setTooltip("Listen for the next incoming MIDI CC and assign it to the selected Session action");
        learnController.setColour(juce::TextButton::buttonOnColourId, col::warning);
        learnController.onClick = [this] {
            const int action = controllerTarget.getSelectedId() - 1;
            if (learningAction == action) {
                engine.controllerMappings.cancelLearn();
                setLearning(-1);
            } else {
                learningAction = action;
                engine.controllerMappings.beginLearn(action);
                learnController.setButtonText("Cancel learn");
                learnController.setToggleState(true, juce::dontSendNotification);
            }
            updateControllerMappingLabel();
        };
        addAndMakeVisible(clearController);
        clearController.setButtonText("Unmap");
        clearController.setName("Clear MIDI controller mapping");
        clearController.setTooltip("Remove the MIDI CC assigned to the selected action (enabled once a CC is mapped)");
        clearController.onClick = [this] {
            engine.controllerMappings.cancelLearn();
            engine.controllerMappings.clearMapping(controllerTarget.getSelectedId() - 1);
            setLearning(-1);
            updateControllerMappingLabel();
        };
        addAndMakeVisible(controllerMappingLabel);
        controllerMappingLabel.setName("MIDI controller assignment");
        controllerMappingLabel.setFont(ui::monoFont(12.0f));
        controllerMappingLabel.setColour(juce::Label::textColourId, col::textDim);
        controllerMappingLabel.setJustificationType(juce::Justification::centredLeft);

        addAndMakeVisible(stopClip);
        stopClip.setButtonText("Stop all");
        stopClip.setGlyph(ui::Glyph::stop);
        stopClip.setName("Stop all Session tracks at selected launch boundary");
        stopClip.setTooltip("Stop every Session track at the next launch boundary");
        stopClip.onClick = [this] { controller.stopAllTracks(); refreshClips(); };
        addAndMakeVisible(editSelectedClip);
        editSelectedClip.setButtonText("Edit clip");
        editSelectedClip.setGlyph(ui::Glyph::edit);
        editSelectedClip.setName("Edit selected Session clip");
        editSelectedClip.setTooltip("Open the selected MIDI clip in the piano roll or show its audio waveform");
        editSelectedClip.onClick = [this] {
            const auto& d = controller.document();
            if (onEditClip) onEditClip(d.selectedTrack, d.selectedScene);
        };
        addAndMakeVisible(importAudio);
        importAudio.setButtonText("Import audio");
        importAudio.setGlyph(ui::Glyph::waveform);
        importAudio.setName("Import audio file to selected Session clip");
        importAudio.setTooltip("Decode a WAV or MP3 off the audio thread and add it to the selected clip");
        importAudio.onClick = [this] { controller.chooseAudioFile(); };
        addAndMakeVisible(removeAudio);
        removeAudio.setButtonText("Remove audio");
        removeAudio.setGlyph(ui::Glyph::close);
        removeAudio.setName("Remove audio from selected Session clip");
        removeAudio.setTooltip("Remove the audio file from the selected clip");
        removeAudio.onClick = [this] {
            const auto& d = controller.document();
            if (controller.removeAudio(d.selectedTrack, d.selectedScene)) {
                setImportStatus("Audio removed from selected clip", {});
                refreshSelectedClip();
                if (onEdited) onEdited();
            }
        };
        addAndMakeVisible(clearClip);
        clearClip.setButtonText("Clear clip");
        clearClip.setGlyph(ui::Glyph::close);
        clearClip.setName("Clear selected Session clip");
        clearClip.setTooltip("Remove all notes and audio from the selected clip. Notes can be restored with Undo in the piano roll.");
        clearClip.onClick = [this] { if (onClearClip) onClearClip(); };
        addAndMakeVisible(audioImportStatus);
        audioImportStatus.setFont(ui::font(12.0f));
        audioImportStatus.setColour(juce::Label::textColourId, col::textDim);
        audioImportStatus.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(audioWaveform);
        rebuildGrid();
        refreshFromDocument();
        updateControllerMappingLabel();
    }

    // Navigation and edit notifications for the owner.
    std::function<void(int, int)> onEditClip;        // open clip in its editor
    std::function<void()> onSelectionChanged;        // selected clip changed
    std::function<void()> onClearClip;               // clear the selected clip
    std::function<void()> onTitleChanged;            // header subtitle text changed
    std::function<void()> onEdited;                  // a committed document edit

    // Rebuild buttons when the number of tracks or scenes changes.
    void rebuildGrid() {
        const auto& d = controller.document();
        for (auto& b : clipButtons) removeChildComponent(b.get());
        for (auto& b : sceneButtons) removeChildComponent(b.get());
        clipButtons.clear();
        sceneButtons.clear();
        for (int scene = 0; scene < d.sceneCount(); ++scene) {
            auto sceneButton = std::make_unique<ui::GlyphButton>(ui::Glyph::play);
            sceneButton->setName("Launch scene " + juce::String(scene + 1));
            sceneButton->setButtonText(d.scenes[static_cast<std::size_t>(scene)].name);
            sceneButton->setTooltip("Launch every track's clip in scene " + juce::String(scene + 1));
            sceneButton->onClick = [this, scene] { controller.launchScene(scene); refreshClips(); };
            addAndMakeVisible(*sceneButton);
            sceneButtons.push_back(std::move(sceneButton));
            for (int track = 0; track < d.trackCount(); ++track) {
                auto button = std::make_unique<SessionClipButton>();
                button->setName("Launch track " + juce::String(track + 1) + " scene " + juce::String(scene + 1));
                button->setTooltip("Track " + juce::String(track + 1) + ", scene " + juce::String(scene + 1)
                                   + ": click to launch, right-click to edit");
                button->onClick = [this, track, scene] {
                    controller.launchClip(track, scene);
                    selectionChanged();
                };
                button->onRightClick = [this, track, scene] {
                    controller.selectClip(track, scene);
                    selectionChanged();
                    if (onEditClip) onEditClip(track, scene);
                };
                addAndMakeVisible(*button);
                clipButtons.push_back(std::move(button));
            }
        }
        resized();
    }

    void refreshFromDocument() {
        launchTiming.setSelectedId(controller.document().launchQuantizationSteps == 4 ? 1 : 2, juce::dontSendNotification);
        refreshSelectedClip();
        refreshClips();
        repaint();
    }

    // After the selected clip's content or the selection changes.
    void refreshSelectedClip() {
        const auto& d = controller.document();
        const auto& clip = d.selectedClip();
        clearClip.setEnabled(!clip.isEmpty());
        removeAudio.setEnabled(clip.hasAudio());
        audioWaveform.setClip(clip.audio.get(), clip.audioFileName);
        if (!clip.hasAudio() && audioImportStatus.getText().isEmpty())
            audioImportStatus.setText("Selected clip: Track " + juce::String(d.selectedTrack + 1)
                + ", Scene " + juce::String(d.selectedScene + 1), juce::dontSendNotification);
        repaint(clipDetailBounds);
        refreshClips();
    }

    void setImportStatus(const juce::String& text, const juce::String& tooltip) {
        audioImportStatus.setText(text, juce::dontSendNotification);
        audioImportStatus.setTooltip(tooltip);
    }

    void setBusy(bool busy) { importAudio.setEnabled(!busy); }

    juce::String subtitle() const {
        const auto& d = controller.document();
        return juce::String(d.trackCount()) + " tracks" + dot() + juce::String(d.sceneCount()) + " scenes"
            + dot() + "launch on next " + (d.launchQuantizationSteps == 4 ? "beat" : "bar");
    }

    // 20 Hz from the owner: live clip states and MIDI-learn feedback.
    void tick() {
        refreshClips();
        if (isShowing()) repaint(trackHeaderBounds);
        const int learned = engine.controllerMappings.takeLearnedAction();
        if (learned >= 0) {
            if (learningAction == learned) setLearning(-1);
            updateControllerMappingLabel();
        }
    }

    void resized() override {
        const auto& d = controller.document();
        const int tracks = juce::jmax(1, d.trackCount()), scenes = juce::jmax(1, d.sceneCount());
        auto r = getLocalBounds().reduced(16, 12);
        auto header = r.removeFromTop(30);
        launchTiming.setBounds(header.removeFromRight(96).reduced(0, 2));
        launchTimingLabel.setBounds(header.removeFromRight(70));
        sessionHelp.setBounds(header);
        r.removeFromTop(8);

        const int trackWidth = (r.getWidth() - sceneColumn - gap * tracks) / tracks;
        const int clipHeight = juce::jlimit(30, 40, (r.getHeight() - 30 - 40 - 50 - 90) / scenes - gap);
        trackHeaderBounds = r.withHeight(headerHeight);
        const int gridTop = r.getY() + headerHeight + gap;
        for (int scene = 0; scene < static_cast<int>(sceneButtons.size()); ++scene) {
            const int y = gridTop + scene * (clipHeight + gap);
            for (int track = 0; track < d.trackCount(); ++track)
                if (auto* b = clipButton(track, scene))
                    b->setBounds(r.getX() + track * (trackWidth + gap), y, trackWidth, clipHeight);
            sceneButtons[static_cast<std::size_t>(scene)]->setBounds(r.getRight() - sceneColumn, y, sceneColumn, clipHeight);
        }
        const int stopY = gridTop + scenes * (clipHeight + gap);
        stopClip.setBounds(r.getRight() - sceneColumn, stopY, sceneColumn, 28);
        gridBounds = juce::Rectangle<int>(r.getX(), r.getY(), r.getWidth(), stopY + 28 - r.getY());
        r.setTop(stopY + 28 + 14);

        auto actions = r.removeFromTop(30);
        editSelectedClip.setBounds(actions.removeFromLeft(104));
        actions.removeFromLeft(6);
        importAudio.setBounds(actions.removeFromLeft(124));
        actions.removeFromLeft(6);
        removeAudio.setBounds(actions.removeFromLeft(124));
        actions.removeFromLeft(6);
        clearClip.setBounds(actions.removeFromLeft(104));
        actions.removeFromLeft(18);
        mappingBounds = actions;
        actions.removeFromLeft(66); // "MIDI map" caption.
        controllerTarget.setBounds(actions.removeFromLeft(92));
        actions.removeFromLeft(6);
        learnController.setBounds(actions.removeFromLeft(112));
        actions.removeFromLeft(6);
        clearController.setBounds(actions.removeFromLeft(66));
        actions.removeFromLeft(10);
        controllerMappingLabel.setBounds(actions);
        r.removeFromTop(14);

        clipDetailBounds = r;
        auto detail = r.reduced(12, 8);
        auto detailHeader = detail.removeFromTop(20);
        audioImportStatus.setBounds(detailHeader.removeFromRight(juce::jmax(100, detailHeader.getWidth() - 220)));
        detail.removeFromTop(6);
        audioWaveform.setBounds(detail);
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        const auto& d = controller.document();
        g.fillAll(col::surface);
        const int tracks = juce::jmax(1, d.trackCount());
        const int trackWidth = (gridBounds.getWidth() - sceneColumn - gap * tracks) / tracks;
        for (int track = 0; track < d.trackCount(); ++track) {
            auto h = juce::Rectangle<int>(gridBounds.getX() + track * (trackWidth + gap), trackHeaderBounds.getY(),
                                          trackWidth, trackHeaderBounds.getHeight()).toFloat();
            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.fillRoundedRectangle(h.withBottom(static_cast<float>(gridBounds.getBottom())), 5.0f);
            g.setColour(col::raised);
            g.fillRoundedRectangle(h, 5.0f);
            g.setColour(ui::trackColour(track));
            g.fillRoundedRectangle(h.removeFromTop(3.0f), 1.5f);
            const bool active = track < static_cast<int>(engine.activePianoRollClip.size())
                && engine.activePianoRollClip[static_cast<std::size_t>(track)].load() >= 0;
            auto text = h.reduced(10.0f, 0.0f);
            if (active) {
                g.setColour(col::accent);
                juce::Path p;
                const auto tri = text.removeFromRight(10.0f).withSizeKeepingCentre(8.0f, 9.0f);
                p.addTriangle(tri.getX(), tri.getY(), tri.getX(), tri.getBottom(), tri.getRight(), tri.getCentreY());
                g.fillPath(p);
            }
            g.setColour(col::text);
            g.setFont(ui::font(13.0f, true));
            g.drawText(juce::String(track + 1), text.removeFromLeft(16.0f), juce::Justification::centredLeft, false);
            g.setColour(col::textDim);
            g.setFont(ui::font(12.5f));
            g.drawText(shortPresetName(d.tracks[static_cast<std::size_t>(track)].settings.preset), text,
                       juce::Justification::centredLeft, true);
        }
        auto sceneHeader = juce::Rectangle<int>(gridBounds.getRight() - sceneColumn, trackHeaderBounds.getY(),
                                                sceneColumn, trackHeaderBounds.getHeight());
        g.setColour(col::raised);
        g.fillRoundedRectangle(sceneHeader.toFloat(), 5.0f);
        ui::drawCaption(g, "Scenes", sceneHeader.reduced(10, 0), col::textDim);

        ui::drawCaption(g, "MIDI map", mappingBounds.withWidth(62), col::textFaint);
        g.setColour(col::line);
        g.fillRect(mappingBounds.getX() - 10, mappingBounds.getY() + 5, 1, mappingBounds.getHeight() - 10);

        ui::drawCard(g, clipDetailBounds.toFloat(), col::raised);
        ui::drawCaption(g, "Clip" + dot() + "Track " + juce::String(d.selectedTrack + 1) + dot() + "Scene "
                               + juce::String(d.selectedScene + 1),
                        clipDetailBounds.reduced(12, 8).removeFromTop(20), ui::trackColour(d.selectedTrack));
    }

    // Live clip and scene button states.
    void refreshClips() {
        namespace col = ui::colours;
        const auto& d = controller.document();
        std::vector<bool> scenePlaying(static_cast<std::size_t>(d.sceneCount()), false);
        for (int track = 0; track < d.trackCount(); ++track) {
            const int active = engine.activePianoRollClip[static_cast<std::size_t>(track)].load();
            if (active >= 0 && active < d.sceneCount()) scenePlaying[static_cast<std::size_t>(active)] = true;
        }
        for (int scene = 0; scene < static_cast<int>(sceneButtons.size()); ++scene) {
            auto& sceneButton = *sceneButtons[static_cast<std::size_t>(scene)];
            const bool playingScene = scenePlaying[static_cast<std::size_t>(scene)];
            const auto sceneColour = playingScene ? juce::Colour(0xff1d3a31)
                : scene == d.selectedScene ? col::hover : col::control;
            if (sceneButton.findColour(juce::TextButton::buttonColourId) != sceneColour) {
                sceneButton.setColour(juce::TextButton::buttonColourId, sceneColour);
                sceneButton.setGlyphColour(playingScene ? col::accent : col::textDim);
            }
            sceneButton.setTitle((scene == d.selectedScene ? "Selected, " : "") + juce::String("Launch scene ")
                                 + juce::String(scene + 1));
            for (int track = 0; track < d.trackCount(); ++track) {
                auto* button = clipButton(track, scene);
                if (button == nullptr) continue;
                const auto index = static_cast<std::size_t>(track);
                const int active = engine.activePianoRollClip[index].load();
                const int requested = engine.requestedPianoRollClip[index].load();
                const auto& clip = d.clip(track, scene);
                auto text = clip.hasAudio()
                    ? "Track " + juce::String(track + 1) + " | AUDIO\n" + clip.audioFileName.substring(0, 20)
                    : "Track " + juce::String(track + 1) + "\nScene " + juce::String(scene + 1);
                if (scene == active) text += " | Playing";
                else if (scene == requested) text += " | Queued";
                if (track == d.selectedTrack && scene == d.selectedScene) text += " | Edit";
                if (button->getButtonText() != text) button->setButtonText(text);

                SessionClipButton::Visual visual;
                const auto noteCount = static_cast<int>(clip.notes().count);
                visual.kind = clip.hasAudio() ? SessionClipButton::Kind::audio
                    : noteCount > 0 ? SessionClipButton::Kind::midi : SessionClipButton::Kind::empty;
                visual.colour = ui::trackColour(track);
                visual.label = clip.hasAudio() ? clip.audioFileName
                    : juce::String(noteCount) + (noteCount == 1 ? " note" : " notes");
                visual.playing = scene == active;
                visual.queued = scene != active && scene == requested;
                visual.selected = track == d.selectedTrack && scene == d.selectedScene;
                if (visual.playing && !clip.hasAudio() && clip.notes().loopSteps > 0) {
                    const int position = engine.sessionTrackPositions[index].load();
                    if (position >= 0)
                        visual.progress = static_cast<float>(std::fmod(position / 1000.0,
                            static_cast<double>(clip.notes().loopSteps)) / static_cast<double>(clip.notes().loopSteps));
                }
                button->setVisual(visual);
            }
        }
    }

    // Called by the owner when a MIDI controller fired a Session action.
    void handleControllerAction(int action) {
        if (action < MidiControllerMappings::sceneCount && action < controller.document().sceneCount())
            controller.launchScene(action);
        else if (action == MidiControllerMappings::stopAllAction) controller.stopAllTracks();
        refreshClips();
    }

private:
    static constexpr int sceneColumn = 132, gap = 6, headerHeight = 30;

    static juce::String dot() { return juce::String(juce::CharPointer_UTF8("  \xc2\xb7  ")); }

    static juce::String shortPresetName(int preset) {
        const int index = juce::jlimit(0, static_cast<int>(presetNames.size()) - 1, preset);
        return juce::String(presetNames[static_cast<std::size_t>(index)]).upToFirstOccurrenceOf(" (", false, false);
    }

    SessionClipButton* clipButton(int track, int scene) const {
        const int tracks = controller.document().trackCount();
        const auto index = static_cast<std::size_t>(scene * tracks + track);
        return index < clipButtons.size() ? clipButtons[index].get() : nullptr;
    }

    void selectionChanged() {
        refreshSelectedClip();
        if (onSelectionChanged) onSelectionChanged();
    }

    void setLearning(int action) {
        learningAction = action;
        learnController.setButtonText("Learn CC");
        learnController.setToggleState(action >= 0, juce::dontSendNotification);
        if (action >= 0) learnController.setButtonText("Cancel learn");
    }

    void updateControllerMappingLabel() {
        const int action = controllerTarget.getSelectedId() - 1;
        clearController.setEnabled(learningAction >= 0 || engine.controllerMappings.mappingFor(action) >= 0);
        if (learningAction == action) {
            controllerMappingLabel.setText(juce::String(juce::CharPointer_UTF8("Move a MIDI CC control\xe2\x80\xa6")),
                                           juce::dontSendNotification);
            return;
        }
        const int code = engine.controllerMappings.mappingFor(action);
        if (code < 0) controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        else controllerMappingLabel.setText("CC " + juce::String(code % 128) + "  |  Ch " + juce::String(code / 128 + 1),
                                            juce::dontSendNotification);
    }

    ProjectController& controller;
    ToneEngine& engine;
    juce::Label sessionHelp, launchTimingLabel, controllerMappingLabel, audioImportStatus;
    juce::ComboBox launchTiming, controllerTarget;
    ui::GlyphButton learnController;
    juce::TextButton clearController;
    std::vector<std::unique_ptr<ui::GlyphButton>> sceneButtons;
    std::vector<std::unique_ptr<SessionClipButton>> clipButtons; // [scene * tracks + track]
    ui::GlyphButton editSelectedClip, stopClip, importAudio, removeAudio, clearClip;
    AudioClipWaveform audioWaveform;
    juce::Rectangle<int> trackHeaderBounds, gridBounds, mappingBounds, clipDetailBounds;
    int learningAction{-1};
};

} // namespace alien
