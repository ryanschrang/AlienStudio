#include <JuceHeader.h>
#include "ToneEngine.h"
#include "TypingPianoRouter.h"
#include "PianoRollEditor.h"
#include "MidiInputPanel.h"
#include "CompactWorkspace.h"
#include "SessionClipButton.h"
#include "AudioClipImport.h"
#include "AudioClipWaveform.h"
#include "SessionProject.h"
#include "TrackMixerPanel.h"
#include "PortableProject.h"
#include "StudioWidgets.h"
#include "Branding.h"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ui = alien::ui;
namespace col = alien::ui::colours;

struct SessionClipContent {
    alien::PianoRollPattern pattern;
    const alien::SessionAudioClip* audio{};
    juce::String audioFileName;
    juce::String audioFilePath;
};

struct SessionProjectLoadResult {
    alien::SessionProjectData project;
    std::array<std::shared_ptr<alien::SessionAudioClip>, 16> audioAssets{};
    std::size_t decodedBytes{};
    juce::String error;
};

// One pad in the step sequencer row. Groups of four alternate shade so beats
// read at a glance; the pad shows its note and velocity and lights an LED
// while the playhead is on it.
class SequencerStepButton final : public juce::ToggleButton {
public:
    SequencerStepButton(const juce::String& label, int index) : juce::ToggleButton(label), stepIndex(index) {}

    void setPlaybackPosition(bool isCurrent) {
        if (playbackPosition == isCurrent) return;
        playbackPosition = isCurrent;
        repaint();
    }

    void setStepInfo(const juce::String& note, float velocity01, bool isSelected) {
        if (note == noteName && std::abs(velocity01 - velocity) < 0.001f && isSelected == selected) return;
        noteName = note; velocity = velocity01; selected = isSelected;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.5f);
        const bool on = getToggleState();
        const bool altGroup = (stepIndex / 4) % 2 == 1;
        auto base = on ? col::accent : juce::Colour(altGroup ? 0xff24262c : 0xff2c2f36);
        if (playbackPosition) base = base.brighter(on ? 0.18f : 0.22f);
        if (isButtonDown) base = base.brighter(0.12f);
        else if (isMouseOver) base = base.brighter(0.06f);
        g.setGradientFill(juce::ColourGradient(base.brighter(0.06f), 0.0f, bounds.getY(),
                                               base.darker(on ? 0.18f : 0.1f), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);

        // Playhead LED.
        const auto led = juce::Rectangle<float>(bounds.getWidth() * 0.36f, 3.0f)
                             .withCentre({bounds.getCentreX(), bounds.getY() + 6.0f});
        g.setColour(playbackPosition ? col::warning : (on ? col::accentInk.withAlpha(0.28f) : col::inset));
        g.fillRoundedRectangle(led, 1.5f);

        const auto ink = on ? col::accentInk : col::textDim;
        auto inner = bounds.reduced(8.0f, 6.0f).withTrimmedTop(8.0f);
        g.setColour(ink);
        g.setFont(ui::font(13.0f, true));
        g.drawText(getButtonText(), inner.removeFromTop(18.0f), juce::Justification::centredLeft, false);
        if (inner.getHeight() > 26.0f) {
            g.setColour(on ? ink.withAlpha(0.8f) : col::textFaint);
            g.setFont(ui::monoFont(11.0f));
            g.drawText(noteName, inner.removeFromTop(16.0f), juce::Justification::centredLeft, false);
        }
        if (on) {
            // Velocity bar.
            const auto bar = juce::Rectangle<float>(bounds.getX() + 8.0f, bounds.getBottom() - 9.0f,
                                                    bounds.getWidth() - 16.0f, 3.0f);
            g.setColour(col::accentInk.withAlpha(0.2f));
            g.fillRoundedRectangle(bar, 1.5f);
            g.setColour(col::accentInk.withAlpha(0.7f));
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, velocity)), 1.5f);
        }
        if (selected) {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.6f);
        }
        if (hasKeyboardFocus(true)) {
            g.setColour(col::accent);
            g.drawRoundedRectangle(bounds.reduced(3.0f), 3.5f, 1.0f);
        }
    }

