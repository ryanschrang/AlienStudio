// Entry point of the AlienTestPlugin VST3: the behaviour comes from the ALIEN_TEST_PLUGIN environment variable.
#include "TestPluginCore.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new alien::testplugin::Processor(alien::testplugin::Behaviour::fromEnvironment());
}
