#include "MidiControllerMappings.h"
#include <cstdlib>
#include <iostream>

using namespace alien;

static void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

int main() {
    MidiControllerMappings mappings;
    MidiControllerActionQueue actions;
    const int sceneTwo = 1;
    mappings.beginLearn(sceneTwo);
    require(mappings.processCC(1, 24, 127) == -1, "learning binds the incoming CC without launching immediately");
    require(mappings.mappingFor(sceneTwo) == 152, "mapping retains MIDI channel and controller number");
    require(mappings.takeLearnedAction() == sceneTwo && mappings.takeLearnedAction() == -1,
        "learn completion is published once for the UI");
    require(mappings.processCC(1, 24, 127) == -1, "a held controller does not repeat its action");
    require(mappings.processCC(1, 24, 0) == -1, "controller release rearms its action");
    const int action = mappings.processCC(1, 24, 64);
    require(action == sceneTwo, "a new high edge launches the mapped scene");
    mappings.resetEdges();
    require(mappings.processCC(1, 24, 127) == sceneTwo,
        "MIDI device reset rearms mapped buttons that were held during disconnect");
    require(actions.push(action), "real-time action enters the bounded UI queue");
    int delivered = -1;
    require(actions.pop(delivered) && delivered == sceneTwo && !actions.pop(delivered),
        "UI drains queued controller actions in order");
    require(mappings.processCC(0, 24, 127) == -1, "same CC on another channel does not trigger the mapping");

    mappings.beginLearn(MidiControllerMappings::stopAllAction);
    require(mappings.processCC(1, 24, 0) == -1, "a control can be learned for stop-all");
    require(mappings.mappingFor(sceneTwo) == -1
        && mappings.mappingFor(MidiControllerMappings::stopAllAction) == 152,
        "relearning an assigned CC moves it instead of leaving duplicate actions");
    require(mappings.processCC(1, 24, 0) == -1
        && mappings.processCC(1, 24, 127) == MidiControllerMappings::stopAllAction,
        "stop-all mapping triggers on its next press edge");
    mappings.clearMapping(MidiControllerMappings::stopAllAction);
    require(mappings.mappingFor(MidiControllerMappings::stopAllAction) == -1
        && mappings.processCC(1, 24, 0) == -1, "clearing removes the mapping safely");
    mappings.beginLearn(99);
    mappings.beginLearn(0);
    mappings.cancelLearn();
    require(mappings.processCC(0, 20, 127) == -1 && mappings.mappingFor(0) == -1,
        "cancelled and invalid learn targets do not assign controls");

    MidiControllerActionQueue bounded;
    int queued = 0;
    for (; queued < 100 && bounded.push(0); ++queued) {}
    require(queued == static_cast<int>(MidiControllerActionQueue::capacity) - 1,
        "action queue is bounded and reports full without blocking");
    std::cout << "MIDI controller learn, channel filtering, edge triggering and action queue passed\n";
}
