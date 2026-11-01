#pragma once
// The Starter Pack: drum one-shots and MIDI clips that ship next to the executable, in a
// "Starter Pack" folder copied there by the build. Every file comes from an openly licensed
// source; content/Starter Pack/manifest.csv says which, and tools/build_starter_pack.py
// rebuilds the folder from pinned downloads. Message thread only: it lists files on disk.
#include <juce_core/juce_core.h>
#include <algorithm>
#include <vector>

namespace alien::starterPack {

inline juce::File folder() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("Starter Pack");
}

// One drum sound: its category (the folder under Drums, such as "Kicks") and its file.
struct DrumSample {
    juce::String category;
    juce::File file;
};

// The pack's drum one-shots, by category then name, in a stable order. Empty when the pack
// is missing (a development build that has not copied it, or a user who deleted it).
inline std::vector<DrumSample> drumSamples(const juce::File& root = folder()) {
    std::vector<DrumSample> samples;
    const auto drums = root.getChildFile("Drums");
    if (!drums.isDirectory()) return samples;
    auto categories = drums.findChildFiles(juce::File::findDirectories, false);
    std::sort(categories.begin(), categories.end(), [](const auto& a, const auto& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
    for (const auto& category : categories) {
        auto files = category.findChildFiles(juce::File::findFiles, false, "*.wav");
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
        for (const auto& file : files) samples.push_back({category.getFileName(), file});
    }
    return samples;
}

} // namespace alien::starterPack
