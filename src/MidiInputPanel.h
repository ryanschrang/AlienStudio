#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiInputDeviceManager.h"\n#include "MidiControllerProfile.h"

namespace alien {
class JuceMidiInputBackend final : public MidiInputBackend {
    class Connection final : public MidiInputConnection, private juce::MidiInputCallback {
    public:
        Connection(MidiInputPipe& p, MidiInputPipe::Token t, MidiControllerProfile& configured)
            : pipe(p), token(t), profile(configured) {}
        bool open(const std::string& id) { input = juce::MidiInput::openDevice(juce::String(id), this); return input != nullptr; }
        ~Connection() override { stop(); }
        void start() override { if (input && !started) { started = true; input->start(); } }
        void stop() override { if (input && started) { input->stop(); started = false; } }
    private:
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override {
            if (message.getRawDataSize() != 3) return;
            const auto* bytes = message.getRawData();
            const int mapped=profile.translate(bytes[0],bytes[1],bytes[2]);
            pipe.push(token,bytes[0],mapped,bytes[2]);
        }
        MidiInputPipe& pipe;
        MidiControllerProfile& profile;
        MidiInputPipe::Token token;
        std::unique_ptr<juce::MidiInput> input;
        bool started{};
    };
public:
    explicit JuceMidiInputBackend(MidiControllerProfile& configured) : profile(configured) {}
    std::vector<MidiInputDevice> available() override {
        std::vector<MidiInputDevice> result;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            result.push_back({d.identifier.toStdString(), d.name.toStdString()});
        return result;
    }
    std::unique_ptr<MidiInputConnection> open(const std::string& id, MidiInputPipe& pipe, MidiInputPipe::Token token) override {
        std::string name=id;
        for(const auto& device:available()) if(device.identifier==id) {name=device.name;break;}
        profile.select(profile.requested(),name);
        auto connection = std::make_unique<Connection>(pipe,token,profile);
        if (!connection->open(id)) return {};
        return connection;
    }
private:
    MidiControllerProfile& profile;
};

// Entire component and manager run on the message thread. MIDI callbacks only
// write compact messages into the pipe; the audio callback owns MIDI gates.
class MidiInputPanel final : public juce::Component {
public:
    explicit MidiInputPanel(MidiInputPipe& p, MidiInputBackend* fakeBackend = nullptr)
        : pipe(p), backend(profile), manager(fakeBackend ? *fakeBackend : backend, pipe) {
        setName("MIDI input and device management");
        addAndMakeVisible(label); label.setText("MIDI", juce::dontSendNotification);
        addAndMakeVisible(inputs); inputs.setName("MIDI input device");
        inputs.setTooltip("Select a USB MIDI input, None, or the built-in test input. All 16 MIDI channels play the current synth preset.");
        inputs.onChange = [this] {
            resetTestButtons();
            const int id = inputs.getSelectedId();
            if (id == 1) manager.selectOff();
            else if (id == 2) manager.selectTest();
            else if (id >= 3 && static_cast<std::size_t>(id - 3) < listed.size())
                manager.selectDevice(listed[static_cast<std::size_t>(id - 3)].identifier);
            profile.select(profile.requested(),manager.selectedName());
            clearActivity();
            rebuild(); updateStatus();
        };
        addAndMakeVisible(refresh); refresh.setButtonText("Refresh"); refresh.setName("Refresh and retry MIDI input");
        refresh.setTooltip("Scan for controllers and reconnect the selected input; releases its held notes");
        refresh.onClick = [this] { refreshDevices(true); };
        addChildComponent(testNote); testNote.setButtonText("Test C4"); testNote.setName("MIDI test note C4");
        testNote.setTooltip("In Test input mode, hold this button to send C4 at velocity 100. Release to send note-off.");
        testNote.onStateChange = [this] {
            const bool down = testNote.isDown();
            if (!down) testBlocked = false;
            if (testBlocked) return;
            if (down != testDown) { testDown = down; manager.sendTest(down ? 0x90 : 0x80, 60, down ? 100 : 0); }
        };
        addChildComponent(sustain); sustain.setButtonText("Sustain"); sustain.setName("MIDI test sustain pedal");
        sustain.setTooltip("Test input CC64: enable, press/release Test C4, then disable to release the sustained note");
        sustain.onClick = [this] { manager.sendTest(0xb0, 64, sustain.getToggleState() ? 127 : 0); };
        addAndMakeVisible(status); status.setName("MIDI input status and activity");
        addAndMakeVisible(profileBox); profileBox.setName("MIDI controller profile");
        profileBox.setTooltip("Automatic recognizes Akai MPK mini models by their Windows MIDI device name");
        profileBox.addItem("Automatic",1); profileBox.addItem("Generic MIDI",2);
        profileBox.addItem("Akai MPK mini",3); profileBox.addItem("MPK mini Plus",4); profileBox.addItem("MPK mini IV",5);
        profileBox.setSelectedId(1,juce::dontSendNotification);
        profileBox.onChange=[this] {
            profile.select(static_cast<MidiControllerFamily>(profileBox.getSelectedId()-1),manager.selectedName());
            updateStatus();
        };
        addAndMakeVisible(assignment); assignment.setName("MPK pad or knob assignment");
        for(int i=0;i<MidiControllerProfile::padCount;++i) assignment.addItem("Pad "+juce::String(i+1),i+1);
        for(int i=0;i<MidiControllerProfile::knobCount;++i) assignment.addItem("Knob "+juce::String(i+1),MidiControllerProfile::padCount+i+1);
        assignment.setSelectedId(1,juce::dontSendNotification);
        assignment.onChange=[this]{syncAssignment();};
        addAndMakeVisible(mappingValue); mappingValue.setName("MPK assignment MIDI number");
        mappingValue.setRange(0,127,1); mappingValue.setSliderStyle(juce::Slider::IncDecButtons);
        mappingValue.setTextBoxStyle(juce::Slider::TextBoxLeft,false,92,26);
        mappingValue.onValueChange=[this] {
            const int item=assignment.getSelectedId()-1,value=static_cast<int>(mappingValue.getValue());
            if(item>=0&&item<MidiControllerProfile::padCount) profile.setPadTarget(item,value);
            else if(item>=MidiControllerProfile::padCount) profile.setKnobTarget(item-MidiControllerProfile::padCount,value);
        };
        addAndMakeVisible(resetMappings); resetMappings.setButtonText("Reset map");
        resetMappings.onClick=[this]{profile.resetMappings();syncAssignment();};
        rebuild(); syncAssignment(); updateStatus();
    }
    void resetTestButtons() {
        if (testDown) manager.sendTest(0x80, 60, 0);
        if (sustain.getToggleState()) manager.sendTest(0xb0, 64, 0);
        testDown = false;
        testBlocked = testNote.isDown();
        sustain.setToggleState(false, juce::dontSendNotification);
    }
    void tick() {
        if (++ticks % 20 == 0) refreshDevices(false); // Main timer runs at 20 Hz.
        const auto count = pipe.activity();
        if (count != lastActivity) { lastActivity = count; activityUntil = juce::Time::getMillisecondCounter() + 200; }
        updateStatus();
    }
    void refreshDevices(bool retry = false) {
        const auto previous = manager.getState();
        manager.refresh(retry);
        if (retry || previous != manager.getState()) clearActivity();
        if (listed != manager.getDevices() || previous != manager.getState()) rebuild();
        updateStatus();
    }
    void resized() override {
        label.setBounds(0,2,36,28); inputs.setBounds(38,2,184,28);
        refresh.setBounds(228,2,65,28); testNote.setBounds(300,2,64,28);
        sustain.setBounds(370,2,78,28);
        const int start=manager.getState()==MidiInputDeviceManager::State::test?454:300;
        status.setBounds(start,2,juce::jmax(1,getWidth()-start),28);
        profileBox.setBounds(38,38,184,28); assignment.setBounds(228,38,112,28);
        mappingValue.setBounds(346,38,126,28); resetMappings.setBounds(478,38,82,28);
    }
private:
    void syncAssignment() {
        const int item=assignment.getSelectedId()-1;
        const int value=item<MidiControllerProfile::padCount?profile.padTarget(item):
            profile.knobTarget(item-MidiControllerProfile::padCount);
        if(value>=0) mappingValue.setValue(value,juce::dontSendNotification);
    }
    void clearActivity() {
        lastActivity = pipe.activity();
        lastOverflow = pipe.overflowCount();
        activityUntil = overflowUntil = 0;
    }
    void rebuild() {
        listed = manager.getDevices();
        inputs.clear(juce::dontSendNotification);
        inputs.addItem("None (off)", 1); inputs.addItem("Test input (no controller)", 2);
        int selected = manager.getState() == MidiInputDeviceManager::State::test ? 2 : 1;
        for (std::size_t i = 0; i < listed.size(); ++i) {
            auto title = juce::String(listed[i].name);
            int duplicateCount = 0;
            for (const auto& d : listed) if (d.name == listed[i].name) ++duplicateCount;
            if (duplicateCount > 1) title += " (port " + juce::String(static_cast<int>(i + 1)) + ")";
            inputs.addItem(title, static_cast<int>(i + 3));
            if (listed[i].identifier == manager.selectedIdentifier()) selected = static_cast<int>(i + 3);
        }
        if (!manager.selectedIdentifier().empty() && selected == 1) {
            inputs.addItem(juce::String(manager.selectedName()) + " (disconnected)", 10000);
            selected = 10000;
        }
        inputs.setSelectedId(selected, juce::dontSendNotification);
        const bool test = manager.getState() == MidiInputDeviceManager::State::test;
        testNote.setVisible(test); sustain.setVisible(test); resized();
    }
    void updateStatus() {
        using State = MidiInputDeviceManager::State;
        const auto state = manager.getState();
        const auto now = juce::Time::getMillisecondCounter();
        const bool active = activityUntil != 0 && static_cast<std::int32_t>(activityUntil - now) > 0;
        if (!active) activityUntil = 0;
        juce::String message;
        if (state == State::off) message = "No MIDI selected";
        else if (state == State::error) message = "Open failed - Retry";
        else if (state == State::disconnected) message = "Disconnected - waiting";
        else if(active) {
            switch(profile.lastActivity()) {
            case MidiActivityKind::pad: message="Pad "+juce::String(profile.lastControl()+1);break;
            case MidiActivityKind::knob: message="Knob/CC "+juce::String(profile.lastControl()+1);break;
            case MidiActivityKind::modulation: message="Modulation";break;
            case MidiActivityKind::sustain: message="Sustain";break;
            case MidiActivityKind::pitchBend: message="Pitch bend";break;
            default: message="Key input";break;
            }
        } else if(state==State::test) message="Test input ready";
        else message=juce::String(midiControllerFamilyName(profile.active()))+" connected";
        status.setText(message, juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, active ? juce::Colour(0xff7ee8a2)
            : state == State::error || state == State::disconnected ? juce::Colour(0xffffc478) : juce::Colour(0xffc5d6df));
        auto tip = state == State::error ? "Could not open the selected MIDI input. Close other MIDI applications and click Refresh to retry."
            : state == State::disconnected ? "Controller disconnected. Its notes were released. Reconnect the same port to resume automatically, or select another input."
            : "MIDI notes and CC64 sustain play the current synth preset independently of computer/mouse piano and sequencer notes.";
        status.setTooltip(tip);
        if (pipe.overflowCount() != lastOverflow) {
            lastOverflow = pipe.overflowCount();
            overflowUntil = juce::Time::getMillisecondCounter() + 2000;
        }
        if (overflowUntil != 0 && static_cast<std::int32_t>(overflowUntil - now) <= 0) overflowUntil = 0;
        if (state != State::off && overflowUntil != 0) {
            status.setText("MIDI overload - notes reset", juce::dontSendNotification);
            status.setColour(juce::Label::textColourId, juce::Colour(0xffffc478));
        }
    }
    MidiInputPipe& pipe;
    MidiControllerProfile profile;
    JuceMidiInputBackend backend;
    MidiInputDeviceManager manager;
    std::vector<MidiInputDevice> listed;
    juce::Label label, status;
    juce::ComboBox inputs,profileBox,assignment;
    juce::Slider mappingValue;
    juce::TextButton refresh,testNote,resetMappings;
    juce::ToggleButton sustain;
    bool testDown{}, testBlocked{};
    unsigned ticks{};
    std::uint32_t activityUntil{}, overflowUntil{};
    std::uint64_t lastActivity{}, lastOverflow{};
};
}
