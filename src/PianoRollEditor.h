#pragma once

#include <optional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PianoRollSequence.h"
#include "PianoRollProcessing.h"
#include "PianoRollClipboard.h"
#include "PianoRollGhostNotes.h"
#include "PianoRollChordStamp.h"
#include "PianoRollProgression.h"
#include "PianoRollScale.h"
#include <cmath>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace alien {

// Message-thread editor. Publishing snapshots and playing notes belong to the
// owner; drawing and pointer gestures never access the audio engine directly.
class PianoRollEditor final : public juce::Component, private juce::ScrollBar::Listener {
public:
    PianoRollEditor() {
        setName("Piano roll note editor");
        setTitle("Piano roll");
        setDescription("Four bar MIDI note grid with a piano keyboard and velocity lane");
        setHelpText("Left-click empty space to draw a note; right-click a note to erase it. "
                    "New notes reuse the last edited note length and velocity. Drag to move; drag its right edge to resize. "
                    "Ctrl-click toggles note selection; Shift-drag empty space to select a group. "
                    "Delete erases selected notes. Arrow keys move selected notes; "
                    "Shift Left or Right changes their length; Shift Up or Down transposes an octave. "
                    "The Edit menu transposes by semitone or octave. Ctrl A selects all, Ctrl Z undoes, "
                    "and Ctrl Y redoes. Ctrl C copies, Ctrl V pastes at the amber cursor, and Ctrl D duplicates. "
                    "Click the ruler to place the amber edit cursor; Edit > Split selected notes at cursor cuts crossing notes there. "
                    "Edit > Ghost notes shows other clips' notes, dimmed and read-only, behind this clip; "
                    "Edit them with this clip lets you edit those clips' notes too. "
                    "Alt C opens chord stamps; Alt L opens scale guides; Alt I inverts, Alt V reverses, and Alt A arpeggiates. Tab moves between controls.");
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);

        addAndMakeVisible(snapLabel);
        snapLabel.setText("Snap", juce::dontSendNotification);
        snapLabel.attachToComponent(&snap, true);
        addAndMakeVisible(snap);
        snap.setName("Piano roll grid snap");
        snap.setTooltip("Snap note starts and ends to sixteenth, eighth or quarter notes");
        snap.addItem("1/16", 1);
        snap.addItem("1/8", 2);
        snap.addItem("1/4", 4);
        snap.setSelectedId(1, juce::dontSendNotification);
        snap.onChange = [this] { repaint(); };

