#pragma once
// Files dragged in from Windows Explorer. A drop takes the same route as a drag out of the sample browser: the
// paths become that drag's payload, so the targets' checks, previews and imports are the same code. One file at a
// time: a Session cell or a Song position names one destination, and a partial import of several would be worse
// than a clear refusal.
#include "MidiClipDrag.h"
#include "SampleDrag.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace alien::filedrop {
inline const char* const oneFileMessage = "Drop one file at a time. Nothing was imported.";

inline bool anyWithExtension(const juce::StringArray& paths, const char* extensions) {
    for (const auto& path : paths)
        if (juce::File::isAbsolutePath(path) && juce::File(path).hasFileExtension(extensions)) return true;
    return false;
}
inline bool anySample(const juce::StringArray& paths) { return anyWithExtension(paths, "wav;mp3"); }
inline bool anyMidi(const juce::StringArray& paths) { return anyWithExtension(paths, "mid;midi"); }

// The browser drag's payload for exactly one file of that kind; void otherwise, which every target refuses.
inline juce::var samplePayload(const juce::StringArray& paths) {
    return paths.size() == 1 && juce::File::isAbsolutePath(paths[0]) ? sampledrag::encode(juce::File(paths[0])) : juce::var{};
}
inline juce::var midiPayload(const juce::StringArray& paths) {
    return paths.size() == 1 && juce::File::isAbsolutePath(paths[0]) ? mididrag::encode(juce::File(paths[0])) : juce::var{};
}
inline juce::DragAndDropTarget::SourceDetails details(const juce::var& payload, int x, int y) {
    return {payload, nullptr, {x, y}};
}

// The main window's tabs that can take a file.
enum Tab { pianoRollTab = 1, sessionTab = 2, songTab = 5, channelRackTab = 8 };
// Where a file held over a view that cannot take it should open, as a drag from the browser does; -1 stays put
// (several files, or already on a view that can take it). `drums` is asked only when a MIDI file needs a new view.
template <typename IsDrums>
int landingTab(const juce::StringArray& paths, int currentTab, bool devicesShown, IsDrums&& drums) {
    if (paths.size() != 1) return -1;
    if (anySample(paths)) return !devicesShown && (currentTab == sessionTab || currentTab == songTab) ? -1 : static_cast<int>(sessionTab);
    if (anyMidi(paths)) {
        if (!devicesShown && (currentTab == pianoRollTab || currentTab == channelRackTab)) return -1;
        return drums(juce::File(paths[0])) ? channelRackTab : pianoRollTab;
    }
    return -1;
}
} // namespace alien::filedrop
