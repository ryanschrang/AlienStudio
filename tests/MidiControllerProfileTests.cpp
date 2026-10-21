#include "MidiControllerProfile.h"
#include <cstdlib>
#include <iostream>
using namespace alien;
static void check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    check(recognizeMidiController("MPK mini 3")==MidiControllerFamily::mpkMini,"recognize MPK mini");
    check(recognizeMidiController("Akai MPK mini Plus")==MidiControllerFamily::mpkMiniPlus,"recognize Plus");
    check(recognizeMidiController("MPK mini IV")==MidiControllerFamily::mpkMiniIV,"recognize IV");
    check(recognizeMidiController("USB Keyboard")==MidiControllerFamily::generic,"preserve generic devices");
    MidiControllerProfile profile;
    profile.select(MidiControllerFamily::automatic,"Akai Professional MPK mini 3");
    check(profile.isAkai()&&profile.active()==MidiControllerFamily::mpkMini,"automatic profile activates");
    profile.setPadTarget(0,48);
    check(profile.translate(0x99,36,100)==48&&profile.lastActivity()==MidiActivityKind::pad
        &&profile.lastControl()==0,"editable pad preset translates bank A");
    check(profile.translate(0x89,36,0)==48,"pad note-off uses the same translation");
    profile.setKnobTarget(0,21);
    check(profile.translate(0xb0,70,93)==21&&profile.lastActivity()==MidiActivityKind::knob,
        "editable knob preset translates CC");
    check(profile.translate(0xb0,1,64)==1&&profile.lastActivity()==MidiActivityKind::modulation,
        "modulation remains expression");
    check(profile.translate(0xe0,0,64)==0&&profile.lastActivity()==MidiActivityKind::pitchBend
        &&profile.lastControl()==8192,"pitch gesture is reported at 14-bit resolution");
    profile.select(MidiControllerFamily::generic,"Akai MPK mini");
    check(profile.translate(0x90,36,127)==36&&profile.lastActivity()==MidiActivityKind::key,
        "generic override bypasses MPK remapping");
    profile.resetMappings();
    check(profile.padTarget(0)==36&&profile.padTarget(15)==51
        &&profile.knobTarget(0)==70&&profile.knobTarget(7)==77,"reset restores common MPK defaults");
    std::cout<<"MPK mini controller profile tests passed\n";
}
