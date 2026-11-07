#pragma once
static void chordFormatTests() {
    alien::SessionProjectData source, parsed; juce::String error;
    require(source.timeline.addChord(0, 3840, 6, alien::ChordType::minor7) != 0, "chord persistence fixture");
    require(source.timeline.addSection(0, 7680, "Verse") != 0, "sections coexist independently with chord ranges");
    const auto json = alien::serializeSessionProject(source);
    require(!json.isEmpty() && alien::parseSessionProject(json, parsed, error), "format 36 chord track serializes and parses");
    require(parsed.timeline.chords.size() == 1 && parsed.timeline.chords[0].root == 6 && parsed.timeline.chords[0].quality == alien::ChordType::minor7
        && parsed.timeline.chords[0].start == 0 && parsed.timeline.chords[0].end == 3840, "chord root, quality and exclusive range roundtrip");
    require(alien::serializeSessionProject(parsed) == json, "chord serialization is canonical with fresh runtime identities");
    auto root = juce::JSON::parse(json); root.getDynamicObject()->setProperty("version", 35);
    root.getDynamicObject()->getProperty("timeline").getDynamicObject()->removeProperty("chords");
    require(alien::parseSessionProject(juce::JSON::toString(root), parsed, error) && parsed.timeline.chords.empty()
        && parsed.timeline.sections.size() == 1, "format 35 migrates to empty chord track while retaining sections");
    root.getDynamicObject()->setProperty("version", 36);
    require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "format 36 requires chord array");
    for (const auto& field : {"start", "end", "root", "quality"}) {
        root = juce::JSON::parse(json);
        auto* array = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("chords").getArray();
        array->getReference(0).getDynamicObject()->setProperty(field, 1.5);
        require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "chord fields use strict integer types");
    }
    for (const auto& field : {"root", "quality", "start", "end"}) {
        root = juce::JSON::parse(json);
        auto* array = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("chords").getArray();
        array->getReference(0).getDynamicObject()->setProperty(field, juce::String(field) == "root" ? 12 : juce::String(field) == "quality" ? 9 : -1);
        require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "invalid chord root, quality or range refuses");
    }
    root = juce::JSON::parse(json); auto* array = root.getDynamicObject()->getProperty("timeline").getDynamicObject()->getProperty("chords").getArray();
    auto overlapping = juce::var(new juce::DynamicObject()); auto* item = overlapping.getDynamicObject();
    item->setProperty("start", 240); item->setProperty("end", 4000); item->setProperty("root", 0); item->setProperty("quality", 0); array->add(overlapping);
    require(!alien::parseSessionProject(juce::JSON::toString(root), parsed, error), "overlapping chord ranges cannot load");
}
