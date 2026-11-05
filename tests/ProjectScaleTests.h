#pragma once
#include "PianoRollEditor.h"

static void projectScaleTests() {
    using alien::ScaleType;
    ToneEngine engine; alien::ProjectController c(engine); alien::PianoRollEditor roll; roll.setSize(1000, 600);
    c.setClipPattern(0, 0, patternWith(60), false); roll.setPattern(patternWith(60));
    int edits = 0, restored = 0, changes = 0;
    roll.onChange = [&] { ++edits; };
    const auto refresh = [&] { roll.setProjectScaleContext(c.document().musicalScale, c.document().identityScope); };
    c.onMusicalScaleChanged = [&] { ++changes; refresh(); };
    c.onDocumentRestored = [&] { ++restored; refresh(); };
    roll.onProjectScaleRequested = [&](int root, ScaleType type, std::uint32_t scope) { return c.setProjectScale(root, type, scope); };
    refresh(); roll.setScaleSettings({2, ScaleType::naturalMinor, true, true});
    const auto child = [&](const char* name) -> juce::Component* {
        for (int i = 0; i < roll.getNumChildComponents(); ++i) if (roll.getChildComponent(i)->getName() == name) return roll.getChildComponent(i);
        require(false, "project scale control exists"); return nullptr;
    };
    auto* tools = dynamic_cast<juce::ComboBox*>(child("Piano roll processing tool")); tools->setSelectedId(6, juce::sendNotificationSync);
    dynamic_cast<juce::Button*>(child("Open piano roll processing settings"))->onClick();
    auto* publish = dynamic_cast<juce::Button*>(child("Use as project scale")); auto* follow = dynamic_cast<juce::Button*>(child("Follow project scale"));
    auto* root = dynamic_cast<juce::ComboBox*>(child("Scale root note"));
    require(publish && follow && root && publish->isVisible() && roll.getLocalBounds().contains(publish->getBounds()), "project-scale controls appear in Scale tool");
    engine.playing.store(true); engine.resetRackPlayback.store(false);
    publish->onClick();
    require(c.document().musicalScale == alien::ProjectMusicalScale{true, 2, ScaleType::naturalMinor} && roll.followsProjectScale() && !root->isEnabled(), "Scale tool publishes current key and follows shared context");
    require(changes == 1 && restored == 0 && engine.playing.load() && !engine.resetRackPlayback.load(), "metadata execution refreshes context without restoring plugins or changing transport requests");
    const auto scope = c.document().identityScope;
    require(!c.setProjectScale(-1, ScaleType::major, scope) && !c.setProjectScale(0, static_cast<ScaleType>(10), scope), "invalid root and scale refuse");
    c.setExternalBusy(true); require(!c.setProjectScale(9, ScaleType::major, scope), "busy context edit refuses"); c.setExternalBusy(false);
    engine.recording.phase.store(alien::RecordControls::countIn); require(!c.setProjectScale(9, ScaleType::major, scope), "recording count-in context edit refuses"); engine.recording.phase.store(alien::RecordControls::idle);
    require(c.setProjectScale(9, ScaleType::naturalMinor, scope) && roll.getScaleSettings().rootPitchClass == 9, "following editor tracks project key changes");
    c.undo(); require(c.document().musicalScale.rootPitchClass == 2 && roll.getScaleSettings().rootPitchClass == 2, "Undo restores model and following editor");
    const auto redo = c.redoLabel(); require(c.setProjectScale(2, ScaleType::naturalMinor, scope) && c.redoLabel() == redo, "same project context preserves redo");
    c.redo(); require(c.document().musicalScale.rootPitchClass == 9, "Redo restores key change");
    follow->setToggleState(false, juce::dontSendNotification); follow->onClick();
    require(root->isEnabled() && roll.getScaleSettings().rootPitchClass == 2, "local override restores prior editor key");
    require(c.setProjectScale(4, ScaleType::major, scope) && roll.getScaleSettings().rootPitchClass == 2, "local override is unaffected by shared key changes");
    follow->setToggleState(true, juce::dontSendNotification); follow->onClick();
    require(roll.getScaleSettings().rootPitchClass == 4 && roll.getScaleSettings().snap && roll.keyScale(64) == alien::ScaleRole::root, "following applies project root while preserving local guide/snap preferences");
    roll.setPitchView(alien::PitchView::scale); const auto shown = roll.shownPitches();
    require(roll.getPattern().count == 1 && roll.getPattern().notes[0].pitch == 60 && c.document().selectedClip().notes().notes[0].pitch == 60 && edits == 0, "key changes never rewrite existing notes or emit note edits");
    require(std::find(shown.begin(), shown.end(), 60) != shown.end(), "out-of-scale existing notes remain visible when folded");
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienProjectScale", ".alienstudio", false);
    bool done = false, ok = false; c.writeProject(file, false, [&](bool result) { done = true; ok = result; });
    for (int i = 0; i < 400 && !done; ++i) pump(10); require(done && ok, "save project key");
    const auto previousScope = c.document().identityScope; c.openProject(file);
    for (int i = 0; i < 400 && c.isBusy(); ++i) pump(10);
    require(!c.isBusy() && c.document().musicalScale == alien::ProjectMusicalScale{true, 4, ScaleType::major} && roll.getScaleSettings().rootPitchClass == 4, "save/reopen retains shared key and refreshes following editor");
    require(!c.setProjectScale(9, ScaleType::major, previousScope), "old project scope refuses edits after reopen");
    c.onDocumentRestored = {}; c.newProject(); publish->onClick();
    require(!c.document().musicalScale.defined, "stale Scale tool cannot set the key of a replacement project");
    refresh(); require(roll.getScaleSettings().rootPitchClass == 2, "undefined project context falls back to local scale");
    c.onMusicalScaleChanged = {}; file.deleteFile();
}
