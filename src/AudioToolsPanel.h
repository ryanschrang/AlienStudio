#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <array>
#include <algorithm>
namespace alien {
class AudioToolsPanel final : public juce::Component {
public:
    AudioToolsPanel() {
        for(auto* c:std::array<juce::Component*,9>{&recover,&record,&settings,&warp,&sourceTempo,&channels,&destination,&status,&subtitle})addAndMakeVisible(c);
        for (auto* label : {&heading, &recordHeading, &warpHeading, &recordNote, &warpNote}) addAndMakeVisible(label);
        heading.setText("Audio lab", juce::dontSendNotification);
        heading.setFont(juce::Font(juce::FontOptions(24.0f)));
        recordHeading.setText("01  RECORD", juce::dontSendNotification); recordHeading.setColour(juce::Label::textColourId, juce::Colour(0xffee8fa0));
        warpHeading.setText("02  TEMPO WARP", juce::dontSendNotification); warpHeading.setColour(juce::Label::textColourId, juce::Colour(0xffa79beb));
        recordNote.setText("Disk streaming / monitoring off", juce::dontSendNotification);
        warpNote.setText("Source file / disk render / 0.5-2x", juce::dontSendNotification);
        recover.setButtonText("Recover takes...");
        recover.onClick=[this]{if(onRecover)onRecover();};
        record.setButtonText("Record take");settings.setButtonText("Audio settings");warp.setButtonText("Render tempo match");
        record.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff703345));
        record.setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xffdb526c));
        channels.addItem("Mono / input 1",1);channels.addItem("Stereo / inputs 1 + 2",2);channels.setSelectedId(1);
        channels.setName("Recording input channels");
        sourceTempo.setRange(30,300,1);sourceTempo.setValue(120);sourceTempo.setSliderStyle(juce::Slider::IncDecButtons);
        sourceTempo.setTextBoxStyle(juce::Slider::TextBoxLeft,false,130,30);sourceTempo.setTextValueSuffix(" source BPM");sourceTempo.setName("Original clip tempo in BPM");
        subtitle.setText("Capture a take. Shape its timing. Keep your source.",juce::dontSendNotification);
        subtitle.setColour(juce::Label::textColourId,juce::Colour(0xff9caec3));
        status.setText("Select a Session clip as the destination. Recording begins after you choose a WAV filename.",juce::dontSendNotification);
        record.onClick=[this]{if(onRecord)onRecord();};settings.onClick=[this]{if(onSettings)onSettings();};warp.onClick=[this]{if(onWarp)onWarp();};
    }
    std::function<void()> onRecord,onSettings,onWarp,onRecover;
    int inputChannels() const{return channels.getSelectedId();}
    double sourceBpm() const{return sourceTempo.getValue();}
    void setDestination(int track,int scene){destination.setText("TRACK "+juce::String(track+1)+"  /  SCENE "+juce::String(scene+1),juce::dontSendNotification);}
    void setStatus(const juce::String& s){status.setText(s,juce::dontSendNotification);status.setTooltip(s);}
    void setBusy(bool busy,bool recording=false,bool rendering=false){recover.setEnabled(!busy);record.setEnabled(!busy || recording);settings.setEnabled(!busy);warp.setEnabled(!busy || rendering);warp.setButtonText(rendering?"Cancel tempo render":"Render tempo match");channels.setEnabled(!busy);sourceTempo.setEnabled(!busy);
        record.setButtonText(recording?"Stop & save take":"Record take");record.setToggleState(recording,juce::dontSendNotification);}
    void setMeter(float value){meter=std::clamp(value,0.0f,1.0f);repaint();}
    void resized() override {
        heading.setBounds(24,10,getWidth()-48,32);
        subtitle.setBounds(24,42,getWidth()-48,24);destination.setBounds(24,76,getWidth()-48,24);
        const int half=(getWidth()-64)/2;
        recordHeading.setBounds(40,120,half-32,22); warpHeading.setBounds(half+56,120,half-32,22);
        recordNote.setBounds(40,256,half-32,20); warpNote.setBounds(half+56,226,half-32,28);
        channels.setBounds(40,145,half-32,30);record.setBounds(40,183,half-32,38);settings.setBounds(40,226,half-32,30);
        sourceTempo.setBounds(half+56,145,half-32,30);warp.setBounds(half+56,183,half-32,38);
        recover.setBounds(24,306,160,28);
        status.setBounds(24,getHeight()-54,getWidth()-48,48);
    }
    void paint(juce::Graphics& g) override {
        const int half=(getWidth()-64)/2;
        for(int i=0;i<2;++i){auto box=juce::Rectangle<float>(24.0f+i*(half+16),114.0f,static_cast<float>(half),164.0f);
            g.setColour(juce::Colour(0xff1e2025));g.fillRoundedRectangle(box,12);g.setColour(juce::Colour(0xff2b2e35));g.drawRoundedRectangle(box,12,1);}
        g.setColour(juce::Colour(0xff314052));g.fillRoundedRectangle(24.0f,286.0f,static_cast<float>(getWidth()-48),5,2);
        g.setColour(meter>0.95f?juce::Colour(0xffee6e82):juce::Colour(0xff55cdb7));g.fillRoundedRectangle(24.0f,286.0f,(getWidth()-48)*meter,5,2);
    }
private:
    juce::TextButton record,settings,warp,recover;juce::ComboBox channels;juce::Slider sourceTempo;
    juce::Label heading, recordHeading, warpHeading, recordNote, warpNote;
    juce::Label destination,status,subtitle;float meter{};
};
}
