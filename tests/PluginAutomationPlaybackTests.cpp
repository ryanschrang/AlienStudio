// Plugin parameter automation playback (T1-09 part 3, AUTO-03): a Song lane drives its plugin parameter, found by
// the plugin's instance ID and the parameter's ID (never a slot index), once per audio block, with no allocation;
// lanes for plugins that are missing, replaced or switched off do nothing; a plugin that crashes while receiving
// automation is switched off like any other failure. The automation editor offers the track's plugin parameters.
#include "ToneEngine.h"
#include "PluginHost.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace alien;

static thread_local std::size_t allocations{};
void* operator new(std::size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static volatile std::uintptr_t zeroAddress = 0;

// A parameter that crashes when it is set, as a broken plugin might.
struct CrashingParameter final : juce::AudioParameterFloat {
    CrashingParameter() : juce::AudioParameterFloat(juce::ParameterID{"broken", 1}, "Broken", 0.0f, 1.0f, 0.5f) {}
    void setValue(float) override { *reinterpret_cast<volatile int*>(zeroAddress) = 1; }
};
// An effect with a "cutoff" and a "mix" parameter (and, if asked, a "broken" one), passing audio through.
struct TestEffect final : juce::AudioProcessor {
    explicit TestEffect(bool broken)
        : juce::AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())) {
        addParameter(cutoff = new juce::AudioParameterFloat(juce::ParameterID{"cutoff", 1}, "Cutoff", 0.0f, 1.0f, 0.5f));
        addParameter(mix = new juce::AudioParameterFloat(juce::ParameterID{"mix", 1}, "Mix", 0.0f, 1.0f, 1.0f));
        if (broken) addParameter(new CrashingParameter());
    }
    const juce::String getName() const override { return "Test effect"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    juce::AudioParameterFloat* cutoff{};
    juce::AudioParameterFloat* mix{};
};

static PluginState effect(const char* id) {
    juce::PluginDescription d; d.name = id; d.pluginFormatName = "VST3"; d.fileOrIdentifier = id; d.isInstrument = false;
    return PluginState(d.createXml()->toString(), {});
}
static PluginFactory factory() {
    return [](const juce::PluginDescription& d, double, int, juce::String&) -> std::unique_ptr<juce::AudioProcessor> {
        return std::make_unique<TestEffect>(d.fileOrIdentifier == "broken");
    };
}
static TestEffect& effectAt(PluginRuntime& runtime, int rack, int slot) {
    return *dynamic_cast<TestEffect*>(runtime.racks[static_cast<std::size_t>(rack)].effects[static_cast<std::size_t>(slot)].processor.get());
}
static automation::ParameterLane ramp(std::uint64_t instance, const char* id, std::int64_t end) {
    automation::ParameterLane lane; lane.target = {instance, id};
    lane.put({0, 0.0f, automation::Curve::linear});
    lane.put({end, 1.0f, automation::Curve::linear});
    return lane;
}

static void lookups() {
    std::vector<automation::ParameterBinding> bindings{{7, "b", 0, 0, 1}, {3, "z", 1, -1, 0}, {7, "a", 0, 0, 0}, {3, "a", 2, 1, 4}};
    automation::sortBindings(bindings);
    require(bindings[0].instance == 3 && bindings[0].parameterId == "a" && bindings[3].parameterId == "b", "bindings sort by instance, then ID");
    const auto* found = automation::findSorted({7, "a"}, bindings);
    require(found != nullptr && found->parameter == 0 && automation::findSorted({3, "z"}, bindings)->slot == -1, "a target is found by binary search");
    require(automation::findSorted({7, "c"}, bindings) == nullptr && automation::findSorted({0, "a"}, bindings) == nullptr, "an unknown or invalid target is not");
    bindings.push_back({7, "a", 0, 1, 0});
    automation::sortBindings(bindings);
    require(automation::findSorted({7, "a"}, bindings) == nullptr, "an ambiguous target drives nothing");
    std::puts("PASS: bindings are found by instance and parameter ID");
}

