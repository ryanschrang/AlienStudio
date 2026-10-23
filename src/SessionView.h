#pragma once
#include "FlLook.h"
#include "ProjectController.h"
#include "SessionClipButton.h"
#include "SessionTrackStrip.h"
#include "AudioClipWaveform.h"
#include "StudioWidgets.h"
#include "SynthEngine.h"
#include <functional>
#include <memory>
#include <vector>

namespace alien {

// GlyphButton drawn as a raised, bevelled button, as FL Studio draws its buttons.
class RaisedGlyphButton : public ui::GlyphButton {
public:
    using ui::GlyphButton::GlyphButton;

protected:
    void paintFace(juce::Graphics& g, juce::Colour colour, bool over, bool down) override {
        if (down) colour = colour.darker(0.15f);
        else if (over) colour = colour.brighter(0.08f);
        ui::fl::paintRaised(g, getLocalBounds().toFloat().reduced(0.5f), colour, 4.0f, down ? 0.10f : 0.22f, down ? 0.36f : 0.30f);
    }
};

// A raised button that also reports right-clicks (used for scene buttons).
class ContextGlyphButton final : public RaisedGlyphButton {
public:
    using RaisedGlyphButton::RaisedGlyphButton;
    std::function<void()> onRightClick;
    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        ui::GlyphButton::mouseUp(event);
    }
};

// Session (clip launcher) page, laid out like FL Studio's Performance Mode: one row per track, with the
// track's controls (light, pan, volume, name, solo, stop) at its left end, and one column per scene with the
// scene's launch button on top. The track column stays pinned at the left and the scene row stays pinned at
// the top while the clip grid scrolls in both directions, so any number of tracks and scenes fits the window.
class SessionView final : public juce::Component {
public:
    // Layout, in pixels. The track column is as wide as SessionTrackStrip; clip columns grow with the window up to maxSceneWidth.
    static constexpr int headerWidth = SessionTrackStrip::width, gap = 6, sceneBarHeight = 30, rowHeight = 36,
                         minSceneWidth = 104, maxSceneWidth = 220;

    SessionView(ProjectController& projectController, ToneEngine& toneEngine)
        : controller(projectController), engine(toneEngine) {
        namespace col = ui::colours;
        addAndMakeVisible(sessionHelp);
        sessionHelp.setText("Click a clip to launch it, right-click to edit. Right-click a track or scene name for more options.",
                            juce::dontSendNotification);
        sessionHelp.setFont(ui::font(12.5f));
        sessionHelp.setColour(juce::Label::textColourId, col::textFaint);
        for (auto* b : {&addTrackButton, &addSceneButton}) {
            addAndMakeVisible(b);
            b->setGlyph(ui::Glyph::plus);
            b->getProperties().set("style", "ghost");
        }
        addTrackButton.setButtonText("Track");
        addTrackButton.setName("Add track");
        addTrackButton.setTooltip("Add a Session track (up to " + juce::String(maxSessionTracks) + ")");
        addTrackButton.onClick = [this] { if (controller.addTrack()) structureChanged(true, "Add track"); };
        addSceneButton.setButtonText("Scene");
        addSceneButton.setName("Add scene");
        addSceneButton.setTooltip("Add a scene (up to " + juce::String(maxSessionScenes) + ")");
        addSceneButton.onClick = [this] { if (controller.addScene()) structureChanged(true, "Add scene"); };

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
            if (onEdited) onEdited("Change launch timing");
        };

