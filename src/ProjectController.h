#pragma once
#include <span>
#include "ProjectDocument.h"
#include "MidiClipCommands.h"
#include "ClipboardClipCommands.h"
#include "SessionClipCommands.h"
#include "RackCommands.h"
#include "ReturnCommands.h"
#include "BusCommands.h"
#include "SidechainCommands.h"
#include "DeviceCommands.h"
#include "TempoCommands.h"
#include "FreezeCommands.h"
#include "SongMarkerCommands.h"
#include "SongClipCommands.h"
#include "ClipNotesCommands.h"
#include "AutomationCommands.h"
#include "RackChannelCommands.h"
#include "RackPatternCommands.h"
#include "ViewModes.h"
#include "ToneEngine.h"
#include "AudioClipImport.h"
#include "SessionAudioImportPlan.h"
#include "SongAudioImport.h"
#include "SongPlacementEdit.h"
#include "DrumPadEdit.h"
#include "ArrangementWarp.h"
#include "AudioRelink.h"
#include "PortableProject.h"
#include "DocumentHistory.h"
#include "PianoRollProcessing.h"
#include "RackSounds.h"
#include "RackChannelNotes.h"
#include "DrumTransientSlices.h"
#include "DiskRecording.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <set>

// Owns the ProjectDocument and is the only code that changes it. Every edit
// goes through here so the audio engine is kept in sync. Callers report each
// finished edit with commit(), which feeds undo/redo, the unsaved-changes
// state and autosave. File work and audio decoding run on a worker thread;
// results come back on the message thread. Message thread only.
namespace alien {

// T1-04 D10: what the status bar and an export say about Channel Rack hits that could not be played (D8, D9).
inline juce::String rackDropText(std::uint64_t hits) {
    return juce::String(static_cast<juce::int64>(hits)) + (hits == 1 ? " Channel Rack hit was" : " Channel Rack hits were") + " not played";
}
inline const char* const rackDropTip =
    "The Channel Rack had no room for them: too many notes, ratchets or nudged hits at the same moment for its "
    "plugin instruments, or not enough memory for every channel. Thin out the densest steps or chords and play again.";
// T1-04 D11: the same for MIDI that a Song or Session track's instrument could not be given.
inline juce::String trackMidiDropText(std::uint64_t events) {
    return juce::String(static_cast<juce::int64>(events)) + (events == 1 ? " MIDI event for a track instrument was" : " MIDI events for track instruments were")
        + " not sent, and their notes were stopped";
}
inline const char* const trackMidiDropTip =
    "More than 4,096 notes or controller messages reached one track's instrument at the same moment, so the rest were "
    "dropped and every instrument was told to end its notes. Thin out stacked clips or dense MIDI on that track.";
// T1-04 D12: streamed audio that played with gaps, or could not be read, during playback. `tracks` are 1-based.
inline juce::String trackListText(const juce::Array<int>& tracks) {
    juce::String out = tracks.size() == 1 ? "track " : "tracks ";
    for (int i = 0; i < tracks.size(); ++i)
        out << (i == 0 ? "" : i == tracks.size() - 1 ? " and " : ", ") << tracks[i];
    return out;
}
inline juce::String streamProblemText(const juce::Array<int>& droppedOut, const juce::Array<int>& unreadable) {
    juce::StringArray parts;
    if (!droppedOut.isEmpty()) parts.add("Streamed audio dropped out on " + trackListText(droppedOut) + ": the disk could not keep up");
    if (!unreadable.isEmpty()) parts.add("Streamed audio on " + trackListText(unreadable) + " could not be read, so it stopped");
    return parts.joinIntoString("; ");
}
inline const char* const streamDropoutTip =
    "Long audio clips are read from disk while they play. The disk did not deliver in time, so there were gaps of "
    "silence. Close programs that use the disk heavily, or keep the project's audio on a faster, local drive.";
inline const char* const streamUnreadableTip =
    "The audio file may have been moved, deleted or changed, or its drive disconnected. Check the file, then open the "
    "project again; Search for missing audio finds a moved file.";
// Everything dropped, for a status line or an export summary; empty if nothing was.
inline juce::String droppedNotesText(std::uint64_t rackHits, std::uint64_t trackEvents) {
    juce::StringArray parts;
    if (rackHits > 0) parts.add(rackDropText(rackHits));
    if (trackEvents > 0) parts.add(trackMidiDropText(trackEvents));
    return parts.joinIntoString("; ");
}

class ProjectController {
public:
    static constexpr std::size_t maxSessionAudioBytes = 512u * 1024u * 1024u;

    explicit ProjectController(ToneEngine& toneEngine);

    ~ProjectController();

    std::function<void()> onMusicalScaleChanged;
    // The key cannot change during a take, or while the last one is still finishing.
    bool projectScaleOpen() const { return !busy && !isRecording() && !engine.recording.active(); }
    // Live side of SetProjectScale. CommandEngine owns the history entry.
    bool applyProjectScale(int root, ScaleType type);
    const ProjectDocument& document() const noexcept { return doc; }
    void setExternalBusy(bool value) { setBusy(value); }
    bool isBusy() const noexcept { return busy; }
    const juce::File& currentFile() const noexcept { return projectFile; }

    // Message-thread notifications.
    std::function<void(const juce::String&, const juce::String&)> onProjectStatus; // text, tooltip
    std::function<void(const juce::String&, const juce::String&)> onImportStatus;
    std::function<void(bool)> onBusyChanged;
    std::function<void()> beforeDocumentReplaced; // e.g. release held notes
    std::function<void()> onDocumentReplaced;     // whole document was opened/replaced
    std::function<void(int, int)> onClipContentChanged;
    std::function<void()> onDocumentRestored;     // undo/redo replaced the document
    std::function<void()> onBeforeSnapshot;
    // Autosave runs unattended while the user may be playing, so it gets its own
    // hook: the owner can decline to interrupt audio to capture plugin state.
    std::function<void()> onBeforeAutosaveSnapshot;
    std::function<void()> beforeStructureEdit;
    std::function<void()> onHistoryChanged;       // undo/redo availability or unsaved state changed

