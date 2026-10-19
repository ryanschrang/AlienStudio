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
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

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

class AlienStudioLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    static const juce::Colour background;
    static const juce::Colour panel;
    static const juce::Colour control;
    static const juce::Colour text;
    static const juce::Colour muted;
    static const juce::Colour accent;

    AlienStudioLookAndFeel() {
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::Label::textColourId, text);
        setColour(juce::TextButton::buttonColourId, control);
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, background);
        setColour(juce::ComboBox::backgroundColourId, control);
        setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff344253));
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::arrowColourId, accent);
        setColour(juce::Slider::trackColourId, accent);
        setColour(juce::Slider::backgroundColourId, juce::Colour(0xff303b4b));
        setColour(juce::Slider::thumbColourId, juce::Colour(0xffe5f4f2));
        setColour(juce::Slider::textBoxTextColourId, text);
        setColour(juce::Slider::textBoxBackgroundColourId, control);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff344253));
        setColour(juce::TabbedComponent::backgroundColourId, panel);
        setColour(juce::TabbedComponent::outlineColourId, juce::Colour(0xff283342));
        setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colour(0xff283342));
        setColour(juce::TabbedButtonBar::frontOutlineColourId, accent);
        setColour(juce::TabbedButtonBar::tabTextColourId, muted);
        setColour(juce::TabbedButtonBar::frontTextColourId, text);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.22f));
        setColour(juce::PopupMenu::highlightedTextColourId, text);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
        const juce::Colour& backgroundColour, bool isMouseOverButton, bool isButtonDown) override {
        auto colour = button.findColour(button.getToggleState()
            ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        if (!colour.isOpaque()) colour = backgroundColour;
        if (isButtonDown) colour = colour.brighter(0.12f);
        else if (isMouseOverButton) colour = colour.brighter(0.07f);
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(colour);
        g.fillRoundedRectangle(bounds, 7.0f);
        if (button.hasKeyboardFocus(true)) {
            g.setColour(accent.withAlpha(0.9f));
            g.drawRoundedRectangle(bounds.reduced(1.0f), 6.0f, 1.3f);
        }
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
        int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override {
        const auto bounds = juce::Rectangle<float>(0.5f, 0.5f,
            static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId)
            .brighter(isButtonDown ? 0.08f : 0.0f));
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(box.findColour(juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(bounds, 6.0f, 1.0f);
        juce::Path arrow;
        const auto centreX = static_cast<float>(buttonX) + static_cast<float>(buttonW) * 0.5f;
        const auto centreY = static_cast<float>(buttonY) + static_cast<float>(buttonH) * 0.5f;
        arrow.addTriangle(centreX - 4.0f, centreY - 2.0f, centreX + 4.0f, centreY - 2.0f,
            centreX, centreY + 3.0f);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId));
        g.fillPath(arrow);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
        float sliderPos, float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
        juce::Slider& slider) override {
        juce::ignoreUnused(minSliderPos, maxSliderPos);
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar)
            return juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                minSliderPos, maxSliderPos, style, slider);
        auto track = juce::Rectangle<float>(static_cast<float>(x) + 2.0f,
            static_cast<float>(y) + static_cast<float>(height) * 0.5f - 2.0f,
            static_cast<float>(width) - 4.0f, 4.0f);
        g.setColour(slider.findColour(juce::Slider::backgroundColourId));
        g.fillRoundedRectangle(track, 2.0f);
        const auto filledWidth = juce::jlimit(0.0f, track.getWidth(), sliderPos - track.getX());
        g.setColour(slider.findColour(juce::Slider::trackColourId));
        g.fillRoundedRectangle(track.withWidth(filledWidth), 2.0f);
        if (style == juce::Slider::LinearHorizontal) {
            g.setColour(slider.findColour(juce::Slider::thumbColourId));
            g.fillEllipse(sliderPos - 6.0f, track.getCentreY() - 6.0f, 12.0f, 12.0f);
        }
    }
};

const juce::Colour AlienStudioLookAndFeel::background{0xff10151e};
const juce::Colour AlienStudioLookAndFeel::panel{0xff171f2b};
const juce::Colour AlienStudioLookAndFeel::control{0xff242e3d};
const juce::Colour AlienStudioLookAndFeel::text{0xffe7f0f2};
const juce::Colour AlienStudioLookAndFeel::muted{0xff95a8b5};
const juce::Colour AlienStudioLookAndFeel::accent{0xff63dfc2};

