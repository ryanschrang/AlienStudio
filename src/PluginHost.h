#pragma once
#include "DiagnosticLog.h"
#include "PluginRuntime.h"
#include "PluginScan.h"
#include "PluginState.h"
#include "StockEffectEditors.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace alien {
// Stock effects are recreated from their identifier, with no plugin scan or file.
inline std::unique_ptr<juce::AudioProcessor> instantiateStock(
    const juce::PluginDescription& description, const PluginState& state, bool instrument, juce::String& error) {
    if (instrument) { error="Stock effects go in FX slots, not the instrument slot"; return {}; }
    auto effect = stock::create(description.fileOrIdentifier);
    if (!effect) { error="Unknown stock effect \""+description.fileOrIdentifier+"\" (saved by a newer Alien Studio?)"; return {}; }
    if (state.stateBase64.isNotEmpty()) {
        juce::MemoryBlock bytes;
        if (!bytes.fromBase64Encoding(state.stateBase64)) { error="Invalid plugin state data"; return {}; }
        effect->setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize()));
    }
    error.clear(); return effect;
}
// Creates the plugin object. A function, so tests can stand in for the real VST3 loader.
using PluginFactory = std::function<std::unique_ptr<juce::AudioProcessor>(const juce::PluginDescription&, double rate, int blockSize, juce::String& error)>;
inline std::unique_ptr<juce::AudioProcessor> createVst3Instance(const juce::PluginDescription& description, double rate, int blockSize, juce::String& error) {
    juce::AudioPluginFormatManager formats; addHostingFormats(formats);
    return formats.createPluginInstance(description,rate,blockSize,error);
}
namespace detail {
// Everything here runs code that belongs to the plugin, so the caller wraps it in a FaultGuard.
// True when the plugin accepted the host's stereo layout and its saved state.
inline bool configureVst3(juce::AudioProcessor& plugin, const PluginState& state, bool instrument, juce::String& error) {
    plugin.enableAllBuses();
    auto layout = plugin.getBusesLayout();
    for (int i=1;i<layout.inputBuses.size();++i) layout.inputBuses.getReference(i)=juce::AudioChannelSet::disabled();
    for (int i=1;i<layout.outputBuses.size();++i) layout.outputBuses.getReference(i)=juce::AudioChannelSet::disabled();
    if (!layout.inputBuses.isEmpty()) layout.inputBuses.getReference(0)=instrument ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::stereo();
    if (!layout.outputBuses.isEmpty()) layout.outputBuses.getReference(0)=juce::AudioChannelSet::stereo();
    if (!plugin.setBusesLayout(layout) || plugin.getTotalNumOutputChannels()!=2 || plugin.getTotalNumInputChannels()!=(instrument?0:2)) {
        error="This first host supports stereo-output instruments and stereo FX without sidechains"; return false;
    }
    juce::MemoryBlock bytes;
    if (state.stateBase64.isNotEmpty()) {
        if (!bytes.fromBase64Encoding(state.stateBase64)) { error="Invalid plugin state data"; return false; }
        plugin.setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize()));
    }
    if (plugin.getTotalNumOutputChannels()!=2 || plugin.getTotalNumInputChannels()!=(instrument?0:2)) { error="Plugin state requested an unsupported bus layout"; return false; }
    if (plugin.getLatencySamples() > PluginRuntime::maxDelay) { error="Plugin latency exceeds the 96,000-sample host limit"; return false; }
    return true;
}
} // namespace detail
// A plugin that crashes or throws while loading is reported and left unloaded; the saved
// description and state stay in the project. `faultOut`, if given, receives what went wrong.
inline std::unique_ptr<juce::AudioProcessor> instantiatePlugin(
    const PluginState& state, bool instrument, double rate, juce::String& error,
    const PluginFactory& factory = &createVst3Instance, fault::Fault* faultOut = nullptr) {
    juce::PluginDescription description;
    auto xml = juce::parseXML(state.descriptionXml);
    if (xml && description.loadFromXml(*xml) && stock::isStock(description)) return instantiateStock(description,state,instrument,error);
    if (!xml || !description.loadFromXml(*xml) || description.pluginFormatName != "VST3") { error="Invalid VST3 description"; return {}; }
    if (description.isInstrument != instrument) { error="Choose an instrument for the instrument slot, or an effect for FX"; return {}; }
    // From here on the plugin's code is running. The instance is held as a plain pointer: if it
    // faults half way through, no destructor may run on it.
    struct Attempt { juce::AudioProcessor* plugin{}; juce::String error; bool accepted{}; } attempt;
    fault::Fault fault;
    const bool completed = fault::guarded([&] {
        auto created = factory(description,rate,PluginRuntime::blockSize,attempt.error);
        if (!created) return;
        attempt.plugin = created.release();
        attempt.accepted = detail::configureVst3(*attempt.plugin,state,instrument,attempt.error);
    },fault);
    const auto name = description.name.isNotEmpty() ? description.name : juce::String("This plugin");
    if (!completed) {
        if (attempt.plugin != nullptr) { std::unique_ptr<juce::AudioProcessor> parked(attempt.plugin); PluginRuntime::quarantine(parked); }
        error = name + " crashed while loading (" + juce::String(fault::describe(fault)) + "), so it was not loaded. Its saved settings are kept.";
        if (faultOut != nullptr) *faultOut = fault;
        return {};
    }
    if (attempt.plugin == nullptr) { error = attempt.error.isNotEmpty() ? attempt.error : name + " could not be created"; return {}; }
    if (!attempt.accepted) {
        error = attempt.error;
        fault::Fault ignored;
        juce::AudioProcessor* const rejected = attempt.plugin;
        if (!fault::guarded([&]{ delete rejected; },ignored)) error += " (and it crashed while being unloaded)";
        return {};
    }
    error.clear(); return std::unique_ptr<juce::AudioProcessor>(attempt.plugin);
}

