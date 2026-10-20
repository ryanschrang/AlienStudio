#pragma once
#include "StudioWidgets.h"
#include "Branding.h"
#include "MidiRecording.h"
#include <array>
#include <functional>

namespace alien {

// Application top bar: brand, view switch, transport (play + LCD with tempo)
// and project file actions. It only reports intent through callbacks.
class TopBar final : public juce::Component {
public:
    static constexpr int height = 92;

    TopBar() {
        namespace col = ui::colours;
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
            const juce::String name(viewNames[static_cast<std::size_t>(i)]);
            addAndMakeVisible(b);
            b.setButtonText(name);
            b.setName("Show " + name + " view");
            b.getProperties().set("style", "tab");
            b.setRadioGroupId(1001);
            b.setTooltip("Show the " + name + " view");
            b.onClick = [this, i] { if (onViewSelected) onViewSelected(i); };
        }

        addAndMakeVisible(extraViews);
        extraViews.addItem("Drums", 5); extraViews.addItem("Arrange", 6);
        extraViews.addItem("Audio lab", 7); extraViews.addItem("Plugins", 8);
        extraViews.setTextWhenNothingSelected("More...");
        extraViews.onChange = [this] { if (onViewSelected && extraViews.getSelectedId() > 0) onViewSelected(extraViews.getSelectedId() - 1); };
        addAndMakeVisible(play);
        play.setButtonText("Play");
        play.setIconOnly(true);
        play.setGlyph(ui::Glyph::play);
        play.setName("Start or stop playback");
        play.setTooltip("Start or stop the current pattern");
        play.setColour(juce::TextButton::buttonColourId, col::control);
        play.setColour(juce::TextButton::buttonOnColourId, col::accent);
        play.setGlyphColour(col::accent);
        play.onClick = [this] { if (onPlayToggled) onPlayToggled(); };

        addAndMakeVisible(record);
        record.setButtonText("Record");
        record.setIconOnly(true);
        record.setGlyph(ui::Glyph::record);
        record.setName("Record MIDI");
        record.setTooltip("Record MIDI into the selected clip. Press again to stop.");
        record.setColour(juce::TextButton::buttonColourId, col::control);
        record.setColour(juce::TextButton::buttonOnColourId, col::danger);
        record.setColour(juce::TextButton::textColourOnId, col::text);
        record.setGlyphColour(col::danger);
        record.onClick = [this] { if (onRecordToggled) onRecordToggled(); };

        addAndMakeVisible(metronome);
        metronome.setButtonText("Metronome");
        metronome.setIconOnly(true);
        metronome.setGlyph(ui::Glyph::metronome);
        metronome.setName("Metronome");
        metronome.setTooltip("Click along with the tempo while playing. The count-in always clicks.");
        metronome.setClickingTogglesState(true);
        metronome.setColour(juce::TextButton::buttonColourId, col::control);
        metronome.setColour(juce::TextButton::buttonOnColourId, col::accent);
        metronome.setColour(juce::TextButton::textColourOnId, col::accentInk);
        metronome.setGlyphColour(col::textDim);
        metronome.onClick = [this] { if (onMetronomeToggled) onMetronomeToggled(metronome.getToggleState()); };

        addAndMakeVisible(recordOptions);
        recordOptions.setButtonText("Recording options");
        recordOptions.setIconOnly(true);
        recordOptions.setGlyph(ui::Glyph::chevronDown);
        recordOptions.setName("Recording options");
        recordOptions.setTooltip("Count-in, metronome volume, overdub or replace, and latency compensation");
        recordOptions.getProperties().set("style", "ghost");
        recordOptions.onClick = [this] { showRecordOptions(); };

