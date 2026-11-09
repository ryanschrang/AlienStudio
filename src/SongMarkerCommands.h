#pragma once
#include "ProjectDocument.h"
#include <algorithm>
#include <cstdint>
#include <string>

// Pure document edits for the Song's markers, loop range and length. The controller and command
// Preview share them, so what Preview predicts is what the live edit does.
namespace alien::song_edit {
inline constexpr int markerColours = 8;

enum class Kind { addMarker, moveMarker, renameMarker, colourMarker, removeMarker, setLoop, setLength };
struct Edit {
    Kind kind{Kind::addMarker};
    std::uint32_t marker{};          // the marker's id (a new marker's id for addMarker)
    std::int64_t tick{}, end{};      // a marker's tick; the loop runs from `tick` to `end`
    std::string name;                // addMarker, renameMarker
    int colour{};                    // addMarker, colourMarker
    bool enabled{};                  // setLoop
    int bars{};                      // setLength
};

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
    case Kind::setLength: return song.lengthBars == e.bars;
    case Kind::addMarker: case Kind::removeMarker: break;
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
