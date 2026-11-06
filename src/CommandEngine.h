#pragma once
#include "ProjectController.h"
#include "SceneCommands.h"
#include "DeviceCommands.h"
#include "TempoCommands.h"
#include <juce_core/juce_core.h>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
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
struct SetMasterVolume { double gain{1.0}; }; // linear master gain, 0..maxMasterGain
struct SetTrackVolume { int track{-1}; double gain{1.0}; }; // linear gain, 0..2 (1 is unity)
struct SetTrackPan { int track{-1}; double pan{}; };         // -1 left, 0 centre, 1 right
struct SetTrackMute { int track{-1}; bool muted{}; };
struct SetTrackSolo { int track{-1}; bool soloed{}; }; // additive: other tracks keep their solo state

// Sends and returns, addressed by return slot (0 is send A). Slots never move.
struct SetTrackSend { int track{-1}, returnSlot{-1}; double level{}; }; // linear level, 0..1
struct SetTrackSendMode { int track{-1}, returnSlot{-1}; bool preFader{}; };
struct CreateReturn {};                                        // takes the first free slot
struct RenameReturn { int returnSlot{-1}; juce::String name; };
struct DeleteReturn { int returnSlot{-1}; };                   // also drops its sends, automation and FX
struct SetReturnVolume { int returnSlot{-1}; double gain{1.0}; }; // linear gain, 0..2 (1 is unity)
struct SetReturnPan { int returnSlot{-1}; double pan{}; };        // -1 left, 0 centre, 1 right
struct SetReturnMute { int returnSlot{-1}; bool muted{}; };

// Group buses, addressed by bus slot. Slots never move.
struct CreateBus {};                                 // takes the first free slot
struct RenameBus { int bus{-1}; juce::String name; };
struct DeleteBus { int bus{-1}; };                   // its tracks go to the master; drops its FX
struct SetBusVolume { int bus{-1}; double gain{1.0}; }; // linear gain, 0..2 (1 is unity)
struct SetBusPan { int bus{-1}; double pan{}; };        // -1 left, 0 centre, 1 right
struct SetBusMute { int bus{-1}; bool muted{}; };
struct SetBusSolo { int bus{-1}; bool soloed{}; };

// Routing. A track's bus is also its group: there is no separate membership to set.
struct SetTrackOutput { int track{-1}, bus{-1}; };   // -1 is the master
// What keys an effect's sidechain input. `rack` is a track, return slot or bus slot (0 for
// the master); `source` is a track, bus slot or return slot, or -1 for no key.
struct SetSidechain { RackStage stage{RackStage::track}; int rack{-1}, effect{-1}; KeySource from{KeySource::track}; int source{-1}; };
// The devices in a rack. A slot of -1 is a track's instrument; other slots are FX slots.
struct InsertDevice { RackStage stage{RackStage::track}; int rack{-1}, slot{-1}; PluginState device; };   // an instrument replaces the one there
struct RemoveDevice { RackStage stage{RackStage::track}; int rack{-1}, slot{-1}; };
struct MoveDevice { RackStage stage{RackStage::track}; int rack{-1}, slot{-1}, to{-1}; };
struct SetDeviceBypass { RackStage stage{RackStage::track}; int rack{-1}, slot{-1}; bool bypassed{}; };

// Session cells, zero-based. CreateClip is MIDI-only; duplication supports all types.
struct CreateClip { int track{-1}, scene{-1}; };
struct DuplicateClip { int track{-1}, scene{-1}, toTrack{-1}, toScene{-1}; };
struct MakeClipUnique { int track{-1}, scene{-1}; };

struct RemoveClipAudio { int track{-1}, scene{-1}; };
// Installs audio that has already been decoded, at the destination captured before
// decoding (see importAudioFile below). It does no file I/O, so Preview and Batch are
// pure; a decoded clip cannot be written as arguments, so parse() refuses this one.
struct ImportClipAudio { std::shared_ptr<const session_audio_import::Prepared> prepared; };
struct CreateRackPattern {};
struct AssignRackClip { int track{-1}, scene{-1}; std::uint64_t clip{}; };
struct MoveClip { int track{-1}, scene{-1}, toTrack{-1}, toScene{-1}; };
struct PasteClip { int track{-1}, scene{-1}; };
struct PasteLinkedClip { int track{-1}, scene{-1}; };
struct DeleteClip { int track{-1}, scene{-1}; }; // remove one Session cell, any clip type

struct CreateScene {};
struct DuplicateScene { int scene{-1}; };
struct MoveScene { int scene{-1}, to{-1}; };
struct RenameScene { int scene{-1}; juce::String name; };
struct DeleteScene { int scene{-1}; };

// The project tempo, and the Song's tempo and time signature changes. Tempo edits unfreeze frozen tracks.
struct SetTempo { int bpm{}; };
struct SetTempoChange { std::int64_t tick{}; double bpm{}; };
struct MoveTempoChange { std::int64_t tick{}, to{}; };
struct RemoveTempoChange { std::int64_t tick{}; };
struct ClearTempoChanges {};
struct SetMeterChange { int bar{}, numerator{}, denominator{}; };   // bars count from 0
struct RemoveMeterChange { int bar{}; };
struct ClearMeterChanges {};

using Command = std::variant<CreateTrack, DeleteTrack, MoveTrack, RenameTrack, ColorTrack, SetTrackVolume, SetTrackPan, SetTrackMute, SetTrackSolo, CreateClip, DuplicateClip, MakeClipUnique, CreateScene, DuplicateScene, MoveScene, RenameScene, DeleteScene, DeleteClip, PasteClip, PasteLinkedClip, MoveClip, CreateRackPattern, AssignRackClip, RemoveClipAudio, SetMasterVolume, ImportClipAudio, SetTrackSend, SetTrackSendMode, CreateReturn, RenameReturn, DeleteReturn, SetReturnVolume, SetReturnPan, SetReturnMute, CreateBus, RenameBus, DeleteBus, SetBusVolume, SetBusPan, SetBusMute, SetBusSolo, SetTrackOutput, SetSidechain, InsertDevice, RemoveDevice, MoveDevice, SetDeviceBypass, SetTempo, SetTempoChange, MoveTempoChange, RemoveTempoChange, ClearTempoChanges, SetMeterChange, RemoveMeterChange, ClearMeterChanges>;

namespace detail {
// Indexed like Command's alternatives.
inline constexpr std::array<const char*, 55> names{"CreateTrack", "DeleteTrack", "MoveTrack", "RenameTrack", "ColorTrack", "SetTrackVolume", "SetTrackPan", "SetTrackMute", "SetTrackSolo", "CreateClip", "DuplicateClip", "MakeClipUnique", "CreateScene", "DuplicateScene", "MoveScene", "RenameScene", "DeleteScene", "DeleteClip", "PasteClip", "PasteLinkedClip", "MoveClip", "CreateRackPattern", "AssignRackClip", "RemoveClipAudio", "SetMasterVolume", "ImportClipAudio", "SetTrackSend", "SetTrackSendMode", "CreateReturn", "RenameReturn", "DeleteReturn", "SetReturnVolume", "SetReturnPan", "SetReturnMute", "CreateBus", "RenameBus", "DeleteBus", "SetBusVolume", "SetBusPan", "SetBusMute", "SetBusSolo", "SetTrackOutput", "SetSidechain", "InsertDevice", "RemoveDevice", "MoveDevice", "SetDeviceBypass", "SetTempo", "SetTempoChange", "MoveTempoChange", "RemoveTempoChange", "ClearTempoChanges", "SetMeterChange", "RemoveMeterChange", "ClearMeterChanges"};
inline constexpr std::array<const char*, 55> labels{"Add track", "Delete track", "Move track", "Rename track", "Colour track", "Set track volume", "Set track pan", "Set track mute", "Set track solo", "Create MIDI clip", "Duplicate clip", "Make clip independent", "Add scene", "Duplicate scene", "Move scene", "Rename scene", "Delete scene", "Clear clip", "Paste clip", "Paste linked clip", "Move clip", "New pattern", "Assign rack clip", "Remove audio", "Set master volume", "Import audio", "Set send level", "Set send mode", "Add return", "Rename return", "Remove return", "Set return volume", "Set return pan", "Set return mute", "Add bus", "Rename bus", "Delete bus", "Set bus volume", "Set bus pan", "Set bus mute", "Set bus solo", "Set track output", "Set sidechain", "Insert device", "Remove device", "Move device", "Set device bypass", "Change tempo", "Tempo change", "Move tempo change", "Remove tempo change", "Remove tempo changes", "Time signature change", "Remove time signature change", "Remove time signature changes"};
static_assert(names.size() == std::variant_size_v<Command> && labels.size() == names.size());

// The name the controller stores for `name` (it trims and caps track names).
inline juce::String trackName(const juce::String& name) { return name.trim().substring(0, 64); }

// The document side of ProjectController::moveTrack(track, +-1).
inline void swapTracks(ProjectDocument& d, int a, int b) {
    d.timeline.tracksSwapped(a, b);
    d.channelRack.tracksSwapped(a, b);
    d.sidechainTracksSwapped(a, b);
    std::swap(d.tracks[static_cast<std::size_t>(a)], d.tracks[static_cast<std::size_t>(b)]);
    for (auto& scene : d.cells) std::swap(scene[static_cast<std::size_t>(a)], scene[static_cast<std::size_t>(b)]);
    if (d.selectedTrack == a) d.selectedTrack = b;
    else if (d.selectedTrack == b) d.selectedTrack = a;
}

inline juce::String trackTitle(const ProjectDocument& d, int track) {
    return "track " + juce::String(track + 1) + " \"" + d.tracks[static_cast<std::size_t>(track)].name + "\"";
}

inline const char* stageName(RackStage stage) noexcept {
    return stage == RackStage::master ? "master" : stage == RackStage::bus ? "bus" : stage == RackStage::returnTrack ? "return" : "track";
}
inline std::optional<RackStage> stageNamed(const juce::String& name) {
    for (const auto stage : {RackStage::track, RackStage::bus, RackStage::returnTrack, RackStage::master}) if (name == stageName(stage)) return stage;
    return {};
}
inline std::optional<KeySource> keySourceNamed(const juce::String& name) {
    for (const auto from : {KeySource::track, KeySource::trackPostFader, KeySource::bus, KeySource::returnTrack}) if (name == keySourceName(from)) return from;
    return {};
}
inline juce::String rackTitle(const ProjectDocument& d, RackStage stage, int rack) {
    if (stage == RackStage::master) return "the master";
    if (stage == RackStage::track) return trackTitle(d, rack);
    return stage == RackStage::bus ? "bus \"" + d.buses[static_cast<std::size_t>(rack)].name + "\""
                                   : "return \"" + d.returns[static_cast<std::size_t>(rack)].name + "\"";
}
inline juce::String rackTitle(const ProjectDocument& d, const SetSidechain& c) { return rackTitle(d, c.stage, c.rack); }
inline juce::String deviceTitle(const ProjectDocument& d, RackStage stage, int rack, int slot) {
    const auto* found = sidechain_edit::rackOf(d, stage, rack);
    const auto& device = slot == device_edit::instrumentSlot ? found->instrument : found->effects[static_cast<std::size_t>(slot)];
    return pluginDisplayName(device) + (slot == device_edit::instrumentSlot ? " (instrument)" : " (FX " + juce::String(slot + 1) + ")") + " on " + rackTitle(d, stage, rack);
}
inline juce::String keyTitle(const ProjectDocument& d, const SetSidechain& c) {
    if (c.from == KeySource::bus) return "bus \"" + d.buses[static_cast<std::size_t>(c.source)].name + "\"";
    if (c.from == KeySource::returnTrack) return "return \"" + d.returns[static_cast<std::size_t>(c.source)].name + "\"";
    return trackTitle(d, c.source) + (c.from == KeySource::trackPostFader ? " after its fader" : "");
}
} // namespace detail

