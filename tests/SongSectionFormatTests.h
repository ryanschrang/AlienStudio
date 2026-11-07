#pragma once
static void sectionFormatTests() {
    alien::SessionProjectData source, parsed; juce::String error;
    require(source.timeline.addSection(0, 3840, "Intro", 4) != 0 && source.timeline.addSection(7680, 11520, "Chorus", 7) != 0, "section persistence fixture");
    const auto json = alien::serializeSessionProject(source);
    require(!json.isEmpty() && alien::parseSessionProject(json, parsed, error), "sections serialize and parse in format 35");
    require(parsed.timeline.sections.size() == 2 && parsed.timeline.sections[1].name == "Chorus"
        && parsed.timeline.sections[1].start == 7680 && parsed.timeline.sections[1].end == 11520 && parsed.timeline.sections[1].colour == 7, "section names, colours and exclusive ranges roundtrip");
    require(alien::serializeSessionProject(parsed) == json, "section serialization is canonical despite fresh runtime IDs");
    auto root = juce::JSON::parse(json);
    root.getDynamicObject()->setProperty("version", 34);
    root.getDynamicObject()->getProperty("timeline").getDynamicObject()->removeProperty("sections");
    require(alien::parseSessionProject(juce::JSON::toString(root), parsed, error) && parsed.timeline.sections.empty(), "format 34 migrates to empty sections");
    root.getDynamicObject()->setProperty("version", 35);
    require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "format 35 requires section array");
    for (const auto& field : {"start", "end", "name", "colour"}) {
        root = juce::JSON::parse(json); auto& section = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("sections").getArray()->getReference(0);
        section.getDynamicObject()->setProperty(field, juce::String(field) == "name" ? juce::var(12) : juce::var(1.5));
        require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "section fields use strict JSON types");
    }
    for (int invalid : {-1, 256}) {
        root = juce::JSON::parse(json); auto* array = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("sections").getArray();
        array->getReference(0).getDynamicObject()->setProperty("colour", invalid);
        require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "section colour refuses rather than clamping");
    }
    root = juce::JSON::parse(json); auto* array = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("sections").getArray();
    array->getReference(1).getDynamicObject()->setProperty("start", 1);
    require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "overlapping section ranges cannot load");
}
