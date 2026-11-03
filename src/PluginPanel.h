#pragma once
#include "PluginBrowser.h"
#include "PluginState.h"
#include "SessionCapacity.h"
#include "StockEffects.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <vector>

namespace alien {
// What the Scan menu can ask for.
enum class ScanCommand { standardFolders, folder, singleFile, retryFailed, rescanAll, openScanLog, openProblemLog, recheckBlocked, togglePreflight,
                         toggleStartupScan };

class PluginPanel final : public juce::Component {
public:
    // Choice ids: stock effects are stockBase + catalog index; scanned VST3 plugins are index + 1.
    static constexpr int stockBase = PluginBrowser::stockBase;
    PluginPanel() {
        for (auto* c:std::array<juce::Component*,15>{&target,&browser,&slot,&liveInput,&scan,&cancelScan,&addInstrument,&addEffect,&open,&bypass,&sidechain,&up,&down,&remove,&status}) addAndMakeVisible(c);
        cancelScan.setVisible(false);
        scan.setButtonText("Scan VST3..."); cancelScan.setButtonText("Cancel scan"); addInstrument.setButtonText("Use instrument"); addEffect.setButtonText("Add FX");
        open.setButtonText("Open editor"); bypass.setButtonText("Bypass / enable"); up.setButtonText("Move FX up"); down.setButtonText("Move FX down"); remove.setButtonText("Remove");
        target.setName("Plugin destination track, return or master"); slot.setName("Instrument or FX slot");
        liveInput.setName("Live MIDI destination");
        liveInput.setTooltip("Hardware MIDI, typing keyboard and on-screen piano. Recording uses its armed track.");
        liveInput.onChange=[this]{
            const int id=liveInput.getSelectedId();
            if(id>=liveRackBase){if(onLiveRackInput)onLiveRackInput(id-liveRackBase);}
            else if(onLiveInput)onLiveInput(id-2);
        };
        target.onChange=[this]{updateAddButtons();if(onTarget)onTarget(target.getSelectedId()-1);};
        scan.onClick=[this]{showScanMenu();}; cancelScan.onClick=[this]{if(onCancelScan)onCancelScan();};
        addInstrument.onClick=[this]{if(onAdd)onAdd(true,browser.selectedChoice());};
        addEffect.onClick=[this]{if(onAdd)onAdd(false,browser.selectedChoice());};
        // Double-click or Enter in the browser adds the row where it belongs, if the destination takes it.
        browser.onChoose=[this](int choice,bool instrument){
            if(!(instrument?addInstrument:addEffect).isEnabled()){
                const auto* chosen=browser.selected();
                setStatus(chosen!=nullptr && chosen->blocked?chosen->name+" is blocked because it crashed earlier. Allow it again from Scan VST3 > Blocked plugins."
                         :"Master FX, returns and buses take effects only. Choose a track to use an instrument.");
                return;
            }
            if(onAdd)onAdd(instrument,choice);
        };
        browser.onSelectionChanged=[this]{updateAddButtons();};
        open.onClick=[this]{action(0);}; bypass.onClick=[this]{action(1);}; remove.onClick=[this]{action(2);}; up.onClick=[this]{action(3);}; down.onClick=[this]{action(4);};
        sidechain.setButtonText("Sidechain..."); sidechain.setName("Sidechain source");
        sidechain.setTooltip("Key this effect from another track: a compressor then ducks this sound whenever that track plays (for example a bass ducking to the kick).");
        sidechain.onClick=[this]{showSidechainMenu();};
        slot.onChange=[this]{updateSidechainButton();};
        setStatus("Pick a destination and add one of Alien Studio's stock effects (EQ, compressor, saturator, reverb, delay, limiter, filter), or scan a 64-bit .vst3 bundle for an instrument or FX. Choose a Live MIDI destination below to play an instrument with your MIDI controller or piano keyboard.");
    }
    std::function<void(ScanCommand)> onScanCommand;
    std::function<void()> onCancelScan;
    // The text to show beside a slot that has a problem (for example "FAILED - switched off"); empty when it is fine.
    // Arguments: destination (a track index, maxSessionTracks for Master FX, or a return's rack index after it) and slot (-1 instrument, 0.. FX).
    std::function<juce::String(int,int)> health;
    std::function<void(int)> onTarget;
    std::function<void(int)> onLiveInput;       // a track index, or -1 for the standalone synth
    std::function<void(int)> onLiveRackInput;   // a Channel Rack channel whose plugin instrument plays live (T1-09)
    static constexpr int liveRackBase=1000;     // destination ids of rack channels (plus the channel)
    // `rackChannels` are the rack channels played by a plugin instrument, with their names; `selectedRack` wins over
    // `selected` when it is one of them.
    void refreshLiveInput(const juce::StringArray& names, int selected,
                          const std::vector<std::pair<int,juce::String>>& rackChannels={}, int selectedRack=-1) {
        liveInput.clear(juce::dontSendNotification);
        liveInput.addItem("Live MIDI: standalone synth",1);
        for(int i=0;i<names.size();++i) liveInput.addItem("Live MIDI: "+names[i],i+2);
        bool rackChosen=false;
        for(const auto& [channel,name]:rackChannels) {
            liveInput.addItem("Live MIDI: Channel Rack "+juce::String(channel+1)+": "+name,liveRackBase+channel);
            rackChosen=rackChosen||channel==selectedRack;
        }
        liveInput.setSelectedId(rackChosen?liveRackBase+selectedRack:selected>=0 && selected<names.size()?selected+2:1,juce::dontSendNotification);
    }
    std::function<void(bool,int)> onAdd;
    // Whether a scanned plugin is blocked after crashing while hosted (T1-09); the browser marks it and it cannot be added.
    std::function<bool(const juce::PluginDescription&)> isBlocked;
    PluginBrowser& pluginBrowser() noexcept { return browser; }
    std::function<void(int,int)> onAction;
    // Whether the effect in (destination, effect index) has a sidechain input; and the choice of key:
    // (effect index, what kind of key, its track, bus or return slot, or -1 for none).
    std::function<bool(int,int)> hasSidechainInput;
    std::function<void(int,KeySource,int)> onSidechain;
    // Menu ids for the keys: Off is 1; then one block of ids per kind of key, by track, bus or return slot.
    static constexpr int keyTrackBase = 2, keyPostBase = 200, keyBusBase = 300, keyReturnBase = 400;
    // Where the shown rack runs in the mix (destinations: tracks, the master, the returns, then the buses).
    RackStage stage() const {
        const int where = destination();
        return where < maxSessionTracks ? RackStage::track : where == maxSessionTracks ? RackStage::master
             : where <= maxSessionTracks + maxReturns ? RackStage::returnTrack : RackStage::bus;
    }
    int destination() const { return target.getSelectedId()-1; }
    // Returns A-D, as in a new project.
    static std::array<juce::String,maxReturns> defaultReturnNames() {
        std::array<juce::String,maxReturns> names;
        for(int r=0;r<defaultReturns;++r)names[static_cast<std::size_t>(r)]="Return "+juce::String::charToString(static_cast<juce::juce_wchar>('A'+r));
        return names;
    }
    // busNames and returnNames: one per slot, empty for a slot that is not in use.
    void refresh(const juce::StringArray& names, const PluginRackState& rack, const juce::KnownPluginList& list,
                 const std::array<juce::String,maxBuses>& busNames = {},
                 const std::array<juce::String,maxReturns>& returnNames = defaultReturnNames()) {
        const int t=target.getSelectedId(); target.clear(juce::dontSendNotification);
        for(int i=0;i<names.size();++i)target.addItem(names[i],i+1);
        // Ids are destination + 1: tracks, then Master FX (maxSessionTracks), then the returns right after it, then the buses.
        constexpr int busFirstId=maxSessionTracks+2+maxReturns;
        for(int b=0;b<maxBuses;++b)if(busNames[static_cast<std::size_t>(b)].isNotEmpty())target.addItem(busNames[static_cast<std::size_t>(b)]+" FX (bus)",busFirstId+b);
        for(int r=0;r<maxReturns;++r){
            const auto& returnName=returnNames[static_cast<std::size_t>(r)];
            if(returnName.isEmpty())continue;
            const auto letter=juce::String::charToString(static_cast<juce::juce_wchar>('A'+r));
            target.addItem(returnName=="Return "+letter?returnName+" FX":returnName+" FX (return "+letter+")",maxSessionTracks+2+r);
        }
        target.addItem("Master FX",maxSessionTracks+1);
        target.setSelectedId(t>maxSessionTracks&&target.indexOfItemId(t)>=0?t:t>maxSessionTracks?maxSessionTracks+1:juce::jlimit(1,names.size(),t),juce::dontSendNotification);
        browser.refresh(list,isBlocked);
        const int s=slot.getSelectedId(); slot.clear(juce::dontSendNotification);
        const int where=destination();
        // The master and the returns are effects only: no instrument row.
        const bool bus=where>=maxSessionTracks;
        if(!bus)slot.addItem("Instrument: "+name(rack.instrument)+problem(where,-1),1);
        for(std::size_t i=0;i<rack.effects.size();++i)slot.addItem("FX "+juce::String(static_cast<int>(i+1))+": "+name(rack.effects[i])+problem(where,static_cast<int>(i)),static_cast<int>(i+2));
        // A bus has no instrument row, so its first effect is id 2; a kept id of 1 would match no item.
        slot.setSelectedId(s>=(bus?2:1)&&s<=static_cast<int>(rack.effects.size()+1)?s:bus?(rack.effects.empty()?0:2):1,juce::dontSendNotification);
        updateAddButtons();
        trackNames=names; keyBusNames=busNames; keyReturnNames=returnNames;
        keyOf.clear(); for(const auto& effect:rack.effects) keyOf.push_back({effect.sidechainFrom,effect.sidechain});
        updateSidechainButton();
    }
    // The menu of keys for the selected effect: Off, then what the mix has made by the time this rack runs.
    // A track's FX hear other tracks before their FX; a bus's FX also tracks after their faders; a return's FX
    // also the buses; the master's FX also the returns.
    void showSidechainMenu(){
        const int index=slot.getSelectedId()-2;
        if(index<0 || index>=static_cast<int>(keyOf.size()))return;
        juce::PopupMenu menu;
        const auto current=keyOf[static_cast<std::size_t>(index)];
        const int where=destination();
        const auto is=[&](KeySource from,int i){return current.second==i && current.first==from;};
        const auto here=stage();
        menu.addItem(1,"Off",true,current.second<0);
        if(here!=RackStage::track)menu.addSectionHeader("Tracks, before their FX");
        for(int t=0;t<trackNames.size();++t)if(t!=where)menu.addItem(keyTrackBase+t,"Key from "+trackNames[t],true,is(KeySource::track,t));
        if(keyReady(here,KeySource::trackPostFader)){
            menu.addSectionHeader("Tracks, after their faders");
            for(int t=0;t<trackNames.size();++t)menu.addItem(keyPostBase+t,"Key from "+trackNames[t]+" (after its fader)",true,is(KeySource::trackPostFader,t));
        }
        if(keyReady(here,KeySource::bus)){
            bool any=false;
            for(int b=0;b<maxBuses;++b)if(keyBusNames[static_cast<std::size_t>(b)].isNotEmpty()){
                if(!any)menu.addSectionHeader("Group buses");
                any=true; menu.addItem(keyBusBase+b,"Key from "+keyBusNames[static_cast<std::size_t>(b)],true,is(KeySource::bus,b));
            }
        }
        if(keyReady(here,KeySource::returnTrack)){
            bool any=false;
            for(int r=0;r<maxReturns;++r)if(keyReturnNames[static_cast<std::size_t>(r)].isNotEmpty()){
                if(!any)menu.addSectionHeader("Returns");
                any=true; menu.addItem(keyReturnBase+r,"Key from "+keyReturnNames[static_cast<std::size_t>(r)],true,is(KeySource::returnTrack,r));
            }
        }
        juce::Component::SafePointer<PluginPanel> safe(this);
        const auto chosen=[safe,index](int id){
            if(!safe||id<=0||!safe->onSidechain)return;
            if(id==1)safe->onSidechain(index,KeySource::track,-1);
            else if(id>=keyReturnBase)safe->onSidechain(index,KeySource::returnTrack,id-keyReturnBase);
            else if(id>=keyBusBase)safe->onSidechain(index,KeySource::bus,id-keyBusBase);
            else if(id>=keyPostBase)safe->onSidechain(index,KeySource::trackPostFader,id-keyPostBase);
            else safe->onSidechain(index,KeySource::track,id-keyTrackBase);
        };
        if(sidechainMenuLauncher){sidechainMenuLauncher(std::move(menu),chosen);return;}
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sidechain),chosen);
    }
    // Tests replace the popup: they get the menu and the function to call with a choice.
    std::function<void(juce::PopupMenu,std::function<void(int)>)> sidechainMenuLauncher;
    // While a scan runs, Scan is disabled and Cancel is offered.
    void setScanning(bool running){scanning=running;scan.setEnabled(!running);cancelScan.setVisible(running);resized();}
    bool isScanning() const{return scanning;}
    void setPreflightEnabled(bool on){preflightEnabled=on;}
    bool preflightIsEnabled() const{return preflightEnabled;}
    void setStartupScanEnabled(bool on){startupScan=on;}
    bool startupScanIsEnabled() const{return startupScan;}
    // Runs a Scan menu command (the menu items call this; tests do too).
    void perform(ScanCommand command){
        if(command==ScanCommand::togglePreflight)preflightEnabled=!preflightEnabled;
        if(command==ScanCommand::toggleStartupScan)startupScan=!startupScan;
        if(onScanCommand)onScanCommand(command);
    }
    // Plugins blocked after crashing while hosted (T1-09): their names for the Blocked plugins menu, and what
    // allows one again (its index in that list) or every one (-1).
    std::function<juce::StringArray()> blockedPlugins;
    std::function<void(int)> onAllowBlocked;
    // The Blocked plugins submenu: one item per plugin (ids from blockedBase), then Allow all.
    static constexpr int blockedBase=100, allowAllBlocked=99;
    juce::PopupMenu blockedMenu() const {
        juce::PopupMenu menu;
        const auto names=blockedPlugins?blockedPlugins():juce::StringArray();
        if(names.isEmpty()){menu.addItem(allowAllBlocked,"No plugin is blocked",false);return menu;}
        for(int i=0;i<names.size();++i)menu.addItem(blockedBase+i,"Allow "+names[i]+" to load again");
        menu.addSeparator();
        menu.addItem(allowAllBlocked,"Allow all of them again");
        return menu;
    }
    // A choice from the Scan menu, by item id (the menu calls this; tests do too).
    void chooseScanItem(int id){
        static constexpr ScanCommand commands[]={ScanCommand::standardFolders,ScanCommand::folder,ScanCommand::singleFile,ScanCommand::retryFailed,ScanCommand::rescanAll,ScanCommand::recheckBlocked,ScanCommand::togglePreflight,ScanCommand::openScanLog,ScanCommand::openProblemLog,ScanCommand::toggleStartupScan};
        if(id>=1&&id<=10){perform(commands[id-1]);return;}
        if(id==allowAllBlocked){if(onAllowBlocked)onAllowBlocked(-1);return;}
        if(id>=blockedBase&&onAllowBlocked)onAllowBlocked(id-blockedBase);
    }
    void showScanMenu(){
        juce::PopupMenu menu;
        menu.addItem(1,"Scan the standard VST3 folders",!scanning);
        menu.addItem(2,"Scan a folder (including subfolders)...",!scanning);
        menu.addItem(3,"Add a single plugin file...",!scanning);
        menu.addSeparator();
        menu.addItem(4,"Retry plugins that failed",!scanning);
        menu.addItem(5,"Rescan everything",!scanning);
        menu.addItem(6,"Re-check plugins the safety check blocked",true);
        menu.addSubMenu("Blocked plugins (crashed while playing)",blockedMenu());
        menu.addSeparator();
        menu.addItem(7,"Check plugins before loading them (recommended)",true,preflightEnabled);
        menu.addItem(10,"Look for new plugins when Alien Studio starts",true,startupScan);
        menu.addItem(8,"Show the last scan log",true);
        menu.addItem(9,"Show the plugin problem log",true);
        juce::Component::SafePointer<PluginPanel> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scan),[safe](int id){if(safe&&id!=0)safe->chooseScanItem(id);});
    }
    void setStatus(const juce::String& text){status.setText(text,juce::dontSendNotification);status.setTooltip(text);}
    void resized() override {
        auto r=getLocalBounds().reduced(20);
        // The browser takes the right-hand side when there is room, otherwise the space under the destination.
        const bool beside=r.getWidth()>=controlsWidth+20+320;
        if(beside){auto right=r.removeFromRight(r.getWidth()-controlsWidth-20);browser.setBounds(right);r.removeFromRight(20);}
        auto row=r.removeFromTop(32);
        target.setBounds(row.removeFromLeft(190)); row.removeFromLeft(12); scan.setBounds(row.removeFromLeft(140)); row.removeFromLeft(8); cancelScan.setBounds(row.removeFromLeft(130));
        r.removeFromTop(12);liveInput.setBounds(r.removeFromTop(30));
        if(!beside){r.removeFromTop(12);browser.setBounds(r.removeFromTop(juce::jmax(120,r.getHeight()-belowStackedBrowser)));}
        r.removeFromTop(12);row=r.removeFromTop(32);
        addInstrument.setBounds(row.removeFromLeft(150));row.removeFromLeft(10);addEffect.setBounds(row.removeFromLeft(120));
        r.removeFromTop(20);slot.setBounds(r.removeFromTop(30));r.removeFromTop(12);row=r.removeFromTop(32);
        for(auto* b:{&open,&bypass,&sidechain,&up,&down,&remove}){b->setBounds(row.removeFromLeft(118));row.removeFromLeft(8);}
        r.removeFromTop(16);status.setBounds(r.removeFromTop(110));
    }