private:
    int stepIndex{};
    bool playbackPosition{}, selected{};
    juce::String noteName;
    float velocity{0.8f};
};

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener, private juce::ChangeListener {
public:
    MainComponent() {
        setLookAndFeel(&studioLookAndFeel);
        juce::LookAndFeel::setDefaultLookAndFeel(&studioLookAndFeel);
        setOpaque(true);
        setSize(1280, 800);

        // Top bar -----------------------------------------------------------
        addAndMakeVisible(brandMark);
        addAndMakeVisible(title);
        title.setText("Alien Studio", juce::dontSendNotification);
        title.setFont(ui::font(15.0f, true));
        title.setColour(juce::Label::textColourId, col::text);
        title.setJustificationType(juce::Justification::bottomLeft);
        title.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(brandSubtitle);
        brandSubtitle.setText("PATTERN STUDIO", juce::dontSendNotification);
        brandSubtitle.setFont(ui::font(9.5f, true));
        brandSubtitle.setColour(juce::Label::textColourId, col::accent);
        brandSubtitle.setJustificationType(juce::Justification::topLeft);
        brandSubtitle.setInterceptsMouseClicks(false, false);

        const std::array<const char*, 4> viewNames{"Steps", "Piano Roll", "Session", "Mixer"};
        for (int i = 0; i < 4; ++i) {
            auto& b = viewButtons[static_cast<std::size_t>(i)];
            addAndMakeVisible(b);
            b.setButtonText(viewNames[static_cast<std::size_t>(i)]);
            b.setName("Show " + juce::String(viewNames[static_cast<std::size_t>(i)]) + " view");
            b.getProperties().set("style", "tab");
            b.setRadioGroupId(1001);
            b.setTooltip("Show the " + juce::String(viewNames[static_cast<std::size_t>(i)]) + " view");
            b.onClick = [this, i] { showView(i); };
        }

        addAndMakeVisible(play);
        play.setButtonText("Play");
        play.setIconOnly(true);
        play.setGlyph(ui::Glyph::play);
        play.setName("Start or stop playback");
        play.setTooltip("Start or stop the current pattern");
        play.setColour(juce::TextButton::buttonColourId, col::control);
        play.setColour(juce::TextButton::buttonOnColourId, col::accent);
        play.setGlyphColour(col::accent);
        play.onClick = [this] { releaseNotes(); engine.playing.store(!engine.playing.load()); };

        addAndMakeVisible(transportDisplay);
        tempo.setSliderStyle(juce::Slider::LinearBarVertical);
        tempo.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120);
        tempo.setName("Tempo");
        tempo.setTooltip("Tempo in beats per minute. Drag up/down or use the mouse wheel; double-click resets to 120.");
        tempo.setDoubleClickReturnValue(true, 120.0);
        tempo.setMouseDragSensitivity(220);
        tempo.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        tempo.onValueChange = [this] { engine.bpm.store(static_cast<int>(tempo.getValue())); };
        tempoLabel.setText("BPM", juce::dontSendNotification); // Drawn by the transport display.

        for (auto* b : {&openProjectButton, &saveProjectButton, &portableProjectButton}) {
            addAndMakeVisible(b);
            b->getProperties().set("style", "ghost");
        }
        saveProjectButton.setButtonText("Save");
        saveProjectButton.setGlyph(ui::Glyph::save);
        saveProjectButton.setName("Save Alien Studio session project");
        saveProjectButton.setTooltip("Save the session project");
        saveProjectButton.onClick = [this] { saveProject(); };
        portableProjectButton.setButtonText("Save portable");
        portableProjectButton.setGlyph(ui::Glyph::collect);
        portableProjectButton.setName("Save project copy with collected audio");
        portableProjectButton.setTooltip("Save a project file and adjacent media folder; move both together");
        portableProjectButton.onClick = [this] { chooseProjectSavePath(true); };
        openProjectButton.setButtonText("Open");
        openProjectButton.setGlyph(ui::Glyph::open);
        openProjectButton.setName("Open Alien Studio session project");
        openProjectButton.setTooltip("Open a session project");
        openProjectButton.onClick = [this] { chooseProjectToOpen(); };

        // Status bar ------------------------------------------------------
        addAndMakeVisible(projectStatus);
        projectStatus.setFont(ui::font(12.0f));
        projectStatus.setColour(juce::Label::textColourId, col::textDim);
        projectStatus.setJustificationType(juce::Justification::centredLeft);
        projectStatus.setText("Unsaved session", juce::dontSendNotification);
        addAndMakeVisible(engineStatus);
        engineStatus.setFont(ui::monoFont(11.5f));
        engineStatus.setColour(juce::Label::textColourId, col::textFaint);
        engineStatus.setJustificationType(juce::Justification::centredRight);

        // Step sequencer --------------------------------------------------
        stepsPanel.onPaint = [this](juce::Graphics& g) { paintStepsPanel(g); };
        stepsPanel.onLayout = [this] { layoutStepsPanel(); };
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new SequencerStepButton(juce::String(i + 1), i));
            button->setName("Sequencer step " + juce::String(i + 1));
            button->setTooltip("Toggle sixteenth-note step " + juce::String(i + 1));
            button->setTitle("Step " + juce::String(i + 1));
            stepsPanel.addAndMakeVisible(button);
            button->onClick = [this, button, i] {
                engine.steps[static_cast<std::size_t>(i)].store(button->getToggleState());
                selectSequencerStep(i);
            };
        }
        engine.steps[0].store(true); pads[0]->setToggleState(true, juce::dontSendNotification);
        engine.steps[4].store(true); pads[4]->setToggleState(true, juce::dontSendNotification);
        engine.steps[8].store(true); pads[8]->setToggleState(true, juce::dontSendNotification);
        engine.steps[12].store(true); pads[12]->setToggleState(true, juce::dontSendNotification);
        for (auto* label : {&selectedStepLabel, &stepPitchLabel, &stepVelocityLabel}) {
            stepsPanel.addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true));
            label->setColour(juce::Label::textColourId, col::textFaint);
        }
        selectedStepLabel.setText("STEP", juce::dontSendNotification);
        stepsPanel.addAndMakeVisible(stepSelector);
        stepSelector.setName("Selected sequencer step");
        for (int i = 0; i < 16; ++i) stepSelector.addItem("Step " + juce::String(i + 1), i + 1);
        stepSelector.onChange = [this] { selectSequencerStep(stepSelector.getSelectedId() - 1); };
        stepPitchLabel.setText("PITCH", juce::dontSendNotification);
        stepsPanel.addAndMakeVisible(stepPitch);
        stepPitch.setName("Selected step MIDI pitch");
        stepPitch.setTooltip("Set the selected step pitch from MIDI note 0 to 127");
        stepPitch.setRange(0, 127, 1);
        stepPitch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 24);
        stepPitch.onValueChange = [this] {
            const int pitch = static_cast<int>(stepPitch.getValue());
            engine.stepPitches[static_cast<std::size_t>(selectedStep)].store(pitch);
            stepPitchName.setText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 4),
                juce::dontSendNotification);
            refreshPadInfo();
        };
        stepsPanel.addAndMakeVisible(stepPitchName);
        stepPitchName.setName("Selected step note name");
        stepPitchName.setFont(ui::monoFont(14.0f, true));
        stepPitchName.setColour(juce::Label::textColourId, col::accent);
        stepPitchName.setJustificationType(juce::Justification::centred);
        stepVelocityLabel.setText("VELOCITY", juce::dontSendNotification);
        stepsPanel.addAndMakeVisible(stepVelocity);
        stepVelocity.setName("Selected step velocity");
        stepVelocity.setTooltip("Set the selected step velocity from 1 to 127");
        stepVelocity.setRange(1, 127, 1);
        stepVelocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 24);
        stepVelocity.onValueChange = [this] {
            engine.stepVelocities[static_cast<std::size_t>(selectedStep)].store(
                static_cast<int>(stepVelocity.getValue()));
            refreshPadInfo();
        };
        selectSequencerStep(0);
        stepsPanel.addAndMakeVisible(stepHelp);
        stepHelp.setText("Click a pad to toggle it and select it for editing. The step row plays the live synth.",
                         juce::dontSendNotification);
        stepHelp.setFont(ui::font(12.5f));
        stepHelp.setColour(juce::Label::textColourId, col::textFaint);
        stepHelp.setJustificationType(juce::Justification::centredLeft);

        // Editor tabs: the stock tab bar is hidden; the top-bar view switch drives it.
        tabs.addTab("16-step sequencer", col::surface, &stepsPanel, false);
        tabs.addTab("Piano roll", col::surface, &pianoRoll, false);
        tabs.addTab("Session", col::surface, &sessionPanel, false);
        tabs.addTab("Mixer", col::surface, &mixerPanel, false);
        tabs.setTabBarDepth(0);
        tabs.setOutline(0);
        tabs.setIndent(0);
        tabs.getTabbedButtonBar().addChangeListener(this);
        addAndMakeVisible(workspace);

        // Devices -----------------------------------------------------------
        devicePanel.onPaint = [this](juce::Graphics& g) { paintDevicePanel(g); };
        devicePanel.onLayout = [this] { layoutDevicePanel(); };
        devicePanel.addAndMakeVisible(deviceHelp);
        deviceHelp.setText("Choose the live keyboard / step synth and MIDI input. Session instruments are in Mixer. Shift+Tab returns.",
                           juce::dontSendNotification);
        deviceHelp.setFont(ui::font(12.5f));
        deviceHelp.setColour(juce::Label::textColourId, col::textFaint);
        for (auto* label : {&midiLabel, &presetLabel}) {
            devicePanel.addAndMakeVisible(label);
            label->setFont(ui::font(10.5f, true));
            label->setColour(juce::Label::textColourId, col::textFaint);
        }
        midiLabel.setText("MIDI INPUT", juce::dontSendNotification);
        devicePanel.addAndMakeVisible(midiInput);

        // Session -------------------------------------------------------------
        sessionPanel.onPaint = [this](juce::Graphics& g) { paintSessionPanel(g); };
        sessionPanel.onLayout = [this] { layoutSessionPanel(); };
        sessionPanel.addAndMakeVisible(sessionHelp);
        sessionHelp.setText("Click a clip to launch it, right-click to edit. Launch a scene to switch every track together.",
                            juce::dontSendNotification);
        sessionHelp.setFont(ui::font(12.5f));
        sessionHelp.setColour(juce::Label::textColourId, col::textFaint);
        sessionPanel.addAndMakeVisible(launchTimingLabel);
        launchTimingLabel.setText("LAUNCH", juce::dontSendNotification);
        launchTimingLabel.setFont(ui::font(10.5f, true));
        launchTimingLabel.setColour(juce::Label::textColourId, col::textFaint);
        launchTimingLabel.setJustificationType(juce::Justification::centredRight);
        sessionPanel.addAndMakeVisible(launchTiming);
        launchTiming.setName("Session launch quantization");
        launchTiming.setTooltip("Choose whether queued clips and scenes launch on the next beat or bar");
        launchTiming.addItem("Beat", 1);
        launchTiming.addItem("Bar", 2);
        launchTiming.setSelectedId(2, juce::dontSendNotification);
        launchTiming.onChange = [this] {
            engine.launchQuantizationSteps.store(launchTiming.getSelectedId() == 1 ? 4 : 16);
            updateEditorTitle();
        };
        sessionPanel.addAndMakeVisible(controllerTarget);
        controllerTarget.setName("MIDI controller mapping action");
        controllerTarget.setTooltip("Choose a Session action to assign to a MIDI CC");
        controllerTarget.addItem("Scene 1", 1);
        controllerTarget.addItem("Scene 2", 2);
        controllerTarget.addItem("Scene 3", 3);
        controllerTarget.addItem("Scene 4", 4);
        controllerTarget.addItem("Stop all", 5);
        controllerTarget.setSelectedId(1, juce::dontSendNotification);
        controllerTarget.onChange = [this] { updateControllerMappingLabel(); };
        sessionPanel.addAndMakeVisible(learnController);
        learnController.setButtonText("Learn CC");
        learnController.setGlyph(ui::Glyph::learn);
        learnController.setName("Learn MIDI controller");
        learnController.setTooltip("Listen for the next incoming MIDI CC and assign it to the selected Session action");
        learnController.setColour(juce::TextButton::buttonOnColourId, col::warning);
        learnController.onClick = [this] {
            const int action = controllerTarget.getSelectedId() - 1;
            if (learningControllerAction == action) {
                engine.controllerMappings.cancelLearn();
                learningControllerAction = -1;
                learnController.setButtonText("Learn CC");
                learnController.setToggleState(false, juce::dontSendNotification);
                updateControllerMappingLabel();
            } else {
                learningControllerAction = action;
                engine.controllerMappings.beginLearn(action);
                learnController.setButtonText("Cancel learn");
                learnController.setToggleState(true, juce::dontSendNotification);
                controllerMappingLabel.setText(ui::utf8("Move a MIDI CC control\xe2\x80\xa6"), juce::dontSendNotification);
            }
        };
        sessionPanel.addAndMakeVisible(clearController);
        clearController.setButtonText("Unmap");
        clearController.setName("Clear MIDI controller mapping");
        clearController.setTooltip("Remove the MIDI CC assigned to the selected action (enabled once a CC is mapped)");
        clearController.onClick = [this] {
            engine.controllerMappings.cancelLearn();
            learningControllerAction = -1;
            engine.controllerMappings.clearMapping(controllerTarget.getSelectedId() - 1);
            learnController.setButtonText("Learn CC");
            learnController.setToggleState(false, juce::dontSendNotification);
            updateControllerMappingLabel();
        };
        sessionPanel.addAndMakeVisible(controllerMappingLabel);
        controllerMappingLabel.setName("MIDI controller assignment");
        controllerMappingLabel.setFont(ui::monoFont(12.0f));
        controllerMappingLabel.setColour(juce::Label::textColourId, col::textDim);
        controllerMappingLabel.setJustificationType(juce::Justification::centredLeft);
        controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        clearController.setEnabled(false);
        for (int scene = 0; scene < sceneCount; ++scene) {
            sessionPanel.addAndMakeVisible(sceneButtons[static_cast<std::size_t>(scene)]);
            auto& sceneButton = sceneButtons[static_cast<std::size_t>(scene)];
            sceneButton.setName("Launch scene " + juce::String(scene + 1));
            sceneButton.setButtonText("Scene " + juce::String(scene + 1));
            sceneButton.setGlyph(ui::Glyph::play);
            sceneButton.setTooltip("Launch every track's clip in scene " + juce::String(scene + 1));
            sceneButton.onClick = [this, scene] { launchScene(scene); };
            for (int track = 0; track < trackCount; ++track) {
                const auto index = cellIndex(track, scene);
                sessionPanel.addAndMakeVisible(clipButtons[index]);
                auto& button = clipButtons[index];
                button.setName("Launch track " + juce::String(track + 1) + " scene " + juce::String(scene + 1));
                button.setTooltip("Track " + juce::String(track + 1) + ", scene " + juce::String(scene + 1)
                                  + ": click to launch, right-click to edit");
                button.onClick = [this, track, scene] { selectAndLaunchClip(track, scene); };
                button.onRightClick = [this, track, scene] { selectClipForEditing(track, scene); };
            }
        }
        sessionPanel.addAndMakeVisible(stopClip);
        stopClip.setButtonText("Stop all");
        stopClip.setGlyph(ui::Glyph::stop);
        stopClip.setName("Stop all Session tracks at selected launch boundary");
        stopClip.setTooltip("Stop every Session track at the next launch boundary");
        stopClip.onClick = [this] { stopAllSessionTracks(); };
        sessionPanel.addAndMakeVisible(editSelectedClip);
        editSelectedClip.setButtonText("Edit clip");
        editSelectedClip.setGlyph(ui::Glyph::edit);
        editSelectedClip.setName("Edit selected Session clip");
        editSelectedClip.setTooltip("Open the selected MIDI clip in the piano roll or show its audio waveform");
        editSelectedClip.onClick = [this] {
            selectClipForEditing(selectedTrack, selectedScene);
        };
        sessionPanel.addAndMakeVisible(importAudio);
        importAudio.setButtonText("Import audio");
        importAudio.setGlyph(ui::Glyph::waveform);
        importAudio.setName("Import audio file to selected Session clip");
        importAudio.setTooltip("Decode a WAV or MP3 off the audio thread and add it to the selected clip");
        importAudio.onClick = [this] { chooseAudioFile(); };
        sessionPanel.addAndMakeVisible(clearClip);
        clearClip.setButtonText("Clear clip");
        clearClip.setGlyph(ui::Glyph::close);
        clearClip.setName("Clear selected Session clip");
        clearClip.setTooltip("Remove all notes and audio from the selected clip. Notes can be restored with Undo in the piano roll.");
        clearClip.onClick = [this] { clearSelectedClip(); };
        sessionPanel.addAndMakeVisible(removeAudio);
        removeAudio.setButtonText("Remove audio");
        removeAudio.setGlyph(ui::Glyph::close);
        removeAudio.setName("Remove audio from selected Session clip");
        removeAudio.setTooltip("Remove the audio file from the selected clip");
        removeAudio.onClick = [this] { removeAudioFromSelectedClip(); };
        sessionPanel.addAndMakeVisible(audioImportStatus);
        audioImportStatus.setFont(ui::font(12.0f));
        audioImportStatus.setColour(juce::Label::textColourId, col::textDim);
        audioImportStatus.setJustificationType(juce::Justification::centredRight);
        sessionPanel.addAndMakeVisible(audioWaveform);
        pianoRoll.onChange = [this] {
            auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
            clip.pattern = pianoRoll.getPattern();
            clearClip.setEnabled(clip.audio != nullptr || clip.pattern.count > 0);
            if (loadingClip) return;
            if (clip.audio != nullptr) return;
            engine.requestedPianoRollClip[static_cast<std::size_t>(selectedTrack)].store(selectedScene);
            if (engine.activePianoRollClip[static_cast<std::size_t>(selectedTrack)].load() == selectedScene)
                engine.pianoRollNotes[static_cast<std::size_t>(selectedTrack)].publish(
                    clip.pattern);
            else
                publishSessionClip(selectedTrack, selectedScene, false);
        };
        pianoRoll.onAudition = [this](int note, float velocity) {
            if (velocity > 0) keyboardState.noteOn(3, note, velocity);
            else keyboardState.noteOff(3, note, 0);
        };
        for (int scene = 0; scene < sceneCount; ++scene)
            for (int track = 0; track < trackCount; ++track) {
                auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                if (scene == 0 && track == 0) clip.pattern = pianoRoll.getPattern();
            }
        engine.requestedPianoRollClip[0].store(0);
        updateAudioClipEditor();
        publishSessionClip(0, 0, false);
        updateSessionButtons();
        presetLabel.setText("LIVE / STEP SYNTH", juce::dontSendNotification);
        devicePanel.addAndMakeVisible(preset);
        preset.setName("Live and step synth preset");
        preset.setTooltip("Sound used by the computer keyboard, on-screen keyboard, MIDI input and step sequencer");
        preset.addItem("Lead (saw)", 1);
        preset.addItem("Bass (square)", 2);
        preset.addItem("Pluck (sine)", 3);
        preset.setSelectedId(1);
        preset.onChange = [this] {
            releaseNotes();
            engine.preset.store(preset.getSelectedId() - 1);
            stepsPanel.repaint();
        };
        workspace.onViewChanged = [this](bool) { releaseNotes(); syncViewButtons(); };

        // Keyboard dock -------------------------------------------------------
        addAndMakeVisible(keyboard);
        keyboard.clearKeyMappings();
        keyboard.setAvailableRange(0, 127);
        keyboard.setOctaveForMiddleC(4);
        keyboard.setLowestVisibleKey(48);
        keyboard.setName("On-screen keyboard");
        typingInput.octaveChanged = [this] { updateOctave(); };
        for (auto* b : {&octaveDown, &octaveUp}) {
            addAndMakeVisible(b);
            b->setIconOnly(true);
        }
        octaveDown.setButtonText("Octave -"); octaveUp.setButtonText("Octave +");
        octaveDown.setGlyph(ui::Glyph::minus); octaveUp.setGlyph(ui::Glyph::plus);
        octaveDown.setTooltip("Octave down (Z)"); octaveUp.setTooltip("Octave up (X)");
        octaveDown.onClick = [this] { typingInput.typing.shift(-1); updateOctave(); };
        octaveUp.onClick = [this] { typingInput.typing.shift(1); updateOctave(); };
        addAndMakeVisible(octaveReadout);
        octaveReadout.setFont(ui::monoFont(13.0f, true));
        octaveReadout.setColour(juce::Label::textColourId, col::accent);
        octaveReadout.setJustificationType(juce::Justification::centred);
        octaveReadout.setTooltip("Lowest computer-keyboard note");
        addAndMakeVisible(help);
        help.setFont(ui::font(12.0f));
        help.setColour(juce::Label::textColourId, col::textFaint);
        help.setJustificationType(juce::Justification::centredRight);
        updateOctave();
        keyboard.setVelocity(0.8f, true);
        keyboardState.addListener(this);
        const auto audioError = audio.initialiseWithDefaultDevices(0, 2);
        audio.addAudioCallback(&engine);
        syncViewButtons();
        startTimerHz(20);
        resized();
        if (audioError.isNotEmpty()) {
            // Wait until the window has finished constructing before showing the alert.
            showAudioErrorWhenVisible(juce::Component::SafePointer<MainComponent>(this), audioError);
        }
    }
    ~MainComponent() override {
        stopTimer();
        audioFileChooser.reset();
        projectFileChooser.reset();
        audioImportPool.removeAllJobs(true, -1);
        tabs.getTabbedButtonBar().removeChangeListener(this);
        releaseNotes();
        keyboardState.removeListener(this);
        audio.removeAudioCallback(&engine);
        audio.closeAudioDevice();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void resized() override {
        auto area = getLocalBounds();
        const bool compact = getWidth() < 1180;

        // Top bar.
        auto top = area.removeFromTop(topBarHeight).reduced(12, 0);
        brandMark.setBounds(top.removeFromLeft(36).withSizeKeepingCentre(36, 36));
        top.removeFromLeft(8);
        if (!compact) {
            auto brand = top.removeFromLeft(118);
            title.setBounds(brand.removeFromTop(brand.getHeight() / 2 + 2));
            brandSubtitle.setBounds(brand.translated(1, 0));
        }
        title.setVisible(!compact);
        brandSubtitle.setVisible(!compact);
        top.removeFromLeft(compact ? 4 : 10);

        const std::array<int, 4> viewWidths{62, 88, 74, 62};
        auto views = top.removeFromLeft(viewWidths[0] + viewWidths[1] + viewWidths[2] + viewWidths[3] + 12)
                         .withSizeKeepingCentre(viewWidths[0] + viewWidths[1] + viewWidths[2] + viewWidths[3] + 12, 32);
        viewSwitchBounds = views.expanded(4, 4);
        for (std::size_t i = 0; i < viewButtons.size(); ++i) {
            viewButtons[i].setBounds(views.removeFromLeft(viewWidths[i]));
            views.removeFromLeft(4);
        }

        auto files = top.removeFromRight(compact ? 112 : 300);
        const bool iconFiles = compact;
        for (auto* b : {&portableProjectButton, &saveProjectButton, &openProjectButton}) {
            b->setIconOnly(iconFiles);
            const int w = iconFiles ? 34 : (b == &portableProjectButton ? 128 : 80);
            b->setBounds(files.removeFromRight(w).withSizeKeepingCentre(w, 32));
            files.removeFromRight(4);
        }

        const int displayWidth = 300;
        const int transportWidth = 44 + 8 + displayWidth;
        auto transport = top.withSizeKeepingCentre(juce::jmin(top.getWidth(), transportWidth), 44);
        play.setBounds(transport.removeFromLeft(44).withSizeKeepingCentre(44, 40));
        transport.removeFromLeft(8);
        transportDisplay.setBounds(transport);

        // Status bar and keyboard dock.
        auto status = area.removeFromBottom(statusBarHeight).reduced(12, 0);
        engineStatus.setBounds(status.removeFromRight(360));
        projectStatus.setBounds(status.withTrimmedLeft(14));

        auto dock = area.removeFromBottom(getHeight() < 760 ? dockHeight - 22 : dockHeight);
        dockBounds = dock;
        auto dockHeader = dock.removeFromTop(34).reduced(12, 5);
        dockHeader.removeFromLeft(78); // "Keyboard" caption.
        octaveDown.setBounds(dockHeader.removeFromLeft(28));
        octaveReadout.setBounds(dockHeader.removeFromLeft(44));
        octaveUp.setBounds(dockHeader.removeFromLeft(28));
        dockHeader.removeFromLeft(12);
        help.setBounds(dockHeader);
        keyboard.setBounds(dock.reduced(12, 0).withTrimmedBottom(10));
        keyboard.setKeyWidth(juce::jlimit(16.0f, 34.0f, static_cast<float>(keyboard.getWidth()) / 36.0f));
        updateOctave();

        workspace.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(col::window);
        // Top bar.
        const auto top = getLocalBounds().removeFromTop(topBarHeight);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1e2025), 0.0f, 0.0f,
                                               juce::Colour(0xff17181c), 0.0f, static_cast<float>(top.getBottom()), false));
        g.fillRect(top);
        g.setColour(juce::Colours::black);
        g.fillRect(top.getX(), top.getBottom() - 1, top.getWidth(), 1);
        g.setColour(col::inset.withAlpha(0.8f));
        g.fillRoundedRectangle(viewSwitchBounds.toFloat(), 7.0f);

        // Keyboard dock.
        g.setColour(col::raised);
        g.fillRect(dockBounds);
        g.setColour(juce::Colours::black);
        g.fillRect(dockBounds.getX(), dockBounds.getY(), dockBounds.getWidth(), 1);
        ui::drawCaption(g, "Keyboard", {dockBounds.getX() + 14, dockBounds.getY() + 5, 72, 24}, col::textFaint);

        // Status bar.
        const auto status = getLocalBounds().removeFromBottom(statusBarHeight);
        g.setColour(col::window);
        g.fillRect(status);
        g.setColour(juce::Colours::black);
        g.fillRect(status.getX(), status.getY(), status.getWidth(), 1);
        const auto dotColour = projectBusy ? col::warning
            : currentProjectFile.existsAsFile() ? col::accent : col::textFaint;
        g.setColour(dotColour);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({18.0f, static_cast<float>(status.getCentreY())}));
    }

    // The main window stays hidden while the startup splash is up, so wait
    // until it is on screen before showing the audio error.
    static void showAudioErrorWhenVisible(juce::Component::SafePointer<MainComponent> safeThis, juce::String error) {
        juce::Timer::callAfterDelay(250, [safeThis, error] {
            if (safeThis == nullptr) return;
            if (!safeThis->isShowing()) { showAudioErrorWhenVisible(safeThis, error); return; }
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Audio initialization failed",
                "Alien Studio could not open the default audio output device.\n\n" + error,
                "OK", safeThis.getComponent());
        });
    }

    void releaseNotes() {
        pianoRoll.releaseAudition();
        typingInput.release();
        keyboardState.allNotesOff(0);
        midiInput.resetTestButtons();
        engine.midi.requestReset();
        engine.notes.panic.store(true);
    }
