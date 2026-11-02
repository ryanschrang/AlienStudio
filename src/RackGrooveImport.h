#pragma once
// Imports a General MIDI drum groove into the Channel Rack (BROWSE-01) as a new pattern named after the file,
// so nothing the rack already holds is overwritten.
//
// Each GM drum key belongs to a role (kick, snare, hat, ...). A role plays on the first channel whose sound
// is that drum: a built-in sound with that id, or a sample or channel name that says so (a Starter Pack
// "Kicks" sample, a channel called "Ride"). With no such channel a new one is added with the built-in sound,
// and roles the built-in kit lacks fold into the nearest one it has: cymbals onto a "Cymbal" channel using
// the open hat, percussion onto the rim shot. Synth and plugin channels never receive hits.
//
// Only notes on MIDI channel 10, the GM drum channel, are hits; notes on other channels are left out.
// Hits become steps, so they show and edit on the step grid. A hit lands on the nearest sixteenth; one played
// late keeps its feel as the step's nudge (percent of a step later), one played early lands on the beat. Two
// hits on one channel's step keep the louder. Velocity becomes step velocity. The pattern is the smallest
// rack length that holds the groove (16 to 128 steps); hits past 128 steps are left out.
// Message thread only.
#include "MidiClipImport.h"
#include "RackModel.h"
#include "RackSounds.h"
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace alien::rack_groove {

enum class Role { kick, snare, rim, clap, closedHat, openHat, tom, cymbal, cowbell, percussion };
inline constexpr int roleCount = 10;

// The GM drum map, plus the hi-hat edge keys (22 closed, 26 open) that the Groove MIDI Dataset uses.
inline Role roleOf(int key) noexcept {
    switch (key) {
    case 35: case 36: return Role::kick;
    case 38: case 40: return Role::snare;
    case 37: return Role::rim;
    case 39: return Role::clap;
    case 22: case 42: case 44: return Role::closedHat;
    case 26: case 46: return Role::openHat;
    case 41: case 43: case 45: case 47: case 48: case 50: case 58: return Role::tom;
    case 49: case 51: case 52: case 53: case 55: case 57: case 59: return Role::cymbal;
    case 56: return Role::cowbell;
    default: return Role::percussion;
    }
}

struct RoleSound { const char* builtin; const char* name; };
inline const RoleSound& soundOf(Role role) noexcept {
    static const std::array<RoleSound, roleCount> sounds{{
        {"kick", "Kick"}, {"snare", "Snare"}, {"rim", "Rim shot"}, {"clap", "Clap"}, {"hat", "Closed hat"},
        {"openhat", "Open hat"}, {"tom", "Tom"}, {"openhat", "Cymbal"}, {"cowbell", "Cowbell"}, {"rim", "Percussion"},
    }};
    return sounds[static_cast<std::size_t>(role)];
}

inline std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The drum a channel plays, or -1 for a synth, plugin, silent channel or a sound it cannot tell. A Starter Pack
// sample's folder says its kind ("Drums/Kicks/Kick Snares On.wav" is a kick); otherwise a built-in sound's id,
// otherwise the first word of the channel's name or file that names a drum, most specific first.
inline int classify(const rack::Project& p, int channel) {
    const auto& info = p.info[static_cast<std::size_t>(channel)];
    if (p.channels[static_cast<std::size_t>(channel)].isInstrument() || info.sample.empty()) return -1;
    const auto name = lower(info.name);
    const auto has = [](const std::string& text, const char* word) { return text.find(word) != std::string::npos; };
    const auto is = [](Role role) { return static_cast<int>(role); };
    if (info.isBuiltin()) {
        // "Cymbal" and "Percussion" channels borrow another role's sound, so their name says what they are.
        if (has(name, "cymbal")) return is(Role::cymbal);
        if (has(name, "percussion")) return is(Role::percussion);
        for (int role = 0; role < roleCount; ++role)
            if (info.builtinId() == soundOf(static_cast<Role>(role)).builtin) return role;
        return -1;
    }
    const juce::File file(juce::String::fromUTF8(info.sample.c_str()));
    const auto stem = lower(file.getFileNameWithoutExtension().toStdString());
    const auto folder = lower(file.getParentDirectory().getFileName().toStdString());
    if (folder == "kicks") return is(Role::kick);
    if (folder == "snares") return is(Role::snare);
    if (folder == "claps") return is(Role::clap);
    if (folder == "hats") return has(stem, "open") ? is(Role::openHat) : is(Role::closedHat);
    if (folder == "toms") return is(Role::tom);
    if (folder == "cymbals") return is(Role::cymbal);
    if (folder == "percussion") return has(stem, "cowbell") ? is(Role::cowbell) : is(Role::percussion);
    for (const auto& text : {name, stem}) {
        if (has(text, "kick")) return is(Role::kick);
        if (has(text, "clap")) return is(Role::clap);
        if (has(text, "hat") || has(text, "hh")) return has(text, "open") ? is(Role::openHat) : is(Role::closedHat);
        if (has(text, "snare")) return is(Role::snare);
        if (has(text, "rim") || has(text, "stick")) return is(Role::rim);
        if (has(text, "tom")) return is(Role::tom);
        if (has(text, "cymbal") || has(text, "crash") || has(text, "ride") || has(text, "splash")) return is(Role::cymbal);
        if (has(text, "cowbell")) return is(Role::cowbell);
        if (has(text, "perc") || has(text, "conga") || has(text, "bongo") || has(text, "shaker")) return is(Role::percussion);
    }
    return -1;
}

inline bool hasDrums(const midi_import::ImportedClip& clip) {
    return std::any_of(clip.notes.begin(), clip.notes.end(), [](const auto& n) { return n.channel == midi_import::gmDrumChannel; });
}

struct Report {
    int pattern{-1};
    int hits{}, merged{}, dropped{}, otherChannels{};
    std::vector<std::string> added;     // names of channels made for the groove
    std::array<int, roleCount> channelOf{};   // -1 where the groove does not use the role
};

// Adds the groove to `p` as a new selected pattern named `name`. False (and `p` unusable) when there is no
// room for a pattern, the clip has no notes, or every channel slot is taken.
inline bool importGroove(rack::Project& p, const midi_import::ImportedClip& clip, const std::string& name, Report* report = nullptr) {
    Report r;
    r.channelOf.fill(-1);
    if (!clip.ok() || !hasDrums(clip) || !p.addPattern()) return false;
    const int pattern = p.selectedPattern;
    r.pattern = pattern;
    if (!name.empty()) p.renamePattern(pattern, name);
    const int steps = (clip.lengthTicks + PianoRollPattern::ticksPerStep - 1) / PianoRollPattern::ticksPerStep;
    int length = 16;
    while (length < steps && length < rack::maxSteps) length *= 2;
    if (length != p.pattern(pattern).length && !p.setPatternLength(pattern, length)) return false;

    std::array<bool, roleCount> used{};
    for (const auto& note : clip.notes)
        if (note.channel == midi_import::gmDrumChannel) used[static_cast<std::size_t>(roleOf(note.pitch))] = true;
    for (int role = 0; role < roleCount; ++role) {
        if (!used[static_cast<std::size_t>(role)]) continue;
        auto& channel = r.channelOf[static_cast<std::size_t>(role)];
        for (int c = 0; c < p.channelCount && channel < 0; ++c) if (classify(p, c) == role) channel = c;
        if (channel >= 0) continue;
        const auto& sound = soundOf(static_cast<Role>(role));
        if (!p.addChannel(sound.name, std::string("builtin:") + sound.builtin)) return false;
        channel = p.channelCount - 1;
        r.added.emplace_back(sound.name);
    }

    std::array<rack::Project::StepRow, rack::maxChannels> rows{};
    std::array<bool, rack::maxChannels> touched{};
    for (const auto& note : clip.notes) {
        if (note.channel != midi_import::gmDrumChannel) { ++r.otherChannels; continue; }
        const int channel = r.channelOf[static_cast<std::size_t>(roleOf(note.pitch))];
        const double exact = PianoRollPattern::fromTicks(note.startTicks);
        const int step = static_cast<int>(std::lround(exact));
        // An early hit just before the loop's end belongs on its first beat; anything later is past the end.
        if (step > length || (step == length && steps > length)) { ++r.dropped; continue; }
        const int slot = step % length;
        if (!touched[static_cast<std::size_t>(channel)]) {
            rows[static_cast<std::size_t>(channel)] = p.pattern(pattern).steps[static_cast<std::size_t>(channel)];
            touched[static_cast<std::size_t>(channel)] = true;
        }
        auto& s = rows[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        const int velocity = std::clamp(static_cast<int>(std::lround(note.velocity * 127.0f)), 1, 127);
        if (s.on) { ++r.merged; if (velocity <= s.velocity) continue; } else ++r.hits;
        s = rack::Step{};
        s.on = true;
        s.velocity = static_cast<std::uint8_t>(velocity);
        s.nudge = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround((exact - step) * 100.0)), 0, rack::maxNudge));
    }
    for (int c = 0; c < p.channelCount; ++c)
        if (touched[static_cast<std::size_t>(c)] && !p.pasteRow(pattern, c, rows[static_cast<std::size_t>(c)])) return false;
    const bool any = r.hits != 0;
    if (report != nullptr) *report = std::move(r);
    return any;
}

