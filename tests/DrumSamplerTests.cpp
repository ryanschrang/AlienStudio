#include "DrumSampler.h"
#include "DrumTransientSlices.h"
#include <iostream>
#include <cstdlib>
#include <new>
static std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void check(bool b, const char* m) { if (!b) { std::cerr << m << '\n'; std::exit(1); } }
static void sliceTests() {
    auto source = std::make_shared<alien::SessionAudioClip>();
    source->sampleRate = 8000; source->channels = 2; source->frames = 83;
    for (int i = 0; i < 83; ++i) { source->left.push_back((i + 1) / 100.f); source->right.push_back(-(i + 1) / 100.f); }
    source->buildWaveform();
    check(alien::drumSliceView(source, {}) == source, "whole sample keeps identity");
    check(!alien::drumSliceView(source, {0, 84}) && !alien::drumSliceView(source, {2, 1}) && !alien::drumSliceView(source, {-1, 2}), "invalid slice ranges refused");
    std::array<std::shared_ptr<const alien::SessionAudioClip>, 8> slices;
    for (int p = 0; p < 8; ++p) {
        const int begin = 83 * p / 8, end = 83 * (p + 1) / 8;
        slices[p] = alien::drumSliceView(source, {begin, end});
        check(slices[p] && slices[p]->valid() && slices[p]->sliceSource == source && slices[p]->decodedBytes() == 0, "slices share PCM without decoding or copying");
        check(!alien::drumSliceView(slices[p], {0, 1}), "nested slice views refused");
        alien::DrumSampler sampler; sampler.prepare(16000); sampler.setSample(p, slices[p].get());
        const auto before = allocations;
        sampler.noteOn(36 + p, 1);
        for (int i = 0; i < (end - begin) * 2; ++i) {
            const double cursor = begin + i * .5;
            const auto a = static_cast<int>(cursor), b = std::min(a + 1, end - 1);
            const float expected = (source->left[a] + (source->left[b] - source->left[a]) * static_cast<float>(cursor - a)) * .25f;
            const auto frame = sampler.nextSample();
            check(std::abs(frame[0] - expected) < 1e-6 && std::abs(frame[1] + expected) < 1e-6, "slice interpolation stays within exclusive end in stereo");
        }
        check(sampler.nextSample()[0] == 0, "each slice stops at its own end");
        sampler.noteOn(36 + p, 1); sampler.noteOn(36 + p, 1);
        const auto expected = source->left[begin] * ((p == 2 || p == 3) ? .25f : .5f);
        check(std::abs(sampler.nextSample()[0] - expected) < 1e-6, "slice retrigger preserves polyphony and hat choke");
        sampler.setSample(p, source.get()); check(sampler.nextSample()[0] == 0, "replacing slice stops old voices");
        check(allocations == before, "slice playback is allocation free");
    }
}
static void transientTests() {
    for (double rate : {8000., 44100., 48000., 96000.}) {
        alien::SessionAudioClip clip; clip.sampleRate = rate; clip.channels = 2;
        clip.frames = static_cast<std::size_t>(rate); clip.left.resize(clip.frames); clip.right.resize(clip.frames);
        for (int hit = 0; hit < 4; ++hit) {
            const auto start = static_cast<std::size_t>(rate * (.05 + hit * .2));
            for (std::size_t f = 0; f < static_cast<std::size_t>(rate * .04); ++f) {
                const auto amplitude = static_cast<float>(std::exp(-static_cast<double>(f) / (rate * .004)));
                clip.left[start + f] = amplitude; clip.right[start + f] = -amplitude;
            }
        }
        clip.buildWaveform(); const auto result = alien::detectDrumTransientSlices(clip);
        check(result.count == 4, "four separated attacks yield four slices at supported sample rates");
        check(result.ranges[0].start == 0 && result.ranges[3].end == static_cast<int>(clip.frames), "auto slices retain lead-in and tail");
        for (int p = 0; p < 3; ++p) {
            check(result.ranges[p].end == result.ranges[p + 1].start, "auto slices partition without gaps");
            check(std::abs(result.ranges[p].end - rate * (.25 + p * .2)) < rate * .006, "boundaries are near the attack, not equal divisions");
        }
        check(alien::detectDrumTransientSlices(clip).ranges == result.ranges, "detection is deterministic");
        clip.channels = 1; clip.right.clear(); check(alien::detectDrumTransientSlices(clip).count == 4, "mono attacks detected");
        std::fill(clip.left.begin(), clip.left.end(), .1f); clip.buildWaveform();
        check(alien::detectDrumTransientSlices(clip).count == 0, "steady sample refuses instead of inventing slices");
        std::fill(clip.left.begin(), clip.left.end(), 0.f); clip.buildWaveform();
        check(alien::detectDrumTransientSlices(clip).count == 0, "silence refuses");
        clip.left[static_cast<std::size_t>(rate * .1)] = 1;
        check(alien::detectDrumTransientSlices(clip).count == 0, "single attack leaves kit unchanged");
        clip.left[static_cast<std::size_t>(rate * .11)] = .5f;
        check(alien::detectDrumTransientSlices(clip).count == 0, "attacks within thirty milliseconds do not create tiny slices");
        clip.left[0] = std::numeric_limits<float>::quiet_NaN();
        check(alien::detectDrumTransientSlices(clip).count == 0, "nonfinite source refuses");
    }
    auto source = std::make_shared<alien::SessionAudioClip>(); source->sampleRate = 8000; source->channels = 1;
    source->frames = 16000; source->left.resize(source->frames);
    for (int hit = 0; hit < 12; ++hit) source->left[400 + hit * 1200] = .2f + hit * .05f;
    source->buildWaveform(); auto result = alien::detectDrumTransientSlices(*source);
    check(result.count == 8 && result.ranges[0].end == 6400, "over eight attacks keeps first and strongest seven boundaries");
    auto bounds = alien::detectAttackBounds(*source, 16);
    check(bounds.size() == 13 && bounds.front() == 0 && bounds[1] == 1600 && bounds[11] == 13600 && bounds.back() == 16000,
          "with room for sixteen, every attack after the first is a boundary");
    bounds = alien::detectAttackBounds(*source, 3);
    check(bounds == std::vector<std::int64_t>{0, 12400, 13600, 16000}, "fewer slices keep the strongest boundaries, in time order");
    check(alien::detectAttackBounds(*source, 1).empty(), "one slice is no slicing");
    const auto view = alien::drumSliceView(source, {2800, 7600});
    result = alien::detectDrumTransientSlices(*view);
    check(result.count == 4 && result.ranges[0].start == 0 && result.ranges[3].end == 4800, "view detection uses local selected range only");
    source->frames = 480001; source->left.resize(source->frames); source->buildWaveform();
    check(alien::detectDrumTransientSlices(*source).count == 0, "over-minute source refuses without truncation");
}
int main() {
    transientTests();
    sliceTests();
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
