#pragma once
#include "ProjectDocument.h"
#include <algorithm>
#include <cstdint>
#include <string>

// Pure document edits for the Song's markers, loop range, length, sections and chords. The controller and
// command Preview share them, so what Preview predicts is what the live edit does. None changes what
// plays except the loop and the length.
namespace alien::song_edit {
inline constexpr int markerColours = 8;

enum class Kind { addMarker, moveMarker, renameMarker, colourMarker, removeMarker, setLoop,
                  addSection, editSection, removeSection, addChord, editChord, removeChord, setLength };
struct Edit {
    Kind kind{Kind::addMarker};
    std::uint32_t marker{};          // the marker's, section's or chord's id (a new one's id for the adds)
    std::int64_t tick{}, end{};      // a marker's tick; the loop, a section or a chord runs from `tick` to `end`
    std::string name;                // addMarker, renameMarker, sections
    int colour{};                    // addMarker, colourMarker, sections
    bool enabled{};                  // setLoop
    int root{};                      // chords: 0 (C) to 11 (B)
    ChordType quality{};             // chords
    int bars{};                      // setLength
};

inline bool isSection(Kind k) noexcept { return k == Kind::addSection || k == Kind::editSection || k == Kind::removeSection; }
inline bool isChord(Kind k) noexcept { return k == Kind::addChord || k == Kind::editChord || k == Kind::removeChord; }
inline timeline::Section sectionOf(const Edit& e) {
    return {e.marker, e.tick, e.end, timeline::Timeline::markerName(e.name), static_cast<std::uint8_t>(e.colour)};
}
inline timeline::SongChord chordOf(const Edit& e) { return {e.marker, e.tick, e.end, e.root, e.quality}; }

inline bool changesLoop(const Edit& e) noexcept { return e.kind == Kind::setLoop; }
inline bool changesLength(const Edit& e) noexcept { return e.kind == Kind::setLength; }

// True when the edit would leave the document as it is. Only meaningful for an edit with valid arguments.
inline bool changesNothing(const ProjectDocument& d, const Edit& e) {
    const auto& song = d.timeline;
    const auto* m = song.findMarker(e.marker);
    switch (e.kind) {
    case Kind::moveMarker: return m != nullptr && m->tick == e.tick;
    case Kind::renameMarker: return m != nullptr && m->name == timeline::Timeline::markerName(e.name);
    case Kind::colourMarker: return m != nullptr && m->colour == e.colour;
    case Kind::setLoop: return song.loopStart == e.tick && song.loopEnd == e.end && song.loopEnabled == e.enabled;
    case Kind::editSection: { const auto* s = song.findSection(e.marker); return s != nullptr && *s == sectionOf(e); }
    case Kind::editChord: { const auto* c = song.findChord(e.marker); return c != nullptr && *c == chordOf(e); }
    case Kind::setLength: return song.lengthBars == e.bars;
    case Kind::addMarker: case Kind::removeMarker: case Kind::addSection: case Kind::removeSection: case Kind::addChord: case Kind::removeChord: break;
    }
    return false;
}

inline juce::String refusal(const ProjectDocument& d, const Edit& e) {
    const auto& song = d.timeline;
    const auto inside = [&](std::int64_t tick) { return tick >= 0 && tick <= song.length(); };
    const auto named = [&] { return !timeline::Timeline::markerName(e.name).empty(); };
    const auto colourOk = [&] { return e.colour >= 0 && e.colour < markerColours; };
    if (e.kind == Kind::addMarker) {
        if (e.marker == 0 || song.findMarker(e.marker) != nullptr) return "That marker is already in the song.";
        if (song.markers.size() >= timeline::maxMarkers) return "The song already has " + juce::String(static_cast<int>(timeline::maxMarkers)) + " markers, the most it can hold.";
        if (!inside(e.tick)) return "A marker must be inside the song.";
        if (!named()) return "A marker needs a name.";
        if (!colourOk()) return "A marker colour must be from 0 to 7.";
        return {};
    }
    if (e.kind == Kind::setLoop) {
        if (e.tick < 0 || e.end > song.length() || e.end - e.tick < timeline::ticksPerStep)
            return "The loop must be inside the song and at least one step long.";
        return {};
    }
    if (e.kind == Kind::setLength) {
        if (e.bars < 1 || e.bars > timeline::maxBars) return "A song is from 1 to " + juce::String(timeline::maxBars) + " bars long.";
        if (const int shortest = song.minLengthBars(); e.bars < shortest)
            return "The song cannot be shorter than " + juce::String(shortest) + (shortest == 1 ? " bar" : " bars") + ": its clips, markers or changes reach that far.";
        return {};
    }
    const auto ranged = [&] { return e.tick >= 0 && e.end <= song.length() && e.end - e.tick >= timeline::ticksPerStep; };
    const auto id = juce::String(static_cast<juce::int64>(e.marker));
    if (isSection(e.kind)) {
        if (e.kind == Kind::addSection) {
            if (e.marker == 0 || song.findSection(e.marker) != nullptr) return "That section is already in the song.";
            if (song.sections.size() >= timeline::maxSections) return "The song already has " + juce::String(static_cast<int>(timeline::maxSections)) + " sections, the most it can hold.";
        } else if (song.findSection(e.marker) == nullptr) return "There is no section " + id + " in the song.";
        if (e.kind == Kind::removeSection) return {};
        if (!ranged()) return "A section must be inside the song and at least one step long.";
        if (!named()) return "A section needs a name.";
        if (!colourOk()) return "A section colour must be from 0 to 7.";
        for (const auto& s : song.sections) if (s.id != e.marker && e.tick < s.end && e.end > s.start) return "Sections cannot overlap.";
        return {};
    }
    if (isChord(e.kind)) {
        if (e.kind == Kind::addChord) {
            if (e.marker == 0 || song.findChord(e.marker) != nullptr) return "That chord is already in the song.";
            if (song.chords.size() >= timeline::maxChords) return "The song already has " + juce::String(static_cast<int>(timeline::maxChords)) + " chords, the most it can hold.";
        } else if (song.findChord(e.marker) == nullptr) return "There is no chord " + id + " in the song.";
        if (e.kind == Kind::removeChord) return {};
        if (!ranged()) return "A chord must be inside the song and at least one step long.";
        if (!timeline::validChordRoot(e.root)) return "A chord root must be from 0 (C) to 11 (B).";
        if (!timeline::validChordQuality(e.quality)) return "That is not a chord quality.";
        for (const auto& c : song.chords) if (c.id != e.marker && e.tick < c.end && e.end > c.start) return "Chords cannot overlap.";
        return {};
    }
    if (song.findMarker(e.marker) == nullptr) return "There is no marker " + juce::String(static_cast<juce::int64>(e.marker)) + " in the song.";
    if (e.kind == Kind::moveMarker && !inside(e.tick)) return "A marker must be inside the song.";
    if (e.kind == Kind::renameMarker && !named()) return "A marker needs a name.";
    if (e.kind == Kind::colourMarker && !colourOk()) return "A marker colour must be from 0 to 7.";
    return {};
}

inline juce::String apply(ProjectDocument& d, const Edit& e) {
    if (const auto why = refusal(d, e); why.isNotEmpty()) return why;
    if (changesNothing(d, e)) return {};
    auto& song = d.timeline;
    bool done = false;
    switch (e.kind) {
    case Kind::addMarker: done = song.addMarkerWithId(e.marker, e.tick, e.name, static_cast<std::uint8_t>(e.colour)); break;
    case Kind::moveMarker: done = song.moveMarker(e.marker, e.tick); break;
    case Kind::renameMarker: done = song.renameMarker(e.marker, e.name); break;
    case Kind::colourMarker: done = song.colourMarker(e.marker, static_cast<std::uint8_t>(e.colour)); break;
    case Kind::removeMarker: done = song.removeMarker(e.marker); break;
    case Kind::addSection: done = song.addSectionWithId(e.marker, e.tick, e.end, e.name, static_cast<std::uint8_t>(e.colour)); break;
    case Kind::editSection: done = song.setSection(sectionOf(e)); break;
    case Kind::removeSection: done = song.removeSection(e.marker); break;
    case Kind::addChord: done = song.addChordWithId(e.marker, e.tick, e.end, e.root, e.quality); break;
    case Kind::editChord: done = song.setChord(chordOf(e)); break;
    case Kind::removeChord: done = song.removeChord(e.marker); break;
    case Kind::setLoop: {
        const bool range = song.setLoop(e.tick, e.end);
        const bool enabled = song.setLoopEnabled(e.enabled);
        done = range || enabled;
        break;
    }
    case Kind::setLength: {
        // A shorter song pulls the loop in with it. A frozen track's render keeps the length it was made for.
        done = song.setLengthBars(e.bars);
        const bool frozen = std::any_of(d.tracks.begin(), d.tracks.end(), [](const auto& t) { return t.settings.frozen(); });
        if (done && frozen) song.freezeBaseLengthBars = song.lengthBars;
        break;
    }
    }
    return done ? juce::String{} : juce::String("The song could not take that edit.");
}
} // namespace alien::song_edit