        addAndMakeVisible(transportDisplay);
        tempo.setSliderStyle(juce::Slider::LinearBarVertical);
        tempo.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        tempo.setRange(60, 200, 1);
        tempo.setValue(120, juce::dontSendNotification);
        tempo.setName("Tempo");
        tempo.setTooltip("Tempo in beats per minute. Drag up/down or use the mouse wheel; double-click resets to 120.");
        tempo.setDoubleClickReturnValue(true, 120.0);
        tempo.setMouseDragSensitivity(220);
        tempo.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        tempo.onValueChange = [this] { if (onTempoChanged) onTempoChanged(static_cast<int>(tempo.getValue())); };
        tempo.onDragEnd = [this] { if (onTempoCommitted) onTempoCommitted(); };

        for (auto* b : {&newButton, &openButton, &saveButton, &portableButton, &undoButton, &redoButton}) {
            addAndMakeVisible(b);
            b->getProperties().set("style", "ghost");
        }
        undoButton.setButtonText("Undo");
        undoButton.setGlyph(ui::Glyph::undo);
        undoButton.setIconOnly(true);
        undoButton.setName("Undo");
        undoButton.onClick = [this] { if (onUndo) onUndo(); };
        redoButton.setButtonText("Redo");
        redoButton.setGlyph(ui::Glyph::redo);
        redoButton.setIconOnly(true);
        redoButton.setName("Redo");
        redoButton.onClick = [this] { if (onRedo) onRedo(); };
        setUndoState({}, {});
        newButton.setButtonText("New");
        newButton.setGlyph(ui::Glyph::newDocument);
        newButton.setName("New Alien Studio session");
        newButton.setTooltip("Start a new, empty session (Ctrl+N)");
        newButton.onClick = [this] { if (onNew) onNew(); };
        saveButton.setButtonText("Save");
        saveButton.setGlyph(ui::Glyph::save);
        saveButton.setName("Save Alien Studio session project");
        saveButton.setTooltip("Save the session project (Ctrl+S)");
        saveButton.onClick = [this] { if (onSave) onSave(); };
        portableButton.setButtonText("Save portable");
        portableButton.setGlyph(ui::Glyph::collect);
        portableButton.setName("Save project copy with collected audio");
        portableButton.setTooltip("Save a project file and adjacent media folder; move both together");
        portableButton.onClick = [this] { if (onSavePortable) onSavePortable(); };
        openButton.setButtonText("Open");
        openButton.setGlyph(ui::Glyph::open);
        openButton.setName("Open Alien Studio session project");
        openButton.setTooltip("Open a session project (Ctrl+O)");
        openButton.onClick = [this] { if (onOpen) onOpen(); };
    }

    std::function<void(int)> onViewSelected;
    std::function<void()> onRecordToggled;
    std::function<void(bool)> onMetronomeToggled;
    std::function<void(const RecordSettings&)> onRecordSettingsChanged;
    std::function<void()> onPlayToggled, onTempoCommitted, onNew, onOpen, onSave, onSavePortable, onUndo, onRedo;
    std::function<void(int)> onTempoChanged;

    void setActiveView(int index) {
        extraViews.setSelectedId(index >= 4 ? index + 1 : 0, juce::dontSendNotification);
        for (std::size_t i = 0; i < viewButtons.size(); ++i)
            viewButtons[i].setToggleState(static_cast<int>(i) == index, juce::dontSendNotification);
    }

    void setPlaying(bool playing) {
        if (play.getToggleState() == playing) return;
        play.setToggleState(playing, juce::dontSendNotification);
        play.setGlyph(playing ? ui::Glyph::stop : ui::Glyph::play);
        play.setButtonText(playing ? "Stop" : "Play");
    }

    // 0 = idle, 1 = counting in, 2 = recording (RecordControls::Phase).
    void setRecordState(int phase) {
        if (phase == recordPhase) return;
        recordPhase = phase;
        record.setColour(juce::TextButton::buttonOnColourId, phase == RecordControls::countIn ? ui::colours::warning : ui::colours::danger);
        record.setColour(juce::TextButton::textColourOnId, phase == RecordControls::countIn ? ui::colours::accentInk : ui::colours::text);
        record.setToggleState(phase != RecordControls::idle, juce::dontSendNotification);
        record.setTooltip(phase == RecordControls::countIn ? "Counting in. Press to cancel."
                          : phase == RecordControls::recording ? "Recording. Press to stop."
                                                               : "Record MIDI into the selected clip. Press again to stop.");
    }

