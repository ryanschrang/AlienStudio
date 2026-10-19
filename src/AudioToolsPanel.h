#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <array>
#include <algorithm>
namespace alien {
class AudioToolsPanel final : public juce::Component {
public:
    AudioToolsPanel() {
        for(auto* c:std::array<juce::Component*,8>{&record,&settings,&warp,&sourceTempo,&channels,&destination,&status,&subtitle})addAndMakeVisible(c);
        record.setButtonText("Record take");settings.setButtonText("Audio settings");warp.setButtonText("Render tempo match");
        record.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff703345));
        record.setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xffdb526c));
        channels.addItem("Mono • input 1",1);channels.addItem("Stereo • inputs 1 + 2",2);channels.setSelectedId(1);
        channels.setName("Recording input channels");
        sourceTempo.setRange(30,300,1);sourceTempo.setValue(120);sourceTempo.setSliderStyle(juce::Slider::IncDecButtons);
        sourceTempo.setTextBoxStyle(juce::Slider::TextBoxLeft,false,130,30);sourceTempo.setTextValueSuffix(" source BPM");sourceTempo.setName("Original clip tempo in BPM");
        subtitle.setText("Capture a take. Shape its timing. Keep your source.",juce::dontSendNotification);
        subtitle.setColour(juce::Label::textColourId,juce::Colour(0xff9caec3));
        status.setText("Select a Session clip as the destination. Recording begins after you choose a WAV filename.",juce::dontSendNotification);
        record.onClick=[this]{if(onRecord)onRecord();};settings.onClick=[this]{if(onSettings)onSettings();};warp.onClick=[this]{if(onWarp)onWarp();};
    }
    std::function<void()> onRecord,onSettings,onWarp;
    int inputChannels() const{return channels.getSelectedId();}
    double sourceBpm() const{return sourceTempo.getValue();}
    void setDestination(int track,int scene){destination.setText("TRACK "+juce::String(track+1)+"  /  SCENE "+juce::String(scene+1),juce::dontSendNotification);}
    void setStatus(const juce::String& s){status.setText(s,juce::dontSendNotification);status.setTooltip(s);}
    void setBusy(bool busy,bool recording=false,bool rendering=false){record.setEnabled(!busy || recording);settings.setEnabled(!busy);warp.setEnabled(!busy || rendering);warp.setButtonText(rendering?"Cancel tempo render":"Render tempo match");channels.setEnabled(!busy);sourceTempo.setEnabled(!busy);
        record.setButtonText(recording?"Stop & save take":"Record take");record.setToggleState(recording,juce::dontSendNotification);}
    void setMeter(float value){meter=std::clamp(value,0.0f,1.0f);repaint();}
    void resized() override {
        subtitle.setBounds(24,42,getWidth()-48,24);destination.setBounds(24,76,getWidth()-48,24);
        const int half=(getWidth()-64)/2;
        channels.setBounds(40,145,half-32,30);record.setBounds(40,183,half-32,38);settings.setBounds(40,226,half-32,30);
        sourceTempo.setBounds(half+56,145,half-32,30);warp.setBounds(half+56,183,half-32,38);
        status.setBounds(24,getHeight()-54,getWidth()-48,48);
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xffe3ebf5));g.setFont(juce::FontOptions(25,juce::Font::bold));g.drawText("Audio lab",24,10,getWidth()-48,32,juce::Justification::centredLeft);
        const int half=(getWidth()-64)/2;
        for(int i=0;i<2;++i){auto box=juce::Rectangle<float>(24.0f+i*(half+16),114.0f,static_cast<float>(half),164.0f);
            g.setColour(juce::Colour(0xff1b2534));g.fillRoundedRectangle(box,12);g.setColour(juce::Colour(0xff334155));g.drawRoundedRectangle(box,12,1);}
        g.setFont(juce::FontOptions(14,juce::Font::bold));g.setColour(juce::Colour(0xffee8fa0));g.drawText("01  RECORD",40,124,half-32,22,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffa79beb));g.drawText("02  TEMPO WARP",half+56,124,half-32,22,juce::Justification::centredLeft);
        g.setFont(13);g.setColour(juce::Colour(0xffa9b8cc));g.drawText("60-second takes • monitoring off",40,256,half-32,20,juce::Justification::centredLeft);
        g.drawText("Fixed tempo • separate WAV • 0.5–2×",half+56,226,half-32,28,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff314052));g.fillRoundedRectangle(24.0f,286.0f,static_cast<float>(getWidth()-48),5,2);
        g.setColour(meter>0.95f?juce::Colour(0xffee6e82):juce::Colour(0xff55cdb7));g.fillRoundedRectangle(24.0f,286.0f,(getWidth()-48)*meter,5,2);
    }
private:
    juce::TextButton record,settings,warp;juce::ComboBox channels;juce::Slider sourceTempo;
    juce::Label destination,status,subtitle;float meter{};
};
}
