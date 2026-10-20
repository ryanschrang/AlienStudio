#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>

namespace alien {

enum class MidiControllerFamily { automatic, generic, mpkMini, mpkMiniPlus, mpkMiniIV };
enum class MidiActivityKind { none, key, pad, knob, modulation, sustain, pitchBend };

inline MidiControllerFamily recognizeMidiController(const std::string& deviceName) {
    std::string name=deviceName;
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(name.find("mpk mini") == std::string::npos && name.find("mpkmini") == std::string::npos)
        return MidiControllerFamily::generic;
    if(name.find("plus") != std::string::npos) return MidiControllerFamily::mpkMiniPlus;
    if(name.find("iv") != std::string::npos || name.find("mini 4") != std::string::npos)
        return MidiControllerFamily::mpkMiniIV;
    return MidiControllerFamily::mpkMini;
}
inline const char* midiControllerFamilyName(MidiControllerFamily family) noexcept {
    switch(family) {
    case MidiControllerFamily::mpkMini: return "Akai MPK mini";
    case MidiControllerFamily::mpkMiniPlus: return "Akai MPK mini Plus";
    case MidiControllerFamily::mpkMiniIV: return "Akai MPK mini IV";
    case MidiControllerFamily::generic: return "Generic MIDI";
    default: return "Automatic";
    }
}

// Shared by the message-thread editor and MIDI callback. Every callback-side
// operation is a bounded scan over atomics; it never locks or allocates.
class MidiControllerProfile {
public:
    static constexpr int padCount=16, knobCount=8;
    MidiControllerProfile() { resetMappings(); }

    void select(MidiControllerFamily requested, const std::string& deviceName) noexcept {
        requestedFamily.store(static_cast<int>(requested),std::memory_order_release);
        const auto detected=recognizeMidiController(deviceName);
        activeFamily.store(static_cast<int>(requested==MidiControllerFamily::automatic?detected:requested),
                           std::memory_order_release);
    }
    MidiControllerFamily requested() const noexcept {
        return static_cast<MidiControllerFamily>(requestedFamily.load(std::memory_order_acquire));
    }
    MidiControllerFamily active() const noexcept {
        return static_cast<MidiControllerFamily>(activeFamily.load(std::memory_order_acquire));
    }
    bool isAkai() const noexcept {
        const auto f=active();
        return f==MidiControllerFamily::mpkMini || f==MidiControllerFamily::mpkMiniPlus
            || f==MidiControllerFamily::mpkMiniIV;
    }

    void resetMappings() noexcept {
        for(int i=0;i<padCount;++i) {
            padInputs[static_cast<std::size_t>(i)].store(36+i,std::memory_order_relaxed);
            padTargets[static_cast<std::size_t>(i)].store(36+i,std::memory_order_relaxed);
        }
        for(int i=0;i<knobCount;++i) {
            knobInputs[static_cast<std::size_t>(i)].store(70+i,std::memory_order_relaxed);
            knobTargets[static_cast<std::size_t>(i)].store(70+i,std::memory_order_relaxed);
        }
    }
    void setPadTarget(int pad,int note) noexcept {
        if(pad>=0&&pad<padCount&&note>=0&&note<128)
            padTargets[static_cast<std::size_t>(pad)].store(note,std::memory_order_release);
    }
    void setKnobTarget(int knob,int cc) noexcept {
        if(knob>=0&&knob<knobCount&&cc>=0&&cc<128)
            knobTargets[static_cast<std::size_t>(knob)].store(cc,std::memory_order_release);
    }
    int padTarget(int pad) const noexcept {
        return pad>=0&&pad<padCount?padTargets[static_cast<std::size_t>(pad)].load(std::memory_order_acquire):-1;
    }
    int knobTarget(int knob) const noexcept {
        return knob>=0&&knob<knobCount?knobTargets[static_cast<std::size_t>(knob)].load(std::memory_order_acquire):-1;
    }

    // status is the high nibble. Returns the translated first data byte.
    int translate(int status,int data1,int data2) noexcept {
        const int type=status&0xf0;
        if(!isAkai()) {
            publish(type==0xe0?MidiActivityKind::pitchBend:
                type==0xb0?(data1==1?MidiActivityKind::modulation:data1==64?MidiActivityKind::sustain:MidiActivityKind::knob):
                MidiActivityKind::key,data1,data2);
            return data1;
        }
        if(type==0x80||type==0x90) {
            for(int i=0;i<padCount;++i)
                if(padInputs[static_cast<std::size_t>(i)].load(std::memory_order_relaxed)==data1) {
                    const int mapped=padTargets[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
                    publish(MidiActivityKind::pad,i,data2); return mapped;
                }
            publish(MidiActivityKind::key,data1,data2); return data1;
        }
        if(type==0xb0) {
            if(data1==1) {publish(MidiActivityKind::modulation,data1,data2);return data1;}
            if(data1==64) {publish(MidiActivityKind::sustain,data1,data2);return data1;}
            for(int i=0;i<knobCount;++i)
                if(knobInputs[static_cast<std::size_t>(i)].load(std::memory_order_relaxed)==data1) {
                    const int mapped=knobTargets[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
                    publish(MidiActivityKind::knob,i,data2); return mapped;
                }
            publish(MidiActivityKind::knob,data1,data2); return data1;
        }
        if(type==0xe0) publish(MidiActivityKind::pitchBend,data1|(data2<<7),data2);
        return data1;
    }

    MidiActivityKind lastActivity() const noexcept {
        return static_cast<MidiActivityKind>(activityKind.load(std::memory_order_acquire));
    }
    int lastControl() const noexcept { return activityControl.load(std::memory_order_acquire); }
    int lastValue() const noexcept { return activityValue.load(std::memory_order_acquire); }
    std::uint64_t activityRevision() const noexcept { return revision.load(std::memory_order_acquire); }
private:
    void publish(MidiActivityKind kind,int control,int value) noexcept {
        activityControl.store(control,std::memory_order_relaxed);
        activityValue.store(value,std::memory_order_relaxed);
        activityKind.store(static_cast<int>(kind),std::memory_order_release);
        revision.fetch_add(1,std::memory_order_release);
    }
    std::array<std::atomic<int>,padCount> padInputs{},padTargets{};
    std::array<std::atomic<int>,knobCount> knobInputs{},knobTargets{};
    std::atomic<int> requestedFamily{static_cast<int>(MidiControllerFamily::automatic)};
    std::atomic<int> activeFamily{static_cast<int>(MidiControllerFamily::generic)};
    std::atomic<int> activityKind{},activityControl{},activityValue{};
    std::atomic<std::uint64_t> revision{};
};

} // namespace alien
