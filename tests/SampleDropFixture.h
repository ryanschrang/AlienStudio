#pragma once
#include "AudioFileWrite.h"
struct SampleDropFixture {
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienSampleDrop", "", false);
    juce::File file = root.getChildFile("sample with spaces.wav");
    SampleDropFixture() {
        require(root.createDirectory().wasOk(), "create drop fixture");
        alien::SessionAudioClip clip; clip.sampleRate = 8000; clip.channels = 1; clip.frames = 8000; clip.left.assign(clip.frames, .25f); clip.buildWaveform();
        std::atomic<bool> cancel{false}; juce::String error;
        require(alien::writeClipWav(file, clip, cancel, error), "write drop fixture");
    }
    ~SampleDropFixture() { root.deleteRecursively(); }
    static void wait(alien::ProjectController& controller) {
        for (int i = 0; i < 400 && controller.isBusy(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(!controller.isBusy(), "sample drop decode completes");
    }
};