    // Undo, redo and unsaved changes -------------------------------------
    // Call after every finished edit. focusTrack/focusScene name the clip the
    // edit changed (shown again when it is undone). Edits with the same
    // coalesceKey made in quick succession become one undo step.
    void commit(const juce::String& label, int focusTrack = -1, int focusScene = -1,
                const juce::String& coalesceKey = {}, bool continuous = false, int undoTrack = -1, int undoScene = -1);

    // Start a fresh history from the current document (e.g. at startup).
    void markAsNewBaseline();

    void runtimePluginEdited() { runtimeDirty = true; historyChanged(); }
    // True when a plugin was edited since its state was last copied into the document.
    bool pluginStateDirty() const noexcept { return runtimeDirty; }
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master);
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns);
    void setPluginRacks(const std::vector<PluginRackState>& racks, const PluginRackState& master, const ReturnRacks& returns, const BusRacks& buses);
    // The group buses' FX racks (empty for unused slots), in the shape PluginHost takes them.
    BusRacks busRacks() const;

    // The return tracks' FX racks (empty for unused slots), in the shape PluginHost takes them.
    ReturnRacks returnRacks() const;
    bool canUndo() const noexcept { return history.canUndo(); }
    bool canRedo() const noexcept { return history.canRedo(); }
    juce::String undoLabel() const { return history.undoLabel(); }
    juce::String redoLabel() const { return history.redoLabel(); }
    bool isDirty() const noexcept { return runtimeDirty || history.currentId() != savedId; }

    bool undo();

    bool redo();

    // Put back a document captured earlier in the same edit, before it was committed
    // (e.g. a command batch that failed part way). The history is not changed.
    void revertTo(const ProjectDocument& state) { restore(state, {{}, state.selectedTrack, state.selectedScene}); }

    // Replace the song with an empty default project.
    void newProject();

    // Autosave and crash recovery ----------------------------------------
    static juce::File recoveryDirectory();
    // Tests point autosave somewhere temporary so a real autosave is never touched.
    static juce::File& recoveryDirectoryOverride() { static juce::File directory; return directory; }
    static juce::File recoveryFile() { return recoveryDirectory().getChildFile("Autosave.alienstudio"); }
    static juce::File recoveryInfoFile() { return recoveryDirectory().getChildFile("Autosave.json"); }

    // An autosave left behind means the last session did not close normally.
    static bool hasRecoverableSession() { return recoveryFile().existsAsFile(); }

    // The project the autosave belonged to, if it had been saved before.
    static juce::File recoveredOriginalFile();

    static void deleteRecoveryFiles();

    // Serialise cleanup with an in-flight writer. A queued writer and its UI
    // completion also carry a generation so neither can revive discarded work.
    void discardRecovery();
    bool isAutosaving() const noexcept { return autosaving; }

    // Write an autosave copy on the worker thread if there are unsaved edits
    // that have not been autosaved yet. Never touches the user's project file.
    void autosaveIfNeeded();

    // Open the autosave. The result keeps the original project path (if any)
    // and counts as unsaved, so the user decides where to keep it.
    void recoverSession();

    // Engine synchronisation ---------------------------------------------
    void pushDocumentToEngine();

    // Track and scene structure --------------------------------------------
    // Appending never disturbs playback. Any other structural edit renumbers
    // tracks or scenes, so Session playback is stopped first; this keeps the
    // engine's per-track slots and clip indices consistent with the document.
    bool addTrack();

    bool deleteTrack(int track);

    bool moveTrack(int track, int delta);

    bool renameTrack(int track, const juce::String& name);

    // Number of colours in the track palette (ui::trackColour).
    static constexpr int trackColourCount = 8;
    bool setTrackColour(int track, int colour);

    bool addScene() { return insertScene(doc.sceneCount(), nullptr); }

    bool duplicateScene(int scene);

    bool deleteScene(int scene);

    bool moveScene(int scene, int delta);

    bool renameScene(int scene, const juce::String& name);

    // Validated noncommitting adapter for the master-volume command.
    bool setMasterVolume(double gain);
    // Legacy engine readback for saving/autosave and remaining direct controls.
    void masterGainEdited() { doc.masterGain = sanitizedMasterGain(engine.masterControls.gain.load()); }
    void returnSettingsEdited(int r);
    void busSettingsEdited(int b);

    // Group buses. A new bus takes the first free slot (its index, or -1 when all are in use);
    // deleting one sends its tracks back to the master and drops its FX. Buses feed only the
    // master, so no edit can make a loop. The caller commits, and rebuilds the plugin racks
    // after a delete.
    int addBus(const juce::String& name = {});
    bool renameBus(int b, const juce::String& name);
    bool deleteBus(int b);
    // Validated noncommitting strip adapters: each writes the document and the engine, and the
    // caller commits. A strip takes the same ranges as a track fader and pan.
    bool setBusVolume(int b, double gain);
    bool setBusPan(int b, double pan);
    bool setBusMute(int b, bool muted);
    bool setBusSolo(int b, bool soloed);
    bool setReturnVolume(int r, double gain);
    bool setReturnPan(int r, double pan);
    bool setReturnMute(int r, bool muted);
    // Return tracks (MIX-05). A new return takes the first free slot (its index, or -1 when all are
    // in use) and that slot's letter: sends and send automation name the slot, so removing a return
    // never moves another's. Removing one turns every track's send to it off, drops its send
    // automation and its FX. The caller commits, and rebuilds the plugin racks after a removal.
    int addReturn(const juce::String& name = {});
    bool renameReturn(int r, const juce::String& name);
    bool deleteReturn(int r);

    // -1 sends the track to the master. The engine hears the change at once; the caller commits.
    bool setTrackOutput(int track, int output);

    // Writes the key to the document only. The caller commits, and the host rebuilds the plugin racks.
    bool applySidechainCommand(const sidechain_edit::Edit& edit) { return !busy && sidechain_edit::apply(doc, edit).isEmpty(); }
    bool applyDeviceCommand(const device_edit::Edit& edit) { return !busy && device_edit::apply(doc, edit).isEmpty(); }
    bool applyConvolutionResponseCommand(const device_edit::ConvolutionResponseEdit& edit);

    // Message-thread command setters. Only the addressed control changes; the
    // caller commits after the complete command/batch, just like track renaming.
    bool setTrackVolume(int track, double gain);
    bool setTrackSend(int track, int r, double level);
    bool setTrackSendMode(int track, int r, bool preFader);
    bool setTrackPan(int track, double pan);

    bool setTrackMute(int track, bool muted);
    // Live side of SetTrackSound. CommandEngine owns the history entry.
    bool setTrackSound(int track, int preset, bool drumKit);
    bool setTrackSolo(int track, bool soloed);

    void trackSettingsEdited(int track);

    // Live sides of SetTrackArmed, SetTrackInput and SetTrackMonitor: the engine hears the change at once.
    // CommandEngine owns the history entry.
    bool setTrackArmed(int track, bool armed);
    bool setTrackInput(int track, int input, bool stereo);
    bool setTrackMonitor(int track, int mode);
    bool recordingSetupOpen(int track) const;
    // The loudest input on `track` since the last call (0 when it is neither armed nor monitoring).
    float takeInputLevel(int track) noexcept;

    // Playback --------------------------------------------------------------
    void selectClip(int track, int scene);

    void launchClip(int track, int scene);

    // A clip or scene launched from the Session view should be heard at once: while Session clips are what the
    // engine plays (not the Song, not the Channel Rack, not a recording's count-in), a stopped transport starts.
    // Returns whether it started; the launch itself waits for the next Beat/Bar boundary as usual.
    bool startTransportForLaunch();

    void launchScene(int scene);

    void stopAllTracks();

    // A launch is transport state, not an edit or a change of the pattern being edited.
    int rackPatternForClip(std::uint64_t clip) const noexcept;
    bool rackPatternInUse(int pattern) const;
    bool canLaunchRackClip(std::uint64_t clip) const;
    bool launchRackClip(std::uint64_t clip, int track = -1);
    bool stopRackClip();

    // Stops one Session track at the next launch boundary; the other tracks keep playing.
    void stopTrack(int track);
    void stopRackOnTrack(int track);

    void publishClip(int track, int scene);

    // Recording ---------------------------------------------------------------
    // What the user is told when a take ends.
    struct RecordingOutcome {
        int track{-1}, scene{-1};
        int notesAdded{};                 // notes now in the clip because of this take
        bool replaced{};                  // the take replaced the clip's earlier notes
        bool refused{};                   // the engine could not start the take
        bool discarded{};                 // the clip changed underneath the take, so nothing was kept
        bool noteLimitReached{};          // the clip is full (1024 notes)
        std::uint64_t droppedEvents{};    // played notes the engine had no room to report
        juce::String error;               // a finished take could not be committed
    };
    struct RecordStart { bool started{}; juce::String reason; };
    std::function<void(const RecordingOutcome&)> onRecordingFinished;
    // Installed by the command adapter. A completed take is one SetClipNotes transaction.
    std::function<juce::String(int, int, const PianoRollPattern&)> recordMidiNotes;

    // Message thread only. Records into the selected clip, which must be empty
    // or hold MIDI. The count-in, metronome and monitoring are done by the
    // engine; the take becomes one undo step when it ends.
    RecordStart startRecording();

    // Ask the engine to end the take; its notes arrive through pollRecording().
    void stopRecording();

    bool isRecording() const noexcept { return take.active || songTake.active; }
    int recordingTrack() const noexcept { return take.active ? take.track : -1; }
    int recordingScene() const noexcept { return take.active ? take.scene : -1; }

    // Drops a take without keeping it (the document is being replaced).
    void abandonRecording();

    // Message-thread timer: turns what the engine captured into notes, lets the
    // looping clip play back what has been recorded so far, and finishes the take.
    void pollRecording();

    // Song-view audio takes ----------------------------------------------------
    // What the user is told when an audio take ends.
    struct SongTakeOutcome {
        int placed{};                // takes now on the timeline
        int recorded{};              // tracks that captured audio
        bool refused{}, abandoned{};
        int ended{};                 // SongTakeCapture::Ended
        int passes{};                // loop passes recorded (0 or 1: not a loop recording)
        std::vector<juce::File> kept; // files of the takes that could not be placed (abandoned, or failed to load)
        juce::String problems;       // disk or loading problems
    };
    std::function<void(const SongTakeOutcome&)> onSongTakeFinished;

    // Message thread only. Records every armed track whose input the device has (`deviceInputs` enabled
    // channels) into a new WAV in `folder` while the song plays; the engine counts in and starts the song,
    // and with punch on records only the loop range. The take is placed as one undo step when it ends.
    RecordStart startSongTake(const juce::File& folder, double sampleRate, int deviceInputs);
    bool isSongTakeActive() const noexcept { return songTake.active; }
    // The tracks being recorded, for the view.
    std::vector<int> songTakeTracks() const;

    // Live side of SetRecordSettings. CommandEngine owns the history entry.
    bool applyRecordSettings(const RecordSettings& settings);

    // Republish only when the clip is playing or queued, so content changes
    // are heard without launching anything new.
    // Republishes every other cell that shares this cell's content, where it is playing or queued.
    void republishLinked(int track, int scene);

    // ---- linked clips (T1-03): Session cells that share one clip's content ----------------------
    bool applyRackCommand(const rack_command::Edit& edit);
    // Copy remembers a cell's clip. Paste puts an independent copy of it in a cell; Paste linked puts a
    // clip that shares its content (an edit of either is an edit of both). Make unique gives a linked
    // cell its own copy again. Each is one undo step; a pasted cell's old content is replaced, not edited.
    bool copyCell(int track, int scene);
    bool hasCopiedCell() const noexcept { return copiedContent.has_value(); }
    bool canPasteLinked() const { return copiedCell && doc.library.clip(copiedCell) != nullptr; }
    clipboard_clip::Clipboard clipClipboard() const { return {copiedCell, copiedContent}; }
    bool applyClipboardClipCommand(const clipboard_clip::Edit& edit, const clipboard_clip::Clipboard& clipboard);
    // Non-committing command boundary. Only the addressed live cell is
    // republished; queued launches, transport and unrelated voices keep running.
    bool canEditMidiClips() const { return !busy && !isRecording() && engine.recording.phase.load()==RecordControls::idle; }
    bool applyMidiClipCommand(const midi_clip::Edit& edit);
    bool applyDuplicateClipCommand(const session_clip::Duplicate& edit);
    bool applyMoveClipCommand(const session_clip::Move& edit);
    // Validated noncommitting adapter for the named command. Publish only a
    // changed cell; unrelated voices, launches and transport keep running.
    bool applyDeleteClipCommand(int track, int scene);
    // Whether a Song placement shares what it plays with something else: another placement of the same
    // cell or placement clip, or a linked cell.
    bool placementShared(std::uint32_t regionId) const { return song_placement::shared(doc, regionId); }
    // Live side of MakeSongClipUnique (see SongPlacementEdit.h). CommandEngine owns the history entry.
    bool applyMakePlacementUnique(std::uint32_t regionId);
    void republishIfLive(int track, int scene);

    // Edits -----------------------------------------------------------------
    // Live side of SetStepOn, SetStepPitch and SetStepVelocity. CommandEngine owns the history entry.
    bool setStepOn(int step, bool on);
    bool setStepPitch(int step, int pitch);
    bool setStepVelocity(int step, int velocity);
    bool hasWarpedAudio() const;
    // Warped audio is prepared for one tempo, so nothing may change it while a take is being captured.
    bool tempoLocked() const;
    // Why the project's state, as opposed to the edit's arguments, refuses a tempo or time signature command.
    juce::String tempoEditRefusal(const tempo_edit::Edit& e) const;
    // Why the project's state, as opposed to the edit's arguments, refuses a Song marker, loop or length command.
    // Markers never change playback; the loop and the length do, so they wait like the tempo while warped audio records.
    juce::String songEditRefusal(const song_edit::Edit& e) const;
    // Why the project's state refuses a Song clip command: while warped audio records, nothing that plays may change.
    // The take list is the recorder's while it records, so comping and forgetting takes wait for it.
    juce::String songClipRefusal(const song_clip::Edit& e) const;
    // Why the project's state refuses an automation lane command: automation plays, so like the loop it waits while
    // warped audio records.
    juce::String automationRefusal(const automation_edit::Edit& e) const;
    // Live side of the automation lane commands. CommandEngine owns the history entry, and its commit plays the result.
    bool applyAutomationCommand(const automation_edit::Edit& e);
    // Live side of the Song clip commands. CommandEngine owns the history entry, and its commit plays the result.
    bool applySongClipCommand(const song_clip::Edit& e);
    // Live side of the Song marker and loop commands. CommandEngine owns the history entry, and its commit plays the new loop.
    bool applySongCommand(const song_edit::Edit& e);
    // Live side of the tempo and time signature commands. CommandEngine owns the history entry.
    bool applyTempoCommand(const tempo_edit::Edit& e);
    // While the tempo control is dragged the engine follows it and the project does not change.
    // The drag ends in SetTempo, so frozen tracks are unfrozen and the Undo step made only then.
    void auditionTempo(int bpm);
    // Back to the project's tempo, and transport released if a drag returned to its start.
    void endTempoAudition();
    // The song's tempo map: its tempo changes after the project tempo.
    timeline::TempoMap songTempo() const noexcept { return {doc.timeline.tempoChanges, static_cast<double>(doc.bpm)}; }
    // Live side of SetLivePreset and SetLaunchQuantization. CommandEngine owns the history entry.
    bool setLivePreset(int preset);
    bool setLaunchQuantization(int steps);

    // Called by the piano roll as notes change. Editing a clip that is not
    // playing queues it to launch, so changes are heard straight away.
    void setClipPattern(int track, int scene, const PianoRollPattern& pattern, bool publishToEngine);

    // Why the project's state refuses a clip notes command: the take recording into a clip owns it until it ends.
    juce::String clipNotesRefusal(const clip_notes::Edit& e) const;
    // Live side of SetClipNotes and SetPlacementClipNotes. CommandEngine owns the history entry. A Session clip that is
    // not playing is queued to launch, so an edit is heard straight away; Song clips playing a placement clip follow it.
    bool applyClipNotesCommand(const clip_notes::Edit& e);
    // The track whose Song regions play placement clip `clip` (-1 when none does).
    int placementTrack(std::uint64_t clip) const;

    bool applyRemoveClipAudioCommand(int track, int scene);

    // Files -----------------------------------------------------------------
    // done(true) once the project is safely on disk; done(false) if the user
    // cancelled or the save failed.
    void save(std::function<void(bool)> done = {});

    void chooseSavePath(bool portable, std::function<void(bool)> done = {});

    void chooseProjectToOpen();

    void chooseAudioFile(std::function<void(const juce::File&)> chosen);

    void writeProject(juce::File file, bool portable, std::function<void(bool)> done = {});

    void openProject(const juce::File& file, bool recovered = false);

    // Session audio arrives through command::ImportClipAudio: capture the
    // destination, decode off the message thread, then run the command on the result.
    std::optional<session_audio_import::Plan> prepareAudioImport(const juce::File& file,int track,int scene,bool emptyOnly=false) const;
    // The command's live half: no history entry, and refused before any live change.
    bool applyImportAudioCommand(const session_audio_import::Prepared& prepared);
    void decodeAudioImport(session_audio_import::Plan plan,std::function<void(const session_audio_import::Prepared&)> done);
    // What the person sees once the command has run or been refused.
    void audioImportFinished(const session_audio_import::Prepared& prepared,const juce::String& refused);
    void reportAudioImportRefusal(const juce::String& why,const juce::File& file) { importStatus(why,file.getFullPathName()); }

    // Browser import captures the destination before decoding. Busy state prevents
    // structural edits while the worker runs; recheck the destination before install.
    bool canImportBrowserSample(int track, int scene, bool song) const;
    // A sample bound for the Song (command::importSampleToSong): decodes the file off the message thread and hands
    // the audio to `done`, which runs ImportSongAudio. A sample for a Session cell takes command::importSampleToCell.
    void decodeSongSample(const juce::File& file, int track, std::function<void(std::shared_ptr<SessionAudioClip>)> done);
    // Live side of ImportSongAudio. CommandEngine owns the history entry.
    bool applySongAudioImport(const song_audio_import::Request& request);
    // What the import said: the refusal, or that the sample is on the Song.
    void songSampleImported(const juce::File& file, const juce::String& error);

    std::shared_ptr<ArrangementPlayback> captureArrangement() const;
    // What the engine currently holds. Commits that do not touch the song (mixer,
    // tempo, names, plugin state) must not rebuild, validate and re-send a snapshot.
    // Shared pointers keep every compared clip alive, so an address can never be
    // reused by a different clip while it is being compared.
    struct PublishedSong {
        bool known{};
        int bpm{};
        alien::timeline::Timeline timeline;
        int tracks{}, scenes{};
        std::vector<std::shared_ptr<const PianoRollPattern>> patterns;
        std::vector<std::shared_ptr<const SessionAudioClip>> audio;
        std::vector<std::shared_ptr<const PianoRollPattern>> placements;   // by doc.placementClips()
    };
    bool songDiffersFromPublished() const;
    void rememberPublishedSong();
    void publishArrangement();
    // Finds transients in a warped clip's audio in the background. `found` gets the new marker list on the
    // message thread (the caller runs SetSongClipWarp with it); nothing is edited here.
    using WarpFound=std::function<void(std::uint32_t,std::vector<timeline::WarpMarker>)>;
    void detectRegionTransients(std::uint32_t id,WarpFound found);
    bool retryWarpPreparation() { if (busy) return false; published.known=false; publishArrangement(); return true; }
    bool cancelWarpPreparation() { if (!warpWorkActive) return false; warpCancelled.store(true); status("Cancelling warp preparation...",{}); return true; }
    // The two markers that fit an unwarped audio clip's source, from where it starts playing to the file's end,
    // to the clip; none when it has no audio left to fit. SetSongClipWarp with them turns warp on.
    std::optional<std::vector<timeline::WarpMarker>> fittedWarp(std::uint32_t id) const;
    // The Arrangement timeline ------------------------------------------------------
    const alien::timeline::Timeline& timeline() const noexcept { return doc.timeline; }

    // Applies `edit` to a copy of the timeline. When it changes something valid for
    // this project the copy becomes the timeline and the change is one undo step
    // (edits with the same coalesceKey made in quick succession merge). Playback
    // follows without restarting. Returns whether anything changed.
    bool editTimeline(const juce::String& label, const std::function<bool(alien::timeline::Timeline&)>& edit,
                      const juce::String& coalesceKey = {});
    // How long a clip or pattern naturally lasts on the timeline, in ticks: its loop
    // for MIDI and rack patterns, its running time at the current tempo for audio
    // (rounded up to a sixteenth so it ends on the grid).
    std::int64_t naturalLength(int row, int source, std::int64_t start = 0) const;
    // Places a Session cell or a rack pattern in the Song over what is there, as one undo step at the clip's natural length.
    // It is the PlaceSongClip reducer, recorded under "Place pattern" or "Place clip"; the Song and Session views run the
    // command itself. `idOut` gets the placed clip's id.
    bool placeRegion(int row, int source, std::int64_t start, std::uint32_t* idOut = nullptr);
    // Live side of NewSongClip (see SongPlacementEdit.h): a new, empty MIDI clip for a track on the timeline, in a free
    // Session cell of that track, which is selected. CommandEngine owns the history entry.
    bool applyNewSongClip(int row, std::int64_t start, std::uint32_t* idOut = nullptr);
    bool applyHarmonicMidiCommand(const song_placement::HarmonicMidi& edit);
    // A finished Song-view audio take on one track: its file, the loaded (or streamed) audio, and
    // where it goes on the timeline.
    struct RecordedTake {
        int track{-1};
        juce::File file;
        std::shared_ptr<SessionAudioClip> clip;
        alien::TakePlacement placement;
        // A loop recording: one placement per pass of the loop (placement is then unused), and the pass
        // that goes on the song. The others are kept in the take list.
        std::vector<alien::TakePlacement> passes;
        std::size_t activePass{};
    };
    // Puts every take of one recording on the timeline as one undo step ("Record take"). Each take
    // becomes the audio of a Session cell of its track that is empty and unused by the timeline (a scene
    // is added when there is none) and a region at its placement, over whatever was there; each pass of
    // a loop recording becomes a take of that cell, and only the active pass is put on the song. When the
    // take list has no room for every pass, the earliest are left out. Nothing changes when any take
    // cannot be placed. Returns how many tracks' takes were placed.
    int addRecordedTakes(const std::vector<RecordedTake>& takes, const juce::String& label = "Record take");
    bool isTrackFrozen(int track) const noexcept;
    bool anyFrozenTrack() const noexcept;
    std::int64_t freezeEndTick(int track) const noexcept { return freeze_edit::endTick(doc, track); }
    juce::String freezeRefusal(int track) const;

    bool applyFreezeTrackCommand(const freeze_edit::Install& edit);
    // Live side of UnfreezeTrack (the Song track menu's Unfreeze). CommandEngine owns the history entry.
    bool applyUnfreezeCommand(int track);

    DrumSliceTarget drumSliceTarget(int track, int pad) const;
    // Drum pad edits (see DrumPadEdit.h) -------------------------------------
    // A sample cannot be sliced during a take, or while the last one is still finishing.
    bool drumSliceOpen() const { return projectScaleOpen(); }
    // Live side of the drum pad commands. CommandEngine owns the history entry.
    bool applyDrumPadCommand(const drum_pad::Edit& e);
    // Set by the app (command::routeDrumPadEdits) so the edits below run as commands. Returns why not, or nothing.
    std::function<juce::String(const drum_pad::Edit&, const juce::String&)> drumPadCommands;
    // Prepared edits must use the command engine for validation, publication and history.
    juce::String commitDrumPad(const drum_pad::Edit& e, const juce::String& label);
    // The track a deferred drum pad action was aimed at, if the project, the track and the pad's sample are still
    // the ones it saw; -1 otherwise.
    int drumTargetTrack(const DrumSliceTarget& target) const;
    bool editDrumSlice(const DrumSliceTarget& target, DrumSlice range);
    bool autoSliceDrum(const DrumSliceTarget& target);
    bool sliceDrum(int track, int pad);
    bool canSliceSessionAudio(int track, int scene) const;
    // What the person sees once a Session clip has become a kit (SliceClipToDrumPads).
    void sessionAudioSliced();
    // The same edit as its own undo step, for callers that are not a view (the Session view runs the command).
    bool sliceSessionAudio(int track, int scene);

