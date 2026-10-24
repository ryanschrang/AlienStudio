#pragma once
// Finding VST3 plugins on disk and testing each one in a child process.
//
//   discoverVst3Bundles(folders)   recursive search that understands bundle layouts
//   inspectBundle(bundle)          which binary is inside, and is it one this 64-bit host can load
//   scanBundleInChild(...)         the child side of "--scan-vst3" (stage markers, exit codes)
//   classify(...)                  what a finished (or dead, or hung) child means
//   runScan(...)                   the whole scan: skip what is unchanged, scan the rest, keep a log
//
// A plugin that crashes, hangs or refuses to load while it is being scanned costs one line in the
// log and one entry in the scan database. It never takes the application down: every bundle is
// loaded by a separate process with a time limit.
#include "DiagnosticLog.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace alien {
inline void addHostingFormats(juce::AudioPluginFormatManager& manager) {
   #if JUCE_PLUGINHOST_VST3
    manager.addFormat(new juce::VST3PluginFormat());
   #endif
    juce::ignoreUnused(manager);
}
} // namespace alien

namespace alien::scan {

// ---------------------------------------------------------------------------------------------
// Where plugins live
// ---------------------------------------------------------------------------------------------
inline std::vector<juce::File> standardVst3Folders() {
    std::vector<juce::File> folders;
   #if JUCE_WINDOWS
    // The VST3 specification's locations: system-wide (64-bit) and per-user.
    const auto common = juce::SystemStats::getEnvironmentVariable("CommonProgramFiles", "C:\\Program Files\\Common Files");
    folders.push_back(juce::File(common).getChildFile("VST3"));
    const auto local = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    if (local.isNotEmpty()) folders.push_back(juce::File(local).getChildFile("Programs").getChildFile("Common").getChildFile("VST3"));
   #elif JUCE_MAC
    folders.push_back(juce::File("/Library/Audio/Plug-Ins/VST3"));
    folders.push_back(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3"));
   #else
    folders.push_back(juce::File("/usr/lib/vst3")); folders.push_back(juce::File("/usr/local/lib/vst3"));
    folders.push_back(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst3"));
   #endif
    return folders;
}

struct Discovery {
    std::vector<juce::File> bundles;
    std::vector<juce::String> problems;     // folders that could not be read, depth limit hit, missing roots
    int foldersVisited{};
};

// Recursively finds VST3 plugins under `roots`.
//  * A "Foo.vst3" folder is one plugin (a bundle); what is inside it is never listed separately,
//    although the real library inside is itself named "Foo.vst3".
//  * A "Foo.vst3" file is one plugin (the older single-file layout).
//  * A root that is itself a plugin is returned as that plugin.
//  * Symbolic links to folders are not followed and each folder is visited once, so a loop cannot
//    trap the search; `maxDepth` bounds the rest. A symbolic link to a plugin bundle is listed.
//  * Unreadable or missing folders are noted in `problems`, not fatal.
//  * The result is sorted and has no duplicates, even when roots overlap.
inline Discovery discoverVst3Bundles(const std::vector<juce::File>& roots, int maxDepth = 12) {
    Discovery found; std::set<juce::String> seenFolders, seenBundles;
    const auto isPluginName = [](const juce::File& f) { return f.hasFileExtension("vst3"); };
    const auto addBundle = [&](const juce::File& f) { if (seenBundles.insert(f.getFullPathName().toLowerCase()).second) found.bundles.push_back(f); };
    std::function<void(const juce::File&, int)> visit = [&](const juce::File& folder, int depth) {
        if (!seenFolders.insert(folder.getFullPathName().toLowerCase()).second) return;
        ++found.foldersVisited;
        if (!folder.hasReadAccess()) { found.problems.push_back("Cannot read folder " + folder.getFullPathName()); return; }
        if (depth >= maxDepth) { found.problems.push_back("Stopped searching below " + folder.getFullPathName() + " (more than " + juce::String(maxDepth) + " levels deep)"); return; }
        for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFilesAndDirectories, juce::File::FollowSymlinks::no)) {
            const auto file = entry.getFile();
            if (isPluginName(file)) { addBundle(file); continue; }
            if (entry.isDirectory() && !file.isSymbolicLink()) visit(file, depth + 1);
        }
    };
    for (const auto& root : roots) {
        if (!root.exists()) { found.problems.push_back("Folder not found: " + root.getFullPathName()); continue; }
        if (isPluginName(root)) { addBundle(root); continue; }
        if (!root.isDirectory()) { found.problems.push_back("Not a folder: " + root.getFullPathName()); continue; }
        visit(root, 0);
    }
    std::sort(found.bundles.begin(), found.bundles.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0; });
    return found;
}