// The stable name, e.g. "RenameTrack". Scripts, macros and key bindings refer to commands by it.
inline juce::String name(const Command& command) { return detail::names[command.index()]; }
// What the undo menu shows for the command on its own.
inline juce::String label(const Command& command) { return detail::labels[command.index()]; }

inline std::optional<midi_clip::Edit> midiEdit(const Command& command) {
    if(const auto* c=std::get_if<CreateClip>(&command)) return midi_clip::Edit{midi_clip::Kind::create,c->track,c->scene};
    return {};
}

inline std::optional<rack_command::Edit> rackEdit(const Command& command) {
    if (std::holds_alternative<CreateRackPattern>(command)) return rack_command::Edit{rack_command::Kind::create};
    if (const auto* c = std::get_if<AssignRackClip>(&command)) return rack_command::Edit{rack_command::Kind::assign, c->track, c->scene, c->clip};
    return {};
}

inline std::optional<session_clip::Duplicate> duplicateEdit(const Command& command) {
    if (const auto* c = std::get_if<DuplicateClip>(&command)) return session_clip::Duplicate{c->track, c->scene, c->toTrack, c->toScene};
    return {};
}

inline std::optional<session_clip::Move> moveEdit(const Command& command) {
    if (const auto* c = std::get_if<MoveClip>(&command)) return session_clip::Move{c->track, c->scene, c->toTrack, c->toScene};
    return {};
}

inline std::optional<clipboard_clip::Edit> clipboardEdit(const Command& command) {
    using Kind = clipboard_clip::Kind;
    if (const auto* c = std::get_if<PasteClip>(&command)) return clipboard_clip::Edit{Kind::paste, c->track, c->scene};
    if (const auto* c = std::get_if<PasteLinkedClip>(&command)) return clipboard_clip::Edit{Kind::link, c->track, c->scene};
    if (const auto* c = std::get_if<MakeClipUnique>(&command)) return clipboard_clip::Edit{Kind::unique, c->track, c->scene};
    return {};
}

inline std::optional<return_edit::Edit> returnEdit(const Command& command) {
    using Kind = return_edit::Kind;
    if (std::holds_alternative<CreateReturn>(command)) return return_edit::Edit{Kind::create};
    if (const auto* c = std::get_if<RenameReturn>(&command)) return return_edit::Edit{Kind::rename, c->returnSlot, c->name};
    if (const auto* c = std::get_if<DeleteReturn>(&command)) return return_edit::Edit{Kind::remove, c->returnSlot};
    return {};
}

inline std::optional<bus_edit::Edit> busEdit(const Command& command) {
    using Kind = bus_edit::Kind;
    if (std::holds_alternative<CreateBus>(command)) return bus_edit::Edit{Kind::create};
    if (const auto* c = std::get_if<RenameBus>(&command)) return bus_edit::Edit{Kind::rename, c->bus, c->name};
    if (const auto* c = std::get_if<DeleteBus>(&command)) return bus_edit::Edit{Kind::remove, c->bus};
    return {};
}

inline std::optional<device_edit::Edit> deviceEdit(const Command& command) {
    using Kind = device_edit::Kind;
    if (const auto* c = std::get_if<InsertDevice>(&command)) return device_edit::Edit{Kind::insert, c->stage, c->rack, c->slot, -1, false, c->device};
    if (const auto* c = std::get_if<RemoveDevice>(&command)) return device_edit::Edit{Kind::remove, c->stage, c->rack, c->slot};
    if (const auto* c = std::get_if<MoveDevice>(&command)) return device_edit::Edit{Kind::move, c->stage, c->rack, c->slot, c->to};
    if (const auto* c = std::get_if<SetDeviceBypass>(&command)) return device_edit::Edit{Kind::bypass, c->stage, c->rack, c->slot, -1, c->bypassed};
    return std::nullopt;
}
inline std::optional<sidechain_edit::Edit> sidechainEdit(const Command& command) {
    if (const auto* c = std::get_if<SetSidechain>(&command)) return sidechain_edit::Edit{c->stage, c->rack, c->effect, c->from, c->source};
    return {};
}

inline std::optional<tempo_edit::Edit> tempoEdit(const Command& command) {
    using Kind = tempo_edit::Kind;
    tempo_edit::Edit e;
    if (const auto* c = std::get_if<SetTempo>(&command)) { e.kind = Kind::projectTempo; e.bpm = c->bpm; return e; }
    if (const auto* c = std::get_if<SetTempoChange>(&command)) { e.kind = Kind::setChange; e.tick = c->tick; e.changeBpm = c->bpm; return e; }
    if (const auto* c = std::get_if<MoveTempoChange>(&command)) { e.kind = Kind::moveChange; e.tick = c->tick; e.to = c->to; return e; }
    if (const auto* c = std::get_if<RemoveTempoChange>(&command)) { e.kind = Kind::removeChange; e.tick = c->tick; return e; }
    if (std::holds_alternative<ClearTempoChanges>(command)) { e.kind = Kind::clearChanges; return e; }
    if (const auto* c = std::get_if<SetMeterChange>(&command)) { e.kind = Kind::setMeter; e.bar = c->bar; e.numerator = c->numerator; e.denominator = c->denominator; return e; }
    if (const auto* c = std::get_if<RemoveMeterChange>(&command)) { e.kind = Kind::removeMeter; e.bar = c->bar; return e; }
    if (std::holds_alternative<ClearMeterChanges>(command)) { e.kind = Kind::clearMeters; return e; }
    return std::nullopt;
}

inline std::optional<scene_edit::Edit> sceneEdit(const Command& command) {
    using Kind = scene_edit::Kind;
    if (std::holds_alternative<CreateScene>(command)) return scene_edit::Edit{Kind::create};
    if (const auto* c = std::get_if<DuplicateScene>(&command)) return scene_edit::Edit{Kind::duplicate, c->scene};
    if (const auto* c = std::get_if<MoveScene>(&command)) return scene_edit::Edit{Kind::move, c->scene, c->to};
    if (const auto* c = std::get_if<RenameScene>(&command)) return scene_edit::Edit{Kind::rename, c->scene, -1, c->name};
    if (const auto* c = std::get_if<DeleteScene>(&command)) return scene_edit::Edit{Kind::remove, c->scene};
    return {};
}