inline juce::String describe(const rack::Project& p, const Report& r, const juce::String& name, const midi_import::ImportedClip& clip) {
    juce::String text = "Imported " + name + " as pattern " + juce::String(r.pattern + 1) + ": " + juce::String(r.hits) + " hits";
    juce::StringArray onto;
    for (int role = 0; role < roleCount; ++role) {
        const int c = r.channelOf[static_cast<std::size_t>(role)];
        if (c >= 0 && c < p.channelCount) onto.addIfNotAlreadyThere(juce::String::fromUTF8(p.info[static_cast<std::size_t>(c)].name.c_str()));
    }
    text << " on " << onto.joinIntoString(", ");
    if (!r.added.empty()) text << " (" << static_cast<int>(r.added.size()) << " channel" << (r.added.size() == 1 ? "" : "s") << " added)";
    if (r.merged != 0) text << "; " << r.merged << " hits sharing a step kept the louder";
    if (r.dropped != 0) text << "; " << r.dropped << " past 128 steps were left out";
    if (r.otherChannels != 0) text << "; " << r.otherChannels << " notes not on the drum channel were left out";
    if (clip.fileTempo > 0.0) text << ". The file's " << juce::roundToInt(clip.fileTempo) << " BPM is not applied";
    return text + ".";
}

} // namespace alien::rack_groove
