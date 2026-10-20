#include "DiskAudioFile.h"
#include "DiskRecording.h"
#include <iostream>
#include <stdexcept>
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("AlienDiskReaderTests", "");
    require(root.createDirectory().wasOk(), "create test directory");
    const auto file = root.getChildFile("Stereo.wav");
    juce::String error;
    auto writer = alien::RecordingFile::create(file, 48000, 2, error, root.getChildFile("Recovery"));
    require(bool(writer), "prepare real WAV");
    std::array<float, 4096> left{}, right{};
    for (int n = 0; n < 4096; ++n) { left[n] = .2f * std::sin(n*.07f); right[n] = -left[n]; }
    const float* inputs[]{left.data(), right.data()};
    require(writer->write(inputs, 2, 4096) && writer->write(inputs, 2, 4096), "write stereo source");
    require(writer->finish(false).file.existsAsFile(), "publish WAV");
    writer.reset();
    auto source = alien::JuceDiskAudioFile::inspect(file, error);
    require(source && error.isEmpty() && source->frames == 8192 && source->channels == 2, "inspect metadata without whole-file decode");
    auto first = source->open(), second = source->open();
    require(first && second, "independent real decoders");
    std::array<float, 16> a{}, b{}, c{}, d{};
    require(first->read(123, a.data(), b.data(), 16)
        && second->read(4106, c.data(), d.data(), 16), "independent source positions");
    for (int n = 0; n < 16; ++n) {
        require(std::abs(a[n]-left[123+n]) < .00001f && std::abs(b[n]+a[n]) < .00001f, "PCM stereo read");
        require(std::abs(c[n]-left[10+n]) < .00001f && std::abs(d[n]+c[n]) < .00001f, "second reader starts elsewhere");
    }
    require(!first->read(8190, a.data(), b.data(), 16)
        && !first->read(-1, a.data(), b.data(), 1)
        && !first->read(0, a.data(), nullptr, 1), "invalid reads rejected");
    first.reset(); second.reset();
    require(file.deleteFile(), "remove test source after decoder close");
    require(!source->open(), "removed source fails reopening");
    require(!alien::JuceDiskAudioFile::inspect(file, error) && error.isNotEmpty(), "missing source has useful error");
    const auto fake = root.getChildFile("damaged.mp3");
    require(fake.replaceWithText("not an mp3"), "make malformed source");
    require(!alien::JuceDiskAudioFile::inspect(fake, error) && error.isNotEmpty(), "malformed MP3 rejected");
    source.reset();
    require(root.deleteRecursively(), "clean test directory");
    std::cout << "Real WAV metadata, independent readers, stereo, bounds and missing media passed\n";
}