// Why `command` cannot be applied to `d`, or an empty string when it can.
inline juce::String refusal(const ProjectDocument& d, const Command& command, const clipboard_clip::Clipboard* clipboard = nullptr) {
    if (const auto* c = std::get_if<ImportClipAudio>(&command)) return c->prepared ? session_audio_import::refusal(d, *c->prepared) : juce::String("There is no decoded audio to import.");
    if (const auto* c = std::get_if<RemoveClipAudio>(&command)) return session_clip::removeAudioRefusal(d, c->track, c->scene);
    if (const auto edit = rackEdit(command)) return rack_command::refusal(d, *edit);
    if (const auto edit = duplicateEdit(command)) return session_clip::duplicateRefusal(d, *edit);
    if (const auto edit = moveEdit(command)) return session_clip::moveRefusal(d, *edit);
    if (const auto edit = clipboardEdit(command)) return clipboard_clip::refusal(d, *edit, clipboard);
    if (const auto* c = std::get_if<DeleteClip>(&command)) return session_clip::deleteRefusal(d, c->track, c->scene);
    if (const auto edit = sceneEdit(command)) return scene_edit::refusal(d, *edit);
    if(const auto edit=midiEdit(command)) return midi_clip::refusal(d,*edit);
    auto track = [&](int t) -> juce::String {
        return t >= 0 && t < d.trackCount() ? juce::String{}
            : "There is no track " + juce::String(t + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    };
    if (std::holds_alternative<CreateTrack>(command)) {
        if (!d.trackRegistry.canAllocate()) return "The project cannot allocate another track identity.";
        return d.canAddTrack() ? juce::String{} : "The project already has the most tracks allowed (" + juce::String(maxSessionTracks) + ").";
    }
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
    if (const auto edit = returnEdit(command)) return return_edit::refusal(d, *edit);
    auto send = [&](int t, int slot) -> juce::String {
        if (const auto why = track(t); why.isNotEmpty()) return why;
        return return_edit::slotInUse(d, slot) ? juce::String{} : "Return slot " + juce::String(slot) + " is not in use.";
    };
    if (const auto* c = std::get_if<SetTrackSend>(&command)) {
        if (const auto why = send(c->track, c->returnSlot); why.isNotEmpty()) return why;
        return std::isfinite(c->level) && c->level >= 0 && c->level <= 1 ? juce::String{} : "A send level must be a finite linear level from 0 to 1.";
    }
    if (const auto* c = std::get_if<SetTrackSendMode>(&command)) return send(c->track, c->returnSlot);
    if (const auto edit = busEdit(command)) return bus_edit::refusal(d, *edit);
    if (const auto edit = sidechainEdit(command)) return sidechain_edit::refusal(d, *edit);
    if (const auto edit = deviceEdit(command)) return device_edit::refusal(d, *edit);
    if (const auto edit = tempoEdit(command)) return tempo_edit::refusal(d, *edit);
    if (const auto* c = std::get_if<SetTrackOutput>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return c->bus == -1 || bus_edit::slotInUse(d, c->bus) ? juce::String{} : "Bus slot " + juce::String(c->bus) + " is not in use.";
    }
    auto stripGain = [](double gain, const char* strip) -> juce::String {
        return std::isfinite(gain) && gain >= 0 && gain <= 2 ? juce::String{} : juce::String(strip) + " volume must be a finite linear gain from 0 to 2 (1 is unity).";
    };
    auto stripPan = [](double pan, const char* strip) -> juce::String {
        return std::isfinite(pan) && pan >= -1 && pan <= 1 ? juce::String{} : juce::String(strip) + " pan must be a finite number from -1 (left) to 1 (right).";
    };
    auto returnSlot = [&](int slot) -> juce::String { return return_edit::slotInUse(d, slot) ? juce::String{} : "Return slot " + juce::String(slot) + " is not in use."; };
    auto busSlot = [&](int slot) -> juce::String { return bus_edit::slotInUse(d, slot) ? juce::String{} : "Bus slot " + juce::String(slot) + " is not in use."; };
    if (const auto* c = std::get_if<SetReturnVolume>(&command)) { if (const auto why = returnSlot(c->returnSlot); why.isNotEmpty()) return why; return stripGain(c->gain, "Return"); }
    if (const auto* c = std::get_if<SetReturnPan>(&command)) { if (const auto why = returnSlot(c->returnSlot); why.isNotEmpty()) return why; return stripPan(c->pan, "Return"); }
    if (const auto* c = std::get_if<SetReturnMute>(&command)) return returnSlot(c->returnSlot);
    if (const auto* c = std::get_if<SetBusVolume>(&command)) { if (const auto why = busSlot(c->bus); why.isNotEmpty()) return why; return stripGain(c->gain, "Bus"); }
    if (const auto* c = std::get_if<SetBusPan>(&command)) { if (const auto why = busSlot(c->bus); why.isNotEmpty()) return why; return stripPan(c->pan, "Bus"); }
    if (const auto* c = std::get_if<SetBusMute>(&command)) return busSlot(c->bus);
    if (const auto* c = std::get_if<SetBusSolo>(&command)) return busSlot(c->bus);
    if (const auto* c = std::get_if<SetMasterVolume>(&command)) return std::isfinite(c->gain) && c->gain >= 0 && c->gain <= maxMasterGain ? juce::String{} : "Master volume must be a finite linear gain from 0 to 4 (1 is unity).";
    if (const auto* c = std::get_if<SetTrackVolume>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return std::isfinite(c->gain) && c->gain >= 0 && c->gain <= 2 ? juce::String{}
            : "Track volume must be a finite linear gain from 0 to 2 (1 is unity).";
    }
    if (const auto* c = std::get_if<SetTrackPan>(&command)) {
        if (const auto why = track(c->track); why.isNotEmpty()) return why;
        return std::isfinite(c->pan) && c->pan >= -1 && c->pan <= 1 ? juce::String{}
            : "Track pan must be a finite number from -1 (left) to 1 (right).";
    }
    if (const auto* c = std::get_if<SetTrackMute>(&command)) return track(c->track);
    if (const auto* c = std::get_if<SetTrackSolo>(&command)) return track(c->track);
    const auto& c = std::get<ColorTrack>(command);
    if (const auto why = track(c.track); why.isNotEmpty()) return why;
    return c.colour >= 0 && c.colour < ProjectController::trackColourCount ? juce::String{}
        : "Track colours are numbered 0 to " + juce::String(ProjectController::trackColourCount - 1) + ".";
}

// One line saying what `command` does to `d` (call before applying it).
inline juce::String describe(const ProjectDocument& d, const Command& command) {
    auto returnName = [&](int r) { return "\"" + d.returns[static_cast<std::size_t>(r)].name + "\""; };
    auto busName = [&](int b) { return "\"" + d.buses[static_cast<std::size_t>(b)].name + "\""; };
    if (const auto edit = tempoEdit(command)) {
        const juce::String thawing = tempo_edit::changesTempo(*edit) && tempo_edit::anyFrozen(d) && !tempo_edit::changesNothing(d, *edit) ? " (frozen tracks are unfrozen first)" : "";
        if (const auto* c = std::get_if<SetTempo>(&command)) return "Set the project tempo to " + juce::String(c->bpm) + " BPM" + thawing;
        if (const auto* c = std::get_if<SetTempoChange>(&command)) return "Set the tempo from tick " + juce::String(c->tick) + " to " + juce::String(c->bpm, 2) + " BPM" + thawing;
        if (const auto* c = std::get_if<MoveTempoChange>(&command)) return "Move the tempo change at tick " + juce::String(c->tick) + " to tick " + juce::String(c->to) + thawing;
        if (const auto* c = std::get_if<RemoveTempoChange>(&command)) return "Remove the tempo change at tick " + juce::String(c->tick) + thawing;
        if (std::holds_alternative<ClearTempoChanges>(command)) return "Remove every tempo change" + thawing;
        if (const auto* c = std::get_if<SetMeterChange>(&command)) return "Set the time signature from bar " + juce::String(c->bar + 1) + " to " + juce::String(c->numerator) + "/" + juce::String(c->denominator);
        if (const auto* c = std::get_if<RemoveMeterChange>(&command)) return "Remove the time signature change at bar " + juce::String(c->bar + 1);
        return "Remove every time signature change";
    }
    if (const auto* c = std::get_if<SetTrackOutput>(&command)) return "Send " + detail::trackTitle(d, c->track) + " to " + (c->bus < 0 ? juce::String("the master") : "bus " + busName(c->bus));
    if (const auto* c = std::get_if<SetSidechain>(&command)) {
        const auto effect = "FX " + juce::String(c->effect + 1) + " on " + detail::rackTitle(d, *c);
        return c->source < 0 ? "Remove the sidechain key from " + effect : "Key " + effect + " from " + detail::keyTitle(d, *c);
    }
    if (const auto* c = std::get_if<InsertDevice>(&command)) {
        const auto rack = detail::rackTitle(d, c->stage, c->rack);
        return "Insert " + pluginDisplayName(c->device) + (c->slot == device_edit::instrumentSlot ? " as the instrument on " : " in FX slot " + juce::String(c->slot + 1) + " on ") + rack;
    }
    if (const auto* c = std::get_if<RemoveDevice>(&command)) return "Remove " + detail::deviceTitle(d, c->stage, c->rack, c->slot);
    if (const auto* c = std::get_if<MoveDevice>(&command)) return "Move " + detail::deviceTitle(d, c->stage, c->rack, c->slot) + " to FX slot " + juce::String(c->to + 1);
    if (const auto* c = std::get_if<SetDeviceBypass>(&command)) return juce::String(c->bypassed ? "Bypass " : "Enable ") + detail::deviceTitle(d, c->stage, c->rack, c->slot);
    if (const auto* c = std::get_if<SetReturnVolume>(&command)) return "Set volume of return " + returnName(c->returnSlot) + " to linear gain " + juce::String(c->gain, 6);
    if (const auto* c = std::get_if<SetReturnPan>(&command)) return "Set pan of return " + returnName(c->returnSlot) + " to " + juce::String(c->pan, 6);
    if (const auto* c = std::get_if<SetReturnMute>(&command)) return juce::String(c->muted ? "Mute return " : "Unmute return ") + returnName(c->returnSlot);
    if (std::holds_alternative<CreateBus>(command)) return "Add \"" + bus_edit::nextName(d) + "\" in bus slot " + juce::String(bus_edit::firstFreeSlot(d));
    if (const auto* c = std::get_if<RenameBus>(&command)) return "Rename bus " + busName(c->bus) + " to \"" + bus_edit::storedName(c->name) + "\"";
    if (const auto* c = std::get_if<DeleteBus>(&command)) return "Delete bus " + busName(c->bus) + ", sending its tracks to the master and dropping its FX";
    if (const auto* c = std::get_if<SetBusVolume>(&command)) return "Set volume of bus " + busName(c->bus) + " to linear gain " + juce::String(c->gain, 6);
    if (const auto* c = std::get_if<SetBusPan>(&command)) return "Set pan of bus " + busName(c->bus) + " to " + juce::String(c->pan, 6);
    if (const auto* c = std::get_if<SetBusMute>(&command)) return juce::String(c->muted ? "Mute bus " : "Unmute bus ") + busName(c->bus);
    if (const auto* c = std::get_if<SetBusSolo>(&command)) return juce::String(c->soloed ? "Solo bus " : "Unsolo bus ") + busName(c->bus);
    if (std::holds_alternative<CreateReturn>(command)) { const int r = return_edit::firstFreeSlot(d); return "Add \"" + defaultReturnName(r) + "\" in return slot " + juce::String(r); }
    if (const auto* c = std::get_if<RenameReturn>(&command)) return "Rename return \"" + d.returns[static_cast<std::size_t>(c->returnSlot)].name + "\" to \"" + return_edit::storedName(c->name) + "\"";
    if (const auto* c = std::get_if<DeleteReturn>(&command)) return "Remove return \"" + d.returns[static_cast<std::size_t>(c->returnSlot)].name + "\" with its sends, automation and FX";
    if (const auto* c = std::get_if<SetTrackSend>(&command)) return "Set " + detail::trackTitle(d, c->track) + " send to \"" + d.returns[static_cast<std::size_t>(c->returnSlot)].name + "\" to linear level " + juce::String(c->level, 6);
    if (const auto* c = std::get_if<SetTrackSendMode>(&command)) return "Make " + detail::trackTitle(d, c->track) + " send to \"" + d.returns[static_cast<std::size_t>(c->returnSlot)].name + "\" " + (c->preFader ? "pre-fader" : "post-fader");
    if (const auto* c = std::get_if<SetMasterVolume>(&command)) return "Set master volume to linear gain " + juce::String(c->gain,6);
    if (const auto* c = std::get_if<ImportClipAudio>(&command)) {
        const auto [track, scene] = session_audio_import::resolve(d, c->prepared->plan);
        return "Import " + c->prepared->plan.file.getFileName() + " into Session track " + juce::String(track + 1) + ", scene " + juce::String(scene + 1);
    }
    if (const auto* c = std::get_if<RemoveClipAudio>(&command)) return "Remove audio from Session track " + juce::String(c->track + 1) + ", scene " + juce::String(c->scene + 1);
    if (const auto edit = rackEdit(command)) {
        if (edit->kind == rack_command::Kind::create) return "Create empty rack pattern " + juce::String(d.channelRack.patternCount + 1);
        return "Assign rack source " + juce::String(static_cast<juce::uint64>(edit->clip)) + " to track " + juce::String(edit->track + 1) + ", scene " + juce::String(edit->scene + 1);
    }
    if (const auto edit = duplicateEdit(command)) return "Duplicate Session clip from track " + juce::String(edit->track + 1) + ", scene " + juce::String(edit->scene + 1) + " to track " + juce::String(edit->toTrack + 1) + ", scene " + juce::String(edit->toScene + 1);
    if (const auto edit = moveEdit(command)) return "Move Session clip from track " + juce::String(edit->track + 1) + ", scene " + juce::String(edit->scene + 1) + " to track " + juce::String(edit->toTrack + 1) + ", scene " + juce::String(edit->toScene + 1);
    if (const auto edit = clipboardEdit(command)) return label(command) + " at track " + juce::String(edit->track + 1) + ", scene " + juce::String(edit->scene + 1);
    if (const auto* c = std::get_if<DeleteClip>(&command)) return "Clear Session clip at track " + juce::String(c->track + 1) + ", scene " + juce::String(c->scene + 1);
    if (const auto edit = sceneEdit(command)) {
        if (edit->kind == scene_edit::Kind::create) return "Add scene " + juce::String(d.sceneCount() + 1);
        return label(command) + " " + juce::String(edit->scene + 1) + " (" + d.scenes[static_cast<std::size_t>(edit->scene)].name + ")";
    }
    if(const auto edit=midiEdit(command)) {
        const auto cell="track "+juce::String(edit->track+1)+", scene "+juce::String(edit->scene+1);
        if(edit->kind==midi_clip::Kind::create)return "Create an empty MIDI clip at "+cell;
        if(edit->kind==midi_clip::Kind::duplicate)return "Duplicate MIDI clip from track "+juce::String(edit->fromTrack+1)+", scene "+juce::String(edit->fromScene+1)+" to "+cell;
        return "Make the MIDI clip at "+cell+" independent";
    }
    if (std::holds_alternative<CreateTrack>(command)) return "Add \"" + d.nextTrackName() + "\" as track " + juce::String(d.trackCount() + 1);
    if (const auto* c = std::get_if<DeleteTrack>(&command)) return "Delete " + detail::trackTitle(d, c->track);
    if (const auto* c = std::get_if<MoveTrack>(&command)) return "Move " + detail::trackTitle(d, c->track) + " to position " + juce::String(c->to + 1);
    if (const auto* c = std::get_if<RenameTrack>(&command)) return "Rename " + detail::trackTitle(d, c->track) + " to \"" + detail::trackName(c->name) + "\"";
    if (const auto* c = std::get_if<SetTrackVolume>(&command)) return "Set volume of " + detail::trackTitle(d, c->track) + " to linear gain " + juce::String(c->gain, 6);
    if (const auto* c = std::get_if<SetTrackPan>(&command)) return "Set pan of " + detail::trackTitle(d, c->track) + " to " + juce::String(c->pan, 6);
    if (const auto* c = std::get_if<SetTrackMute>(&command)) return juce::String(c->muted ? "Mute " : "Unmute ") + detail::trackTitle(d, c->track);
    if (const auto* c = std::get_if<SetTrackSolo>(&command)) return juce::String(c->soloed ? "Solo " : "Unsolo ") + detail::trackTitle(d, c->track);
    const auto& c = std::get<ColorTrack>(command);
    return "Colour " + detail::trackTitle(d, c.track) + " with colour " + juce::String(c.colour);
}

// Applies `command` to a document on its own, exactly as the controller's
// operation changes the document (the engine is not involved). Returns the
// refusal, or an empty string when it was applied.
inline juce::String applyTo(ProjectDocument& d, const Command& command, const clipboard_clip::Clipboard* clipboard = nullptr) {
    if (const auto why = refusal(d, command, clipboard); why.isNotEmpty()) return why;
    if (const auto edit = returnEdit(command)) {
        if (const auto why = return_edit::apply(d, *edit); why.isNotEmpty()) return why;
        return d.synchronizeTrackRegistry() ? juce::String{} : juce::String("The project cannot resolve its track identities.");
    }
    if (const auto edit = busEdit(command)) {
        if (const auto why = bus_edit::apply(d, *edit); why.isNotEmpty()) return why;
        return d.synchronizeTrackRegistry() ? juce::String{} : juce::String("The project cannot resolve its track identities.");
    }
    if (const auto edit = sidechainEdit(command)) return sidechain_edit::apply(d, *edit);
    if (const auto edit = deviceEdit(command)) return device_edit::apply(d, *edit);
    if (const auto edit = tempoEdit(command)) return tempo_edit::apply(d, *edit);
    if (const auto* c = std::get_if<SetTrackOutput>(&command)) { d.tracks[static_cast<std::size_t>(c->track)].settings.output = c->bus; return {}; }
    if (const auto* c = std::get_if<SetReturnVolume>(&command)) { d.returns[static_cast<std::size_t>(c->returnSlot)].settings.gain = static_cast<float>(c->gain); return {}; }
    if (const auto* c = std::get_if<SetReturnPan>(&command)) { d.returns[static_cast<std::size_t>(c->returnSlot)].settings.pan = static_cast<float>(c->pan); return {}; }
    if (const auto* c = std::get_if<SetReturnMute>(&command)) { d.returns[static_cast<std::size_t>(c->returnSlot)].settings.mute = c->muted; return {}; }
    if (const auto* c = std::get_if<SetBusVolume>(&command)) { d.buses[static_cast<std::size_t>(c->bus)].settings.gain = static_cast<float>(c->gain); return {}; }
    if (const auto* c = std::get_if<SetBusPan>(&command)) { d.buses[static_cast<std::size_t>(c->bus)].settings.pan = static_cast<float>(c->pan); return {}; }
    if (const auto* c = std::get_if<SetBusMute>(&command)) { d.buses[static_cast<std::size_t>(c->bus)].settings.mute = c->muted; return {}; }
    if (const auto* c = std::get_if<SetBusSolo>(&command)) { d.buses[static_cast<std::size_t>(c->bus)].settings.solo = c->soloed; return {}; }
    if (const auto* c = std::get_if<SetTrackSend>(&command)) { d.tracks[static_cast<std::size_t>(c->track)].settings.send[static_cast<std::size_t>(c->returnSlot)] = static_cast<float>(c->level); return {}; }
    if (const auto* c = std::get_if<SetTrackSendMode>(&command)) { d.tracks[static_cast<std::size_t>(c->track)].settings.sendPre[static_cast<std::size_t>(c->returnSlot)] = c->preFader; return {}; }
    if (const auto* c = std::get_if<SetMasterVolume>(&command)) { d.masterGain=static_cast<float>(c->gain); return {}; }
    if (const auto* c = std::get_if<ImportClipAudio>(&command)) return session_audio_import::apply(d, *c->prepared);
    if (const auto* c = std::get_if<RemoveClipAudio>(&command)) return session_clip::removeAudio(d, c->track, c->scene) ? juce::String{} : "Clip audio could not be removed.";
    if (const auto edit = rackEdit(command)) return rack_command::apply(d, *edit);
    if (const auto edit = duplicateEdit(command)) return session_clip::duplicate(d, *edit);
    if (const auto edit = moveEdit(command)) return session_clip::move(d, *edit) ? juce::String{} : "The Session clip could not be moved.";
    if (const auto edit = clipboardEdit(command)) return clipboard_clip::apply(d, *edit, clipboard);
    if (const auto* c = std::get_if<DeleteClip>(&command)) return session_clip::remove(d, c->track, c->scene) ? juce::String{} : "The Session clip could not be deleted.";
    if (const auto edit = sceneEdit(command)) return scene_edit::apply(d, *edit);
    if(const auto edit=midiEdit(command)) return midi_clip::apply(d,*edit)?juce::String{}:"The MIDI clip edit could not be applied.";
    if (std::holds_alternative<CreateTrack>(command)) {
        d.tracks.push_back({d.nextTrackName(), {}, d.nextTrackColour()});
        for (auto& scene : d.cells) scene.emplace_back();
    } else if (const auto* c = std::get_if<DeleteTrack>(&command)) {
        d.timeline.trackDeleted(c->track);
        d.channelRack.trackDeleted(c->track);
        d.sidechainTrackDeleted(c->track);
        d.tracks.erase(d.tracks.begin() + c->track);
        for (auto& scene : d.cells) scene.erase(scene.begin() + c->track);
        d.prune();
        if (d.selectedTrack > c->track) --d.selectedTrack;
        d.clampSelection();
    } else if (const auto* c = std::get_if<MoveTrack>(&command)) {
        const int step = c->to > c->track ? 1 : -1;
        for (int t = c->track; t != c->to; t += step) detail::swapTracks(d, t, t + step);
    } else if (const auto* c = std::get_if<RenameTrack>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].name = detail::trackName(c->name);
    } else if (const auto* c = std::get_if<SetTrackVolume>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].settings.gain = static_cast<float>(c->gain);
    } else if (const auto* c = std::get_if<SetTrackPan>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].settings.pan = static_cast<float>(c->pan);
    } else if (const auto* c = std::get_if<SetTrackMute>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].settings.mute = c->muted;
    } else if (const auto* c = std::get_if<SetTrackSolo>(&command)) {
        d.tracks[static_cast<std::size_t>(c->track)].settings.solo = c->soloed;
    } else {
        const auto& colour = std::get<ColorTrack>(command);
        d.tracks[static_cast<std::size_t>(colour.track)].colour = colour.colour;
    }
    if (!d.synchronizeTrackRegistry()) return "The project cannot resolve its track identities.";
    return {};
}

