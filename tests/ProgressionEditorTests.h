#pragma once
#include "PianoRollEditor.h"

static void progressionEditorChecks(const juce::File& preview = {}) {
    alien::PianoRollEditor editor;
    editor.setSize(960, 500);
    auto find = [&](const juce::String& name) -> juce::Component* {
        for (auto* child : editor.getChildren()) if (child->getName() == name) return child;
        return nullptr;
    };
    auto* tools = dynamic_cast<juce::ComboBox*>(find("Piano roll processing tool"));
    auto* open = dynamic_cast<juce::TextButton*>(find("Open piano roll processing settings"));
    auto* apply = dynamic_cast<juce::TextButton*>(find("Apply piano roll processing"));
    require(tools && open && apply, "progression has an existing accessible tools entry");
    clickButton(*open);
    tools->setSelectedId(11, juce::sendNotificationSync);
    require(editor.isProcessingPanelOpen() && apply->isEnabled(), "progression opens on an empty clip");
    for (int i = 1; i <= 4; ++i) {
        auto* chord = find("Progression chord " + juce::String(i));
        require(chord && chord->isVisible() && chord->getWidth() > 60 && !chord->getBounds().intersects(apply->getBounds()),
                "all chord controls fit at minimum window width");
    }
    if (preview != juce::File{}) {
        auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 1.5f);
        auto stream = preview.createOutputStream();
        require(stream && juce::PNGImageFormat().writeImageToStream(image, *stream), "progression preview renders");
    }
    int edits = 0;
    editor.onChange = [&] { ++edits; };
    clickButton(*apply);
    require(editor.getPattern().count == 12 && edits == 1 && !editor.isProcessingPanelOpen(),
            "apply publishes one edit containing all chords");
    editor.undo();
    require(editor.getPattern().count == 0, "one undo removes the entire progression");
    editor.redo();
    require(editor.getPattern().count == 12, "one redo restores all chords");
    tools->setSelectedId(11, juce::sendNotificationSync);
    clickButton(*open);
    auto* tonic = dynamic_cast<juce::Slider*>(find("Progression tonic MIDI note"));
    require(tonic != nullptr, "tonic control is accessible");
    tonic->setValue(127, juce::sendNotificationSync);
    const auto before = editor.getPattern();
    clickButton(*apply);
    require(editor.isProcessingPanelOpen() && alien::samePianoRollPattern(before, editor.getPattern()),
            "invalid progression preserves clip and keeps settings open for correction");
    auto* cancel = dynamic_cast<juce::TextButton*>(find("Cancel piano roll processing"));
    require(cancel != nullptr, "cancel is accessible");
    clickButton(*cancel);
    require(!find("Progression scale")->isVisible() && !find("Progression chord 1")->isVisible(),
            "cancel hides progression controls");
}
