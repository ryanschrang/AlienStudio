#pragma once
#include "PluginState.h"
#include "StockEffects.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace alien {
class PluginPanel final : public juce::Component {
public:
    // Choice ids: stock effects are stockBase + catalog index; scanned VST3 plugins are index + 1.
    static constexpr int stockBase = 1000;
    PluginPanel() {
        for (auto* c:std::array<juce::Component*,12>{&target,&available,&slot,&scan,&addInstrument,&addEffect,&open,&bypass,&up,&down,&remove,&status}) addAndMakeVisible(c);
        scan.setButtonText("Scan VST3..."); addInstrument.setButtonText("Use instrument"); addEffect.setButtonText("Add FX");
        open.setButtonText("Open editor"); bypass.setButtonText("Bypass / enable"); up.setButtonText("Move FX up"); down.setButtonText("Move FX down"); remove.setButtonText("Remove");
        available.setTextWhenNothingSelected("Choose a stock effect or a scanned VST3 plugin");
        target.setName("Plugin destination track or master"); available.setName("Stock effects and scanned VST3 plugins"); slot.setName("Instrument or FX slot");
        target.onChange=[this]{if(onTarget)onTarget(target.getSelectedId()-1);};
        scan.onClick=[this]{if(onScan)onScan();};
        addInstrument.onClick=[this]{if(onAdd)onAdd(true,available.getSelectedId());};
        addEffect.onClick=[this]{if(onAdd)onAdd(false,available.getSelectedId());};
        open.onClick=[this]{action(0);}; bypass.onClick=[this]{action(1);}; remove.onClick=[this]{action(2);}; up.onClick=[this]{action(3);}; down.onClick=[this]{action(4);};
        setStatus("Pick a destination and add one of Alien Studio's stock effects (EQ, compressor, reverb, delay, limiter), or scan a 64-bit .vst3 bundle for an instrument or FX. MIDI routing is selected in Drums.");
    }
    std::function<void()> onScan;
    std::function<void(int)> onTarget;
    std::function<void(bool,int)> onAdd;
    std::function<void(int,int)> onAction;
    int destination() const { return target.getSelectedId()-1; }
    void refresh(const juce::StringArray& names, const PluginRackState& rack, const juce::KnownPluginList& list) {
        const int t=target.getSelectedId(); target.clear(juce::dontSendNotification);
        for(int i=0;i<names.size();++i)target.addItem(names[i],i+1);
        target.addItem("Master FX",maxSessionTracks+1);
        target.setSelectedId(t==maxSessionTracks+1?t:juce::jlimit(1,names.size(),t),juce::dontSendNotification);
        const int selectedPlugin=available.getSelectedId(); available.clear(juce::dontSendNotification);
        for(std::size_t i=0;i<stock::catalog.size();++i)available.addItem(juce::String(stock::catalog[i].name)+" [stock FX]",stockBase+static_cast<int>(i));
        auto descriptions=list.getTypes(); if(!descriptions.isEmpty())available.addSeparator();
        for(int i=0;i<descriptions.size();++i)available.addItem(descriptions[i].name+(descriptions[i].isInstrument?" [instrument]":" [FX]"),i+1);
        available.setSelectedId(available.indexOfItemId(selectedPlugin)>=0?selectedPlugin:stockBase,juce::dontSendNotification);
        const int s=slot.getSelectedId(); slot.clear(juce::dontSendNotification);
        if(destination()!=maxSessionTracks)slot.addItem("Instrument: "+name(rack.instrument),1);
        for(std::size_t i=0;i<rack.effects.size();++i)slot.addItem("FX "+juce::String(static_cast<int>(i+1))+": "+name(rack.effects[i]),static_cast<int>(i+2));
        // Master FX has no instrument row, so its first effect is id 2; a kept id of 1 would match no item.
        const bool master=destination()==maxSessionTracks;
        slot.setSelectedId(s>=(master?2:1)&&s<=static_cast<int>(rack.effects.size()+1)?s:master?(rack.effects.empty()?0:2):1,juce::dontSendNotification);
        addInstrument.setEnabled(destination()!=maxSessionTracks);
    }
    void setStatus(const juce::String& text){status.setText(text,juce::dontSendNotification);status.setTooltip(text);}
    void resized() override {
        auto r=getLocalBounds().reduced(20);auto row=r.removeFromTop(32);
        target.setBounds(row.removeFromLeft(190)); row.removeFromLeft(12); scan.setBounds(row.removeFromLeft(140));
        r.removeFromTop(12);available.setBounds(r.removeFromTop(30));r.removeFromTop(12);row=r.removeFromTop(32);
        addInstrument.setBounds(row.removeFromLeft(150));row.removeFromLeft(10);addEffect.setBounds(row.removeFromLeft(120));
        r.removeFromTop(20);slot.setBounds(r.removeFromTop(30));r.removeFromTop(12);row=r.removeFromTop(32);
        for(auto* b:{&open,&bypass,&up,&down,&remove}){b->setBounds(row.removeFromLeft(135));row.removeFromLeft(8);}
        r.removeFromTop(16);status.setBounds(r.removeFromTop(110));
    }
private:
    static juce::String name(const PluginState& p){auto xml=juce::parseXML(p.descriptionXml);return (xml?xml->getStringAttribute("name"):"Empty")+(p.bypass?" [bypassed]":"");}
    void action(int op){if(onAction && slot.getSelectedId()>0)onAction(op,slot.getSelectedId()-2);}
    juce::ComboBox target,available,slot;
    juce::TextButton scan,addInstrument,addEffect,open,bypass,up,down,remove;
    juce::Label status;
};
}
