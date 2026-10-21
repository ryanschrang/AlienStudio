#include "WaveformCache.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>

static void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
static void u16(std::ostream& o,std::uint16_t v){o.put(static_cast<char>(v));o.put(static_cast<char>(v>>8));}
static void u32(std::ostream& o,std::uint32_t v){u16(o,static_cast<std::uint16_t>(v));u16(o,static_cast<std::uint16_t>(v>>16));}
static juce::File wav(const juce::File& file){
    std::ofstream out(file.getFullPathName().toStdString(),std::ios::binary);
    constexpr int frames=20000,channels=2;const std::uint32_t bytes=frames*channels*2;
    out.write("RIFF",4);u32(out,36+bytes);out.write("WAVE",4);
    out.write("fmt ",4);u32(out,16);u16(out,1);u16(out,channels);u32(out,48000);
    u32(out,48000*channels*2);u16(out,channels*2);u16(out,16);
    out.write("data",4);u32(out,bytes);
    for(int i=0;i<frames;++i){const auto a=static_cast<std::int16_t>((i%200)-100);u16(out,static_cast<std::uint16_t>(a));u16(out,static_cast<std::uint16_t>(-a));}
    return file;
}
int main(){
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienWaveformCacheTest","",false);
    require(root.createDirectory().wasOk(),"create test directory");
    const auto source=wav(root.getChildFile("long.wav"));
    const auto project=root.getChildFile("Song.alienstudio");
    const auto cache=alien::LongWaveformCache::fileFor(project,source);
    require(cache.getParentDirectory()==project.getSiblingFile("Song.alienstudio.cache").getChildFile("waveforms"),
        "saved projects keep waveform cache beside the project");
    const auto first=alien::LongWaveformCache::loadOrCreate(source,cache);
    require(first.data&&first.error.isEmpty()&&!first.cacheHit&&cache.existsAsFile(),"first request generates and persists peaks");
    require(first.data->channels==2&&first.data->frames==20000&&first.data->peaks.size()==20000,
        "metadata and detailed stereo peaks are preserved");
    const auto second=alien::LongWaveformCache::loadOrCreate(source,cache);
    require(second.data&&second.cacheHit,"unchanged source reuses cache");
    {std::ofstream append(source.getFullPathName().toStdString(),std::ios::binary|std::ios::app);append.put(0);}
    require(!alien::LongWaveformCache::load(source,cache).data,"source size change invalidates cached peaks");
    bool cancel=true;
    const auto cancelled=alien::LongWaveformCache::generate(source,[&]{return cancel;});
    require(!cancelled.data&&cancelled.error.containsIgnoreCase("cancel"),"obsolete background work cancels between bounded reads");
    root.deleteRecursively();
}
