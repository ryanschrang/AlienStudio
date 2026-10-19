#pragma once
#include "PluginRuntime.h"
#include "PluginState.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace alien {
inline void addHostingFormats(juce::AudioPluginFormatManager& manager) {
   #if JUCE_PLUGINHOST_VST3
    manager.addFormat(new juce::VST3PluginFormat());
   #endif
}
// The main executable is also the isolated scanner. It exits without opening
// an audio device or main window. One explicit bundle is scanned per child.
inline int scanPluginBundle(const juce::String& path, const juce::File& output) {
    juce::AudioPluginFormatManager formats; addHostingFormats(formats);
    juce::KnownPluginList list;
    for (auto* format : formats.getFormats()) {
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions,path);
        for (const auto* description : descriptions) list.addType(*description);
    }
    auto xml = list.createXml();
    return xml && xml->writeTo(output) && list.getNumTypes() > 0 ? 0 : 1;
}
inline std::unique_ptr<juce::AudioPluginInstance> instantiatePlugin(
    const PluginState& state, bool instrument, double rate, juce::String& error) {
    juce::PluginDescription description;
    auto xml = juce::parseXML(state.descriptionXml);
    if (!xml || !description.loadFromXml(*xml) || description.pluginFormatName != "VST3") { error="Invalid VST3 description"; return {}; }
    if (description.isInstrument != instrument) { error="Choose an instrument for the instrument slot, or an effect for FX"; return {}; }
    juce::AudioPluginFormatManager formats; addHostingFormats(formats);
    auto plugin = formats.createPluginInstance(description,rate,PluginRuntime::blockSize,error);
    if (!plugin) return {};
    plugin->enableAllBuses();
    auto layout = plugin->getBusesLayout();
    for (int i=1;i<layout.inputBuses.size();++i) layout.inputBuses.getReference(i)=juce::AudioChannelSet::disabled();
    for (int i=1;i<layout.outputBuses.size();++i) layout.outputBuses.getReference(i)=juce::AudioChannelSet::disabled();
    if (!layout.inputBuses.isEmpty()) layout.inputBuses.getReference(0)=instrument ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::stereo();
    if (!layout.outputBuses.isEmpty()) layout.outputBuses.getReference(0)=juce::AudioChannelSet::stereo();
    if (!plugin->setBusesLayout(layout) || plugin->getTotalNumOutputChannels()!=2 || plugin->getTotalNumInputChannels()!=(instrument?0:2)) {
        error="This first host supports stereo-output instruments and stereo FX without sidechains"; return {};
    }
    juce::MemoryBlock bytes;
    if (state.stateBase64.isNotEmpty()) {
        if (!bytes.fromBase64Encoding(state.stateBase64)) { error="Invalid plugin state data"; return {}; }
        plugin->setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize()));
    }
    if (plugin->getTotalNumOutputChannels()!=2 || plugin->getTotalNumInputChannels()!=(instrument?0:2)) { error="Plugin state requested an unsupported bus layout"; return {}; }
    if (plugin->getLatencySamples() > PluginRuntime::maxDelay) { error="Plugin latency exceeds the 96,000-sample host limit"; return {}; }
    error.clear(); return plugin;
}