    void setRecordSettings(const RecordSettings& settings) {
        recordSettings = settings;
        metronome.setToggleState(settings.metronome, juce::dontSendNotification);
    }

    // Shown in the options tooltip so the player can judge the compensation.
    void setOutputLatencyMs(double ms) {
        recordOptions.setTooltip("Count-in, metronome volume, overdub or replace, and latency compensation. Device output latency "
                                 + juce::String(ms, 1) + " ms is compensated automatically.");
    }

    void setPosition(int step, bool running, const juce::String& source) { transportDisplay.setPosition(step, running, source); }
    void setTempo(int bpm) { tempo.setValue(bpm, juce::dontSendNotification); }
    bool isTempoDragging() const { return tempo.isMouseButtonDown(); }

    void setBusy(bool busy) {
        record.setEnabled(!busy || recordPhase != RecordControls::idle);
        saveButton.setEnabled(!busy); portableButton.setEnabled(!busy); openButton.setEnabled(!busy);
        newButton.setEnabled(!busy);
    }

    // Empty label: nothing to undo/redo.
    void setUndoState(const juce::String& undoLabel, const juce::String& redoLabel) {
        undoButton.setEnabled(undoLabel.isNotEmpty());
        redoButton.setEnabled(redoLabel.isNotEmpty());
        undoButton.setTooltip(undoLabel.isNotEmpty() ? "Undo " + undoLabel + " (Ctrl+Z)" : juce::String("Nothing to undo"));
        redoButton.setTooltip(redoLabel.isNotEmpty() ? "Redo " + redoLabel + " (Ctrl+Shift+Z)" : juce::String("Nothing to redo"));
    }

    void resized() override {
        extraViews.setBounds(12, 62, 155, 24);
        auto top = getLocalBounds().withHeight(58).reduced(12, 0);
        const bool compact = getWidth() < 1320;
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

        const std::array<int, 4> widths{62, 88, 74, 62};
        const int total = widths[0] + widths[1] + widths[2] + widths[3] + 12;
        auto views = top.removeFromLeft(total).withSizeKeepingCentre(total, 32);
        viewSwitchBounds = views.expanded(4, 4);
        for (std::size_t i = 0; i < viewButtons.size(); ++i) {
            viewButtons[i].setBounds(views.removeFromLeft(widths[i]));
            views.removeFromLeft(4);
        }

        auto files = top.removeFromRight(compact ? 152 : 372);
        for (auto* b : {&portableButton, &saveButton, &openButton, &newButton}) {
            b->setIconOnly(compact);
            const int w = compact ? 34 : (b == &portableButton ? 128 : b == &newButton ? 70 : 76);
            b->setBounds(files.removeFromRight(w).withSizeKeepingCentre(w, 32));
            files.removeFromRight(compact ? 4 : 2);
        }
        top.removeFromRight(6);
        auto history = top.removeFromRight(72);
        redoButton.setBounds(history.removeFromRight(34).withSizeKeepingCentre(34, 32));
        history.removeFromRight(4);
        undoButton.setBounds(history.removeFromRight(34).withSizeKeepingCentre(34, 32));

        const int recordWidth = 34 + 20 + 6 + 40 + 6;
        const int transportWidth = recordWidth + 44 + 8 + 300;
        auto transport = top.withSizeKeepingCentre(juce::jmin(top.getWidth(), transportWidth), 44);
        metronome.setBounds(transport.removeFromLeft(34).withSizeKeepingCentre(34, 32));
        recordOptions.setBounds(transport.removeFromLeft(20).withSizeKeepingCentre(20, 32));
        transport.removeFromLeft(6);
        record.setBounds(transport.removeFromLeft(40).withSizeKeepingCentre(40, 40));
        transport.removeFromLeft(6);
        play.setBounds(transport.removeFromLeft(44).withSizeKeepingCentre(44, 40));
        transport.removeFromLeft(8);
        transportDisplay.setBounds(transport);
    }

    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1e2025), 0.0f, 0.0f,
                                               juce::Colour(0xff17181c), 0.0f, static_cast<float>(r.getBottom()), false));
        g.fillRect(r);
        g.setColour(juce::Colours::black);
        g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);
        g.setColour(ui::colours::inset.withAlpha(0.8f));
        g.fillRoundedRectangle(viewSwitchBounds.toFloat(), 7.0f);
    }

