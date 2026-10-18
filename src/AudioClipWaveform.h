#pragma once

#include "SessionAudioClip.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <utility>

namespace alien {

class AudioClipWaveform final : public juce::Component {
public:
    void setClip(const SessionAudioClip* replacement, juce::String displayName) {
        clip = replacement;
        name = std::move(displayName);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff111724));
        g.setColour(juce::Colour(0xff354154));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 5.0f, 1.0f);
        if (clip == nullptr || clip->waveformCount == 0) {
            g.setColour(juce::Colour(0xff9aabba));
            g.drawText("Select a clip, then import a WAV or MP3", getLocalBounds().reduced(10),
                juce::Justification::centredLeft);
            return;
        }

        const auto area = getLocalBounds().reduced(8, 18).toFloat();
        const float laneHeight = clip->channels == 2 ? area.getHeight() * 0.5f : area.getHeight();
        g.setColour(juce::Colour(0xff314155));
        g.drawHorizontalLine(static_cast<int>(area.getY() + laneHeight * 0.5f), area.getX(), area.getRight());
        if (clip->channels == 2)
            g.drawHorizontalLine(static_cast<int>(area.getY() + laneHeight + laneHeight * 0.5f), area.getX(), area.getRight());

        g.setColour(juce::Colour(0xff54e4d0));
        const int columns = juce::jmax(1, static_cast<int>(area.getWidth()));
        for (int x = 0; x < columns; ++x) {
            const auto index = std::min(clip->waveformCount - 1,
                static_cast<std::size_t>(x) * clip->waveformCount / static_cast<std::size_t>(columns));
            const auto& peak = clip->waveform[index];
            drawPeak(g, area.getX() + static_cast<float>(x), area.getY(), laneHeight,
                peak.leftMin, peak.leftMax);
            if (clip->channels == 2)
                drawPeak(g, area.getX() + static_cast<float>(x), area.getY() + laneHeight, laneHeight,
                    peak.rightMin, peak.rightMax);
        }
        g.setColour(juce::Colour(0xffb9ced8));
        g.drawText(name, 8, 2, getWidth() - 16, 16, juce::Justification::centredLeft, true);
    }

private:
    static void drawPeak(juce::Graphics& g, float x, float y, float height, float minimum, float maximum) {
        const float centre = y + height * 0.5f;
        const float amplitude = height * 0.43f;
        g.drawVerticalLine(static_cast<int>(x), centre - maximum * amplitude, centre - minimum * amplitude);
    }

    const SessionAudioClip* clip{};
    juce::String name;
};

} // namespace alien
