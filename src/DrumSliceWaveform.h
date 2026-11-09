#pragma once
#include "SessionAudioClip.h"
#include "StudioTheme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace alien {
class DrumSliceWaveform final : public juce::Component {
public:
    DrumSliceWaveform() {
        setName("Drum slice waveform");
        setTitle("Applied drum slice on the source waveform");
        setInterceptsMouseClicks(false, false);
    }

    void setAsset(std::shared_ptr<const SessionAudioClip> next) {
        if (asset == next) return;
        asset = std::move(next);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        g.fillAll(col::inset);
        const auto* source = asset && asset->sliceSource ? asset->sliceSource.get() : asset.get();
        if (!source || !source->frames || !source->waveformCount) {
            g.setColour(col::textDim);
            g.drawFittedText("No source waveform", getLocalBounds().reduced(4), juce::Justification::centred, 1);
            return;
        }

        auto bounds = getLocalBounds().reduced(6, 4);
        auto caption = bounds.removeFromTop(18);
        const auto start = asset->sliceSource ? asset->sliceOffset : 0;
        const auto end = start + asset->frames;
        g.setColour(col::textDim);
        g.drawFittedText("Applied frames " + juce::String(start) + "–" + juce::String(end),
                         caption, juce::Justification::centredLeft, 1);
        if (bounds.isEmpty()) return;

        const auto area = bounds.toFloat();
        const auto xForFrame = [&](std::size_t frame) {
            return area.getX() + area.getWidth() * static_cast<float>(frame) / static_cast<float>(source->frames);
        };
        const float left = xForFrame(start), right = xForFrame(end);
        g.setColour(col::accent.withAlpha(.22f));
        g.fillRect(left, area.getY(), right - left, area.getHeight());
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(bounds);

        const bool stereo = source->channels == 2;
        const float height = area.getHeight() / (stereo ? 2.0f : 1.0f);
        const float binWidth = area.getWidth() / static_cast<float>(source->waveformCount);
        const auto drawPeak = [&](float x, float centre, float low, float high) {
            const auto y = centre - std::clamp(high, -1.0f, 1.0f) * height * .45f;
            const auto bottom = centre - std::clamp(low, -1.0f, 1.0f) * height * .45f;
            g.fillRect(x, y, std::max(1.0f, binWidth), std::max(1.0f, bottom - y));
        };
        g.setColour(col::textDim);
        for (std::size_t i = 0; i < source->waveformCount; ++i) {
            const auto& peak = source->waveform[i];
            const auto x = area.getX() + static_cast<float>(i) * binWidth;
            drawPeak(x, area.getY() + height * .5f, peak.leftMin, peak.leftMax);
            if (stereo) drawPeak(x, area.getY() + height * 1.5f, peak.rightMin, peak.rightMax);
        }
        g.setColour(col::accent);
        g.fillRect(left, area.getY(), 1.0f, area.getHeight());
        g.fillRect(std::min(right, area.getRight() - 1.0f), area.getY(), 1.0f, area.getHeight());
    }

private:
    std::shared_ptr<const SessionAudioClip> asset;
};
} // namespace alien