static void runtime() {
    auto rt = std::make_unique<PluginRuntime>();
    rt->prepare(48000);
    PluginHost host(*rt, {}, factory());
    std::vector<PluginRackState> racks(2); PluginRackState master;
    racks[0].effects = {effect("first"), effect("second")};
    racks[1].effects = {effect("broken")};
    require(host.restore(racks, master, 48000).isEmpty(), "the test effects load");
    require(rt->parameterBindings.size() == 7, "restoring binds every automatable parameter with an ID");
    const auto first = racks[0].effects[0].instanceId, second = racks[0].effects[1].instanceId, broken = racks[1].effects[0].instanceId;

    std::vector<automation::ParameterLane> lanes{ramp(second, "cutoff", 1000), ramp(first, "mix", 1000)};
    lanes[1].points = {{0, 0.25f, automation::Curve::hold}};
    lanes.push_back(ramp(12345, "cutoff", 1000));   // a plugin that is not loaded
    auto off = ramp(first, "cutoff", 1000); off.enabled = false; lanes.push_back(off);
    const auto before = allocations;
    const int changed = rt->applyParameterAutomation(lanes, 250.0);
    require(allocations == before, "applying automation does not allocate");
    require(changed == 2 && std::abs(effectAt(*rt, 0, 1).cutoff->get() - 0.25f) < 1e-6f && std::abs(effectAt(*rt, 0, 0).mix->get() - 0.25f) < 1e-6f,
            "each lane sets its own plugin's parameter, by instance ID, not slot");
    require(effectAt(*rt, 0, 0).cutoff->get() == 0.5f, "a disabled lane, and a lane for a missing plugin, change nothing");
    require(rt->applyParameterAutomation(lanes, 250.0) == 0, "an unchanged value is not sent again");
    rt->applyParameterAutomation(lanes, 2000.0);
    require(effectAt(*rt, 0, 1).cutoff->get() == 1.0f, "after the last point the lane holds its value");

    // The FX are swapped round: the lanes follow the plugins.
    std::swap(racks[0].effects[0], racks[0].effects[1]);
    host.restore(racks, master, 48000);
    rt->applyParameterAutomation(lanes, 750.0);
    require(std::abs(effectAt(*rt, 0, 0).cutoff->get() - 0.75f) < 1e-6f && effectAt(*rt, 0, 1).cutoff->get() == 0.5f,
            "after a reorder the lane still drives the same plugin");

    // A plugin that crashes while receiving automation is switched off and reported, nothing else.
    std::vector<automation::ParameterLane> crashing{ramp(broken, "broken", 1000)};
    rt->applyParameterAutomation(crashing, 100.0);
    PluginFault fault;
    require(rt->racks[1].effects[0].faulted.get() && rt->faults.pop(fault) && fault.phase == PluginPhase::automation && fault.rack == 1,
            "a crash while receiving automation switches the plugin off");
    require(rt->applyParameterAutomation(crashing, 200.0) == 0, "and it is not called again");
    std::puts("PASS: lanes drive plugin parameters without allocating, through reorders and crashes");
}

// Through the engine: a one-bar Song with a lane from 0 to 1 sweeps the parameter while it plays.
static void engine() {
    auto engine = std::make_unique<ToneEngine>();
    engine->prepare(48000);
    PluginHost host(*engine->plugins, {}, factory());
    std::vector<PluginRackState> racks(1); PluginRackState master;
    racks[0].effects = {effect("swept")};
    host.restore(racks, master, 48000);
    auto& swept = effectAt(*engine->plugins, 0, 0);
    auto song = std::make_unique<ArrangementPlayback>();
    song->timeline.setLengthBars(1);
    song->timeline.parameterLanes.push_back(ramp(racks[0].effects[0].instanceId, "cutoff", timeline::ticksPerBar));
    require(engine->arrangementMailbox.publish(*song), "the song is accepted");
    std::array<float, 512> left{}, right{};
    const auto render = [&](int blocks) {
        float* outputs[]{left.data(), right.data()};
        for (int b = 0; b < blocks; ++b) engine->audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
    };
    engine->patternMode.store(2);
    render(4);
    require(swept.cutoff->get() == 0.5f, "stopped, the parameter keeps its own value");
    engine->playing.store(true);
    // One bar at the default tempo; render a quarter of it and check where the sweep is.
    const double barSeconds = 4.0 * 60.0 / engine->bpm.load();
    const int quarter = static_cast<int>(barSeconds * 48000.0 / 4.0 / 512.0);
    float previous = -1.0f; bool rising = true;
    for (int b = 0; b < quarter; ++b) { render(1); rising = rising && swept.cutoff->get() >= previous; previous = swept.cutoff->get(); }
    const float expected = static_cast<float>(quarter * 512) / static_cast<float>(barSeconds * 48000.0);
    require(rising && expected > 0.2f && std::abs(swept.cutoff->get() - expected) < 0.05f,
            "while the song plays the parameter follows the lane, a block at a time");
    engine->playing.store(false);
    render(2);
    const float stopped = swept.cutoff->get();
    render(4);
    require(swept.cutoff->get() == stopped, "stopping leaves the last value");
    std::puts("PASS: a Song lane sweeps its plugin parameter while the song plays");
}

static juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p, bool right = false) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p,
        juce::ModifierKeys(right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier),
        1.0f, 0, 0, 0, 0, &c, &c, now, p, now, 1, false);
}
// The automation editor lists the chosen track's plugin parameters, one submenu per plugin, and draws their lanes.
static void editor() {
    ToneEngine engine;
    ProjectController c(engine);
    c.pushDocumentToEngine();
    TrackAutomationEditor editor(c);
    editor.setSize(900, 180);
    editor.pluginParameters = [](int track) {
        std::vector<TrackAutomationEditor::PluginParameter> list;
        if (track == 0) {
            list.push_back({{42, "cutoff"}, "FX 1: Filter", "Cutoff"});
            list.push_back({{42, "mix"}, "FX 1: Filter", "Mix"});
            list.push_back({{77, "decay"}, "FX 2: Reverb", "Decay"});
        }
        return list;
    };
    editor.refresh();
    juce::ComboBox* parameter = nullptr; juce::ComboBox* track = nullptr; juce::TextButton* read = nullptr;
    for (auto* child : editor.getChildren()) {
        if (child->getName() == "Automation parameter") parameter = dynamic_cast<juce::ComboBox*>(child);
        if (child->getName() == "Automation track") track = dynamic_cast<juce::ComboBox*>(child);
        if (child->getName() == "Automation read") read = dynamic_cast<juce::TextButton*>(child);
    }
    require(parameter != nullptr && track != nullptr && read != nullptr, "the editor's controls");
    juce::StringArray menus;
    for (juce::PopupMenu::MenuItemIterator it(*parameter->getRootMenu()); it.next();) if (it.getItem().subMenu) menus.add(it.getItem().text);
    require(menus == juce::StringArray({"FX 1: Filter", "FX 2: Reverb"}) && parameter->getItemText(parameter->getNumItems() - 1) == "Decay",
            "each plugin's parameters are offered in a submenu of their own");
    parameter->setSelectedId(TrackAutomationEditor::pluginBase + 1, juce::sendNotificationSync);
    editor.mouseDown(mouse(editor, {450, 42}));
    editor.mouseUp(mouse(editor, {450, 42}));
    const auto& lanes = c.timeline().parameterLanes;
    require(lanes.size() == 1 && lanes[0].target == automation::ParameterTarget{42, "mix"} && lanes[0].points.size() == 1
            && lanes[0].points[0].value == 1.0f && c.timeline().automationLanes.empty(),
            "drawing at the top makes a plugin lane point at the parameter's maximum, not a track lane");
    editor.mouseDown(mouse(editor, {450, 42}, true));
    editor.mouseUp(mouse(editor, {450, 42}, true));
    require(c.timeline().parameterLanes[0].points.empty(), "right-clicking the point deletes it");
    require(c.undo() && c.timeline().parameterLanes[0].points.size() == 1, "which undoes in one step");
    read->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    require(!c.timeline().parameterLanes[0].enabled, "Read switches the plugin lane off");
    editor.refresh();
    require(parameter->getSelectedId() == TrackAutomationEditor::pluginBase + 1, "the parameter stays chosen across a refresh");
    track->setSelectedId(2, juce::sendNotificationSync);
    require(parameter->getSelectedId() == 1 && parameter->getNumItems() == 2 + 4, "a track without plugins offers its own lanes only");
    std::puts("PASS: the automation editor offers and draws plugin parameter lanes");
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    lookups();
    runtime();
    engine();
    editor();
    return 0;
}
