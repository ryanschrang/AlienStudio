#pragma once
#include "Arrangement.h"
#include "AudioClipImport.h"
#include "StreamingTempoWarp.h"

namespace alien {
// A cache belongs to its published snapshot/stream source, never to the project.
// It disappears when the playback engine releases its retained sources.
struct WarpCacheDirectory {
    juce::File path;
    ~WarpCacheDirectory() { path.deleteRecursively(); } // uniquely generated cache directory only
};
class WarpCacheFile final : public DiskAudioFile {
public:
    explicit WarpCacheFile(std::shared_ptr<JuceDiskAudioFile> source, std::shared_ptr<WarpCacheDirectory> folder)
        : DiskAudioFile(source->sampleRate, source->channels, source->frames), directory(std::move(folder)), file(std::move(source)) {}
    std::unique_ptr<DiskAudioReader> open() const override { return file->open(); }
private:
    std::shared_ptr<WarpCacheDirectory> directory;
    std::shared_ptr<JuceDiskAudioFile> file;
};

// Worker only. Bounded decoding buffers support both decoded and long-file clips.
inline std::shared_ptr<SessionAudioClip> renderRegionWarp(const SessionAudioClip& input,
    const timeline::Region& region, const timeline::Timeline& song, std::int64_t tailTicks,
    const std::atomic<bool>& cancel, juce::String& error) {
    if (!input.valid()) {error="Invalid warp source audio.";return {};}
    const timeline::TempoMap tempo{song.tempoChanges, song.tempo};
    const auto totalSeconds = tempo.secondsBetween(static_cast<double>(region.start), static_cast<double>(region.end() + tailTicks));
    if (!timeline::validWarp(region.warp) || region.warp.empty() || !std::isfinite(totalSeconds) || totalSeconds <= 0
        || totalSeconds * input.sampleRate > static_cast<double>(INT64_MAX - 8192)) { error = "Invalid warp duration."; return {}; }
    const auto total = static_cast<std::int64_t>(std::ceil(totalSeconds * input.sampleRate));
    auto sourceAt = [&](std::int64_t frame) {
        const double tick = tempo.tickAfter(static_cast<double>(region.start), frame / input.sampleRate);
        return timeline::warpSeconds(region.warp, region.offset + tick - region.start) * input.sampleRate;
    };
    const double firstSource=sourceAt(0),lastSource=sourceAt(total);
    if(!std::isfinite(firstSource) || !std::isfinite(lastSource) || std::abs(firstSource)>static_cast<double>(INT64_MAX/2) || std::abs(lastSource)>static_cast<double>(INT64_MAX/2)) {error="Warp source range is too large.";return {};}
    // Refuse extreme settings rather than silently changing pitch or using an unbounded search.
    std::vector<double> breaks{static_cast<double>(region.start), static_cast<double>(region.end() + tailTicks)};
    for (const auto& m : region.warp) breaks.push_back(region.start + m.tick - static_cast<double>(region.offset));
    for (const auto& t : song.tempoChanges) breaks.push_back(static_cast<double>(t.tick));
    std::sort(breaks.begin(), breaks.end());
    for (std::size_t i=1;i<breaks.size();++i) {
        const double a=std::max(breaks[i-1], static_cast<double>(region.start)), b=std::min(breaks[i], static_cast<double>(region.end()+tailTicks));
        if (b<=a) continue;
        const double sourceSeconds=timeline::warpSeconds(region.warp,region.offset+b-region.start)-timeline::warpSeconds(region.warp,region.offset+a-region.start);
        const double duration=tempo.secondsBetween(a,b);
        if (sourceSeconds > 0 && (duration/sourceSeconds < .125 || duration/sourceSeconds > 8)) { error="Warp segment exceeds the supported 1/8 to 8x duration range."; return {}; }
    }
    std::unique_ptr<DiskAudioReader> reader;
    if (input.streamed()) { reader=input.streamSource->open(); if (!reader) { error="Cannot open warp source audio."; return {}; } }
    auto directory=std::make_shared<WarpCacheDirectory>();
    directory->path=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienWarpCache-"+juce::Uuid().toString(), "", false);
    if(directory->path.createDirectory().failed()){error="Cannot create the warp cache directory.";return {};}
    const auto path=directory->path.getChildFile("audio.wav");
    std::unique_ptr<juce::OutputStream> stream=path.createOutputStream();
    juce::WavAudioFormat format;
    const auto options=juce::AudioFormatWriterOptions{}.withSampleRate(input.sampleRate).withNumChannels(input.channels)
        .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto output=format.createWriterFor(stream,options);
    if (!output) {error="Cannot create a floating-point warp cache.";return {};}
    const auto sourceLength=static_cast<std::int64_t>(input.frames);
    const auto loopStart=static_cast<std::int64_t>(std::clamp(region.audioOffset*input.sampleRate,0.0,static_cast<double>(sourceLength-1)));
    std::vector<float> left(16384),right(16384);
    auto read=[&](float* const* data,int channels,std::int64_t start,int count) {
        if (count>static_cast<int>(left.size()) || cancel.load()) return false;
        auto physical=[&](std::int64_t logical) {
            if (region.loopSource && logical>=sourceLength) logical=loopStart+(logical-loopStart)%(sourceLength-loopStart);
            return region.reversed ? sourceLength-1-logical : logical;
        };
        for (int n=0;n<count;) {
            const auto first=physical(start+n);
            if (first<0 || first>=sourceLength) { for(int c=0;c<channels;++c)data[c][n]=0; ++n; continue; }
            int span=1; const int direction=region.reversed?-1:1;
            while(n+span<count && physical(start+n+span)==first+direction*span && first+direction*span>=0 && first+direction*span<sourceLength) ++span;
            const auto begin=direction==1?first:first-span+1;
            if (reader) { if(!reader->read(begin,left.data(),input.channels==2?right.data():nullptr,span))return false; }
            else for(int k=0;k<span;++k) {left[k]=input.left[static_cast<std::size_t>(begin+k)]; if(channels==2)right[k]=input.right[static_cast<std::size_t>(begin+k)];}
            for(int k=0;k<span;++k) {const int at=direction==1?k:span-1-k;data[0][n+k]=left[at];if(channels==2)data[1][n+k]=right[at];}
            n+=span;
        }
        return true;
    };
    const auto virtualLength=region.loopSource?std::max(sourceLength,static_cast<std::int64_t>(sourceAt(total))+16384):sourceLength;
    const bool ok=streamMappedTempoWarp(virtualLength,input.sampleRate,input.channels,total,sourceAt,cancel,read,
        [&](const float* const* data,int channels,int count) { return output->writeFromFloatArrays(data,channels,count); });
    if (!ok || cancel.load()) {output.reset();error=cancel.load()?"Warp render cancelled.":"Warp source read or cache write failed.";return {};}
    const bool flushed=output->flush();output.reset();
    if (!flushed) {error="Could not flush the warp cache.";return {};}
    auto cache=JuceDiskAudioFile::inspect(path,error);
    if (!cache) {path.deleteFile();return {};}
    auto clip=std::make_shared<SessionAudioClip>();clip->sampleRate=cache->sampleRate;clip->channels=cache->channels;clip->frames=static_cast<std::size_t>(cache->frames);
    clip->streamSource=std::make_shared<WarpCacheFile>(std::move(cache),std::move(directory));
    return clip;
}

// Worker-side onset suggestions. Linked stereo peak energy avoids antiphase
// cancellation. Suggestions keep source time and infer ticks from the current map.
inline std::vector<timeline::WarpMarker> detectWarpTransients(const SessionAudioClip& source,
    const std::vector<timeline::WarpMarker>& map, const std::atomic<bool>& cancel, juce::String& error) {
    if(map.size()<2)return {};
    std::unique_ptr<DiskAudioReader> reader;
    if(source.streamed()) {reader=source.streamSource->open();if(!reader){error="Cannot read transient source.";return {};}}
    constexpr int block=256;
    std::array<float,block> left{},right{};
    auto result=std::vector<timeline::WarpMarker>{map.front()};
    double envelope=0,last=-1;
    const auto first=std::max<std::int64_t>(0,static_cast<std::int64_t>(map.front().seconds*source.sampleRate));
    const auto end=std::min<std::int64_t>(static_cast<std::int64_t>(source.frames),static_cast<std::int64_t>(map.back().seconds*source.sampleRate));
    for(auto pos=first;pos<end;pos+=block) {
        if(cancel.load())return {};
        const int count=static_cast<int>(std::min<std::int64_t>(block,end-pos));
        if(reader) {if(!reader->read(pos,left.data(),source.channels==2?right.data():nullptr,count)){error="Transient source read failed.";return {};}}
        else for(int n=0;n<count;++n){left[n]=source.left[static_cast<std::size_t>(pos+n)];if(source.channels==2)right[n]=source.right[static_cast<std::size_t>(pos+n)];}
        double peak=0;for(int n=0;n<count;++n)peak=std::max(peak,static_cast<double>(std::max(std::abs(left[n]),source.channels==2?std::abs(right[n]):0.0f)));
        const double seconds=pos/source.sampleRate;
        if(peak>.02 && peak>envelope*3 && seconds-last>=.08 && result.size()+1<timeline::maxWarpMarkers) {
            const auto tick=static_cast<std::int64_t>(std::llround(timeline::warpTick(map,seconds)));
            if(tick>result.back().tick && tick<map.back().tick && seconds>result.back().seconds && seconds<map.back().seconds) {result.push_back({tick,seconds});last=seconds;}
        }
        envelope=.8*envelope+.2*peak;
    }
    result.push_back(map.back());return result;
}

// Render before publication/export. Mutations affect only the private playback
// snapshot: the saved document retains source markers, offsets and source files.
inline bool prepareArrangementWarps(ArrangementPlayback& song, const std::atomic<bool>& cancel, juce::String& error, std::int64_t from=0, std::int64_t to=timeline::maxLength) try {
    if(cancel.load()){error="Warp preparation cancelled.";return false;}
    for (auto& r : song.timeline.regions) {
        if (r.warp.empty()) continue;
        std::int64_t tail=0;
        for(const auto& next:song.timeline.regions)if(next.row==r.row && next.start==r.end())tail=std::max(tail,next.crossfade);
        if (r.muted || r.start>=to || r.end()+tail<=from) {r.warp.clear();r.muted=true;continue;}
        const auto* source=song.clipFor(r,static_cast<std::size_t>(r.row)).audio;
        if (!source) {error="A warped region has no available audio source.";return false;}
        // Retain the original musical origin through a trim/split. Rendering
        // from each new cut would reset WSOLA's alignment and change the sound.
        const auto prefix=std::min<std::int64_t>(timeline::maxLength,std::max<std::int64_t>(0,r.offset-r.warp.front().tick));
        auto renderRegion=r;renderRegion.start-=prefix;renderRegion.length+=prefix;renderRegion.offset-=prefix;
        renderRegion.audioOffset=std::max(0.0,timeline::warpSeconds(r.warp,static_cast<double>(renderRegion.offset)));
        auto clip=renderRegionWarp(*source,renderRegion,song.timeline,tail,cancel,error);
        if (!clip) return false;
        const auto at=std::lower_bound(song.warped.begin(),song.warped.end(),r.id,[](const auto& w,std::uint32_t id){return w.id<id;});
        song.warped.insert(at,ArrangementPlayback::WarpedRegion{r.id,std::move(clip)});
        r.audioOffset=timeline::TempoMap{song.timeline.tempoChanges,song.timeline.tempo}.secondsBetween(static_cast<double>(renderRegion.start),static_cast<double>(r.start));
        r.loopSource=false;r.reversed=false;r.warp.clear();
    }
    if(cancel.load()){error="Warp preparation cancelled.";return false;}
    return true;
} catch (...) {
    error="Could not prepare warped audio: source or cache failed.";return false;
}
} // namespace alien
