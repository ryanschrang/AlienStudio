#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "DrumSampler.h"
#include "SessionCapacity.h"
#include <functional>

namespace alien {
class DrumSamplerPanel final : public juce::Component {
public:
    DrumSamplerPanel() : filter("*.wav;*.mp3", "*", "Audio samples"),
        browser(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            juce::File{}, &filter, nullptr) {
        for (auto* c : std::array<juce::Component*, 8>{&track, &pad, &load, &clear, &audition,
            &route, &status, &browser}) addAndMakeVisible(c);
        for (int i = 0; i < 4; ++i) track.addItem("Track " + juce::String(i + 1), i + 1);
        for (int i = 0; i < drumPadCount; ++i) pad.addItem("Pad " + juce::String(i + 1)
            + " / MIDI " + juce::String(drumBaseNote + i), i + 1);
        track.setSelectedId(1); pad.setSelectedId(1);
        track.setName("Drum kit track"); pad.setName("Drum pad to load");
        track.onChange = [this] { refresh(); }; pad.onChange = [this] { refresh(); };
        load.setButtonText("Load selected file"); clear.setButtonText("Clear pad");
        audition.setButtonText("Play pad");
        load.onClick = [this] { if (onLoad && browser.getSelectedFile(0).existsAsFile())
            onLoad(track.getSelectedId() - 1, pad.getSelectedId() - 1, browser.getSelectedFile(0)); };
        clear.onClick = [this] { if (onClear) onClear(track.getSelectedId() - 1, pad.getSelectedId() - 1); };
        audition.onClick = [this] { if (onHit) onHit(track.getSelectedId() - 1, pad.getSelectedId() - 1); };
        route.addItem("MIDI input: Live synth", 1);
        for (int i = 0; i < 4; ++i) route.addItem("MIDI input: Drum track " + juce::String(i + 1), i + 2);
        route.setSelectedId(1); route.setName("MIDI drum routing");
        route.onChange = [this] { if (onRoute) onRoute(route.getSelectedId() - 2); };
        for (int i = 0; i < drumPadCount; ++i) {
            auto& button = pads[static_cast<std::size_t>(i)];
            addAndMakeVisible(button);
            button.setName("Audition drum pad " + juce::String(i + 1));
            button.onClick = [this, i] { pad.setSelectedId(i + 1); if (onHit) onHit(track.getSelectedId() - 1, i); };
        }
        refresh();
    }
    std::function<void(int,int,const juce::File&)> onLoad;
    std::function<void(int,int)> onClear, onHit;
    std::function<void(int)> onRoute;
    void setPaths(const std::array<std::array<juce::String, drumPadCount>, maxSessionTracks>& paths) { names = paths; refresh(); }
    void setTrackNames(const juce::StringArray& labels) {
        const int selection = track.getSelectedId();
        const int inputRoute = route.getSelectedId();
        track.clear(juce::dontSendNotification); route.clear(juce::dontSendNotification);
        route.addItem("MIDI input: Live synth", 1);
        for (int i = 0; i < labels.size(); ++i) { track.addItem(labels[i], i + 1); route.addItem("MIDI input: " + labels[i], i + 2); }
        track.setSelectedId(juce::jlimit(1, labels.size(), selection), juce::dontSendNotification);
        route.setSelectedId(juce::jlimit(1, labels.size() + 1, inputRoute), juce::dontSendNotification);
        refresh();
    }
    void setLiveInput(int destination) { route.setSelectedId(destination+2,juce::dontSendNotification); }
    void setStatus(const juce::String& text) { status.setText(text, juce::dontSendNotification); status.setTooltip(text); }
    void setBusy(bool busy) { load.setEnabled(!busy); clear.setEnabled(!busy); }
    void resized() override {
        const int w = getWidth();
        track.setBounds(12, 8, 110, 28); pad.setBounds(130, 8, 170, 28);
        load.setBounds(308, 8, 145, 28); clear.setBounds(461, 8, 95, 28);
        audition.setBounds(564, 8, 95, 28); route.setBounds(667, 8, w - 679, 28);
        status.setBounds(12, 40, w - 24, 26);
        browser.setBounds(12, 72, w / 2 - 20, getHeight() - 84);
        const int padWidth = (w / 2 - 28) / 4;
        for (int i = 0; i < drumPadCount; ++i)
            pads[static_cast<std::size_t>(i)].setBounds(w / 2 + 8 + (i % 4) * padWidth,
                82 + (i / 4) * 85, padWidth - 6, 75);
    }
private:
    void refresh() {
        const auto t = static_cast<std::size_t>(std::max(0, track.getSelectedId() - 1));
        for (int i = 0; i < drumPadCount; ++i) {
            const auto& path = names[t][static_cast<std::size_t>(i)];
            pads[static_cast<std::size_t>(i)].setButtonText("Pad " + juce::String(i + 1)
                + " / " + juce::String(drumBaseNote + i) + "\n"
                + (path.isEmpty() ? "Empty" : juce::File(path).getFileName()));
        }
        setStatus("Load a sample to enable this track's kit. Sequence MIDI 36–43 in its piano roll. Pads 3/4 choke each other.");
    }
    juce::WildcardFileFilter filter;
    juce::FileBrowserComponent browser;
    juce::ComboBox track, pad, route;
    juce::TextButton load, clear, audition;
    juce::Label status;
    std::array<juce::TextButton, drumPadCount> pads;
    std::array<std::array<juce::String, drumPadCount>, maxSessionTracks> names{};
};
} // namespace alien
