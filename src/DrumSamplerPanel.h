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
        for (auto* c : std::array<juce::Component*, 17>{&track, &pad, &load, &clear, &audition, &slice, &autoSlice,
            &route, &status, &browser, &startLabel, &endLabel, &startFrame, &endFrame, &applySlice, &wholeSample, &sourceLength}) addAndMakeVisible(c);
        for (int i = 0; i < 4; ++i) track.addItem("Track " + juce::String(i + 1), i + 1);
        for (int i = 0; i < drumPadCount; ++i) pad.addItem("Pad " + juce::String(i + 1)
            + " / MIDI " + juce::String(drumBaseNote + i), i + 1);
        track.setSelectedId(1); pad.setSelectedId(1);
        track.setName("Drum kit track"); pad.setName("Drum pad to load");
        track.onChange = [this] { refresh(); }; pad.onChange = [this] { refresh(); };
        load.setButtonText("Load selected file"); clear.setButtonText("Clear pad");
        audition.setButtonText("Play pad");
        slice.setButtonText("Slice to 8 pads"); slice.setName("Slice sample to 8 pads");
        slice.setTooltip("Replace all eight pads with equal slices of the selected pad sample. Undo restores the kit. Pads 3 and 4 retain their hat choke behavior.");
        slice.onClick = [this] { if (onSlice) onSlice(track.getSelectedId() - 1, pad.getSelectedId() - 1); };
        autoSlice.setButtonText("Auto slice"); autoSlice.setName("Auto slice sample at attacks");
        autoSlice.setTooltip("Find up to eight slices at attacks in this sample range (one minute maximum). Replaces all pads; unused pads are cleared. Undo restores the kit. No useful attacks leaves it unchanged. Pads 3/4 keep their hat choke.");
        autoSlice.onClick = [this] { if (!editingBusy && onAutoSlice) onAutoSlice(editingTarget); };
        startLabel.setText("Start frame", juce::dontSendNotification);
        endLabel.setText("End (exclusive)", juce::dontSendNotification);
        startFrame.setName("Slice start frame"); endFrame.setName("Slice end frame");
        startFrame.setInputRestrictions(16); endFrame.setInputRestrictions(16);
        startFrame.setTooltip("First source frame to play, starting at zero.");
        endFrame.setTooltip("First source frame not played; up to the original sample length.");
        applySlice.setButtonText("Apply range"); applySlice.setName("Apply drum slice range");
        wholeSample.setButtonText("Whole sample"); wholeSample.setName("Restore whole drum sample");
        applySlice.onClick = [this] {
            const auto start = startFrame.getText().trim(), end = endFrame.getText().trim();
            if (start.isEmpty() || end.isEmpty() || start.length() > 9 || end.length() > 9
                || !start.containsOnly("0123456789") || !end.containsOnly("0123456789")) {
                setStatus("Enter whole source-frame numbers for Start and End."); return;
            }
            if (end.getIntValue() <= start.getIntValue()) {
                setStatus("Start must be before the exclusive End frame."); return;
            }
            applyRange({start.getIntValue(), end.getIntValue()});
        };
        wholeSample.onClick = [this] { applyRange({}); };
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
    std::function<void(int,int)> onClear, onHit, onSlice;
    std::function<void(int)> onRoute;
    std::function<bool(const DrumSliceTarget&, DrumSlice)> onApplySlice;
    std::function<void(const DrumSliceTarget&)> onAutoSlice;
    void setSliceTargets(const std::array<std::array<DrumSliceTarget, drumPadCount>, maxSessionTracks>& value) { targets = value; refreshSliceEditor(); }
    void setPaths(const std::array<std::array<juce::String, drumPadCount>, maxSessionTracks>& paths,
                  const std::array<std::array<DrumSlice, drumPadCount>, maxSessionTracks>& ranges = {}) { names = paths; slices = ranges; refresh(); }
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
    void setBusy(bool busy) { editingBusy = busy; load.setEnabled(!busy); clear.setEnabled(!busy); slice.setEnabled(!busy); updateSliceEnabled(); }
    void resized() override {
        const int w = getWidth();
        track.setBounds(12, 8, 110, 28); pad.setBounds(130, 8, 170, 28);
        load.setBounds(308, 8, 145, 28); clear.setBounds(461, 8, 95, 28);
        audition.setBounds(564, 8, 95, 28); route.setBounds(667, 8, w - 679, 28);
        slice.setBounds(12, 40, 140, 26); autoSlice.setBounds(160, 40, 110, 26); status.setBounds(278, 40, w - 290, 26);
        startLabel.setBounds(12, 72, 82, 26); startFrame.setBounds(96, 72, 96, 26);
        endLabel.setBounds(202, 72, 120, 26); endFrame.setBounds(324, 72, 96, 26);
        applySlice.setBounds(432, 72, 110, 26); wholeSample.setBounds(550, 72, 125, 26);
        sourceLength.setBounds(683, 72, w - 695, 26);
        browser.setBounds(12, 108, w / 2 - 20, getHeight() - 120);
        const int padWidth = (w / 2 - 28) / 4;
        for (int i = 0; i < drumPadCount; ++i)
            pads[static_cast<std::size_t>(i)].setBounds(w / 2 + 8 + (i % 4) * padWidth,
                118 + (i / 4) * 85, padWidth - 6, 75);
    }