// The command's arguments as an object, e.g. {"track": 2, "name": "Bass"}.
inline juce::var arguments(const Command& command) {
    auto* object = new juce::DynamicObject();
    juce::var result(object);
    if (const auto* c = std::get_if<SetTrackOutput>(&command)) { object->setProperty("track", c->track); object->setProperty("bus", c->bus); return result; }
    if (const auto* c = std::get_if<SetTempo>(&command)) { object->setProperty("bpm", c->bpm); return result; }
    if (const auto* c = std::get_if<SetTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); object->setProperty("bpm", c->bpm); return result; }
    if (const auto* c = std::get_if<MoveTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); object->setProperty("to", static_cast<juce::int64>(c->to)); return result; }
    if (const auto* c = std::get_if<RemoveTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); return result; }
    if (std::holds_alternative<ClearTempoChanges>(command) || std::holds_alternative<ClearMeterChanges>(command)) return result;
    if (const auto* c = std::get_if<SetMeterChange>(&command)) { object->setProperty("bar", c->bar); object->setProperty("numerator", c->numerator); object->setProperty("denominator", c->denominator); return result; }
    if (const auto* c = std::get_if<RemoveMeterChange>(&command)) { object->setProperty("bar", c->bar); return result; }
    if (const auto* c = std::get_if<SetSidechain>(&command)) {
        object->setProperty("stage", detail::stageName(c->stage)); object->setProperty("rack", c->rack); object->setProperty("effect", c->effect);
        object->setProperty("from", keySourceName(c->from)); object->setProperty("source", c->source); return result;
    }
    if (const auto* c = std::get_if<InsertDevice>(&command)) {
        object->setProperty("stage", detail::stageName(c->stage)); object->setProperty("rack", c->rack); object->setProperty("slot", c->slot);
        object->setProperty("description", c->device.descriptionXml); return result;
    }
    if (const auto* c = std::get_if<RemoveDevice>(&command)) { object->setProperty("stage", detail::stageName(c->stage)); object->setProperty("rack", c->rack); object->setProperty("slot", c->slot); return result; }
    if (const auto* c = std::get_if<MoveDevice>(&command)) { object->setProperty("stage", detail::stageName(c->stage)); object->setProperty("rack", c->rack); object->setProperty("slot", c->slot); object->setProperty("to", c->to); return result; }
    if (const auto* c = std::get_if<SetDeviceBypass>(&command)) { object->setProperty("stage", detail::stageName(c->stage)); object->setProperty("rack", c->rack); object->setProperty("slot", c->slot); object->setProperty("bypassed", c->bypassed); return result; }
    if (const auto* c = std::get_if<SetTrackSend>(&command)) { object->setProperty("track", c->track); object->setProperty("return", c->returnSlot); object->setProperty("level", c->level); return result; }
    if (const auto* c = std::get_if<SetTrackSendMode>(&command)) { object->setProperty("track", c->track); object->setProperty("return", c->returnSlot); object->setProperty("preFader", c->preFader); return result; }
    if (const auto* c = std::get_if<RenameReturn>(&command)) { object->setProperty("return", c->returnSlot); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<SetReturnVolume>(&command)) { object->setProperty("return", c->returnSlot); object->setProperty("gain", c->gain); return result; }
    if (const auto* c = std::get_if<SetReturnPan>(&command)) { object->setProperty("return", c->returnSlot); object->setProperty("pan", c->pan); return result; }
    if (const auto* c = std::get_if<SetReturnMute>(&command)) { object->setProperty("return", c->returnSlot); object->setProperty("muted", c->muted); return result; }
    if (const auto* c = std::get_if<RenameBus>(&command)) { object->setProperty("bus", c->bus); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<DeleteBus>(&command)) { object->setProperty("bus", c->bus); return result; }
    if (const auto* c = std::get_if<SetBusVolume>(&command)) { object->setProperty("bus", c->bus); object->setProperty("gain", c->gain); return result; }
    if (const auto* c = std::get_if<SetBusPan>(&command)) { object->setProperty("bus", c->bus); object->setProperty("pan", c->pan); return result; }
    if (const auto* c = std::get_if<SetBusMute>(&command)) { object->setProperty("bus", c->bus); object->setProperty("muted", c->muted); return result; }
    if (const auto* c = std::get_if<SetBusSolo>(&command)) { object->setProperty("bus", c->bus); object->setProperty("soloed", c->soloed); return result; }
    if (const auto* c = std::get_if<DeleteReturn>(&command)) { object->setProperty("return", c->returnSlot); return result; }
    if (const auto* c = std::get_if<SetMasterVolume>(&command)) { object->setProperty("gain",c->gain); return result; }
    if (const auto* c = std::get_if<ImportClipAudio>(&command)) {
        if (c->prepared) object->setProperty("file", c->prepared->plan.file.getFullPathName());
        return result;
    }
    if (const auto* c = std::get_if<RemoveClipAudio>(&command)) {
        object->setProperty("track", c->track); object->setProperty("scene", c->scene); return result;
    }
    if (const auto edit = rackEdit(command)) {
        if (edit->kind == rack_command::Kind::assign) {
            object->setProperty("track", edit->track); object->setProperty("scene", edit->scene);
            object->setProperty("clip", juce::String(static_cast<juce::uint64>(edit->clip)));
        }
        return result;
    }
    if (const auto edit = duplicateEdit(command)) {
        object->setProperty("track", edit->track); object->setProperty("scene", edit->scene);
        object->setProperty("toTrack", edit->toTrack); object->setProperty("toScene", edit->toScene); return result;
    }
    if (const auto edit = moveEdit(command)) {
        object->setProperty("track", edit->track); object->setProperty("scene", edit->scene);
        object->setProperty("toTrack", edit->toTrack); object->setProperty("toScene", edit->toScene); return result;
    }
    if (const auto edit = clipboardEdit(command)) {
        object->setProperty("track", edit->track); object->setProperty("scene", edit->scene); return result;
    }
    if (const auto* c = std::get_if<DeleteClip>(&command)) {
        object->setProperty("track", c->track); object->setProperty("scene", c->scene); return result;
    }
    if (const auto edit = sceneEdit(command)) {
        if (edit->kind != scene_edit::Kind::create) object->setProperty("scene", edit->scene);
        if (edit->kind == scene_edit::Kind::move) object->setProperty("to", edit->to);
        if (edit->kind == scene_edit::Kind::rename) object->setProperty("name", edit->name);
        return result;
    }
    if(const auto edit=midiEdit(command)) {
        if(const auto* copy=std::get_if<DuplicateClip>(&command)) {
            object->setProperty("track",copy->track);object->setProperty("scene",copy->scene);
            object->setProperty("toTrack",copy->toTrack);object->setProperty("toScene",copy->toScene);
        } else {object->setProperty("track",edit->track);object->setProperty("scene",edit->scene);}
        return result;
    }
    if (const auto* c = std::get_if<DeleteTrack>(&command)) object->setProperty("track", c->track);
    else if (const auto* c = std::get_if<MoveTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("to", c->to); }
    else if (const auto* c = std::get_if<RenameTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("name", c->name); }
    else if (const auto* c = std::get_if<ColorTrack>(&command)) { object->setProperty("track", c->track); object->setProperty("colour", c->colour); }
    else if (const auto* c = std::get_if<SetTrackVolume>(&command)) { object->setProperty("track", c->track); object->setProperty("gain", c->gain); }
    else if (const auto* c = std::get_if<SetTrackPan>(&command)) { object->setProperty("track", c->track); object->setProperty("pan", c->pan); }
    else if (const auto* c = std::get_if<SetTrackMute>(&command)) { object->setProperty("track", c->track); object->setProperty("muted", c->muted); }
    else if (const auto* c = std::get_if<SetTrackSolo>(&command)) { object->setProperty("track", c->track); object->setProperty("soloed", c->soloed); }
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
    if (index == 25) { error = "ImportClipAudio takes decoded audio, which cannot be written as arguments."; return std::nullopt; }
    static const std::array<juce::StringArray, 55> allowed{
        juce::StringArray{}, juce::StringArray{"track"}, juce::StringArray{"track", "to"},
        juce::StringArray{"track", "name"}, juce::StringArray{"track", "colour"},
        juce::StringArray{"track", "gain"}, juce::StringArray{"track", "pan"},
        juce::StringArray{"track", "muted"}, juce::StringArray{"track", "soloed"},
        juce::StringArray{"track","scene"},juce::StringArray{"track","scene","toTrack","toScene"},juce::StringArray{"track","scene"},
        juce::StringArray{},juce::StringArray{"scene"},juce::StringArray{"scene","to"},juce::StringArray{"scene","name"},juce::StringArray{"scene"},juce::StringArray{"track","scene"},juce::StringArray{"track","scene"},juce::StringArray{"track","scene"},juce::StringArray{"track","scene","toTrack","toScene"},juce::StringArray{},juce::StringArray{"track","scene","clip"},juce::StringArray{"track","scene"},juce::StringArray{"gain"},juce::StringArray{},
        juce::StringArray{"track","return","level"},juce::StringArray{"track","return","preFader"},juce::StringArray{},juce::StringArray{"return","name"},juce::StringArray{"return"},
        juce::StringArray{"return","gain"},juce::StringArray{"return","pan"},juce::StringArray{"return","muted"},
        juce::StringArray{},juce::StringArray{"bus","name"},juce::StringArray{"bus"},
        juce::StringArray{"bus","gain"},juce::StringArray{"bus","pan"},juce::StringArray{"bus","muted"},juce::StringArray{"bus","soloed"},
        juce::StringArray{"track","bus"},juce::StringArray{"stage","rack","effect","from","source"},juce::StringArray{"stage","rack","slot","description"},juce::StringArray{"stage","rack","slot"},juce::StringArray{"stage","rack","slot","to"},juce::StringArray{"stage","rack","slot","bypassed"},
        {"bpm"}, {"tick", "bpm"}, {"tick", "to"}, {"tick"}, {}, {"bar", "numerator", "denominator"}, {"bar"}, {}
    };
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
    auto number = [&](const char* key) {
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && (value.isInt() || value.isInt64() || value.isDouble())) {
            const double numeric = static_cast<double>(value);
            if (std::isfinite(numeric)) return numeric;
        }
        if (ok) error = commandName + " needs a finite-number \"" + juce::String(key) + "\" argument.";
        ok = false;
        return 0.0;
    };
    auto boolean = [&](const char* key) {
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && value.isBool()) return static_cast<bool>(value);
        if (ok) error = commandName + " needs a boolean \"" + juce::String(key) + "\" argument.";
        ok = false;
        return false;
    };
    auto wide = [&](const char* key) {
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && (value.isInt() || value.isInt64())) return static_cast<std::int64_t>(static_cast<juce::int64>(value));
        if (ok) error = commandName + " needs a whole-number \"" + juce::String(key) + "\" argument.";
        ok = false;
        return std::int64_t{};
    };
    switch (index) {
        case 47: command = SetTempo{integer("bpm")}; break;
        case 48: { const auto tick = wide("tick"); command = SetTempoChange{tick, number("bpm")}; break; }
        case 49: { const auto tick = wide("tick"); command = MoveTempoChange{tick, wide("to")}; break; }
        case 50: command = RemoveTempoChange{wide("tick")}; break;
        case 51: command = ClearTempoChanges{}; break;
        case 52: { const int bar = integer("bar"), top = integer("numerator"); command = SetMeterChange{bar, top, integer("denominator")}; break; }
        case 53: command = RemoveMeterChange{integer("bar")}; break;
        case 54: command = ClearMeterChanges{}; break;
        case 0: command = CreateTrack{}; break;
        case 1: command = DeleteTrack{integer("track")}; break;
        case 2: { const int track = integer("track"); command = MoveTrack{track, integer("to")}; break; }
        case 3: { const int track = integer("track"); command = RenameTrack{track, text("name")}; break; }
        case 4: { const int track = integer("track"); command = ColorTrack{track, integer("colour")}; break; }
        case 5: { const int track = integer("track"); command = SetTrackVolume{track, number("gain")}; break; }
        case 6: { const int track = integer("track"); command = SetTrackPan{track, number("pan")}; break; }
        case 7: { const int track = integer("track"); command = SetTrackMute{track, boolean("muted")}; break; }
        case 8: { const int track = integer("track"); command = SetTrackSolo{track, boolean("soloed")}; break; }
        case 9: {const int t=integer("track"),s=integer("scene");command=CreateClip{t,s};break;}
        case 10: {const int t=integer("track"),s=integer("scene"),to=integer("toTrack"),scene=integer("toScene");command=DuplicateClip{t,s,to,scene};break;}
        case 11: {const int t=integer("track"),s=integer("scene");command=MakeClipUnique{t,s};break;}
        case 18: { const int t = integer("track"), s = integer("scene"); command = PasteClip{t, s}; break; }
        case 19: { const int t = integer("track"), s = integer("scene"); command = PasteLinkedClip{t, s}; break; }
        case 20: { const int t = integer("track"), s = integer("scene"), to = integer("toTrack"), scene = integer("toScene"); command = MoveClip{t, s, to, scene}; break; }
        case 24: command = SetMasterVolume{number("gain")}; break;
        case 26: { const int t = integer("track"), r = integer("return"); command = SetTrackSend{t, r, number("level")}; break; }
        case 27: { const int t = integer("track"), r = integer("return"); command = SetTrackSendMode{t, r, boolean("preFader")}; break; }
        case 28: command = CreateReturn{}; break;
        case 29: { const int r = integer("return"); command = RenameReturn{r, text("name")}; break; }
        case 30: command = DeleteReturn{integer("return")}; break;
        case 31: { const int r = integer("return"); command = SetReturnVolume{r, number("gain")}; break; }
        case 32: { const int r = integer("return"); command = SetReturnPan{r, number("pan")}; break; }
        case 33: { const int r = integer("return"); command = SetReturnMute{r, boolean("muted")}; break; }
        case 34: command = CreateBus{}; break;
        case 35: { const int b = integer("bus"); command = RenameBus{b, text("name")}; break; }
        case 36: command = DeleteBus{integer("bus")}; break;
        case 37: { const int b = integer("bus"); command = SetBusVolume{b, number("gain")}; break; }
        case 38: { const int b = integer("bus"); command = SetBusPan{b, number("pan")}; break; }
        case 39: { const int b = integer("bus"); command = SetBusMute{b, boolean("muted")}; break; }
        case 40: { const int b = integer("bus"); command = SetBusSolo{b, boolean("soloed")}; break; }
        case 41: { const int t = integer("track"); command = SetTrackOutput{t, integer("bus")}; break; }
        case 42: {
            const auto stageText = text("stage"); const int rack = integer("rack"), effect = integer("effect");
            const auto fromText = text("from"); const int source = integer("source");
            if (!ok) break;
            const auto stage = detail::stageNamed(stageText); const auto from = detail::keySourceNamed(fromText);
            if (!stage) { ok = false; error = "SetSidechain needs a \"stage\" of track, bus, return or master."; break; }
            if (!from) { ok = false; error = "SetSidechain needs a \"from\" of track, post, bus or return."; break; }
            command = SetSidechain{*stage, rack, effect, *from, source}; break;
        }
        case 43: case 44: case 45: case 46: {
            const auto stageText = text("stage"); const int rack = integer("rack"), slot = integer("slot");
            if (!ok) break;
            const auto stage = detail::stageNamed(stageText);
            if (!stage) { ok = false; error = commandName + " needs a \"stage\" of track, bus, return or master."; break; }
            if (index == 43) { const auto description = text("description"); if (ok) command = InsertDevice{*stage, rack, slot, PluginState(description, {})}; }
            else if (index == 44) command = RemoveDevice{*stage, rack, slot};
            else if (index == 45) { const int to = integer("to"); if (ok) command = MoveDevice{*stage, rack, slot, to}; }
            else { const bool bypassed = boolean("bypassed"); if (ok) command = SetDeviceBypass{*stage, rack, slot, bypassed}; }
            break;
        }
        case 23: { const int t = integer("track"), s = integer("scene"); command = RemoveClipAudio{t,s}; break; }
        case 21: command = CreateRackPattern{}; break;
        case 22: {
            const int track = integer("track"), scene = integer("scene"); const auto value = text("clip");
            const auto digits = value.toStdString(); std::uint64_t clip{};
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), clip);
            if (ok && (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || !clip
                || value != juce::String(static_cast<juce::uint64>(clip)))) {
                ok = false; error = "AssignRackClip needs a positive canonical decimal-string clip identity.";
            }
            command = AssignRackClip{track, scene, clip}; break;
        }
        case 12: command = CreateScene{}; break;
        case 13: command = DuplicateScene{integer("scene")}; break;
        case 14: { const int s = integer("scene"); command = MoveScene{s, integer("to")}; break; }
        case 15: { const int s = integer("scene"); command = RenameScene{s, text("name")}; break; }
        case 16: command = DeleteScene{integer("scene")}; break;
        case 17: { const int t = integer("track"), s = integer("scene"); command = DeleteClip{t, s}; break; }
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
    explicit Preview(const ProjectDocument& document) : after(document) {}
    ProjectDocument after;      // the document as the commands would leave it
    juce::StringArray changes;  // one line per command, in order
};

