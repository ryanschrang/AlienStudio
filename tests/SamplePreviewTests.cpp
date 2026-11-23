#include "SamplePreview.h"
#include <cstdlib>
#include <iostream>

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
    std::cout << "SamplePreviewTests passed\n";
}
