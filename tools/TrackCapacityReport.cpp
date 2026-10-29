// T1-04 diagnostic: measure selected compiled types without constructing an
// engine, opening a device, or allocating a hypothetical project's contents.
#include "Arrangement.h"
#include "PianoRollSequence.h"
#include "RackModel.h"
#include "RackEngine.h"
#include "SessionAudioClip.h"
#include "SessionCapacity.h"
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {
using Bytes = std::uint64_t;
constexpr Bytes noteMailbox = sizeof(alien::PianoRollMailbox);
constexpr Bytes launchMailbox = sizeof(alien::SessionClipMailbox);
constexpr Bytes scheduler = sizeof(alien::PianoRollScheduler);
// ToneEngine preallocates one of each type per configured Session track.
constexpr Bytes noteStatePerTrack = noteMailbox + launchMailbox + scheduler;
constexpr Bytes clipReference = sizeof(alien::ArrangementClip);
constexpr Bytes inFlightSongSlots = 3; // ArrangementMailbox owns three snapshots.
struct Profile { const char* label; Bytes tracks, scenes; };
constexpr std::array profiles{
    Profile{"current configured limits", alien::maxSessionTracks, alien::maxSessionScenes},
    Profile{"hypothetical", 32, 64},
    Profile{"hypothetical", 64, 128},
    Profile{"hypothetical; not a supported target", 500, 128}
};
static_assert(sizeof(alien::ArrangementPlayback) >= clipReference
    * alien::maxSessionTracks * alien::maxSessionScenes);
void object(const char* label, Bytes bytes) { std::cout << "| " << label << " | " << bytes << " |\n"; }
}

int main() {
    std::cout << "# Track capacity baseline\n\n"
        << "Compiler-measured static object sizes on a " << sizeof(void*) * 8 << "-bit build.\n"
        << "Compiled limits: " << alien::maxSessionTracks << " Session tracks, "
        << alien::maxSessionScenes << " scenes, " << alien::PianoRollPattern::maxNotes
        << " notes per sequence; " << alien::rack::maxChannels << " rack channels and "
        << alien::rack::maxPatterns << " rack patterns.\n\n"
        << "Rack occurrence slots: " << alien::rack::RackPlayer::launchSlots << " launch, "
        << alien::rack::RackPlayer::timelineSlots << " Song; shared voice bound "
        << alien::rack::RackPlayer::voiceCount << ", shared pending-hit bound " << alien::rack::RackPlayer::maxPending << ".\n\n"
        << "| Compiled object | Bytes |\n| --- | ---: |\n";
    object("PianoRollPattern", sizeof(alien::PianoRollPattern));
    object("PianoRollMailbox (three note buffers)", noteMailbox);
    object("SessionClipMailbox (three launch buffers)", launchMailbox);
    object("PianoRollScheduler (owned note/event state)", scheduler);
    object("Selected note state per configured Session track", noteStatePerTrack);
    object("ArrangementClip (source references)", clipReference);
    object("ArrangementPlayback (shallow object; not its pointees)", sizeof(alien::ArrangementPlayback));
    object("rack::RackPlayer (shallow)", sizeof(alien::rack::RackPlayer));
    object("rack::RackPlayer fixed heap pools", alien::rack::RackPlayer::fixedStorageBytes());
    object("rack::Pattern (shallow object; not pointed-to note rows)", sizeof(alien::rack::Pattern));
    object("rack::Snapshot (shallow object; not its pattern/note pointees)", sizeof(alien::rack::Snapshot));
    std::cout << "\n| Profile | Tracks | Scenes | Selected preallocated note state (bytes) | Three Song reference tables (bytes) | Note state (MiB) |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: |\n" << std::fixed << std::setprecision(3);
    for (const auto& p : profiles) {
        const Bytes notes = noteStatePerTrack * p.tracks;
        const Bytes tables = clipReference * p.tracks * p.scenes * inFlightSongSlots;
        std::cout << "| " << p.label << " | " << p.tracks << " | " << p.scenes << " | "
            << notes << " | " << tables << " | " << static_cast<double>(notes) / (1024.0 * 1024.0) << " |\n";
    }
    std::cout << "\nThese are selected structural costs, not total process/engine memory or CPU measurements.\n"
        << "Note-state bytes = tracks * (sizeof(PianoRollMailbox) + sizeof(SessionClipMailbox) + sizeof(PianoRollScheduler)).\n"
        << "Table bytes = 3 * tracks * scenes * sizeof(ArrangementClip); snapshot headers and pointees are excluded.\n"
        << "Hypothetical rows keep the compiled note limit and current object layout. They do not raise any limits.\n"
        << "Excluded from the track projections: plugin instances/state, delay-line sample allocations, decoded audio, stream buffers, synth/drum voices,\n"
        << "rack note pointees, document/history metadata, UI, allocator overhead and live/export performance.\n"
        << "The engine preallocates these note objects by configured capacity, even for inactive tracks.\n"
        << "The separately listed RackPlayer pool is fixed at construction; it includes rack voices, pending hits and per-voice synths.\n"
        << "Do not add its shallow object twice or multiply its shared pools by the occurrence count.\n"
        << "No engine or hypothetical large project was constructed by this report.\n";
}
