#pragma once
#include "StudioWidgets.h"
#include <array>
#include <functional>
#include <utility>

namespace alien {

// Window-level menus reuse the existing application actions. State is queried
// when a menu opens AND when a result arrives, so a stale popup cannot start a
// file operation after a recording/export began. All callbacks are message-thread only.
class ApplicationToolbar final : public juce::Component, public juce::MenuBarModel {
public:
    static constexpr int height = 28;
    enum Command {
        newProject = 1000, openProject, save, saveAs, savePortable, exportSong, exportLoop, quit,
        undo, redo, devices, keyboard, playStop, record, metronome, songMode,
        audioSettings, midiSettings, recordingOptions, plugins, recoverTakes, shortcuts, about,
        viewBase = 2000, viewCount = 9
    };
    struct State {
        bool busy{}, recording{}, playing{}, deviceView{}, keyboardVisible{};
        bool metronome{}, songMode{}, hasSong{}, hasLoop{};
        int view{};
        juce::String undoLabel, redoLabel;
    };
    std::function<State()> queryState;
    std::function<void(int)> onCommand;

    ApplicationToolbar() : menus(this) {
        setName("Application settings toolbar");
        addAndMakeVisible(menus);
        menus.setName("Application menus");
        menus.setColour(juce::PopupMenu::backgroundColourId, ui::colours::raised);
        addAndMakeVisible(audio);addAndMakeVisible(midi);
        audio.setName("Open audio settings");midi.setName("Open MIDI settings");
        audio.setTooltip("Audio input/output device, sample rate and buffer size (Ctrl+,)");
        midi.setTooltip("MIDI inputs and controller settings");
        audio.onClick=[this]{menuItemSelected(audioSettings,4);};
        midi.onClick=[this]{menuItemSelected(midiSettings,4);};
        addAndMakeVisible(piano);
        piano.setButtonText("Piano");piano.setName("Show or hide on-screen piano");
        piano.setGlyph(ui::Glyph::piano);piano.setIconOnly(true);
        piano.setClickingTogglesState(true);
        piano.setTooltip("Show the on-screen piano in any workspace");
        piano.onClick=[this]{menuItemSelected(keyboard,2);};
    }
    ~ApplicationToolbar() override { menus.setModel(nullptr); }

    juce::StringArray getMenuBarNames() override {return {"File","Edit","View","Transport","Options","Help"};}
    State currentState() const {return queryState ? queryState() : State{};}
    static bool enabled(int id,const State& s) {
        if(id>=viewBase&&id<viewBase+viewCount)return true;
        const bool idle=!s.busy&&!s.recording;
        switch(id) {
            case newProject:case openProject:case save:case saveAs:case savePortable:return idle;
            case exportSong:return idle&&s.hasSong;
            case exportLoop:return idle&&s.hasSong&&s.hasLoop;
            case undo:return idle&&s.undoLabel.isNotEmpty();
            case redo:return idle&&s.redoLabel.isNotEmpty();
            case audioSettings:case recordingOptions:case recoverTakes:case songMode:return idle;
            case record:return !s.busy||s.recording;
            case devices:case keyboard:case playStop:case metronome:case midiSettings:
            case plugins:case shortcuts:case about:case quit:return true;
            default:return false;
        }
    }
    juce::PopupMenu getMenuForIndex(int index,const juce::String&) override {
        const auto s=currentState();juce::PopupMenu menu;
        auto item=[&](int id,juce::String text,juce::String shortcut={},bool ticked=false){
            juce::PopupMenu::Item entry(text);entry.itemID=id;entry.isEnabled=enabled(id,s);
            entry.isTicked=ticked;entry.shortcutKeyDescription=shortcut;menu.addItem(std::move(entry));
        };
        switch(index) {
            case 0:
                item(newProject,"New project","Ctrl+N");item(openProject,"Open project...","Ctrl+O");
                menu.addSeparator();item(save,"Save","Ctrl+S");item(saveAs,"Save as...","Ctrl+Shift+S");
                item(savePortable,"Save portable copy...");menu.addSeparator();
                item(exportSong,"Export song WAV...");item(exportLoop,"Export loop WAV...");
                menu.addSeparator();item(quit,"Exit Alien Studio");break;
            case 1:
                item(undo,s.undoLabel.isEmpty()?juce::String("Undo"):"Undo "+s.undoLabel,"Ctrl+Z");
                item(redo,s.redoLabel.isEmpty()?juce::String("Redo"):"Redo "+s.redoLabel,"Ctrl+Shift+Z / Ctrl+Y");break;
            case 2: {
                const std::array<const char*,viewCount> names{"Steps","Piano Roll","Session","Mixer","Drums","Song","Audio lab","Plugins","Channel Rack"};
                for(int i=0;i<viewCount;++i)item(viewBase+i,names[static_cast<size_t>(i)],{},!s.deviceView&&s.view==i);
                menu.addSeparator();item(devices,"Devices / MIDI input",{},s.deviceView);
                item(keyboard,"Show on-screen keyboard",{},s.keyboardVisible);break;
            }
            case 3:
                item(playStop,s.playing?"Stop playback":"Start playback");
                item(record,s.recording?"Stop recording":"Record");menu.addSeparator();
                item(metronome,"Metronome",{},s.metronome);item(songMode,"Song mode",{},s.songMode);
                item(recordingOptions,"Recording options...");break;
            case 4:
                item(audioSettings,"Audio settings...","Ctrl+,");item(midiSettings,"MIDI input / controller settings...");
                item(recordingOptions,"Recording options...");item(plugins,"Plugin instruments & FX...");
                menu.addSeparator();item(recoverTakes,"Recover recorded takes...");break;
            case 5:item(shortcuts,"Keyboard shortcuts...");item(about,"About Alien Studio...");break;
            default:break;
        }
        return menu;
    }
    void menuItemSelected(int id,int) override {
        if(enabled(id,currentState())&&onCommand)onCommand(id);
    }
    void openMenus() { menus.showMenu(0); }
    void refreshState() {
        const auto s=currentState();audio.setEnabled(enabled(audioSettings,s));midi.setEnabled(enabled(midiSettings,s));
        piano.setToggleState(s.keyboardVisible,juce::dontSendNotification);
        piano.setTooltip(s.keyboardVisible?"Hide the on-screen piano":"Show the on-screen piano in any workspace");
    }
    void resized() override {
        auto r=getLocalBounds().reduced(8,2);
        midi.setBounds(r.removeFromRight(64));r.removeFromRight(6);
        audio.setBounds(r.removeFromRight(108));r.removeFromRight(8);
        piano.setBounds(r.removeFromRight(34));r.removeFromRight(8);
        menus.setBounds(r);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::raised);g.setColour(ui::colours::lineStrong);
        g.fillRect(0,getHeight()-1,getWidth(),1);
    }
private:
    juce::MenuBarComponent menus;
    juce::TextButton audio{"Audio settings"},midi{"MIDI"};
    ui::GlyphButton piano;
};
}
