// The Starter Pack that ships next to the executable: every file is accounted for in its
// manifest under a license we may redistribute, every sample decodes as a usable one-shot,
// every MIDI clip parses and ends every note it starts, and the app lists the drums in order.
#include "StarterPack.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

static void require(bool ok, const juce::String& message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static const juce::File pack{juce::String(ALIEN_STARTER_PACK_DIR)};

// file -> license, from manifest.csv ("file,source,license,origin"; names hold no commas).
static std::map<juce::String, juce::String> manifest() {
    std::map<juce::String, juce::String> rows;
    juce::StringArray lines; lines.addLines(pack.getChildFile("manifest.csv").loadFileAsString());
    require(lines[0] == "file,source,license,origin", "the manifest has its header");
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty()) continue;
        const auto fields = juce::StringArray::fromTokens(lines[i], ",", "");
        require(fields.size() >= 4, "manifest row has every field: " + lines[i]);
        require(rows.emplace(fields[0], fields[2]).second, "manifest lists a file once: " + fields[0]);
    }
    return rows;
}

static void everyFileIsLicensed() {
    const auto rows = manifest();
    const std::set<juce::String> allowed{"CC0-1.0", "MIT", "CC-BY-4.0"};
    std::set<juce::String> content;
    for (const auto& entry : juce::RangedDirectoryIterator(pack, true, "*", juce::File::findFiles)) {
        const auto path = entry.getFile().getRelativePathFrom(pack).replaceCharacter('\\', '/');
        if (path.endsWithIgnoreCase(".wav") || path.endsWithIgnoreCase(".mid")) content.insert(path);
    }
    require(content.size() == rows.size(), "the manifest lists exactly the pack's samples and clips");
    for (const auto& [file, license] : rows) {
        require(content.count(file) == 1, "listed file exists: " + file);
        require(allowed.count(license) == 1, "redistributable license for " + file + ": " + license);
    }
    for (const auto* name : {"README.md", "LICENSES/CC0-1.0.txt", "LICENSES/MIT-free-midi-chords.txt", "LICENSES/CC-BY-4.0.txt"})
        require(pack.getChildFile(name).existsAsFile(), juce::String("the pack carries ") + name);
    std::puts("PASS: every Starter Pack file is in the manifest under a redistributable license");
}

static void samplesDecode() {
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    juce::int64 bytes = 0;
    const auto samples = alien::starterPack::drumSamples(pack);
    require(samples.size() >= 30, "the pack has a full set of drum sounds");
    for (const auto& sample : samples) {
        const auto name = sample.file.getFileName();
        bytes += sample.file.getSize();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sample.file));
        require(reader != nullptr, "decodes: " + name);
        require(reader->numChannels == 2 && (reader->sampleRate == 44100.0 || reader->sampleRate == 48000.0), "stereo at 44.1 or 48 kHz: " + name);
        const double seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        require(seconds > 0.05 && seconds <= 4.01, "a one-shot of at most four seconds: " + name);
        juce::AudioBuffer<float> audio(2, static_cast<int>(reader->lengthInSamples));
        require(reader->read(&audio, 0, audio.getNumSamples(), 0, true, true), "reads: " + name);
        const float peak = audio.getMagnitude(0, audio.getNumSamples());
        require(peak > 0.5f && peak < 0.95f, "normalized below full scale: " + name);
        const int head = static_cast<int>(reader->sampleRate * 0.03);
        require(audio.getMagnitude(0, std::min(head, audio.getNumSamples())) > 0.05f, "the hit starts at once: " + name);
        const int tail = std::min(64, audio.getNumSamples());
        require(audio.getMagnitude(audio.getNumSamples() - tail, tail) < 0.01f, "it fades out without a click: " + name);
    }
    require(bytes < 16 * 1024 * 1024, "the samples stay small enough to ship in the repository");
    std::puts("PASS: every Starter Pack sample decodes as a stereo one-shot that starts at once and ends quietly");
}

static void clipsParse() {
    int clips = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(pack.getChildFile("MIDI"), true, "*.mid", juce::File::findFiles)) {
        const auto file = entry.getFile(); const auto name = file.getFileName();
        juce::FileInputStream in(file); juce::MidiFile midi;
        require(in.openedOk() && midi.readFrom(in), "parses: " + name);
        const double ppq = midi.getTimeFormat();
        require(ppq > 0, "timed in ticks per quarter note: " + name);
        std::map<int, int> held; int notes = 0; double last = 0;
        for (int t = 0; t < midi.getNumTracks(); ++t) {
            juce::MidiMessageSequence sequence(*midi.getTrack(t)); sequence.sort();
            for (const auto* event : sequence) {
                const auto& m = event->message;
                const int key = m.getChannel() * 128 + m.getNoteNumber();
                if (m.isNoteOn()) { ++held[key]; ++notes; }
                else if (m.isNoteOff()) { require(held[key] > 0, "every note-off ends a note: " + name); --held[key]; }
                if (m.isNoteOnOrOff()) last = std::max(last, m.getTimeStamp());
            }
        }
        require(notes > 0, "has notes: " + name);
        for (const auto& [key, count] : held) require(count == 0, "no note is left hanging: " + name);
        if (file.getParentDirectory().getFileName() == "Drum Grooves")
            require(last <= ppq * 16, "a groove is four bars long: " + name);
        ++clips;
    }
    require(clips >= 30, "the pack has chord progressions and drum grooves");
    std::puts("PASS: every Starter Pack MIDI clip parses and ends every note it starts");
}

static void drumListing() {
    const auto samples = alien::starterPack::drumSamples(pack);
    std::set<juce::String> categories;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        categories.insert(samples[i].category);
        if (i > 0 && samples[i].category == samples[i - 1].category)
            require(samples[i - 1].file.getFileName().compareNatural(samples[i].file.getFileName()) < 0, "sorted by name within a category");
    }
    for (const auto* category : {"Kicks", "Snares", "Hats", "Claps", "Toms", "Cymbals", "Percussion"})
        require(categories.count(category) == 1, juce::String("has ") + category);
    require(alien::starterPack::drumSamples(pack.getChildFile("missing")).empty(), "a missing pack lists nothing");
    require(alien::starterPack::folder().getFileName() == "Starter Pack"
            && alien::starterPack::folder().getParentDirectory() == juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory(),
            "the app looks for the pack next to its executable");
    std::puts("PASS: the app lists the Starter Pack's drums by category and name");
}

int main() {
    require(pack.isDirectory(), "the source Starter Pack folder exists");
    everyFileIsLicensed();
    samplesDecode();
    clipsParse();
    drumListing();
}
