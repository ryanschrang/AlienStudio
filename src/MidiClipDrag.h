#pragma once
// The drag payload for a MIDI clip dragged out of the sample browser (BROWSE-01). It is a separate kind from a
// sample's (SampleDrag.h), so the Session and Song drop targets, which take audio, never see one; the piano
// roll and the Channel Rack take it and import the file (MidiClipImport.h, RackGrooveImport.h).
#include <juce_gui_basics/juce_gui_basics.h>

namespace alien::mididrag {
inline bool supported(const juce::File& file) { return file.hasFileExtension("mid;midi"); }
inline juce::var encode(const juce::File& file) {
    if (file == juce::File{} || !supported(file)) return {};
    return "alien-midi-clip\n" + file.getFullPathName();
}
inline juce::File decode(const juce::var& value) {
    if (!value.isString()) return {};
    const auto text = value.toString();
    const juce::String prefix("alien-midi-clip\n");
    if (!text.startsWith(prefix)) return {};
    const auto path = text.substring(prefix.length());
    if (path.containsAnyOf("\r\n") || !juce::File::isAbsolutePath(path)) return {};
    const juce::File file(path);
    return supported(file) ? file : juce::File{};
}
} // namespace alien::mididrag
