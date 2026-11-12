#pragma once
#include "ProjectDocument.h"
#include "SceneCommands.h"
#include <cmath>
#include <memory>

// A decoded sample placed on the Song (the ImportSongAudio command): the audio goes into a Session cell of the
// track that neither holds a clip nor is used by the Song, in a new scene when the track has none, and a Song clip
// of the sample's length starts at `tick`. Pure: the controller and Preview both run it.
namespace alien::song_audio_import {
struct Request {
    int track{-1};
    std::int64_t tick{};
    juce::File file;
    std::shared_ptr<SessionAudioClip> clip;
    bool keepClips{};   // refuse rather than replace Song clips the sample would cover
};

inline juce::String apply(ProjectDocument& d, const Request& r) {
    if (!r.clip) return "There is no decoded audio to import.";
    if (r.track < 0 || r.track >= d.trackCount()) return "Choose a track for the sample.";
    if (d.tracks[static_cast<std::size_t>(r.track)].settings.frozen()) return "Unfreeze the track before importing a sample to it.";
    if (r.tick < 0) return "Sample does not fit in the Song.";
    const auto end = timeline::TempoMap{d.timeline.tempoChanges, static_cast<double>(d.bpm)}.tickAfter(static_cast<double>(r.tick), r.clip->frames / r.clip->sampleRate);
    if (end > timeline::maxLength || end <= r.tick) return "Sample does not fit in the Song.";
    const auto last = static_cast<std::int64_t>(std::ceil(end));
    if (r.keepClips && std::any_of(d.timeline.regions.begin(), d.timeline.regions.end(), [&](const auto& region) {
            return region.row == r.track && region.start < last && region.start + region.length > r.tick; }))
        return "Drop onto a free Song range; existing clips were preserved.";
    const juce::String unplaced = "Sample could not be placed in the Song.";
    auto next = d;
    const auto usedBySong = [&](int scene) {
        for (const auto& region : next.timeline.takes) if (region.row == r.track && region.source == scene) return true;
        for (const auto& region : next.timeline.regions) if (region.row == r.track && region.source == scene) return true;
        return false;
    };
    int scene = -1;
    for (int s = 0; s < next.sceneCount() && scene < 0; ++s)
        if (!next.cellClip(r.track, s) && !usedBySong(s)) scene = s;
    if (scene < 0) {
        if (!next.canAddScene() || scene_edit::apply(next, {scene_edit::Kind::create}).isNotEmpty()) return unplaced;
        scene = next.sceneCount() - 1;
    }
    auto clip = next.clip(r.track, scene);
    clip.audio = r.clip;
    clip.audioFileName = r.file.getFileName();
    clip.audioFilePath = r.file.getFullPathName();
    next.setClip(r.track, scene, clip);
    auto song = next.timeline;
    song.tempo = next.bpm;
    timeline::Region region;
    region.row = r.track; region.source = scene;
    region.start = r.tick; region.length = std::max<std::int64_t>(1, last - r.tick);
    if (!song.recordTake(region) || !song.valid(next.trackCount(), next.sceneCount(), next.channelRack.patternCount)) return unplaced;
    next.timeline = std::move(song);
    next.selectedTrack = r.track; next.selectedScene = scene;
    d = std::move(next);
    return {};
}
} // namespace alien::song_audio_import
