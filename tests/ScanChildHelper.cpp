// Stands in for "Alien Studio --scan-vst3 <bundle> <result.xml>" in the scanner tests. It speaks the
// same protocol (stage markers, redirected output, exit codes) but misbehaves on request: the
// behaviour is chosen by the start of the bundle's file name.
#include "PluginScan.h"
#include <chrono>
#include <cstdlib>
#include <thread>

static volatile std::uintptr_t zeroAddress = 0;

int main(int argc, char** argv) {
   #if defined(_MSC_VER)
    // abort() is intentional in this helper. Disable the CRT report/fault UI so
    // unattended Windows runs observe the child exit rather than a hidden dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
   #endif
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc != 4 || juce::String(argv[1]) != "--scan-vst3") return alien::scan::exitBadArguments;
    const juce::String bundle = argv[2];
    const juce::File output(argv[3]);
    const auto name = juce::File(bundle).getFileName();
    alien::scan::redirectOutputToFile(output);
    alien::scan::writeStage(output, "start");
    alien::scan::writeStage(output, "load");
    if (name.startsWith("crash")) { *reinterpret_cast<volatile int*>(zeroAddress) = 1; }
    if (name.startsWith("abort")) std::abort();
    if (name.startsWith("hang")) {
        // Leaves a heartbeat so a test can tell whether this process is still alive after the parent gave up on it.
        const auto beat = output.getSiblingFile(output.getFileName() + ".beat");
        for (int i = 0; i < 72000; ++i) { beat.replaceWithText(juce::String(i)); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return 0;
    }
    if (name.startsWith("none")) return alien::scan::exitNoPlugins;
    if (name.startsWith("lib")) { std::fprintf(stderr, "The plugin library could not be loaded: Windows error 126: The specified module could not be found.\n"); return alien::scan::exitLibraryFailed; }
    if (name.startsWith("exit9")) return 9;
    if (name.startsWith("chatty")) {
        // Far more than any pipe buffer holds: a parent that waited on a pipe would stall here.
        for (int i = 0; i < 40000; ++i) { std::fprintf(stdout, "plugin chatter line %d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i); std::fprintf(stderr, "noise %d\n", i); }
    }
    juce::PluginDescription d; d.name = "Helper " + name; d.pluginFormatName = "VST3"; d.fileOrIdentifier = bundle; d.isInstrument = name.contains("synth");
    d.uniqueId = d.deprecatedUid = 1234; d.manufacturerName = "Test"; d.version = "1.0";
    juce::KnownPluginList list; list.addType(d);
    alien::scan::writeStage(output, "write");
    auto xml = list.createXml();
    if (!xml || !xml->writeTo(output)) return alien::scan::exitOutputFailed;
    alien::scan::writeStage(output, "done");
    return alien::scan::exitOk;
}
