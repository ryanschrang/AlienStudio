#include "SessionAudioClip.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static alien::SessionAudioClip monoClip() {
    alien::SessionAudioClip clip;
    clip.sampleRate = 8000;
    clip.channels = 1;
    clip.frames = 4;
    clip.left = {0.0f, 1.0f, 0.0f, -1.0f};
    clip.buildWaveform();
    return clip;
}

static void clipModelTests() {
    auto clip = monoClip();
    require(clip.valid(), "decoded mono clip metadata and sample storage validate");
    require(clip.waveformCount == 4 && clip.waveform[1].leftMax == 1.0f
        && clip.waveform[3].rightMin == -1.0f, "waveform peaks represent mono samples on both lanes");
    clip.channels = 3;
    require(!clip.valid(), "clips with more than two channels are rejected");
    clip = monoClip();
    clip.sampleRate = 1000;
    require(!clip.valid(), "unsupported sample rates are rejected");
}

static void playerTests() {
    auto clip = monoClip();
    alien::SessionAudioPlayer player;
    require(player.queue(&clip, 4) && player.hasPending()
        && player.pendingQuantizationSteps() == 4, "audio clip queues with the selected beat quantization");
    require(!player.isPlaying(), "queued clip remains silent before its launch boundary");
    require(player.launchPendingNow(8000) && player.isPlaying(), "launch activates decoded clip without copying it");
    const std::array<float, 4> expected{0.0f, 1.0f, 0.0f, -1.0f};
    for (float value : expected) {
        const auto sample = player.nextSample();
        require(std::abs(sample[0] - value) < 1.0e-6f && sample[0] == sample[1],
            "mono playback emits the decoded samples equally to left and right");
    }
    require(!player.isPlaying() && player.nextSample()[0] == 0.0f,
        "one-shot clip ends in silence without a stuck playback gate");

    alien::SessionAudioClip stereo;
    stereo.sampleRate = 8000;
    stereo.channels = 2;
    stereo.frames = 2;
    stereo.left = {0.25f, 0.5f};
    stereo.right = {-0.25f, -0.5f};
    stereo.buildWaveform();
    require(stereo.valid(), "decoded stereo clip validates");
    player.queue(&stereo, 16);
    player.launchPendingNow(16000);
    const auto first = player.nextSample();
    const auto second = player.nextSample();
    require(std::abs(first[0] - 0.25f) < 1.0e-6f && std::abs(first[1] + 0.25f) < 1.0e-6f
        && std::abs(second[0] - 0.375f) < 1.0e-6f && std::abs(second[1] + 0.375f) < 1.0e-6f,
        "stereo playback preserves channels and linearly resamples to the device rate");
    player.stop();
    require(!player.isPlaying() && player.nextSample()[0] == 0.0f, "stop clears active playback immediately");
}

static void mailboxTests() {
    alien::SessionClipMailbox mailbox;
    alien::SessionClipLaunch request;
    request.pattern.add(60, 0, 1);
    request.clipIndex = 3;
    require(mailbox.publish(request), "session mailbox accepts a MIDI clip launch");
    alien::SessionClipLaunch received;
    require(mailbox.consume(received) && received.clipIndex == 3 && received.pattern.count == 1
        && received.audio == nullptr, "audio-thread mailbox transfers MIDI launch without changing clip data");

    auto clip = monoClip();
    request = {};
    request.audio = &clip;
    request.clipIndex = 2;
    require(mailbox.publish(request) && mailbox.consume(received)
        && received.audio == &clip && received.clipIndex == 2,
        "session mailbox transfers an immutable audio pointer without copying decoded samples");
    std::cout << "Session audio clip validation, waveform, resampling, playback and mailbox passed\n";
}

int main() {
    clipModelTests();
    playerTests();
    mailboxTests();
}
