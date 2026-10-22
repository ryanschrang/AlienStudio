#pragma once
#include "StudioTheme.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

// The drawing FL Studio's channel rack and Playlist use for their controls: raised, bevelled
// button faces, round lights, small shaded knobs and the grip lines on a name button. The Session
// view uses these; the colours stay Alien Studio's.
namespace alien::ui::fl {

// A raised, bevelled face: light on top, dark at the bottom, a bright edge along the top and a dark outline.
inline void paintRaised(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour face, float radius,
                        float lift = 0.22f, float sink = 0.30f) {
    g.setGradientFill(juce::ColourGradient(face.brighter(lift), r.getX(), r.getY(), face.darker(sink), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle(r, radius);
    g.setColour(juce::Colours::white.withAlpha(0.13f));
    g.drawLine(r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f, 1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.22f));
    g.drawLine(r.getX() + radius, r.getBottom() - 1.5f, r.getRight() - radius, r.getBottom() - 1.5f, 1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.65f));
    g.drawRoundedRectangle(r, radius, 1.0f);
}

// A round light: glowing in `colour` when lit, dark when not, and ringed when `ringed` (a muted channel).
inline void paintLight(juce::Graphics& g, juce::Rectangle<float> led, juce::Colour colour, bool lit, bool ringed) {
    namespace col = ui::colours;
    g.setColour(col::inset);
    g.fillEllipse(led.expanded(1.5f));
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawEllipse(led.expanded(1.5f), 1.0f);
    if (lit) {
        g.setColour(colour.withAlpha(0.13f)); g.fillEllipse(led.expanded(4.5f));
        g.setColour(colour.withAlpha(0.20f)); g.fillEllipse(led.expanded(2.5f));
        g.setGradientFill(juce::ColourGradient(colour.brighter(0.55f), led.getCentreX() - 2.0f, led.getY(), colour.darker(0.25f), led.getCentreX(), led.getBottom(), false));
        g.fillEllipse(led);
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillEllipse(led.getX() + 2.2f, led.getY() + 1.4f, 3.2f, 2.2f);
    } else {
        g.setColour(col::lineStrong.darker(0.4f));
        g.fillEllipse(led);
    }
    if (ringed) { g.setColour(col::mute.withAlpha(0.9f)); g.drawEllipse(led.expanded(3.5f), 1.4f); }
}

// A small knob with a pointer. `amount` is 0..1 around a 270 degree sweep; `moved` puts a dot in `colour` where the
// pointer ends (the knob is off its default).
inline void paintKnob(juce::Graphics& g, juce::Rectangle<float> r, float amount, bool moved, juce::Colour colour, bool active) {
    namespace col = ui::colours;
    const float start = juce::MathConstants<float>::pi * 1.25f, range = juce::MathConstants<float>::pi * 1.5f;
    const auto cap = r.withSizeKeepingCentre(20.0f, 20.0f);
    const auto centre = cap.getCentre();
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(cap.translated(0.0f, 1.5f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(active ? 0xff646b75 : 0xff4d525a), centre.x - 4.0f, centre.y - 6.0f,
                                           juce::Colour(0xff22252a), centre.x + 5.0f, centre.y + 9.0f, true));
    g.fillEllipse(cap);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawEllipse(cap, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawEllipse(cap.reduced(1.3f), 0.8f);
    const float angle = start + range * juce::jlimit(0.0f, 1.0f, amount);
    const float dx = std::sin(angle), dy = -std::cos(angle);
    g.setColour(active ? col::text : col::textFaint);
    g.drawLine(centre.x + dx * 2.5f, centre.y + dy * 2.5f, centre.x + dx * 8.2f, centre.y + dy * 8.2f, 2.0f);
    if (moved && active) { g.setColour(colour); g.fillEllipse(centre.x + dx * 8.2f - 1.6f, centre.y + dy * 8.2f - 1.6f, 3.2f, 3.2f); }
}

// Grip lines at the right end of a name button.
inline void paintGrip(juce::Graphics& g, juce::Rectangle<float> button) {
    for (int i = 0; i < 4; ++i) {
        const float x = button.getRight() - 15.0f + static_cast<float>(i) * 3.0f;
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRect(x, button.getY() + 4.0f, 1.0f, button.getHeight() - 8.0f);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.fillRect(x + 1.0f, button.getY() + 4.0f, 1.0f, button.getHeight() - 8.0f);
    }
}

} // namespace alien::ui::fl