        // Grid: pinned track column, scrolling clips, pinned scene row.
        addAndMakeVisible(trackStrip);
        trackStrip.setRows(rowHeight, gap);
        trackStrip.onSelect = [this](int track) {
            controller.selectClip(track, controller.document().selectedScene);
            selectionChanged();
        };
        trackStrip.onRename = [this](int track) { renameTrackAt(track); };
        trackStrip.onMenu = [this](int track) { showTrackMenu(track); };
        trackStrip.onStop = [this](int) { refreshClips(); };
        trackStrip.onMixEdited = [this](int track, bool committed) { if (onTrackMixEdited) onTrackMixEdited(track, committed); };
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&gridContent, false);
        viewport.setScrollBarThickness(10);
        viewport.onScrolled = [this] { syncScroll(); };
        addAndMakeVisible(sceneStrip);
        sceneStrip.setInterceptsMouseClicks(false, true);

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
                engine.controllerMappings.beginLearn(action);
                setLearning(action);
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
        addAndMakeVisible(relinkAudio);
        relinkAudio.setButtonText("Relink audio");
        relinkAudio.setGlyph(ui::Glyph::open);
        relinkAudio.setName("Relink missing audio files");
        relinkAudio.setTooltip("Search a folder for audio files this project can no longer find");
        relinkAudio.onClick = [this] { if (onRelinkAudio) onRelinkAudio(); };
        relinkAudio.setVisible(false);
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
                if (onEdited) onEdited("Remove audio");
            }
        };
        addAndMakeVisible(clearClip);
        clearClip.setButtonText("Clear clip");
        clearClip.setGlyph(ui::Glyph::close);
        clearClip.setName("Clear selected Session clip");
        clearClip.setTooltip("Remove all notes and audio from the selected clip");
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

    // Notifications for the owner.
    std::function<void(int, int)> onEditClip;           // open a clip in its editor
    std::function<void()> onSelectionChanged;           // selected clip changed
    std::function<void()> onClearClip;                  // clear the selected clip
    std::function<void()> onRelinkAudio;                // choose a folder to search for missing audio
    std::function<void()> onTitleChanged;               // header subtitle text changed
    std::function<void()> onStructureChanged;           // tracks/scenes added, removed, renamed or moved
    std::function<void(const juce::String&)> onEdited;  // a committed document edit, with an undo label
    // Optional presenter for deterministic UI tests and alternative menu hosts.
    // The native asynchronous popup remains the default.
    std::function<void(juce::PopupMenu, std::function<void(int)>)> trackMenuLauncher;
    std::function<void(int, bool)> onTrackMixEdited;    // a track's volume, pan, mute or solo changed here (committed: final value)

    // Rebuild the clip and scene buttons after the grid's shape changes.
    void rebuildGrid() {
        const auto& d = controller.document();
        clipButtons.clear();
        sceneButtons.clear();
        for (int scene = 0; scene < d.sceneCount(); ++scene) {
            auto sceneButton = std::make_unique<ContextGlyphButton>(ui::Glyph::play);
            sceneButton->setName("Launch scene " + juce::String(scene + 1));
            sceneButton->setButtonText(d.scenes[static_cast<std::size_t>(scene)].name);
            sceneButton->setTooltip("Launch every track's clip in " + d.scenes[static_cast<std::size_t>(scene)].name
                                    + ". Right-click to rename, duplicate, move or delete.");
            sceneButton->onClick = [this, scene] { controller.launchScene(scene); refreshClips(); };
            sceneButton->onRightClick = [this, scene] { showSceneMenu(scene); };
            sceneStrip.addAndMakeVisible(*sceneButton);
            sceneButtons.push_back(std::move(sceneButton));
            for (int track = 0; track < d.trackCount(); ++track) {
                auto button = std::make_unique<SessionClipButton>();
                const auto& trackName = d.tracks[static_cast<std::size_t>(track)].name;
                button->setName("Launch track " + juce::String(track + 1) + " scene " + juce::String(scene + 1));
                button->setTooltip(trackName + ", " + d.scenes[static_cast<std::size_t>(scene)].name
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
                gridContent.addAndMakeVisible(*button);
                clipButtons.push_back(std::move(button));
            }
        }
        addTrackButton.setEnabled(d.canAddTrack());
        addSceneButton.setEnabled(d.canAddScene());
        resized();
        refreshClips();
    }

    void refreshFromDocument() {
        launchTiming.setSelectedId(controller.document().launchQuantizationSteps == 4 ? 1 : 2, juce::dontSendNotification);
        refreshSelectedClip();
        repaint();
    }

    // After the selected clip's content or the selection changes.
    void refreshSelectedClip() {
        const auto& d = controller.document();
        const auto& clip = d.selectedClip();
        clearClip.setEnabled(!clip.isEmpty());
        removeAudio.setEnabled(clip.isAudioClip());
        audioWaveform.setClip(clip.audio.get(), clip.audioFileName, clip.audioFilePath,
            controller.currentFile(), clip.audioOffline());
        // The button only takes up room while something in the project is offline.
        const bool anyOffline = !controller.missingMedia().empty();
        if (relinkAudio.isVisible() != anyOffline) { relinkAudio.setVisible(anyOffline); resized(); }
        if (clip.audioOffline())
            setImportStatus("Audio offline: " + clip.audioFileName + ". Use Relink audio to search for it.", clip.audioFilePath);
        else if (!clip.hasAudio() && audioImportStatus.getText().isEmpty())
            audioImportStatus.setText("Selected clip: " + d.tracks[static_cast<std::size_t>(d.selectedTrack)].name
                + ", " + d.scenes[static_cast<std::size_t>(d.selectedScene)].name, juce::dontSendNotification);
        repaint(clipDetailBounds);
        trackStrip.repaint();
        refreshClips();
    }

    void setImportStatus(const juce::String& text, const juce::String& tooltip) {
        audioImportStatus.setText(text, juce::dontSendNotification);
        audioImportStatus.setTooltip(tooltip);
    }

    void setBusy(bool busy) { importAudio.setEnabled(!busy); }

    juce::String subtitle() const {
        const auto& d = controller.document();
        return juce::String(d.trackCount()) + (d.trackCount() == 1 ? " track" : " tracks") + dot()
            + juce::String(d.sceneCount()) + (d.sceneCount() == 1 ? " scene" : " scenes")
            + dot() + "launch on next " + (d.launchQuantizationSteps == 4 ? "beat" : "bar");
    }

    // 20 Hz from the owner: live clip states and MIDI-learn feedback.
    void tick() {
        refreshClips();
        if (isShowing()) trackStrip.repaint();
        const int learned = engine.controllerMappings.takeLearnedAction();
        if (learned >= 0) {
            if (learningAction == learned) setLearning(-1);
            updateControllerMappingLabel();
        }
    }

    // Called by the owner when a MIDI controller fired a Session action.
    void handleControllerAction(int action) {
        if (action < MidiControllerMappings::sceneCount && action < controller.document().sceneCount())
            controller.launchScene(action);
        else if (action == MidiControllerMappings::stopAllAction) controller.stopAllTracks();
        refreshClips();
    }

    // Where things are, for tests and for the owner.
    SessionTrackStrip& tracks() { return trackStrip; }
    SessionClipButton* clipSlot(int track, int scene) const { return clipButton(track, scene); }
    ContextGlyphButton* sceneLauncher(int scene) const {
        return scene >= 0 && scene < static_cast<int>(sceneButtons.size()) ? sceneButtons[static_cast<std::size_t>(scene)].get() : nullptr;
    }
    juce::Rectangle<int> clipArea(int track, int scene) const {
        auto* b = clipButton(track, scene);
        return b == nullptr ? juce::Rectangle<int>() : getLocalArea(b->getParentComponent(), b->getBounds());
    }
    juce::Rectangle<int> sceneArea(int scene) const {
        auto* b = sceneLauncher(scene);
        return b == nullptr ? juce::Rectangle<int>() : getLocalArea(b->getParentComponent(), b->getBounds());
    }
    juce::Rectangle<int> stopAllArea() const { return stopClip.getBounds(); }
    juce::Viewport& clipViewport() { return viewport; }
    int sceneWidth() const { return currentSceneWidth; }

    // The mouse wheel over the track column or the scene row scrolls the clips too. When the clips cannot scroll the viewport
    // passes the event back up to this component, so it is handed to the viewport only once: the second time it goes on to
    // the parent. (Without the guard the two hand it to each other without end.)
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        if (forwardingWheel) { juce::Component::mouseWheelMove(e, wheel); return; }
        const juce::ScopedValueSetter<bool> forwarding(forwardingWheel, true);
        viewport.mouseWheelMove(e.getEventRelativeTo(&viewport), wheel);
    }

    void resized() override {
        const auto& d = controller.document();
        auto r = getLocalBounds().reduced(16, 12);
        auto header = r.removeFromTop(30);
        launchTiming.setBounds(header.removeFromRight(96).reduced(0, 2));
        launchTimingLabel.setBounds(header.removeFromRight(70));
        header.removeFromRight(12);
        addSceneButton.setBounds(header.removeFromRight(84).reduced(0, 1));
        header.removeFromRight(4);
        addTrackButton.setBounds(header.removeFromRight(84).reduced(0, 1));
        header.removeFromRight(12);
        sessionHelp.setBounds(header);
        r.removeFromTop(8);

        // Reserve the action bar and a minimum clip detail area below the grid.
        const int belowGrid = 14 + 30 + 14 + 96;
        const int scenes = juce::jmax(1, d.sceneCount()), tracks = juce::jmax(1, d.trackCount());
        const int bar = viewport.getScrollBarThickness();
        const int contentHeight = tracks * (rowHeight + gap);
        const int wanted = sceneBarHeight + gap + contentHeight;
        const int gridHeight = juce::jlimit(sceneBarHeight + gap + rowHeight + 12,
                                            juce::jmax(120, r.getHeight() - belowGrid), wanted + bar);
        // Decide scroll bars before sizing scenes so a vertical bar never forces a horizontal one.
        const int visibleClipsHeight = gridHeight - sceneBarHeight - gap;
        int clipsWidth = r.getWidth() - headerWidth - gap;
        const bool needsVBar = contentHeight > visibleClipsHeight;
        if (needsVBar) clipsWidth -= bar;
        const int sceneWidth = juce::jlimit(minSceneWidth, maxSceneWidth, (clipsWidth - (scenes - 1) * gap) / scenes);
        const int contentWidth = scenes * sceneWidth + (scenes - 1) * gap;
        const bool needsHBar = contentWidth > clipsWidth;
        auto grid = r.removeFromTop(gridHeight);
        gridBounds = grid;

        auto trackColumn = grid.removeFromLeft(headerWidth);
        grid.removeFromLeft(gap);
        stopClip.setBounds(trackColumn.removeFromTop(sceneBarHeight));
        trackColumn.removeFromTop(gap);

        auto sceneRow = grid.removeFromTop(sceneBarHeight);
        grid.removeFromTop(gap);
        viewport.setBounds(grid);
        // The clips viewport, the scene row and the track column share the same visible area.
        const int visibleWidth = viewport.getWidth() - (needsVBar ? bar : 0);
        const int visibleHeight = viewport.getHeight() - (needsHBar ? bar : 0);
        sceneStrip.setBounds(sceneRow.getX(), sceneRow.getY(), juce::jmax(0, visibleWidth), sceneBarHeight);
        trackStrip.setBounds(trackColumn.getX(), viewport.getY(), headerWidth, juce::jmax(0, visibleHeight));

        gridContent.setSize(contentWidth, contentHeight);
        for (int scene = 0; scene < d.sceneCount(); ++scene)
            for (int track = 0; track < d.trackCount(); ++track)
                if (auto* b = clipButton(track, scene))
                    b->setBounds(scene * (sceneWidth + gap), track * (rowHeight + gap), sceneWidth, rowHeight);
        currentSceneWidth = sceneWidth;
        syncScroll();

        r.removeFromTop(14);
        auto actions = r.removeFromTop(30);
        editSelectedClip.setBounds(actions.removeFromLeft(104));
        actions.removeFromLeft(6);
        importAudio.setBounds(actions.removeFromLeft(124));
        actions.removeFromLeft(6);
        removeAudio.setBounds(actions.removeFromLeft(124));
        actions.removeFromLeft(6);
        if (relinkAudio.isVisible()) { relinkAudio.setBounds(actions.removeFromLeft(124)); actions.removeFromLeft(6); }
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
        audioImportStatus.setBounds(detailHeader.removeFromRight(juce::jmax(100, detailHeader.getWidth() - 260)));
        detail.removeFromTop(6);
        audioWaveform.setBounds(detail);
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        const auto& d = controller.document();
        g.fillAll(col::surface);
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(viewport.getBounds().toFloat(), 5.0f);

        ui::drawCaption(g, "MIDI map", mappingBounds.withWidth(62), col::textFaint);
        g.setColour(col::line);
        g.fillRect(mappingBounds.getX() - 10, mappingBounds.getY() + 5, 1, mappingBounds.getHeight() - 10);

        ui::drawCard(g, clipDetailBounds.toFloat(), col::raised);
        const auto& track = d.tracks[static_cast<std::size_t>(d.selectedTrack)];
        ui::drawCaption(g, "Clip" + dot() + track.name + dot() + d.scenes[static_cast<std::size_t>(d.selectedScene)].name,
                        clipDetailBounds.reduced(12, 8).removeFromTop(20), ui::trackColour(track.colour));
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
        for (int scene = 0; scene < static_cast<int>(sceneButtons.size()) && scene < d.sceneCount(); ++scene) {
            auto& sceneButton = *sceneButtons[static_cast<std::size_t>(scene)];
            const bool playingScene = scenePlaying[static_cast<std::size_t>(scene)];
            const auto sceneColour = playingScene ? juce::Colour(0xff1d3a31)
                : scene == d.selectedScene ? col::hover : col::control;
            if (sceneButton.findColour(juce::TextButton::buttonColourId) != sceneColour) {
                sceneButton.setColour(juce::TextButton::buttonColourId, sceneColour);
                sceneButton.setGlyphColour(playingScene ? col::accent : col::textDim);
            }
            sceneButton.setTitle((scene == d.selectedScene ? "Selected, " : "") + juce::String("Launch ")
                                 + d.scenes[static_cast<std::size_t>(scene)].name);
            for (int track = 0; track < d.trackCount(); ++track) {
                auto* button = clipButton(track, scene);
                if (button == nullptr) continue;
                const auto index = static_cast<std::size_t>(track);
                const int active = engine.activePianoRollClip[index].load();
                const int requested = engine.requestedPianoRollClip[index].load();
                const auto& clip = d.clip(track, scene);
                auto text = clip.hasAudio()
                    ? "Track " + juce::String(track + 1) + " | AUDIO\n" + clip.audioFileName.substring(0, 20)
                    : clip.audioOffline()
                    ? "Track " + juce::String(track + 1) + " | AUDIO OFFLINE\n" + clip.audioFileName.substring(0, 20)
                    : "Track " + juce::String(track + 1) + "\nScene " + juce::String(scene + 1);
                if (scene == active) text += " | Playing";
                else if (scene == requested) text += " | Queued";
                if (track == d.selectedTrack && scene == d.selectedScene) text += " | Edit";
                if (button->getButtonText() != text) button->setButtonText(text);

                SessionClipButton::Visual visual;
                const auto noteCount = static_cast<int>(clip.notes().count);
                visual.kind = clip.hasAudio() ? SessionClipButton::Kind::audio
                    : clip.audioOffline() ? SessionClipButton::Kind::offline
                    : noteCount > 0 ? SessionClipButton::Kind::midi : SessionClipButton::Kind::empty;
                visual.colour = ui::trackColour(d.tracks[index].colour);
                visual.label = clip.hasAudio() ? clip.audioFileName
                    : clip.audioOffline() ? clip.audioFileName
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

private:
    struct GridViewport final : juce::Viewport {
        std::function<void()> onScrolled;
        void visibleAreaChanged(const juce::Rectangle<int>&) override { if (onScrolled) onScrolled(); }
    };

    static juce::String dot() { return juce::String(juce::CharPointer_UTF8("  \xc2\xb7  ")); }

    void syncScroll() {
        trackStrip.setOffsetY(viewport.getViewPositionY());
        const int offsetX = viewport.getViewPositionX();
        for (std::size_t scene = 0; scene < sceneButtons.size(); ++scene)
            sceneButtons[scene]->setBounds(static_cast<int>(scene) * (currentSceneWidth + gap) - offsetX, 0, currentSceneWidth, sceneBarHeight);
    }

    void showTrackMenu(int track) {
        const auto& d = controller.document();
        juce::PopupMenu menu;
        menu.addSectionHeader(d.tracks[static_cast<std::size_t>(track)].name);
        menu.addItem(1, "Rename...");
        menu.addItem(2, "Move left", track > 0);
        menu.addItem(3, "Move right", track < d.trackCount() - 1);
        menu.addSeparator();
        menu.addItem(4, "Add track", d.canAddTrack());
        menu.addItem(5, "Delete track", d.trackCount() > 1);
        juce::Component::SafePointer<SessionView> safe(this);
        std::function<void(int)> done = [safe, track](int result) {
            if (safe == nullptr) return;
            auto& c = safe->controller;
            if (result == 1) safe->renameTrackAt(track);
            else if (result == 2 && c.moveTrack(track, -1)) safe->structureChanged(true, "Move track");
            else if (result == 3 && c.moveTrack(track, 1)) safe->structureChanged(true, "Move track");
            else if (result == 4 && c.addTrack()) safe->structureChanged(true, "Add track");
            else if (result == 5 && c.deleteTrack(track)) safe->structureChanged(true, "Delete track");
        };
        if (trackMenuLauncher) { trackMenuLauncher(std::move(menu), std::move(done)); return; }
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), std::move(done));
    }

    void showSceneMenu(int scene) {
        const auto& d = controller.document();
        juce::PopupMenu menu;
        menu.addSectionHeader(d.scenes[static_cast<std::size_t>(scene)].name);
        menu.addItem(1, "Rename...");
        menu.addItem(2, "Duplicate", d.canAddScene());
        menu.addItem(3, "Move up", scene > 0);
        menu.addItem(4, "Move down", scene < d.sceneCount() - 1);
        menu.addSeparator();
        menu.addItem(5, "Delete scene", d.sceneCount() > 1);
        juce::Component::SafePointer<SessionView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, scene](int result) {
            if (safe == nullptr) return;
            auto& c = safe->controller;
            if (result == 1) safe->renameSceneAt(scene);
            else if (result == 2 && c.duplicateScene(scene)) safe->structureChanged(true, "Duplicate scene");
            else if (result == 3 && c.moveScene(scene, -1)) safe->structureChanged(true, "Move scene");
            else if (result == 4 && c.moveScene(scene, 1)) safe->structureChanged(true, "Move scene");
            else if (result == 5 && c.deleteScene(scene)) safe->structureChanged(true, "Delete scene");
        });
    }

    void renameTrackAt(int track) {
        if (track < 0 || track >= controller.document().trackCount()) return;
        askForName("Rename track", controller.document().tracks[static_cast<std::size_t>(track)].name,
            [this, track](const juce::String& name) {
                if (controller.renameTrack(track, name)) structureChanged(true, "Rename track");
            });
    }

    void renameSceneAt(int scene) {
        if (scene < 0 || scene >= controller.document().sceneCount()) return;
        askForName("Rename scene", controller.document().scenes[static_cast<std::size_t>(scene)].name,
            [this, scene](const juce::String& name) {
                if (controller.renameScene(scene, name)) structureChanged(true, "Rename scene");
            });
    }

    void askForName(const juce::String& title, const juce::String& current, std::function<void(const juce::String&)> apply) {
        auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", current);
        window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<SessionView> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create(
            [safe, window, apply](int result) {
                if (safe != nullptr && result == 1) apply(window->getTextEditorContents("name"));
            }), true);
    }

    void structureChanged(bool shapeChanged, const juce::String& label) {
        if (shapeChanged) rebuildGrid();
        refreshSelectedClip();
        repaint();
        if (onStructureChanged) onStructureChanged();
        if (onTitleChanged) onTitleChanged();
        if (onEdited) onEdited(label);
    }

    SessionClipButton* clipButton(int track, int scene) const {
        const int tracks = controller.document().trackCount();
        const auto index = static_cast<std::size_t>(scene * tracks + track);
        return track >= 0 && track < tracks && index < clipButtons.size() ? clipButtons[index].get() : nullptr;
    }

    void selectionChanged() {
        refreshSelectedClip();
        if (onSelectionChanged) onSelectionChanged();
    }

    void setLearning(int action) {
        learningAction = action;
        learnController.setButtonText(action >= 0 ? "Cancel learn" : "Learn CC");
        learnController.setToggleState(action >= 0, juce::dontSendNotification);
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
    ui::GlyphButton addTrackButton, addSceneButton, learnController;
    juce::TextButton clearController;
    SessionTrackStrip trackStrip{controller, engine};
    GridViewport viewport;
    juce::Component gridContent, sceneStrip;
    std::vector<std::unique_ptr<ContextGlyphButton>> sceneButtons;
    std::vector<std::unique_ptr<SessionClipButton>> clipButtons; // [scene * tracks + track]
    ui::GlyphButton editSelectedClip, importAudio, removeAudio, clearClip, relinkAudio;
    RaisedGlyphButton stopClip;
    AudioClipWaveform audioWaveform;
    juce::Rectangle<int> gridBounds, mappingBounds, clipDetailBounds;
    int learningAction{-1};
    int currentSceneWidth{minSceneWidth};
    bool forwardingWheel{};
};

} // namespace alien