// What a Plugins panel button (1 bypass, 2 remove, 3 move up, 4 move down) does to the rack
// it shows. Nothing, when a move has nowhere to go. Other problems are for the command to refuse.
inline std::optional<Command> pluginPanelCommand(const ProjectDocument& d, RackStage stage, int rack, int op, int slot) {
    if (op == 1) {
        const auto* found = sidechain_edit::rackOf(d, stage, rack);
        const bool present = found != nullptr && (slot == device_edit::instrumentSlot || (slot >= 0 && slot < static_cast<int>(found->effects.size())));
        const bool bypassed = present && (slot == device_edit::instrumentSlot ? found->instrument : found->effects[static_cast<std::size_t>(slot)]).bypass;
        return SetDeviceBypass{stage, rack, slot, !bypassed};
    }
    if (op == 2) return RemoveDevice{stage, rack, slot};
    if (op != 3 && op != 4) return std::nullopt;
    const auto* found = sidechain_edit::rackOf(d, stage, rack);
    const int to = slot + (op == 3 ? -1 : 1);
    if (found == nullptr || slot < 0 || slot >= static_cast<int>(found->effects.size()) || to < 0 || to >= static_cast<int>(found->effects.size())) return std::nullopt;
    return MoveDevice{stage, rack, slot, to};
}

