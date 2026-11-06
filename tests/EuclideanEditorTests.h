#pragma once
#include "PianoRollEditor.h"

static void euclideanEditorTests() {
    using namespace alien;
    ToneEngine engine; ProjectController c(engine); PianoRollEditor roll; roll.setSize(960, 500);
    PianoRollPattern initial; initial.add(42, 1, 2, .4f);
    c.setClipPattern(0, 0, initial, false); c.commit("Fixture notes"); roll.setPattern(initial);
    bool loading = false; int changes = 0, commits = 0;
    roll.onChange = [&] { if (!loading) { ++changes; c.setClipPattern(0, 0, roll.getPattern(), true); } };
    roll.onEditCommitted = [&] { ++commits; c.commit("Edit notes", 0, 0); };
    const auto context = [&] { roll.setProjectScaleContext(c.document().musicalScale, c.document().identityScope); };
    c.onMusicalScaleChanged = context;
    c.onDocumentRestored = [&] { loading = true; roll.setPattern(*c.document().clip(0, 0).pattern); loading = false; context(); };
    require(c.setProjectScale(0, ScaleType::major, c.document().identityScope), "generator fixture defines project key");
    roll.setScaleSettings({2, ScaleType::naturalMinor, true, true}); // local setting must not override explicit project following
    const auto child = [&](const char* name) -> juce::Component* {
        for (auto* component : roll.getChildren()) if (component->getName() == name) return component;
        require(false, "Euclidean editor control exists"); return nullptr;
    };
    auto* tools = dynamic_cast<juce::ComboBox*>(child("Piano roll processing tool"));
    auto* open = dynamic_cast<juce::Button*>(child("Open piano roll processing settings"));
    auto* apply = dynamic_cast<juce::Button*>(child("Apply piano roll processing"));
    const auto click = [](juce::Button* b) { require(b != nullptr, "generator button exists"); b->triggerClick(); pump(30); };
    tools->setSelectedId(12, juce::sendNotificationSync); click(open);
    auto* steps = dynamic_cast<juce::Slider*>(child("Euclidean steps"));
    auto* hits = dynamic_cast<juce::Slider*>(child("Euclidean hits"));
    auto* rotation = dynamic_cast<juce::Slider*>(child("Euclidean rotation"));
    auto* pitch = dynamic_cast<juce::Slider*>(child("Euclidean MIDI pitch"));
    auto* follow = dynamic_cast<juce::ToggleButton*>(child("Euclidean follow project scale"));
    require(steps && hits && rotation && pitch && follow && !follow->getToggleState(), "generator controls exist and project following defaults off");
    for (auto* control : std::array<juce::Component*, 5>{steps, hits, rotation, pitch, follow})
        require(control->isVisible() && control->getWidth() > 40 && roll.getLocalBounds().contains(control->getBounds())
            && !control->getBounds().intersects(apply->getBounds()), "generator controls fit minimum width without Apply overlap");
    steps->setValue(8, juce::sendNotificationSync); hits->setValue(3, juce::sendNotificationSync);
    rotation->setValue(1, juce::sendNotificationSync); pitch->setValue(61, juce::sendNotificationSync); click(follow);
    require(c.setProjectScale(1, ScaleType::major, c.document().identityScope), "project key can change while generator panel is open");
    roll.setInsertionStep(4); click(apply);
    require(!roll.isProcessingPanelOpen() && changes == 1 && commits == 1 && roll.getPattern().count == 4, "generator publishes one edit and one owner commit");
    require(samePianoRollPattern(roll.getPattern(), *c.document().clip(0, 0).pattern), "owner receives exact generated notes");
    require(roll.getPattern().notes[1].pitch == 61 && roll.getPattern().notes[1].startStep == 5
        && roll.getPattern().notes[2].startStep == 8 && roll.getPattern().notes[3].startStep == 11, "Apply uses current project key and rotated cursor positions");
    c.undo(); require(samePianoRollPattern(roll.getPattern(), initial), "one owner Undo restores pre-generator notes");
    c.redo(); require(roll.getPattern().count == 4, "one owner Redo restores all generated notes");
    c.undo(); const auto redo = c.redoLabel(); const auto before = roll.getPattern();
    click(open); hits->setValue(0, juce::sendNotificationSync); click(apply);
    require(roll.isProcessingPanelOpen() && samePianoRollPattern(before, roll.getPattern()) && c.redoLabel() == redo && commits == 1, "zero hits keeps settings open and preserves owner redo");
    hits->setValue(3, juce::sendNotificationSync); roll.setInsertionStep(60); click(apply);
    require(samePianoRollPattern(before, roll.getPattern()) && c.redoLabel() == redo && commits == 1, "cycle overflow refuses atomically in editor");
    hits->setValue(9, juce::sendNotificationSync); roll.setInsertionStep(0); click(apply);
    require(samePianoRollPattern(before, roll.getPattern()) && commits == 1, "invalid hit count keeps editor notes unchanged");
    click(dynamic_cast<juce::Button*>(child("Cancel piano roll processing")));
    require(!follow->isVisible() && !steps->isVisible(), "Cancel hides generator controls");
    // Generator is also available without a selection in a wholly empty clip.
    PianoRollEditor empty; empty.setSize(960, 500);
    juce::ComboBox* emptyTools = nullptr; juce::Button* emptyOpen = nullptr; juce::Button* emptyApply = nullptr;
    for (auto* component : empty.getChildren()) {
        if (component->getName() == "Piano roll processing tool") emptyTools = dynamic_cast<juce::ComboBox*>(component);
        if (component->getName() == "Open piano roll processing settings") emptyOpen = dynamic_cast<juce::Button*>(component);
        if (component->getName() == "Apply piano roll processing") emptyApply = dynamic_cast<juce::Button*>(component);
    }
    require(emptyTools && emptyOpen && emptyApply, "empty clip exposes generator");
    emptyTools->setSelectedId(12, juce::sendNotificationSync); click(emptyOpen);
    require(emptyApply->isEnabled(), "empty clip needs no note selection to generate"); click(emptyApply);
    require(empty.getPattern().count == 5, "default generator creates five hits in empty clip");
    empty.undo(); require(empty.getPattern().count == 0, "one local Undo removes entire rhythm");
    empty.redo(); require(empty.getPattern().count == 5, "one local Redo restores entire rhythm");
}
