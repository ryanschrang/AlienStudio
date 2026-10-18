#include "AudioClipImport.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static void writeU16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
}

static void writeU32(std::ostream& out, std::uint32_t value) {
    writeU16(out, static_cast<std::uint16_t>(value & 0xffff));
    writeU16(out, static_cast<std::uint16_t>(value >> 16));
}

static juce::File createTestWav() {
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto file = directory.getNonexistentChildFile("AlienStudioAudioClipTest", ".wav", false);
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    const std::array<std::int16_t, 4> samples{0, 32767, 0, -32768};
    const auto bytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    out.write("RIFF", 4); writeU32(out, 36 + bytes); out.write("WAVE", 4);
    out.write("fmt ", 4); writeU32(out, 16); writeU16(out, 1); writeU16(out, 1);
    writeU32(out, 8000); writeU32(out, 16000); writeU16(out, 2); writeU16(out, 16);
    out.write("data", 4); writeU32(out, bytes);
    for (const auto sample : samples) writeU16(out, static_cast<std::uint16_t>(sample));
    out.close();
    return file;
}

int main() {
    const auto wav = createTestWav();
    const auto decoded = alien::decodeSessionAudioClip(wav);
    require(decoded.clip != nullptr && decoded.error.isEmpty(), "valid PCM WAV imports successfully");
    require(decoded.clip->valid() && decoded.clip->channels == 1 && decoded.clip->frames == 4
        && decoded.clip->waveformCount == 4, "WAV import publishes validated samples and waveform peaks");
    require(decoded.clip->left[1] > 0.99f && decoded.clip->left[3] < -0.99f,
        "WAV decoder preserves normalized sample values");

    const auto unsupported = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("AlienStudioUnsupportedAudio.xyz");
    require(!alien::decodeSessionAudioClip(unsupported).clip,
        "unsupported file extensions are rejected before decoder lookup");
    wav.deleteFile();
    std::cout << "Background-import WAV decoding and format validation passed\n";
}
