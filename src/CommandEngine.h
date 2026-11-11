#pragma once
#include "ProjectController.h"
#include "SceneCommands.h"
#include "DeviceCommands.h"
#include "TempoCommands.h"
#include "RackChannelCommands.h"
#include "RackPatternCommands.h"
#include "SongMarkerCommands.h"
#include "SongClipCommands.h"
#include "AutomationCommands.h"
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

// The Channel Rack's channels, zero-based. AddRackChannel takes a built-in sound id, or nothing for an empty sampler.
struct AddRackChannel { juce::String sound; };
struct AddRackSynthChannel { int preset{-1}; };
struct RemoveRackChannel { int channel{-1}; };
struct MoveRackChannel { int channel{-1}, to{-1}; };
struct DuplicateRackChannel { int channel{-1}; std::uint64_t instance{rack_channel::freshInstance()}; };   // carries the copy's plugin identity
struct RenameRackChannel { int channel{-1}; juce::String name; };
struct SetRackChannelVolume { int channel{-1}; double volume{}; };
struct SetRackChannelPan { int channel{-1}; double pan{}; };
struct SetRackChannelMute { int channel{-1}; bool muted{}; };
struct SetRackChannelSolo { int channel{-1}; bool soloed{}; };
struct SetRackChannelOutput { int channel{-1}, output{rack::masterOutput}; };   // -1 is the master
struct SetRackChannelChoke { int channel{-1}, group{}; };
// The Channel Rack's steps and patterns, zero-based. A step value's field is its name: velocity, pan, pitch, probability, gate, nudge or ratchet.
struct SetRackStep { int pattern{-1}, channel{-1}, step{-1}; bool on{}; };
struct SetRackStepValue { int pattern{-1}, channel{-1}, step{-1}; juce::String field; int value{}; };
struct FillRackSteps { int pattern{-1}, channel{-1}, every{}; };
struct RotateRackSteps { int pattern{-1}, channel{-1}, distance{}; };
struct ReverseRackSteps { int pattern{-1}, channel{-1}; };
struct RepeatRackSteps { int pattern{-1}, channel{-1}, period{}; };
struct ClearRackSteps { int pattern{-1}, channel{-1}; };
struct SetRackPatternLength { int pattern{-1}, length{}; };
struct SetRackSwing { double amount{}; };   // 0 is straight, 1 the most shuffled; applies to every pattern
struct CloneRackPattern { int pattern{-1}; };
struct ClearRackPattern { int pattern{-1}; };
struct RenameRackPattern { int pattern{-1}; juce::String name; };
struct DeleteRackPattern { int pattern{-1}; };
// What a rack channel plays. A plugin's identity is carried, so Preview and execute agree. Copied steps exist only as data, so PasteRackSteps has no script form.
struct SetRackChannelSound { int channel{-1}; juce::String sound; };
struct SilenceRackChannel { int channel{-1}; };
struct SetRackChannelSynth { int channel{-1}, preset{-1}; };
struct SetRackChannelPlugin { int channel{-1}; juce::String description, name; std::uint64_t instance{rack_channel::freshInstance()}; };
struct PasteRackSteps { int pattern{-1}, channel{-1}; std::shared_ptr<const rack::Project::StepRow> row; };
// Edits that carry their content as data, so like PasteRackSteps none has a script form.
struct SetRackChannelNotes { int pattern{-1}, channel{-1}; std::shared_ptr<const PianoRollPattern> notes; };
struct ImportRackGroove { std::shared_ptr<const midi_import::ImportedClip> groove; juce::String name; };
struct LoadRackSample { int channel{-1}; juce::String path, name; std::shared_ptr<SessionAudioClip> clip; };
// Song markers and the loop range. A marker is named by its id, which lasts while the project is open
// (a project gives its markers new ids when it opens). AddSongMarker carries its new marker's id, so Preview and execute agree.
struct AddSongMarker { std::int64_t tick{}; juce::String name; int colour{}; std::uint32_t marker{timeline::newId()}; };
struct MoveSongMarker { std::uint32_t marker{}; std::int64_t tick{}; };
struct RenameSongMarker { std::uint32_t marker{}; juce::String name; };
struct ColourSongMarker { std::uint32_t marker{}; int colour{}; };
struct RemoveSongMarker { std::uint32_t marker{}; };
struct SetSongLoop { std::int64_t start{}, end{}; bool enabled{true}; };
struct SetSongLength { int bars{}; };
// Song sections and chords, named by their ids like markers; the adds carry their new id. A section or chord runs
// from `start` to `end` (ticks, end exclusive). A chord's root is 0 (C) to 11 (B) and its quality a ChordType.
struct AddSongSection { std::int64_t start{}, end{}; juce::String name; int colour{}; std::uint32_t section{timeline::newId()}; };
struct EditSongSection { std::uint32_t section{}; std::int64_t start{}, end{}; juce::String name; int colour{}; };
struct RemoveSongSection { std::uint32_t section{}; };
struct AddSongChord { std::int64_t start{}, end{}; int root{}; ChordType quality{}; std::uint32_t chord{timeline::newId()}; };
struct EditSongChord { std::uint32_t chord{}; std::int64_t start{}, end{}; int root{}; ChordType quality{}; };
struct RemoveSongChord { std::uint32_t chord{}; };
// Clips placed in the Song, named by their region ids (which last while the project is open). A move or slip is a
// distance in ticks; trims, fade handles and crossfades act on one clip (a crossfade on a touching left and right pair).
// GroupSongClips carries its new group's id, so Preview and execute agree.
struct MoveSongClips { timeline::Ids clips; std::int64_t ticks{}; int lanes{}; };
struct TrimSongClipStart { std::uint32_t clip{}; std::int64_t tick{}; };
struct TrimSongClipEnd { std::uint32_t clip{}; std::int64_t tick{}; };
struct SlipSongClips { timeline::Ids clips; std::int64_t ticks{}; };
struct DeleteSongClips { timeline::Ids clips; };
struct SetSongClipsMuted { timeline::Ids clips; bool muted{}; };
struct SetSongClipGain { timeline::Ids clips; double gain{1.0}; };
struct SetSongClipFades { timeline::Ids clips; std::int64_t fadeIn{}, fadeOut{}; };
struct SetSongClipFadeIn { std::uint32_t clip{}; std::int64_t ticks{}; };
struct SetSongClipFadeOut { std::uint32_t clip{}; std::int64_t ticks{}; };
struct SetSongClipFadeShape { timeline::Ids clips; timeline::FadeEdge edge{}; timeline::FadeCurve curve{}; };
struct RemoveSongClipFades { timeline::Ids clips; };
struct CrossfadeSongClips { std::uint32_t left{}, right{}; std::int64_t ticks{}; };
struct GroupSongClips { timeline::Ids clips; std::uint32_t group{timeline::newId()}; };
struct UngroupSongClips { timeline::Ids clips; };
struct LoopSongClips { timeline::Ids clips; bool looped{}; };
struct ReverseSongClips { timeline::Ids clips; bool reversed{}; };
// These make clips. The new ones take the next Song ids, which a Preview of them hands out too.
// CopySongClips copies the clips `ticks` later (earlier when negative) and rack patterns `lanes` lanes down, as one unit.
struct SplitSongClips { timeline::Ids clips; std::int64_t tick{}; };
struct DuplicateSongClips { timeline::Ids clips; };
struct CopySongClips { timeline::Ids clips; std::int64_t ticks{}; int lanes{}; };
// Recorded takes, by id. CompSongTake plays the take from `start` to `end` (ticks) on its track over what is
// there, as new Song clips; the take is kept. ForgetSongTake removes it from the take list; the comp and its
// audio stay.
struct CompSongTake { std::uint32_t take{}; std::int64_t start{}, end{}; };
struct ForgetSongTake { std::uint32_t take{}; };
// Warp markers pin an audio clip's source seconds to ticks of its content (counted like Region::offset).
// SetSongClipWarp sets the whole list: none turns warp off, two or more turn it on. The first and last markers
// bound the warped audio, so Add puts one between them and Move and Remove leave those two alone.
struct SetSongClipWarp { std::uint32_t clip{}; std::vector<timeline::WarpMarker> markers; };
struct AddSongWarpMarker { std::uint32_t clip{}; std::int64_t tick{}; };
struct MoveSongWarpMarker { std::uint32_t clip{}; int marker{}; std::int64_t tick{}; };
struct RemoveSongWarpMarker { std::uint32_t clip{}; int marker{}; };
// Automation lanes: a track's volume, pan or send lane, or a plugin parameter's lane (see automation_edit::LaneRef).
// A point is named by its tick; one at a tick that already has a point replaces it. Adding a lane's first point
// makes the lane, and removing its last point leaves the lane empty. A value is the parameter's own (linear gain
// 0 to 2 for volume, -1 to 1 for pan, 0 to 1 for a send or a plugin parameter); curve 0 is linear, 1 is hold.
struct SetAutomationRead { automation_edit::LaneRef lane; bool enabled{true}; };
struct AddAutomationPoint { automation_edit::LaneRef lane; std::int64_t tick{}; float value{}; automation::Curve curve{}; };
struct MoveAutomationPoint { automation_edit::LaneRef lane; std::int64_t from{}, tick{}; float value{}; automation::Curve curve{}; };
struct RemoveAutomationPoint { automation_edit::LaneRef lane; std::int64_t tick{}; };
// Puts a Session cell (`row` is a track, `source` a scene) or a rack pattern (`row` is a rack lane row, from
// timeline::rackRowBase) in the Song at `start`, over whatever is there. `length` 0 is the clip's own length.
struct PlaceSongClip { int row{}, source{}; std::int64_t start{}, length{}; };
// Pastes copied Song clips at `at` (ticks), moving rack patterns `lanes` lanes, over what is there. A placement clip that
// was cut or edited since it was copied comes back with its copied notes, as part of the paste. The copied clips exist
// only as data, so like PasteRackSteps it has no script form.
struct PasteSongClips { std::shared_ptr<const song_clip::Pasted> clips; std::int64_t at{}; int lanes{}; };
// Slices a channel playing an audio file into new channels after it (T2-01): one for each part between consecutive
// `bounds` (the file's frames, ascending), with the channel's mix settings and no steps. The channel stays.
struct SliceRackChannel { int channel{-1}; std::vector<std::int64_t> bounds; };

