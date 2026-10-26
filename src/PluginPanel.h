#pragma once
#include "PluginState.h"
#include "SessionCapacity.h"
#include "StockEffects.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>

namespace alien {
// What the Scan menu can ask for.
enum class ScanCommand { standardFolders, folder, singleFile, retryFailed, rescanAll, openScanLog, openProblemLog, recheckBlocked, togglePreflight };

class PluginPanel final : public juce::Component {
public:
    // Choice ids: stock effects are stockBase + catalog index; scanned VST3 plugins are index + 1.
    static constexpr int stockBase = 1000;
    PluginPanel() {
        for (auto* c:std::array<juce::Component*,14>{&target,&available,&slot,&liveInput,&scan,&cancelScan,&addInstrument,&addEffect,&open,&bypass,&up,&down,&remove,&status}) addAndMakeVisible(c);
        cancelScan.setVisible(false);
        scan.setButtonText("Scan VST3..."); cancelScan.setButtonText("Cancel scan"); addInstrument.setButtonText("Use instrument"); addEffect.setButtonText("Add FX");
        open.setButtonText("Open editor"); bypass.setButtonText("Bypass / enable"); up.setButtonText("Move FX up"); down.setButtonText("Move FX down"); remove.setButtonText("Remove");
        available.setTextWhenNothingSelected("Choose a stock effect or a scanned VST3 plugin");
        target.setName("Plugin destination track, return or master"); available.setName("Stock effects and scanned VST3 plugins"); slot.setName("Instrument or FX slot");
        liveInput.setName("Live MIDI destination");
        liveInput.setTooltip("Hardware MIDI, typing keyboard and on-screen piano. Recording uses its armed track.");
        liveInput.onChange=[this]{if(onLiveInput)onLiveInput(liveInput.getSelectedId()-2);};
        target.onChange=[this]{if(onTarget)onTarget(target.getSelectedId()-1);};
        scan.onClick=[this]{showScanMenu();}; cancelScan.onClick=[this]{if(onCancelScan)onCancelScan();};
        addInstrument.onClick=[this]{if(onAdd)onAdd(true,available.getSelectedId());};
        addEffect.onClick=[this]{if(onAdd)onAdd(false,available.getSelectedId());};
        open.onClick=[this]{action(0);}; bypass.onClick=[this]{action(1);}; remove.onClick=[this]{action(2);}; up.onClick=[this]{action(3);}; down.onClick=[this]{action(4);};
        setStatus("Pick a destination and add one of Alien Studio's stock effects (EQ, compressor, saturator, reverb, delay, limiter, filter), or scan a 64-bit .vst3 bundle for an instrument or FX. Choose a Live MIDI destination below to play an instrument with your MIDI controller or piano keyboard.");
    }
    std::function<void(ScanCommand)> onScanCommand;
    std::function<void()> onCancelScan;
    // The text to show beside a slot that has a problem (for example "FAILED - switched off"); empty when it is fine.
    // Arguments: destination (a track index, maxSessionTracks for Master FX, or a return's rack index after it) and slot (-1 instrument, 0.. FX).
    std::function<juce::String(int,int)> health;
    std::function<void(int)> onTarget;
    std::function<void(int)> onLiveInput;
    void refreshLiveInput(const juce::StringArray& names, int selected) {
        liveInput.clear(juce::dontSendNotification);
        liveInput.addItem("Live MIDI: standalone synth",1);
        for(int i=0;i<names.size();++i) liveInput.addItem("Live MIDI: "+names[i],i+2);
        liveInput.setSelectedId(selected>=0 && selected<names.size()?selected+2:1,juce::dontSendNotification);
    }
    std::function<void(bool,int)> onAdd;
    std::function<void(int,int)> onAction;
    int destination() const { return target.getSelectedId()-1; }
    // busNames: one per group bus slot, empty for a slot that is not in use.
    void refresh(const juce::StringArray& names, const PluginRackState& rack, const juce::KnownPluginList& list,
                 const std::array<juce::String,maxBuses>& busNames = {}) {
        const int t=target.getSelectedId(); target.clear(juce::dontSendNotification);
        for(int i=0;i<names.size();++i)target.addItem(names[i],i+1);
        // Ids are destination + 1: tracks, then Master FX (maxSessionTracks), then the returns right after it, then the buses.
        constexpr int busFirstId=maxSessionTracks+2+maxReturns;
        for(int b=0;b<maxBuses;++b)if(busNames[static_cast<std::size_t>(b)].isNotEmpty())target.addItem(busNames[static_cast<std::size_t>(b)]+" FX (bus)",busFirstId+b);
        for(int r=0;r<maxReturns;++r)target.addItem("Return "+juce::String::charToString(static_cast<juce::juce_wchar>('A'+r))+" FX",maxSessionTracks+2+r);
        target.addItem("Master FX",maxSessionTracks+1);
        target.setSelectedId(t>maxSessionTracks&&target.indexOfItemId(t)>=0?t:t>maxSessionTracks?maxSessionTracks+1:juce::jlimit(1,names.size(),t),juce::dontSendNotification);
        const int selectedPlugin=available.getSelectedId(); available.clear(juce::dontSendNotification);
        for(std::size_t i=0;i<stock::catalog.size();++i)available.addItem(juce::String(stock::catalog[i].name)+" [stock FX]",stockBase+static_cast<int>(i));
        auto descriptions=list.getTypes(); if(!descriptions.isEmpty())available.addSeparator();
        for(int i=0;i<descriptions.size();++i)available.addItem(descriptions[i].name+(descriptions[i].isInstrument?" [instrument]":" [FX]"),i+1);
        available.setSelectedId(available.indexOfItemId(selectedPlugin)>=0?selectedPlugin:stockBase,juce::dontSendNotification);
        const int s=slot.getSelectedId(); slot.clear(juce::dontSendNotification);
        const int where=destination();
        // The master and the returns are effects only: no instrument row.
        const bool bus=where>=maxSessionTracks;
        if(!bus)slot.addItem("Instrument: "+name(rack.instrument)+problem(where,-1),1);
        for(std::size_t i=0;i<rack.effects.size();++i)slot.addItem("FX "+juce::String(static_cast<int>(i+1))+": "+name(rack.effects[i])+problem(where,static_cast<int>(i)),static_cast<int>(i+2));
        // A bus has no instrument row, so its first effect is id 2; a kept id of 1 would match no item.
        slot.setSelectedId(s>=(bus?2:1)&&s<=static_cast<int>(rack.effects.size()+1)?s:bus?(rack.effects.empty()?0:2):1,juce::dontSendNotification);
        addInstrument.setEnabled(!bus);
    }
    // While a scan runs, Scan is disabled and Cancel is offered.
    void setScanning(bool running){scanning=running;scan.setEnabled(!running);cancelScan.setVisible(running);resized();}
    bool isScanning() const{return scanning;}
    void setPreflightEnabled(bool on){preflightEnabled=on;}
    bool preflightIsEnabled() const{return preflightEnabled;}
    // Runs a Scan menu command (the menu items call this; tests do too).
    void perform(ScanCommand command){
        if(command==ScanCommand::togglePreflight)preflightEnabled=!preflightEnabled;
        if(onScanCommand)onScanCommand(command);
    }
    void showScanMenu(){
        juce::PopupMenu menu;
        menu.addItem(1,"Scan the standard VST3 folders",!scanning);
        menu.addItem(2,"Scan a folder (including subfolders)...",!scanning);
        menu.addItem(3,"Add a single plugin file...",!scanning);
        menu.addSeparator();
        menu.addItem(4,"Retry plugins that failed",!scanning);
        menu.addItem(5,"Rescan everything",!scanning);
        menu.addItem(6,"Re-check plugins that were blocked",true);
        menu.addSeparator();
        menu.addItem(7,"Check plugins before loading them (recommended)",true,preflightEnabled);
        menu.addItem(8,"Show the last scan log",true);
        menu.addItem(9,"Show the plugin problem log",true);
        static constexpr ScanCommand commands[]={ScanCommand::standardFolders,ScanCommand::folder,ScanCommand::singleFile,ScanCommand::retryFailed,ScanCommand::rescanAll,ScanCommand::recheckBlocked,ScanCommand::togglePreflight,ScanCommand::openScanLog,ScanCommand::openProblemLog};
        juce::Component::SafePointer<PluginPanel> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scan),[safe](int id){if(safe&&id>=1&&id<=9)safe->perform(commands[id-1]);});
    }
    void setStatus(const juce::String& text){status.setText(text,juce::dontSendNotification);status.setTooltip(text);}
    void resized() override {
        auto r=getLocalBounds().reduced(20);auto row=r.removeFromTop(32);
        target.setBounds(row.removeFromLeft(190)); row.removeFromLeft(12); scan.setBounds(row.removeFromLeft(140)); row.removeFromLeft(8); cancelScan.setBounds(row.removeFromLeft(130));
        r.removeFromTop(12);liveInput.setBounds(r.removeFromTop(30));
        r.removeFromTop(12);available.setBounds(r.removeFromTop(30));r.removeFromTop(12);row=r.removeFromTop(32);
        addInstrument.setBounds(row.removeFromLeft(150));row.removeFromLeft(10);addEffect.setBounds(row.removeFromLeft(120));
        r.removeFromTop(20);slot.setBounds(r.removeFromTop(30));r.removeFromTop(12);row=r.removeFromTop(32);
        for(auto* b:{&open,&bypass,&up,&down,&remove}){b->setBounds(row.removeFromLeft(135));row.removeFromLeft(8);}
        r.removeFromTop(16);status.setBounds(r.removeFromTop(110));
    }
private:
    juce::String problem(int where,int index) const{if(!health)return {};const auto text=health(where,index);return text.isEmpty()?juce::String():"  ["+text+"]";}
    static juce::String name(const PluginState& p){auto xml=juce::parseXML(p.descriptionXml);return (xml?xml->getStringAttribute("name"):"Empty")+(p.bypass?" [bypassed]":"");}
    void action(int op){if(onAction && slot.getSelectedId()>0)onAction(op,slot.getSelectedId()-2);}
    juce::ComboBox target,available,slot,liveInput;
    juce::TextButton scan,cancelScan,addInstrument,addEffect,open,bypass,up,down,remove;
    juce::Label status;
    bool scanning{},preflightEnabled{true};
};
}
