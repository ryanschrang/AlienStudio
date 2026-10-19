#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Arrangement.h"
#include <functional>
#include <vector>

namespace alien {
class ArrangementPanel final : public juce::Component {
public:
    ArrangementPanel() {
        for (auto* c : std::array<juce::Component*, 13>{&track,&scene,&start,&length,&songLength,&page,
            &add,&apply,&duplicate,&remove,&undo,&exportWav,&status}) addAndMakeVisible(c);
        for (int i=1;i<=4;++i) { track.addItem("Track "+juce::String(i),i); scene.addItem("Scene "+juce::String(i),i); }
        track.setSelectedId(1); scene.setSelectedId(1);
        setup(start,1,128,1,"Start bar "); setup(length,1,128,4,"Bars ");
        setup(songLength,1,128,16,"Song bars "); setup(page,1,113,1,"View from ");
        track.setName("Arrangement destination track"); scene.setName("Source Session scene");
        start.setName("Region start bar"); length.setName("Region length in bars");
        songLength.setName("Song duration in bars"); page.setName("First visible timeline bar");
        add.setButtonText("Add clip"); apply.setButtonText("Apply move/length"); duplicate.setButtonText("Duplicate");
        remove.setButtonText("Delete"); undo.setButtonText("Undo"); exportWav.setButtonText("Export WAV");
        add.onClick=[this] { auto next=data; if(next.add(controls())) commit(next,static_cast<int>(next.count)-1); else rejected(); };
        apply.onClick=[this] { auto next=data; if(selected>=0 && next.replace(static_cast<std::size_t>(selected),controls())) commit(next,selected); else rejected(); };
        duplicate.onClick=[this] { if(selected<0) return; auto next=data; auto r=data.regions[static_cast<std::size_t>(selected)];
            r.startBar+=r.lengthBars; if(next.add(r)) commit(next,static_cast<int>(next.count)-1); else rejected(); };
        remove.onClick=[this] { auto next=data; if(selected>=0 && next.erase(static_cast<std::size_t>(selected))) commit(next,-1); };
        undo.onClick=[this] { if(history.empty()) return; data=history.back(); history.pop_back(); selected=-1; syncLength(); changed(); };
        songLength.onValueChange=[this] { auto next=data; next.lengthBars=static_cast<int>(songLength.getValue());
            if(next.valid()) commit(next,selected); else { syncLength(); rejected(); } };
        page.onValueChange=[this] { repaint(); };
        exportWav.onClick=[this] { if(onExport) onExport(); };
        status.setText("Select track/scene, start bar and length, then Add clip. Click a region to move, duplicate or delete it.",juce::dontSendNotification);
    }
    std::function<void()> onChange, onExport;
    const Arrangement& getArrangement() const { return data; }
    void setArrangement(const Arrangement& value) { data=value; selected=-1; history.clear(); syncLength(); repaint(); }
    void setGridNames(const juce::StringArray& tracks, const juce::StringArray& scenes) {
        const int t = track.getSelectedId(), s = scene.getSelectedId();
        track.clear(juce::dontSendNotification); scene.clear(juce::dontSendNotification);
        for (int i = 0; i < tracks.size(); ++i) track.addItem(tracks[i], i + 1);
        for (int i = 0; i < scenes.size(); ++i) scene.addItem(scenes[i], i + 1);
        track.setSelectedId(juce::jlimit(1, tracks.size(), t), juce::dontSendNotification);
        scene.setSelectedId(juce::jlimit(1, scenes.size(), s), juce::dontSendNotification);
        visibleTracks = tracks.size(); repaint();
    }
    void setPlayhead(int thousandths) { position=thousandths; repaint(); }
    void setStatus(const juce::String& text) { status.setText(text,juce::dontSendNotification); status.setTooltip(text); }
    void resized() override {
        track.setBounds(12,8,110,28); scene.setBounds(130,8,110,28);
        start.setBounds(248,8,140,28); length.setBounds(396,8,115,28); songLength.setBounds(519,8,155,28);
        page.setBounds(682,8,getWidth()-694,28);
        add.setBounds(12,44,90,28); apply.setBounds(110,44,145,28); duplicate.setBounds(263,44,95,28);
        remove.setBounds(366,44,85,28); undo.setBounds(459,44,75,28); exportWav.setBounds(542,44,120,28);
        status.setBounds(12,getHeight()-34,getWidth()-24,28);
    }
    void paint(juce::Graphics& g) override {
        const float x=90, width=static_cast<float>(getWidth()-106), barWidth=width/16;
        const int first=static_cast<int>(page.getValue())-1;
        const float rowHeight=std::max(12.0f,static_cast<float>(getHeight()-145)/visibleTracks);
        for(int t=0;t<visibleTracks;++t) {
            g.setColour(juce::Colour(0xff95a8b5));
            g.drawText("Track "+juce::String(t+1),12,100+static_cast<int>(t*rowHeight),70,28,juce::Justification::centredLeft);
            g.setColour(juce::Colour(0xff293344)); g.fillRect(x,100+t*rowHeight,width,rowHeight-2);
        }
        for(int b=0;b<=16;++b) {
            g.setColour(juce::Colour(0xff425064)); g.drawVerticalLine(static_cast<int>(x+b*barWidth),100,100+visibleTracks*rowHeight);
            if(b<16) { g.setColour(juce::Colour(0xffb9ced8)); g.drawText(juce::String(first+b+1),static_cast<int>(x+b*barWidth),78,40,20,juce::Justification::centredLeft); }
        }
        for(std::size_t i=0;i<data.count;++i) {
            const auto& r=data.regions[i];
            const int left=std::max(r.startBar,first), right=std::min(r.startBar+r.lengthBars,first+16);
            if(right<=left) continue;
            auto box=juce::Rectangle<float>(x+(left-first)*barWidth+2,102+r.track*rowHeight,(right-left)*barWidth-4,rowHeight-6);
            const std::array<juce::Colour,4> colours{juce::Colour(0xff55cdb7),juce::Colour(0xff9b8ee8),juce::Colour(0xff69afe2),juce::Colour(0xffdcaf68)};
            const auto colour=colours[static_cast<std::size_t>(r.track) % colours.size()];
            g.setColour(colour.withAlpha(static_cast<int>(i)==selected?0.55f:0.22f));g.fillRoundedRectangle(box,6);
            g.setColour(colour);g.fillRoundedRectangle(box.withWidth(3),1.5f);
            if(static_cast<int>(i)==selected)g.drawRoundedRectangle(box,6,1.5f);
            g.setColour(juce::Colours::white);g.drawText("Scene "+juce::String(r.scene+1),box.toNearestInt(),juce::Justification::centred);
        }
        const float bar=static_cast<float>(position)/1000;
        if(position>=0 && bar>=first && bar<first+16) { g.setColour(juce::Colours::orange); g.drawVerticalLine(static_cast<int>(x+(bar-first)*barWidth),100,100+visibleTracks*rowHeight); }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        const float rowHeight=std::max(12.0f,static_cast<float>(getHeight()-145)/visibleTracks);
        if(e.y<100 || e.x<90) return;
        const int t=static_cast<int>((e.y-100)/rowHeight);
        const int b=static_cast<int>(page.getValue())-1+static_cast<int>((e.x-90)*16.0f/(getWidth()-106));
        selected=-1;
        for(std::size_t i=0;i<data.count;++i) { const auto& r=data.regions[i]; if(r.track==t && b>=r.startBar && b<r.startBar+r.lengthBars) {
            selected=static_cast<int>(i); track.setSelectedId(r.track+1); scene.setSelectedId(r.scene+1);
            start.setValue(r.startBar+1,juce::dontSendNotification); length.setValue(r.lengthBars,juce::dontSendNotification); break;
        }} repaint();
    }
private:
    static void setup(juce::Slider& slider,double min,double max,double value,const juce::String& prefix) {
        slider.setRange(min,max,1); slider.setSliderStyle(juce::Slider::IncDecButtons);
        slider.setTextBoxStyle(juce::Slider::TextBoxLeft,false,110,28); slider.setValue(value);
        slider.textFromValueFunction=[prefix](double v) { return prefix+juce::String(static_cast<int>(v)); };
        slider.valueFromTextFunction=[](const juce::String& s) { return static_cast<double>(s.retainCharacters("0123456789").getIntValue()); };
    }
    ArrangementRegion controls() const { return {track.getSelectedId()-1,scene.getSelectedId()-1,static_cast<int>(start.getValue())-1,static_cast<int>(length.getValue())}; }
    void syncLength() { songLength.setValue(data.lengthBars,juce::dontSendNotification); }
    void commit(const Arrangement& next,int index) { if(history.size()==64) history.erase(history.begin());history.push_back(data);data=next;selected=index;changed(); }
    void changed() { setStatus("Arrangement updated. Changes during playback restart the song.");repaint();if(onChange)onChange(); }
    void rejected() { setStatus("Region overlaps another clip, exceeds the song length, or no region is selected. No changes made."); }
    Arrangement data;
    std::vector<Arrangement> history;
    int selected{-1},position{-1},visibleTracks{4};
    juce::ComboBox track,scene;
    juce::Slider start,length,songLength,page;
    juce::TextButton add,apply,duplicate,remove,undo,exportWav;
    juce::Label status;
};
} // namespace alien
