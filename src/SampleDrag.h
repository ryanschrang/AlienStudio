#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace alien::sampledrag {
inline juce::var encode(const juce::File& file) {
    if (file == juce::File{} || !file.hasFileExtension("wav;mp3")) return {};
    return "alien-local-sample\n" + file.getFullPathName();
}
inline juce::File decode(const juce::var& value) {
    if (!value.isString()) return {};
    const auto text = value.toString();
    const juce::String prefix("alien-local-sample\n");
    if (!text.startsWith(prefix)) return {};
    const auto path = text.substring(prefix.length());
    if (path.containsAnyOf("\r\n") || !juce::File::isAbsolutePath(path)) return {};
    const juce::File file(path);
    return file.hasFileExtension("wav;mp3") ? file : juce::File{};
}
} // namespace alien::sampledrag
