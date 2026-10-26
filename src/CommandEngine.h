#pragma once
#include "ProjectController.h"
#include <juce_core/juce_core.h>
#include <array>
#include <limits>
#include <optional>
#include <variant>
#include <vector>

// Named, undoable commands (roadmap T1-02). A command has a stable name and
// typed arguments. execute() applies one command as one undo step; batch()
// applies several as one undo step, all or nothing; preview() applies them to
// a copy of the document and reports the result without touching the
// project, the engine or the undo history. Commands run through the
// controller's own operations and commit(), so undo/redo remain the snapshot
// history in DocumentHistory. Tracks are numbered from zero. Message thread only.
namespace alien::command {

struct CreateTrack {};                              // appended after the last track
struct DeleteTrack { int track{-1}; };
struct MoveTrack { int track{-1}, to{-1}; };        // `to` is the track's index afterwards
struct RenameTrack { int track{-1}; juce::String name; };
struct ColorTrack { int track{-1}, colour{-1}; };   // index into the track colour palette

using Command = std::variant<CreateTrack, DeleteTrack, MoveTrack, RenameTrack, ColorTrack>;

namespace detail {
// Indexed like Command's alternatives.
inline constexpr std::array<const char*, 5> names{"CreateTrack", "DeleteTrack", "MoveTrack", "RenameTrack", "ColorTrack"};
inline constexpr std::array<const char*, 5> labels{"Add track", "Delete track", "Move track", "Rename track", "Colour track"};
static_assert(names.size() == std::variant_size_v<Command> && labels.size() == names.size());

// The name the controller stores for `name` (it trims and caps track names).
inline juce::String trackName(const juce::String& name) { return name.trim().substring(0, 64); }

// The document side of ProjectController::moveTrack(track, +-1).
inline void swapTracks(ProjectDocument& d, int a, int b) {
    d.timeline.tracksSwapped(a, b);
    d.channelRack.tracksSwapped(a, b);
    d.sidechainTracksSwapped(a, b);
    std::swap(d.tracks[static_cast<std::size_t>(a)], d.tracks[static_cast<std::size_t>(b)]);
    for (auto& scene : d.clips) std::swap(scene[static_cast<std::size_t>(a)], scene[static_cast<std::size_t>(b)]);
    if (d.selectedTrack == a) d.selectedTrack = b;
    else if (d.selectedTrack == b) d.selectedTrack = a;
}

inline juce::String trackTitle(const ProjectDocument& d, int track) {
    return "track " + juce::String(track + 1) + " \"" + d.tracks[static_cast<std::size_t>(track)].name + "\"";
}
} // namespace detail

// The stable name, e.g. "RenameTrack". Scripts, macros and key bindings refer to commands by it.
inline juce::String name(const Command& command) { return detail::names[command.index()]; }
// What the undo menu shows for the command on its own.
inline juce::String label(const Command& command) { return detail::labels[command.index()]; }

// Why `command` cannot be applied to `d`, or an empty string when it can.
inline juce::String refusal(const ProjectDocument& d, const Command& command) {
    auto track = [&](int t) -> juce::String {
        return t >= 0 && t < d.trackCount() ? juce::String{}
            : "There is no track " + juce::String(t + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    };
    if (std::holds_alternative<CreateTrack>(command))
        return d.canAddTrack() ? juce::String{} : "The project already has the most tracks allowed (" + juce::String(maxSessionTracks) + ").";
    if (const auto* c = std::get_if<DeleteTrack>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return d.trackCount() > 1 ? juce::String{} : "The last track cannot be deleted.";
    }
    if (const auto* c = std::get_if<MoveTrack>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return track(c->to);
    }
    if (const auto* c = std::get_if<RenameTrack>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return detail::trackName(c->name).isNotEmpty() ? juce::String{} : "A track name cannot be blank.";
    }
    const auto& c = std::get<ColorTrack>(command);
    if (const auto why = track(c.track); why.isNotEmpty()) return why;
    return c.colour >= 0 && c.colour < ProjectController::trackColourCount ? juce::String{}
        : "Track colours are numbered 0 to " + juce::String(ProjectController::trackColourCount - 1) + ".";
}

// One line saying what `command` does to `d` (call before applying it).
inline juce::String describe(const ProjectDocument& d, const Command& command) {
    if (std::holds_alternative<CreateTrack>(command)) return "Add \"" + d.nextTrackName() + "\" as track " + juce::String(d.trackCount() + 1);
    if (const auto* c = std::get_if<DeleteTrack>(&command)) return "Delete " + detail::trackTitle(d, c->track);
    if (const auto* c = std::get_if<MoveTrack>(&command)) return "Move " + detail::trackTitle(d, c->track) + " to position " + juce::String(c->to + 1);
    if (const auto* c = std::get_if<RenameTrack>(&command)) return "Rename " + detail::trackTitle(d, c->track) + " to \"" + detail::trackName(c->name) + "\"";
    const auto& c = std::get<ColorTrack>(command);
    return "Colour " + detail::trackTitle(d, c.track) + " with colour " + juce::String(c.colour);
}

// Applies `command` to a document on its own, exactly as the controller's
// operation changes the document (the engine is not involved). Returns the
// refusal, or an empty string when it was applied.
inline juce::String applyTo(ProjectDocument& d, const Command& command) {
    if (const auto why = refusal(d, command); why.isNotEmpty()) return why;
    if (std::holds_alternative<CreateTrack>(command)) {
        d.tracks.push_back({d.nextTrackName(), {}, d.nextTrackColour()});
        for (auto& scene : d.clips) scene.emplace_back();
    } else if (const auto* c = std::get_if<DeleteTrack>(&command)) {
        d.timeline.trackDeleted(c->track);
        d.channelRack.trackDeleted(c->track);
        d.sidechainTrackDeleted(c->track);
        d.tracks.erase(d.tracks.begin() + c->track);
        for (auto& scene : d.clips) scene.erase(scene.begin() + c->track);
        if (d.selectedTrack > c->track) --d.selectedTrack;
        d.clampSelection();
    } else if (const auto* c = std::get_if<MoveTrack>(&command)) {
        const int step = c->to > c->track ? 1 : -1;
        for (int t = c->track; t != c->to; t += step) detail::swapTracks(d, t, t + step);
    } else if (const auto* c = std::get_if<RenameTrack>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].name = detail::trackName(c->name);
    } else {
        const auto& colour = std::get<ColorTrack>(command);
        d.tracks[static_cast<std::size_t>(colour.track)].colour = colour.colour;
    }
    return {};
}

// The command's arguments as an object, e.g. {"track": 2, "name": "Bass"}.
inline juce::var arguments(const Command& command) {
    auto* object = new juce::DynamicObject();
    juce::var result(object);
    if (const auto* c = std::get_if<DeleteTrack>(&command)) object->setProperty("track", c->track);
    else if (const auto* c = std::get_if<MoveTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("to", c->to); }
    else if (const auto* c = std::get_if<RenameTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("name", c->name); }
    else if (const auto* c = std::get_if<ColorTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("colour", c->colour); }
    return result;
}

// Builds a command from its stable name and arguments (as arguments() gives
// them). Unknown names, missing or mistyped arguments and unknown argument
// names are refused with a reason in `error`. Ranges are checked when the
// command is applied, against the document it is applied to.
inline std::optional<Command> parse(const juce::String& commandName, const juce::var& args, juce::String& error) {
    std::size_t index = 0;
    while (index < detail::names.size() && commandName != detail::names[index]) ++index;
    if (index >= detail::names.size()) { error = "Unknown command \"" + commandName + "\"."; return std::nullopt; }
    static const std::array<juce::StringArray, 5> allowed{
        juce::StringArray{}, juce::StringArray{"track"}, juce::StringArray{"track", "to"},
        juce::StringArray{"track", "name"}, juce::StringArray{"track", "colour"}};
    const auto* object = args.getDynamicObject();
    if (object == nullptr && !args.isVoid()) { error = commandName + " takes an object of arguments."; return std::nullopt; }
    if (object != nullptr)
        for (const auto& property : object->getProperties())
            if (!allowed[index].contains(property.name.toString())) {
                error = commandName + " has no \"" + property.name.toString() + "\" argument."; return std::nullopt;
            }
    bool ok = true;
    auto integer = [&](const char* key) {
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && (value.isInt() || value.isInt64())) {
            const auto wide = static_cast<juce::int64>(value);
            if (wide >= std::numeric_limits<int>::min() && wide <= std::numeric_limits<int>::max()) return static_cast<int>(wide);
        }
        if (ok) error = commandName + " needs a whole-number \"" + juce::String(key) + "\" argument.";
        ok = false;
        return -1;
    };
    auto text = [&](const char* key) {
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && value.isString()) return value.toString();
        if (ok) error = commandName + " needs a text \"" + juce::String(key) + "\" argument.";
        ok = false;
        return juce::String{};
    };
    std::optional<Command> command;
    switch (index) {
        case 0: command = CreateTrack{}; break;
        case 1: command = DeleteTrack{integer("track")}; break;
        case 2: { const int track = integer("track"); command = MoveTrack{track, integer("to")}; break; }
        case 3: { const int track = integer("track"); command = RenameTrack{track, text("name")}; break; }
        default: { const int track = integer("track"); command = ColorTrack{track, integer("colour")}; break; }
    }
    if (!ok) return std::nullopt;
    return command;
}