class CommandEngine {
public:
    explicit CommandEngine(ProjectController& projectController) : controller(projectController) {}

    // Applies the commands to a copy of the document. The project, the engine and the undo history are untouched.
    Preview preview(const std::vector<Command>& commands, const clipboard_clip::Clipboard* supplied = nullptr) const {
        const auto clipboard = supplied ? *supplied : controller.clipClipboard();
        object_ids::PreviewScope identities;
        Preview p(controller.document());
        if (commands.empty()) { p.error = "There is nothing to do."; return p; }
        for (std::size_t i = 0; i < commands.size(); ++i) {
            if (const auto why = refusal(p.after, commands[i], &clipboard); why.isNotEmpty()) {
                p.error = why; p.failedCommand = static_cast<int>(i); p.changes.clear();
                p.after = controller.document();
                return p;
            }
            p.changes.add(describe(p.after, commands[i]));
            if (const auto why = applyTo(p.after, commands[i], &clipboard); why.isNotEmpty()) {
                p.error = why; p.failedCommand = static_cast<int>(i); p.changes.clear();
                p.after = controller.document(); return p;
            }
        }
        p.ok = true;
        p.changed = !sameContent(p.after, controller.document());
        return p;
    }
    Preview preview(const Command& command) const { return preview(std::vector<Command>{command}); }