private:
    static bool canSliceDrumSource(const std::shared_ptr<const SessionAudioClip>& audio, const juce::String& path);
public:
    void loadDrum(const juce::File& file, int track, int pad);
    // Channel Rack ------------------------------------------------------------
    const rack::Project& channelRack() const noexcept { return doc.channelRack; }
    // Sample assets for a render on another thread (null for synth channels, silent channels or missing files).
    // Synth presets travel in the rack snapshot. Loaded files in `keepAlive` must outlive the render.
    std::array<const SessionAudioClip*, rack::maxChannels> rackClipsForExport(std::vector<std::shared_ptr<const SessionAudioClip>>& keepAlive) const;

    // Applies `edit` to a copy of the rack. When it changes something valid the
    // copy becomes the rack, the engine plays it, and the change is one undo
    // step (edits with the same coalesceKey made in quick succession merge, so a
    // dragged paint stroke or fader is one step). Choosing which pattern is shown
    // is not an edit and is not recorded.
    bool editRack(const juce::String& label, const std::function<bool(rack::Project&)>& edit,
                  const juce::String& coalesceKey = {});
    // Live side of the rack channel commands. CommandEngine owns the history entry.
    // Why the loaded audio refuses slicing a channel: the file must be decoded here, and the slices inside it.
    juce::String rackSliceRefusal(const rack_channel::Edit& e) const;
    // Finds the attacks in a sample channel's audio (its slice, when it plays one) on a worker, then hands `found`
    // the slices' edges in the file's frames, ready for SliceRackChannel (T2-01). As many slices as the rack has room
    // for, 16 at most. Nothing is handed over when there are no useful attacks, or when the project, the channel or
    // the recording state changed meanwhile; the status line says which.
    // `addsChannels` false is for slices that stay on the channel's keys: sixteen at most, whatever room the rack has.
    bool findRackAttacks(int channel, std::function<void(const std::vector<std::int64_t>&)> found, bool addsChannels = true);
    bool applyRackChannelCommand(const rack_channel::Edit& e);
    // Live side of the rack step and pattern commands. CommandEngine owns the history entry.
    bool applyRackPatternCommand(const rack_pattern::Edit& e);
    // Live side of LoadRackSample. The file's decoded audio becomes available as the channel starts naming it.
    bool applyRackSampleCommand(const rack_channel::Edit& e, const std::shared_ptr<SessionAudioClip>& clip);
    // A new decode of a file the channel already names: no edit and no Undo step, but it is what plays from now on.
    bool replaceRackSampleAudio(const std::string& path, const std::shared_ptr<SessionAudioClip>& clip);
    // After a commit: the host rebuilds if which channel holds which plugin has changed.
    void rackChannelsCommitted() { notifyRackPlugins(); }
    // The owner hosts the rack's plugin instruments: told when which channel holds which one has changed
    // (a plugin chosen or replaced, a channel added, removed, moved or duplicated, undo and redo).
    std::function<void()> onRackPluginsChanged;
    // Keeps each rack plugin instrument's current state (from the host, by instance). Like capturing a track's
    // plugins before a save, it is not an undo step of its own.
    void setRackPluginStates(const std::vector<std::pair<std::uint64_t, juce::String>>& states);
    // The rack's plugin instruments in channel order (an empty state where a channel has none).
    std::vector<PluginState> rackPlugins() const;
    bool selectRackPattern(int index);
    // Resizes the Channel Rack's name column (clamped). Like choosing a pattern it is saved with the project but is
    // not an undo step, and the engine does not need to hear about it.
    bool setRackNameColumnWidth(int width) { return doc.channelRack.setNameColumnWidth(width); }
    // Sounds a channel now, whatever the transport is doing (clicking its name).
    void previewRackChannel(int channel, float velocity = 1.0f, int pitch = 0, bool held = false);
    // Rack samples arrive through command::loadRackSample: decode off the message
    // thread, then run LoadRackSample on the result. The project is busy until then.
    void decodeRackSample(const juce::File& file, std::function<void(const AudioClipImportResult&)> done);
    // What the person sees once the command has run or been refused.
    void rackSampleFinished(const juce::File& file, const juce::String& refused);
    // True when the channel names a file that is not loaded (moved or deleted).
    // The decoded audio a rack channel plays (its slice of a file, for a sliced channel), or null.
    const SessionAudioClip* rackChannelAudio(int channel) const;
    bool rackChannelOffline(int channel) const;

    // Test fixtures only: product code uses command::installDecodedAudio. Same checks and reducer as the command.
    bool installAudio(const juce::File& file, int track, int scene, std::shared_ptr<SessionAudioClip> clip);
    std::size_t availableAudioBytes() const noexcept { return maxSessionAudioBytes - retainedBytes; }

    // Audio the project refers to that is not loaded (see MissingMedia), clips
    // first in scene order, then drum pads.
    std::vector<MissingMedia> missingMedia() const;

    struct RelinkTarget {
        MissingMedia item;
        std::uint32_t trackId{}, sceneId{};
        library::ClipId cell;
        RelinkedAudio found;
    };
    struct PreparedRelink {
        std::uint32_t scope{};
        juce::File folder;
        std::vector<RelinkTarget> targets;
        bool truncated{};
    };
    using RelinkCompletion = std::function<void(std::shared_ptr<const PreparedRelink>)>;

    // Looks for every missing file by name inside `folder` and its subfolders,
    // loads what it finds off the message thread, and installs it all as one
    // undoable step. Files that are not found stay offline and keep their saved
    // reference, so nothing is lost by trying.
    void relinkMissingAudio(const juce::File& folder, RelinkCompletion complete);

