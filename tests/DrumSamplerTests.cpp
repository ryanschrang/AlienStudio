#include "DrumSampler.h"
#include <iostream>
#include <cstdlib>
#include <new>
static std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void check(bool b, const char* m) { if (!b) { std::cerr << m << '\n'; std::exit(1); } }
int main() {
    alien::SessionAudioClip clip;
    clip.sampleRate=24000; clip.frames=4; clip.channels=1; clip.left={1,0.5f,0.25f,0}; clip.buildWaveform();
    alien::DrumSampler sampler; sampler.prepare(48000); sampler.setSample(0,&clip);
    const auto before=allocations;
    sampler.noteOn(36,0.5f);
    check(std::abs(sampler.nextSample()[0]-0.125f)<1e-6f,"velocity gain");
    check(std::abs(sampler.nextSample()[0]-0.09375f)<1e-6f,"sample rate interpolation");
    for(int i=0;i<20;++i) sampler.nextSample();
    check(sampler.nextSample()[0]==0,"one shot ends");
    sampler.noteOn(35,1); sampler.noteOn(44,1); check(sampler.nextSample()[0]==0,"unmapped notes silent");
    sampler.noteOn(36,1); sampler.noteOn(36,1); check(sampler.nextSample()[0]==0.5f,"retrigger polyphony");
    sampler.stop(); check(sampler.nextSample()[0]==0,"panic silence");
    sampler.setSample(2,&clip); sampler.setSample(3,&clip);
    sampler.noteOn(38,1); sampler.noteOn(39,1); check(sampler.nextSample()[0]==0.25f,"hat choke pair");
    sampler.stop(); sampler.noteOn(36,1,0); sampler.noteOn(36,1,9); sampler.stop(9);
    check(sampler.nextSample()[0]==0.25f,"MIDI reset preserves sequenced drums");
    sampler.setSample(0,nullptr); check(sampler.nextSample()[0]==0,"pad removal clears old sample voices");
    sampler.setSample(0,&clip); for(int i=0;i<40;++i) sampler.noteOn(36,1);
    check(sampler.nextSample()[0]==4.0f,"bounded 16 voice stealing");
    sampler.prepare(44100); check(sampler.nextSample()[0]==0,"device restart silence");
    check(allocations==before,"sampler DSP allocated");
    std::cout<<"Drum sampler timing, velocity, choke, lifecycle and allocation passed\n";
}