using Command = std::variant<CreateTrack, DeleteTrack, MoveTrack, RenameTrack, ColorTrack, SetTrackVolume, SetTrackPan, SetTrackMute, SetTrackSolo, CreateClip, DuplicateClip, MakeClipUnique, CreateScene, DuplicateScene, MoveScene, RenameScene, DeleteScene, DeleteClip, PasteClip, PasteLinkedClip, MoveClip, CreateRackPattern, AssignRackClip, RemoveClipAudio, SetMasterVolume, ImportClipAudio, SetTrackSend, SetTrackSendMode, CreateReturn, RenameReturn, DeleteReturn, SetReturnVolume, SetReturnPan, SetReturnMute, CreateBus, RenameBus, DeleteBus, SetBusVolume, SetBusPan, SetBusMute, SetBusSolo, SetTrackOutput, SetSidechain, InsertDevice, RemoveDevice, MoveDevice, SetDeviceBypass, SetTempo, SetTempoChange, MoveTempoChange, RemoveTempoChange, ClearTempoChanges, SetMeterChange, RemoveMeterChange, ClearMeterChanges, AddRackChannel, AddRackSynthChannel, RemoveRackChannel, MoveRackChannel, DuplicateRackChannel, RenameRackChannel, SetRackChannelVolume, SetRackChannelPan, SetRackChannelMute, SetRackChannelSolo, SetRackChannelOutput, SetRackChannelChoke, SetRackStep, SetRackStepValue, FillRackSteps, RotateRackSteps, ReverseRackSteps, RepeatRackSteps, ClearRackSteps, SetRackPatternLength, SetRackSwing, CloneRackPattern, ClearRackPattern, RenameRackPattern, DeleteRackPattern, SetRackChannelSound, SilenceRackChannel, SetRackChannelSynth, SetRackChannelPlugin, PasteRackSteps, SetRackChannelNotes, ImportRackGroove, LoadRackSample, AddSongMarker, MoveSongMarker, RenameSongMarker, ColourSongMarker, RemoveSongMarker, SetSongLoop, MoveSongClips, TrimSongClipStart, TrimSongClipEnd, SlipSongClips, DeleteSongClips, SetSongClipsMuted, SetSongClipGain, SetSongClipFades, SetSongClipFadeIn, SetSongClipFadeOut, SetSongClipFadeShape, RemoveSongClipFades, CrossfadeSongClips, GroupSongClips, UngroupSongClips, LoopSongClips, ReverseSongClips, SplitSongClips, DuplicateSongClips, CopySongClips, AddSongSection, EditSongSection, RemoveSongSection, AddSongChord, EditSongChord, RemoveSongChord, SetSongLength, CompSongTake, ForgetSongTake, SetSongClipWarp, AddSongWarpMarker, MoveSongWarpMarker, RemoveSongWarpMarker, SetAutomationRead, AddAutomationPoint, MoveAutomationPoint, RemoveAutomationPoint, PlaceSongClip, PasteSongClips, SliceRackChannel>;