private:
    // The sound a channel plays: a built-in drum sound (generated once and kept
    // for the life of the program), a loaded file, or nothing (silent or offline).
    const SessionAudioClip* rackClip(const rack::ChannelInfo& info) const { return rackAsset(info).get(); }
    // The same, owned. A sliced channel plays a view of its file's decoded audio (T2-01): the views are kept while the
    // open project's samples are (an open lets go of the rest once the engine has, D14), and shared by every channel playing that slice. A slice
    // past the end of its file, or of audio that is streamed, plays nothing.
    // A channel with key zones (T2-01) plays a key map: a clip with no audio of its own that names the slice for each
    // key. Maps are kept like the slice views, one for each file, range and set of zones.
    std::shared_ptr<const SessionAudioClip> rackAsset(const rack::ChannelInfo& info) const;
    // The audio a channel's sample gives before any key zones: the whole file, or the channel's slice of it.
    std::shared_ptr<const SessionAudioClip> rackRangeAsset(const rack::ChannelInfo& info) const;
    // Hands the engine the rack and the sound for each channel. Skipped when
    // nothing changed, so unrelated edits do not rebuild the snapshot.
    static std::uint64_t freshInstanceId();
    // A duplicated plugin channel is a second instance with the same sound: it gets an identity of its own.
    static void freshPluginInstances(rack::Project& p);
    void notifyRackPlugins();
    std::uint64_t notifiedRackPlugins{1469598103934665603ull};
    void pushRackToEngine();
    bool retain(const std::shared_ptr<SessionAudioClip>& clip);
    // T1-04 D5. Opening a project used to keep the audio of the one it replaced for the controller's
    // lifetime, so every open over an open project grew memory until maxSessionAudioBytes refused the next.
    // Called once the new project, its history and its published Song are in place: a clip that only
    // `retained` still owns belongs to nothing that could hand it out again (not the document, history,
    // clipboard, rack caches or a running job), so the engine's raw pointers are the only other way to reach
    // it. Those clips are set aside until the engine confirms it has let go. Anything else stays, as before.
    //
    // D14: `dropped` holds what the rack caches let go of (forgetUnusedRackAudio): slice views, key maps and
    // decoded files the new project does not use. A clip is set aside when every reference to it comes from
    // `retained`, `dropped` or another clip being set aside (a slice view's source, a key map's zones), so a
    // chain of views and maps goes together with the file under it. A dropped clip something else still
    // owns simply loses the cache's reference, once the engine has let go.
    struct ReplacedAudio { std::vector<std::shared_ptr<const SessionAudioClip>> clips; std::size_t bytes{}; };
    ReplacedAudio setAsideRetainedAudio(std::vector<std::shared_ptr<const SessionAudioClip>> dropped = {});
    // T1-04 D14. Message thread, when a project replaces the open one (after the new one is published and its
    // history reset): the Channel Rack caches keep only what the new project's channels use. Decoded rack
    // files, slice views and key maps used to be kept for the controller's lifetime, so each project opened
    // with different rack samples left the last one's decoded audio behind, counted against
    // maxSessionAudioBytes. Answers what the caches let go of, for setAsideRetainedAudio.
    //
    // D17: also run by the stopped-transport sweep (releaseUnreachableAudio), with `used` holding every file a
    // channel names in an undo or redo step, so a file the channel was switched away from stays while undo can
    // bring it back and goes once that step is gone.
    static void addRackFiles(const rack::Project& rackProject, std::set<std::string>& files);
    std::vector<std::shared_ptr<const SessionAudioClip>> forgetUnusedRackAudio(std::set<std::string> used = {});
    // Message thread, after the new project has been published to the engine: asks the callback to let go.
    void awaitAudioRelease(ReplacedAudio replaced);