class PluginHost : private juce::AudioProcessorListener {
public:
    explicit PluginHost(PluginRuntime& engineRuntime) : runtime(engineRuntime) {}
    ~PluginHost() { editors.clear(); clearListeners(); }
    struct EditorWindow final : juce::DocumentWindow {
        EditorWindow(juce::AudioProcessor& processor) : juce::DocumentWindow(processor.getName(),juce::Colour(0xff17181c),juce::DocumentWindow::closeButton) {
            setUsingNativeTitleBar(true);
            auto* editor = processor.createEditorIfNeeded();
            if (!editor) editor = new juce::GenericAudioProcessorEditor(processor);
            setContentOwned(editor,true); setResizable(editor->isResizable(),false);
            centreWithSize(getWidth(),getHeight()); setVisible(true);
        }
        void closeButtonPressed() override { setVisible(false); }
    };
    // Caller detaches audio first. Missing/unsupported instances stay as saved
    // placeholders; never silently substitute another sound or erase state.
    juce::String restore(const std::vector<PluginRackState>& tracks, const PluginRackState& master, double rate) {
        editors.clear(); clearListeners();
        juce::String errors;
        for (int t=0;t<=maxSessionTracks;++t) {
            auto& rack=runtime.racks[t];
            PluginRuntime::release(rack.instrument);for(auto& slot:rack.effects)PluginRuntime::release(slot);
            rack={};
            const PluginRackState empty;
            const auto& state=t==maxSessionTracks?master:t<static_cast<int>(tracks.size())?tracks[t]:empty;
            rack.instrumentAssigned = !state.instrument.descriptionXml.isEmpty();
            if (t != maxSessionTracks) loadSlot(rack.instrument,state.instrument,true,rate,errors);
            rack.effectCount=static_cast<int>(state.effects.size());
            for (int i=0;i<rack.effectCount;++i) loadSlot(rack.effects[i],state.effects[i],false,rate,errors);
            int latency = rack.instrument.processor ? rack.instrument.processor->getLatencySamples() : 0;
            for (int i=0;i<rack.effectCount;++i) if (rack.effects[i].processor) latency += rack.effects[i].processor->getLatencySamples();
            if (latency > PluginRuntime::maxDelay) {
                errors += "Rack latency exceeds 96,000 samples; processors left unloaded\n";
                if (rack.instrument.processor) rack.instrument.processor->removeListener(this);
                for (auto& slot:rack.effects) if(slot.processor) slot.processor->removeListener(this);
                PluginRuntime::release(rack.instrument);for(auto& slot:rack.effects)PluginRuntime::release(slot);
                rack.instrument.processor.reset(); for(auto& slot:rack.effects)slot.processor.reset();
            }
        }
        return errors;
    }
    void snapshot(std::vector<PluginRackState>& tracks, PluginRackState& master) {
        for (int t=0;t<=maxSessionTracks;++t) {
            if (t!=maxSessionTracks && t>=static_cast<int>(tracks.size())) continue;
            auto& state=t==maxSessionTracks?master:tracks[t]; const auto& rack=runtime.racks[t];
            capture(rack.instrument,state.instrument);
            for (std::size_t i=0;i<state.effects.size();++i) capture(rack.effects[i],state.effects[i]);
        }
    }
    void openEditor(int track,int slot) {
        auto& rack=runtime.racks[track]; auto* processor=slot<0?rack.instrument.processor.get():rack.effects[slot].processor.get();
        if (!processor) return;
        // One editor per processor. Closing hides it; rack restore destroys it
        // before the processor, avoiding editor/instance lifetime races.
        for (auto& e:editors) if (e.first==processor) { e.second->setVisible(true); e.second->toFront(true); return; }
        editors.emplace_back(processor,std::make_unique<EditorWindow>(*processor));
    }
    std::atomic<bool> changed{};
private:
    void clearListeners() {
        for (auto& rack:runtime.racks) {
            if(rack.instrument.processor) rack.instrument.processor->removeListener(this);
            for(auto& slot:rack.effects) if(slot.processor) slot.processor->removeListener(this);
        }
    }
    void loadSlot(PluginRuntime::Slot& slot,const PluginState& state,bool instrument,double rate,juce::String& errors) {
        if(state.descriptionXml.isEmpty()) return;
        juce::String error; slot.processor=instantiatePlugin(state,instrument,rate,error); slot.bypass=state.bypass;
        if (!slot.processor) { errors+=error+"\n"; return; }
        runtime.prepareSlot(slot); slot.processor->addListener(this);
    }
    static void capture(const PluginRuntime::Slot& slot,PluginState& state) {
        if (!slot.processor) return;
        juce::MemoryBlock bytes; slot.processor->getStateInformation(bytes);
        state.stateBase64=bytes.toBase64Encoding();
    }
    void audioProcessorParameterChanged(juce::AudioProcessor*,int,float) override { changed.store(true); }
    void audioProcessorChanged(juce::AudioProcessor*,const juce::AudioProcessorListener::ChangeDetails& details) override {
        if (details.parameterInfoChanged || details.programChanged || details.nonParameterStateChanged) changed.store(true);
    }
    PluginRuntime& runtime;
    std::vector<std::pair<juce::AudioProcessor*,std::unique_ptr<EditorWindow>>> editors;
};
}
