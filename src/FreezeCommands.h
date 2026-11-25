#pragma once
#include "ProjectDocument.h"
#include "SceneCommands.h"
#include <algorithm>

// Pure document edit for unfreezing one Song track (the UnfreezeTrack command). The controller and
// command Preview share it, so what Preview predicts is what the live edit does.
namespace alien::freeze_edit {
inline juce::String refusal(const ProjectDocument& d, int track) {
    if (track < 0 || track >= d.trackCount())
        return "There is no track " + juce::String(track + 1) + " (the project has " + juce::String(d.trackCount()) + ").";
    return d.tracks[static_cast<std::size_t>(track)].settings.frozen() ? juce::String{}
        : "Track " + juce::String(track + 1) + " is not frozen.";
}

// Puts the track's original Song clips back in place of its frozen audio, forgets the frozen
// render's Session cell and, once no track is frozen, gives the song back the length it had.
inline juce::String unfreeze(ProjectDocument& d, int track) {
    if (const auto why = refusal(d, track); why.isNotEmpty()) return why;
    if (!d.timeline.unfreezeTrack(track)) return "The track's original Song clips could not be restored.";
    auto& settings = d.tracks[static_cast<std::size_t>(track)].settings;
    const int scene = settings.freezeScene;
    settings.freezeScene = -1;
    if (d.isValidCell(track, scene)) d.clearClip(track, scene);
    if (std::none_of(d.tracks.begin(), d.tracks.end(), [](const auto& t) { return t.settings.frozen(); })) {
        d.timeline.lengthBars = std::max(d.timeline.freezeBaseLengthBars, d.timeline.minLengthBars());
        d.timeline.freezeBaseLengthBars = 0;
    }
    return {};
}

struct Target {
    std::uint32_t scope{}, trackId{};
    int track{-1};
    int bpm{};
    std::shared_ptr<const timeline::Timeline> timeline;
};

inline Target capture(const ProjectDocument& doc, int track) {
    if (track < 0 || track >= doc.trackCount()) return {};
    return {doc.identityScope, doc.tracks[static_cast<std::size_t>(track)].id, track, doc.bpm,
            std::make_shared<const timeline::Timeline>(doc.timeline)};
}

struct Install {
    Target target;
    juce::File file;
    std::shared_ptr<SessionAudioClip> audio;
};

inline std::int64_t endTick(const ProjectDocument& doc, int track) noexcept {
    std::int64_t end = 0;
    for (const auto& region : doc.timeline.regions)
        if (region.row == track) end = std::max(end, region.end());
    return end;
}

// The scene whose cell on `track` takes the frozen audio: the first empty cell that no Song clip, take or
// frozen original uses, or sceneCount() when there is none and a scene has to be added.
inline int freeCell(const ProjectDocument& doc, int track) {
    int scene = 0;
    for (; scene < doc.sceneCount(); ++scene) {
        if (!doc.clip(track, scene).isEmpty()) continue;
        const auto usesCell = [=](const timeline::Region& region) {
            return region.row == track && region.source == scene;
        };
        if (std::none_of(doc.timeline.regions.begin(), doc.timeline.regions.end(), usesCell)
            && std::none_of(doc.timeline.takes.begin(), doc.timeline.takes.end(), usesCell)
            && std::none_of(doc.timeline.frozenOriginals.begin(), doc.timeline.frozenOriginals.end(), usesCell))
            break;
    }
    return scene;
}

inline juce::String freezeRefusal(const ProjectDocument& doc, int track) {
    for (int s = 0; s < doc.sceneCount(); ++s)
        if (doc.isValidCell(track, s) && doc.clip(track, s).rackClip != 0)
            return "Clear rack launch assignments before freezing this Session track. Rack channels keep their own routing.";
    if(track<0 || track>=doc.trackCount())return "Choose a Session track.";
    if(doc.tracks[static_cast<std::size_t>(track)].settings.frozen())return "Track is already frozen.";
    if(endTick(doc, track)==0)return "Place this track's clips in Song before freezing.";
    for(int c=0;c<doc.channelRack.channelCount;++c)if(doc.channelRack.channels[static_cast<std::size_t>(c)].output==track)
        return "Freeze currently supports Session tracks without Channel Rack inputs. Reroute those channels first.";
    for(const auto& region:doc.timeline.regions)if(region.row==track) {
        if(doc.placementContent(region).audioOffline())return "Relink missing audio before freezing.";
        if(region.group!=0)for(const auto& other:doc.timeline.regions)if(other.group==region.group && other.row!=track)
            return "Ungroup clips shared with other tracks before freezing.";
    }
    // T1-04 D19: refused before the render, rather than after it, when the frozen audio has nowhere to go.
    if (freeCell(doc, track) == doc.sceneCount() && !doc.canAddScene())
        return "The Session is at its scene limit and this track has no empty cell for the frozen audio. "
               "Clear an unused cell on this track or delete a scene, then freeze.";
    return {};
}

inline juce::String install(ProjectDocument& doc, const Install& edit, float playbackLevel) {
    const auto& target = edit.target;
    const int track = target.track;
    if (target.scope != doc.identityScope || track < 0 || track >= doc.trackCount()
        || doc.tracks[static_cast<std::size_t>(track)].id != target.trackId
        || doc.bpm != target.bpm || !target.timeline || doc.timeline != *target.timeline)
        return "The project or track changed while rendering the freeze.";
    if (const auto why = freezeRefusal(doc, track); why.isNotEmpty()) return why;
    if (edit.file == juce::File{} || !edit.audio || !edit.audio->valid())
        return "The freeze render did not produce valid audio.";
    const timeline::TempoMap tempo{doc.timeline.tempoChanges, static_cast<double>(doc.bpm)};
    const auto ticks = tempo.tickAfter(0.0, static_cast<double>(edit.audio->frames) / edit.audio->sampleRate);
    if (!std::isfinite(ticks) || ticks < 1 || ticks > timeline::maxLength)
        return "The freeze render is outside the supported Song length.";

    auto candidate = doc;
    const int scene = freeCell(candidate, track);
    if (scene == candidate.sceneCount())
        if (const auto why = scene_edit::apply(candidate, {scene_edit::Kind::create}); why.isNotEmpty()) return why;

    auto cache = candidate.clip(track, scene);
    cache.audio = edit.audio;
    cache.audioFilePath = edit.file.getFullPathName();
    cache.audioFileName = edit.file.getFileName();
    candidate.setClip(track, scene, cache);
    if (std::none_of(candidate.tracks.begin(), candidate.tracks.end(), [](const auto& t) { return t.settings.frozen(); }))
        candidate.timeline.freezeBaseLengthBars = candidate.timeline.lengthBars;
    if (!candidate.timeline.freezeTrack(track, scene, static_cast<std::int64_t>(std::ceil(ticks)), 1.0 / playbackLevel))
        return "The freeze render could not replace the original Song clips.";
    candidate.tracks[static_cast<std::size_t>(track)].settings.freezeScene = scene;
    if (!toProjectData(candidate).valid()) return "The frozen track would make the project invalid.";
    doc = std::move(candidate);
    return {};
}
} // namespace alien::freeze_edit