public:
    // Message thread (the app's timer calls it): says in the status bar how many Channel Rack hits (T1-04 D10)
    // and track-instrument MIDI events (D11) the engine has not been able to play since the last report, at most
    // once a second so a long run of them does not flood it. Returns whether it reported.
    bool reportDroppedNotes(std::uint32_t nowMs = juce::Time::getMillisecondCounter());
    // Message thread (the app's timer calls it): says in the status bar which tracks' streamed audio dropped out or
    // could not be read since the last report (T1-04 D12), at most once a second. Returns whether it reported.
    bool reportStreamProblems(std::uint32_t nowMs = juce::Time::getMillisecondCounter());
    // Message thread (the app's timer calls it). Frees the audio of replaced projects once the engine has
    // answered the request made after the last of them; until then (or with no device running) it waits.
    // Returns the bytes freed.
    std::size_t releaseReplacedAudio();
    // Audio of replaced projects still waiting for the engine (T1-04 D5).
    std::size_t replacedAudioBytes() const noexcept { return replacedBytes; }
    // T1-04 D16. Message thread (the app's timer calls it). D5 frees audio only when a project replaces the open
    // one, so within a session every clip ever loaded stayed counted against maxSessionAudioBytes: a replaced
    // sample after its redo step was overwritten, a freeze render after Unfreeze and a new edit, a recording
    // take that was undone and replaced, audio whose undo step fell off the 200-step history. At most once a
    // second, while the transport is stopped and nothing is loading, recording or warping, the clips nothing
    // but `retained` can reach (not the document, history, clipboard, rack caches, a Song snapshot or a running
    // job) are set aside; the engine is asked to let go (editedAudioRequest), and the next call after it
    // answers frees them. Returns the bytes freed.
    std::size_t releaseUnreachableAudio(std::uint32_t nowMs = juce::Time::getMillisecondCounter());
    // Audio an edit left unreachable, waiting for the engine (T1-04 D16).
    std::size_t unreachableAudioBytes() const noexcept { return unreachableBytes; }