// ---------------------------------------------------------------------------------------------
// What is inside a bundle
// ---------------------------------------------------------------------------------------------
enum class Platform { Windows, Mac, Linux };  // not "linux": GCC defines that as a macro
inline constexpr Platform currentPlatform() {
   #if JUCE_WINDOWS
    return Platform::Windows;
   #elif JUCE_MAC
    return Platform::Mac;
   #else
    return Platform::Linux;
   #endif
}
enum class Arch { unknown, x64, x86, arm64, other };

// Machine type of a Windows executable or DLL, read from its header. Unknown for anything else.
inline Arch peArchitecture(const juce::File& file) {
    juce::FileInputStream in(file);
    if (!in.openedOk()) return Arch::unknown;
    juce::uint8 head[64]{};
    if (in.read(head, 64) < 64 || head[0] != 'M' || head[1] != 'Z') return Arch::unknown;
    const auto peOffset = static_cast<juce::int64>(head[0x3c]) | (static_cast<juce::int64>(head[0x3d]) << 8) | (static_cast<juce::int64>(head[0x3e]) << 16) | (static_cast<juce::int64>(head[0x3f]) << 24);
    if (peOffset < 64 || peOffset > 1024 * 1024 || !in.setPosition(peOffset)) return Arch::unknown;
    juce::uint8 pe[6]{};
    if (in.read(pe, 6) < 6 || pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return Arch::unknown;
    switch (pe[4] | (pe[5] << 8)) {
        case 0x8664: return Arch::x64;
        case 0x014c: return Arch::x86;
        case 0xaa64: return Arch::arm64;
        default: return Arch::other;
    }
}

struct BundleInfo {
    juce::File binary;          // the file that is actually loaded (the bundle itself for single-file plugins)
    juce::String problem;       // non-empty: this bundle cannot work here, and why
};
inline BundleInfo inspectBundle(const juce::File& bundle, Platform target = currentPlatform()) {
    BundleInfo info;
    if (!bundle.exists()) { info.problem = "The file no longer exists."; return info; }
    if (bundle.isDirectory()) {
        const auto contents = bundle.getChildFile("Contents");
        if (!contents.isDirectory()) { info.problem = "Not a valid VST3 bundle: it has no Contents folder."; return info; }
        const char* wanted = target == Platform::Windows ? "x86_64-win" : target == Platform::Mac ? "MacOS" : "x86_64-linux";
        const auto archFolder = contents.getChildFile(wanted);
        if (archFolder.isDirectory()) {
            const char* extension = target == Platform::Windows ? "*.vst3" : target == Platform::Mac ? "*" : "*.so";
            auto candidates = archFolder.findChildFiles(juce::File::findFiles, false, extension);
            if (!candidates.isEmpty()) {
                candidates.sort();
                // Prefer the library named like the bundle; some bundles carry helper libraries too.
                info.binary = candidates.getFirst();
                for (const auto& c : candidates) if (c.getFileNameWithoutExtension() == bundle.getFileNameWithoutExtension()) { info.binary = c; break; }
                return info;
            }
            info.problem = juce::String("The ") + wanted + " folder inside the bundle contains no plugin library.";
            return info;
        }
        juce::StringArray others;
        for (const auto& d : contents.findChildFiles(juce::File::findDirectories, false)) others.add(d.getFileName());
        if (target == Platform::Windows && others.contains("x86-win")) info.problem = "This plugin only contains a 32-bit build (x86-win). Alien Studio is 64-bit and needs the 64-bit version.";
        else if (target == Platform::Windows && (others.contains("arm64-win") || others.contains("arm64ec-win"))) info.problem = "This plugin only contains an ARM build; it has no x86_64-win build for this version of Alien Studio.";
        else info.problem = juce::String("This bundle has no ") + wanted + " build" + (others.isEmpty() ? "." : " (it contains: " + others.joinIntoString(", ") + ").");
        return info;
    }
    // A single file: on Windows its header says what it was built for.
    info.binary = bundle;
    if (target == Platform::Windows) {
        switch (peArchitecture(bundle)) {
            case Arch::x86: info.problem = "This plugin is a 32-bit file. Alien Studio is 64-bit and needs the 64-bit version."; break;
            case Arch::arm64: info.problem = "This plugin is built for ARM processors, not x64."; break;
            case Arch::other: info.problem = "This file was built for a processor type Alien Studio cannot load."; break;
            default: break;
        }
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// The child process
// ---------------------------------------------------------------------------------------------
// Protocol between the scanner and the process it starts (the application itself, "--scan-vst3"):
//   exit 0 and a result file     plugins found
//   exit 2                        library loaded, no plugin in it
//   exit 5                        the operating system could not load the library (reason printed)
//   a sibling "<result>.stage" file always names the last step begun, so a crash can be placed
//   a sibling "<result>.log" file receives anything the plugin prints
enum ChildExit : int { exitOk = 0, exitNoPlugins = 2, exitBadArguments = 3, exitOutputFailed = 4, exitLibraryFailed = 5 };

inline juce::File siblingOf(const juce::File& output, const juce::String& suffix) { return output.getSiblingFile(output.getFileName() + suffix); }
// A result path no other process or thread can also pick. getNonexistentChildFile is not atomic: two
// checks started together chose the same name, then deleted each other's ".stage" and ".log" files and
// read each other's results.
inline juce::File uniqueOutput(const juce::File& folder, const juce::String& prefix) {
    return folder.getChildFile(prefix + "-" + juce::Uuid().toDashedString() + ".xml");
}

inline void writeStage(const juce::File& output, const juce::String& stage) {
    juce::FileOutputStream out(siblingOf(output, ".stage"));
    if (!out.openedOk()) return;
    out.setPosition(0); out.truncate(); out.writeText(stage, false, false, nullptr); out.flush();
}
// Sends the child's own stdout/stderr to a file the parent reads afterwards. A plugin that prints a
// lot can therefore never fill a pipe and stall the scan.
inline void redirectOutputToFile(const juce::File& output) {
    const auto path = siblingOf(output, ".log");
   #if JUCE_WINDOWS
    _wfreopen(path.getFullPathName().toWideCharPointer(), L"w", stdout); _wfreopen(path.getFullPathName().toWideCharPointer(), L"a", stderr);
   #else
    if (std::freopen(path.getFullPathName().toRawUTF8(), "w", stdout) == nullptr) return;
    if (std::freopen(path.getFullPathName().toRawUTF8(), "a", stderr) == nullptr) return;
   #endif
}
#if JUCE_WINDOWS
inline juce::String lastSystemError() {
    const auto code = GetLastError();
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    juce::String text = message != nullptr ? juce::String(message).trim() : juce::String();
    if (message != nullptr) LocalFree(message);
    return "Windows error " + juce::String(static_cast<int>(code)) + (text.isNotEmpty() ? ": " + text : juce::String());
}
#endif

// The helper process (scan or check) must never wait for someone to click a system error dialog: nobody may be there
// to see it, and it would hold the helper until its time limit. Windows shows one when a library cannot be loaded
// ("Bad Image" for a damaged .vst3), when a drive is not ready, and when a process crashes. A process inherits this
// setting from whoever starts it, so the helper sets it itself rather than relying on its parent (Alien Studio uses
// the default, which shows them). Returns the new mode (0 on other systems).
inline unsigned quietSystemErrorDialogs() {
   #if JUCE_WINDOWS
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return GetErrorMode();
   #else
    return 0;
   #endif
}

// What "--scan-vst3 <bundle> <result.xml>" does. Returns the process exit code.
inline int scanBundleInChild(const juce::String& bundlePath, const juce::File& output) {
    quietSystemErrorDialogs();
    redirectOutputToFile(output);
    writeStage(output, "start");
    const juce::File bundle(bundlePath);
    const auto info = inspectBundle(bundle);
    if (info.problem.isNotEmpty()) { std::fprintf(stderr, "%s\n", info.problem.toRawUTF8()); return exitLibraryFailed; }
    juce::AudioPluginFormatManager formats; addHostingFormats(formats);
    juce::KnownPluginList list;
    writeStage(output, "load");            // JUCE loads the library and reads its class list in one call
    for (auto* format : formats.getFormats()) {
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, bundlePath);
        for (const auto* description : descriptions) list.addType(*description);
    }
    if (list.getNumTypes() == 0) {
        // Find out why: either the system cannot load the library (missing dependency, wrong architecture)
        // or it loads and offers nothing. The folder is made current first, as JUCE does, so dependencies next to it resolve.
        writeStage(output, "diagnose");
        if (info.binary != juce::File{}) {
            const auto previous = juce::File::getCurrentWorkingDirectory();
            info.binary.getParentDirectory().setAsCurrentWorkingDirectory();
            juce::DynamicLibrary library;
            const bool loaded = library.open(info.binary.getFullPathName());
           #if JUCE_WINDOWS
            const auto reason = loaded ? juce::String() : lastSystemError();
           #else
            const auto reason = loaded ? juce::String() : juce::String("dlopen failed");
           #endif
            previous.setAsCurrentWorkingDirectory();
            if (!loaded) { std::fprintf(stderr, "The plugin library could not be loaded: %s\n", reason.toRawUTF8()); return exitLibraryFailed; }
            // Deliberately never closed: unloading a plugin library is itself a place plugins crash.
            (void) new juce::DynamicLibrary(std::move(library));
        }
        return exitNoPlugins;
    }
    writeStage(output, "write");
    auto xml = list.createXml();
    if (!xml || !xml->writeTo(output)) return exitOutputFailed;
    writeStage(output, "done");
    return exitOk;
}

// ---------------------------------------------------------------------------------------------
// The parent: running a child and interpreting what happened
// ---------------------------------------------------------------------------------------------
struct ChildRequest { juce::File executable; juce::String bundle; juce::File output; int timeoutMs{60000}; const std::atomic<bool>* cancel{nullptr}; };
struct ChildResult {
    enum class Outcome { finished, timedOut, couldNotStart, cancelled };
    Outcome outcome{Outcome::finished};
    juce::uint32 exitCode{0};
    juce::String stage;         // last step the child recorded
    juce::String printed;       // what the plugin printed
    juce::String xml;           // the result file, if any
    juce::int64 milliseconds{0};
};
using ChildRunner = std::function<ChildResult(const ChildRequest&)>;

inline ChildResult runChildProcess(const ChildRequest& request) {
    ChildResult result;
    const auto stageFile = siblingOf(request.output, ".stage"), logFile = siblingOf(request.output, ".log");
    for (const auto& f : {request.output, stageFile, logFile}) f.deleteFile();
    const auto started = juce::Time::getMillisecondCounter();
    juce::ChildProcess child;
    if (!request.executable.existsAsFile()) {
        result.outcome = ChildResult::Outcome::couldNotStart;   // POSIX would otherwise "start" a child that immediately fails to exec
    } else if (!child.start(juce::StringArray{request.executable.getFullPathName(), "--scan-vst3", request.bundle, request.output.getFullPathName()}, 0)) {
        result.outcome = ChildResult::Outcome::couldNotStart;
    } else {
        // Waits in short slices so that a cancelled scan does not have to wait out a plugin that is stuck.
        bool finished = false, cancelled = false;
        for (;;) {
            if (child.waitForProcessToFinish(100)) { finished = true; break; }
            if (request.cancel != nullptr && request.cancel->load()) { cancelled = true; break; }
            if (juce::Time::getMillisecondCounter() - started >= static_cast<juce::uint32>(std::max(0, request.timeoutMs))) break;
        }
        if (finished) result.exitCode = child.getExitCode();   // note: JUCE reports 0 on POSIX when the child died of a signal
        else {
            child.kill(); child.waitForProcessToFinish(3000);
            result.outcome = cancelled ? ChildResult::Outcome::cancelled : ChildResult::Outcome::timedOut;
        }
    }
    result.milliseconds = static_cast<juce::int64>(juce::Time::getMillisecondCounter() - started);
    result.stage = stageFile.loadFileAsString().trim();
    result.printed = logFile.loadFileAsString().substring(0, 4000).trim();
    if (request.output.existsAsFile()) result.xml = request.output.loadFileAsString();
    for (const auto& f : {request.output, stageFile, logFile}) f.deleteFile();
    return result;
}

enum class Status { ok, noPlugins, incompatible, crashed, timedOut, failed, couldNotStart };
inline const char* statusLabel(Status s) {
    switch (s) {
        case Status::ok: return "OK"; case Status::noPlugins: return "NO PLUGINS"; case Status::incompatible: return "INCOMPATIBLE";
        case Status::crashed: return "CRASHED"; case Status::timedOut: return "TIMED OUT"; case Status::failed: return "FAILED";
        case Status::couldNotStart: return "NOT STARTED";
    }
    return "?";
}
inline const char* statusKey(Status s) {
    switch (s) {
        case Status::ok: return "ok"; case Status::noPlugins: return "noplugins"; case Status::incompatible: return "incompatible";
        case Status::crashed: return "crashed"; case Status::timedOut: return "timedout"; case Status::failed: return "failed";
        case Status::couldNotStart: return "notstarted";
    }
    return "failed";
}
inline Status statusFromKey(const juce::String& key) {
    for (auto s : {Status::ok, Status::noPlugins, Status::incompatible, Status::crashed, Status::timedOut, Status::failed, Status::couldNotStart})
        if (key == statusKey(s)) return s;
    return Status::failed;
}

inline juce::String hex32(juce::uint32 value) { return "0x" + juce::String::toHexString(static_cast<juce::int64>(value)).toUpperCase().paddedLeft('0', 8); }
inline juce::String exceptionName(juce::uint32 code) {
    switch (code) {
        case 0xC0000005u: return "access violation";
        case 0xC000001Du: return "illegal instruction";
        case 0xC0000096u: return "privileged instruction";
        case 0xC0000094u: return "integer divide by zero";
        case 0xC00000FDu: return "stack overflow";
        case 0xC0000374u: return "heap corruption";
        case 0xC0000409u: return "fatal error (stack buffer overrun)";
        case 0xC0000135u: return "a required DLL was not found";
        case 0xC0000142u: return "a required DLL failed to initialise";
        case 0xC000007Bu: return "invalid image format (32-bit / 64-bit mismatch)";
        case 0x80000003u: return "breakpoint";
        default: return {};
    }
}
inline juce::String describeStage(const juce::String& stage) {
    if (stage.isEmpty() || stage == "start") return "starting up";
    if (stage == "load") return "loading the plugin and reading its class list";
    if (stage == "diagnose") return "working out why it has no plugins";
    if (stage == "write") return "writing its results";
    if (stage == "done") return "finishing";
    return stage;
}

struct Verdict { Status status{Status::failed}; juce::String detail; };
// `pluginsInResult` is how many plugin types the result file described (0 if absent or unreadable).
inline Verdict classify(const ChildResult& r, int pluginsInResult, int timeoutMs) {
    using O = ChildResult::Outcome;
    const auto seconds = [](juce::int64 ms) { return juce::String(static_cast<double>(ms) / 1000.0, 1) + " s"; };
    if (r.outcome == O::couldNotStart) return {Status::couldNotStart, "The scanner process could not be started."};
    if (r.outcome == O::cancelled) return {Status::failed, "Cancelled before it finished."};
    if (r.outcome == O::timedOut)
        return {Status::timedOut, "Did not finish within " + juce::String(timeoutMs / 1000) + " s while " + describeStage(r.stage)
                + ". It may be waiting on a licence check, update prompt or other dialog."};
    const bool ntStatusCrash = (r.exitCode & 0xC0000000u) == 0xC0000000u || r.exitCode == 0x80000003u;
    if (ntStatusCrash) {
        const auto name = exceptionName(r.exitCode);
        return {Status::crashed, "Crashed while " + describeStage(r.stage) + " (exit code " + hex32(r.exitCode) + (name.isNotEmpty() ? ", " + name : juce::String()) + ") after " + seconds(r.milliseconds) + "."};
    }
   #if JUCE_WINDOWS
    // MSVC abort() exits with 3 when CRT reporting is disabled. This overlaps
    // exitBadArguments, but argument rejection occurs before loading begins.
    // Only classify it as a probable abort when the stage proves plugin code ran.
    if (r.exitCode == 3 && (r.stage == "load" || r.stage == "diagnose"))
        return {Status::crashed, "The scanner most likely aborted while " + describeStage(r.stage)
                + " (Windows CRT exit code 3) after " + seconds(r.milliseconds) + "."};
   #endif
    if (r.exitCode == exitLibraryFailed) {
        const auto why = r.printed.isNotEmpty() ? r.printed.upToFirstOccurrenceOf("\n", false, false).trim() : juce::String("The operating system could not load it.");
        return {Status::incompatible, why};
    }
    if (r.exitCode == exitNoPlugins) return {Status::noPlugins, "The file loads, but it does not contain a VST3 plugin."};
    if (r.exitCode != exitOk) return {Status::failed, "The scanner exited with code " + juce::String(static_cast<int>(r.exitCode)) + " while " + describeStage(r.stage) + "."};
    // Exit code 0. Only a finished protocol counts: POSIX reports a signal death as exit code 0.
    if (r.stage != "done")
        return {Status::crashed, "The scanner ended without finishing while " + describeStage(r.stage) + " (the plugin most likely crashed) after " + seconds(r.milliseconds) + "."};
    if (pluginsInResult <= 0) return {Status::failed, "The scanner finished but its result could not be read."};
    return {Status::ok, {}};
}

// ---------------------------------------------------------------------------------------------
// What earlier scans learned
// ---------------------------------------------------------------------------------------------
struct Fingerprint { juce::int64 size{}, modified{}; bool operator==(const Fingerprint&) const = default; };
inline Fingerprint fingerprintOf(const juce::File& bundle) {
    const auto info = inspectBundle(bundle);
    const auto& file = info.binary != juce::File{} && info.binary.existsAsFile() ? info.binary : bundle;
    return {file.getSize(), file.getLastModificationTime().toMilliseconds()};
}

class ScanDatabase {
public:
    struct Entry {
        juce::String path;
        Fingerprint fingerprint;
        Status status{Status::ok};
        juce::String detail, stage;
        int plugins{0}, attempts{0};
        juce::int64 scannedAt{0};
    };
    const Entry* find(const juce::File& bundle) const { auto it = entries.find(key(bundle)); return it == entries.end() ? nullptr : &it->second; }
    void put(Entry e) { const auto k = key(juce::File(e.path)); entries[k] = std::move(e); }
    void erase(const juce::File& bundle) { entries.erase(key(bundle)); }
    std::vector<Entry> all() const { std::vector<Entry> v; for (const auto& [k, e] : entries) v.push_back(e); return v; }
    std::vector<Entry> failures() const { std::vector<Entry> v; for (const auto& [k, e] : entries) if (e.status != Status::ok) v.push_back(e); return v; }
    bool save(const juce::File& file) const {
        juce::XmlElement root("PluginScanState"); root.setAttribute("version", 1);
        for (const auto& [k, e] : entries) {
            auto* x = root.createNewChildElement("Bundle");
            x->setAttribute("path", e.path); x->setAttribute("size", juce::String(e.fingerprint.size)); x->setAttribute("modified", juce::String(e.fingerprint.modified));
            x->setAttribute("status", statusKey(e.status)); x->setAttribute("detail", e.detail); x->setAttribute("stage", e.stage);
            x->setAttribute("plugins", e.plugins); x->setAttribute("attempts", e.attempts); x->setAttribute("scannedAt", juce::String(e.scannedAt));
        }
        if (!file.getParentDirectory().createDirectory()) return false;
        return root.writeTo(file);
    }
    bool load(const juce::File& file) {
        entries.clear();
        auto xml = juce::parseXML(file);
        if (!xml || xml->getTagName() != "PluginScanState") return false;
        for (auto* x : xml->getChildWithTagNameIterator("Bundle")) {
            Entry e; e.path = x->getStringAttribute("path"); if (e.path.isEmpty()) continue;
            e.fingerprint = {x->getStringAttribute("size").getLargeIntValue(), x->getStringAttribute("modified").getLargeIntValue()};
            e.status = statusFromKey(x->getStringAttribute("status")); e.detail = x->getStringAttribute("detail"); e.stage = x->getStringAttribute("stage");
            e.plugins = x->getIntAttribute("plugins"); e.attempts = x->getIntAttribute("attempts"); e.scannedAt = x->getStringAttribute("scannedAt").getLargeIntValue();
            put(std::move(e));
        }
        return true;
    }
    static juce::File defaultFile() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio").getChildFile("PluginScanState.xml");
    }
private:
    static juce::String key(const juce::File& f) { return f.getFullPathName().toLowerCase(); }
    std::map<juce::String, Entry> entries;
};

// ---------------------------------------------------------------------------------------------
// A whole scan
// ---------------------------------------------------------------------------------------------
struct ScanOptions {
    std::vector<juce::File> folders;        // searched recursively
    std::vector<juce::File> bundles;        // scanned as given
    bool retryFailed{false};                // scan again bundles that failed before, even though the file is unchanged
    bool rescanUnchanged{false};            // scan everything, even bundles that scanned fine and have not changed
    int timeoutMs{60000};
    juce::File executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File scratchFolder = juce::File::getSpecialLocation(juce::File::tempDirectory);
    juce::String appVersion;
};
struct BundleReport {
    enum class Action { scanned, skippedUnchanged, skippedKnownFailure, rejectedBeforeLoading };
    juce::File bundle;
    Action action{Action::scanned};
    Status status{Status::ok};
    juce::String detail;
    juce::StringArray plugins;              // "Name [instrument]" / "Name [FX]"
    juce::int64 milliseconds{0};
};
struct ScanReport {
    std::vector<BundleReport> bundles;
    std::vector<juce::String> problems;
    int foldersVisited{0}, pluginsAdded{0}, pluginsRemoved{0};
    bool cancelled{false};
    juce::int64 milliseconds{0};
    int count(Status s, bool includeSkipped = true) const {
        int n = 0;
        for (const auto& b : bundles) if (b.status == s && (includeSkipped || b.action == BundleReport::Action::scanned)) ++n;
        return n;
    }
    int skippedUnchanged() const { int n = 0; for (const auto& b : bundles) n += b.action == BundleReport::Action::skippedUnchanged; return n; }
    int failedCount() const { int n = 0; for (const auto& b : bundles) n += b.status != Status::ok; return n; }
    juce::String summary() const {
        juce::String s = juce::String(static_cast<int>(bundles.size())) + " plugin file(s): " + juce::String(count(Status::ok)) + " OK";
        const std::pair<Status, const char*> kinds[] = {{Status::crashed, "crashed"}, {Status::timedOut, "timed out"}, {Status::incompatible, "incompatible"}, {Status::noPlugins, "contained no plugin"}, {Status::failed, "failed"}, {Status::couldNotStart, "could not be started"}};
        for (const auto& [kind, word] : kinds) if (const int n = count(kind)) s += ", " + juce::String(n) + " " + word;
        if (const int n = skippedUnchanged()) s += " (" + juce::String(n) + " unchanged, not scanned again)";
        s += ". " + juce::String(pluginsAdded) + " plugin(s) added";
        if (pluginsRemoved > 0) s += ", " + juce::String(pluginsRemoved) + " removed because their files are gone";
        if (cancelled) s += ". Cancelled before finishing";
        return s + ".";
    }
    // Everything about this scan, one file per entry: what the "Show the last scan log" command opens.
    juce::String fullText() const {
        juce::String t;
        t << "Alien Studio plugin scan  " << juce::Time::getCurrentTime().toString(true, true) << "\n"
          << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "") << "\n\n"
          << summary() << " Took " << juce::String(static_cast<double>(milliseconds) / 1000.0, 1) << " s.\n";
        for (const auto& p : problems) t << "Folder problem: " << p << "\n";
        t << "\n";
        for (const auto& b : bundles) {
            juce::String label = statusLabel(b.status);
            if (b.action == BundleReport::Action::skippedUnchanged) label = "UNCHANGED";
            else if (b.action == BundleReport::Action::skippedKnownFailure) label = "SKIPPED " + label;
            t << label.paddedRight(' ', 22) << b.bundle.getFullPathName() << "\n";
            if (b.detail.isNotEmpty()) t << "      " << b.detail << "\n";
            if (!b.plugins.isEmpty()) t << "      " << b.plugins.joinIntoString(", ") << "\n";
        }
        if (const auto a = attention(); a.isNotEmpty()) t << "\nNEEDS ATTENTION\n" << a;
        return t;
    }
    // The failures with advice, for the end of the log and the plugin panel.
    juce::String attention() const {
        juce::String s;
        for (const auto& b : bundles) if (b.status != Status::ok)
            s += juce::String(statusLabel(b.status)) + "  " + b.bundle.getFullPathName() + "\n    " + b.detail + "\n";
        return s;
    }
};
struct ScanHooks {
    std::function<void(int done, int total, const juce::String& current)> progress;
    std::function<void()> checkpoint;       // after every bundle: save the plugin list and database, so a crash loses nothing
};

