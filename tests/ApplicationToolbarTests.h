#pragma once
#include "ApplicationToolbar.h"
#include <set>

static void applicationToolbarChecks() {
    using Menu=alien::ApplicationToolbar;
    Menu toolbar;Menu::State state;std::vector<int> commands;
    toolbar.queryState=[&]{return state;};toolbar.onCommand=[&](int id){commands.push_back(id);};
    require(!state.keyboardVisible,"on-screen piano starts hidden");
    require(toolbar.getMenuBarNames()==juce::StringArray{"File","Edit","View","Transport","Options","Help"},"six named application menus");
    auto entry=[&](int index,int id) {
        auto menu=toolbar.getMenuForIndex(index,{});
        juce::PopupMenu::MenuItemIterator iterator(menu);
        while(iterator.next())if(iterator.getItem().itemID==id)return juce::PopupMenu::Item(iterator.getItem());
        require(false,"expected command is present in its menu");return juce::PopupMenu::Item{};
    };
    require(entry(0,Menu::save).isEnabled,"save available while idle");
    require(!entry(0,Menu::exportSong).isEnabled,"empty song cannot be exported from menu");
    require(!entry(1,Menu::undo).isEnabled,"empty undo disabled");
    require(entry(2,Menu::detachView).isEnabled&&!entry(2,Menu::dockView).isEnabled&&!entry(2,Menu::dockAllViews).isEnabled,"only Detach is available for docked view");
    state.undoLabel="Move clip";state.redoLabel="Delete marker";
    require(entry(1,Menu::undo).text=="Undo Move clip"&&entry(1,Menu::undo).isEnabled,"undo shows current project operation");
    require(entry(1,Menu::redo).text=="Redo Delete marker","redo shows current project operation");
    state.hasSong=true;state.hasLoop=true;
    require(entry(0,Menu::exportSong).isEnabled&&entry(0,Menu::exportLoop).isEnabled,"song and loop export enabled with source");
    state.view=5;state.metronome=true;state.songMode=true;
    require(entry(2,Menu::viewBase+5).isTicked&&!entry(2,Menu::viewBase+2).isTicked,"View reflects active Song tab");
    require(entry(3,Menu::metronome).isTicked&&entry(3,Menu::songMode).isTicked,"transport toggles reflect live state");
    state.deviceView=true;state.keyboardVisible=false;
    require(!entry(2,Menu::detachView).isEnabled,"Devices page is not a detachable editor tab");
    require(entry(2,Menu::devices).isTicked&&!entry(2,Menu::viewBase+5).isTicked&&!entry(2,Menu::keyboard).isTicked,"devices and hidden keyboard reflected independently");
    state.playing=true;state.recording=true;
    require(entry(3,Menu::playStop).text=="Stop playback"&&entry(3,Menu::record).text=="Stop recording","transport command labels match playback and recording");
    require(!entry(4,Menu::audioSettings).isEnabled&&!entry(0,Menu::newProject).isEnabled,"recording blocks device changes and project replacement");
    state.busy=true;
    require(entry(3,Menu::record).isEnabled,"stopping an active take remains available while busy");
    toolbar.menuItemSelected(Menu::record,3);require(commands==std::vector<int>{Menu::record},"stop-take command dispatches");
    // A menu opened before an asynchronous operation began must recheck state.
    commands.clear();toolbar.menuItemSelected(Menu::save,0);toolbar.menuItemSelected(Menu::undo,1);
    toolbar.menuItemSelected(Menu::audioSettings,4);toolbar.menuItemSelected(999999,0);
    require(commands.empty(),"stale/unknown menu commands are rejected at selection time");
    state={};state.hasSong=true;state.hasLoop=true;state.undoLabel="Edit";state.redoLabel="Edit";
    state.viewDetached=true;state.hasDetachedViews=true;
    require(entry(2,Menu::detachView).text=="Show detached view"&&entry(2,Menu::dockView).isEnabled&&entry(2,Menu::dockAllViews).isEnabled,"View menu exposes raise and docking actions for detached windows");
    std::set<int> ids;
    for(int index=0;index<6;++index) {
        auto menu=toolbar.getMenuForIndex(index,{});
        require(menu.getNumItems()>0,"every named menu has actions");
        juce::PopupMenu::MenuItemIterator iterator(menu);
        while(iterator.next()) {const auto& item=iterator.getItem();if(item.itemID>0) {
            require(item.isEnabled,"idle example enables all actions");ids.insert(item.itemID);
            const auto before=commands.size();toolbar.menuItemSelected(item.itemID,index);
            require(commands.size()==before+1&&commands.back()==item.itemID,"every listed action reaches callback with its own ID");
        }}
    }
    require(ids.size()==35,"all application commands and nine views are reachable");
    for(int index:{-1,6,99})require(toolbar.getMenuForIndex(index,{}).getNumItems()==0,"invalid menu index is empty");
    for(int width:{960,1180,1280,1920}) {
        toolbar.setSize(width,Menu::height);
        for(auto* child:toolbar.getChildren()) {
            require(toolbar.getLocalBounds().contains(child->getBounds()),"toolbar control stays inside minimum/wide window");
            for(auto* other:toolbar.getChildren())if(child!=other)require(!child->getBounds().intersects(other->getBounds()),"toolbar controls do not overlap");
        }
    }
    juce::Button* audio=nullptr;juce::Button* midi=nullptr;juce::Button* piano=nullptr;
    for(auto* child:toolbar.getChildren())if(auto* b=dynamic_cast<juce::Button*>(child)) {
        if(b->getName()=="Open audio settings")audio=b;
        if(b->getName()=="Open MIDI settings")midi=b;
        if(b->getName()=="Show or hide on-screen piano")piano=b;
    }
    require(audio&&midi,"audio and MIDI quick-access buttons exist");
    commands.clear();toolbar.refreshState();clickButton(*audio);clickButton(*midi);
    require(commands==std::vector<int>{Menu::audioSettings,Menu::midiSettings},"quick-access buttons dispatch settings commands");
    require(piano&&!piano->getToggleState(),"piano icon starts unlit while piano is hidden");
    toolbar.onCommand=[&](int id){commands.push_back(id);if(id==Menu::keyboard)state.keyboardVisible=!state.keyboardVisible;toolbar.refreshState();};
    for(int view=0;view<Menu::viewCount;++view) {
        state.view=view;state.keyboardVisible=false;toolbar.refreshState();
        clickButton(*piano);
        require(state.keyboardVisible&&piano->getToggleState()&&entry(2,Menu::keyboard).isTicked,"piano icon reveals panel in every workspace and lights up");
        toolbar.menuItemSelected(Menu::keyboard,2);
        require(!state.keyboardVisible&&!piano->getToggleState(),"View menu hides piano and unlights the icon");
    }
    state.recording=true;toolbar.refreshState();require(!audio->isEnabled()&&midi->isEnabled(),"audio quick access disabled during recording without blocking MIDI page");
    require(piano->isEnabled(),"piano toggle remains usable while recording");
    std::cout<<"PASS: application menus, action dispatch, state labels/checkmarks, stale-menu guards, quick settings and layout\n";
}

static void renderToolbarPreview(const juce::File& file) {
    alien::ui::StudioLookAndFeel theme;juce::Component shell;
    alien::ApplicationToolbar toolbar;alien::TopBar transport;
    shell.setLookAndFeel(&theme);shell.addAndMakeVisible(toolbar);shell.addAndMakeVisible(transport);
    shell.setSize(1280,alien::ApplicationToolbar::height+alien::TopBar::height);
    toolbar.setBounds(0,0,1280,alien::ApplicationToolbar::height);
    transport.setBounds(0,alien::ApplicationToolbar::height,1280,alien::TopBar::height);
    transport.setActiveView(5);transport.setSongMode(true);
    auto image=shell.createComponentSnapshot(shell.getLocalBounds());
    auto output=file.createOutputStream();require(output!=nullptr,"toolbar preview output");
    output->setPosition(0);output->truncate();juce::PNGImageFormat png;
    require(png.writeImageToStream(image,*output),"toolbar preview rendered");
    shell.setLookAndFeel(nullptr);
}
