#pragma once
#include "AudioClipImport.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

namespace alien {

// An audio file a project refers to that could not be loaded: moved, deleted,
// on a drive that is not connected, or unreadable. The project still opens; the
// clip or drum pad stays silent until the file is relinked.
struct MissingMedia {
    bool drumPad{};      // false: a Session audio clip; true: a drum sampler pad
    int track{}, slot{}; // slot is the scene for a clip, the pad for a drum pad
    juce::String path;   // absolute path as the project last knew it
    juce::String reason; // why it could not be loaded
    bool rackChannel{};  // true: a Channel Rack sample; track is then the channel index and slot is 0

    juce::String fileName() const { return fileNameOf(path); }
    static juce::String fileNameOf(const juce::String& path) {
        return path.replaceCharacter('\\', '/').fromLastOccurrenceOf("/", false, false);
    }
};

// Worker-thread index of the audio files under a folder, used to find media
// again by name. Nothing here touches the document or the audio thread.
class MediaIndex {
public:
    // Large enough for a sample library, small enough that choosing a drive
    // root by mistake cannot scan forever.
    static constexpr std::size_t maxFilesIndexed = 100000;

    explicit MediaIndex(const juce::File& root, const std::atomic<bool>* cancelFlag = nullptr) {
        // Extensions are matched here, case-insensitively, exactly as the importer does
        // (a wildcard pattern would be case-sensitive on some filesystems and miss "KICK.WAV").
        juce::RangedDirectoryIterator iterator(root, true, "*", juce::File::findFiles,
                                               juce::File::FollowSymlinks::noCycles);
        for (const auto& entry : iterator) {
            if (cancelFlag != nullptr && cancelFlag->load()) { cancelled = true; break; }
            const auto file = entry.getFile();
            const auto extension = file.getFileExtension().toLowerCase();
            if (extension != ".wav" && extension != ".mp3") continue;
            if (count >= maxFilesIndexed) { truncated = true; break; }
            byName[file.getFileName().toLowerCase()].push_back(file);
            ++count;
        }
    }

    // Files with the same name as originalPath, best first: the most trailing
    // folder names in common with the original location, then the shallowest.
    // A project that was moved as a whole keeps its folder layout, so this picks
    // the right "kick.wav" when several folders each contain one.
    std::vector<juce::File> candidates(const juce::String& originalPath) const {
        const auto found = byName.find(MissingMedia::fileNameOf(originalPath).toLowerCase());
        if (found == byName.end()) return {};
        auto result = found->second;
        const auto wanted = components(originalPath);
        std::stable_sort(result.begin(), result.end(), [&](const juce::File& a, const juce::File& b) {
            const int scoreA = sharedTail(wanted, components(a.getFullPathName()));
            const int scoreB = sharedTail(wanted, components(b.getFullPathName()));
            if (scoreA != scoreB) return scoreA > scoreB;
            const auto lengthA = a.getFullPathName().length(), lengthB = b.getFullPathName().length();
            if (lengthA != lengthB) return lengthA < lengthB;
            return a.getFullPathName() < b.getFullPathName();
        });
        return result;
    }

    std::size_t filesIndexed() const noexcept { return count; }
    bool wasTruncated() const noexcept { return truncated; }
    bool wasCancelled() const noexcept { return cancelled; }

private:
    static juce::StringArray components(const juce::String& path) {
        juce::StringArray parts;
        parts.addTokens(path.replaceCharacter('\\', '/').toLowerCase(), "/", "");
        parts.removeEmptyStrings();
        return parts;
    }
    static int sharedTail(const juce::StringArray& a, const juce::StringArray& b) {
        int shared = 0;
        while (shared < a.size() && shared < b.size() && a[a.size() - 1 - shared] == b[b.size() - 1 - shared]) ++shared;
        return shared;
    }

    std::map<juce::String, std::vector<juce::File>> byName;
    std::size_t count{};
    bool truncated{}, cancelled{};
};

// Tries the candidates in order and returns the first one that decodes.
struct RelinkedAudio {
    juce::File file;
    std::shared_ptr<SessionAudioClip> clip;
    juce::String error;
};
inline RelinkedAudio decodeFirstCandidate(const std::vector<juce::File>& candidates, std::size_t maxTries = 3) {
    RelinkedAudio result;
    if (candidates.empty()) { result.error = "No file with this name in the chosen folder."; return result; }
    for (std::size_t i = 0; i < candidates.size() && i < maxTries; ++i) {
        auto decoded = decodeSessionAudioClip(candidates[i]);
        if (decoded.clip) { result.file = candidates[i]; result.clip = std::move(decoded.clip); result.error.clear(); return result; }
        result.error = decoded.error;
    }
    return result;
}

} // namespace alien