private:
    juce::String problem(int where,int index) const{if(!health)return {};const auto text=health(where,index);return text.isEmpty()?juce::String():"  ["+text+"]";}
    juce::String name(const PluginState& p) const {
        auto xml=juce::parseXML(p.descriptionXml);
        auto text=(xml?xml->getStringAttribute("name"):"Empty")+(p.bypass?" [bypassed]":"");
        if(p.sidechain>=0) text+=" [keyed by "+keyName(p.sidechainFrom,p.sidechain)+"]";
        return text;
    }
    void updateSidechainButton(){
        const int index=slot.getSelectedId()-2;
        sidechain.setEnabled(index>=0 && hasSidechainInput && hasSidechainInput(destination(),index));
    }
    void action(int op){if(onAction && slot.getSelectedId()>0)onAction(op,slot.getSelectedId()-2);}
    // Use instrument takes an instrument on a track; Add FX takes an effect anywhere. A blocked plugin takes neither.
    void updateAddButtons(){
        const auto* chosen=browser.selected();
        const bool usable=chosen!=nullptr && !chosen->blocked;
        addInstrument.setEnabled(usable && chosen->instrument && destination()<maxSessionTracks);
        addEffect.setEnabled(usable && !chosen->instrument);
    }
    static constexpr int controlsWidth=780;
    // Under a stacked browser: the add row (12+32), slot (20+30), slot buttons (12+32), and a gap (16)
    // with three lines of status (72). The browser grows into everything else.
    static constexpr int belowStackedBrowser=12+32+20+30+12+32+16+72;
    juce::ComboBox target,slot,liveInput;
    PluginBrowser browser;
    juce::TextButton scan,cancelScan,addInstrument,addEffect,open,bypass,sidechain,up,down,remove;
    juce::String keyName(KeySource from,int i) const {
        const auto slotName=[&](const auto& names,const juce::String& fallback){
            const auto& n=names[static_cast<std::size_t>(i)]; return n.isNotEmpty()?n:fallback; };
        switch(from){
        case KeySource::bus: return i<maxBuses?slotName(keyBusNames,"a bus"):juce::String("a bus");
        case KeySource::returnTrack: return i<maxReturns?slotName(keyReturnNames,"a return"):juce::String("a return");
        case KeySource::trackPostFader:
        case KeySource::track: break;
        }
        const auto track=i<trackNames.size()?trackNames[i]:"Track "+juce::String(i+1);
        return from==KeySource::trackPostFader?track+" after its fader":track;
    }
    juce::StringArray trackNames;
    std::array<juce::String,maxBuses> keyBusNames{};
    std::array<juce::String,maxReturns> keyReturnNames{};
    std::vector<std::pair<KeySource,int>> keyOf;   // each effect's key, as last shown
    juce::Label status;
    bool scanning{},preflightEnabled{true},startupScan{true};
};
}