struct Result {
    bool ok{};
    juce::String error;     // why nothing was done (empty when ok)
    int failedCommand{-1};  // index of the command that was refused
    bool changed{};         // the commands changed the document (an undo step was recorded)
};

struct Preview : Result {
    ProjectDocument after;      // the document as the commands would leave it
    juce::StringArray changes;  // one line per command, in order
};

class CommandEngine {
public:
    explicit CommandEngine(ProjectController& projectController) : controller(projectController) {}

    // Applies the commands to a copy of the document. The project, the engine and the undo history are untouched.
    Preview preview(const std::vector<Command>& commands) const {
        Preview p;
        p.after = controller.document();
        if (commands.empty()) { p.error = "There is nothing to do."; return p; }
        for (std::size_t i = 0; i < commands.size(); ++i) {
            if (const auto why = refusal(p.after, commands[i]); why.isNotEmpty()) {
                p.error = why; p.failedCommand = static_cast<int>(i); p.changes.clear();
                p.after = controller.document();
                return p;
            }
            p.changes.add(describe(p.after, commands[i]));
            applyTo(p.after, commands[i]);
        }
        p.ok = true;
        p.changed = !sameContent(p.after, controller.document());
        return p;
    }
    Preview preview(const Command& command) const { return preview(std::vector<Command>{command}); }