private:
    void updateSliceEnabled() {
        const bool enabled = !editingBusy && editingTarget.asset && !editingTarget.frozen && !editingTarget.asset->streamed();
        for (auto* c : std::array<juce::Component*, 4>{&startFrame, &endFrame, &applySlice, &wholeSample}) c->setEnabled(enabled);
        autoSlice.setEnabled(enabled);
    }
    void refreshSliceEditor() {
        const auto t = static_cast<std::size_t>(std::max(0, track.getSelectedId() - 1));
        const auto p = static_cast<std::size_t>(std::max(0, pad.getSelectedId() - 1));
        const auto& next = targets[t][p]; const auto range = slices[t][p];
        if (!hasShownTarget || next != editingTarget || range != shownRange) {
            editingTarget = next; shownRange = range; hasShownTarget = true;
            const auto asset = next.asset;
            const auto source = asset && asset->sliceSource ? asset->sliceSource : asset;
            const int frames = source ? static_cast<int>(source->frames) : 0;
            startFrame.setText(juce::String(range.start), false);
            endFrame.setText(juce::String(range.whole() ? frames : range.end), false);
            sourceLength.setText(source ? "Source: " + juce::String(frames) + " frames" : "Source offline or empty", juce::dontSendNotification);
        }
        updateSliceEnabled();
    }
    void applyRange(DrumSlice range) {
        if (editingBusy || !editingTarget.asset || editingTarget.frozen) return;
        const auto target = editingTarget;
        if (!onApplySlice || !onApplySlice(target, range))
            setStatus("Slice unchanged. Check Start < End within the source, or refresh if the sample changed.");
    }
    void refresh() {
        const auto t = static_cast<std::size_t>(std::max(0, track.getSelectedId() - 1));
        for (int i = 0; i < drumPadCount; ++i) {
            const auto& path = names[t][static_cast<std::size_t>(i)];
            pads[static_cast<std::size_t>(i)].setButtonText("Pad " + juce::String(i + 1)
                + " / " + juce::String(drumBaseNote + i) + "\n"
                + (path.isEmpty() ? "Empty" : juce::File(path).getFileName())
                + (slices[t][i].whole() ? juce::String{} : "\nFrames " + juce::String(slices[t][i].start) + "-" + juce::String(slices[t][i].end)));
        }
        setStatus("Load a sample to enable this track's kit. Sequence MIDI 36–43 in its piano roll. Pads 3/4 choke each other.");
        refreshSliceEditor();
    }
    juce::WildcardFileFilter filter;
    juce::FileBrowserComponent browser;
    juce::ComboBox track, pad, route;
    juce::TextButton load, clear, audition, slice, autoSlice;
    juce::Label status, startLabel, endLabel, sourceLength;
    juce::TextEditor startFrame, endFrame;
    juce::TextButton applySlice, wholeSample;
    std::array<std::array<DrumSliceTarget, drumPadCount>, maxSessionTracks> targets{};
    DrumSliceTarget editingTarget;
    DrumSlice shownRange;
    bool hasShownTarget{}, editingBusy{};
    std::array<juce::TextButton, drumPadCount> pads;
    std::array<std::array<juce::String, drumPadCount>, maxSessionTracks> names{};
    std::array<std::array<DrumSlice, drumPadCount>, maxSessionTracks> slices{};
};
} // namespace alien