class SequencerStepButton final : public juce::ToggleButton {
public:
    explicit SequencerStepButton(const juce::String& label) : juce::ToggleButton(label) {}

    void setPlaybackPosition(bool isCurrent) {
        if (playbackPosition == isCurrent) return;
        playbackPosition = isCurrent;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        auto fill = getToggleState() ? AlienStudioLookAndFeel::accent
                                     : juce::Colour(0xff263241);
        if (isMouseOver) fill = fill.brighter(0.08f);
        if (isButtonDown) fill = fill.brighter(0.12f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(playbackPosition ? juce::Colour(0xffffcf70)
                                     : juce::Colour(0xff3b4a5c));
        g.drawRoundedRectangle(bounds, 6.0f, playbackPosition ? 2.0f : 1.0f);
        g.setColour(getToggleState() ? juce::Colour(0xff10201f)
                                     : AlienStudioLookAndFeel::text);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
        if (hasKeyboardFocus(true)) {
            g.setColour(AlienStudioLookAndFeel::accent);
            g.drawRoundedRectangle(bounds.reduced(2.0f), 4.0f, 1.0f);
        }
    }

private:
    bool playbackPosition{};
};

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener, private juce::ChangeListener {
public:
    MainComponent() {
        setLookAndFeel(&studioLookAndFeel);
        setSize(1120, 720);
        addAndMakeVisible(title);
        title.setText("ALIEN STUDIO", juce::dontSendNotification);
        title.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::text);
        title.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(brandSubtitle);
        brandSubtitle.setText("PATTERN STUDIO", juce::dontSendNotification);
        brandSubtitle.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        brandSubtitle.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::accent);
        brandSubtitle.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(play);
        play.setButtonText("Play");
        play.setName("Start or stop playback");
        play.setTooltip("Start or stop the current pattern");
        play.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff245a55));
        play.setColour(juce::TextButton::buttonOnColourId, AlienStudioLookAndFeel::accent);
        play.onClick = [this] { releaseNotes(); engine.playing.store(!engine.playing.load()); };
        addAndMakeVisible(tempo);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120);
        tempo.setTooltip("Playback tempo in beats per minute");
        tempo.onValueChange = [this] { engine.bpm.store(static_cast<int>(tempo.getValue())); };
        addAndMakeVisible(tempoLabel);
        tempoLabel.setText("BPM", juce::dontSendNotification);
        tempoLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        tempoLabel.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        tempoLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(saveProjectButton);
        saveProjectButton.setButtonText("Save");
        saveProjectButton.setName("Save Alien Studio session project");
        saveProjectButton.onClick = [this] { saveProject(); };
        addAndMakeVisible(portableProjectButton);
        portableProjectButton.setButtonText("Save portable");
        portableProjectButton.setName("Save project copy with collected audio");
        portableProjectButton.setTooltip("Save a project file and adjacent media folder; move both together");
        portableProjectButton.onClick = [this] { chooseProjectSavePath(true); };
        addAndMakeVisible(openProjectButton);
        openProjectButton.setButtonText("Open");
        openProjectButton.setName("Open Alien Studio session project");
        openProjectButton.onClick = [this] { chooseProjectToOpen(); };
        addAndMakeVisible(projectStatus);
        projectStatus.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        projectStatus.setJustificationType(juce::Justification::centredLeft);
        projectStatus.setText("Unsaved session", juce::dontSendNotification);
        for (int i = 0; i < 16; ++i) {
            auto* button = pads.add(new SequencerStepButton(juce::String(i + 1)));
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
        stepsPanel.addAndMakeVisible(selectedStepLabel);
        selectedStepLabel.setText("Edit step", juce::dontSendNotification);
        selectedStepLabel.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        stepsPanel.addAndMakeVisible(stepSelector);
        stepSelector.setName("Selected sequencer step");
        for (int i = 0; i < 16; ++i) stepSelector.addItem("Step " + juce::String(i + 1), i + 1);
        stepSelector.onChange = [this] { selectSequencerStep(stepSelector.getSelectedId() - 1); };
        stepsPanel.addAndMakeVisible(stepPitchLabel);
        stepPitchLabel.setText("Pitch", juce::dontSendNotification);
        stepPitchLabel.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        stepsPanel.addAndMakeVisible(stepPitch);
        stepPitch.setName("Selected step MIDI pitch");
        stepPitch.setTooltip("Set the selected step pitch from MIDI note 0 to 127");
        stepPitch.setRange(0, 127, 1);
        stepPitch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 26);
        stepPitch.onValueChange = [this] {
            const int pitch = static_cast<int>(stepPitch.getValue());
            engine.stepPitches[static_cast<std::size_t>(selectedStep)].store(pitch);
            stepPitchName.setText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 4),
                juce::dontSendNotification);
        };
        stepsPanel.addAndMakeVisible(stepPitchName);
        stepPitchName.setName("Selected step note name");
        stepPitchName.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::text);
        stepPitchName.setJustificationType(juce::Justification::centredLeft);
        stepsPanel.addAndMakeVisible(stepVelocityLabel);
        stepVelocityLabel.setText("Velocity", juce::dontSendNotification);
        stepVelocityLabel.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        stepsPanel.addAndMakeVisible(stepVelocity);
        stepVelocity.setName("Selected step velocity");
        stepVelocity.setTooltip("Set the selected step velocity from 1 to 127");
        stepVelocity.setRange(1, 127, 1);
        stepVelocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 26);
        stepVelocity.onValueChange = [this] {
            engine.stepVelocities[static_cast<std::size_t>(selectedStep)].store(
                static_cast<int>(stepVelocity.getValue()));
        };
        selectSequencerStep(0);
        tabs.addTab("16-step sequencer", AlienStudioLookAndFeel::panel, &stepsPanel, false);
        tabs.addTab("Piano roll", AlienStudioLookAndFeel::panel, &pianoRoll, false);
        tabs.addTab("Session", AlienStudioLookAndFeel::panel, &sessionPanel, false);
        tabs.addTab("Mixer", AlienStudioLookAndFeel::panel, &mixerPanel, false);
        tabs.getTabbedButtonBar().addChangeListener(this);
        addAndMakeVisible(workspace);
        devicePanel.addAndMakeVisible(deviceTitle);
        deviceTitle.setText("Device rack", juce::dontSendNotification);
        deviceTitle.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        deviceTitle.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::text);
        devicePanel.addAndMakeVisible(deviceHelp);
        deviceHelp.setText("Choose the live keyboard / step synth and MIDI input. Session instruments are in Mixer. Shift+Tab returns.",
                           juce::dontSendNotification);
        deviceHelp.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        devicePanel.addAndMakeVisible(midiLabel);
        midiLabel.setText("MIDI input", juce::dontSendNotification);
        midiLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        devicePanel.addAndMakeVisible(midiInput);
        stepsPanel.addAndMakeVisible(stepHelp);
        stepHelp.setText("16 steps  |  Sixteenth notes  |  A3 synth pattern", juce::dontSendNotification);
        stepHelp.setJustificationType(juce::Justification::centred);
        sessionPanel.addAndMakeVisible(sessionTitle);
        sessionTitle.setText("Session scenes", juce::dontSendNotification);
        sessionTitle.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        sessionTitle.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::text);
        sessionPanel.addAndMakeVisible(sessionHelp);
        sessionHelp.setText("Launch a scene to switch all tracks together. Click a clip to launch; right-click to edit or view audio.",
                            juce::dontSendNotification);
        sessionHelp.setColour(juce::Label::textColourId, AlienStudioLookAndFeel::muted);
        sessionPanel.addAndMakeVisible(launchTimingLabel);
        launchTimingLabel.setText("Launch at", juce::dontSendNotification);
        launchTimingLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        sessionPanel.addAndMakeVisible(launchTiming);
        launchTiming.setName("Session launch quantization");
        launchTiming.setTooltip("Choose whether queued clips and scenes launch on the next beat or bar");
        launchTiming.addItem("Beat", 1);
        launchTiming.addItem("Bar", 2);
        launchTiming.setSelectedId(2, juce::dontSendNotification);
        launchTiming.onChange = [this] {
            engine.launchQuantizationSteps.store(launchTiming.getSelectedId() == 1 ? 4 : 16);
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
        learnController.setName("Learn MIDI controller");
        learnController.setTooltip("Listen for the next incoming MIDI CC and assign it to the selected Session action");
        learnController.onClick = [this] {
            const int action = controllerTarget.getSelectedId() - 1;
            if (learningControllerAction == action) {
                engine.controllerMappings.cancelLearn();
                learningControllerAction = -1;
                learnController.setButtonText("Learn CC");
                updateControllerMappingLabel();
            } else {
                learningControllerAction = action;
                engine.controllerMappings.beginLearn(action);
                learnController.setButtonText("Cancel learn");
                controllerMappingLabel.setText("Move a MIDI CC control…", juce::dontSendNotification);
            }
        };
        sessionPanel.addAndMakeVisible(clearController);
        clearController.setButtonText("Clear");
        clearController.setName("Clear MIDI controller mapping");
        clearController.onClick = [this] {
            engine.controllerMappings.cancelLearn();
            learningControllerAction = -1;
            engine.controllerMappings.clearMapping(controllerTarget.getSelectedId() - 1);
            learnController.setButtonText("Learn CC");
            updateControllerMappingLabel();
        };
        sessionPanel.addAndMakeVisible(controllerMappingLabel);
        controllerMappingLabel.setName("MIDI controller assignment");
        controllerMappingLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        controllerMappingLabel.setJustificationType(juce::Justification::centredLeft);
        controllerMappingLabel.setText("Unmapped", juce::dontSendNotification);
        for (int scene = 0; scene < sceneCount; ++scene) {
            sessionPanel.addAndMakeVisible(sceneButtons[static_cast<std::size_t>(scene)]);
            auto& sceneButton = sceneButtons[static_cast<std::size_t>(scene)];
            sceneButton.setName("Launch scene " + juce::String(scene + 1));
            sceneButton.setButtonText("Launch scene " + juce::String(scene + 1));
            sceneButton.onClick = [this, scene] { launchScene(scene); };
            for (int track = 0; track < trackCount; ++track) {
                const auto index = cellIndex(track, scene);
                sessionPanel.addAndMakeVisible(clipButtons[index]);
                auto& button = clipButtons[index];
                button.setName("Launch track " + juce::String(track + 1) + " scene " + juce::String(scene + 1));
                button.onClick = [this, track, scene] { selectAndLaunchClip(track, scene); };
                button.onRightClick = [this, track, scene] { selectClipForEditing(track, scene); };
            }
        }
        sessionPanel.addAndMakeVisible(stopClip);
        stopClip.setButtonText("Stop all tracks");
        stopClip.setName("Stop all Session tracks at selected launch boundary");
        stopClip.onClick = [this] { stopAllSessionTracks(); };
        sessionPanel.addAndMakeVisible(editSelectedClip);
        editSelectedClip.setButtonText("Edit selected clip");
        editSelectedClip.setName("Edit selected Session clip");
        editSelectedClip.setTooltip("Open the selected MIDI clip in the piano roll or show its audio waveform");
        editSelectedClip.onClick = [this] {
            selectClipForEditing(selectedTrack, selectedScene);
        };
        addAndMakeVisible(importAudio);
        importAudio.setButtonText("Import WAV/MP3");
        importAudio.setName("Import audio file to selected Session clip");
        importAudio.setTooltip("Decode a WAV or MP3 off the audio thread and add it to the selected clip");
        importAudio.onClick = [this] { chooseAudioFile(); };
        sessionPanel.addAndMakeVisible(removeAudio);
        removeAudio.setButtonText("Remove audio");
        removeAudio.setName("Remove audio from selected Session clip");
        removeAudio.onClick = [this] { removeAudioFromSelectedClip(); };
        sessionPanel.addAndMakeVisible(audioImportStatus);
        audioImportStatus.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        audioImportStatus.setJustificationType(juce::Justification::centredLeft);
        sessionPanel.addAndMakeVisible(audioWaveform);
        pianoRoll.onChange = [this] {
            auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
            clip.pattern = pianoRoll.getPattern();
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
        devicePanel.addAndMakeVisible(presetLabel);
        presetLabel.setText("Live / step synth", juce::dontSendNotification);
        presetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb9ced8));
        devicePanel.addAndMakeVisible(preset);
        preset.addItem("Lead (saw)", 1);
        preset.addItem("Bass (square)", 2);
        preset.addItem("Pluck (sine)", 3);
        preset.setSelectedId(1);
        preset.onChange = [this] { releaseNotes(); engine.preset.store(preset.getSelectedId() - 1); };
        workspace.onViewChanged = [this](bool) { releaseNotes(); };
        addAndMakeVisible(keyboard);
        keyboard.clearKeyMappings();
        keyboard.setAvailableRange(0, 127);
        keyboard.setOctaveForMiddleC(4);
        keyboard.setLowestVisibleKey(48);
        keyboard.setColour(juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour(0xffdce7e8));
        keyboard.setColour(juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour(0xff202a38));
        keyboard.setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour(0xff536274));
        keyboard.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, AlienStudioLookAndFeel::accent.withAlpha(0.28f));
        keyboard.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, AlienStudioLookAndFeel::accent.withAlpha(0.60f));
        keyboard.setColour(juce::MidiKeyboardComponent::textLabelColourId, juce::Colour(0xff18222e));
        keyboard.setColour(juce::MidiKeyboardComponent::shadowColourId, juce::Colour(0x55000000));
        typingInput.octaveChanged = [this] { updateOctave(); };
        for (auto* b : {&octaveDown, &octaveUp}) {
            addAndMakeVisible(b);
        }
        octaveDown.setButtonText("Octave -"); octaveUp.setButtonText("Octave +");
        octaveDown.onClick = [this] { typingInput.typing.shift(-1); updateOctave(); };
        octaveUp.onClick = [this] { typingInput.typing.shift(1); updateOctave(); };
        addAndMakeVisible(help);
        updateOctave();
        keyboard.setVelocity(0.8f, true);
        keyboardState.addListener(this);
        const auto audioError = audio.initialiseWithDefaultDevices(0, 2);
        audio.addAudioCallback(&engine);
        startTimerHz(20);
        resized();
        if (audioError.isNotEmpty()) {
            // Wait until the window has finished constructing before showing the alert.
            juce::MessageManager::callAsync(
                [safeThis = juce::Component::SafePointer<MainComponent>(this), audioError] {
                    if (safeThis == nullptr) return;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Audio initialization failed",
                        "Alien Studio could not open the default audio output device.\n\n" + audioError,
                        "OK", safeThis.getComponent());
                });
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
        setLookAndFeel(nullptr);
    }
    void resized() override {
        const int right = getWidth() - 20;
        title.setBounds(20, 4, 166, 24);
        brandSubtitle.setBounds(21, 27, 160, 15);
        play.setBounds(194, 8, 82, 34);
        tempoLabel.setBounds(286, 8, 42, 34);
        tempo.setBounds(330, 8, 150, 34);
        saveProjectButton.setBounds(494, 8, 66, 34);
        openProjectButton.setBounds(568, 8, 66, 34);
        portableProjectButton.setBounds(642, 8, 112, 34);
        projectStatus.setBounds(762, 8, juce::jmax(100, right - 762), 34);
        workspace.setBounds(16, 54, getWidth() - 32, getHeight() - 228);
        const auto rackWidth = devicePanel.getWidth();
        deviceTitle.setBounds(24, 18, rackWidth - 48, 34);
        deviceHelp.setBounds(24, 54, rackWidth - 48, 28);
        presetLabel.setBounds(30, 108, 120, 30);
        preset.setBounds(150, 108, juce::jmin(260, rackWidth - 180), 32);
        midiLabel.setBounds(30, 168, 120, 30);
        midiInput.setBounds(150, 168, juce::jmax(300, rackWidth - 180), 32);
        sessionTitle.setBounds(24, 12, sessionPanel.getWidth() - 48, 28);
        sessionHelp.setBounds(24, 40, sessionPanel.getWidth() - 48, 22);
        const int labelWidth = 118;
        const int gridWidth = juce::jmax(280, sessionPanel.getWidth() - labelWidth - 44);
        const int trackWidth = gridWidth / trackCount;
        const int sceneHeight = 36;
        const int sceneGap = 3;
        const int gridTop = 68;
        for (int scene = 0; scene < sceneCount; ++scene) {
            sceneButtons[static_cast<std::size_t>(scene)].setBounds(24, gridTop + scene * (sceneHeight + sceneGap), labelWidth - 8, sceneHeight);
            for (int track = 0; track < trackCount; ++track)
                clipButtons[cellIndex(track, scene)].setBounds(labelWidth + 24 + track * trackWidth,
                    gridTop + scene * (sceneHeight + sceneGap), trackWidth - 8, sceneHeight);
        }
        const int actionY = gridTop + sceneCount * (sceneHeight + sceneGap) + 4;
        editSelectedClip.setBounds(24, actionY, 174, 30);
        stopClip.setBounds(206, actionY, 146, 30);
        launchTimingLabel.setBounds(360, actionY, 76, 30);
        launchTiming.setBounds(438, actionY, 94, 30);
        removeAudio.setBounds(544, actionY, 104, 30);
        audioImportStatus.setBounds(656, actionY, juce::jmax(100, sessionPanel.getWidth() - 680), 30);
        const int mappingY = actionY + 32;
        controllerTarget.setBounds(24, mappingY, 130, 30);
        learnController.setBounds(162, mappingY, 104, 30);
        clearController.setBounds(274, mappingY, 70, 30);
        controllerMappingLabel.setBounds(352, mappingY, juce::jmax(100, sessionPanel.getWidth() - 376), 30);
        const int waveformY = mappingY + 34;
        const int waveformHeight = juce::jlimit(36, 68, sessionPanel.getHeight() - waveformY - 10);
        audioWaveform.setBounds(24, waveformY, sessionPanel.getWidth() - 48, waveformHeight);
        octaveDown.setBounds(20, getHeight() - 154, 100, 30);
        octaveUp.setBounds(130, getHeight() - 154, 100, 30);
        importAudio.setBounds(245, getHeight() - 154, 150, 30);
        help.setBounds(410, getHeight() - 154, getWidth() - 430, 30);
        keyboard.setBounds(20, getHeight() - 110, getWidth() - 40, 90);
        const int width = juce::jmax(1, (getWidth() - 60) / 8);
        stepHelp.setBounds(20, 40, getWidth() - 80, 35);
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setBounds(30 + (i % 8) * width, 110 + (i / 8) * 75, juce::jmax(1, width - 8), 60);
        selectedStepLabel.setBounds(24, 270, 58, 30);
        stepSelector.setBounds(84, 270, 82, 30);
        stepPitchLabel.setBounds(180, 270, 42, 30);
        stepPitch.setBounds(222, 270, 220, 30);
        stepPitchName.setBounds(444, 270, 48, 30);
        stepVelocityLabel.setBounds(510, 270, 58, 30);
        stepVelocity.setBounds(568, 270, juce::jmax(150, getWidth() - 600), 30);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(AlienStudioLookAndFeel::background);
        auto header = juce::Rectangle<float>(12.0f, 5.0f, static_cast<float>(getWidth() - 24), 42.0f);
        g.setColour(AlienStudioLookAndFeel::panel);
        g.fillRoundedRectangle(header, 10.0f);
        g.setColour(juce::Colour(0xff273342));
        g.drawRoundedRectangle(header, 10.0f, 1.0f);
        auto footer = juce::Rectangle<float>(12.0f, static_cast<float>(getHeight() - 174),
            static_cast<float>(getWidth() - 24), 158.0f);
        g.setColour(AlienStudioLookAndFeel::panel);
        g.fillRoundedRectangle(footer, 10.0f);
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
    void selectSequencerStep(int step) {
        selectedStep = juce::jlimit(0, 15, step);
        stepSelector.setSelectedId(selectedStep + 1, juce::dontSendNotification);
        const auto index = static_cast<std::size_t>(selectedStep);
        const int pitch = engine.stepPitches[index].load();
        stepPitch.setValue(pitch, juce::dontSendNotification);
        stepPitchName.setText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 4),
            juce::dontSendNotification);
        stepVelocity.setValue(engine.stepVelocities[index].load(), juce::dontSendNotification);
    }

    void updateOctave() {
        help.setText("Play notes (outside text fields): A W S E D F T G Y H U J | Z/X octave | C"
                     + juce::String(typingInput.typing.octave()), juce::dontSendNotification);
        keyboard.setLowestVisibleKey(12 * (typingInput.typing.octave() + 1));
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const int newMode = tabs.getCurrentTabIndex() == 0 ? 0 : 1;
        if (engine.patternMode.exchange(newMode) != newMode) releaseNotes();
        else pianoRoll.releaseAudition();
    }
    void timerCallback() override {
        const bool isPlaying = engine.playing.load();
        if (play.getToggleState() != isPlaying)
            play.setToggleState(isPlaying, juce::dontSendNotification);
        const juce::String transportText = isPlaying ? "Stop" : "Play";
        if (play.getButtonText() != transportText) play.setButtonText(transportText);
        const int currentStep = engine.patternMode.load() == 0 && isPlaying
            ? engine.currentStep.load() : -1;
        for (int i = 0; i < pads.size(); ++i)
            pads[i]->setPlaybackPosition(i == currentStep);
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
        repaint();
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
        projectStatus.setText(portable ? "Collecting audio…" : "Saving project…", juce::dontSendNotification);
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
        projectStatus.setText("Opening project…", juce::dontSendNotification);
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
        audioImportStatus.setText("Loading audio…", juce::dontSendNotification);
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
    void updateAudioClipEditor() {
        const auto& clip = clips[static_cast<std::size_t>(selectedScene)][static_cast<std::size_t>(selectedTrack)];
        audioWaveform.setClip(clip.audio, clip.audioFileName);
        removeAudio.setEnabled(clip.audio != nullptr);
        if (clip.audio == nullptr && audioImportStatus.getText().isEmpty())
            audioImportStatus.setText("Selected clip: Track " + juce::String(selectedTrack + 1)
                + ", Scene " + juce::String(selectedScene + 1), juce::dontSendNotification);
    }
    void updateControllerMappingLabel() {
        if (learningControllerAction == controllerTarget.getSelectedId() - 1) {
            controllerMappingLabel.setText("Move a MIDI CC control…", juce::dontSendNotification);
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
            sceneButton.setColour(juce::TextButton::buttonColourId,
                scenePlaying[static_cast<std::size_t>(scene)] ? juce::Colour(0xff245a55)
                : scene == selectedScene ? juce::Colour(0xff303b4b) : AlienStudioLookAndFeel::control);
            sceneButton.setButtonText(
                (scene == selectedScene ? "Selected | " : "") + juce::String("Launch scene ") + juce::String(scene + 1));
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
                button.setButtonText(text);
                button.setColour(juce::TextButton::buttonColourId,
                    scene == active ? juce::Colour(0xff245a55)
                    : scene == requested ? juce::Colour(0xff67522e)
                    : track == selectedTrack && scene == selectedScene ? juce::Colour(0xff303b4b)
                    : AlienStudioLookAndFeel::control);
            }
        }
        stopClip.setButtonText("Stop all tracks");
    }
    AlienStudioLookAndFeel studioLookAndFeel;
    ToneEngine engine;
    alien::MidiInputPanel midiInput{engine.midi};
    alien::TrackMixerPanel mixerPanel{engine.tracks};
    juce::AudioDeviceManager audio;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard{keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard};
    juce::TextButton octaveDown, octaveUp;
    juce::Label help;
    juce::ComboBox preset;
    juce::Label title;
    juce::Label brandSubtitle, tempoLabel;
    juce::TextButton play;
    juce::TextButton saveProjectButton, openProjectButton, portableProjectButton;
    juce::Label projectStatus;
    juce::Slider tempo{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::OwnedArray<SequencerStepButton> pads;
    juce::Component stepsPanel;
    juce::Label stepHelp;
    juce::Label selectedStepLabel, stepPitchLabel, stepPitchName, stepVelocityLabel;
    juce::ComboBox stepSelector;
    juce::Slider stepPitch{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Slider stepVelocity{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    alien::PianoRollEditor pianoRoll;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    juce::Component sessionPanel;
    juce::Label sessionTitle, sessionHelp, launchTimingLabel, controllerMappingLabel;
    juce::Label audioImportStatus;
    juce::ComboBox launchTiming, controllerTarget;
    juce::TextButton learnController, clearController;
    std::array<juce::TextButton, 4> sceneButtons;
    std::array<alien::SessionClipButton, 16> clipButtons;
    juce::TextButton editSelectedClip, stopClip, importAudio, removeAudio;
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
    juce::Component devicePanel;
    juce::Label deviceTitle, deviceHelp, presetLabel, midiLabel;
    alien::CompactWorkspace workspace{tabs, devicePanel};
    TypingPianoRouter typingInput{*this, keyboardState};
};

class AlienStudioApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Alien Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override {
        window = std::make_unique<Window>(getApplicationName());
    }
    void shutdown() override { window.reset(); }
private:
    class Window : public juce::DocumentWindow {
    public:
        explicit Window(juce::String name) : DocumentWindow(name, juce::Colours::black,
            juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(960, 650, 1920, 1440);
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                centreWithSize(juce::jmin(getWidth(), display->userArea.getWidth() - 40),
                               juce::jmin(getHeight(), display->userArea.getHeight() - 40));
            else centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void activeWindowStatusChanged() override {
            if (!isActiveWindow())
                if (auto* content = dynamic_cast<MainComponent*>(getContentComponent())) content->releaseNotes();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(AlienStudioApp)