class PluginHost : private juce::AudioProcessorListener {
public:
    // logFile: where problems are written (empty = nowhere). factory: how VST3 plugins are made.
    explicit PluginHost(PluginRuntime& engineRuntime, juce::File logFile = {}, PluginFactory pluginFactory = &createVst3Instance)
        : runtime(engineRuntime), log(std::move(logFile)), factory(std::move(pluginFactory)) {}
    ~PluginHost() { closeEditors(); clearListeners(); }
    // Message thread: called with a readable description whenever a plugin is found to have failed.
    std::function<void(const juce::String&)> onProblem;
    // Message thread: asked before a VST3 plugin is loaded into the session. An empty answer means go ahead;
    // anything else is the reason it must not be (the slot stays in the project with its saved settings).
    std::function<juce::String(const juce::PluginDescription&)> preflight;

    struct EditorWindow final : juce::DocumentWindow {
        explicit EditorWindow(const juce::String& title) : juce::DocumentWindow(title,juce::Colour(0xff17181c),juce::DocumentWindow::closeButton) { setUsingNativeTitleBar(true); }
        void closeButtonPressed() override { setVisible(false); }
    };
    // Caller detaches audio first. Missing/unsupported instances stay as saved
    // placeholders; never silently substitute another sound or erase state.
    // A plugin that crashes while loading is a placeholder too; the others still load.
    juce::String restore(const std::vector<PluginRackState>& tracks, const PluginRackState& master, double rate) {
        return restore(tracks, master, ReturnRacks{}, rate);
    }
    juce::String restore(const std::vector<PluginRackState>& tracks, const PluginRackState& master, const ReturnRacks& returns, double rate) {
        closeEditors(); clearListeners();
        juce::String errors;
        loadProblems.clear();
        for (int t=0;t<PluginRuntime::rackCount;++t) {
            auto& rack=runtime.racks[static_cast<std::size_t>(t)];
            runtime.destroy(rack.instrument,t,-1);
            for (int i=0;i<static_cast<int>(rack.effects.size());++i) runtime.destroy(rack.effects[static_cast<std::size_t>(i)],t,i);
            rack={};
            const PluginRackState empty;
            const auto& state=PluginRuntime::isReturn(t)?returns[static_cast<std::size_t>(t-PluginRuntime::returnBase)]
                             :t==maxSessionTracks?master:t<static_cast<int>(tracks.size())?tracks[static_cast<std::size_t>(t)]:empty;
            // Only Session tracks have an instrument; the master and the returns are effects only.
            rack.instrumentAssigned = t < maxSessionTracks && !state.instrument.descriptionXml.isEmpty();
            if (t < maxSessionTracks) loadSlot(rack.instrument,state.instrument,true,rate,errors,t,-1);
            rack.effectCount=juce::jmin(static_cast<int>(state.effects.size()),static_cast<int>(rack.effects.size()));
            for (int i=0;i<rack.effectCount;++i) loadSlot(rack.effects[static_cast<std::size_t>(i)],state.effects[static_cast<std::size_t>(i)],false,rate,errors,t,i);
            int latency = rack.instrument.processor ? rack.instrument.processor->getLatencySamples() : 0;
            for (int i=0;i<rack.effectCount;++i) if (rack.effects[static_cast<std::size_t>(i)].processor) latency += rack.effects[static_cast<std::size_t>(i)].processor->getLatencySamples();
            if (latency > PluginRuntime::maxDelay) {
                errors += "Rack latency exceeds 96,000 samples; processors left unloaded\n";
                if (rack.instrument.processor) rack.instrument.processor->removeListener(this);
                for (auto& slot:rack.effects) if(slot.processor) slot.processor->removeListener(this);
                runtime.destroy(rack.instrument,t,-1);
                for (int i=0;i<static_cast<int>(rack.effects.size());++i) runtime.destroy(rack.effects[static_cast<std::size_t>(i)],t,i);
            }
        }
        brokenEditors.clear();   // those processors are gone (or parked), and their addresses may be reused
        drain(errors);           // a plugin that failed while preparing is part of this restore's result
        return errors;
    }
    // Copies each plugin's state into the rack states. A plugin that has failed keeps the state
    // saved before the failure: asking a broken plugin for its state would only break it more.
    void snapshot(std::vector<PluginRackState>& tracks, PluginRackState& master) { ReturnRacks unused; snapshot(tracks, master, unused); }
    void snapshot(std::vector<PluginRackState>& tracks, PluginRackState& master, ReturnRacks& returns) {
        for (int t=0;t<PluginRuntime::rackCount;++t) {
            if (t<maxSessionTracks && t>=static_cast<int>(tracks.size())) continue;
            auto& state=PluginRuntime::isReturn(t)?returns[static_cast<std::size_t>(t-PluginRuntime::returnBase)]
                       :t==maxSessionTracks?master:tracks[static_cast<std::size_t>(t)];
            auto& rack=runtime.racks[static_cast<std::size_t>(t)];
            capture(rack.instrument,state.instrument,t,-1);
            for (std::size_t i=0;i<state.effects.size() && i<rack.effects.size();++i) capture(rack.effects[i],state.effects[i],t,static_cast<int>(i));
        }
    }
    // Opens (or raises) a plugin's editor. Returns an empty string on success, otherwise a message
    // for the user; the failure is also logged. An editor that crashes while opening is contained.
    juce::String openEditor(int track,int slot) {
        if (track<0 || track>=PluginRuntime::rackCount || slot<-1 || slot>=static_cast<int>(runtime.racks[0].effects.size())) return {};
        auto& rack=runtime.racks[static_cast<std::size_t>(track)];
        auto& entry=slot<0?rack.instrument:rack.effects[static_cast<std::size_t>(slot)];
        auto* processor=entry.processor.get();
        if (!processor) return {};
        const auto label=slotLabel(entry,track,slot);
        if (entry.faulted.get()) return report("warn",label+" has failed and is switched off, so its editor cannot be opened. Remove it, or reload the plugins, to try it again.");
        if (brokenEditors.count(processor)!=0) return report("warn",label+"'s editor crashed earlier in this session and is not opened again. Restart Alien Studio to try it again.");
        // One editor per processor. Closing hides it; rack restore destroys it
        // before the processor, avoiding editor/instance lifetime races.
        for (auto& e:editors) if (e.first==processor) { e.second->setVisible(true); e.second->toFront(true); return {}; }
        auto window=std::make_unique<EditorWindow>(processor->getName());
        juce::AudioProcessorEditor* editor=nullptr; fault::Fault fault;
        const bool created=fault::guarded([&]{
            editor=processor->createEditorIfNeeded();
            if (editor==nullptr) editor=new juce::GenericAudioProcessorEditor(*processor);
        },fault);
        if (!created) { brokenEditors.insert(processor); return report("error",label+" failed while opening its editor ("+juce::String(fault::describe(fault))+"). Its audio is still running, but save your work: the plugin may be unstable."); }
        if (editor==nullptr) return report("error",label+" did not provide an editor.");
        const bool shown=fault::guarded([&]{
            window->setContentOwned(editor,true); window->setResizable(editor->isResizable(),false);
            window->centreWithSize(window->getWidth(),window->getHeight()); window->setVisible(true);
        },fault);
        if (!shown) {
            brokenEditors.insert(processor);
            window.release();   // half-built and attached to the plugin: leaving it alone is the only safe option
            return report("error",label+" failed while showing its editor ("+juce::String(fault::describe(fault))+"). Its audio is still running, but save your work: the plugin may be unstable.");
        }
        editors.emplace_back(processor,std::move(window));
        return {};
    }
    // Message thread, from the UI timer: reports any plugin the audio thread has switched off.
    void poll() {
        juce::String text; drain(text);
        if (text.isNotEmpty() && onProblem) onProblem(text.trim());
    }
    // Empty when the slot is fine, otherwise a short description for the plugin panel.
    juce::String health(int track,int slot) const {
        if (track<0 || track>=PluginRuntime::rackCount || slot<-1 || slot>=static_cast<int>(runtime.racks[0].effects.size())) return {};
        const auto& rack=runtime.racks[static_cast<std::size_t>(track)];
        const auto& entry=slot<0?rack.instrument:rack.effects[static_cast<std::size_t>(slot)];
        if (!entry.processor) {
            const auto found=loadProblems.find({track,slot});
            return found!=loadProblems.end() ? "NOT LOADED - "+found->second : juce::String();
        }
        if (entry.faulted.get()) return "FAILED - switched off";
        if (brokenEditors.count(entry.processor.get())!=0) return "editor crashed";
        return {};
    }
    // Message thread, with the device callback detached, before an offline snapshot.
    void closeEditorWindows() { closeEditors(); }
    const DiagnosticLog& diagnosticLog() const noexcept { return log; }
    std::atomic<bool> changed{};
private:
    void closeEditors() {
        for (auto& e:editors) {
            fault::Fault fault;
            if (!fault::guarded([&]{ e.second.reset(); },fault)) {
                // The editor's destructor crashed inside the plugin, so the plugin can no longer be trusted.
                if (const auto found=findSlot(e.first)) runtime.markFaulted(*found->slot,found->rack,found->index,PluginPhase::editor,fault);
                brokenEditors.insert(e.first);
            }
        }
        editors.clear();
    }
    struct Found { PluginRuntime::Slot* slot; int rack; int index; };
    std::optional<Found> findSlot(const juce::AudioProcessor* processor) {
        for (int t=0;t<PluginRuntime::rackCount;++t) {
            auto& rack=runtime.racks[static_cast<std::size_t>(t)];
            if (rack.instrument.processor.get()==processor) return Found{&rack.instrument,t,-1};
            for (int i=0;i<static_cast<int>(rack.effects.size());++i)
                if (rack.effects[static_cast<std::size_t>(i)].processor.get()==processor) return Found{&rack.effects[static_cast<std::size_t>(i)],t,i};
        }
        return std::nullopt;
    }
    static juce::String where(int rack,int index) {
        const juce::String place = rack<0 ? juce::String("unknown location") : rack==PluginRuntime::masterIndex ? juce::String("master")
            : PluginRuntime::isReturn(rack) ? "return "+juce::String::charToString(static_cast<juce::juce_wchar>('A'+rack-PluginRuntime::returnBase))
            : "track "+juce::String(rack+1);
        return place+(index<0?(rack<0?"":" instrument"):" FX "+juce::String(index+1));
    }
    static juce::String slotLabel(const PluginRuntime::Slot& slot,int rack,int index) {
        const auto name=slot.name.isNotEmpty()?slot.name:slot.processor?slot.processor->getName():juce::String("A plugin");
        return name+" ("+where(rack,index)+")";
    }
    juce::String report(const juce::String& level,const juce::String& message) { log.write(level,message); return message; }
    // Turns queued runtime faults into text, appends it to `text`, and logs each one.
    void drain(juce::String& text) {
        PluginFault f;
        while (runtime.faults.pop(f)) {
            const PluginRuntime::Slot* slot=nullptr;
            if (f.rack>=0 && f.rack<PluginRuntime::rackCount) {
                const auto& rack=runtime.racks[static_cast<std::size_t>(f.rack)];
                if (f.slot<0) slot=&rack.instrument; else if (f.slot<static_cast<int>(rack.effects.size())) slot=&rack.effects[static_cast<std::size_t>(f.slot)];
            }
            const auto name=slot && slot->name.isNotEmpty()?slot->name:juce::String("A plugin");
            juce::String line=name+" ("+where(f.rack,f.slot)+") "+juce::String(describe(f))+". ";
            if (f.phase==PluginPhase::process || f.phase==PluginPhase::prepare || f.phase==PluginPhase::query)
                line += f.slot<0 ? "It was switched off and its track is silent. " : "It was switched off and audio now bypasses it. ";
            line += "Its saved settings are kept; remove it or reload the plugins to try again.";
            const auto moduleName=juce::String(fault::moduleContaining(f.fault.instruction));
            if (moduleName.isNotEmpty() && !line.contains(moduleName)) line += " [module "+moduleName+"]";
            log.write("fault",line);
            text += line+"\n";
        }
        const auto dropped=runtime.faults.droppedCount();
        if (dropped!=reportedDrops) { log.write("warn",juce::String(static_cast<int>(dropped-reportedDrops))+" further plugin fault report(s) were dropped because too many arrived at once."); reportedDrops=dropped; }
    }
    void clearListeners() {
        for (auto& rack:runtime.racks) {
            if(rack.instrument.processor) rack.instrument.processor->removeListener(this);
            for(auto& slot:rack.effects) if(slot.processor) slot.processor->removeListener(this);
        }
    }
    void loadSlot(PluginRuntime::Slot& slot,const PluginState& state,bool instrument,double rate,juce::String& errors,int rack,int index) {
        if(state.descriptionXml.isEmpty()) return;
        slot.name=pluginDisplayName(state);
        juce::String error; fault::Fault fault;
        if (preflight) {
            juce::PluginDescription description;
            if (auto xml=juce::parseXML(state.descriptionXml); xml && description.loadFromXml(*xml) && description.pluginFormatName=="VST3") {
                const auto refused=preflight(description);
                if (refused.isNotEmpty()) {
                    slot.bypass=state.bypass;
                    errors+=refused+"\n"; loadProblems[{rack,index}]="blocked by the safety check";
                    log.write("fault",slot.name+" ("+where(rack,index)+"): "+refused);
                    return;
                }
            }
        }
        slot.processor=instantiatePlugin(state,instrument,rate,error,factory,&fault); slot.bypass=state.bypass;
        if (!slot.processor) {
            errors+=error+"\n";
            loadProblems[{rack,index}]=fault.occurred()?"crashed while loading":"could not be loaded";
            log.write(fault.occurred()?"fault":"warn",slot.name+" ("+where(rack,index)+"): "+error);
            return;
        }
        runtime.prepareSlot(slot,rack,index); slot.processor->addListener(this);
    }
    void capture(PluginRuntime::Slot& slot,PluginState& state,int rack,int index) {
        if (!slot.processor || slot.faulted.get()) return;
        juce::MemoryBlock bytes; fault::Fault fault;
        if (!fault::guarded([&]{ slot.processor->getStateInformation(bytes); },fault)) { runtime.markFaulted(slot,rack,index,PluginPhase::saveState,fault); return; }
        state.stateBase64=bytes.toBase64Encoding();
    }
    void audioProcessorParameterChanged(juce::AudioProcessor*,int,float) override { changed.store(true); }
    void audioProcessorChanged(juce::AudioProcessor*,const juce::AudioProcessorListener::ChangeDetails& details) override {
        if (details.parameterInfoChanged || details.programChanged || details.nonParameterStateChanged) changed.store(true);
    }
    PluginRuntime& runtime;
    DiagnosticLog log;
    PluginFactory factory;
    std::vector<std::pair<juce::AudioProcessor*,std::unique_ptr<EditorWindow>>> editors;
    std::set<const juce::AudioProcessor*> brokenEditors;   // editors that crashed: never reopened this session
    std::map<std::pair<int,int>,juce::String> loadProblems;   // (rack, slot) -> why nothing is loaded there
    std::uint32_t reportedDrops{0};
};
}