        addAndMakeVisible(velocityLabel);
        velocityLabel.setText("Velocity", juce::dontSendNotification);
        velocityLabel.attachToComponent(&velocity, true);
        addAndMakeVisible(velocity);
        velocity.setName("Selected note velocity");
        velocity.setTooltip("Set the velocity of every selected note, from 1 to 127");
        velocity.setSliderStyle(juce::Slider::LinearHorizontal);
        velocity.setWantsKeyboardFocus(true);
        velocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 24);
        velocity.setRange(1, 127, 1);
        velocity.setValue(102, juce::dontSendNotification);
        velocity.setEnabled(false);
        velocity.onValueChange = [this] {
            const bool singleEdit = !editOpen;
            if (singleEdit) beginEdit();
            setSelectedVelocity(static_cast<float>(velocity.getValue() / 127.0));
            if (singleEdit) endEdit();
        };
        velocity.onDragStart = [this] { beginEdit(); };
        velocity.onDragEnd = [this] { endEdit(); };

        addAndMakeVisible(probabilityLabel);
        probabilityLabel.setText("Chance", juce::dontSendNotification);
        probabilityLabel.attachToComponent(&probability, true);
        addAndMakeVisible(probability);
        probability.setName("Selected note probability");
        probability.setTooltip("Set the playback probability of every selected note, from 0 to 100 percent");
        probability.setSliderStyle(juce::Slider::LinearHorizontal);
        probability.setWantsKeyboardFocus(true);
        probability.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 24);
        probability.setRange(0, 100, 1);
        probability.setValue(100, juce::dontSendNotification);
        probability.setEnabled(false);
        probability.onValueChange = [this] {
            const bool singleEdit = !editOpen;
            if (singleEdit) beginEdit();
            setSelectedProbability(static_cast<float>(probability.getValue() / 100.0));
            if (singleEdit) endEdit();
        };
        probability.onDragStart = [this] { beginEdit(); };
        probability.onDragEnd = [this] { endEdit(); };

        addAndMakeVisible(clearButton);
        clearButton.setButtonText("Clear");
        clearButton.setName("Clear piano roll notes");
        clearButton.setTooltip("Remove all notes from the piano roll");
        clearButton.onClick = [this] { clearAllNotes(); };
        addAndMakeVisible(exampleButton);
        exampleButton.setButtonText("Load example");
        exampleButton.setName("Load example piano roll melody");
        exampleButton.setTooltip("Add an original example to an empty loop; clear existing notes first");
        exampleButton.onClick = [this] { loadExample(); };
        configureButton(editActionsButton, "Edit", "Piano roll edit actions");
        editActionsButton.setTooltip("Select all, Copy, Paste, Duplicate, Transpose, Split selected notes at the amber cursor, and Ghost notes");
        editActionsButton.onClick = [this] { showEditMenu(); };

        addAndMakeVisible(tools);
        tools.setName("Piano roll processing tool");
        tools.addItem("Strum", 1);
        tools.addItem("Chop", 2);
        tools.addItem("Randomize", 3);
        tools.addItem("Quantize", 4);
        tools.addItem("Chord stamp", 5);
        tools.addItem("Scale", 6);
        tools.addItem("Invert up", 7);
        tools.addItem("Invert down", 8);
        tools.addItem("Reverse", 9);
        tools.addItem("Arpeggiate", 10);
        tools.addItem("Chord progression", 11);
        tools.setSelectedId(1, juce::dontSendNotification);
        tools.setTooltip("Choose a note processing tool, then open its settings");
        tools.onChange = [this] {
            if (!panelOpen) return;
            const auto tool = tools.getSelectedId();
            if (tool >= 7 && tool <= 9) { closeToolPanel(); runImmediateTool(tool); }
            else { configureToolPanel(); resized(); }
        };
        addChildComponent(progressionMode);
        progressionMode.setName("Progression scale");
        progressionMode.addItem("Major", 1);
        progressionMode.addItem("Natural minor", 2);
        progressionMode.setSelectedId(1, juce::dontSendNotification);
        progressionMode.setTooltip("Diatonic triads in the chosen scale; root MIDI note sets key and register");
        for (std::size_t i = 0; i < progressionDegrees.size(); ++i) {
            auto& degree = progressionDegrees[i];
            addChildComponent(degree);
            degree.setName("Progression chord " + juce::String(static_cast<int>(i + 1)));
            for (int d = 1; d <= 7; ++d) degree.addItem("Degree " + juce::String(d), d);
            degree.setSelectedId(progressionSettings.degrees[i], juce::dontSendNotification);
            degree.setTooltip("Choose scale degree for chord " + juce::String(static_cast<int>(i + 1)));
        }
        configureButton(toolButton, "Edit...", "Open piano roll processing settings");
        toolButton.onClick = [this] {
            const auto tool = tools.getSelectedId();
            if (tool >= 7 && tool <= 9) runImmediateTool(tool); else openToolPanel(tool);
        };
        configureButton(undoButton, "Undo", "Undo piano roll edit");
        undoButton.setTooltip("Undo the last piano roll edit (Ctrl Z)");
        undoButton.onClick = [this] { undo(); };
        configureButton(redoButton, "Redo", "Redo piano roll edit");
        redoButton.setTooltip("Redo a piano roll edit (Ctrl Y)");
        redoButton.onClick = [this] { redo(); };

        addChildComponent(panelTitle);
        addChildComponent(panelHelp);
        panelTitle.setName("Piano roll processing settings title");
        panelHelp.setName("Piano roll processing settings help");
        panelHelp.setText("240 ticks = one 1/16 note. Changes apply to selected notes only.", juce::dontSendNotification);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            addChildComponent(parameters[i]);
            addChildComponent(parameterLabels[i]);
            parameters[i].setSliderStyle(juce::Slider::LinearHorizontal);
            parameters[i].setWantsKeyboardFocus(true);
            parameters[i].setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 25);
            parameterLabels[i].setJustificationType(juce::Justification::centredRight);
            parameterLabels[i].setFont(juce::FontOptions(12.0f));
        }
        addChildComponent(seed);
        addChildComponent(seedLabel);
        seed.setName("Processing random seed");
        seed.setTooltip("The same seed and notes reproduce the same randomized result");
        seed.setSliderStyle(juce::Slider::IncDecButtons);
        seed.setWantsKeyboardFocus(true);
        seed.setTextBoxStyle(juce::Slider::TextBoxRight, false, 125, 25);
        seed.setRange(1, 999999999, 1);
        seed.onValueChange = [this] { refreshChordPreview(); };
        seedLabel.setText("Random seed", juce::dontSendNotification);
        seedLabel.setJustificationType(juce::Justification::centredRight);
        addChildComponent(descending);
        descending.setButtonText("High to low");
        descending.setName("Strum from high pitch to low pitch");
        addChildComponent(chordTypeLabel);
        addChildComponent(chordType);
        chordTypeLabel.setText("Chord", juce::dontSendNotification);
        chordTypeLabel.setJustificationType(juce::Justification::centredRight);
        chordType.setName("Chord stamp type");
        for (const auto& item : std::initializer_list<std::pair<const char*, int>> {
                 {"Major", 1}, {"Minor", 2}, {"Diminished", 3}, {"Augmented", 4},
                 {"Suspended 2", 5}, {"Suspended 4", 6}, {"Dominant 7", 7},
                 {"Major 7", 8}, {"Minor 7", 9} }) chordType.addItem(item.first, item.second);
        chordType.setSelectedId(1, juce::dontSendNotification);
        chordType.onChange = [this] { updateChordInversionRange(); refreshChordPreview(); };
        addChildComponent(previewChordButton);
        previewChordButton.setButtonText("Preview");
        previewChordButton.setName("Preview chord stamp");
        previewChordButton.setTooltip("Toggle to audition the chord without adding notes");
        previewChordButton.onClick = [this] {
            if (previewChordButton.getToggleState()) startChordPreview();
            else releaseChordPreview(false);
        };
        addChildComponent(scaleRootLabel);
        addChildComponent(scaleRoot);
        scaleRootLabel.setText("Root", juce::dontSendNotification);
        scaleRootLabel.setJustificationType(juce::Justification::centredRight);
        scaleRoot.setName("Scale root note");
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
            scaleRoot.addItem(juce::String(scalePitchClassName(pitchClass)), pitchClass + 1);
        scaleRoot.setSelectedId(1, juce::dontSendNotification);
        addChildComponent(scaleTypeLabel);
        addChildComponent(scaleType);
        scaleTypeLabel.setText("Scale", juce::dontSendNotification);
        scaleTypeLabel.setJustificationType(juce::Justification::centredRight);
        scaleType.setName("Scale type");
        for (const auto& item : std::initializer_list<std::pair<const char*, int>> {
                 {"Major", 1}, {"Natural minor", 2}, {"Harmonic minor", 3}, {"Melodic minor", 4},
                 {"Dorian", 5}, {"Mixolydian", 6}, {"Major pentatonic", 7},
                 {"Minor pentatonic", 8}, {"Blues", 9}, {"Chromatic", 10} })
            scaleType.addItem(item.first, item.second);
        scaleType.setSelectedId(1, juce::dontSendNotification);
        addChildComponent(scaleHighlight);
        scaleHighlight.setButtonText("Highlight scale");
        scaleHighlight.setName("Highlight selected scale");
        scaleHighlight.setToggleState(false, juce::dontSendNotification);
        addChildComponent(scaleSnap);
        scaleSnap.setButtonText("Snap note pitches");
        scaleSnap.setName("Snap piano roll notes to scale");
        for (auto* control : std::initializer_list<juce::Component*> { &scaleRoot, &scaleType, &scaleHighlight, &scaleSnap })
            control->setWantsKeyboardFocus(true);
        scaleRoot.onChange = [this] { updateScaleSettings(); };
        scaleType.onChange = [this] { updateScaleSettings(); };
        scaleHighlight.onClick = [this] { updateScaleSettings(); };
        scaleSnap.onClick = [this] { updateScaleSettings(); };
        addChildComponent(applyButton);
        applyButton.setButtonText("Apply");
        applyButton.setName("Apply piano roll processing");
        applyButton.onClick = [this] { applyTool(); };
        addChildComponent(cancelButton);
        cancelButton.setButtonText("Cancel");
        cancelButton.setName("Cancel piano roll processing");
        cancelButton.onClick = [this] { closeToolPanel(); };
        for (auto& parameter : parameters)
            parameter.onValueChange = [this] { refreshChordPreview(); };
        history.reserve(historyLimit);

        addAndMakeVisible(pitchScroll);
        pitchScroll.setName("Piano roll pitch range");
        pitchScroll.setTitle("Piano roll pitch range");
        pitchScroll.setRangeLimits(0, highestPitch - lowestPitch + 1);
        pitchScroll.setSingleStepSize(1);
        pitchScroll.addListener(this);
        updatePitchScroll();

        for (auto* label : { &snapLabel, &velocityLabel, &probabilityLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xff9ea3ab));
        velocity.setColour(juce::Slider::thumbColourId, juce::Colour(0xfff2f4f6));
        velocity.setColour(juce::Slider::trackColourId, juce::Colour(0xff4fe0b6));
        pitchScroll.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff31343b));
        updateSelection();
    }

    ~PianoRollEditor() override {
        releaseAudition();
        pitchScroll.removeListener(this);
    }

    std::function<void()> onChange;
    // Called once per finished edit gesture (and after this editor's own
    // undo/redo), so the owner can record a project-wide undo step.
    std::function<void()> onEditCommitted;
    // Positive velocity starts a preview; zero releases that same pitch.
    std::function<void(int, float)> onAudition;
    // The user picked another ghost-note source; the owner answers with setGhostClips.
    std::function<void(GhostNoteSource)> onGhostSourceChanged;

    // Ghost notes are drawn only: no selection, hit-testing, history or onChange.
    GhostNoteSource getGhostSource() const noexcept { return ghostSource; }
    void setGhostSource(GhostNoteSource source) {
        if (ghostSource == source) return;
        ghostSource = source;
        if (source == GhostNoteSource::off) ghostClips.clear();
        if (onGhostSourceChanged) onGhostSourceChanged(source);
        repaint();
    }
    std::span<const PianoRollGhostClip> getGhostClips() const noexcept { return ghostClips; }
    void setGhostClips(std::vector<PianoRollGhostClip> clips) {
        ghostClips = ghostSource == GhostNoteSource::off ? std::vector<PianoRollGhostClip> {} : std::move(clips);
        repaint();
    }
    // Multi-clip editing: the owner merges the shown clips into the pattern it gives setPattern and splits each
    // edit back into them (PianoRollMultiClip.h). The editor only keeps the switch, colours notes by clip and says
    // how many clips it is editing.
    std::function<void(bool)> onMultiClipChanged;
    bool isMultiClipEditing() const noexcept { return multiClip; }
    void setMultiClipEditing(bool on) {
        if (multiClip == on) return;
        multiClip = on;
        if (onMultiClipChanged) onMultiClipChanged(on);
        repaint();
    }
    // The colour of a note that belongs to another clip (nullopt: the opened clip's own colour).
    std::function<std::optional<juce::Colour>(std::uint32_t)> noteTint;

    // The piano keyboard (PROD-06). Keys are named with their octave (C4 is MIDI 60): every key, or only the Cs.
    enum class KeyLabels { cNotes, all };
    KeyLabels getKeyLabels() const noexcept { return keyLabels; }
    void setKeyLabels(KeyLabels labels) { if (keyLabels != labels) { keyLabels = labels; repaint(); } }
    // What a key shows: the note being auditioned, a note sounding under the playhead, or a selected note, each in
    // its note's colour (another clip's colour when editing several), or nothing.
    enum class KeyState { none, selected, sounding, auditioned };
    struct KeyLight { KeyState state{KeyState::none}; juce::Colour colour; };
    KeyLight keyLight(int pitch) const {
        if (pitch < 0 || pitch > 127) return {};
        if (pitch == auditionPitch) return {KeyState::auditioned, juce::Colour(0xff4fe0b6)};
        KeyLight light;
        const bool playing = std::isfinite(playhead) && playhead >= 0 && playhead < pattern.loopSteps;
        for (std::size_t i = 0; i < pattern.count; ++i) {
            const auto& note = pattern.notes[i];
            if (note.pitch != pitch) continue;
            const auto colour = noteColour(note.id);
            if (playing && playhead >= note.startStep && playhead < note.startStep + note.lengthSteps)
                return {KeyState::sounding, colour.brighter(0.45f)};
            if (light.state == KeyState::none && selection.contains(note.id)) light = {KeyState::selected, colour};
        }
        return light;
    }
    void setEditingClipCount(int clips) { if (editingClips != clips) { editingClips = clips; updateSelection(); } }
    int editingClipCount() const noexcept { return editingClips; }
    std::size_t ghostNoteCount() const {
        return forEachGhostNote(ghostClips, pattern.loopSteps, [](const PianoRollGhostNote&) {});
    }

    const PianoRollPattern& getPattern() const noexcept { return pattern; }

    std::span<const std::uint32_t> getSelectedIds() const noexcept { return { selection.ids.data(), selection.count }; }
    void setSelection(std::span<const std::uint32_t> ids) {
        selection.clear();
        for (const auto id : ids) if (pattern.find(id) != nullptr) selection.add(id);
        activeId = selection.count != 0 ? selection.ids[0] : 0;
        updateSelection();
    }
    bool canUndo() const noexcept { return historyPosition != 0; }
    // Shared by the Edit menu and octave keyboard command; one history entry.
    void transposeSelectedNotes(int semitones) {
        if (locked) return;
        prepareClipboardAction();
        beginEdit();
        const auto result = processTranspose(pattern, selection, semitones);
        selection = result.selection;
        if (result.changed()) {
            changed();
            if (const auto* moved = pattern.find(activeId)) {
                if (moved->pitch < lowestVisiblePitch) setLowestVisible(moved->pitch);
                if (moved->pitch > topPitch()) setLowestVisible(moved->pitch - visiblePitches + 1);
            }
        }
        endEdit();
        if (!result.changed()) {
            statusMessage = selection.count == 0 ? "Select notes before transposing."
                : result.status == ProcessingStatus::invalidInput
                    ? "No changes: transposition would exceed MIDI pitches 0-127."
                    : "No changes: the selected pitches are unchanged.";
            updateSelection();
        }
    }
    bool canRedo() const noexcept { return historyPosition < history.size(); }
    void splitSelectedNotes() {
        if (locked) return;
        prepareClipboardAction();
        beginEdit();
        const auto result = processSplit(pattern, selection, insertionStep);
        selection = result.selection;
        if (result.changed()) changed();
        endEdit();
        if (!result.changed()) {
            statusMessage = result.status == ProcessingStatus::capacityExceeded
                ? "No changes: splitting these notes would exceed the note limit. Select fewer notes or delete notes first."
                : selection.count == 0 ? "Select notes before splitting."
                : "No changes: place the amber cursor inside a selected note using the ruler.";
            updateSelection();
        }
    }
    bool isProcessingPanelOpen() const noexcept { return panelOpen; }
    double getInsertionStep() const noexcept { return insertionStep; }
    void setInsertionStep(double step) {
        if (!std::isfinite(step)) return;
        insertionStep = PianoRollPattern::fromTicks(PianoRollPattern::toTicks(
            juce::jlimit(0.0, static_cast<double>(pattern.loopSteps), step)));
        statusMessage.clear();
        repaint();
    }
    void undo() {
        endEdit();
        if (!canUndo()) return;
        restore(history[--historyPosition].before);
        if (onEditCommitted) onEditCommitted();
    }
    void redo() {
        endEdit();
        if (!canRedo()) return;
        restore(history[historyPosition++].after);
        if (onEditCommitted) onEditCommitted();
    }

    void setPattern(const PianoRollPattern& replacement) {
        if (!replacement.valid()) return;
        releaseAudition();
        pattern = replacement;
        selection.clear();
        activeId = 0;
        insertionStep = 0.0;
        drag = Drag::none;
        editOpen = false;
        history.clear();
        historyPosition = 0;
        changed();
    }

    // Removes every note as one undoable edit (the toolbar Clear button and
    // the Session "Clear clip" action share this).
    void clearAllNotes() {
        if (pattern.count == 0) return;
        releaseAudition();
        beginEdit();
        pattern.clear();
        selection.clear();
        activeId = 0;
        insertionStep = 0.0;
        drag = Drag::none;
        changed();
        endEdit();
    }

    void setPlayheadStep(double step) {
        if (playhead == step) return;
        playhead = step;
        repaint();
    }

    // Read-only while a take is being recorded into the clip that is open, so an
    // edit cannot collide with the notes being captured.
    void setLocked(bool lock) {
        if (locked == lock) return;
        locked = lock;
        if (lock) {
            releaseAudition();
            endEdit();
            if (hasKeyboardFocus(true)) giveAwayKeyboardFocus();
        }
        setInterceptsMouseClicks(!lock, !lock);
        setAlpha(lock ? 0.8f : 1.0f);
    }
    bool isLocked() const noexcept { return locked; }

    void releaseAudition() {
        releasePreviewNote();
        releaseChordPreview(true);
        if (drag == Drag::audition) drag = Drag::none;
    }

    void resized() override {
        snap.setBounds(184, 8, 76, 28);
        snapLabel.setBounds(146, 8, 34, 28);
        editActionsButton.setBounds(104, 8, 40, 28);
        velocity.setBounds(312, 8, 78, 28);
        velocityLabel.setBounds(264, 8, 44, 28);
        probability.setBounds(440, 8, 96, 28);
        probabilityLabel.setBounds(394, 8, 42, 28);
        clearButton.setBounds(540, 8, 52, 28);
        exampleButton.setBounds(596, 8, 88, 28);
        tools.setBounds(688, 8, 90, 28);
        toolButton.setBounds(782, 8, 47, 28);
        undoButton.setBounds(833, 8, 43, 28);
        redoButton.setBounds(880, 8, 43, 28);
        if (getWidth() < 980) {
            snapLabel.setBounds(140, 8, 28, 28);
            snap.setBounds(172, 8, 44, 28);
            int right = getWidth() - 8;
            redoButton.setBounds(right - 43, 8, 43, 28); right -= 50;
            undoButton.setBounds(right - 43, 8, 43, 28); right -= 50;
            toolButton.setBounds(right - 47, 8, 47, 28); right -= 55;
            tools.setBounds(right - 90, 8, 90, 28); right -= 98;
            exampleButton.setBounds(right - 93, 8, 93, 28); right -= 101;
            clearButton.setBounds(right - 52, 8, 52, 28);
            probability.setBounds(clearButton.getX() - 84, 8, 80, 28);
            probabilityLabel.setBounds(probability.getX() - 44, 8, 40, 28);
            velocity.setBounds(probabilityLabel.getX() - 104, 8, 100, 28);
            velocityLabel.setBounds(velocity.getX() - 48, 8, 44, 28);
        }
        panelTitle.setBounds(20, 48, getWidth() - 40, 24);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const int x = 16 + static_cast<int>(i) * ((getWidth() - 32) / 3);
            const int width = (getWidth() - 32) / 3;
            parameterLabels[i].setBounds(x, 78, 106, 26);
            parameters[i].setBounds(x + 112, 78, width - 120, 26);
        }
        if (tools.getSelectedId() == 5) {
            chordTypeLabel.setBounds(16, 113, 72, 26);
            chordType.setBounds(94, 113, 180, 26);
            seedLabel.setBounds(282, 113, 72, 26);
            seed.setBounds(360, 113, 110, 26);
            previewChordButton.setBounds(480, 113, 88, 28);
        } else if (tools.getSelectedId() == 11) {
            const int available = getWidth() - 220;
            const int modeWidth = juce::jmin(140, available / 4);
            progressionMode.setBounds(16, 113, modeWidth, 26);
            const int degreeWidth = (available - modeWidth - 24) / 4;
            for (std::size_t i = 0; i < progressionDegrees.size(); ++i)
                progressionDegrees[i].setBounds(24 + modeWidth + static_cast<int>(i) * degreeWidth, 113, degreeWidth - 6, 26);
        } else if (tools.getSelectedId() == 6) {
            scaleRootLabel.setBounds(16, 78, 54, 26);
            scaleRoot.setBounds(76, 78, 92, 26);
            scaleTypeLabel.setBounds(178, 78, 54, 26);
            scaleType.setBounds(238, 78, 170, 26);
            scaleHighlight.setBounds(424, 78, 140, 26);
            scaleSnap.setBounds(572, 78, 160, 26);
        } else {
            seedLabel.setBounds(16, 113, 106, 26);
            seed.setBounds(128, 113, 190, 26);
            descending.setBounds(336, 113, 150, 26);
        }
        applyButton.setBounds(getWidth() - 184, 113, 78, 28);
        cancelButton.setBounds(getWidth() - 98, 113, 78, 28);
        panelHelp.setBounds(20, 145, getWidth() - 40, 22);
        const auto a = areas();
        pitchScroll.setBounds(a.grid.getRight() + 4, a.grid.getY(), 12, a.grid.getHeight());
    }

    void paint(juce::Graphics& g) override {
        const auto a = areas();
        g.fillAll(juce::Colour(0xff16171b));
        g.setColour(juce::Colour(0xff4fe0b6));
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText("PIANO ROLL", 12, 7, 88, 29, juce::Justification::centredLeft);

        if (panelOpen) {
            g.setColour(juce::Colour(0xff1d1f24));
            g.fillRoundedRectangle(10.0f, 44.0f, static_cast<float>(getWidth() - 20), 128.0f, 5.0f);
            g.setColour(juce::Colour(0xff363940));
            g.drawRoundedRectangle(10.0f, 44.0f, static_cast<float>(getWidth() - 20), 128.0f, 5.0f, 1.0f);
        }

        g.setColour(juce::Colour(0xff1d1f24));
        g.fillRect(a.ruler);
        g.setFont(juce::FontOptions(10.5f));
        const auto rowHeight = rowSize(a);
        for (int row = 0; row < visiblePitches; ++row) {
            const auto pitch = topPitch() - row;
            const auto y = a.grid.getY() + row * rowHeight;
            const juce::Rectangle<float> rowBounds(static_cast<float>(a.grid.getX()), y,
                                                  static_cast<float>(a.grid.getWidth()), rowHeight);
            const bool scaleTone = scaleSettings.highlight && isPitchInScale(pitch, scaleSettings);
            const bool tonic = scaleTone && pitch % 12 == scaleSettings.rootPitchClass;
            g.setColour(tonic ? juce::Colour(0xff1f3530)
                              : scaleTone ? juce::Colour(0xff1c2a27)
                              : isBlack(pitch) ? juce::Colour(0xff17181c) : juce::Colour(0xff1e2025));
            g.fillRect(rowBounds);
            g.setColour(pitch % 12 == 0 ? juce::Colour(0xff33363d) : juce::Colour(0xff222429));
            g.drawHorizontalLine(juce::roundToInt(y + rowHeight),
                                 static_cast<float>(a.grid.getX()), static_cast<float>(a.grid.getRight()));

            const juce::Rectangle<float> key(static_cast<float>(a.piano.getX()), y,
                                             static_cast<float>(a.piano.getWidth()), rowHeight);
            const auto light = keyLight(pitch);
            const bool lit = light.state != KeyState::none;
            g.setColour(lit && !isBlack(pitch) ? light.colour : juce::Colour(0xffe2e4e7));
            g.fillRect(key);
            if (isBlack(pitch)) {
                g.setColour(lit ? light.colour.darker(0.35f) : juce::Colour(0xff1a1b1f));
                g.fillRect(key.withWidth(key.getWidth() * 0.72f));
            }
            g.setColour(juce::Colour(0xffa6abb2));
            g.drawHorizontalLine(juce::roundToInt(y + rowHeight), key.getX(), key.getRight());
            // Every key's name when there is room for it, or only the Cs.
            if (pitch % 12 == 0 || (keyLabels == KeyLabels::all && rowHeight >= 8.0f)) {
                g.setColour(isBlack(pitch) && !lit ? juce::Colour(0xffc4c8ce) : juce::Colour(0xff4a4f57));
                g.setFont(juce::FontOptions(juce::jmin(10.5f, rowHeight - 1.0f)));
                g.drawText(noteName(pitch), key.reduced(3.0f, 0.0f), juce::Justification::centredRight);
                g.setFont(juce::FontOptions(10.5f));
            }
        }

        for (int step = 0; step <= pattern.loopSteps; ++step) {
            const auto x = stepX(step, a);
            const bool bar = step % 16 == 0;
            const bool beat = step % 4 == 0;
            g.setColour(bar ? juce::Colour(0xff41454d) : beat ? juce::Colour(0xff2d3036)
                                                                        : juce::Colour(0xff222429));
            g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(a.grid.getY()),
                               static_cast<float>(a.grid.getBottom()));
            if (step < pattern.loopSteps && beat) {
                g.setColour(bar ? juce::Colour(0xffe8eaed) : juce::Colour(0xff8a9098));
                g.drawText(bar ? juce::String(step / 16 + 1)
                               : juce::String(step / 16 + 1) + "." + juce::String((step % 16) / 4 + 1),
                           juce::roundToInt(x) + 4, a.ruler.getY(),
                           juce::roundToInt(stepSize(a) * 4) - 4, a.ruler.getHeight(),
                           juce::Justification::centredLeft);
            }
        }
        forEachGhostNote(ghostClips, pattern.loopSteps, [&](const PianoRollGhostNote& ghost) { paintGhost(g, ghost, a); });
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (!selection.contains(pattern.notes[i].id)) paintNote(g, pattern.notes[i], a, false);
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (selection.contains(pattern.notes[i].id)) paintNote(g, pattern.notes[i], a, true);
        if (drag == Drag::marquee) {
            g.setColour(juce::Colour(0x304ce3cb));
            g.fillRect(marquee);
            g.setColour(juce::Colour(0xff4fe0b6));
            g.drawRect(marquee, 1.0f);
        }

        g.setColour(juce::Colour(0xff131417));
        g.fillRect(a.velocity);
        g.setFont(juce::FontOptions(10.5f));
        g.setColour(juce::Colour(0xff686d75));
        g.drawText("VELOCITY", a.piano.getX(), a.velocity.getY() + 3, a.piano.getWidth(), 16,
                   juce::Justification::centredLeft);
        for (int level = 0; level <= 2; ++level) {
            g.setColour(juce::Colour(0xff24262b));
            const auto y = velocityY(level / 2.0f, a);
            g.drawHorizontalLine(juce::roundToInt(y), static_cast<float>(a.velocity.getX()),
                                 static_cast<float>(a.velocity.getRight()));
        }
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (!selection.contains(pattern.notes[i].id)) paintVelocity(g, pattern.notes[i], a, false);
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (selection.contains(pattern.notes[i].id)) paintVelocity(g, pattern.notes[i], a, true);

        const auto cursorX = juce::jlimit(static_cast<float>(a.grid.getX()) + 1.0f,
                                        static_cast<float>(a.grid.getRight()) - 1.0f, stepX(insertionStep, a));
        g.setColour(juce::Colour(0x75ffcc74));
        g.drawLine(cursorX, static_cast<float>(a.ruler.getBottom()), cursorX,
                   static_cast<float>(a.grid.getBottom()), 1.0f);
        juce::Path cursorMarker;
        cursorMarker.addTriangle(cursorX - 4.0f, static_cast<float>(a.ruler.getY()) + 2.0f,
                                 cursorX + 4.0f, static_cast<float>(a.ruler.getY()) + 2.0f,
                                 cursorX, static_cast<float>(a.ruler.getY()) + 9.0f);
        g.setColour(juce::Colour(0xffffcc74));
        g.fillPath(cursorMarker);

        if (std::isfinite(playhead) && playhead >= 0 && playhead < pattern.loopSteps) {
            const auto x = stepX(playhead, a);
            g.setColour(juce::Colour(0xffe8eaed));
            g.drawLine(x, static_cast<float>(a.ruler.getY()), x,
                       static_cast<float>(a.velocity.getBottom()), 1.8f);
        }
        g.setColour(hasKeyboardFocus(false) ? juce::Colour(0xff4fe0b6) : juce::Colour(0xff2c2e34));
        g.drawRect(a.grid);
        g.setFont(juce::FontOptions(11.0f));
        g.setColour(juce::Colour(0xff9ea3ab));
        juce::String status = juce::String(pattern.loopSteps / 16.0, 0) + "-bar loop  |  "
            + noteName(lowestVisiblePitch) + " - " + noteName(topPitch()) + "  |  "
            + juce::String(static_cast<int>(pattern.count)) + "/" + juce::String(static_cast<int>(PianoRollPattern::maxNotes)) + " notes";
        status += "  |  " + juce::String(static_cast<int>(selection.count)) + " selected";
        status += "  |  Edit cursor: step " + juce::String(insertionStep + 1.0, 3);
        if (editingClips > 1) status += "  |  editing " + juce::String(editingClips) + " clips";
        if (ghostSource != GhostNoteSource::off) {
            const auto ghosts = static_cast<int>(ghostNoteCount());
            status += "  |  " + juce::String(ghosts) + (ghosts == 1 ? " ghost note" : " ghost notes");
        }
        if (const auto* note = pattern.find(activeId))
            status += "  |  " + noteName(note->pitch) + ": step " + juce::String(note->startStep + 1, 3)
                   + ", length " + juce::String(note->lengthSteps, 3) + ", velocity "
                   + juce::String(juce::roundToInt(note->velocity * 127.0f)) + ", chance "
                   + juce::String(juce::roundToInt(note->probability * 100.0f)) + "%";
        if (statusMessage.isNotEmpty()) status = statusMessage;
        g.drawText(status, 12, a.velocity.getBottom() + 4, getWidth() - 24, 17,
                   juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff686d75));
        if (!compactPanel())
            g.drawText("Click: add  |  Right-click: erase  |  Shift-drag: select  |  Ctrl C / V / D: clipboard  |  Alt S/U/R/Q/C/L/I/V/A: tools",
                       12, a.velocity.getBottom() + 21, getWidth() - 24, 17, juce::Justification::centredLeft);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        endEdit();
        grabKeyboardFocus();
        const auto a = areas();
        drag = Drag::none;
        statusMessage.clear();
        if (a.ruler.contains(e.getPosition()) && !e.mods.isPopupMenu()) {
            setInsertionStep(snapStep(e.position.x, a));
            return;
        }
        if (a.piano.contains(e.getPosition())) {
            if (!e.mods.isPopupMenu()) {
                drag = Drag::audition;
                preview(pitchAt(e.position.y, a));
            }
            return;
        }
        if (a.velocity.contains(e.getPosition())) {
            const auto hit = hitVelocity(e.position.x, a);
            if (hit != 0 && !selection.contains(hit)) selectOnly(hit);
            activeId = hit;
            if (hit != 0 && !e.mods.isPopupMenu()) {
                beginEdit();
                drag = Drag::velocity;
                changeVelocity(e.position.y, a);
            }
            updateSelection();
            return;
        }
        if (!a.grid.contains(e.getPosition())) return;
        const auto hit = hitNote(e.position, a);
        if (e.mods.isPopupMenu()) {
            if (hit == 0) return;
            if (!selection.contains(hit)) selectOnly(hit);
            eraseSelected();
            return;
        }
        dragOrigin = e.position;
        if (hit == 0 && e.mods.isShiftDown()) {
            setInsertionStep(snapStep(e.position.x, a));
            marqueeBase = e.mods.isCtrlDown() || e.mods.isCommandDown() ? selection : PianoRollSelection {};
            selection = marqueeBase;
            activeId = selection.count != 0 ? selection.ids[0] : 0;
            marquee = {};
            drag = Drag::marquee;
        } else if (hit == 0) {
            beginEdit();
            setInsertionStep(snapStep(e.position.x, a));
            const auto clickedPitch = pitchAt(e.position.y, a);
            const auto createdPitch = scaleSettings.snap ? snapPitchToScale(clickedPitch, scaleSettings) : clickedPitch;
            const auto created = pattern.add(createdPitch, insertionStep,
                                             rememberedLength > 0.0 ? rememberedLength : snapSteps(),
                                             rememberedVelocity, rememberedProbability);
            selectOnly(created);
            if (created != 0) {
                dragPattern = pattern;
                dragOriginal = *pattern.find(created);
                drag = Drag::move;
                changed();
            } else {
                endEdit();
                updateSelection();
            }
        } else if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
            selection.toggle(hit);
            activeId = selection.contains(hit) ? hit : selection.count != 0 ? selection.ids[0] : 0;
        } else {
            if (!selection.contains(hit)) selectOnly(hit);
            activeId = hit;
        }
        if (hit != 0 && selection.contains(hit)) {
            const auto* note = pattern.find(hit);
            dragOriginal = *note;
            dragPattern = pattern;
            beginEdit();
            const auto bounds = noteBounds(*note, a);
            drag = e.position.x >= bounds.getRight() - juce::jmin(6.0f, bounds.getWidth() * 0.4f)
                 ? Drag::resize : Drag::move;
        }
        updateSelection();
    }

    void mouseDoubleClick(const juce::MouseEvent&) override {}

    void mouseDrag(const juce::MouseEvent& e) override {
        const auto a = areas();
        if (drag == Drag::audition) {
            if (a.piano.contains(e.getPosition())) preview(pitchAt(e.position.y, a));
            else releaseAudition();
            return;
        }
        if (drag == Drag::marquee) {
            marquee = juce::Rectangle<float>(dragOrigin, e.position).getIntersection(a.grid.toFloat());
            selection = marqueeBase;
            for (std::size_t i = 0; i < pattern.count; ++i)
                if (noteBounds(pattern.notes[i], a).intersects(marquee)) selection.add(pattern.notes[i].id);
            activeId = selection.count != 0 ? selection.ids[0] : 0;
            updateSelection();
            return;
        }
        if (drag == Drag::velocity) { changeVelocity(e.position.y, a); return; }
        const auto* current = pattern.find(activeId);
        if (current == nullptr) return;
        const auto delta = (e.position.x - dragOrigin.x) / stepSize(a);
        if (drag == Drag::move) {
            moveSelection(dragPattern,
                std::round((dragOriginal.startStep + delta) / snapSteps()) * snapSteps() - dragOriginal.startStep,
                -juce::roundToInt((e.position.y - dragOrigin.y) / rowSize(a)), true);
        } else if (drag == Drag::resize) {
            const auto end = std::round((dragOriginal.startStep + dragOriginal.lengthSteps + delta)
                                       / snapSteps()) * snapSteps();
            resizeSelection(dragPattern, end - dragOriginal.startStep - dragOriginal.lengthSteps);
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        releaseAudition();
        drag = Drag::none;
        endEdit();
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override { releaseAudition(); }

    void mouseMove(const juce::MouseEvent& e) override {
        const auto a = areas();
        if (const auto* note = pattern.find(hitNote(e.position, a))) {
            const auto bounds = noteBounds(*note, a);
            setMouseCursor(e.position.x >= bounds.getRight() - juce::jmin(6.0f, bounds.getWidth() * 0.4f)
                               ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
        } else setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
        if (wheel.deltaY != 0.0f) setLowestVisible(lowestVisiblePitch + (wheel.deltaY > 0 ? 3 : -3));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (locked || !hasKeyboardFocus(true)) return false;
        const auto code = key.getKeyCode();
        const auto letter = code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        bool textEntry = false;
        for (auto* c = focused; c != nullptr && c != this; c = c->getParentComponent())
            textEntry = textEntry || dynamic_cast<juce::TextEditor*>(c) != nullptr;
        if (panelOpen && code == juce::KeyPress::escapeKey && !textEntry) { closeToolPanel(); return true; }
        if (key.getModifiers().isAltDown() && !key.getModifiers().isCtrlDown()
            && !key.getModifiers().isCommandDown()) {
            const auto tool = letter == 'S' ? 1 : letter == 'U' ? 2 : letter == 'R' ? 3
                            : letter == 'Q' ? 4 : letter == 'C' ? 5 : letter == 'L' ? 6
                            : letter == 'I' ? (key.getModifiers().isShiftDown() ? 8 : 7)
                            : letter == 'V' ? 9 : letter == 'A' ? 10 : 0;
            if (tool != 0 && !textEntry) {
                if (tool >= 7 && tool <= 9) runImmediateTool(tool); else openToolPanel(tool);
                return true;
            }
            return false;
        }
        if ((key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
            && !key.getModifiers().isAltDown() && !textEntry) {
            if (letter == 'Z') { if (key.getModifiers().isShiftDown()) redo(); else undo(); return true; }
            if (letter == 'Y') { redo(); return true; }
            if (letter == 'A') { selection.selectAll(pattern); activeId = selection.count ? selection.ids[0] : 0; updateSelection(); return true; }
            if (letter == 'C') { copyNotes(); return true; }
            if (letter == 'V') { pasteNotes(); return true; }
            if (letter == 'D') { duplicateNotes(); return true; }
            return false;
        }
        if (!hasKeyboardFocus(false)
            || (key.getModifiers().isAnyModifierKeyDown() && !key.getModifiers().isShiftDown())) return false;
        if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey) {
            setLowestVisible(lowestVisiblePitch + (code == juce::KeyPress::pageUpKey ? 12 : -12));
            return true;
        }
        const auto* note = pattern.find(activeId);
        if (note == nullptr) return false;
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
            eraseSelected();
            return true;
        }
        if (code != juce::KeyPress::leftKey && code != juce::KeyPress::rightKey
            && code != juce::KeyPress::upKey && code != juce::KeyPress::downKey) return false;
        if (key.getModifiers().isShiftDown()
            && (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)) {
            transposeSelectedNotes(code == juce::KeyPress::upKey ? 12 : -12);
            return true;
        }
        beginEdit();
        const auto before = pattern;
        if (key.getModifiers().isShiftDown() && (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)) {
            resizeSelection(before, code == juce::KeyPress::rightKey ? snapSteps() : -snapSteps());
        } else {
            moveSelection(before,
                code == juce::KeyPress::rightKey ? snapSteps() : code == juce::KeyPress::leftKey ? -snapSteps() : 0,
                code == juce::KeyPress::upKey ? 1 : code == juce::KeyPress::downKey ? -1 : 0, false);
            if (const auto* moved = pattern.find(activeId)) {
                if (moved->pitch < lowestVisiblePitch) setLowestVisible(moved->pitch);
                if (moved->pitch > topPitch()) setLowestVisible(moved->pitch - visiblePitches + 1);
            }
        }
        endEdit();
        return true;
    }

    void focusLost(FocusChangeType) override { releaseAudition(); endEdit(); drag = Drag::none; repaint(); }
    void focusGained(FocusChangeType) override { repaint(); }
    void visibilityChanged() override { if (!isShowing()) { releaseAudition(); endEdit(); drag = Drag::none; } }

private:
    struct Areas { juce::Rectangle<int> ruler, piano, grid, velocity; };
    enum class Drag { none, move, resize, velocity, audition, marquee };
    static constexpr int lowestPitch = 0, highestPitch = 127, visiblePitches = 24;
    bool compactPanel() const { return panelOpen && getHeight() < 378; }

    Areas areas() const {
        const int panelOffset = panelOpen ? 136 : 0;
        const int gridX = 64, gridY = 68 + panelOffset, gridWidth = juce::jmax(64, getWidth() - gridX - 26);
        const int velocityHeight = compactPanel() ? 50 : 76;
        const int footerHeight = compactPanel() ? 24 : 44;
        const int gridHeight = juce::jmax(48, getHeight() - gridY - velocityHeight - footerHeight - 6);
        return { { gridX, 44 + panelOffset, gridWidth, 24 }, { 10, gridY, 54, gridHeight },
                 { gridX, gridY, gridWidth, gridHeight }, { gridX, gridY + gridHeight + 6, gridWidth, velocityHeight } };
    }
    float stepSize(const Areas& a) const { return a.grid.getWidth() / static_cast<float>(juce::jmax(1, pattern.loopSteps)); }
    static float rowSize(const Areas& a) { return a.grid.getHeight() / static_cast<float>(visiblePitches); }
    float stepX(double step, const Areas& a) const { return a.grid.getX() + static_cast<float>(step) * stepSize(a); }
    int topPitch() const { return lowestVisiblePitch + visiblePitches - 1; }
    int pitchAt(float y, const Areas& a) const {
        return juce::jlimit(lowestVisiblePitch, topPitch(), topPitch() - static_cast<int>((y - a.grid.getY()) / rowSize(a)));
    }
    int snapSteps() const { return juce::jmax(1, snap.getSelectedId()); }
    int snapStep(float x, const Areas& a) const {
        return juce::jlimit(0, pattern.loopSteps - 1,
            static_cast<int>((x - a.grid.getX()) / stepSize(a) / snapSteps()) * snapSteps());
    }
    static bool isBlack(int pitch) {
        const auto p = pitch % 12;
        return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
    }
    static juce::String noteName(int pitch) {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String(names[pitch % 12]) + juce::String(pitch / 12 - 1);
    }
    static const char* scalePitchClassName(int pitchClass) {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names[juce::jlimit(0, 11, pitchClass)];
    }
    juce::Rectangle<float> noteBounds(const PianoRollNote& note, const Areas& a) const {
        return { stepX(note.startStep, a) + 1.0f,
                 a.grid.getY() + (topPitch() - note.pitch) * rowSize(a) + 1.0f,
                 juce::jmax(2.0f, static_cast<float>(note.lengthSteps) * stepSize(a) - 2.0f), juce::jmax(2.0f, rowSize(a) - 2.0f) };
    }
    std::uint32_t hitNote(juce::Point<float> position, const Areas& a) const {
        if (!a.grid.toFloat().contains(position)) return 0;
        if (const auto* note = pattern.find(activeId))
            if (noteBounds(*note, a).contains(position)) return activeId;
        for (auto i = pattern.count; i > 0; --i)
            if (noteBounds(pattern.notes[i - 1], a).contains(position)) return pattern.notes[i - 1].id;
        return 0;
    }
    float velocityX(const PianoRollNote& note, const Areas& a) const { return stepX(note.startStep, a) + 3.0f; }
    static float velocityY(float value, const Areas& a) {
        return a.velocity.getBottom() - 8.0f - value * (a.velocity.getHeight() - 18.0f);
    }
    std::uint32_t hitVelocity(float x, const Areas& a) const {
        const auto radius = juce::jmax(7.0f, stepSize(a) * 0.45f);
        if (const auto* note = pattern.find(activeId))
            if (std::abs(velocityX(*note, a) - x) <= radius) return activeId;
        auto distance = radius;
        std::uint32_t found = 0;
        for (auto i = pattern.count; i > 0; --i) {
            const auto delta = std::abs(velocityX(pattern.notes[i - 1], a) - x);
            if (delta <= distance) { distance = delta; found = pattern.notes[i - 1].id; }
        }
        return found;
    }
    // A note's colour: another clip's when editing several, otherwise the piano roll's own.
    juce::Colour noteColour(std::uint32_t id) const {
        const auto tint = noteTint ? noteTint(id) : std::nullopt;
        return tint.value_or(juce::Colour(0xff3fc9a2));
    }
    void paintNote(juce::Graphics& g, const PianoRollNote& note, const Areas& a, bool isSelected) const {
        if (note.pitch < lowestVisiblePitch || note.pitch > topPitch()) return;
        const auto bounds = noteBounds(note, a);
        const auto opacity = 0.25f + 0.75f * note.probability;
        const auto tint = noteTint ? noteTint(note.id) : std::nullopt;   // a note of another clip being edited
        g.setColour((isSelected ? juce::Colour(0xffb8f7e4) : tint.value_or(juce::Colour(0xff3fc9a2))).withMultipliedAlpha(opacity));
        g.fillRoundedRectangle(bounds, 2.0f);
        g.setColour(isSelected ? juce::Colour(0xffffffff) : juce::Colour(0xff7af0cf));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 1.5f, 1.0f);
        g.setColour(juce::Colour(0xff073b2c));
        g.drawVerticalLine(juce::roundToInt(bounds.getRight() - 3.0f), bounds.getY() + 2.0f, bounds.getBottom() - 2.0f);
        if (bounds.getWidth() > 35.0f && bounds.getHeight() > 12.0f) {
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(noteName(note.pitch), bounds.reduced(4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }
    void paintGhost(juce::Graphics& g, const PianoRollGhostNote& ghost, const Areas& a) const {
        if (ghost.pitch < lowestVisiblePitch || ghost.pitch > topPitch()) return;
        const PianoRollNote shape { 0, ghost.pitch, ghost.startStep, ghost.lengthSteps };
        const auto bounds = noteBounds(shape, a);
        g.setColour(juce::Colour(0x389ea3ab));
        g.fillRoundedRectangle(bounds, 2.0f);
        g.setColour(juce::Colour(0x709ea3ab));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 1.5f, 1.0f);
    }
    void paintVelocity(juce::Graphics& g, const PianoRollNote& note, const Areas& a, bool isSelected) const {
        const auto x = velocityX(note, a), y = velocityY(note.velocity, a);
        g.setColour(isSelected ? juce::Colour(0xffb8f7e4) : juce::Colour(0xff3fc9a2));
        g.drawLine(x, velocityY(0.0f, a), x, y, isSelected ? 2.4f : 1.4f);
        g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
    }
    void changeVelocity(float y, const Areas& a) {
        const auto value = juce::jlimit(1.0f / 127.0f, 1.0f,
            (a.velocity.getBottom() - 8.0f - y) / (a.velocity.getHeight() - 18.0f));
        setSelectedVelocity(value);
    }
    void setSelectedVelocity(float value) {
        bool edited = false;
        for (std::size_t i = 0; i < selection.count; ++i)
            if (const auto* note = pattern.find(selection.ids[i]))
                if (note->velocity != value) edited = pattern.setVelocity(note->id, value) || edited;
        if (edited) changed();
    }
    void setSelectedProbability(float value) {
        bool edited = false;
        for (std::size_t i = 0; i < selection.count; ++i)
            if (const auto* note = pattern.find(selection.ids[i]))
                if (note->probability != value) edited = pattern.setProbability(note->id, value) || edited;
        if (edited) changed();
    }
    void eraseSelected() {
        if (selection.count == 0) return;
        beginEdit();
        for (std::size_t i = 0; i < selection.count; ++i) pattern.erase(selection.ids[i]);
        selection.clear();
        activeId = 0;
        drag = Drag::none;
        changed();
        endEdit();
    }
    void selectOnly(std::uint32_t id) {
        selection.clear();
        if (pattern.find(id) != nullptr) selection.add(id);
        activeId = selection.count != 0 ? id : 0;
    }
    void updateSelection() {
        selection.retain(pattern);
        if (!selection.contains(activeId)) activeId = selection.count != 0 ? selection.ids[0] : 0;
        const auto* note = pattern.find(activeId);
        velocity.setEnabled(note != nullptr);
        if (note != nullptr) velocity.setValue(juce::roundToInt(note->velocity * 127.0f), juce::dontSendNotification);
        probability.setEnabled(note != nullptr);
        if (note != nullptr) probability.setValue(juce::roundToInt(note->probability * 100.0f), juce::dontSendNotification);
        clearButton.setEnabled(pattern.count != 0);
        exampleButton.setEnabled(pattern.count == 0);
        undoButton.setEnabled(canUndo());
        redoButton.setEnabled(canRedo());
        applyButton.setEnabled(tools.getSelectedId() == 5 || tools.getSelectedId() == 6 || tools.getSelectedId() == 11 || selection.count != 0);
        if (panelOpen)
            panelTitle.setText(tools.getSelectedId() == 5
                               ? "Chord stamp at step " + juce::String(insertionStep + 1.0, 3)
                               : tools.getSelectedId() == 11 ? "Chord progression at step " + juce::String(insertionStep + 1.0, 3)
                               : tools.getSelectedId() == 6 ? "Scale highlighting and snapping"
                               : tools.getText() + " selected notes  |  "
                                 + juce::String(static_cast<int>(selection.count)) + " selected",
                               juce::dontSendNotification);
        repaint();
    }
    void changed() {
        if (const auto* note = pattern.find(activeId)) {
            rememberedLength = note->lengthSteps;
            rememberedVelocity = note->velocity;
            rememberedProbability = note->probability;
        }
        updateSelection();
        if (onChange) onChange();
    }
    void preview(int pitch) {
        if (pitch == auditionPitch) return;
        releasePreviewNote();
        auditionPitch = pitch;
        if (onAudition) onAudition(pitch, 0.8f);
        repaint();
    }
    void releasePreviewNote() {
        const auto note = auditionPitch;
        auditionPitch = -1;
        if (note >= 0 && onAudition) onAudition(note, 0.0f);
        if (note >= 0) repaint();
    }
    void loadExample() {
        if (pattern.count != 0) return;
        beginEdit();
        static constexpr int pitches[] { 60, 64, 67, 64, 62, 65, 69, 65, 64, 67, 71, 67, 62, 65, 67, 60 };
        for (int i = 0; i < 16; ++i)
            pattern.add(pitches[i], i * 4, i == 15 ? 4 : 3, i % 4 == 0 ? 0.86f : 0.65f);
        selectOnly(pattern.count != 0 ? pattern.notes[0].id : 0);
        changed();
        endEdit();
    }

    void moveSelection(const PianoRollPattern& original, double deltaTime, int deltaPitch, bool alignAnchor) {
        double earliest = -original.loopSteps, latest = original.loopSteps;
        int minimumPitch = -127, maximumPitch = 127;
        for (std::size_t i = 0; i < selection.count; ++i) if (const auto* note = original.find(selection.ids[i])) {
            earliest = std::max(earliest, -note->startStep);
            latest = std::min(latest, original.loopSteps - note->startStep - note->lengthSteps);
            minimumPitch = std::max(minimumPitch, lowestPitch - note->pitch);
            maximumPitch = std::min(maximumPitch, highestPitch - note->pitch);
        }
        // Each note limits the same group offset so relative chord timing,
        // pitch intervals and durations stay intact at the clip boundaries.
        deltaTime = juce::jlimit(earliest, latest, deltaTime);
        if (alignAnchor) if (const auto* anchor = original.find(activeId)) {
            const auto firstGrid = std::ceil((anchor->startStep + earliest) / snapSteps()) * snapSteps();
            const auto lastGrid = std::floor((anchor->startStep + latest) / snapSteps()) * snapSteps();
            if (firstGrid <= lastGrid)
                deltaTime = juce::jlimit(firstGrid, lastGrid, anchor->startStep + deltaTime) - anchor->startStep;
        }
        deltaPitch = juce::jlimit(minimumPitch, maximumPitch, deltaPitch);
        if (scaleSettings.snap && deltaPitch != 0)
            if (const auto* anchor = original.find(activeId)) {
                const auto target = juce::jlimit(lowestPitch, highestPitch, anchor->pitch + deltaPitch);
                const auto snappedDelta = snapPitchToScale(target, scaleSettings, deltaPitch > 0 ? 1 : -1) - anchor->pitch;
                deltaPitch = snappedDelta >= minimumPitch && snappedDelta <= maximumPitch ? snappedDelta : 0;
            }
        bool edited = false;
        for (std::size_t i = 0; i < selection.count; ++i) {
            const auto* note = original.find(selection.ids[i]);
            const auto* current = pattern.find(selection.ids[i]);
            if (note == nullptr || current == nullptr) continue;
            const auto start = PianoRollPattern::fromTicks(PianoRollPattern::toTicks(note->startStep + deltaTime));
            const auto pitch = note->pitch + deltaPitch;
            if (current->startStep != start || current->pitch != pitch)
                edited = pattern.move(note->id, pitch, start) || edited;
        }
        if (edited) changed();
    }
    void resizeSelection(const PianoRollPattern& original, double deltaLength) {
        double minimum = -original.loopSteps, maximum = original.loopSteps;
        for (std::size_t i = 0; i < selection.count; ++i) if (const auto* note = original.find(selection.ids[i])) {
            minimum = std::max(minimum, PianoRollPattern::minLength - note->lengthSteps);
            maximum = std::min(maximum, original.loopSteps - note->startStep - note->lengthSteps);
        }
        deltaLength = juce::jlimit(minimum, maximum, deltaLength);
        bool edited = false;
        for (std::size_t i = 0; i < selection.count; ++i) {
            const auto* note = original.find(selection.ids[i]);
            const auto* current = pattern.find(selection.ids[i]);
            if (note == nullptr || current == nullptr) continue;
            const auto length = PianoRollPattern::fromTicks(PianoRollPattern::toTicks(note->lengthSteps + deltaLength));
            if (current->lengthSteps != length) edited = pattern.resize(note->id, length) || edited;
        }
        if (edited) changed();
    }

    struct Snapshot {
        PianoRollPattern pattern;
        PianoRollSelection selection;
        std::uint32_t active {};
        double insertion {};
    };
    struct HistoryEntry { Snapshot before, after; };
    static constexpr std::size_t historyLimit = 64;
    Snapshot snapshot() const { return { pattern, selection, activeId, insertionStep }; }
    void beginEdit() {
        if (editOpen) return;
        editBefore = snapshot();
        editOpen = true;
        statusMessage.clear();
    }
    void endEdit() {
        if (!editOpen) return;
        editOpen = false;
        if (samePianoRollPattern(editBefore.pattern, pattern)) return;
        history.resize(historyPosition);
        if (history.size() == historyLimit) { history.erase(history.begin()); --historyPosition; }
        history.push_back({ editBefore, snapshot() });
        historyPosition = history.size();
        updateSelection();
        if (onEditCommitted) onEditCommitted();
    }
    void restore(const Snapshot& state) {
        releaseAudition();
        drag = Drag::none;
        pattern = state.pattern;
        selection = state.selection;
        activeId = state.active;
        insertionStep = state.insertion;
        statusMessage.clear();
        changed();
    }

    void showEditMenu() {
        endEdit();
        releaseAudition();
        drag = Drag::none;
        // A real focus transition also releases held computer-piano keys before
        // a popup can receive their key-up events outside the editor hierarchy.
        editActionsButton.grabKeyboardFocus();
        grabKeyboardFocus();
        juce::PopupMenu menu;
        const auto add = [&menu](int id, const char* label, const char* shortcut, bool enabled) {
            juce::PopupMenu::Item item(label);
            item.itemID = id;
            item.shortcutKeyDescription = shortcut;
            item.isEnabled = enabled;
            menu.addItem(std::move(item));
        };
        add(1, "Select all", "Ctrl+A", pattern.count != 0);
        menu.addSeparator();
        add(2, "Copy", "Ctrl+C", selection.count != 0);
        add(3, "Paste", "Ctrl+V", !clipboard.empty());
        add(4, "Duplicate", "Ctrl+D", selection.count != 0);
        menu.addSeparator();
        add(5, "Transpose up a semitone", "", selection.count != 0);
        add(6, "Transpose down a semitone", "", selection.count != 0);
        add(7, "Transpose up an octave", "Shift+Up", selection.count != 0);
        add(8, "Transpose down an octave", "Shift+Down", selection.count != 0);
        menu.addSeparator();
        add(9, "Split selected notes at cursor", "", selection.count != 0);
        menu.addSeparator();
        juce::PopupMenu labels;
        labels.addItem(30, "C notes only", true, keyLabels == KeyLabels::cNotes);
        labels.addItem(31, "All notes", true, keyLabels == KeyLabels::all);
        menu.addSubMenu("Keyboard labels", labels);
        juce::PopupMenu ghosts;
        ghosts.addItem(20, "Off", true, ghostSource == GhostNoteSource::off);
        ghosts.addItem(21, "Other clips in this scene", true, ghostSource == GhostNoteSource::sameScene);
        ghosts.addItem(22, "This track's clips in other scenes", true, ghostSource == GhostNoteSource::sameTrack);
        ghosts.addSeparator();
        ghosts.addItem(23, "Edit them with this clip", ghostSource != GhostNoteSource::off, multiClip);
        menu.addSubMenu("Ghost notes", ghosts);
        juce::Component::SafePointer<PianoRollEditor> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(editActionsButton).withMinimumWidth(200),
            [safe](int action) {
                if (safe == nullptr) return;
                if (action == 1) {
                    safe->selection.selectAll(safe->pattern);
                    safe->activeId = safe->selection.count != 0 ? safe->selection.ids[0] : 0;
                    safe->updateSelection();
                } else if (action == 2) safe->copyNotes();
                else if (action == 3) safe->pasteNotes();
                else if (action == 4) safe->duplicateNotes();
                else if (action >= 5 && action <= 8)
                    safe->transposeSelectedNotes(action == 5 ? 1 : action == 6 ? -1 : action == 7 ? 12 : -12);
                else if (action == 9) safe->splitSelectedNotes();
                else if (action >= 20 && action <= 22)
                    safe->setGhostSource(action == 20 ? GhostNoteSource::off
                                         : action == 21 ? GhostNoteSource::sameScene : GhostNoteSource::sameTrack);
                else if (action == 23) safe->setMultiClipEditing(!safe->multiClip);
                else if (action == 30 || action == 31) safe->setKeyLabels(action == 30 ? KeyLabels::cNotes : KeyLabels::all);
                if (action != 0) safe->grabKeyboardFocus();
            });
    }
    void prepareClipboardAction() {
        endEdit();
        releaseAudition();
        drag = Drag::none;
    }
    void copyNotes() {
        prepareClipboardAction();
        const auto result = clipboard.copy(pattern, selection);
        if (result.status == ClipboardStatus::copied)
            statusMessage = "Copied " + juce::String(static_cast<int>(clipboard.size()))
                + " selected notes. Click the ruler or empty grid to choose where Ctrl+V pastes.";
        else reportClipboardFailure(result.status);
        repaint();
    }
    void pasteNotes() {
        prepareClipboardAction();
        beginEdit();
        finishClipboardEdit(clipboard.paste(pattern, insertionStep));
    }
    void duplicateNotes() {
        prepareClipboardAction();
        beginEdit();
        finishClipboardEdit(duplicatePianoRollSelection(pattern, selection, snapSteps()));
    }
    void finishClipboardEdit(const ClipboardResult& result) {
        if (result.changed()) {
            selection = result.selection;
            activeId = selection.count != 0 ? selection.ids[0] : 0;
            insertionStep = juce::jlimit(0.0, static_cast<double>(pattern.loopSteps),
                std::ceil(result.endStep / snapSteps()) * snapSteps());
            changed();
            endEdit();
        } else {
            endEdit();
            reportClipboardFailure(result.status);
            repaint();
        }
    }
    void reportClipboardFailure(ClipboardStatus status) {
        if (status == ClipboardStatus::capacityExceeded)
            statusMessage = juce::String("No changes: copying these notes would exceed the ") + juce::String(static_cast<int>(PianoRollPattern::maxNotes)) + "-note limit. Delete notes or copy a smaller selection.";
        else if (status == ClipboardStatus::outOfBounds)
            statusMessage = "No changes: the copied block would extend beyond the loop. Choose an earlier paste position.";
        else if (status == ClipboardStatus::emptyClipboard)
            statusMessage = "Copy selected notes with Ctrl+C before pasting. This piano roll uses its own note clipboard.";
        else if (status == ClipboardStatus::emptySelection)
            statusMessage = "Select notes with Ctrl-click, Shift-drag empty grid space, or use Ctrl+A before copying or duplicating.";
        else
            statusMessage = "No changes: the note selection or paste position is invalid.";
    }

    void configureButton(juce::TextButton& button, const char* text, const char* name) {
        addAndMakeVisible(button);
        button.setButtonText(text);
        button.setName(name);
    }
    void configureParameter(std::size_t index, const char* text, const char* name,
                            double minimum, double maximum, double value) {
        auto& slider = parameters[index];
        parameterLabels[index].setText(text, juce::dontSendNotification);
        slider.setName(name);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSkewFactor(1.0);
        if (maximum > 960.0) slider.setSkewFactorFromMidPoint(240.0);
        else if (maximum == 960.0) slider.setSkewFactorFromMidPoint(120.0);
        slider.setValue(value, juce::dontSendNotification);
        slider.setVisible(true);
        parameterLabels[index].setVisible(true);
    }
    void configureToolPanel() {
        releaseChordPreview(true);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            parameters[i].setVisible(false);
            parameterLabels[i].setVisible(false);
        }
        const auto tool = tools.getSelectedId();
        panelTitle.setText(tool == 5 ? "Chord stamp at step " + juce::String(insertionStep + 1.0, 3)
                                    : tool == 11 ? "Chord progression at step " + juce::String(insertionStep + 1.0, 3)
                                    : tool == 6 ? "Scale highlighting and snapping"
                                    : tool == 10 ? "Arpeggiate selected chords"
                                    : tools.getText() + " selected notes  |  " + juce::String(static_cast<int>(selection.count))
                                      + " selected", juce::dontSendNotification);
        panelHelp.setText(tool == 5 ? "Choose a voicing, preview it, then add the chord as one undoable edit."
                                    : tool == 11 ? "Four diatonic triads. 16 steps = one bar. Inserts at the cursor; existing notes are kept. Undo removes all four."
                                    : tool == 6 ? "Changes apply immediately. Snap affects newly drawn notes and pitch moves; existing notes stay unchanged."
                                    : tool == 10 ? "Sequence notes sharing a start time. Rate and gate use the 240-tick sixteenth grid."
                                    : "240 ticks = one 1/16 note. Changes apply to selected notes only.",
                          juce::dontSendNotification);
        applyButton.setButtonText(tool == 6 ? "Done" : "Apply");
        seed.setVisible(tool == 1 || tool == 3 || tool == 5);
        seedLabel.setVisible(seed.isVisible());
        descending.setVisible(tool == 1 || tool == 10);
        progressionMode.setVisible(tool == 11);
        for (auto& degree : progressionDegrees) degree.setVisible(tool == 11);
        chordType.setVisible(tool == 5);
        chordTypeLabel.setVisible(tool == 5);
        previewChordButton.setVisible(tool == 5);
        for (auto* control : std::initializer_list<juce::Component*> {
                 &scaleRoot, &scaleRootLabel, &scaleType, &scaleTypeLabel, &scaleHighlight, &scaleSnap })
            control->setVisible(tool == 6);
        if (tool == 1) {
            descending.setButtonText("High to low");
            descending.setName("Strum from high pitch to low pitch");
            seed.setName("Processing random seed");
            seedLabel.setText("Random seed", juce::dontSendNotification);
            seed.setRange(1, 999999999, 1);
            configureParameter(0, "Delay (ticks)", "Strum delay ticks", 0, 960, strumSettings.delaySteps * 240.0);
            configureParameter(1, "Velocity +/- %", "Strum velocity variation percent", 0, 100, strumSettings.velocityVariation * 100.0);
            seed.setValue(strumSettings.seed, juce::dontSendNotification);
            descending.setToggleState(strumSettings.descending, juce::dontSendNotification);
        } else if (tool == 2) {
            configureParameter(0, "Slice (ticks)", "Chop slice ticks", 1, 61440, chopSettings.divisionSteps * 240.0);
            configureParameter(1, "Gate (%)", "Chop gate percent", 5, 100, chopSettings.gate * 100.0);
        } else if (tool == 3) {
            seed.setName("Processing random seed");
            seedLabel.setText("Random seed", juce::dontSendNotification);
            seed.setRange(1, 999999999, 1);
            configureParameter(0, "Timing +/- ticks", "Randomize timing ticks", 0, 960, randomizeSettings.timingSteps * 240.0);
            configureParameter(1, "Velocity +/- %", "Randomize velocity variation percent", 0, 100, randomizeSettings.velocityVariation * 100.0);
            configureParameter(2, "Pitch +/- st", "Randomize pitch semitones", 0, 24, randomizeSettings.pitchSemitones);
            seed.setValue(randomizeSettings.seed, juce::dontSendNotification);
        } else if (tool == 4) {
            configureParameter(0, "Grid (ticks)", "Quantize grid ticks", 1, 61440, quantizeSettings.gridSteps * 240.0);
            configureParameter(1, "Strength (%)", "Quantize strength percent", 0, 100, quantizeSettings.strength * 100.0);
            configureParameter(2, "Swing (%)", "Quantize swing percent", 0, 75, quantizeSettings.swing * 100.0);
        } else if (tool == 5) {
            configureParameter(0, "Root (MIDI)", "Chord root MIDI note", 0, 127, chordSettings.root);
            configureParameter(1, "Length (ticks)", "Chord stamp length ticks", 1, 61440, chordSettings.lengthSteps * 240.0);
            configureParameter(2, "Velocity", "Chord stamp velocity", 1, 127, chordSettings.velocity * 127.0f);
            seed.setName("Chord inversion");
            seedLabel.setText("Inversion", juce::dontSendNotification);
            seed.setRange(0, 3, 1);
            seed.setValue(chordSettings.inversion, juce::dontSendNotification);
            chordType.setSelectedId(static_cast<int>(chordSettings.type) + 1, juce::dontSendNotification);
            updateChordInversionRange();
        } else if (tool == 6) {
            scaleRoot.setSelectedId(scaleSettings.rootPitchClass + 1, juce::dontSendNotification);
            scaleType.setSelectedId(static_cast<int>(scaleSettings.type) + 1, juce::dontSendNotification);
            scaleHighlight.setToggleState(scaleSettings.highlight, juce::dontSendNotification);
            scaleSnap.setToggleState(scaleSettings.snap, juce::dontSendNotification);
        } else if (tool == 11) {
            configureParameter(0, "Tonic (MIDI)", "Progression tonic MIDI note", 0, 127, progressionSettings.tonic);
            configureParameter(1, "Steps / chord", "Progression chord length steps", 1, 64, progressionSettings.chordSteps);
            configureParameter(2, "Velocity", "Progression velocity", 1, 127, progressionSettings.velocity * 127.0f);
            progressionMode.setSelectedId(progressionSettings.minor ? 2 : 1, juce::dontSendNotification);
            for (std::size_t i = 0; i < progressionDegrees.size(); ++i)
                progressionDegrees[i].setSelectedId(progressionSettings.degrees[i], juce::dontSendNotification);
        } else if (tool == 10) {
            configureParameter(0, "Rate (ticks)", "Arpeggiate rate ticks", 1, 3840, arpeggiateSettings.rateSteps * 240.0);
            configureParameter(1, "Gate (%)", "Arpeggiate gate percent", 5, 100, arpeggiateSettings.gate * 100.0);
            descending.setButtonText("High to low");
            descending.setName("Arpeggiate from high pitch to low pitch");
            descending.setToggleState(arpeggiateSettings.descending, juce::dontSendNotification);
        }
        updateSelection();
    }
    void openToolPanel(int tool) {
        endEdit();
        releaseAudition();
        drag = Drag::none;
        tools.setSelectedId(tool, juce::dontSendNotification);
        statusMessage.clear();
        panelOpen = true;
        for (auto* child : std::initializer_list<juce::Component*> { &panelTitle, &panelHelp, &applyButton, &cancelButton })
            child->setVisible(true);
        configureToolPanel();
        resized();
        repaint();
        if (tool == 6) scaleRoot.grabKeyboardFocus(); else parameters[0].grabKeyboardFocus();
    }
    void closeToolPanel() {
        releaseChordPreview(true);
        panelOpen = false;
        progressionMode.setVisible(false);
        for (auto& degree : progressionDegrees) degree.setVisible(false);
        for (auto* child : std::initializer_list<juce::Component*> { &panelTitle, &panelHelp, &applyButton,
                                                                 &cancelButton, &seed, &seedLabel, &descending,
                                                                 &chordType, &chordTypeLabel, &previewChordButton,
                                                                 &scaleRoot, &scaleRootLabel, &scaleType, &scaleTypeLabel,
                                                                 &scaleHighlight, &scaleSnap })
            child->setVisible(false);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            parameters[i].setVisible(false);
            parameterLabels[i].setVisible(false);
        }
        resized();
        repaint();
        grabKeyboardFocus();
    }
    void applyTool() {
        endEdit();
        if (tools.getSelectedId() == 6) {
            closeToolPanel();
            return;
        }
        beginEdit();
        if (tools.getSelectedId() == 11) {
            progressionSettings.tonic = static_cast<int>(parameters[0].getValue());
            progressionSettings.chordSteps = parameters[1].getValue();
            progressionSettings.velocity = static_cast<float>(parameters[2].getValue() / 127.0);
            progressionSettings.minor = progressionMode.getSelectedId() == 2;
            for (std::size_t i = 0; i < progressionDegrees.size(); ++i)
                progressionSettings.degrees[i] = progressionDegrees[i].getSelectedId();
            const auto result = insertProgression(pattern, insertionStep, progressionSettings);
            if (result.changed()) {
                selection = result.selection;
                activeId = selection.ids[0];
                changed();
                endEdit();
                closeToolPanel();
            } else {
                endEdit();
                statusMessage = result.status == ChordStampStatus::outOfBounds
                    ? "No changes: four chords do not fit after the cursor. Shorten Steps / chord or move the cursor."
                    : result.status == ChordStampStatus::capacityExceeded
                    ? "No changes: the progression exceeds the clip note limit."
                    : "No changes: choose a lower tonic so every chord fits the MIDI range.";
                panelHelp.setText(statusMessage, juce::dontSendNotification);
                updateSelection();
            }
            return;
        }
        if (tools.getSelectedId() == 5) {
            releaseChordPreview(true);
            chordSettings = currentChordSettings();
            const auto result = stampChord(pattern, insertionStep, chordSettings);
            if (result.changed()) {
                selection = result.selection;
                activeId = selection.ids[0];
                changed();
                endEdit();
                closeToolPanel();
            } else {
                endEdit();
                const auto message = result.status == ChordStampStatus::capacityExceeded
                    ? juce::String("No changes: the chord would exceed the ") + juce::String(static_cast<int>(PianoRollPattern::maxNotes)) + "-note limit."
                    : result.status == ChordStampStatus::outOfBounds
                    ? "No changes: the chord duration extends beyond the loop."
                    : "No changes: the root, inversion, length or velocity is invalid.";
                panelHelp.setText(message, juce::dontSendNotification);
                statusMessage = message;
                updateSelection();
            }
            return;
        }
        ProcessingResult result;
        const auto value = [this](std::size_t i) { return parameters[i].getValue(); };
        const auto randomSeed = static_cast<std::uint32_t>(seed.getValue());
        if (tools.getSelectedId() == 1) {
            strumSettings = { value(0) / 240.0, static_cast<float>(value(1) / 100.0), descending.getToggleState(), randomSeed };
            result = processStrum(pattern, selection, strumSettings);
        } else if (tools.getSelectedId() == 2) {
            chopSettings = { value(0) / 240.0, value(1) / 100.0 };
            result = processChop(pattern, selection, chopSettings);
        } else if (tools.getSelectedId() == 3) {
            randomizeSettings = { value(0) / 240.0, static_cast<float>(value(1) / 100.0), static_cast<int>(value(2)), randomSeed };
            result = processRandomize(pattern, selection, randomizeSettings);
        } else if (tools.getSelectedId() == 4) {
            quantizeSettings = { value(0) / 240.0, value(1) / 100.0, value(2) / 100.0 };
            result = processQuantize(pattern, selection, quantizeSettings);
        } else {
            arpeggiateSettings = { value(0) / 240.0, value(1) / 100.0, descending.getToggleState() };
            result = processArpeggiate(pattern, selection, arpeggiateSettings);
        }
        selection = result.selection;
        if (result.changed()) {
            changed();
            endEdit();
            closeToolPanel();
        } else {
            endEdit();
            const auto message = result.status == ProcessingStatus::capacityExceeded
                ? juce::String("No changes: processing would exceed the ") + juce::String(static_cast<int>(PianoRollPattern::maxNotes)) + "-note limit. Increase slice length or select fewer notes."
                : result.status == ProcessingStatus::invalidInput ? "No changes: the processing settings are invalid."
                : "No changes: selected notes already match these settings.";
            panelHelp.setText(message, juce::dontSendNotification);
            statusMessage = message;
            updateSelection();
        }
    }
    void runImmediateTool(int tool) {
        endEdit();
        releaseAudition();
        drag = Drag::none;
        tools.setSelectedId(tool, juce::dontSendNotification);
        beginEdit();
        ProcessingResult result;
        if (tool == 7) result = processInvertChord(pattern, selection, false);
        else if (tool == 8) result = processInvertChord(pattern, selection, true);
        else result = processReverse(pattern, selection);
        selection = result.selection;
        if (result.changed()) {
            changed();
            endEdit();
        } else {
            endEdit();
            statusMessage = selection.count == 0 ? "Select notes before using " + tools.getItemText(tool - 1)
                                                  : "No changes: the selected notes cannot be transformed further.";
            updateSelection();
        }
        grabKeyboardFocus();
    }
    ChordStampSettings currentChordSettings() const {
        return { static_cast<int>(parameters[0].getValue()),
                 static_cast<ChordType>(juce::jlimit(1, 9, chordType.getSelectedId()) - 1),
                 static_cast<int>(seed.getValue()), parameters[1].getValue() / 240.0,
                 static_cast<float>(parameters[2].getValue() / 127.0) };
    }
    void updateChordInversionRange() {
        ChordStampSettings settings;
        settings.type = static_cast<ChordType>(juce::jlimit(1, 9, chordType.getSelectedId()) - 1);
        const auto count = chordPitches(settings).count;
        seed.setRange(0, static_cast<double>(count > 0 ? count - 1 : 0), 1);
    }
    void releaseChordPreview(bool clearToggle) {
        for (std::size_t i = 0; i < previewedChordCount; ++i)
            if (onAudition) onAudition(previewedChordPitches[i], 0.0f);
        previewedChordCount = 0;
        if (clearToggle) previewChordButton.setToggleState(false, juce::dontSendNotification);
    }
    void startChordPreview() {
        releaseChordPreview(false);
        const auto settings = currentChordSettings();
        const auto pitches = chordPitches(settings);
        if (pitches.count == 0) {
            previewChordButton.setToggleState(false, juce::dontSendNotification);
            return;
        }
        previewedChordPitches = pitches.values;
        previewedChordCount = pitches.count;
        for (std::size_t i = 0; i < previewedChordCount; ++i)
            if (onAudition) onAudition(previewedChordPitches[i], settings.velocity);
    }
    void refreshChordPreview() {
        if (panelOpen && tools.getSelectedId() == 5 && previewChordButton.getToggleState())
            startChordPreview();
    }
    void updateScaleSettings() {
        scaleSettings.rootPitchClass = juce::jlimit(1, 12, scaleRoot.getSelectedId()) - 1;
        scaleSettings.type = static_cast<ScaleType>(juce::jlimit(1, 10, scaleType.getSelectedId()) - 1);
        scaleSettings.highlight = scaleHighlight.getToggleState();
        scaleSettings.snap = scaleSnap.getToggleState();
        statusMessage = juce::String(scalePitchClassName(scaleSettings.rootPitchClass)) + " "
            + scaleType.getText() + (scaleSettings.snap ? " scale snap on" : " scale highlight");
        repaint();
    }
    void setLowestVisible(int pitch) {
        const auto next = juce::jlimit(lowestPitch, highestPitch - visiblePitches + 1, pitch);
        if (next == lowestVisiblePitch) return;
        releaseAudition();
        lowestVisiblePitch = next;
        updatePitchScroll();
        repaint();
    }
    void updatePitchScroll() {
        pitchScroll.setCurrentRange(highestPitch - topPitch(), visiblePitches, juce::dontSendNotification);
    }
    void scrollBarMoved(juce::ScrollBar*, double newStart) override {
        setLowestVisible(highestPitch - juce::roundToInt(newStart) - visiblePitches + 1);
    }

    PianoRollPattern pattern;
    juce::ComboBox snap, tools, chordType, scaleRoot, scaleType;
    juce::Label snapLabel, velocityLabel, probabilityLabel, chordTypeLabel, scaleRootLabel, scaleTypeLabel;
    juce::Slider velocity, probability;
    juce::TextButton clearButton, exampleButton, editActionsButton, toolButton, undoButton, redoButton, applyButton, cancelButton;
    juce::Label panelTitle, panelHelp, seedLabel;
    std::array<juce::Label, 3> parameterLabels;
    std::array<juce::Slider, 3> parameters;
    juce::Slider seed;
    juce::ToggleButton descending, previewChordButton, scaleHighlight, scaleSnap;
    juce::ScrollBar pitchScroll { true };
    PianoRollSelection selection, marqueeBase;
    PianoRollClipboard clipboard;
    GhostNoteSource ghostSource { GhostNoteSource::off };
    bool multiClip{false};
    KeyLabels keyLabels{KeyLabels::cNotes};
    int editingClips{1};
    std::vector<PianoRollGhostClip> ghostClips;
    std::uint32_t activeId {};
    PianoRollPattern dragPattern;
    PianoRollNote dragOriginal {};
    juce::Point<float> dragOrigin;
    juce::Rectangle<float> marquee;
    Drag drag { Drag::none };
    std::vector<HistoryEntry> history;
    std::size_t historyPosition {};
    Snapshot editBefore;
    bool editOpen {}, panelOpen {}, locked {};
    juce::String statusMessage;
    StrumSettings strumSettings;
    ChopSettings chopSettings;
    RandomizeSettings randomizeSettings;
    QuantizeSettings quantizeSettings;
    ArpeggiateSettings arpeggiateSettings;
    ProgressionSettings progressionSettings;
    juce::ComboBox progressionMode;
    std::array<juce::ComboBox, 4> progressionDegrees;
    ChordStampSettings chordSettings;
    ScaleSettings scaleSettings;
    std::array<int, 4> previewedChordPitches {};
    std::size_t previewedChordCount {};
    int lowestVisiblePitch { 48 }, auditionPitch { -1 };
    double playhead { -1.0 }, insertionStep {}, rememberedLength {};
    float rememberedVelocity { 0.8f };
    float rememberedProbability { 1.0f };
};

} // namespace alien
