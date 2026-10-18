#pragma once
#include "MidiInputPanel.h"

static void checkMidiInputPanel() {
    struct Backend final : alien::MidiInputBackend {
        struct Connection final : alien::MidiInputConnection {
            void start() override {} void stop() override {}
        };
        std::vector<alien::MidiInputDevice> devices{{"a", "USB Keys"}, {"b", "USB Keys"}};
        bool fail{};
        std::vector<alien::MidiInputDevice> available() override { return devices; }
        std::unique_ptr<alien::MidiInputConnection> open(const std::string&, alien::MidiInputPipe&, alien::MidiInputPipe::Token) override {
            if (fail) return {};
            return std::make_unique<Connection>();
        }
    } backend;
    alien::MidiInputPipe pipe;
    alien::MidiInputPanel panel(pipe, &backend);
    juce::Component host;
    host.setBounds(0, 0, 680, 100); host.addAndMakeVisible(panel); panel.setBounds(10, 10, 660, 32);
    host.addToDesktop(0); host.setVisible(true); host.getPeer()->handleFocusGain(); pump();
    auto control = [&](const juce::String& name) -> juce::Component* {
        for (auto* c : panel.getChildren()) if (c->getName() == name) return c;
        require(false, "missing accessible MIDI input control"); return nullptr;
    };
    auto* inputs = dynamic_cast<juce::ComboBox*>(control("MIDI input device"));
    auto* status = dynamic_cast<juce::Label*>(control("MIDI input status and activity"));
    auto* test = dynamic_cast<juce::Button*>(control("MIDI test note C4"));
    auto* sustain = dynamic_cast<juce::Button*>(control("MIDI test sustain pedal"));
    auto* retry = dynamic_cast<juce::Button*>(control("Refresh and retry MIDI input"));
    require(inputs && status && test && sustain && retry && inputs->getNumItems() == 4,
        "MIDI UI lists None, test input and friendly device names");
    require(inputs->getItemText(2) != inputs->getItemText(3), "equal-name controllers have distinguishable port labels");
    inputs->setSelectedId(2, juce::sendNotificationSync); pump();
    require(test->isShowing() && sustain->isShowing(), "hardware-free test controls appear only for Test input");
    int ons{}, offs{};
    auto send = [&](int note, float v, int) { if (note >= 0) { if (v > 0) ++ons; else ++offs; } };
    sustain->triggerClick(); pump();
    test->triggerClick();
    for (int i = 0; i < 12; ++i) pump(); // JUCE's simulated button press includes a timed down/up flash.
    pipe.process(send); panel.tick();
    require(ons == 1 && offs == 0 && pipe.isNoteOn(60) && status->getText().containsIgnoreCase("activity"),
        "test button uses real MIDI note/CC64 path and activity indicator");
    sustain->triggerClick(); pump(); pipe.process(send);
    require(offs == 1 && !pipe.isNoteOn(60), "test sustain off releases the note");
    inputs->grabKeyboardFocus(); host.getPeer()->handleKeyPress(juce::KeyPress(juce::KeyPress::tabKey)); pump();
    require(panel.hasKeyboardFocus(true) && !inputs->hasKeyboardFocus(false), "MIDI controls preserve accessible Tab navigation");
    inputs->setSelectedId(3, juce::sendNotificationSync); pump();
    require(!test->isShowing() && status->getText().containsIgnoreCase("connected"), "selecting hardware hides simulator controls");
    backend.devices.erase(backend.devices.begin()); panel.refreshDevices();
    require(inputs->getText().containsIgnoreCase("disconnected") && status->getText().containsIgnoreCase("disconnected"),
        "unplug retains selected controller with an explicit disconnected state");
    backend.devices.push_back({"a", "USB Keys"}); panel.refreshDevices();
    require(!inputs->getText().containsIgnoreCase("disconnected"), "reconnection restores the same identifier after list reorder");
    backend.fail = true; inputs->setSelectedId(3, juce::sendNotificationSync); pump();
    require(status->getText().containsIgnoreCase("failed"), "open error is visible with retry guidance");
    backend.fail = false; retry->triggerClick(); pump();
    require(status->getText().containsIgnoreCase("connected"), "Refresh retries a failed open");
    for (auto* c : panel.getChildren()) if (c->isVisible())
        require(panel.getLocalBounds().contains(c->getBounds()), "MIDI controls fit the smallest app width");
    host.removeFromDesktop();
    std::cout << "PASS: MIDI selector, simulated sustain, activity, hot-plug, errors/retry and accessible layout\n";
}
