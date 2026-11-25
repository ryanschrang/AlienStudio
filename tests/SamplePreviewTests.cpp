#include "SamplePreview.h"
#include <cstdlib>
#include <iostream>

// Deterministically model a seek while the GUI request grows a slot, before it
// can publish. No test hooks or sleeps are needed in the preview implementation.
static alien::SamplePreview* cancelDuringAllocation{};
void* operator new(std::size_t size) {
    if (auto* preview = cancelDuringAllocation) { cancelDuringAllocation = nullptr; preview->cancelAligned(); }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    alien::SamplePreview preview;
    preview.prepare(48000);
    std::vector<float> audio(4800, .8f);
    const auto queue = [&](bool aligned) {
        require(preview.request(audio.data(), nullptr, 4800, 48000, aligned), "queue preview");
        preview.beginBlock();
    };
    const auto audible = [&] {
        for (int i = 0; i < 300; ++i) preview.nextSample();
        return preview.nextSample()[0] > .39f;
    };
    constexpr double step = 1.0 / 24000; // 120 BPM at 48 kHz
    queue(true);
    for (int i = 0; i < 12000; ++i) {
        // Irregular callback boundaries must not restart the wait or consume audio.
        if (i % 127 == 0) preview.beginBlock();
        require(preview.nextSample(true, .5 + i * step, step)[0] == 0 && preview.waitingForBeat(),
            "a half-beat request stays silent until the beat");
    }
    preview.nextSample(true, 1, step);
    require(!preview.waitingForBeat() && audible(), "start on the beat with the whole preview available");

    queue(true);
    preview.nextSample(true, 1.25, step);
    preview.nextSample(true, .25, step); // backwards seek or a non-beat-aligned loop wrap
    require(preview.waitingForBeat(), "a backward jump does not launch off the beat");
    preview.nextSample(true, 8.75, step); // forward seek
    require(preview.waitingForBeat(), "a forward jump does not launch off the beat");
    preview.nextSample(true, 9 + step * .4, step);
    require(!preview.waitingForBeat() && audible(), "fractional sample crossing starts within one sample of the beat");

    queue(true);
    preview.nextSample(true, .8, step);
    require(preview.nextSample(false, 0, step)[0] == 0 && !preview.waitingForBeat() && !audible(),
        "transport stop cancels a queued start");
    queue(true);
    preview.beginBlock(true);
    require(!preview.waitingForBeat() && !audible(), "panic reclaims a waiting preview");
    queue(true);
    preview.stop(); preview.beginBlock();
    require(!audible(), "browser stop cancels the pending audition");
    queue(true);
    preview.prepare(44100); preview.beginBlock();
    require(!audible(), "device preparation cancels the pending audition");
    queue(true);
    queue(false);
    require(!preview.waitingForBeat() && audible(), "a new immediate preview replaces a waiting one");
    queue(true);
    preview.nextSample(true, 0, step);
    require(!preview.waitingForBeat() && audible(), "transport origin is a beat boundary");
    preview.cancelAligned();
    require(!audible(), "a seek cancels an active aligned audition");
    queue(true); preview.cancelAligned(); preview.beginBlock();
    require(!preview.waitingForBeat() && !audible(), "a seek cancels a waiting aligned audition");
    require(preview.request(audio.data(), nullptr, 4800, 48000, true), "queue aligned work before its first block");
    preview.cancelAligned(); preview.beginBlock();
    require(!preview.waitingForBeat() && !audible(), "ready work from before the seek cannot start later");
    queue(false); preview.cancelAligned();
    require(audible(), "a seek preserves an active independent immediate audition");
    require(preview.request(audio.data(), nullptr, 4800, 48000), "queue immediate work before a seek");
    preview.cancelAligned(); preview.beginBlock();
    require(audible(), "a seek preserves a ready immediate audition");
    {
        alien::SamplePreview inflight; inflight.prepare(48000);
        cancelDuringAllocation = &inflight;
        require(inflight.request(audio.data(), nullptr, 4800, 48000, true) && !cancelDuringAllocation,
            "a seek interrupts the GUI storage allocation before publication");
        inflight.beginBlock();
        require(!inflight.waitingForBeat() && inflight.nextSample(true, 0, step)[0] == 0,
            "an aligned producer that crossed the seek cannot publish stale playback");
        require(inflight.request(audio.data(), nullptr, 4800, 48000, true), "a fresh request after the seek is accepted");
        inflight.beginBlock(); inflight.nextSample(true, 0, step);
        require(!inflight.waitingForBeat(), "the fresh epoch can align to the new beat");
        for (int i = 0; i < 300; ++i) inflight.nextSample();
        require(inflight.nextSample()[0] > .39f, "fresh post-seek playback remains audible");
    }
    std::cout << "SamplePreviewTests passed\n";
}