private:
    void registerStream(const std::shared_ptr<SessionAudioClip>& clip);
public:
    struct RelinkPlan {
        explicit RelinkPlan(const ProjectDocument& document) : after(document) {}
        ProjectDocument after;
        juce::String error;
        juce::StringArray stillOffline;
        std::vector<std::shared_ptr<const SessionAudioClip>> sources, drumViews;
        std::map<std::string, std::shared_ptr<const SessionAudioClip>> rackAssets;
        std::vector<std::pair<int, int>> cells;
        int relinked{};
        bool memoryFull{};
    };

    RelinkPlan planRelink(const PreparedRelink& prepared) const;

    void installRelink(RelinkPlan& plan);

    void reportRelink(const PreparedRelink& prepared, const RelinkPlan& plan);

private:
    static juce::String describeOffline(const std::vector<MissingMedia>& missing);

    struct LoadResult {
        SessionProjectData project;
        std::vector<std::shared_ptr<SessionAudioClip>> audio; // [scene * tracks + track]
        std::vector<std::array<std::shared_ptr<const SessionAudioClip>, drumPadCount>> drums;
        std::vector<std::pair<std::string, std::shared_ptr<SessionAudioClip>>> rackClips; // by resolved path
        std::size_t decodedBytes{};
        std::vector<MissingMedia> missing; // referenced audio that could not be loaded; the project still opens
        juce::String error;
    };

    static juce::String ui8(const char* bytes) { return juce::String(juce::CharPointer_UTF8(bytes)); }
    static bool validStep(int step) noexcept { return step >= 0 && step < 16; }

    // Worker thread.
    static LoadResult loadProjectFile(const juce::File& file);

    void finishLoad(const juce::File& file, const LoadResult& result, bool recovered);

    // Apply an undo/redo state and bring the engine in line with it.
    void restore(const ProjectDocument& state, const DocumentHistory::Step& step);

    // Human-readable cause when the document cannot be serialised.
    static juce::String describeInvalidSession(const ProjectDocument& d);

    void historyChanged() { if (onHistoryChanged) onHistoryChanged(); }

    // Recording helpers -----------------------------------------------------
    // Makes the target clip the one looping on its track. Overdub leaves a clip
    // that is already playing untouched; replace plays it silent so only the
    // new notes are heard while recording.
    void prepareClipForTake(int track, int scene, bool replace);

    // Make the engine play what the document holds for this clip.
    void syncLiveClip(int track, int scene);

    // The clip loops with everything recorded so far, so a take can be heard
    // layering up while it is made.
    void previewTake();

    void finishTake();

    // T1-04 D7: prepares the engine's slots for `tracks` tracks before an edit or open commits them. When
    // memory runs out it says so and answers false; the caller then changes nothing.
    bool prepareEngineTracks(int tracks, const juce::String& action);

    bool prepareRackChannels(int channels, const juce::String& action);

    void publishStop(std::size_t track);

    // `copyFrom`: a scene to copy. Its clips are copied as independent clips (an edit of one copy
    // does not reach the other), as Duplicate scene always did.
    bool insertScene(int index, const std::vector<library::ClipId>* copyFrom);

    void stopEverySlot();

    void setBusy(bool b) { busy = b; if (onBusyChanged) onBusyChanged(b); }
    void status(const juce::String& text, const juce::String& tip) { if (onProjectStatus) onProjectStatus(text, tip); }
    void importStatus(const juce::String& text, const juce::String& tip) { if (onImportStatus) onImportStatus(text, tip); }

    struct Take {
        bool active{}, replace{}, refused{}, cancelled{};
        int track{-1}, scene{-1};
        std::uint32_t id{};
        std::uint64_t droppedAtStart{};
        std::size_t previewedNotes{};
        RecordingAssembler assembler;
    };
    Take take;
    struct SongTake {
        bool active{}, abandoned{};
        std::uint32_t id{};
        std::vector<std::pair<int, std::shared_ptr<DiskRecording>>> recordings; // track, its take
        double sampleRate{44100.0};
        int bpm{120}, offsetMs{};
        std::vector<timeline::TempoChange> tempoChanges;   // the song's tempo map when the take began
        bool punch{};
        std::int64_t punchIn{}, punchOut{};
        std::int64_t loopStart{}, loopEnd{};   // where each pass of a loop recording begins and ends
    };
    SongTake songTake;

    void stopSongTake();
    void pollSongTake();
    // The engine has ended the take (or never started it) and no longer touches the sinks. The files are
    // finished and loaded off the message thread; then every take is placed in one undo step.
    void finishSongTake(bool refused);
    std::uint32_t lastTakeId{};
    library::ClipId copiedCell{};                  // what Copy remembered, for Paste linked
    std::optional<SessionClip> copiedContent;      // and its content, for Paste (kept if the cell changes)

    ToneEngine& engine;
    std::uint64_t reportedRackDrops{engine.droppedRackHits()};   // D10: hits already reported
    std::uint64_t reportedTrackDrops{engine.droppedTrackMidi()};  // D11: track MIDI events already reported
    std::uint32_t dropReportedAt{};
    bool dropReported{};
    // D12: stream underruns and failures already reported, per engine track slot.
    void markStreamProblemsReported() noexcept;
    std::array<std::uint64_t, ToneEngine::sessionTracks> reportedUnderruns{}, reportedStreamFailures{};
    std::uint32_t streamReportedAt{};
    bool streamReported{};
    ProjectDocument doc;
    juce::File projectFile;
    bool busy{};
    // Every decoded clip stays alive while the engine might reference it.
    std::vector<std::shared_ptr<const SessionAudioClip>> retained;
    std::size_t retainedBytes{};   // includes replacedAudio until it is freed
    std::vector<std::shared_ptr<const SessionAudioClip>> replacedAudio;   // T1-04 D5: waits for audioReleaseDone
    std::size_t replacedBytes{};
    std::uint64_t replacedFence{};
    std::vector<std::shared_ptr<const SessionAudioClip>> unreachableAudio;   // T1-04 D16: waits for editedAudioDone
    std::size_t unreachableBytes{};
    std::uint64_t unreachableFence{};
    std::uint32_t unreachableSweptAt{};
    bool unreachableSwept{};
    juce::ThreadPool pool{1};
    std::unique_ptr<juce::FileChooser> audioFileChooser, projectFileChooser;
    DocumentHistory history;
    std::uint64_t savedId{}, autosavedId{};
    bool runtimeDirty{};
    bool autosaveWarned{};
    PublishedSong published;
    std::atomic<bool> warpCancelled{};
    bool warpWorkActive{}; // message thread only
    // Decoded Channel Rack samples by path, kept (with `retained`) while the open project uses them, so the
    // engine can hold raw pointers to them. An open or New lets go of the rest (forgetUnusedRackAudio, D14).
    std::map<std::string, std::shared_ptr<const SessionAudioClip>> rackAudio;
    // Views of rack files for sliced channels, by file and frame range, and every one ever handed to the engine.
    mutable std::map<std::tuple<std::string, std::int64_t, std::int64_t>, std::shared_ptr<const SessionAudioClip>> rackSliceViews;
    mutable std::vector<std::shared_ptr<const SessionAudioClip>> retainedSliceViews;
    struct RackKeyMap {
        std::shared_ptr<const SessionAudioClip> file, base;
        std::vector<rack::KeyZone> zones;
        std::shared_ptr<const SessionAudioClip> map;
    };
    mutable std::vector<RackKeyMap> rackKeyMaps;   // kept, as the engine may still be playing one, until an open (D14)
    struct PublishedRack {
        bool known{};
        rack::Project project;
        std::array<const SessionAudioClip*, rack::maxChannels> clips{};
    } rackPublished;
    bool autosaving{};
    std::mutex recoveryMutex;
    std::atomic<std::uint64_t> recoveryGeneration{};
    juce::File recoveringFrom;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace alien