    // One command, one undo step. Repeated wheel edits can use a timed key;
    // a drag uses its own unique key with continuous=true until release.
    Result execute(const Command& command, const juce::String& coalesceKey = {}, bool continuous = false) {
        return batch({command}, label(command), coalesceKey, continuous);
    }

    // Every command, in order, as one undo step named `undoLabel`. If any command
    // would be refused, nothing changes: not the document, the engine or the history.
    Result batch(const std::vector<Command>& commands, const juce::String& undoLabel,
                 const juce::String& coalesceKey = {}, bool continuous = false) {
        if (controller.isBusy()) return refused("The project is busy; try again when it has finished.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return sceneEdit(c).has_value(); })) return refused("Stop recording before editing scenes.");
        if(!controller.canEditMidiClips() && std::any_of(commands.begin(),commands.end(),[](const auto& c){return midiEdit(c).has_value();})) return refused("Stop recording before editing MIDI clips.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return std::holds_alternative<DeleteClip>(c); })) return refused("Stop recording before deleting Session clips.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return clipboardEdit(c).has_value(); })) return refused("Stop recording before pasting or making clips independent.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return moveEdit(c).has_value(); })) return refused("Stop recording before moving Session clips.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return duplicateEdit(c).has_value(); })) return refused("Stop recording before duplicating Session clips.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return rackEdit(c).has_value(); })) return refused("Stop recording before creating or assigning rack patterns.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return std::holds_alternative<RemoveClipAudio>(c); })) return refused("Stop recording before removing Session clip audio.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return std::holds_alternative<ImportClipAudio>(c); })) return refused("Stop recording before importing Session clip audio.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return returnEdit(c).has_value(); })) return refused("Stop recording before adding, renaming or removing returns.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return busEdit(c).has_value(); })) return refused("Stop recording before adding, renaming or deleting buses.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return sidechainEdit(c).has_value(); })) return refused("Stop recording before changing a sidechain key.");
        if (!controller.canEditMidiClips() && std::any_of(commands.begin(), commands.end(), [](const auto& c) { return deviceEdit(c).has_value(); })) return refused("Stop recording before changing plugins.");
        for (const auto& command : commands)
            if (const auto edit = tempoEdit(command))
                if (const auto why = controller.tempoEditRefusal(*edit); why.isNotEmpty()) return refused(why);
        const auto clipboard = controller.clipClipboard();
        Result result = preview(commands, &clipboard);
        if (!result.ok || !result.changed) return result;
        std::size_t audioBytes = 0;
        for (const auto& command : commands)
            if (const auto* c = std::get_if<ImportClipAudio>(&command)) audioBytes += c->prepared->decoded.clip->decodedBytes();
        if (audioBytes > controller.availableAudioBytes()) return refused("Session audio storage is full (512 MiB limit)");
        const ProjectDocument before = controller.document();
        for (std::size_t i = 0; i < commands.size(); ++i)
            if (!run(commands[i], clipboard)) {
                // The preview accepted it, so the controller should have too. Put everything back.
                controller.revertTo(before);
                Result failed = refused("The project could not " + label(commands[i]).toLowerCase() + ".");
                failed.failedCommand = static_cast<int>(i);
                return failed;
            }
        const bool selects=std::any_of(commands.begin(),commands.end(),[](const auto& c){const auto edit=midiEdit(c);return duplicateEdit(c).has_value() || moveEdit(c).has_value() || (edit && edit->kind!=midi_clip::Kind::makeUnique);});
        // An import leaves the selection alone, but Undo still shows the clip it changed.
        int focusTrack = selects ? controller.document().selectedTrack : -1, focusScene = selects ? controller.document().selectedScene : -1;
        if (const auto* c = commands.size() == 1 ? std::get_if<ImportClipAudio>(&commands[0]) : nullptr)
            std::tie(focusTrack, focusScene) = session_audio_import::resolve(controller.document(), c->prepared->plan);
        controller.commit(undoLabel.isNotEmpty() ? undoLabel : juce::String("Edit tracks"),
            focusTrack,focusScene,coalesceKey,continuous,
            selects?before.selectedTrack:-1,selects?before.selectedScene:-1);
        return result;
    }

    // For a command that changes plugin racks. Live plugin state is captured only for an edit
    // that will be accepted and changes something, and the racks are rebuilt only if it did.
    template <class Capture, class Rebuild>
    Result executeWithPlugins(const Command& command, Capture&& capture, Rebuild&& rebuild) {
        if (!controller.canEditMidiClips() && sidechainEdit(command)) return refused("Stop recording before changing a sidechain key.");
        if (!controller.canEditMidiClips() && deviceEdit(command)) return refused("Stop recording before changing plugins.");
        const auto predicted = preview(command);
        if (!predicted.ok || !predicted.changed) return predicted;
        capture();
        auto result = execute(command);
        if (result.ok && result.changed) rebuild();
        return result;
    }
    bool undo() { return controller.undo(); }
    bool redo() { return controller.redo(); }

