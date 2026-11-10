#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <vector>

// A VST3 plugin's presets: its own programs, and .vstpreset files in the standard VST3 preset folders
// (<root>/<vendor>/<plugin>/..., where the roots are the user's Documents\VST3 Presets and the shared
// ProgramData\VST3 Presets) and in the bundle's Contents\Resources\Presets. Finding the files touches only the disk;
// listing programs and loading a preset run plugin code, so the caller (PluginHost) runs them in a FaultGuard, on the
// message thread, as a plugin's own preset menu would.
//
// Loading a preset is an edit made inside the plugin, like turning one of its knobs: the project is marked changed and
// the plugin's new state is saved with it, but it is not an Undo step.
namespace alien::plugin_presets {
inline constexpr int maxPresets = 4096;                       // files listed per plugin; more are left out and reported
inline constexpr juce::int64 maxPresetBytes = 16 * 1024 * 1024;
inline constexpr int maxPrograms = 1024;

struct Preset {
    juce::String name;      // the file name without ".vstpreset", or the program's name
    juce::String folder;    // where it came from: a subfolder ("Bass/Plucks"), "Factory", "Shared", or "Programs"
    juce::File file;        // a preset file, or none for a program
    int program{-1};        // a program number, or -1 for a file
    bool isProgram() const noexcept { return program >= 0; }
    bool operator==(const Preset&) const = default;
};

struct Found {
    std::vector<Preset> presets;
    bool truncated{};       // there were more than maxPresets files
};

// The folders a VST3 host looks in for presets on Windows: the user's, then the shared ones.
inline juce::Array<juce::File> standardRoots() {
    return {juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("VST3 Presets"),
            juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory).getChildFile("VST3 Presets")};
}

// A vendor or plugin name as a folder name: characters Windows does not allow in one become "_".
inline juce::String folderName(const juce::String& name) {
    auto s = name.trim();
    for (const auto c : juce::String("\\/:*?\"<>|")) s = s.replaceCharacter(c, '_');
    return s;
}

// The .vstpreset files for the plugin `vendor` / `plugin`, from each of `roots` and, when `bundle` is a .vst3 bundle
// folder, from its Contents\Resources\Presets. Sorted by folder, then name; at most maxPresets.
inline Found findFiles(const juce::String& vendor, const juce::String& plugin, const juce::Array<juce::File>& roots, const juce::File& bundle = {}) {
    Found found;
    const auto add = [&](const juce::File& dir, const juce::String& label) {
        if (!dir.isDirectory()) return;
        for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*.vstpreset", juce::File::findFiles)) {
            if (static_cast<int>(found.presets.size()) >= maxPresets) { found.truncated = true; return; }
            const auto file = entry.getFile();
            Preset p;
            p.name = file.getFileNameWithoutExtension();
            const auto sub = file.getParentDirectory().getRelativePathFrom(dir).replaceCharacter('\\', '/');
            p.folder = sub == "." ? label : label.isEmpty() ? sub : label + "/" + sub;
            p.file = file;
            found.presets.push_back(std::move(p));
        }
    };
    if (vendor.trim().isNotEmpty() && plugin.trim().isNotEmpty()) {
        for (int i = 0; i < roots.size(); ++i) {
            const auto label = i == 0 ? juce::String{} : juce::String("Shared");
            const auto exact = roots[i].getChildFile(vendor.trim()).getChildFile(plugin.trim());
            const auto safe = roots[i].getChildFile(folderName(vendor)).getChildFile(folderName(plugin));
            add(exact, label);
            if (safe != exact) add(safe, label);
        }
    }
    if (bundle.isDirectory() && bundle.hasFileExtension(".vst3")) add(bundle.getChildFile("Contents").getChildFile("Resources").getChildFile("Presets"), "Factory");
    std::stable_sort(found.presets.begin(), found.presets.end(), [](const Preset& a, const Preset& b) {
        if (const int c = a.folder.compareNatural(b.folder); c != 0) return c < 0;
        return a.name.compareNatural(b.name) < 0;
    });
    return found;
}

// The plugin's own programs, when it has more than one (one is just its current sound). Plugin code: guard it.
inline std::vector<Preset> programs(juce::AudioProcessor& plugin) {
    std::vector<Preset> list;
    const int count = std::min(plugin.getNumPrograms(), maxPrograms);
    if (count <= 1) return list;
    for (int i = 0; i < count; ++i) {
        Preset p;
        p.name = plugin.getProgramName(i).trim();
        if (p.name.isEmpty()) p.name = "Program " + juce::String(i + 1);
        p.folder = "Programs"; p.program = i;
        list.push_back(std::move(p));
    }
    return list;
}

// The indices of `presets` that match every word of `query` in their name or folder, ignoring case.
inline std::vector<int> filter(const std::vector<Preset>& presets, const juce::String& query) {
    juce::StringArray words;
    words.addTokens(query.toLowerCase(), " ", "\"");
    words.removeEmptyStrings();
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const auto text = (presets[static_cast<std::size_t>(i)].name + " " + presets[static_cast<std::size_t>(i)].folder).toLowerCase();
        if (std::all_of(words.begin(), words.end(), [&](const juce::String& w) { return text.contains(w); })) rows.push_back(i);
    }
    return rows;
}

namespace detail {
struct PresetSetter final : juce::ExtensionsVisitor {
    explicit PresetSetter(const juce::MemoryBlock& data) : data(data) {}
    void visitVST3Client(const VST3Client& client) override { vst3 = true; accepted = client.setPreset(data); }
    const juce::MemoryBlock& data;
    bool vst3{}, accepted{};
};
}

// Loads `preset` into `plugin`. Empty on success, otherwise why not. Plugin code: guard it.
inline juce::String load(juce::AudioProcessor& plugin, const Preset& preset) {
    if (preset.isProgram()) {
        if (preset.program >= plugin.getNumPrograms()) return "That program is no longer in " + plugin.getName() + ".";
        plugin.setCurrentProgram(preset.program);
        return {};
    }
    if (!preset.file.existsAsFile()) return "The preset file \"" + preset.file.getFileName() + "\" is gone.";
    if (preset.file.getSize() > maxPresetBytes) return "\"" + preset.name + "\" is larger than 16 MiB, so it is not loaded.";
    juce::MemoryBlock data;
    if (!preset.file.loadFileAsData(data) || data.isEmpty()) return "\"" + preset.file.getFileName() + "\" could not be read.";
    auto* instance = dynamic_cast<juce::AudioPluginInstance*>(&plugin);
    if (instance == nullptr) return "Only VST3 plugins load .vstpreset files.";
    detail::PresetSetter setter(data);
    instance->getExtensions(setter);
    if (!setter.vst3) return "Only VST3 plugins load .vstpreset files.";
    if (!setter.accepted) return plugin.getName() + " did not accept \"" + preset.name + "\". It may be for another plugin.";
    return {};
}
} // namespace alien::plugin_presets
