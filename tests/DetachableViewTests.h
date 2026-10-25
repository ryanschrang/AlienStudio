#pragma once
#include "DetachableView.h"
#include "TypingPianoRouter.h"
#include <set>

static void detachableViewChecks() {
    std::array<juce::Component,9> editors;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    std::array<std::unique_ptr<alien::DetachableView>,9> hosts;
    const std::array<const char*,9> names{"Steps","Piano Roll","Session","Mixer","Drums","Song","Audio lab","Plugins","Rack"};
    int opened=0,closed=0,docked=0;
    for(size_t i=0;i<hosts.size();++i) {
        hosts[i]=std::make_unique<alien::DetachableView>(names[i],editors[i]);
        hosts[i]->onWindowOpened=[&](juce::Component&){++opened;};
        hosts[i]->onWindowClosed=[&](juce::Component&){++closed;};
        hosts[i]->onDocked=[&]{++docked;};
        tabs.addTab(names[i],alien::ui::colours::surface,hosts[i].get(),false);
    }
    tabs.setSize(1100,550);tabs.setCurrentTabIndex(5,false);
    editors[5].getProperties().set("shared edit",42);
    hosts[5]->detach();auto* song=hosts[5]->detachedWindow();
    require(song&&song->isVisible()&&editors[5].getTopLevelComponent()==song,"Song editor moves into a native window");
    require(tabs.getNumTabs()==9&&tabs.getTabContentComponent(5)==hosts[5].get(),"detaching preserves all tab identities and indices");
    hosts[5]->detach();require(hosts[5]->detachedWindow()==song&&opened==1,"detaching twice raises existing window without duplicates");
    tabs.setCurrentTabIndex(3,false);hosts[3]->detach();
    require(hosts[3]->detachedWindow()!=song&&hosts[5]->isDetached(),"Song and Mixer stay open simultaneously in separate windows");
    const auto floatedSize=editors[5].getLocalBounds();
    tabs.setCurrentTabIndex(5,false);tabs.setSize(1300,700);
    require(editors[5].getTopLevelComponent()==song&&editors[5].getLocalBounds()==floatedSize,"selecting/resizing main tabs never steals or resizes floated editor");
    editors[5].getProperties().set("shared edit",99);
    song->closeButtonPressed();
    require(!hosts[5]->isDetached()&&editors[5].getParentComponent()==hosts[5].get(),"native close button docks the existing editor");
    require(static_cast<int>(editors[5].getProperties()["shared edit"])==99,"edits survive detaching and docking on the same component");
    auto* content=hosts[3]->detachedWindow()->getContentComponent();juce::Button* dockButton=nullptr;
    for(auto* child:content->getChildren())if(auto* button=dynamic_cast<juce::Button*>(child))dockButton=button;
    require(dockButton!=nullptr,"detached window has an explicit Dock button");clickButton(*dockButton);
    require(!hosts[3]->isDetached()&&editors[3].getParentComponent()==hosts[3].get(),"Dock button returns Mixer without recreating it");
    for(int index:{0,1,2,8}) {hosts[static_cast<size_t>(index)]->detach();hosts[static_cast<size_t>(index)]->dock();}
    require(opened==closed&&docked==closed,"Steps, Piano Roll, Session and Rack complete the same detach/dock lifecycle");
    hosts[5]->detach();hosts[3]->detach();
    tabs.clearTabs();for(auto& host:hosts)host.reset();
    require(opened==closed&&editors[5].getParentComponent()==nullptr&&editors[3].getParentComponent()==nullptr,
            "owner shutdown closes remaining windows without deleting shared editors");
    std::cout<<"PASS: native detachable windows, simultaneous Song/Mixer, stable tab indices, shared edits, close/Dock and shutdown\n";
}

static void detachedTypingChecks() {
    juce::Component root,floating;juce::TextEditor text;
    root.setBounds(10,10,300,150);root.setWantsKeyboardFocus(true);root.addToDesktop(0);root.setVisible(true);
    floating.setBounds(350,10,300,150);floating.setWantsKeyboardFocus(true);
    floating.addAndMakeVisible(text);text.setBounds(10,50,250,30);floating.addToDesktop(0);floating.setVisible(true);
    juce::MidiKeyboardState notes;std::set<int> held;
    {
        TypingPianoRouter router(root,notes,[&](int k){return held.count(k)!=0;});
        router.addAuxiliaryRoot(floating);router.addAuxiliaryRoot(floating);
        floating.getPeer()->handleFocusGain();floating.grabKeyboardFocus();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        held.insert('A');floating.getPeer()->handleKeyPress(juce::KeyPress('a',{},'a'));
        require(notes.isNoteOn(2,60),"shared typing piano plays from detached workspace");
        held.clear();floating.getPeer()->handleKeyUpOrDown(false);
        require(!notes.isNoteOn(2,60),"detached key-up releases shared note");
        text.grabKeyboardFocus();juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        held.insert('A');floating.getPeer()->handleKeyPress(juce::KeyPress('a',{},'a'));
        require(!notes.isNoteOn(2,60)&&text.getText().containsIgnoreCase("a"),"detached text editor keeps typing without playing notes");
        held.clear();floating.getPeer()->handleKeyUpOrDown(false);floating.grabKeyboardFocus();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        held.insert('A');floating.getPeer()->handleKeyPress(juce::KeyPress('a',{},'a'));
        require(notes.isNoteOn(2,60),"typing resumes after text entry");
        router.removeAuxiliaryRoot(floating);require(!notes.isNoteOn(2,60),"docking removes listener and releases held UI note");
        held.clear();floating.getPeer()->handleKeyUpOrDown(false);
        root.getPeer()->handleFocusGain();root.grabKeyboardFocus();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        held.insert('A');root.getPeer()->handleKeyPress(juce::KeyPress('a',{},'a'));
        require(notes.isNoteOn(2,60),"main window typing input survives detached-window removal");
        held.clear();root.getPeer()->handleKeyUpOrDown(false);
    }
    floating.removeFromDesktop();root.removeFromDesktop();
    std::cout<<"PASS: shared detached typing piano, text focus, note release and listener removal\n";
}