namespace detail {
// Indexed like Command's alternatives.
inline constexpr std::array<const char*, 134> names{"CreateTrack", "DeleteTrack", "MoveTrack", "RenameTrack", "ColorTrack", "SetTrackVolume", "SetTrackPan", "SetTrackMute", "SetTrackSolo", "CreateClip", "DuplicateClip", "MakeClipUnique", "CreateScene", "DuplicateScene", "MoveScene", "RenameScene", "DeleteScene", "DeleteClip", "PasteClip", "PasteLinkedClip", "MoveClip", "CreateRackPattern", "AssignRackClip", "RemoveClipAudio", "SetMasterVolume", "ImportClipAudio", "SetTrackSend", "SetTrackSendMode", "CreateReturn", "RenameReturn", "DeleteReturn", "SetReturnVolume", "SetReturnPan", "SetReturnMute", "CreateBus", "RenameBus", "DeleteBus", "SetBusVolume", "SetBusPan", "SetBusMute", "SetBusSolo", "SetTrackOutput", "SetSidechain", "InsertDevice", "RemoveDevice", "MoveDevice", "SetDeviceBypass", "SetTempo", "SetTempoChange", "MoveTempoChange", "RemoveTempoChange", "ClearTempoChanges", "SetMeterChange", "RemoveMeterChange", "ClearMeterChanges", "AddRackChannel", "AddRackSynthChannel", "RemoveRackChannel", "MoveRackChannel", "DuplicateRackChannel", "RenameRackChannel", "SetRackChannelVolume", "SetRackChannelPan", "SetRackChannelMute", "SetRackChannelSolo", "SetRackChannelOutput", "SetRackChannelChoke", "SetRackStep", "SetRackStepValue", "FillRackSteps", "RotateRackSteps", "ReverseRackSteps", "RepeatRackSteps", "ClearRackSteps", "SetRackPatternLength", "SetRackSwing", "CloneRackPattern", "ClearRackPattern", "RenameRackPattern", "DeleteRackPattern", "SetRackChannelSound", "SilenceRackChannel", "SetRackChannelSynth", "SetRackChannelPlugin", "PasteRackSteps", "SetRackChannelNotes", "ImportRackGroove", "LoadRackSample", "AddSongMarker", "MoveSongMarker", "RenameSongMarker", "ColourSongMarker", "RemoveSongMarker", "SetSongLoop", "MoveSongClips", "TrimSongClipStart", "TrimSongClipEnd", "SlipSongClips", "DeleteSongClips", "SetSongClipsMuted", "SetSongClipGain", "SetSongClipFades", "SetSongClipFadeIn", "SetSongClipFadeOut", "SetSongClipFadeShape", "RemoveSongClipFades", "CrossfadeSongClips", "GroupSongClips", "UngroupSongClips", "LoopSongClips", "ReverseSongClips", "SplitSongClips", "DuplicateSongClips", "CopySongClips", "AddSongSection", "EditSongSection", "RemoveSongSection", "AddSongChord", "EditSongChord", "RemoveSongChord", "SetSongLength", "CompSongTake", "ForgetSongTake", "SetSongClipWarp", "AddSongWarpMarker", "MoveSongWarpMarker", "RemoveSongWarpMarker", "SetAutomationRead", "AddAutomationPoint", "MoveAutomationPoint", "RemoveAutomationPoint", "PlaceSongClip", "PasteSongClips", "SliceRackChannel"};
inline constexpr std::array<const char*, 134> labels{"Add track", "Delete track", "Move track", "Rename track", "Colour track", "Set track volume", "Set track pan", "Set track mute", "Set track solo", "Create MIDI clip", "Duplicate clip", "Make clip independent", "Add scene", "Duplicate scene", "Move scene", "Rename scene", "Delete scene", "Clear clip", "Paste clip", "Paste linked clip", "Move clip", "New pattern", "Assign rack clip", "Remove audio", "Set master volume", "Import audio", "Set send level", "Set send mode", "Add return", "Rename return", "Remove return", "Set return volume", "Set return pan", "Set return mute", "Add bus", "Rename bus", "Delete bus", "Set bus volume", "Set bus pan", "Set bus mute", "Set bus solo", "Set track output", "Set sidechain", "Insert device", "Remove device", "Move device", "Set device bypass", "Change tempo", "Tempo change", "Move tempo change", "Remove tempo change", "Remove tempo changes", "Time signature change", "Remove time signature change", "Remove time signature changes", "Add channel", "Add synth channel", "Delete channel", "Move channel", "Duplicate channel", "Rename channel", "Change channel volume", "Change channel pan", "Mute channel", "Solo channel", "Route channel", "Change choke group", "Edit step", "Edit step", "Edit channel steps", "Edit channel steps", "Edit channel steps", "Edit channel steps", "Clear channel", "Change pattern length", "Change swing", "Clone pattern", "Clear pattern", "Rename pattern", "Delete pattern", "Change channel sound", "Silence channel", "Change channel instrument", "Change channel instrument", "Paste steps", "Edit channel notes", "Import drum groove", "Load channel sample", "Add marker", "Move marker", "Rename marker", "Colour marker", "Delete marker", "Set loop", "Move clips", "Trim clip start", "Resize clip", "Slip clips", "Delete clips", "Mute clips", "Set clip gain", "Fade clips", "Fade in", "Fade out", "Fade shape", "Remove fades", "Crossfade clips", "Group clips", "Ungroup clips", "Loop clip source", "Reverse clips", "Split", "Duplicate", "Copy clips", "Add Song section", "Edit Song section", "Delete Song section", "Add Song chord", "Edit Song chord", "Delete Song chord", "Change song length", "Comp recording take", "Forget recording take", "Set warp markers", "Add warp marker", "Move warp marker", "Remove warp marker", "Set automation read", "Add automation point", "Move automation point", "Remove automation point", "Place Song clip", "Paste", "Slice channel"};
static_assert(names.size() == std::variant_size_v<Command> && labels.size() == names.size());

// SetSongClipFadeShape's "edge" argument.
inline const char* fadeEdgeName(timeline::FadeEdge edge) noexcept {
    return edge == timeline::FadeEdge::in ? "in" : edge == timeline::FadeEdge::out ? "out" : "crossfade";
}
inline std::optional<timeline::FadeEdge> fadeEdgeNamed(const juce::String& name) noexcept {
    if (name == "in") return timeline::FadeEdge::in;
    if (name == "out") return timeline::FadeEdge::out;
    if (name == "crossfade") return timeline::FadeEdge::crossfade;
    return std::nullopt;
}

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

inline std::optional<rack_channel::Edit> rackChannelEdit(const Command& command) {
    using Kind = rack_channel::Kind;
    rack_channel::Edit e;
    if (const auto* c = std::get_if<AddRackChannel>(&command)) { e.kind = Kind::add; e.text = c->sound; return e; }
    if (const auto* c = std::get_if<AddRackSynthChannel>(&command)) { e.kind = Kind::addSynth; e.value = c->preset; return e; }
    if (const auto* c = std::get_if<RemoveRackChannel>(&command)) { e.kind = Kind::remove; e.channel = c->channel; return e; }
    if (const auto* c = std::get_if<MoveRackChannel>(&command)) { e.kind = Kind::move; e.channel = c->channel; e.value = c->to; return e; }
    if (const auto* c = std::get_if<DuplicateRackChannel>(&command)) { e.kind = Kind::duplicate; e.channel = c->channel; e.instance = c->instance; return e; }
    if (const auto* c = std::get_if<RenameRackChannel>(&command)) { e.kind = Kind::rename; e.channel = c->channel; e.text = c->name; return e; }
    if (const auto* c = std::get_if<SetRackChannelVolume>(&command)) { e.kind = Kind::volume; e.channel = c->channel; e.level = c->volume; return e; }
    if (const auto* c = std::get_if<SetRackChannelPan>(&command)) { e.kind = Kind::pan; e.channel = c->channel; e.level = c->pan; return e; }
    if (const auto* c = std::get_if<SetRackChannelMute>(&command)) { e.kind = Kind::mute; e.channel = c->channel; e.on = c->muted; return e; }
    if (const auto* c = std::get_if<SetRackChannelSolo>(&command)) { e.kind = Kind::solo; e.channel = c->channel; e.on = c->soloed; return e; }
    if (const auto* c = std::get_if<SetRackChannelOutput>(&command)) { e.kind = Kind::output; e.channel = c->channel; e.value = c->output; return e; }
    if (const auto* c = std::get_if<SetRackChannelChoke>(&command)) { e.kind = Kind::choke; e.channel = c->channel; e.value = c->group; return e; }
    if (const auto* c = std::get_if<SetRackChannelSound>(&command)) { e.kind = Kind::sound; e.channel = c->channel; e.text = c->sound; return e; }
    if (const auto* c = std::get_if<SilenceRackChannel>(&command)) { e.kind = Kind::silence; e.channel = c->channel; return e; }
    if (const auto* c = std::get_if<SetRackChannelSynth>(&command)) { e.kind = Kind::synth; e.channel = c->channel; e.value = c->preset; return e; }
    if (const auto* c = std::get_if<SetRackChannelPlugin>(&command)) { e.kind = Kind::plugin; e.channel = c->channel; e.description = c->description; e.text = c->name; e.instance = c->instance; return e; }
    if (const auto* c = std::get_if<LoadRackSample>(&command)) { e.kind = Kind::sample; e.channel = c->channel; e.description = c->path; e.text = c->name; return e; }
    if (const auto* c = std::get_if<SliceRackChannel>(&command)) { e.kind = Kind::slice; e.channel = c->channel; e.bounds = c->bounds; return e; }
    return std::nullopt;
}

inline std::optional<song_clip::Edit> songClipEdit(const Command& command) {
    using Kind = song_clip::Kind;
    song_clip::Edit e;
    if (const auto* c = std::get_if<MoveSongClips>(&command)) { e.kind = Kind::move; e.clips = c->clips; e.ticks = c->ticks; e.lanes = c->lanes; return e; }
    if (const auto* c = std::get_if<TrimSongClipStart>(&command)) { e.kind = Kind::trimStart; e.clips = {c->clip}; e.ticks = c->tick; return e; }
    if (const auto* c = std::get_if<TrimSongClipEnd>(&command)) { e.kind = Kind::trimEnd; e.clips = {c->clip}; e.ticks = c->tick; return e; }
    if (const auto* c = std::get_if<SlipSongClips>(&command)) { e.kind = Kind::slip; e.clips = c->clips; e.ticks = c->ticks; return e; }
    if (const auto* c = std::get_if<DeleteSongClips>(&command)) { e.kind = Kind::remove; e.clips = c->clips; return e; }
    if (const auto* c = std::get_if<SetSongClipsMuted>(&command)) { e.kind = Kind::mute; e.clips = c->clips; e.on = c->muted; return e; }
    if (const auto* c = std::get_if<SetSongClipGain>(&command)) { e.kind = Kind::gain; e.clips = c->clips; e.gain = c->gain; return e; }
    if (const auto* c = std::get_if<SetSongClipFades>(&command)) { e.kind = Kind::fades; e.clips = c->clips; e.ticks = c->fadeIn; e.fadeOut = c->fadeOut; return e; }
    if (const auto* c = std::get_if<SetSongClipFadeIn>(&command)) { e.kind = Kind::fadeIn; e.clips = {c->clip}; e.ticks = c->ticks; return e; }
    if (const auto* c = std::get_if<SetSongClipFadeOut>(&command)) { e.kind = Kind::fadeOut; e.clips = {c->clip}; e.ticks = c->ticks; return e; }
    if (const auto* c = std::get_if<SetSongClipFadeShape>(&command)) { e.kind = Kind::fadeShape; e.clips = c->clips; e.edge = c->edge; e.curve = c->curve; return e; }
    if (const auto* c = std::get_if<RemoveSongClipFades>(&command)) { e.kind = Kind::clearFades; e.clips = c->clips; return e; }
    if (const auto* c = std::get_if<CrossfadeSongClips>(&command)) { e.kind = Kind::crossfade; e.clips = {c->left, c->right}; e.ticks = c->ticks; return e; }
    if (const auto* c = std::get_if<GroupSongClips>(&command)) { e.kind = Kind::group; e.clips = c->clips; e.group = c->group; return e; }
    if (const auto* c = std::get_if<UngroupSongClips>(&command)) { e.kind = Kind::ungroup; e.clips = c->clips; return e; }
    if (const auto* c = std::get_if<LoopSongClips>(&command)) { e.kind = Kind::loopSource; e.clips = c->clips; e.on = c->looped; return e; }
    if (const auto* c = std::get_if<ReverseSongClips>(&command)) { e.kind = Kind::reverse; e.clips = c->clips; e.on = c->reversed; return e; }
    if (const auto* c = std::get_if<SplitSongClips>(&command)) { e.kind = Kind::split; e.clips = c->clips; e.ticks = c->tick; return e; }
    if (const auto* c = std::get_if<DuplicateSongClips>(&command)) { e.kind = Kind::duplicate; e.clips = c->clips; return e; }
    if (const auto* c = std::get_if<CopySongClips>(&command)) { e.kind = Kind::copy; e.clips = c->clips; e.ticks = c->ticks; e.lanes = c->lanes; return e; }
    if (const auto* c = std::get_if<CompSongTake>(&command)) { e.kind = Kind::comp; e.take = c->take; e.ticks = c->start; e.end = c->end; return e; }
    if (const auto* c = std::get_if<ForgetSongTake>(&command)) { e.kind = Kind::forget; e.take = c->take; return e; }
    if (const auto* c = std::get_if<SetSongClipWarp>(&command)) { e.kind = Kind::setWarp; e.clips = {c->clip}; e.warp = c->markers; return e; }
    if (const auto* c = std::get_if<AddSongWarpMarker>(&command)) { e.kind = Kind::addWarp; e.clips = {c->clip}; e.ticks = c->tick; return e; }
    if (const auto* c = std::get_if<MoveSongWarpMarker>(&command)) { e.kind = Kind::moveWarp; e.clips = {c->clip}; e.index = c->marker; e.ticks = c->tick; return e; }
    if (const auto* c = std::get_if<RemoveSongWarpMarker>(&command)) { e.kind = Kind::removeWarp; e.clips = {c->clip}; e.index = c->marker; return e; }
    if (const auto* c = std::get_if<PlaceSongClip>(&command)) { e.kind = Kind::place; e.row = c->row; e.source = c->source; e.ticks = c->start; e.length = c->length; return e; }
    if (const auto* c = std::get_if<PasteSongClips>(&command)) { e.kind = Kind::paste; e.pasted = c->clips; e.ticks = c->at; e.lanes = c->lanes; return e; }
    return std::nullopt;
}

inline std::optional<automation_edit::Edit> automationEdit(const Command& command) {
    using Kind = automation_edit::Kind;
    automation_edit::Edit e;
    if (const auto* c = std::get_if<SetAutomationRead>(&command)) { e.kind = Kind::setRead; e.lane = c->lane; e.enabled = c->enabled; return e; }
    if (const auto* c = std::get_if<AddAutomationPoint>(&command)) { e.kind = Kind::addPoint; e.lane = c->lane; e.point = {c->tick, c->value, c->curve}; return e; }
    if (const auto* c = std::get_if<MoveAutomationPoint>(&command)) { e.kind = Kind::movePoint; e.lane = c->lane; e.from = c->from; e.point = {c->tick, c->value, c->curve}; return e; }
    if (const auto* c = std::get_if<RemoveAutomationPoint>(&command)) { e.kind = Kind::removePoint; e.lane = c->lane; e.from = c->tick; return e; }
    return std::nullopt;
}
inline std::optional<song_edit::Edit> songEdit(const Command& command) {
    using Kind = song_edit::Kind;
    song_edit::Edit e;
    if (const auto* c = std::get_if<AddSongMarker>(&command)) { e.kind = Kind::addMarker; e.marker = c->marker; e.tick = c->tick; e.name = c->name.toStdString(); e.colour = c->colour; return e; }
    if (const auto* c = std::get_if<MoveSongMarker>(&command)) { e.kind = Kind::moveMarker; e.marker = c->marker; e.tick = c->tick; return e; }
    if (const auto* c = std::get_if<RenameSongMarker>(&command)) { e.kind = Kind::renameMarker; e.marker = c->marker; e.name = c->name.toStdString(); return e; }
    if (const auto* c = std::get_if<ColourSongMarker>(&command)) { e.kind = Kind::colourMarker; e.marker = c->marker; e.colour = c->colour; return e; }
    if (const auto* c = std::get_if<RemoveSongMarker>(&command)) { e.kind = Kind::removeMarker; e.marker = c->marker; return e; }
    if (const auto* c = std::get_if<SetSongLoop>(&command)) { e.kind = Kind::setLoop; e.tick = c->start; e.end = c->end; e.enabled = c->enabled; return e; }
    if (const auto* c = std::get_if<SetSongLength>(&command)) { e.kind = Kind::setLength; e.bars = c->bars; return e; }
    if (const auto* c = std::get_if<AddSongSection>(&command)) { e.kind = Kind::addSection; e.marker = c->section; e.tick = c->start; e.end = c->end; e.name = c->name.toStdString(); e.colour = c->colour; return e; }
    if (const auto* c = std::get_if<EditSongSection>(&command)) { e.kind = Kind::editSection; e.marker = c->section; e.tick = c->start; e.end = c->end; e.name = c->name.toStdString(); e.colour = c->colour; return e; }
    if (const auto* c = std::get_if<RemoveSongSection>(&command)) { e.kind = Kind::removeSection; e.marker = c->section; return e; }
    if (const auto* c = std::get_if<AddSongChord>(&command)) { e.kind = Kind::addChord; e.marker = c->chord; e.tick = c->start; e.end = c->end; e.root = c->root; e.quality = c->quality; return e; }
    if (const auto* c = std::get_if<EditSongChord>(&command)) { e.kind = Kind::editChord; e.marker = c->chord; e.tick = c->start; e.end = c->end; e.root = c->root; e.quality = c->quality; return e; }
    if (const auto* c = std::get_if<RemoveSongChord>(&command)) { e.kind = Kind::removeChord; e.marker = c->chord; return e; }
    return std::nullopt;
}

inline std::optional<rack_pattern::Edit> rackPatternEdit(const Command& command) {
    using Kind = rack_pattern::Kind;
    rack_pattern::Edit e;
    if (const auto* c = std::get_if<SetRackStep>(&command)) { e.kind = Kind::setStep; e.pattern = c->pattern; e.channel = c->channel; e.step = c->step; e.on = c->on; return e; }
    if (const auto* c = std::get_if<SetRackStepValue>(&command)) { e.kind = Kind::setStepValue; e.pattern = c->pattern; e.channel = c->channel; e.step = c->step; e.text = c->field; e.value = c->value; return e; }
    if (const auto* c = std::get_if<FillRackSteps>(&command)) { e.kind = Kind::fill; e.pattern = c->pattern; e.channel = c->channel; e.value = c->every; return e; }
    if (const auto* c = std::get_if<RotateRackSteps>(&command)) { e.kind = Kind::rotate; e.pattern = c->pattern; e.channel = c->channel; e.value = c->distance; return e; }
    if (const auto* c = std::get_if<ReverseRackSteps>(&command)) { e.kind = Kind::reverse; e.pattern = c->pattern; e.channel = c->channel; return e; }
    if (const auto* c = std::get_if<RepeatRackSteps>(&command)) { e.kind = Kind::repeat; e.pattern = c->pattern; e.channel = c->channel; e.value = c->period; return e; }
    if (const auto* c = std::get_if<ClearRackSteps>(&command)) { e.kind = Kind::clearSteps; e.pattern = c->pattern; e.channel = c->channel; return e; }
    if (const auto* c = std::get_if<SetRackPatternLength>(&command)) { e.kind = Kind::length; e.pattern = c->pattern; e.value = c->length; return e; }
    if (const auto* c = std::get_if<SetRackSwing>(&command)) { e.kind = Kind::swing; e.amount = c->amount; return e; }
    if (const auto* c = std::get_if<CloneRackPattern>(&command)) { e.kind = Kind::clone; e.pattern = c->pattern; return e; }
    if (const auto* c = std::get_if<ClearRackPattern>(&command)) { e.kind = Kind::clear; e.pattern = c->pattern; return e; }
    if (const auto* c = std::get_if<RenameRackPattern>(&command)) { e.kind = Kind::rename; e.pattern = c->pattern; e.text = c->name; return e; }
    if (const auto* c = std::get_if<DeleteRackPattern>(&command)) { e.kind = Kind::remove; e.pattern = c->pattern; return e; }
    if (const auto* c = std::get_if<PasteRackSteps>(&command)) { e.kind = Kind::paste; e.pattern = c->pattern; e.channel = c->channel; e.row = c->row; return e; }
    if (const auto* c = std::get_if<SetRackChannelNotes>(&command)) { e.kind = Kind::notes; e.pattern = c->pattern; e.channel = c->channel; e.notes = c->notes; return e; }
    if (const auto* c = std::get_if<ImportRackGroove>(&command)) { e.kind = Kind::groove; e.groove = c->groove; e.text = c->name; return e; }
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
    if (const auto* c = std::get_if<LoadRackSample>(&command); c != nullptr && c->clip == nullptr) return "There is no decoded audio to load.";
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
    if (const auto edit = rackChannelEdit(command)) return rack_channel::refusal(d, *edit);
    if (const auto edit = rackPatternEdit(command)) return rack_pattern::refusal(d, *edit);
    if (const auto edit = songEdit(command)) return song_edit::refusal(d, *edit);
    if (const auto edit = songClipEdit(command)) return song_clip::refusal(d, *edit);
    if (const auto edit = automationEdit(command)) return automation_edit::refusal(d, *edit);
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
    if (const auto edit = rackChannelEdit(command)) {
        using Kind = rack_channel::Kind;
        if (edit->kind == Kind::add) return edit->text.isEmpty() ? juce::String("Add an empty sampler channel") : "Add a channel playing \"" + edit->text + "\"";
        if (edit->kind == Kind::addSynth) return "Add a synth channel playing " + juce::String(alien::presetNames[static_cast<std::size_t>(edit->value)]);
        const auto channel = "rack channel " + juce::String(edit->channel + 1) + " \"" + juce::String::fromUTF8(d.channelRack.info[static_cast<std::size_t>(edit->channel)].name.c_str()) + "\"";
        switch (edit->kind) {
        case Kind::remove: return "Delete " + channel;
        case Kind::move: return "Move " + channel + " to position " + juce::String(edit->value + 1);
        case Kind::duplicate: return "Duplicate " + channel;
        case Kind::rename: return "Rename " + channel + " to \"" + edit->text + "\"";
        case Kind::volume: return "Set the volume of " + channel + " to " + juce::String(edit->level, 2);
        case Kind::pan: return "Set the pan of " + channel + " to " + juce::String(edit->level, 2);
        case Kind::mute: return juce::String(edit->on ? "Mute " : "Unmute ") + channel;
        case Kind::solo: return juce::String(edit->on ? "Solo " : "Unsolo ") + channel;
        case Kind::output: return "Route " + channel + (edit->value < 0 ? juce::String(" to the master") : " to " + detail::trackTitle(d, edit->value));
        case Kind::choke: return "Put " + channel + (edit->value == 0 ? juce::String(" in no choke group") : " in choke group " + juce::String(edit->value));
        case Kind::sound: return "Give " + channel + " the built-in sound \"" + edit->text + "\"";
        case Kind::silence: return "Silence " + channel;
        case Kind::synth: return "Give " + channel + " the synth preset " + juce::String(alien::presetNames[static_cast<std::size_t>(edit->value)]);
        case Kind::plugin: return "Give " + channel + " the plugin instrument \"" + edit->text + "\"";
        case Kind::sample: return "Load \"" + edit->text + "\" into " + channel;
        case Kind::slice: return "Slice " + channel + " into " + juce::String(static_cast<int>(edit->bounds.size()) - 1) + " channels";
        case Kind::add: case Kind::addSynth: break;
        }
    }
    if (const auto edit = automationEdit(command)) {
        const auto& r = edit->lane;
        const auto lane = r.isPlugin()
            ? "the automation of plugin " + juce::String(static_cast<juce::uint64>(r.plugin.instance)) + "'s parameter \"" + juce::String::fromUTF8(r.plugin.parameterId.c_str()) + "\""
            : "the " + juce::String(automation_edit::parameterName(r.parameter)) + " automation of " + detail::trackTitle(d, r.track);
        const auto point = [](const automation::Point& p) {
            return "tick " + juce::String(p.tick) + ", value " + juce::String(p.value, 3) + (p.curve == automation::Curve::hold ? ", held" : "");
        };
        switch (edit->kind) {
        case automation_edit::Kind::setRead: return juce::String(edit->enabled ? "Play " : "Stop playing ") + lane;
        case automation_edit::Kind::addPoint: return "Add a point at " + point(edit->point) + " to " + lane;
        case automation_edit::Kind::movePoint: return "Move the point at tick " + juce::String(edit->from) + " of " + lane + " to " + point(edit->point);
        case automation_edit::Kind::removePoint: return "Delete the point at tick " + juce::String(edit->from) + " of " + lane;
        }
    }
    if (songEdit(command)) {
        const auto markerTitle = [&](std::uint32_t id) {
            const auto* m = d.timeline.findMarker(id);
            return m != nullptr ? "the marker \"" + juce::String::fromUTF8(m->name.c_str()) + "\"" : "marker " + juce::String(static_cast<juce::int64>(id));
        };
        if (const auto* c = std::get_if<AddSongMarker>(&command)) return "Add the marker \"" + c->name.trim() + "\" at tick " + juce::String(c->tick);
        if (const auto* c = std::get_if<MoveSongMarker>(&command)) return "Move " + markerTitle(c->marker) + " to tick " + juce::String(c->tick);
        if (const auto* c = std::get_if<RenameSongMarker>(&command)) return "Rename " + markerTitle(c->marker) + " to \"" + c->name.trim() + "\"";
        if (const auto* c = std::get_if<ColourSongMarker>(&command)) return "Give " + markerTitle(c->marker) + " colour " + juce::String(c->colour);
        if (const auto* c = std::get_if<RemoveSongMarker>(&command)) return "Delete " + markerTitle(c->marker);
        const auto range = [](std::int64_t start, std::int64_t end) { return " from tick " + juce::String(start) + " to " + juce::String(end); };
        const auto chordName = [](int root, ChordType quality) { return juce::String(timeline::chordRootName(root)) + " " + timeline::chordQualityName(quality); };
        const auto sectionTitle = [&](std::uint32_t id) {
            const auto* s = d.timeline.findSection(id);
            return s != nullptr ? "the section \"" + juce::String::fromUTF8(s->name.c_str()) + "\"" : "section " + juce::String(static_cast<juce::int64>(id));
        };
        const auto chordTitle = [&](std::uint32_t id) {
            const auto* c = d.timeline.findChord(id);
            return c != nullptr ? "the " + chordName(c->root, c->quality) + " chord at tick " + juce::String(c->start) : "chord " + juce::String(static_cast<juce::int64>(id));
        };
        if (const auto* c = std::get_if<AddSongSection>(&command)) return "Add the section \"" + c->name.trim() + "\"" + range(c->start, c->end);
        if (const auto* c = std::get_if<EditSongSection>(&command)) return "Make " + sectionTitle(c->section) + " \"" + c->name.trim() + "\", colour " + juce::String(c->colour) + "," + range(c->start, c->end);
        if (const auto* c = std::get_if<RemoveSongSection>(&command)) return "Delete " + sectionTitle(c->section);
        if (const auto* c = std::get_if<AddSongChord>(&command)) return "Add " + chordName(c->root, c->quality) + range(c->start, c->end);
        if (const auto* c = std::get_if<EditSongChord>(&command)) return "Make " + chordTitle(c->chord) + " " + chordName(c->root, c->quality) + range(c->start, c->end);
        if (const auto* c = std::get_if<RemoveSongChord>(&command)) return "Delete " + chordTitle(c->chord);
        if (const auto* c = std::get_if<SetSongLength>(&command)) return "Make the song " + juce::String(c->bars) + (c->bars == 1 ? " bar long" : " bars long");
        const auto& loop = std::get<SetSongLoop>(command);
        return "Loop ticks " + juce::String(loop.start) + " to " + juce::String(loop.end) + (loop.enabled ? ", with the loop on" : ", with the loop off");
    }
    if (const auto edit = songClipEdit(command)) {
        using Kind = song_clip::Kind;
        const auto count = edit->clips.size();
        const juce::String clips = count == 1 ? "Song clip " + juce::String(static_cast<juce::int64>(edit->clips[0])) : juce::String(static_cast<int>(count)) + " Song clips";
        const auto ticks = juce::String(edit->ticks);
        switch (edit->kind) {
        case Kind::move: return "Move " + clips + " by " + ticks + " ticks" + (edit->lanes != 0 ? " and " + juce::String(edit->lanes) + " lanes" : juce::String{});
        case Kind::trimStart: return "Trim the start of " + clips + " to tick " + ticks;
        case Kind::trimEnd: return "End " + clips + " at tick " + ticks;
        case Kind::slip: return "Slip the source under " + clips + " by " + ticks + " ticks";
        case Kind::remove: return "Delete " + clips;
        case Kind::mute: return (edit->on ? "Mute " : "Unmute ") + clips;
        case Kind::gain: return "Set the gain of " + clips + " to " + juce::String(edit->gain, 3);
        case Kind::fades: return "Fade " + clips + " in over " + ticks + " ticks and out over " + juce::String(edit->fadeOut);
        case Kind::fadeIn: return "Fade " + clips + " in over " + ticks + " ticks";
        case Kind::fadeOut: return "Fade " + clips + " out over " + ticks + " ticks";
        case Kind::fadeShape: return juce::String(edit->edge == timeline::FadeEdge::in ? "Shape the fade in of " : edit->edge == timeline::FadeEdge::out ? "Shape the fade out of " : "Shape the crossfade of ")
                                  + clips + " " + (timeline::validFadeCurve(edit->curve) ? juce::String(timeline::fadeCurveLabel(edit->curve)) : juce::String("?"));
        case Kind::clearFades: return "Remove the fades of " + clips;
        case Kind::crossfade: return "Crossfade Song clip " + juce::String(static_cast<juce::int64>(edit->clips[0])) + " into Song clip "
                                  + juce::String(static_cast<juce::int64>(edit->clips[1])) + " over " + ticks + " ticks";
        case Kind::group: return "Group " + clips;
        case Kind::ungroup: return "Ungroup " + clips;
        case Kind::loopSource: return (edit->on ? "Loop the source of " : "Stop looping the source of ") + clips;
        case Kind::reverse: return (edit->on ? "Reverse " : "Play forward ") + clips;
        case Kind::split: return "Split " + clips + " at tick " + ticks;
        case Kind::duplicate: return "Duplicate " + clips;
        case Kind::copy: return "Copy " + clips + " " + ticks + " ticks later" + (edit->lanes != 0 ? " and " + juce::String(edit->lanes) + " lanes down" : juce::String{});
        case Kind::comp: return "Comp ticks " + ticks + " to " + juce::String(edit->end) + " from take " + juce::String(static_cast<juce::int64>(edit->take));
        case Kind::forget: return "Forget take " + juce::String(static_cast<juce::int64>(edit->take));
        case Kind::setWarp: return edit->warp.empty() ? "Turn off warp for " + clips : "Warp " + clips + " with " + juce::String(static_cast<int>(edit->warp.size())) + " markers";
        case Kind::addWarp: return "Add a warp marker to " + clips + " at content tick " + ticks;
        case Kind::moveWarp: return "Move warp marker " + juce::String(edit->index) + " of " + clips + " to content tick " + ticks;
        case Kind::removeWarp: return "Remove warp marker " + juce::String(edit->index) + " of " + clips;
        case Kind::place: {
            const auto where = song_clip::placement(d, edit->row, edit->source);
            const auto what = timeline::isPatternRow(where.row)
                ? "rack pattern " + juce::String(where.source + 1) + " on rack lane " + juce::String(where.row - timeline::rackRowBase + 1)
                : "scene " + juce::String(edit->source + 1) + " of track " + juce::String(edit->row + 1);
            return "Place " + what + " in the song at tick " + ticks + (edit->length > 0 ? " for " + juce::String(edit->length) + " ticks" : juce::String{});
        }
        case Kind::paste: {
            const int pasted = edit->pasted != nullptr ? static_cast<int>(edit->pasted->clips.regions.size()) : 0;
            return "Paste " + juce::String(pasted) + (pasted == 1 ? " copied Song clip" : " copied Song clips") + " at tick " + ticks
                 + (edit->lanes != 0 ? ", rack patterns " + juce::String(edit->lanes) + (std::abs(edit->lanes) == 1 ? " lane down" : " lanes down") : juce::String{});
        }
        }
    }
    if (const auto edit = rackPatternEdit(command)) {
        using Kind = rack_pattern::Kind;
        if (edit->kind == Kind::swing) return "Set the rack's swing to " + juce::String(edit->amount, 2);
        if (edit->kind == Kind::groove) return "Import the drum groove \"" + edit->text + "\" as a new rack pattern";
        const auto pattern = "rack pattern " + juce::String(edit->pattern + 1) + " \"" + juce::String::fromUTF8(d.channelRack.patternNames[static_cast<std::size_t>(edit->pattern)].c_str()) + "\"";
        const auto channel = edit->channel >= 0 ? "rack channel " + juce::String(edit->channel + 1) + " \"" + juce::String::fromUTF8(d.channelRack.info[static_cast<std::size_t>(edit->channel)].name.c_str()) + "\" in " + pattern : juce::String{};
        switch (edit->kind) {
        case Kind::setStep: return juce::String(edit->on ? "Turn on" : "Turn off") + " step " + juce::String(edit->step + 1) + " of " + channel;
        case Kind::setStepValue: return "Set the " + edit->text + " of step " + juce::String(edit->step + 1) + " of " + channel + " to " + juce::String(edit->value);
        case Kind::fill: return "Fill every " + juce::String(edit->value) + " steps of " + channel;
        case Kind::rotate: return "Rotate the steps of " + channel + " by " + juce::String(edit->value);
        case Kind::reverse: return "Reverse the steps of " + channel;
        case Kind::repeat: return "Repeat the first " + juce::String(edit->value) + " steps of " + channel;
        case Kind::clearSteps: return "Clear the steps of " + channel;
        case Kind::paste: return "Paste copied steps into " + channel;
        case Kind::notes: return "Edit the notes of " + channel;
        case Kind::length: return "Set the length of " + pattern + " to " + juce::String(edit->value) + " steps";
        case Kind::clone: return "Clone " + pattern;
        case Kind::clear: return "Clear " + pattern;
        case Kind::rename: return "Rename " + pattern + " to \"" + edit->text + "\"";
        case Kind::remove: return "Delete " + pattern;
        case Kind::swing: case Kind::groove: break;
        }
    }
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
    if (const auto edit = rackChannelEdit(command)) return rack_channel::apply(d, *edit);
    if (const auto edit = rackPatternEdit(command)) return rack_pattern::apply(d, *edit);
    if (const auto edit = songEdit(command)) return song_edit::apply(d, *edit);
    if (const auto edit = songClipEdit(command)) return song_clip::apply(d, *edit);
    if (const auto edit = automationEdit(command)) return automation_edit::apply(d, *edit);
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
    if (const auto* c = std::get_if<AddRackChannel>(&command)) { object->setProperty("sound", c->sound); return result; }
    if (const auto* c = std::get_if<AddRackSynthChannel>(&command)) { object->setProperty("preset", c->preset); return result; }
    if (const auto* c = std::get_if<RemoveRackChannel>(&command)) { object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<MoveRackChannel>(&command)) { object->setProperty("channel", c->channel); object->setProperty("to", c->to); return result; }
    if (const auto* c = std::get_if<DuplicateRackChannel>(&command)) { object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<RenameRackChannel>(&command)) { object->setProperty("channel", c->channel); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<SetRackChannelVolume>(&command)) { object->setProperty("channel", c->channel); object->setProperty("volume", c->volume); return result; }
    if (const auto* c = std::get_if<SetRackChannelPan>(&command)) { object->setProperty("channel", c->channel); object->setProperty("pan", c->pan); return result; }
    if (const auto* c = std::get_if<SetRackChannelMute>(&command)) { object->setProperty("channel", c->channel); object->setProperty("muted", c->muted); return result; }
    if (const auto* c = std::get_if<SetRackChannelSolo>(&command)) { object->setProperty("channel", c->channel); object->setProperty("soloed", c->soloed); return result; }
    if (const auto* c = std::get_if<SetRackChannelOutput>(&command)) { object->setProperty("channel", c->channel); object->setProperty("output", c->output); return result; }
    if (const auto* c = std::get_if<SetRackChannelChoke>(&command)) { object->setProperty("channel", c->channel); object->setProperty("group", c->group); return result; }
    if (const auto* c = std::get_if<SetRackStep>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); object->setProperty("step", c->step); object->setProperty("on", c->on); return result; }
    if (const auto* c = std::get_if<SetRackStepValue>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); object->setProperty("step", c->step); object->setProperty("field", c->field); object->setProperty("value", c->value); return result; }
    if (const auto* c = std::get_if<FillRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); object->setProperty("every", c->every); return result; }
    if (const auto* c = std::get_if<RotateRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); object->setProperty("distance", c->distance); return result; }
    if (const auto* c = std::get_if<ReverseRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<RepeatRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); object->setProperty("period", c->period); return result; }
    if (const auto* c = std::get_if<ClearRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<SetRackPatternLength>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("length", c->length); return result; }
    if (const auto* c = std::get_if<SetRackSwing>(&command)) { object->setProperty("amount", c->amount); return result; }
    if (const auto* c = std::get_if<CloneRackPattern>(&command)) { object->setProperty("pattern", c->pattern); return result; }
    if (const auto* c = std::get_if<ClearRackPattern>(&command)) { object->setProperty("pattern", c->pattern); return result; }
    if (const auto* c = std::get_if<RenameRackPattern>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<DeleteRackPattern>(&command)) { object->setProperty("pattern", c->pattern); return result; }
    if (const auto* c = std::get_if<SetRackChannelSound>(&command)) { object->setProperty("channel", c->channel); object->setProperty("sound", c->sound); return result; }
    if (const auto* c = std::get_if<SilenceRackChannel>(&command)) { object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<SetRackChannelSynth>(&command)) { object->setProperty("channel", c->channel); object->setProperty("preset", c->preset); return result; }
    if (const auto* c = std::get_if<SetRackChannelPlugin>(&command)) { object->setProperty("channel", c->channel); object->setProperty("description", c->description); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<PasteRackSteps>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<SetRackChannelNotes>(&command)) { object->setProperty("pattern", c->pattern); object->setProperty("channel", c->channel); return result; }
    if (const auto* c = std::get_if<SliceRackChannel>(&command)) {
        object->setProperty("channel", c->channel);
        juce::Array<juce::var> bounds;
        for (const auto b : c->bounds) bounds.add(static_cast<juce::int64>(b));
        object->setProperty("bounds", bounds);
        return result;
    }
    if (const auto* c = std::get_if<ImportRackGroove>(&command)) { object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<LoadRackSample>(&command)) { object->setProperty("channel", c->channel); object->setProperty("path", c->path); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<SetTempo>(&command)) { object->setProperty("bpm", c->bpm); return result; }
    if (const auto* c = std::get_if<SetTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); object->setProperty("bpm", c->bpm); return result; }
    if (const auto* c = std::get_if<MoveTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); object->setProperty("to", static_cast<juce::int64>(c->to)); return result; }
    if (const auto* c = std::get_if<RemoveTempoChange>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); return result; }
    if (std::holds_alternative<ClearTempoChanges>(command) || std::holds_alternative<ClearMeterChanges>(command)) return result;
    if (const auto* c = std::get_if<SetMeterChange>(&command)) { object->setProperty("bar", c->bar); object->setProperty("numerator", c->numerator); object->setProperty("denominator", c->denominator); return result; }
    if (const auto* c = std::get_if<RemoveMeterChange>(&command)) { object->setProperty("bar", c->bar); return result; }
    if (const auto* c = std::get_if<AddSongMarker>(&command)) { object->setProperty("tick", static_cast<juce::int64>(c->tick)); object->setProperty("name", c->name); object->setProperty("colour", c->colour); return result; }
    if (const auto* c = std::get_if<MoveSongMarker>(&command)) { object->setProperty("marker", static_cast<juce::int64>(c->marker)); object->setProperty("tick", static_cast<juce::int64>(c->tick)); return result; }
    if (const auto* c = std::get_if<RenameSongMarker>(&command)) { object->setProperty("marker", static_cast<juce::int64>(c->marker)); object->setProperty("name", c->name); return result; }
    if (const auto* c = std::get_if<ColourSongMarker>(&command)) { object->setProperty("marker", static_cast<juce::int64>(c->marker)); object->setProperty("colour", c->colour); return result; }
    if (const auto* c = std::get_if<RemoveSongMarker>(&command)) { object->setProperty("marker", static_cast<juce::int64>(c->marker)); return result; }
    if (const auto* c = std::get_if<AddSongSection>(&command)) { object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); object->setProperty("name", c->name); object->setProperty("colour", c->colour); return result; }
    if (const auto* c = std::get_if<EditSongSection>(&command)) { object->setProperty("section", static_cast<juce::int64>(c->section)); object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); object->setProperty("name", c->name); object->setProperty("colour", c->colour); return result; }
    if (const auto* c = std::get_if<RemoveSongSection>(&command)) { object->setProperty("section", static_cast<juce::int64>(c->section)); return result; }
    if (const auto* c = std::get_if<AddSongChord>(&command)) { object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); object->setProperty("root", c->root); object->setProperty("quality", static_cast<int>(c->quality)); return result; }
    if (const auto* c = std::get_if<EditSongChord>(&command)) { object->setProperty("chord", static_cast<juce::int64>(c->chord)); object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); object->setProperty("root", c->root); object->setProperty("quality", static_cast<int>(c->quality)); return result; }
    if (const auto* c = std::get_if<RemoveSongChord>(&command)) { object->setProperty("chord", static_cast<juce::int64>(c->chord)); return result; }
    if (const auto* c = std::get_if<SetSongLoop>(&command)) { object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); object->setProperty("enabled", c->enabled); return result; }
    if (const auto* c = std::get_if<SetSongLength>(&command)) { object->setProperty("bars", c->bars); return result; }
    if (const auto edit = automationEdit(command)) {
        const auto& r = edit->lane;
        if (r.isPlugin()) {
            object->setProperty("instance", juce::String(static_cast<juce::uint64>(r.plugin.instance)));
            object->setProperty("parameterId", juce::String::fromUTF8(r.plugin.parameterId.c_str()));
        } else { object->setProperty("track", r.track); object->setProperty("parameter", juce::String(automation_edit::parameterName(r.parameter))); }
        using Kind = automation_edit::Kind;
        if (edit->kind == Kind::setRead) object->setProperty("enabled", edit->enabled);
        if (edit->kind == Kind::movePoint) object->setProperty("from", static_cast<juce::int64>(edit->from));
        if (edit->kind == Kind::removePoint) object->setProperty("tick", static_cast<juce::int64>(edit->from));
        if (edit->kind == Kind::addPoint || edit->kind == Kind::movePoint) {
            object->setProperty("tick", static_cast<juce::int64>(edit->point.tick));
            object->setProperty("value", static_cast<double>(edit->point.value));
            object->setProperty("curve", static_cast<int>(edit->point.curve));
        }
        return result;
    }
    if (const auto* c = std::get_if<CompSongTake>(&command)) { object->setProperty("take", static_cast<juce::int64>(c->take)); object->setProperty("start", static_cast<juce::int64>(c->start)); object->setProperty("end", static_cast<juce::int64>(c->end)); return result; }
    if (const auto* c = std::get_if<ForgetSongTake>(&command)) { object->setProperty("take", static_cast<juce::int64>(c->take)); return result; }
    if (const auto edit = songClipEdit(command)) {
        using Kind = song_clip::Kind;
        const auto id = [](std::uint32_t v) { return juce::var(static_cast<juce::int64>(v)); };
        if (edit->kind == Kind::paste) {
            object->setProperty("at", static_cast<juce::int64>(edit->ticks)); object->setProperty("lanes", edit->lanes);
            object->setProperty("clips", edit->pasted != nullptr ? static_cast<int>(edit->pasted->clips.regions.size()) : 0);
            return result;
        }
        if (edit->kind == Kind::place) {
            object->setProperty("row", edit->row); object->setProperty("source", edit->source);
            object->setProperty("start", static_cast<juce::int64>(edit->ticks));
            if (edit->length != 0) object->setProperty("length", static_cast<juce::int64>(edit->length));
            return result;
        }
        if (edit->kind == Kind::crossfade) {
            object->setProperty("left", id(edit->clips[0])); object->setProperty("right", id(edit->clips[1]));
            object->setProperty("ticks", static_cast<juce::int64>(edit->ticks)); return result;
        }
        if (song_clip::warpEdit(edit->kind)) {
            object->setProperty("clip", id(edit->clips[0]));
            if (edit->kind == Kind::setWarp) {
                juce::Array<juce::var> markers;
                for (const auto& m : edit->warp) {
                    auto* marker = new juce::DynamicObject;
                    marker->setProperty("tick", static_cast<juce::int64>(m.tick)); marker->setProperty("seconds", m.seconds);
                    markers.add(juce::var(marker));
                }
                object->setProperty("markers", markers);
            }
            if (edit->kind == Kind::moveWarp || edit->kind == Kind::removeWarp) object->setProperty("marker", edit->index);
            if (edit->kind == Kind::addWarp || edit->kind == Kind::moveWarp) object->setProperty("tick", static_cast<juce::int64>(edit->ticks));
            return result;
        }
        if (song_clip::oneClip(edit->kind)) {
            object->setProperty("clip", id(edit->clips[0]));
            object->setProperty(edit->kind == Kind::trimStart || edit->kind == Kind::trimEnd ? "tick" : "ticks", static_cast<juce::int64>(edit->ticks));
            return result;
        }
        juce::Array<juce::var> list;
        for (const auto v : edit->clips) list.add(id(v));
        object->setProperty("clips", list);
        switch (edit->kind) {
        case Kind::move: case Kind::copy: object->setProperty("ticks", static_cast<juce::int64>(edit->ticks)); object->setProperty("lanes", edit->lanes); break;
        case Kind::split: object->setProperty("tick", static_cast<juce::int64>(edit->ticks)); break;
        case Kind::slip: object->setProperty("ticks", static_cast<juce::int64>(edit->ticks)); break;
        case Kind::mute: object->setProperty("muted", edit->on); break;
        case Kind::gain: object->setProperty("gain", edit->gain); break;
        case Kind::fades: object->setProperty("fadeIn", static_cast<juce::int64>(edit->ticks)); object->setProperty("fadeOut", static_cast<juce::int64>(edit->fadeOut)); break;
        case Kind::fadeShape: object->setProperty("edge", detail::fadeEdgeName(edit->edge)); object->setProperty("curve", static_cast<int>(edit->curve)); break;
        case Kind::loopSource: object->setProperty("looped", edit->on); break;
        case Kind::reverse: object->setProperty("reversed", edit->on); break;
        default: break;
        }
        return result;
    }
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
    if (index == 84) { error = "PasteRackSteps takes copied steps, which cannot be written as arguments."; return std::nullopt; }
    if (index == 85) { error = "SetRackChannelNotes takes a note sequence, which cannot be written as arguments."; return std::nullopt; }
    if (index == 86) { error = "ImportRackGroove takes a read MIDI file, which cannot be written as arguments."; return std::nullopt; }
    if (index == 87) { error = "LoadRackSample takes decoded audio, which cannot be written as arguments."; return std::nullopt; }
    if (index == 132) { error = "PasteSongClips takes copied Song clips, which cannot be written as arguments."; return std::nullopt; }
    static const std::array<juce::StringArray, 134> allowed{
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
        {"bpm"}, {"tick", "bpm"}, {"tick", "to"}, {"tick"}, {}, {"bar", "numerator", "denominator"}, {"bar"}, {},
        {"sound"}, {"preset"}, {"channel"}, {"channel", "to"}, {"channel"}, {"channel", "name"}, {"channel", "volume"}, {"channel", "pan"}, {"channel", "muted"}, {"channel", "soloed"}, {"channel", "output"}, {"channel", "group"},
        {"pattern", "channel", "step", "on"}, {"pattern", "channel", "step", "field", "value"}, {"pattern", "channel", "every"}, {"pattern", "channel", "distance"}, {"pattern", "channel"}, {"pattern", "channel", "period"}, {"pattern", "channel"},
        {"pattern", "length"}, {"amount"}, {"pattern"}, {"pattern"}, {"pattern", "name"}, {"pattern"},
        {"channel", "sound"}, {"channel"}, {"channel", "preset"}, {"channel", "description", "name"}, {}, {}, {}, {},
        {"tick", "name", "colour"}, {"marker", "tick"}, {"marker", "name"}, {"marker", "colour"}, {"marker"}, {"start", "end", "enabled"},
        {"clips", "ticks", "lanes"}, {"clip", "tick"}, {"clip", "tick"}, {"clips", "ticks"}, {"clips"}, {"clips", "muted"}, {"clips", "gain"}, {"clips", "fadeIn", "fadeOut"}, {"clip", "ticks"}, {"clip", "ticks"}, {"clips", "edge", "curve"}, {"clips"}, {"left", "right", "ticks"}, {"clips"}, {"clips"}, {"clips", "looped"}, {"clips", "reversed"},
        {"clips", "tick"}, {"clips"}, {"clips", "ticks", "lanes"},
        {"start", "end", "name", "colour"}, {"section", "start", "end", "name", "colour"}, {"section"}, {"start", "end", "root", "quality"}, {"chord", "start", "end", "root", "quality"}, {"chord"}, {"bars"}, {"take", "start", "end"}, {"take"},
        {"clip", "markers"}, {"clip", "tick"}, {"clip", "marker", "tick"}, {"clip", "marker"},
        {"track", "parameter", "instance", "parameterId", "enabled"}, {"track", "parameter", "instance", "parameterId", "tick", "value", "curve"},
        {"track", "parameter", "instance", "parameterId", "from", "tick", "value", "curve"}, {"track", "parameter", "instance", "parameterId", "tick"},
        {"row", "source", "start", "length"}, juce::StringArray{}, juce::StringArray{"channel", "bounds"}
    };
    static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(allowed)>> == detail::names.size(), "every command lists its arguments");
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
    // A marker id is a positive whole number that fits 32 bits.
    auto marker = [&](const char* key) {
        const auto value = wide(key);
        if (ok && (value <= 0 || value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))) {
            error = commandName + " needs a positive \"" + juce::String(key) + "\" id that fits 32 bits."; ok = false;
        }
        return ok ? static_cast<std::uint32_t>(value) : std::uint32_t{};
    };
    // An automation lane: "track" and "parameter" (volume, pan, sendA to sendH), or a plugin's "instance" (a decimal
    // string, as arguments() writes it) and "parameterId".
    auto automationLane = [&] {
        automation_edit::LaneRef lane;
        const bool plugin = object != nullptr && (object->hasProperty("instance") || object->hasProperty("parameterId"));
        if (plugin) {
            if (ok && (object->hasProperty("track") || object->hasProperty("parameter"))) {
                error = commandName + " takes a track and parameter, or a plugin instance and parameterId, not both."; ok = false; return lane;
            }
            const auto value = text("instance");
            const auto digits = value.toStdString(); std::uint64_t instance{};
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), instance);
            if (ok && (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || instance == 0
                || value != juce::String(static_cast<juce::uint64>(instance)))) {
                error = commandName + " needs a positive canonical decimal-string plugin \"instance\"."; ok = false;
            }
            lane.plugin = {instance, text("parameterId").toStdString()};
            return lane;
        }
        lane.track = integer("track");
        const auto name = text("parameter");
        if (const auto parameter = automation_edit::parameterNamed(name)) lane.parameter = *parameter;
        else if (ok) { error = commandName + " needs \"parameter\" to be volume, pan or sendA to sendH."; ok = false; }
        return lane;
    };
    auto curve = [&] {
        const int value = integer("curve");
        if (ok && value != 0 && value != 1) { error = commandName + " needs \"curve\" to be 0 (linear) or 1 (hold)."; ok = false; }
        return static_cast<automation::Curve>(value);
    };
    auto pointValue = [&] {
        const double value = number("value");
        if (ok && std::abs(value) > 1.0e6) { error = commandName + " needs a \"value\" the parameter can hold."; ok = false; }
        return static_cast<float>(value);
    };
    // A list of Song clip ids: an array of positive whole numbers that fit 32 bits.
    auto clips = [&](const char* key) {
        timeline::Ids ids;
        const auto value = object != nullptr ? object->getProperty(key) : juce::var{};
        if (ok && !value.isArray()) { error = commandName + " needs a \"" + juce::String(key) + "\" list of clip ids."; ok = false; }
        if (ok) for (const auto& item : *value.getArray()) {
            const bool whole = item.isInt() || item.isInt64();
            const auto v = whole ? static_cast<juce::int64>(item) : juce::int64{};
            if (!whole || v <= 0 || v > static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max())) {
                error = commandName + " needs \"" + juce::String(key) + "\" to list positive clip ids that fit 32 bits."; ok = false; break;
            }
            ids.push_back(static_cast<std::uint32_t>(v));
        }
        return ids;
    };
    switch (index) {
        case 88: { const auto tick = wide("tick"); const auto name = text("name"); command = AddSongMarker{tick, name, integer("colour")}; break; }
        case 89: { const auto id = marker("marker"); command = MoveSongMarker{id, wide("tick")}; break; }
        case 90: { const auto id = marker("marker"); command = RenameSongMarker{id, text("name")}; break; }
        case 91: { const auto id = marker("marker"); command = ColourSongMarker{id, integer("colour")}; break; }
        case 92: command = RemoveSongMarker{marker("marker")}; break;
        case 93: { const auto start = wide("start"), end = wide("end"); command = SetSongLoop{start, end, boolean("enabled")}; break; }
        case 94: { auto ids = clips("clips"); const auto ticks = wide("ticks"); command = MoveSongClips{std::move(ids), ticks, integer("lanes")}; break; }
        case 95: { const auto id = marker("clip"); command = TrimSongClipStart{id, wide("tick")}; break; }
        case 96: { const auto id = marker("clip"); command = TrimSongClipEnd{id, wide("tick")}; break; }
        case 97: { auto ids = clips("clips"); command = SlipSongClips{std::move(ids), wide("ticks")}; break; }
        case 98: command = DeleteSongClips{clips("clips")}; break;
        case 99: { auto ids = clips("clips"); command = SetSongClipsMuted{std::move(ids), boolean("muted")}; break; }
        case 100: { auto ids = clips("clips"); command = SetSongClipGain{std::move(ids), number("gain")}; break; }
        case 101: { auto ids = clips("clips"); const auto in = wide("fadeIn"); command = SetSongClipFades{std::move(ids), in, wide("fadeOut")}; break; }
        case 102: { const auto id = marker("clip"); command = SetSongClipFadeIn{id, wide("ticks")}; break; }
        case 103: { const auto id = marker("clip"); command = SetSongClipFadeOut{id, wide("ticks")}; break; }
        case 104: {
            auto ids = clips("clips"); const auto edge = detail::fadeEdgeNamed(text("edge")); const int curve = integer("curve");
            if (ok && !edge) { ok = false; error = "SetSongClipFadeShape needs an \"edge\" of in, out or crossfade."; }
            if (ok && (curve < 0 || curve >= timeline::fadeCurveCount)) { ok = false; error = "SetSongClipFadeShape needs a \"curve\" from 0 to " + juce::String(timeline::fadeCurveCount - 1) + "."; }
            if (ok) command = SetSongClipFadeShape{std::move(ids), *edge, static_cast<timeline::FadeCurve>(curve)};
            break;
        }
        case 105: command = RemoveSongClipFades{clips("clips")}; break;
        case 106: { const auto left = marker("left"), right = marker("right"); command = CrossfadeSongClips{left, right, wide("ticks")}; break; }
        case 107: command = GroupSongClips{clips("clips")}; break;
        case 108: command = UngroupSongClips{clips("clips")}; break;
        case 109: { auto ids = clips("clips"); command = LoopSongClips{std::move(ids), boolean("looped")}; break; }
        case 110: { auto ids = clips("clips"); command = ReverseSongClips{std::move(ids), boolean("reversed")}; break; }
        case 111: { auto ids = clips("clips"); command = SplitSongClips{std::move(ids), wide("tick")}; break; }
        case 112: command = DuplicateSongClips{clips("clips")}; break;
        case 113: { auto ids = clips("clips"); const auto ticks = wide("ticks"); command = CopySongClips{std::move(ids), ticks, integer("lanes")}; break; }
        case 114: { const auto start = wide("start"), end = wide("end"); const auto name = text("name"); command = AddSongSection{start, end, name, integer("colour")}; break; }
        case 115: { const auto id = marker("section"); const auto start = wide("start"), end = wide("end"); const auto name = text("name"); command = EditSongSection{id, start, end, name, integer("colour")}; break; }
        case 116: command = RemoveSongSection{marker("section")}; break;
        case 117: case 118: {
            const auto id = index == 118 ? marker("chord") : std::uint32_t{};
            const auto start = wide("start"), end = wide("end"); const int root = integer("root"), quality = integer("quality");
            if (ok && (quality < 0 || quality > 8)) { ok = false; error = commandName + " needs a \"quality\" from 0 to 8."; }
            if (!ok) break;
            if (index == 117) command = AddSongChord{start, end, root, static_cast<ChordType>(quality)};
            else command = EditSongChord{id, start, end, root, static_cast<ChordType>(quality)};
            break;
        }
        case 119: command = RemoveSongChord{marker("chord")}; break;
        case 120: command = SetSongLength{integer("bars")}; break;
        case 121: { const auto take = marker("take"); const auto start = wide("start"); command = CompSongTake{take, start, wide("end")}; break; }
        case 122: command = ForgetSongTake{marker("take")}; break;
        case 123: {
            const auto id = marker("clip");
            std::vector<timeline::WarpMarker> markers;
            const auto list = object != nullptr ? object->getProperty("markers") : juce::var{};
            if (ok && !list.isArray()) { error = commandName + " needs a \"markers\" list of {tick, seconds}."; ok = false; }
            if (ok) for (const auto& item : *list.getArray()) {
                const auto* m = item.getDynamicObject();
                const auto tick = m != nullptr ? m->getProperty("tick") : juce::var{};
                const auto seconds = m != nullptr ? m->getProperty("seconds") : juce::var{};
                const bool numeric = seconds.isInt() || seconds.isInt64() || seconds.isDouble();
                if (m == nullptr || m->getProperties().size() != 2 || !(tick.isInt() || tick.isInt64()) || !numeric || !std::isfinite(static_cast<double>(seconds))) {
                    error = commandName + " needs each marker to be {tick: whole number, seconds: finite number}."; ok = false; break;
                }
                markers.push_back({static_cast<std::int64_t>(static_cast<juce::int64>(tick)), static_cast<double>(seconds)});
            }
            command = SetSongClipWarp{id, std::move(markers)};
            break;
        }
        case 124: { const auto id = marker("clip"); command = AddSongWarpMarker{id, wide("tick")}; break; }
        case 125: { const auto id = marker("clip"); const int index = integer("marker"); command = MoveSongWarpMarker{id, index, wide("tick")}; break; }
        case 126: { const auto id = marker("clip"); command = RemoveSongWarpMarker{id, integer("marker")}; break; }
        case 127: { const auto lane = automationLane(); command = SetAutomationRead{lane, boolean("enabled")}; break; }
        case 128: { const auto lane = automationLane(); const auto tick = wide("tick"); const auto value = pointValue(); command = AddAutomationPoint{lane, tick, value, curve()}; break; }
        case 129: {
            const auto lane = automationLane(); const auto from = wide("from"), tick = wide("tick"); const auto value = pointValue();
            command = MoveAutomationPoint{lane, from, tick, value, curve()}; break;
        }
        case 130: { const auto lane = automationLane(); command = RemoveAutomationPoint{lane, wide("tick")}; break; }
        case 133: {
            const int channel = integer("channel");
            std::vector<std::int64_t> bounds;
            const auto value = object != nullptr ? object->getProperty("bounds") : juce::var{};
            if (ok && !value.isArray()) { error = "SliceRackChannel needs a \"bounds\" list of frames."; ok = false; }
            if (ok) for (const auto& item : *value.getArray()) {
                if (!(item.isInt() || item.isInt64())) { error = "SliceRackChannel needs \"bounds\" to list whole frame numbers."; ok = false; break; }
                bounds.push_back(static_cast<std::int64_t>(static_cast<juce::int64>(item)));
            }
            command = SliceRackChannel{channel, std::move(bounds)};
            break;
        }
        case 131: {
            // "length" may be left out: the clip's own length.
            const int row = integer("row"); const int source = integer("source"); const auto start = wide("start");
            const auto length = object != nullptr && object->hasProperty("length") ? wide("length") : std::int64_t{};
            command = PlaceSongClip{row, source, start, length};
            break;
        }
        case 55: command = AddRackChannel{text("sound")}; break;
        case 56: command = AddRackSynthChannel{integer("preset")}; break;
        case 57: command = RemoveRackChannel{integer("channel")}; break;
        case 58: { const int channel = integer("channel"); command = MoveRackChannel{channel, integer("to")}; break; }
        case 59: command = DuplicateRackChannel{integer("channel")}; break;
        case 60: { const int channel = integer("channel"); command = RenameRackChannel{channel, text("name")}; break; }
        case 61: { const int channel = integer("channel"); command = SetRackChannelVolume{channel, number("volume")}; break; }
        case 62: { const int channel = integer("channel"); command = SetRackChannelPan{channel, number("pan")}; break; }
        case 63: { const int channel = integer("channel"); command = SetRackChannelMute{channel, boolean("muted")}; break; }
        case 64: { const int channel = integer("channel"); command = SetRackChannelSolo{channel, boolean("soloed")}; break; }
        case 65: { const int channel = integer("channel"); command = SetRackChannelOutput{channel, integer("output")}; break; }
        case 66: { const int channel = integer("channel"); command = SetRackChannelChoke{channel, integer("group")}; break; }
        case 67: { const int pattern = integer("pattern"), channel = integer("channel"), step = integer("step"); command = SetRackStep{pattern, channel, step, boolean("on")}; break; }
        case 68: { const int pattern = integer("pattern"), channel = integer("channel"), step = integer("step"); const auto field = text("field"); command = SetRackStepValue{pattern, channel, step, field, integer("value")}; break; }
        case 69: { const int pattern = integer("pattern"), channel = integer("channel"); command = FillRackSteps{pattern, channel, integer("every")}; break; }
        case 70: { const int pattern = integer("pattern"), channel = integer("channel"); command = RotateRackSteps{pattern, channel, integer("distance")}; break; }
        case 71: { const int pattern = integer("pattern"); command = ReverseRackSteps{pattern, integer("channel")}; break; }
        case 72: { const int pattern = integer("pattern"), channel = integer("channel"); command = RepeatRackSteps{pattern, channel, integer("period")}; break; }
        case 73: { const int pattern = integer("pattern"); command = ClearRackSteps{pattern, integer("channel")}; break; }
        case 74: { const int pattern = integer("pattern"); command = SetRackPatternLength{pattern, integer("length")}; break; }
        case 75: command = SetRackSwing{number("amount")}; break;
        case 76: command = CloneRackPattern{integer("pattern")}; break;
        case 77: command = ClearRackPattern{integer("pattern")}; break;
        case 78: { const int pattern = integer("pattern"); command = RenameRackPattern{pattern, text("name")}; break; }
        case 79: command = DeleteRackPattern{integer("pattern")}; break;
        case 80: { const int channel = integer("channel"); command = SetRackChannelSound{channel, text("sound")}; break; }
        case 81: command = SilenceRackChannel{integer("channel")}; break;
        case 82: { const int channel = integer("channel"); command = SetRackChannelSynth{channel, integer("preset")}; break; }
        case 83: { const int channel = integer("channel"); const auto description = text("description"); command = SetRackChannelPlugin{channel, description, text("name")}; break; }
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
        for (const auto& command : commands)
            if (const auto edit = songEdit(command))
                if (const auto why = controller.songEditRefusal(*edit); why.isNotEmpty()) return refused(why);
        for (const auto& command : commands)
            if (const auto edit = songClipEdit(command))
                if (const auto why = controller.songClipRefusal(*edit); why.isNotEmpty()) return refused(why);
        for (const auto& command : commands)
            if (const auto edit = automationEdit(command))
                if (const auto why = controller.automationRefusal(*edit); why.isNotEmpty()) return refused(why);
        for (const auto& command : commands)
            if (const auto edit = rackChannelEdit(command))
                if (const auto why = controller.rackSliceRefusal(*edit); why.isNotEmpty()) return refused(why);
        const auto clipboard = controller.clipClipboard();
        Result result = preview(commands, &clipboard);
        if (!result.ok || !result.changed) return result;
        std::size_t audioBytes = 0;
        for (const auto& command : commands)
            if (const auto* c = std::get_if<ImportClipAudio>(&command)) audioBytes += c->prepared->decoded.clip->decodedBytes();
            else if (const auto* s = std::get_if<LoadRackSample>(&command)) audioBytes += s->clip->decodedBytes();
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
        controller.rackChannelsCommitted();
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
        if (const auto edit = songEdit(command)) return controller.applySongCommand(*edit);
        if (const auto edit = songClipEdit(command)) return controller.applySongClipCommand(*edit);
        if (const auto edit = automationEdit(command)) return controller.applyAutomationCommand(*edit);
        if (const auto* c = std::get_if<LoadRackSample>(&command)) return controller.applyRackSampleCommand(*rackChannelEdit(command), c->clip);
        if (const auto edit = rackChannelEdit(command)) return controller.applyRackChannelCommand(*edit);
        if (const auto edit = rackPatternEdit(command)) return controller.applyRackPatternCommand(*edit);
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

// Gives a rack channel a finished decode and tells the person what happened. The same file loaded
// again changes nothing in the project, so it makes no Undo step, but the new decode still plays.
inline Result loadDecodedRackSample(ProjectController& controller, const juce::File& file, int channel, std::shared_ptr<SessionAudioClip> clip) {
    const auto path = file.getFullPathName();
    auto result = CommandEngine(controller).execute(LoadRackSample{channel, path, file.getFileNameWithoutExtension(), clip});
    if (result.ok && !result.changed && !controller.replaceRackSampleAudio(path.toStdString(), clip)) {
        result.ok = false;
        result.error = "Session audio storage is full (512 MiB limit)";
    }
    controller.rackSampleFinished(file, result.error);
    return result;
}

// Decodes an audio file off the message thread, then runs LoadRackSample on the result.
inline void loadRackSample(ProjectController& controller, const juce::File& file, int channel) {
    if (controller.isBusy()) { controller.reportAudioImportRefusal("The project is busy; try again when it has finished.", file); return; }
    controller.decodeRackSample(file, [&controller, file, channel](const AudioClipImportResult& decoded) {
        if (decoded.clip) loadDecodedRackSample(controller, file, channel, decoded.clip);
        else controller.reportAudioImportRefusal("Could not load sample: " + decoded.error, file);
    });
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
