#pragma once
#include "SessionProject.h"

namespace alien {
// Legacy absolute references remain supported. Relative references are rooted at
// the project directory, never at the process working directory.
inline juce::File resolveProjectAudio(const juce::File& projectFile, const juce::String& path) {
    if (path.isEmpty()) return {};
    if (juce::File::isAbsolutePath(path)) return juce::File(path);
    const auto parent = projectFile.getParentDirectory();
    const auto file = parent.getChildFile(path);
    return file.isAChildOf(parent) ? file : juce::File{};
}

// Worker-thread operation. Copy into a new, owned media directory before
// atomically replacing the project JSON. Existing projects/media stay intact on
// failure, and filenames never collide with one another or older project saves.
inline bool writeSessionProjectFile(const juce::File& target, SessionProjectData project,
    bool collectAudio, SessionProjectData& savedProject, juce::String& error) {
    if (!project.valid()) { error = "Session data is invalid."; return false; }
    juce::File mediaDirectory;
    juce::StringArray originals, copies;
    auto fail = [&](const juce::String& reason) {
        error = reason;
        if (mediaDirectory != juce::File{}) mediaDirectory.deleteRecursively();
        return false;
    };
    auto collectPath = [&](juce::String& path) {
        if (path.isEmpty()) return true;
        const auto source = resolveProjectAudio(target, path);
        if (source == juce::File{}) return fail("Invalid audio reference: " + path);
        if (collectAudio) {
            if (!source.existsAsFile()) return fail("Missing audio: " + source.getFullPathName());
            int index = originals.indexOf(source.getFullPathName());
            if (index < 0) {
                if (mediaDirectory == juce::File{}) {
                    mediaDirectory = target.getParentDirectory().getChildFile(
                        target.getFileNameWithoutExtension() + "_media_" + juce::Uuid().toString());
                    if (mediaDirectory.createDirectory().failed()) return fail("Could not create project media directory.");
                }
                const auto destination = mediaDirectory.getChildFile(
                    juce::String(originals.size() + 1) + "_" + source.getFileName());
                if (!source.copyFileTo(destination)) return fail("Could not copy audio: " + source.getFullPathName());
                originals.add(source.getFullPathName());
                copies.add(destination.getRelativePathFrom(target.getParentDirectory()).replaceCharacter('\\', '/'));
                index = copies.size() - 1;
            }
            path = copies[index];
        } else {
            path = source.isAChildOf(target.getParentDirectory())
                ? source.getRelativePathFrom(target.getParentDirectory()).replaceCharacter('\\', '/')
                : source.getFullPathName();
        }
        return true;
    };
    for (auto& scene : project.clips) for (auto& clip : scene)
        if (!collectPath(clip.audioFilePath)) return false;
    for (auto& kit : project.drumPaths) for (auto& path : kit)
        if (!collectPath(path)) return false;
    const auto json = serializeSessionProject(project);
    juce::TemporaryFile temporary(target);
    if (json.isEmpty() || !temporary.getFile().replaceWithText(json)
        || !temporary.overwriteTargetFileWithTemporary())
        return fail("Could not write project file.");
    savedProject = std::move(project);
    error.clear();
    return true;
}
} // namespace alien