inline juce::String describePlugin(const juce::PluginDescription& d) { return d.name + (d.isInstrument ? " [instrument]" : " [FX]"); }

inline ScanReport runScan(const ScanOptions& options, ScanDatabase& database, juce::KnownPluginList& known, const ChildRunner& runner,
                          const DiagnosticLog& log, const std::atomic<bool>& cancel, const ScanHooks& hooks = {}) {
    ScanReport report; const auto started = juce::Time::getMillisecondCounter();
    log.write("info", "==== Plugin scan started ====");
    log.write("info", "Alien Studio " + options.appVersion + " on " + juce::SystemStats::getOperatingSystemName() + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : ""));
    for (const auto& f : options.folders) log.write("info", "Searching recursively: " + f.getFullPathName());
    for (const auto& b : options.bundles) log.write("info", "Plugin file: " + b.getFullPathName());
    log.write("info", juce::String("Options: ") + (options.retryFailed ? "retry failed; " : "") + (options.rescanUnchanged ? "rescan unchanged; " : "") + "time limit " + juce::String(options.timeoutMs / 1000) + " s per file");

    auto found = discoverVst3Bundles(options.folders);
    for (const auto& b : options.bundles) {
        bool have = false; for (const auto& existing : found.bundles) have = have || existing == b;
        if (!have) found.bundles.push_back(b);
    }
    report.foldersVisited = found.foldersVisited; report.problems = found.problems;
    for (const auto& p : found.problems) log.write("warn", p);
    log.write("info", "Found " + juce::String(static_cast<int>(found.bundles.size())) + " plugin file(s) in " + juce::String(found.foldersVisited) + " folder(s)");

    const int total = static_cast<int>(found.bundles.size());
    const auto sameBundle = [](const juce::PluginDescription& d, const juce::File& bundle) { return juce::File(d.fileOrIdentifier) == bundle; };
    int index = 0;
    for (const auto& bundle : found.bundles) {
        if (cancel.load()) { report.cancelled = true; log.write("warn", "Scan cancelled by the user after " + juce::String(index) + " of " + juce::String(total) + " files."); break; }
        ++index;
        if (hooks.progress) hooks.progress(index - 1, total, bundle.getFileName());
        BundleReport entry; entry.bundle = bundle;
        const auto prefix = "[" + juce::String(index).paddedLeft(' ', juce::String(total).length()) + "/" + juce::String(total) + "] ";
        const auto line = [&](const juce::String& level, const juce::String& label, const juce::String& text) {
            log.write(level, prefix + label.paddedRight(' ', 12) + bundle.getFullPathName() + "  " + text);
        };
        const auto finish = [&] { report.bundles.push_back(entry); if (hooks.checkpoint) hooks.checkpoint(); };

        const auto info = inspectBundle(bundle);
        const auto fingerprint = fingerprintOf(bundle);
        const auto* previous = database.find(bundle);
        const int attempts = previous != nullptr ? previous->attempts : 0;
        const auto record = [&](Status status, const juce::String& detail, const juce::String& stage, int plugins) {
            ScanDatabase::Entry e; e.path = bundle.getFullPathName(); e.fingerprint = fingerprint; e.status = status; e.detail = detail; e.stage = stage;
            e.plugins = plugins; e.attempts = attempts + 1; e.scannedAt = juce::Time::currentTimeMillis(); database.put(std::move(e));
        };

        if (info.problem.isNotEmpty()) {   // no need to start anything: the file says it cannot work
            entry.action = BundleReport::Action::rejectedBeforeLoading; entry.status = Status::incompatible; entry.detail = info.problem;
            record(Status::incompatible, info.problem, {}, 0);
            line("warn", statusLabel(entry.status), info.problem);
            finish(); continue;
        }
        if (previous != nullptr && previous->fingerprint == fingerprint && !options.rescanUnchanged) {
            bool listed = false; for (const auto& d : known.getTypes()) listed = listed || sameBundle(d, bundle);
            if (previous->status == Status::ok && listed) {
                entry.action = BundleReport::Action::skippedUnchanged; entry.status = Status::ok;
                line("info", "UNCHANGED", "scanned earlier, " + juce::String(previous->plugins) + " plugin(s); not scanned again");
                finish(); continue;
            }
            if (previous->status != Status::ok && previous->status != Status::couldNotStart && !options.retryFailed) {
                entry.action = BundleReport::Action::skippedKnownFailure; entry.status = previous->status;
                entry.detail = previous->detail + " (Failed on an earlier scan and the file has not changed since. Use \"Retry failed plugins\" to try again.)";
                line("warn", juce::String("SKIPPED ") + statusLabel(previous->status), entry.detail);
                finish(); continue;
            }
        }

        ChildRequest request; request.executable = options.executable; request.bundle = bundle.getFullPathName(); request.timeoutMs = options.timeoutMs;
        request.output = uniqueOutput(options.scratchFolder, "AlienPluginScan");
        request.cancel = &cancel;
        const auto result = runner(request);
        if (result.outcome == ChildResult::Outcome::cancelled) {   // stopped part-way: say so, and record nothing about this file
            report.cancelled = true;
            log.write("warn", "Scan cancelled by the user while scanning " + bundle.getFullPathName() + " (file " + juce::String(index) + " of " + juce::String(total) + "); nothing was recorded for it.");
            break;
        }
        juce::KnownPluginList foundTypes;
        if (result.xml.isNotEmpty()) if (auto xml = juce::parseXML(result.xml)) foundTypes.recreateFromXml(*xml);
        const auto verdict = classify(result, foundTypes.getNumTypes(), options.timeoutMs);
        entry.status = verdict.status; entry.detail = verdict.detail; entry.milliseconds = result.milliseconds;
        const auto timing = " in " + juce::String(static_cast<double>(result.milliseconds) / 1000.0, 1) + " s";
        if (verdict.status == Status::ok) {
            for (const auto& old : known.getTypes()) if (sameBundle(old, bundle)) known.removeType(old);
            for (const auto& d : foundTypes.getTypes()) { entry.plugins.add(describePlugin(d)); if (known.addType(d)) ++report.pluginsAdded; }
            record(Status::ok, {}, result.stage, entry.plugins.size());
            line("info", "OK", juce::String(entry.plugins.size()) + " plugin(s)" + timing + ": " + entry.plugins.joinIntoString(", "));
        } else if (verdict.status == Status::couldNotStart) {
            line("error", statusLabel(verdict.status), verdict.detail + " Not recorded as a plugin failure.");
        } else {
            record(verdict.status, verdict.detail, result.stage, 0);
            line(verdict.status == Status::crashed || verdict.status == Status::timedOut ? "fault" : "warn", statusLabel(verdict.status), verdict.detail + (result.printed.isNotEmpty() ? "  Plugin output: " + result.printed.substring(0, 500) : juce::String()));
        }
        finish();
    }
    if (hooks.progress) hooks.progress(index, total, {});

    // Plugins whose files have been uninstalled since an earlier scan.
    if (!report.cancelled)
        for (const auto& d : known.getTypes())
            if (d.pluginFormatName == "VST3" && !juce::File(d.fileOrIdentifier).exists()) {
                known.removeType(d); ++report.pluginsRemoved;
                log.write("info", "Removed from the list, file no longer exists: " + d.name + " (" + d.fileOrIdentifier + ")");
            }
    report.milliseconds = static_cast<juce::int64>(juce::Time::getMillisecondCounter() - started);
    log.write("info", "Summary: " + report.summary() + " Took " + juce::String(static_cast<double>(report.milliseconds) / 1000.0, 1) + " s.");
    if (const auto attention = report.attention(); attention.isNotEmpty()) log.write("warn", "Needs attention:\n" + attention);
    log.write("info", "==== Plugin scan finished ====");
    if (hooks.checkpoint) hooks.checkpoint();
    return report;
}

} // namespace alien::scan
