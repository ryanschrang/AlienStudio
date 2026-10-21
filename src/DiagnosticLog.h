#pragma once
// A plain-text log the user can open and attach to a bug report. One line per event, local time,
// oldest lines dropped by rotating to "<name>.1" once the file passes its size limit. Safe to call
// from any thread except the audio thread (it takes a lock and writes a file). An empty File
// means "log nowhere", which is what the unit tests use unless they are testing the log itself.
#include <juce_core/juce_core.h>

namespace alien {
class DiagnosticLog {
public:
    static constexpr juce::int64 defaultLimitBytes = 1024 * 1024;
    explicit DiagnosticLog(juce::File logFile = {}, juce::int64 limitBytes = defaultLimitBytes)
        : file(std::move(logFile)), limit(limitBytes) {}
    // Copies share the file, not the lock; each copy serialises its own writes, which is enough for
    // one process appending whole lines.
    DiagnosticLog(const DiagnosticLog& other) : file(other.file), limit(other.limit) {}
    DiagnosticLog(DiagnosticLog&& other) noexcept : file(std::move(other.file)), limit(other.limit) {}
    DiagnosticLog& operator=(const DiagnosticLog& other) { file = other.file; limit = other.limit; return *this; }
    DiagnosticLog& operator=(DiagnosticLog&& other) noexcept { file = std::move(other.file); limit = other.limit; return *this; }

    // %APPDATA%\Alien Studio\Logs on Windows, the platform equivalent elsewhere.
    static juce::File directory() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio").getChildFile("Logs");
    }
    static juce::File defaultFile(const juce::String& name) { return directory().getChildFile(name); }

    const juce::File& path() const noexcept { return file; }
    bool enabled() const noexcept { return file != juce::File{}; }

    // level is a short word such as "info", "warn", "error", "fault".
    void write(const juce::String& level, const juce::String& message) const {
        if (!enabled()) return;
        const juce::ScopedLock lock(mutex);
        if (!file.getParentDirectory().createDirectory()) return;
        if (limit > 0 && file.existsAsFile() && file.getSize() > limit) {
            const auto older = file.withFileExtension(file.getFileExtension() + ".1");
            older.deleteFile();
            file.moveFileTo(older);
        }
        // One physical line per event, so grep and the log viewer both work.
        const auto flat = message.replace("\r\n", " | ").replace("\n", " | ").replace("\r", " | ");
        file.appendText(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") + "  " + level.paddedRight(' ', 5) + "  " + flat + "\n", false, false, "\n");
    }
    // The whole log, for tests and for the "copy log" command.
    juce::String read() const { return enabled() && file.existsAsFile() ? file.loadFileAsString() : juce::String(); }

private:
    juce::File file;
    juce::int64 limit;
    mutable juce::CriticalSection mutex;
};
} // namespace alien
