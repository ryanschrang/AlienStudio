#include "SessionProject.h"
#include "PortableProject.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdlib>
#include <iostream>
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    require(argc == 2, "examples directory argument");
    const juce::File root{juce::String(argv[1])};
    for (const auto* relative : {"production/Production.alienstudio", "live/LivePerformance.alienstudio", "recording/MultitrackReference.alienstudio"}) {
        const auto file = root.getChildFile(relative);
        alien::SessionProjectData project;
        juce::String error;
        if (!alien::parseSessionProject(file.loadFileAsString(), project, error)) {
            std::cerr << relative << ": " << error << '\n'; return 1;
        }
        require(project.valid() && project.trackCount() == 4 && project.sceneCount() == 4, "representative project grid");
        alien::SessionProjectData reopened;
        require(alien::parseSessionProject(alien::serializeSessionProject(project), reopened, error), "fixture survives save/reopen");
        if (juce::String(relative).startsWith("recording")) {
            require(project.arrangement.count == 4, "four reference tracks have arrangement regions");
            for (int t = 0; t < 4; ++t) {
                const auto source = alien::resolveProjectAudio(file, project.clips[0][t].audioFilePath);
                require(source.existsAsFile(), "all recording reference media resolve");
                juce::WavAudioFormat format;
                std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(source.createInputStream().release(), true));
                require(reader && reader->numChannels == 1 && reader->sampleRate == 8000 && reader->lengthInSamples == 32000, "real four-second reference audio");
            }
        } else {
            std::size_t count = 0;
            for (const auto& scene : project.clips) for (const auto& clip : scene) {
                require(clip.audioFilePath.isEmpty(), "production/live fixtures require no external media or plugins");
                count += clip.pattern.count;
            }
            require(count > 0, "musical fixture contains playable notes");
        }
    }
    std::cout << "Representative projects parse, round-trip and resolve reference audio\n";
}