private:
    static constexpr int topBarHeight = 58, statusBarHeight = 26, dockHeight = 132;

    // View management -------------------------------------------------------
    void showView(int index) {
        if (workspace.isDeviceView()) workspace.showDevices(false);
        tabs.setCurrentTabIndex(index);
        syncViewButtons();
    }

    void syncViewButtons() {
        const int current = tabs.getCurrentTabIndex();
        for (std::size_t i = 0; i < viewButtons.size(); ++i)
            viewButtons[i].setToggleState(!workspace.isDeviceView() && static_cast<int>(i) == current,
                                          juce::dontSendNotification);
        updateEditorTitle();
    }

    void updateEditorTitle() {
        const auto clipName = "Track " + juce::String(selectedTrack + 1) + ui::utf8("  \xc2\xb7  Scene ") + juce::String(selectedScene + 1);
        switch (tabs.getCurrentTabIndex()) {
        case 0: workspace.setEditorTitle("Step Sequencer", ui::utf8("16 steps  \xc2\xb7  1/16 notes  \xc2\xb7  ") + preset.getText()); break;
        case 1: workspace.setEditorTitle("Piano Roll", clipName); break;
        case 2: workspace.setEditorTitle("Session", ui::utf8("4 tracks  \xc2\xb7  4 scenes  \xc2\xb7  launch on next ")
                                         + juce::String(engine.launchQuantizationSteps.load() == 4 ? "beat" : "bar")); break;
        case 3: workspace.setEditorTitle("Mixer", "4 channels"); break;
        default: break;
        }
    }

    // Step sequencer --------------------------------------------------------
    void refreshPadInfo() {
        for (int i = 0; i < pads.size(); ++i) {
            const auto index = static_cast<std::size_t>(i);
            pads[i]->setStepInfo(juce::MidiMessage::getMidiNoteName(engine.stepPitches[index].load(), true, true, 4),
                                 static_cast<float>(engine.stepVelocities[index].load()) / 127.0f, i == selectedStep);
        }
    }

    void layoutStepsPanel() {
        auto r = stepsPanel.getLocalBounds().reduced(20, 16);
        stepHelp.setBounds(r.removeFromTop(22));
        r.removeFromTop(14);
        const bool singleRow = r.getWidth() >= 860;
        const int channelWidth = r.getWidth() >= 1000 ? 150 : 0;
        const int rowHeight = juce::jlimit(96, 150, (r.getHeight() - 140) / (singleRow ? 2 : 3));
        auto rack = r.removeFromTop(singleRow ? rowHeight : rowHeight * 2 + 8);
        rackBounds = rack;
        channelCardBounds = rack.removeFromLeft(channelWidth);
        if (channelWidth > 0) rack.removeFromLeft(12);
        const int perRow = singleRow ? 16 : 8;
        const int groupGap = 10, padGap = 4;
        const int groups = perRow / 4;
        const int padW = (rack.getWidth() - (groups - 1) * groupGap - (perRow - groups) * padGap) / perRow;
        const int padH = singleRow ? rack.getHeight() : (rack.getHeight() - 8) / 2;
        for (int i = 0; i < pads.size(); ++i) {
            const int col = i % perRow, row = i / perRow;
            const int x = rack.getX() + col * (padW + padGap) + (col / 4) * (groupGap - padGap);
            pads[i]->setBounds(x, rack.getY() + row * (padH + 8), padW, padH);
        }
        r.removeFromTop(18);
        auto inspector = r.removeFromTop(104);
        inspectorBounds = inspector;
        auto row = inspector.reduced(16, 0).withTrimmedTop(56).withHeight(30);
        auto stepCol = row.removeFromLeft(110);
        selectedStepLabel.setBounds(stepCol.getX(), stepCol.getY() - 18, 100, 16);
        stepSelector.setBounds(stepCol.withWidth(100));
        row.removeFromLeft(14);
        const int sliderWidth = (row.getWidth() - 70 - 24) / 2;
        auto pitchCol = row.removeFromLeft(sliderWidth);
        stepPitchLabel.setBounds(pitchCol.getX(), pitchCol.getY() - 18, 100, 16);
        stepPitch.setBounds(pitchCol);
        stepPitchName.setBounds(row.removeFromLeft(64));
        row.removeFromLeft(24);
        stepVelocityLabel.setBounds(row.getX(), row.getY() - 18, 100, 16);
        stepVelocity.setBounds(row);
    }

    void paintStepsPanel(juce::Graphics& g) {
        g.fillAll(col::surface);
        if (!channelCardBounds.isEmpty()) {
            auto card = channelCardBounds.toFloat();
            ui::drawCard(g, card, col::raised);
            g.setColour(col::accent);
            g.fillRoundedRectangle(card.removeFromLeft(4.0f).reduced(0.0f, 1.0f), 2.0f);
            auto text = channelCardBounds.reduced(14, 12);
            ui::drawCaption(g, "Channel", text.removeFromTop(14));
            g.setColour(col::text);
            g.setFont(ui::font(14.0f, true));
            g.drawText("Step Synth", text.removeFromTop(22), juce::Justification::centredLeft, true);
            g.setColour(col::textDim);
            g.setFont(ui::font(12.5f));
            g.drawText(preset.getText(), text.removeFromTop(18), juce::Justification::centredLeft, true);
            g.setColour(col::textFaint);
            g.drawText(engine.playing.load() && engine.patternMode.load() == 0 ? "Playing" : ui::utf8("1 bar  \xc2\xb7  16 steps"),
                       text.removeFromTop(18), juce::Justification::centredLeft, true);
        }
        ui::drawCard(g, inspectorBounds.toFloat(), col::raised);
        ui::drawCaption(g, "Step " + juce::String(selectedStep + 1) + " inspector",
                        inspectorBounds.reduced(16, 0).removeFromTop(30), col::textDim);
    }

    // Devices ---------------------------------------------------------------
    void layoutDevicePanel() {
        auto r = devicePanel.getLocalBounds().reduced(20, 16);
        deviceHelp.setBounds(r.removeFromTop(22));
        r.removeFromTop(14);
        const int cardWidth = juce::jmin(560, r.getWidth());
        synthCardBounds = r.removeFromTop(112).withWidth(cardWidth);
        r.removeFromTop(14);
        midiCardBounds = r.removeFromTop(112).withWidth(cardWidth);
        auto synth = synthCardBounds.reduced(18, 14).withTrimmedLeft(4);
        presetLabel.setBounds(synth.removeFromTop(16));
        synth.removeFromTop(10);
        preset.setBounds(synth.removeFromTop(32).withWidth(juce::jmin(280, synth.getWidth())));
        auto midi = midiCardBounds.reduced(18, 14).withTrimmedLeft(4);
        midiLabel.setBounds(midi.removeFromTop(16));
        midi.removeFromTop(10);
        midiInput.setBounds(midi.removeFromTop(32));
    }

    void paintDevicePanel(juce::Graphics& g) {
        g.fillAll(col::surface);
        for (auto bounds : {synthCardBounds, midiCardBounds}) {
            auto card = bounds.toFloat();
            ui::drawCard(g, card, col::raised);
            g.setColour(bounds == synthCardBounds ? col::accent : col::solo);
            g.fillRoundedRectangle(card.removeFromLeft(4.0f).reduced(0.0f, 1.0f), 2.0f);
        }
        g.setColour(col::textFaint);
        g.setFont(ui::font(12.0f));
        g.drawText("Plays from the computer keyboard, the on-screen keyboard, MIDI input and the step sequencer.",
                   synthCardBounds.reduced(22, 14).removeFromBottom(18), juce::Justification::centredLeft, true);
        g.drawText("All 16 MIDI channels play the live synth. Hot-plugged controllers reconnect automatically.",
                   midiCardBounds.reduced(22, 14).removeFromBottom(18), juce::Justification::centredLeft, true);
    }

    // Session ---------------------------------------------------------------
    void layoutSessionPanel() {
        auto r = sessionPanel.getLocalBounds().reduced(16, 12);
        auto header = r.removeFromTop(30);
        launchTiming.setBounds(header.removeFromRight(96).reduced(0, 2));
        launchTimingLabel.setBounds(header.removeFromRight(70));
        sessionHelp.setBounds(header);
        r.removeFromTop(8);

        const int sceneColumn = 132, gap = 6;
        const int trackWidth = (r.getWidth() - sceneColumn - gap * trackCount) / trackCount;
        const int clipHeight = juce::jlimit(30, 40, (r.getHeight() - 30 - 40 - 50 - 90) / sceneCount - gap);
        const int headerHeight = 30;
        trackHeaderBounds = r.withHeight(headerHeight);
        const int gridTop = r.getY() + headerHeight + gap;
        for (int scene = 0; scene < sceneCount; ++scene) {
            const int y = gridTop + scene * (clipHeight + gap);
            for (int track = 0; track < trackCount; ++track)
                clipButtons[cellIndex(track, scene)].setBounds(r.getX() + track * (trackWidth + gap), y, trackWidth, clipHeight);
            sceneButtons[static_cast<std::size_t>(scene)].setBounds(r.getRight() - sceneColumn, y, sceneColumn, clipHeight);
        }
        const int stopY = gridTop + sceneCount * (clipHeight + gap);
        stopClip.setBounds(r.getRight() - sceneColumn, stopY, sceneColumn, 28);
        gridBounds = juce::Rectangle<int>(r.getX(), r.getY(), r.getWidth(), stopY + 28 - r.getY());
        r.setTop(stopY + 28 + 14);

        auto actions = r.removeFromTop(30);
        actionBarBounds = actions;
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

    void paintSessionPanel(juce::Graphics& g) {
        g.fillAll(col::surface);
        const int sceneColumn = 132, gap = 6;
        const int trackWidth = (gridBounds.getWidth() - sceneColumn - gap * trackCount) / trackCount;
        for (int track = 0; track < trackCount; ++track) {
            auto h = juce::Rectangle<int>(gridBounds.getX() + track * (trackWidth + gap), trackHeaderBounds.getY(),
                                          trackWidth, trackHeaderBounds.getHeight()).toFloat();
            // Column backdrop.
            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.fillRoundedRectangle(h.withBottom(static_cast<float>(gridBounds.getBottom())), 5.0f);
            const auto colour = ui::trackColour(track);
            g.setColour(col::raised);
            g.fillRoundedRectangle(h, 5.0f);
            g.setColour(colour);
            g.fillRoundedRectangle(h.removeFromTop(3.0f), 1.5f);
            const bool active = engine.activePianoRollClip[static_cast<std::size_t>(track)].load() >= 0;
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
            static const std::array<const char*, 3> presetNames{"Lead", "Bass", "Pluck"};
            const int presetIndex = juce::jlimit(0, 2, engine.tracks[static_cast<std::size_t>(track)].preset.load());
            g.setColour(col::textDim);
            g.setFont(ui::font(12.5f));
            g.drawText(presetNames[static_cast<std::size_t>(presetIndex)], text, juce::Justification::centredLeft, true);
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
        ui::drawCaption(g, ui::utf8("Clip  \xc2\xb7  Track ") + juce::String(selectedTrack + 1) + ui::utf8("  \xc2\xb7  Scene ") + juce::String(selectedScene + 1),
                        clipDetailBounds.reduced(12, 8).removeFromTop(20), ui::trackColour(selectedTrack));
    }

    juce::Rectangle<int> viewSwitchBounds, dockBounds;
    juce::Rectangle<int> rackBounds, channelCardBounds, inspectorBounds;
    juce::Rectangle<int> synthCardBounds, midiCardBounds;
    juce::Rectangle<int> trackHeaderBounds, gridBounds, actionBarBounds, mappingBounds, clipDetailBounds;
    void selectSequencerStep(int step) {
        selectedStep = juce::jlimit(0, 15, step);
        stepSelector.setSelectedId(selectedStep + 1, juce::dontSendNotification);
        const auto index = static_cast<std::size_t>(selectedStep);
        const int pitch = engine.stepPitches[index].load();
        stepPitch.setValue(pitch, juce::dontSendNotification);
        stepPitchName.setText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 4),
            juce::dontSendNotification);
        stepVelocity.setValue(engine.stepVelocities[index].load(), juce::dontSendNotification);
        refreshPadInfo();
        stepsPanel.repaint(inspectorBounds);
    }

    void updateOctave() {
        help.setText(ui::utf8("Computer keys  A W S E D F T G Y H U J  \xc2\xb7  Z / X octave  \xc2\xb7  works outside text fields"),
                     juce::dontSendNotification);
        octaveReadout.setText("C" + juce::String(typingInput.typing.octave()), juce::dontSendNotification);
        keyboard.setLowestVisibleKey(12 * (typingInput.typing.octave() + 1));
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const int newMode = tabs.getCurrentTabIndex() == 0 ? 0 : 1;
        if (engine.patternMode.exchange(newMode) != newMode) releaseNotes();
        else pianoRoll.releaseAudition();
        syncViewButtons();
    }
    void timerCallback() override {
        const bool isPlaying = engine.playing.load();
        if (play.getToggleState() != isPlaying) {
            play.setToggleState(isPlaying, juce::dontSendNotification);
            play.setGlyph(isPlaying ? ui::Glyph::stop : ui::Glyph::play);
            stepsPanel.repaint(channelCardBounds);
        }
        const juce::String transportText = isPlaying ? "Stop" : "Play";
        if (play.getButtonText() != transportText) play.setButtonText(transportText);
        const bool stepMode = engine.patternMode.load() == 0;
        const int currentStep = stepMode && isPlaying ? engine.currentStep.load() : -1;
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setPlaybackPosition(i == currentStep);
        int displayStep = currentStep;
        if (!stepMode) {
            const int position = engine.sessionTrackPositions[static_cast<std::size_t>(selectedTrack)].load();
            displayStep = position >= 0 ? position / 1000 : -1;
        }
        transportDisplay.setPosition(displayStep, isPlaying || displayStep >= 0, stepMode ? "Steps" : "Session");
        midiInput.tick();
        mixerPanel.tick();
        int controllerAction = -1;
        while (engine.controllerActions.pop(controllerAction)) {
            if (controllerAction < sceneCount) launchScene(controllerAction);
            else if (controllerAction == alien::MidiControllerMappings::stopAllAction) stopAllSessionTracks();
        }
        const int learnedAction = engine.controllerMappings.takeLearnedAction();
        if (learnedAction >= 0) {
            if (learningControllerAction == learnedAction) {
                learningControllerAction = -1;
                learnController.setButtonText("Learn CC");
                learnController.setToggleState(false, juce::dontSendNotification);
            }
            updateControllerMappingLabel();
        }
        for (int note = 0; note < 128; ++note) {
            const bool on = engine.midi.isNoteOn(note);
            if (on != keyboardState.isNoteOn(4, note)) {
                if (on) keyboardState.noteOn(4, note, 0.8f);
                else keyboardState.noteOff(4, note, 0.0f);
            }
        }
        pianoRoll.setPlayheadStep(engine.sessionTrackPositions[static_cast<std::size_t>(selectedTrack)].load() / 1000.0);
        updateSessionButtons();
        if (sessionPanel.isShowing()) sessionPanel.repaint(trackHeaderBounds);
        if (++statusTicks % 10 == 0) updateEngineStatus();
    }
    void updateEngineStatus() {
        juce::String text = "No audio device";
        if (auto* device = audio.getCurrentAudioDevice()) {
            auto name = device->getName();
            if (name.length() > 28) name = name.substring(0, 27) + "...";
            text = name + ui::utf8("  \xc2\xb7  ") + juce::String(device->getCurrentSampleRate() / 1000.0, 1) + ui::utf8(" kHz  \xc2\xb7  ")
                + juce::String(device->getCurrentBufferSizeSamples()) + ui::utf8(" smp  \xc2\xb7  CPU ")
                + juce::String(juce::roundToInt(audio.getCpuUsage() * 100.0)) + "%";
        }
        engineStatus.setText(text, juce::dontSendNotification);
    }
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override {
        if (channel == 4) return; // MIDI highlight only; sound uses its own audio pipe.
        if (!keyboardState.isNoteOnForChannels(0x7 & ~(1 << (channel - 1)), note))
            engine.notes.push(note, velocity);
    }
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float) override {
        if (channel == 4) return;
        if (!keyboardState.isNoteOnForChannels(0x7, note)) engine.notes.push(note, 0);
    }
    void selectAndLaunchClip(int clip) {
        selectAndLaunchClip(selectedTrack, clip);
    }
    void selectAndLaunchClip(int track, int scene) {
        loadClip(track, scene);
        publishSessionClip(selectedTrack, selectedScene, false);
        updateSessionButtons();
    }
    void selectClipForEditing(int clip) {
        selectClipForEditing(selectedTrack, clip);
    }
    void selectClipForEditing(int track, int scene) {
        loadClip(track, scene);
        if (clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].audio != nullptr)
            tabs.setCurrentTabIndex(2);
        else {
            tabs.setCurrentTabIndex(1);
            pianoRoll.grabKeyboardFocus();
        }
        updateSessionButtons();
    }
    static constexpr int trackCount = 4, sceneCount = 4;
    static constexpr std::size_t maxSessionAudioBytes = 512u * 1024u * 1024u;
    static constexpr std::size_t cellIndex(int track, int scene) noexcept {
        return static_cast<std::size_t>(scene * trackCount + track);
    }
    void saveEditedClip() {
        clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].pattern = pianoRoll.getPattern();
    }
    void loadClip(int track, int scene) {
        saveEditedClip();
        selectedTrack = juce::jlimit(0, trackCount - 1, track);
        selectedScene = juce::jlimit(0, sceneCount - 1, scene);
        loadingClip = true;
        pianoRoll.setPattern(clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].pattern);
        loadingClip = false;
        updateAudioClipEditor();
        updateEditorTitle();
        sessionPanel.repaint(clipDetailBounds);
    }
    void launchScene(int scene) {
        saveEditedClip();
        scene = juce::jlimit(0, sceneCount - 1, scene);
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < trackCount; ++track) {
            const auto index = static_cast<std::size_t>(track);
            engine.requestedPianoRollClip[index].store(scene);
            publishSessionClip(track, scene, true);
        }
        updateSessionButtons();
    }
    void stopAllSessionTracks() {
        saveEditedClip();
        engine.synchronizedSceneLaunch.store(true);
        for (int track = 0; track < trackCount; ++track) {
            engine.requestedPianoRollClip[static_cast<std::size_t>(track)].store(-1);
            alien::SessionClipLaunch stopped;
            stopped.pattern.clear();
            stopped.clipIndex = -1;
            engine.pianoRollLaunches[static_cast<std::size_t>(track)].publish(stopped);
        }
        updateSessionButtons();
    }
    void publishSessionClip(int track, int scene, bool) {
        const auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        engine.requestedPianoRollClip[static_cast<std::size_t>(track)].store(scene);
        alien::SessionClipLaunch launch;
        launch.pattern = clip.audio == nullptr ? clip.pattern : alien::PianoRollPattern{};
        launch.audio = clip.audio;
        launch.clipIndex = scene;
        engine.pianoRollLaunches[static_cast<std::size_t>(track)].publish(launch);
    }
    void saveProject() {
        if (currentProjectFile.existsAsFile()) writeProject(currentProjectFile);
        else chooseProjectSavePath();
    }
    void chooseProjectSavePath(bool portable = false) {
        projectFileChooser = std::make_unique<juce::FileChooser>("Save Alien Studio session",
            currentProjectFile.getParentDirectory().exists()
                ? currentProjectFile : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("AlienStudioSession.alienstudio"), "*.alienstudio");
        juce::Component::SafePointer<MainComponent> safe(this);
        projectFileChooser->launchAsync(juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe, portable](const juce::FileChooser& chooser) {
                if (safe != nullptr && chooser.getResult() != juce::File{})
                    safe->writeProject(chooser.getResult(), portable);
            });
    }
    void writeProject(juce::File file, bool portable = false) {
        if (projectBusy) return;
        saveEditedClip();
        alien::SessionProjectData project;
        project.launchQuantizationSteps = engine.launchQuantizationSteps.load();
        project.selectedTrack = selectedTrack;
        project.selectedScene = selectedScene;
        project.bpm = engine.bpm.load();
        project.synthPreset = engine.preset.load();
        for (std::size_t i = 0; i < engine.tracks.size(); ++i)
            project.trackSettings[i] = engine.tracks[i].snapshot();
        for (std::size_t i = 0; i < project.stepPattern.size(); ++i)
            project.stepPattern[i] = engine.steps[i].load();
        for (std::size_t i = 0; i < project.stepPitches.size(); ++i) {
            project.stepPitches[i] = engine.stepPitches[i].load();
            project.stepVelocities[i] = engine.stepVelocities[i].load();
        }
        for (int scene = 0; scene < sceneCount; ++scene)
            for (int track = 0; track < trackCount; ++track) {
                const auto& source = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                auto& destination = project.clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                destination.pattern = source.pattern;
                destination.audioFilePath = source.audioFilePath;
            }
        const auto json = alien::serializeSessionProject(project);
        if (json.isEmpty()) {
            projectStatus.setText("Could not save: session data is invalid", juce::dontSendNotification);
            return;
        }
        const auto target = file.hasFileExtension("alienstudio") ? file : file.withFileExtension("alienstudio");
        setProjectBusy(true);
        projectStatus.setText(portable ? ui::utf8("Collecting audio\xe2\x80\xa6") : ui::utf8("Saving project\xe2\x80\xa6"), juce::dontSendNotification);
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> saveJob = [safe, target, project, portable] {
            auto saved = std::make_shared<alien::SessionProjectData>();
            juce::String error;
            const bool success = alien::writeSessionProjectFile(target, project, portable, *saved, error);
            juce::MessageManager::callAsync([safe, target, success, saved, project, error] {
                if (safe != nullptr) {
                    safe->setProjectBusy(false);
                    if (success) {
                        // Update only audio references from this snapshot; preserve edits made while saving.
                        for (int scene = 0; scene < sceneCount; ++scene)
                            for (int track = 0; track < trackCount; ++track) {
                                auto& current = safe->clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                                const auto& old = project.clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                                const auto& written = saved->clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                                if (current.audioFilePath == old.audioFilePath && written.audioFilePath.isNotEmpty())
                                    current.audioFilePath = alien::resolveProjectAudio(target, written.audioFilePath).getFullPathName();
                            }
                        safe->currentProjectFile = target;
                        safe->projectStatus.setText("Saved " + target.getFileName(), juce::dontSendNotification);
                        safe->projectStatus.setTooltip(target.getFullPathName());
                    } else {
                        safe->projectStatus.setText("Save failed: " + error, juce::dontSendNotification);
                        safe->projectStatus.setTooltip(error);
                    }
                }
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioImportPool.addJob(std::move(saveJob));
    }
    void setProjectBusy(bool busy) {
        projectBusy = busy;
        saveProjectButton.setEnabled(!busy); portableProjectButton.setEnabled(!busy);
        openProjectButton.setEnabled(!busy); importAudio.setEnabled(!busy);
        repaint(0, getHeight() - statusBarHeight, 40, statusBarHeight);
    }
    void chooseProjectToOpen() {
        projectFileChooser = std::make_unique<juce::FileChooser>("Open Alien Studio session",
            currentProjectFile.getParentDirectory().exists() ? currentProjectFile : juce::File{},
            "*.alienstudio");
        juce::Component::SafePointer<MainComponent> safe(this);
        projectFileChooser->launchAsync(juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
            [safe](const juce::FileChooser& chooser) {
                if (safe != nullptr && chooser.getResult().existsAsFile())
                    safe->startProjectLoad(chooser.getResult());
            });
    }
    void startProjectLoad(const juce::File& file) {
        if (projectBusy) return;
        setProjectBusy(true);
        projectStatus.setText(ui::utf8("Opening project\xe2\x80\xa6"), juce::dontSendNotification);
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> loadJob = [safe, file] {
            auto result = std::make_shared<SessionProjectLoadResult>();
            if (file.getSize() <= 0 || file.getSize() > 4 * 1024 * 1024) {
                result->error = "Project file is empty or exceeds the 4 MiB limit.";
            } else {
                const auto json = file.loadFileAsString();
                if (!alien::parseSessionProject(json, result->project, result->error)) {
                    // Keep the parser's specific validation error.
                } else {
                    for (int scene = 0; scene < sceneCount && result->error.isEmpty(); ++scene)
                        for (int track = 0; track < trackCount && result->error.isEmpty(); ++track) {
                            const auto& clip = result->project.clips[static_cast<std::size_t>(scene)]
                                [static_cast<std::size_t>(track)];
                            if (clip.audioFilePath.isEmpty()) continue;
                            const auto audioFile = alien::resolveProjectAudio(file, clip.audioFilePath);
                            const auto decoded = alien::decodeSessionAudioClip(audioFile);
                            if (!decoded.clip) {
                                result->error = "Track " + juce::String(track + 1) + ", Scene "
                                    + juce::String(scene + 1) + ": " + decoded.error;
                                break;
                            }
                            const auto bytes = decoded.clip->frames * static_cast<std::size_t>(decoded.clip->channels)
                                * sizeof(float);
                            if (bytes > maxSessionAudioBytes - result->decodedBytes) {
                                result->error = "Project audio exceeds the 512 MiB session limit.";
                                break;
                            }
                            result->decodedBytes += bytes;
                            result->audioAssets[cellIndex(track, scene)] = decoded.clip;
                        }
                }
            }
            juce::MessageManager::callAsync([safe, file, result] {
                if (safe != nullptr) safe->finishProjectLoad(file, *result);
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioImportPool.addJob(std::move(loadJob));
    }
    void finishProjectLoad(const juce::File& file, const SessionProjectLoadResult& result) {
        setProjectBusy(false);
        if (result.error.isNotEmpty()) {
            projectStatus.setText("Open failed: " + result.error, juce::dontSendNotification);
            projectStatus.setTooltip(result.error);
            return;
        }
        if (result.decodedBytes > maxSessionAudioBytes - retainedAudioBytes) {
            projectStatus.setText("Open failed: retained audio memory limit reached", juce::dontSendNotification);
            projectStatus.setTooltip("Close and restart Alien Studio to release audio from older sessions.");
            return;
        }

        saveEditedClip();
        engine.playing.store(false);
        engine.synchronizedSceneLaunch.store(false);
        releaseNotes();
        for (std::size_t track = 0; track < ToneEngine::sessionTracks; ++track) {
            engine.requestedPianoRollClip[track].store(-1);
            alien::SessionClipLaunch stopped;
            stopped.pattern.clear();
            stopped.clipIndex = -1;
            engine.pianoRollLaunches[track].publish(stopped);
        }
        for (int scene = 0; scene < sceneCount; ++scene)
            for (int track = 0; track < trackCount; ++track) {
                const auto index = cellIndex(track, scene);
                const auto& source = result.project.clips[static_cast<std::size_t>(scene)]
                    [static_cast<std::size_t>(track)];
                auto& destination = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
                destination.pattern = source.pattern;
                destination.audioFilePath = source.audioFilePath.isEmpty() ? juce::String{}
                    : alien::resolveProjectAudio(file, source.audioFilePath).getFullPathName();
                const auto& asset = result.audioAssets[index];
                destination.audio = asset.get();
                destination.audioFileName = source.audioFilePath.isNotEmpty()
                    ? alien::resolveProjectAudio(file, source.audioFilePath).getFileName() : juce::String{};
                if (asset != nullptr) retainedAudioAssets.push_back(asset);
            }
        retainedAudioBytes += result.decodedBytes;
        selectedTrack = result.project.selectedTrack;
        selectedScene = result.project.selectedScene;
        launchTiming.setSelectedId(result.project.launchQuantizationSteps == 4 ? 1 : 2,
            juce::dontSendNotification);
        engine.launchQuantizationSteps.store(result.project.launchQuantizationSteps);
        tempo.setValue(result.project.bpm, juce::dontSendNotification);
        engine.bpm.store(result.project.bpm);
        preset.setSelectedId(result.project.synthPreset + 1, juce::dontSendNotification);
        engine.preset.store(result.project.synthPreset);
        for (std::size_t i = 0; i < engine.tracks.size(); ++i)
            engine.tracks[i].set(result.project.trackSettings[i]);
        mixerPanel.refreshSettings();
        for (std::size_t i = 0; i < result.project.stepPattern.size(); ++i) {
            engine.steps[i].store(result.project.stepPattern[i]);
            pads[static_cast<int>(i)]->setToggleState(result.project.stepPattern[i], juce::dontSendNotification);
            engine.stepPitches[i].store(result.project.stepPitches[i]);
            engine.stepVelocities[i].store(result.project.stepVelocities[i]);
        }
        selectSequencerStep(selectedStep);
        updateEditorTitle();
        loadingClip = true;
        pianoRoll.setPattern(clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)].pattern);
        loadingClip = false;
        currentProjectFile = file;
        updateAudioClipEditor();
        updateSessionButtons();
        tabs.setCurrentTabIndex(clips[static_cast<std::size_t>(selectedScene)]
            [static_cast<std::size_t>(selectedTrack)].audio != nullptr ? 2 : 1);
        projectStatus.setText("Opened " + file.getFileName(), juce::dontSendNotification);
        projectStatus.setTooltip(file.getFullPathName());
    }
    void chooseAudioFile() {
        audioFileChooser = std::make_unique<juce::FileChooser>("Import audio clip",
            juce::File{}, "*.wav;*.mp3");
        juce::Component::SafePointer<MainComponent> safe(this);
        audioFileChooser->launchAsync(juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
            [safe](const juce::FileChooser& chooser) {
                if (safe && chooser.getResult().existsAsFile())
                    safe->startAudioImport(chooser.getResult(), safe->selectedTrack, safe->selectedScene);
            });
    }
    void startAudioImport(const juce::File& file, int track, int scene) {
        audioImportStatus.setText(ui::utf8("Loading audio\xe2\x80\xa6"), juce::dontSendNotification);
        audioImportStatus.setTooltip(file.getFullPathName());
        juce::Component::SafePointer<MainComponent> safe(this);
        std::function<juce::ThreadPoolJob::JobStatus()> importJob = [safe, file, track, scene] {
            auto result = std::make_shared<alien::AudioClipImportResult>(alien::decodeSessionAudioClip(file));
            juce::MessageManager::callAsync([safe, file, track, scene, result] {
                if (safe != nullptr) safe->finishAudioImport(file, track, scene, *result);
            });
            return juce::ThreadPoolJob::jobHasFinished;
        };
        audioImportPool.addJob(std::move(importJob));
    }
    void finishAudioImport(const juce::File& file, int track, int scene,
                           const alien::AudioClipImportResult& result) {
        if (!result.clip) {
            audioImportStatus.setText(result.error, juce::dontSendNotification);
            audioImportStatus.setTooltip(result.error);
            return;
        }
        const auto* data = result.clip.get();
        const auto bytes = data->frames * static_cast<std::size_t>(data->channels) * sizeof(float);
        if (bytes > maxSessionAudioBytes - retainedAudioBytes) {
            audioImportStatus.setText("Session audio storage is full (512 MiB limit)", juce::dontSendNotification);
            audioImportStatus.setTooltip(audioImportStatus.getText());
            return;
        }
        retainedAudioBytes += bytes;
        retainedAudioAssets.push_back(result.clip);
        auto& clip = clips[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)];
        clip.audio = data;
        clip.audioFileName = file.getFileName();
        clip.audioFilePath = file.getFullPathName();
        if (track == selectedTrack && scene == selectedScene) {
            updateAudioClipEditor();
            tabs.setCurrentTabIndex(2);
        }
        if (engine.activePianoRollClip[static_cast<std::size_t>(track)].load() == scene
            || engine.requestedPianoRollClip[static_cast<std::size_t>(track)].load() == scene)
            publishSessionClip(track, scene, false);
        audioImportStatus.setText("Imported " + file.getFileName(), juce::dontSendNotification);
        audioImportStatus.setTooltip(file.getFullPathName());
        updateSessionButtons();
    }
    void removeAudioFromSelectedClip() {
        auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        if (clip.audio == nullptr) return;
        clip.audio = nullptr;
        clip.audioFileName.clear();
        clip.audioFilePath.clear();
        updateAudioClipEditor();
        const int active = engine.activePianoRollClip[static_cast<std::size_t>(selectedTrack)].load();
        const int requested = engine.requestedPianoRollClip[static_cast<std::size_t>(selectedTrack)].load();
        if (active == selectedScene || requested == selectedScene)
            publishSessionClip(selectedTrack, selectedScene, false);
        audioImportStatus.setText("Audio removed from selected clip", juce::dontSendNotification);
        updateSessionButtons();
    }
    void clearSelectedClip() {
        auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        const bool hadAudio = clip.audio != nullptr;
        if (hadAudio) removeAudioFromSelectedClip();
        // The selected clip is always the one loaded in the piano roll, so this
        // clears it as an undoable edit and republishes it through onChange.
        pianoRoll.clearAllNotes();
        audioImportStatus.setText("Cleared Track " + juce::String(selectedTrack + 1) + ", Scene "
            + juce::String(selectedScene + 1), juce::dontSendNotification);
        updateAudioClipEditor();
        updateSessionButtons();
    }
    void updateAudioClipEditor() {
        const auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        clearClip.setEnabled(clip.audio != nullptr || clip.pattern.count > 0);
        audioWaveform.setClip(clip.audio, clip.audioFileName);
        removeAudio.setEnabled(clip.audio != nullptr);
        if (clip.audio == nullptr && audioImportStatus.getText().isEmpty())
            audioImportStatus.setText("Selected clip: Track " + juce::String(selectedTrack + 1)
                + ", Scene " + juce::String(selectedScene + 1), juce::dontSendNotification);
    }
    void updateControllerMappingLabel() {
        clearController.setEnabled(learningControllerAction >= 0
            || engine.controllerMappings.mappingFor(controllerTarget.getSelectedId() - 1) >= 0);
        if (learningControllerAction == controllerTarget.getSelectedId() - 1) {
            controllerMappingLabel.setText(ui::utf8("Move a MIDI CC control\xe2\x80\xa6"), juce::dontSendNotification);
            return;
        }
        const int code = engine.controllerMappings.mappingFor(controllerTarget.getSelectedId() - 1);
        if (code < 0) controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        else controllerMappingLabel.setText("CC " + juce::String(code % 128) + "  |  Ch "
            + juce::String(code / 128 + 1), juce::dontSendNotification);
    }
    void updateSessionButtons() {
        std::array<bool, sceneCount> scenePlaying{};
        for (int track = 0; track < trackCount; ++track) {
            const int active = engine.activePianoRollClip[static_cast<std::size_t>(track)].load();
            if (active >= 0 && active < sceneCount) scenePlaying[static_cast<std::size_t>(active)] = true;
        }
        for (int scene = 0; scene < sceneCount; ++scene) {
            auto& sceneButton = sceneButtons[static_cast<std::size_t>(scene)];
            const bool playingScene = scenePlaying[static_cast<std::size_t>(scene)];
            const auto sceneColour = playingScene ? juce::Colour(0xff1d3a31) : scene == selectedScene ? col::hover : col::control;
            if (sceneButton.findColour(juce::TextButton::buttonColourId) != sceneColour) {
                sceneButton.setColour(juce::TextButton::buttonColourId, sceneColour);
                sceneButton.setGlyphColour(playingScene ? col::accent : col::textDim);
            }
            sceneButton.setTitle((scene == selectedScene ? "Selected, " : "") + juce::String("Launch scene ")
                                 + juce::String(scene + 1));
            for (int track = 0; track < trackCount; ++track) {
                const auto index = static_cast<std::size_t>(track);
                const int active = engine.activePianoRollClip[index].load();
                const int requested = engine.requestedPianoRollClip[index].load();
                const auto& clip = clips[static_cast<std::size_t>(scene)][index];
                auto text = clip.audio != nullptr
                    ? "Track " + juce::String(track + 1) + " | AUDIO\n" + clip.audioFileName.substring(0, 20)
                    : "Track " + juce::String(track + 1) + "\nScene " + juce::String(scene + 1);
                if (scene == active) text += " | Playing";
                else if (scene == requested) text += " | Queued";
                if (track == selectedTrack && scene == selectedScene) text += " | Edit";
                auto& button = clipButtons[cellIndex(track, scene)];
                if (button.getButtonText() != text) button.setButtonText(text);

                alien::SessionClipButton::Visual visual;
                const auto noteCount = static_cast<int>(clip.pattern.count);
                visual.kind = clip.audio != nullptr ? alien::SessionClipButton::Kind::audio
                    : noteCount > 0 ? alien::SessionClipButton::Kind::midi : alien::SessionClipButton::Kind::empty;
                visual.colour = ui::trackColour(track);
                visual.label = clip.audio != nullptr ? clip.audioFileName
                    : juce::String(noteCount) + (noteCount == 1 ? " note" : " notes");
                visual.playing = scene == active;
                visual.queued = scene != active && scene == requested;
                visual.selected = track == selectedTrack && scene == selectedScene;
                if (visual.playing && clip.audio == nullptr && clip.pattern.loopSteps > 0) {
                    const int position = engine.sessionTrackPositions[index].load();
                    if (position >= 0)
                        visual.progress = static_cast<float>(std::fmod(position / 1000.0, static_cast<double>(clip.pattern.loopSteps))
                                                             / static_cast<double>(clip.pattern.loopSteps));
                }
                button.setVisual(visual);
            }
        }
    }
    ui::StudioLookAndFeel studioLookAndFeel;
    juce::TooltipWindow tooltipWindow{this, 650};
    ToneEngine engine;
    alien::MidiInputPanel midiInput{engine.midi};
    alien::TrackMixerPanel mixerPanel{engine.tracks};
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard{keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard};
    ui::GlyphButton octaveDown, octaveUp;
    juce::Label help, octaveReadout;
    juce::ComboBox preset;
    alien::branding::LogoMark brandMark;
    juce::Label title;
    juce::Label brandSubtitle, tempoLabel;
    std::array<ui::GlyphButton, 4> viewButtons;
    ui::GlyphButton play;
    ui::GlyphButton saveProjectButton, openProjectButton, portableProjectButton;
    juce::Label projectStatus, engineStatus;
    juce::Slider tempo{juce::Slider::LinearBarVertical, juce::Slider::NoTextBox};
    ui::TransportDisplay transportDisplay{tempo};
    int statusTicks{};
    juce::OwnedArray<SequencerStepButton> pads;
    ui::PaintedPanel stepsPanel;
    juce::Label stepHelp;
    juce::Label selectedStepLabel, stepPitchLabel, stepPitchName, stepVelocityLabel;
    juce::ComboBox stepSelector;
    juce::Slider stepPitch{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Slider stepVelocity{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    alien::PianoRollEditor pianoRoll;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    ui::PaintedPanel sessionPanel;
    juce::Label sessionHelp, launchTimingLabel, controllerMappingLabel;
    juce::Label audioImportStatus;
    juce::ComboBox launchTiming, controllerTarget;
    ui::GlyphButton learnController;
    juce::TextButton clearController;
    std::array<ui::GlyphButton, 4> sceneButtons;
    std::array<alien::SessionClipButton, 16> clipButtons;
    ui::GlyphButton editSelectedClip, stopClip, importAudio, removeAudio, clearClip;
    alien::AudioClipWaveform audioWaveform;
    std::array<std::array<SessionClipContent, trackCount>, sceneCount> clips{};
    std::vector<std::shared_ptr<alien::SessionAudioClip>> retainedAudioAssets;
    std::size_t retainedAudioBytes{};
    juce::ThreadPool audioImportPool{1};
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> projectFileChooser;
    juce::File currentProjectFile;
    int selectedTrack{}, selectedScene{}, selectedStep{};
    int learningControllerAction{-1};
    bool loadingClip{}, projectBusy{};
    ui::PaintedPanel devicePanel;
    juce::Label deviceHelp, presetLabel, midiLabel;
    alien::CompactWorkspace workspace{tabs, devicePanel};
    TypingPianoRouter typingInput{*this, keyboardState};
};

class AlienStudioApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Alien Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override {
        splash = std::make_unique<alien::branding::SplashWindow>(getApplicationVersion());
        splash->onClick = [this] { finishStartup(); };
        splash->setStatus("Loading interface and audio engine");
        splash->setProgress(0.55f);
        splash->showCentred();
        startedAt = juce::Time::getMillisecondCounter();
        // Let the splash paint before the (synchronous) main window construction.
        juce::Timer::callAfterDelay(90, [this] { createMainWindow(); });
    }
    void shutdown() override { splash.reset(); window.reset(); }
private:
    static constexpr juce::uint32 minimumSplashMs = 1600;

    void createMainWindow() {
        if (window != nullptr) return;
        window = std::make_unique<Window>(getApplicationName());
        // Keep it hidden behind the splash. JUCE's Linux peer can map the native
        // window while it is being sized, so hide the peer explicitly as well.
        window->setVisible(false);
        if (auto* peer = window->getPeer()) peer->setVisible(false);
        if (splash != nullptr) { splash->setStatus("Ready"); splash->setProgress(1.0f); }
        const auto elapsed = juce::Time::getMillisecondCounter() - startedAt;
        const int remaining = elapsed >= minimumSplashMs ? 0 : static_cast<int>(minimumSplashMs - elapsed);
        juce::Timer::callAfterDelay(remaining, [this] { finishStartup(); });
    }

    void finishStartup() {
        if (window == nullptr || startupFinished) return;
        startupFinished = true;
        window->setVisible(true);
        window->toFront(true);
        if (splash != nullptr) {
            splash->onDismissed = [this] { splash.reset(); };
            splash->dismiss();
        }
    }

    class Window : public juce::DocumentWindow {
    public:
        explicit Window(juce::String name) : juce::DocumentWindow(name, alien::ui::colours::window,
            juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(960, 650, 1920, 1440);
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                centreWithSize(juce::jmin(getWidth(), display->userArea.getWidth() - 40),
                               juce::jmin(getHeight(), display->userArea.getHeight() - 40));
            else centreWithSize(getWidth(), getHeight());
           #if ! JUCE_WINDOWS
            // Windows takes the icon from the embedded ICO resource; elsewhere set it here.
            setIcon(alien::branding::artworkAtSize(256));
           #endif
            // Shown by the application once the startup splash finishes.
        }
        void activeWindowStatusChanged() override {
            if (!isActiveWindow())
                if (auto* content = dynamic_cast<MainComponent*>(getContentComponent())) content->releaseNotes();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
    std::unique_ptr<alien::branding::SplashWindow> splash;
    juce::uint32 startedAt{};
    bool startupFinished{};
};
START_JUCE_APPLICATION(AlienStudioApp)