    // One command, one undo step.
    Result execute(const Command& command) { return batch({command}, label(command)); }

    // Every command, in order, as one undo step named `undoLabel`. If any command
    // would be refused, nothing changes: not the document, the engine or the history.
    Result batch(const std::vector<Command>& commands, const juce::String& undoLabel) {
        if (controller.isBusy()) return refused("The project is busy; try again when it has finished.");
        Result result = preview(commands);
        if (!result.ok || !result.changed) return result;
        const ProjectDocument before = controller.document();
        for (std::size_t i = 0; i < commands.size(); ++i)
            if (!run(commands[i])) {
                // The preview accepted it, so the controller should have too. Put everything back.
                controller.revertTo(before);
                Result failed = refused("The project could not " + label(commands[i]).toLowerCase() + ".");
                failed.failedCommand = static_cast<int>(i);
                return failed;
            }
        controller.commit(undoLabel.isNotEmpty() ? undoLabel : juce::String("Edit tracks"));
        return result;
    }

    bool undo() { return controller.undo(); }
    bool redo() { return controller.redo(); }

private:
    static Result refused(const juce::String& why) { Result r; r.error = why; return r; }

    bool run(const Command& command) {
        if (std::holds_alternative<CreateTrack>(command)) return controller.addTrack();
        if (const auto* c = std::get_if<DeleteTrack>(&command)) return controller.deleteTrack(c->track);
        if (const auto* c = std::get_if<MoveTrack>(&command)) {
            const int step = c->to > c->track ? 1 : -1;
            for (int t = c->track; t != c->to; t += step) if (!controller.moveTrack(t, step)) return false;
            return true;
        }
        if (const auto* c = std::get_if<RenameTrack>(&command)) return controller.renameTrack(c->track, c->name);
        const auto& c = std::get<ColorTrack>(command);
        return controller.setTrackColour(c.track, c.colour);
    }

    ProjectController& controller;
};

} // namespace alien::command