private:
    void showRecordOptions() {
        namespace rm = recordmenu;
        const auto& current = recordSettings;
        juce::PopupMenu menu;
        menu.addSectionHeader("Count-in");
        for (int bars = 0; bars <= RecordSettings::maxCountInBars; ++bars)
            menu.addItem(rm::countInBase + bars, bars == 0 ? juce::String("Off") : juce::String(bars) + (bars == 1 ? " bar" : " bars"),
                         true, current.countInBars == bars);
        menu.addSectionHeader("Metronome volume");
        const int volume = rm::closestVolumeIndex(current.metronomeVolume);
        for (int i = 0; i < static_cast<int>(rm::volumeLevels.size()); ++i)
            menu.addItem(rm::volumeBase + i, rm::volumeNames[static_cast<std::size_t>(i)], true, i == volume);
        menu.addSectionHeader("Recording over a clip that has notes");
        menu.addItem(rm::overdubId, "Overdub: add to the notes already there", true, !current.replace);
        menu.addItem(rm::replaceId, "Replace: start the clip over", true, current.replace);

        juce::PopupMenu latency;
        bool listed = false;
        for (int i = 0; i < static_cast<int>(rm::latencyChoices.size()); ++i) {
            const int ms = rm::latencyChoices[static_cast<std::size_t>(i)];
            const bool on = ms == current.latencyOffsetMs;
            listed = listed || on;
            latency.addItem(rm::latencyBase + i,
                            ms == 0 ? juce::String("0 ms (device latency only)")
                                    : (ms > 0 ? "+" : "") + juce::String(ms) + " ms" + (ms > 0 ? " (earlier)" : " (later)"),
                            true, on);
        }
        if (!listed) latency.addItem(9000, "Current: " + juce::String(current.latencyOffsetMs) + " ms", false, true);
        latency.addSeparator();
        latency.addItem(rm::nudgeEarlierId, "Nudge earlier by " + juce::String(rm::nudgeMs) + " ms");
        latency.addItem(rm::nudgeLaterId, "Nudge later by " + juce::String(rm::nudgeMs) + " ms");
        menu.addSeparator();
        menu.addSubMenu("Latency compensation (" + (current.latencyOffsetMs > 0 ? juce::String("+") : juce::String())
                        + juce::String(current.latencyOffsetMs) + " ms)", latency);

        juce::Component::SafePointer<TopBar> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recordOptions), [safe](int id) {
            if (safe == nullptr || id <= 0) return;
            const auto updated = recordmenu::apply(safe->recordSettings, id);
            if (updated == safe->recordSettings) return;
            safe->recordSettings = updated;
            if (safe->onRecordSettingsChanged) safe->onRecordSettingsChanged(updated);
        });
    }

    juce::ComboBox extraViews;
    branding::LogoMark brandMark;
    juce::Label title, brandSubtitle;
    std::array<ui::GlyphButton, 4> viewButtons;
    ui::GlyphButton play, record, metronome, recordOptions, newButton, openButton, saveButton, portableButton, undoButton, redoButton;
    RecordSettings recordSettings;
    int recordPhase{RecordControls::idle};
    juce::Slider tempo{juce::Slider::LinearBarVertical, juce::Slider::NoTextBox};
    ui::TransportDisplay transportDisplay{tempo};
    juce::Rectangle<int> viewSwitchBounds;
};

} // namespace alien
