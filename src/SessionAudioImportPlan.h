#pragma once
#include "ProjectDocument.h"
#include "AudioClipImport.h"
#include <optional>

namespace alien::session_audio_import {
struct Plan {
    juce::File file;
    std::uint32_t scope{}, trackId{}, sceneId{};
    library::ClipId cell;
    std::shared_ptr<const library::PatternData> content;
    SessionClip previous;
    bool emptyOnly{}; // a browser or Explorer drop never replaces what a cell already holds
};
struct Prepared { Plan plan; AudioClipImportResult decoded; };
inline std::pair<int,int> resolve(const ProjectDocument& d, const Plan& p) {
    if (d.identityScope != p.scope) return {-1,-1};
    int track=-1,scene=-1;
    for(int t=0;t<d.trackCount();++t)if(d.tracks[static_cast<std::size_t>(t)].id==p.trackId)track=t;
    for(int s=0;s<d.sceneCount();++s)if(d.scenes[static_cast<std::size_t>(s)].id==p.sceneId)scene=s;
    return {track,scene};
}
inline juce::String destinationRefusal(const ProjectDocument& d,int track,int scene) {
    if(!d.isValidCell(track,scene))return "The audio-import destination no longer exists.";
    if(d.tracks[static_cast<std::size_t>(track)].settings.frozen())return "Unfreeze the destination before importing audio.";
    if(d.clip(track,scene).rackClip)return "Clear the rack assignment before importing audio into this cell.";
    const auto link=d.linkOf(track,scene);
    if(link)for(int s=0;s<d.sceneCount();++s)for(int t=0;t<d.trackCount();++t)
        if(d.linkOf(t,s)==link && d.tracks[static_cast<std::size_t>(t)].settings.frozen())return "Unfreeze linked tracks before replacing their shared audio.";
    return {};
}
inline std::optional<Plan> prepare(const ProjectDocument& d,const juce::File& file,int track,int scene,bool emptyOnly=false) {
    if(file==juce::File{} || destinationRefusal(d,track,scene).isNotEmpty() || (emptyOnly && d.cellClip(track,scene)))return {};
    const auto id=d.cellClip(track,scene);
    return Plan{file,d.identityScope,d.tracks[static_cast<std::size_t>(track)].id,d.scenes[static_cast<std::size_t>(scene)].id,id,d.library.content(id),d.clip(track,scene),emptyOnly};
}
inline juce::String refusal(const ProjectDocument& d,const Plan& p) {
    if(p.file==juce::File{})return "The audio-import plan has no source file.";
    const auto [track,scene]=resolve(d,p);
    if(const auto why=destinationRefusal(d,track,scene);why.isNotEmpty())return why;
    if(p.emptyOnly && d.cellClip(track,scene))return "Destination is no longer empty.";
    const auto now=d.clip(track,scene);
    if(d.cellClip(track,scene)!=p.cell || d.library.content(p.cell)!=p.content
        || now.audio!=p.previous.audio || now.audioFilePath!=p.previous.audioFilePath || now.audioFileName!=p.previous.audioFileName)
        return "The clip changed while audio was being decoded; import again.";
    return {};
}
inline juce::String refusal(const ProjectDocument& d,const Prepared& p) {
    if(const auto why=refusal(d,p.plan);why.isNotEmpty())return why;
    if(!p.decoded.clip || !p.decoded.clip->valid())return p.decoded.error.isNotEmpty()?p.decoded.error:juce::String("Audio decoding did not produce a valid clip.");
    return {};
}
// No decoding, retention, engine publication or history: the ImportClipAudio
// command's Preview uses this directly. Only a validated candidate is installed.
inline juce::String apply(ProjectDocument& d,const Prepared& p) {
    if(const auto why=refusal(d,p);why.isNotEmpty())return why;
    const auto [track,scene]=resolve(d,p.plan);auto clip=d.clip(track,scene);
    clip.audio=p.decoded.clip;clip.audioFileName=p.plan.file.getFileName();clip.audioFilePath=p.plan.file.getFullPathName();
    d.setClip(track,scene,clip);return {};
}
} // namespace alien::session_audio_import