private:
    static Result refused(const juce::String& why) { Result r; r.error = why; return r; }

    bool run(const Command& command, const clipboard_clip::Clipboard& clipboard) {
        if (const auto* c = std::get_if<SetTrackOutput>(&command)) {
            // An unchanged output is a no-op, not a failure: the rest of a batch still runs.
            return controller.document().tracks[static_cast<std::size_t>(c->track)].settings.output == c->bus || controller.setTrackOutput(c->track, c->bus);
        }
        if (const auto edit = sidechainEdit(command)) return controller.applySidechainCommand(*edit);
        if (const auto edit = deviceEdit(command)) return controller.applyDeviceCommand(*edit);
        if (const auto edit = tempoEdit(command)) return controller.applyTempoCommand(*edit);
        if (std::holds_alternative<CreateReturn>(command)) return controller.addReturn() >= 0;
        if (const auto* c = std::get_if<RenameReturn>(&command)) {
            // An unchanged name is a no-op, not a failure: the rest of a batch still runs.
            return controller.document().returns[static_cast<std::size_t>(c->returnSlot)].name == return_edit::storedName(c->name) || controller.renameReturn(c->returnSlot, c->name);
        }
        if (const auto* c = std::get_if<DeleteReturn>(&command)) return controller.deleteReturn(c->returnSlot);
        if (const auto* c = std::get_if<SetReturnVolume>(&command)) return controller.setReturnVolume(c->returnSlot, c->gain);
        if (const auto* c = std::get_if<SetReturnPan>(&command)) return controller.setReturnPan(c->returnSlot, c->pan);
        if (const auto* c = std::get_if<SetReturnMute>(&command)) return controller.setReturnMute(c->returnSlot, c->muted);
        if (std::holds_alternative<CreateBus>(command)) return controller.addBus() >= 0;
        if (const auto* c = std::get_if<RenameBus>(&command)) return controller.renameBus(c->bus, c->name);
        if (const auto* c = std::get_if<DeleteBus>(&command)) return controller.deleteBus(c->bus);
        if (const auto* c = std::get_if<SetBusVolume>(&command)) return controller.setBusVolume(c->bus, c->gain);
        if (const auto* c = std::get_if<SetBusPan>(&command)) return controller.setBusPan(c->bus, c->pan);
        if (const auto* c = std::get_if<SetBusMute>(&command)) return controller.setBusMute(c->bus, c->muted);
        if (const auto* c = std::get_if<SetBusSolo>(&command)) return controller.setBusSolo(c->bus, c->soloed);
        if (const auto* c = std::get_if<SetTrackSend>(&command)) return controller.setTrackSend(c->track, c->returnSlot, c->level);
        if (const auto* c = std::get_if<SetTrackSendMode>(&command)) return controller.setTrackSendMode(c->track, c->returnSlot, c->preFader);
        if (const auto* c = std::get_if<SetMasterVolume>(&command)) return controller.setMasterVolume(c->gain);
        if (const auto* c = std::get_if<ImportClipAudio>(&command)) return controller.applyImportAudioCommand(*c->prepared);
        if (const auto* c = std::get_if<RemoveClipAudio>(&command)) return controller.applyRemoveClipAudioCommand(c->track,c->scene);
        if (const auto edit = rackEdit(command)) return controller.applyRackCommand(*edit);
        if (const auto edit = duplicateEdit(command)) return controller.applyDuplicateClipCommand(*edit);
        if (const auto edit = moveEdit(command)) return controller.applyMoveClipCommand(*edit);
        if (const auto edit = clipboardEdit(command)) return controller.applyClipboardClipCommand(*edit, clipboard);
        if (std::holds_alternative<CreateScene>(command)) return controller.addScene();
        if (const auto* c = std::get_if<DuplicateScene>(&command)) return controller.duplicateScene(c->scene);
        if (const auto* c = std::get_if<DeleteScene>(&command)) return controller.deleteScene(c->scene);
        if (const auto* c = std::get_if<RenameScene>(&command)) return controller.renameScene(c->scene, c->name);
        if (const auto* c = std::get_if<MoveScene>(&command)) {
            const int delta = c->to > c->scene ? 1 : -1;
            for (int s = c->scene; s != c->to; s += delta) if (!controller.moveScene(s, delta)) return false;
            return true;
        }
        if (const auto* c = std::get_if<DeleteClip>(&command)) return controller.applyDeleteClipCommand(c->track, c->scene);
        if(const auto edit=midiEdit(command)) return controller.applyMidiClipCommand(*edit);
        if (std::holds_alternative<CreateTrack>(command)) return controller.addTrack();
        if (const auto* c = std::get_if<DeleteTrack>(&command)) return controller.deleteTrack(c->track);
        if (const auto* c = std::get_if<MoveTrack>(&command)) {
            const int step = c->to > c->track ? 1 : -1;
            for (int t = c->track; t != c->to; t += step) if (!controller.moveTrack(t, step)) return false;
            return true;
        }
        if (const auto* c = std::get_if<RenameTrack>(&command)) return controller.renameTrack(c->track, c->name);
        if (const auto* c = std::get_if<SetTrackVolume>(&command)) return controller.setTrackVolume(c->track, c->gain);
        if (const auto* c = std::get_if<SetTrackPan>(&command)) return controller.setTrackPan(c->track, c->pan);
        if (const auto* c = std::get_if<SetTrackMute>(&command)) return controller.setTrackMute(c->track, c->muted);
        if (const auto* c = std::get_if<SetTrackSolo>(&command)) return controller.setTrackSolo(c->track, c->soloed);
        const auto& c = std::get<ColorTrack>(command);
        return controller.setTrackColour(c.track, c.colour);
    }

    ProjectController& controller;
};

// The transport's tempo control. Wheel steps within a moment are one Undo step; a drag calls
// this once, at release. Either way the engine ends on the project's tempo, even if refused.
inline Result setProjectTempo(ProjectController& controller, int bpm) {
    const auto result = CommandEngine(controller).execute(SetTempo{bpm}, "tempo");
    controller.endTempoAudition();
    return result;
}

// The entry points for audio bound for a Session cell. Each ends in ImportClipAudio,
// so there is one set of checks, one reducer and one Undo step.

// Installs a finished decode and tells the person what happened.
inline Result importPreparedAudio(ProjectController& controller, session_audio_import::Prepared prepared) {
    const auto shared = std::make_shared<const session_audio_import::Prepared>(std::move(prepared));
    const auto result = CommandEngine(controller).execute(ImportClipAudio{shared});
    controller.audioImportFinished(*shared, result.error);
    return result;
}

namespace detail {
inline void decodeThenImport(ProjectController& controller, session_audio_import::Plan plan) {
    controller.decodeAudioImport(std::move(plan), [&controller](const session_audio_import::Prepared& prepared) { importPreparedAudio(controller, prepared); });
}
} // namespace detail

// A file for a Session cell, replacing any audio it holds (the Import audio button).
inline void importAudioFile(ProjectController& controller, const juce::File& file, int track, int scene) {
    auto plan = controller.prepareAudioImport(file, track, scene);
    if (!plan) { controller.reportAudioImportRefusal("Choose an editable Session cell and stop recording before importing audio.", file); return; }
    detail::decodeThenImport(controller, std::move(*plan));
}

// A file dropped from the sample browser or Explorer: only an empty cell takes it.
inline void importSampleToCell(ProjectController& controller, const juce::File& file, int track, int scene) {
    if (!controller.canEditMidiClips()) return;
    auto plan = controller.prepareAudioImport(file, track, scene, true);
    if (!plan) { controller.reportAudioImportRefusal("Choose an empty, unfrozen Session cell.", file); return; }
    detail::decodeThenImport(controller, std::move(*plan));
}

// Audio that was decoded elsewhere: a finished take or a tempo render.
inline Result installDecodedAudio(ProjectController& controller, const juce::File& file, int track, int scene, std::shared_ptr<SessionAudioClip> clip) {
    const auto plan = controller.prepareAudioImport(file, track, scene);
    if (!plan) {
        Result refused; refused.error = "That Session cell cannot take audio right now.";
        controller.reportAudioImportRefusal(refused.error, file);
        return refused;
    }
    return importPreparedAudio(controller, {*plan, {std::move(clip), {}}});
}

} // namespace alien::command
