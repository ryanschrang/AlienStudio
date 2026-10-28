#pragma once
#include "FlLook.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>

namespace alien {

// One clip slot in the Session grid. Left-click launches, right-click edits.
// The owner describes what the slot holds with setVisual(); the button text is
// still maintained for accessibility but the slot draws its own clip tile.
class SessionClipButton final : public juce::TextButton {
public:
    enum class Kind { empty, midi, audio, offline, rack };
    struct Visual {
        Kind kind{Kind::empty};
        juce::Colour colour{0xff4fe0b6};
        juce::String label;
        bool playing{}, queued{}, selected{};
        float progress{-1.0f}; // 0..1 while playing a MIDI clip, otherwise negative.

        bool operator==(const Visual& o) const {
            return kind == o.kind && colour == o.colour && label == o.label && playing == o.playing
                && queued == o.queued && selected == o.selected
                && std::abs(progress - o.progress) < 0.002f;
        }
    };

    std::function<void()> onRightClick;

    void setVisual(const Visual& v) {
        const bool blinking = v.queued;
        if (v == visual && !blinking) return;
        visual = v;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (event.mods.isRightButtonDown()) {
            if (onRightClick) onRightClick();
            return;
        }
        juce::TextButton::mouseUp(event);
    }

    // Where the launch triangle sits: a small dark button at the left end of the tile, as in Ableton's clip slots.
    static constexpr float launchWidth = 24.0f;

    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        const float radius = 3.0f;
        const auto& v = visual;
        const auto ink = juce::Colour(0xff0d0e10);
        const auto launch = r.withWidth(launchWidth);
        auto triangle = [&](juce::Colour colour) {
            const auto tri = juce::Rectangle<float>(10.0f, 10.0f).withCentre(launch.getCentre());
            juce::Path p;
            p.addTriangle(tri.getX() + 1.0f, tri.getY(), tri.getX() + 1.0f, tri.getBottom(), tri.getRight(), tri.getCentreY());
            g.setColour(colour);
            g.fillPath(p);
        };

        if (v.kind == Kind::empty) {
            // An empty slot is a dark raised tile; the launch triangle is lit while the track sits on it.
            auto face = juce::Colour(over ? 0xff353840 : 0xff2d3036);
            if (down) face = face.brighter(0.1f);
            ui::fl::paintRaised(g, r, face, radius, 0.18f, 0.34f);
            triangle(juce::Colour(v.playing ? 0xff9ea3ab : over ? 0xff6f747c : 0xff4a4e56));
            if (over) {
                g.setColour(juce::Colour(0xff9096a0));
                g.setFont(juce::FontOptions(11.5f));
                g.drawText("Empty", r.withTrimmedLeft(launchWidth + 6.0f), juce::Justification::centredLeft, true);
            }
        } else {
            // Offline audio (file not found) is drawn in a muted red that cannot be mistaken for a playable clip.
            auto fill = v.kind == Kind::offline ? juce::Colour(0xff4a2a2e)
                : v.playing ? v.colour : v.colour.withMultipliedSaturation(0.75f).withMultipliedBrightness(0.62f);
            if (down) fill = fill.brighter(0.12f);
            else if (over) fill = fill.brighter(0.06f);
            ui::fl::paintRaised(g, r, fill, radius, v.playing ? 0.30f : 0.38f, v.playing ? 0.20f : 0.24f);
            const auto textColour = v.kind == Kind::offline ? juce::Colour(0xffe8b4b8)
                : v.playing ? ink : juce::Colour(0xfff1f2f4);

            // The launch button: a darker inset square with the triangle on it.
            g.setColour(juce::Colours::black.withAlpha(v.playing ? 0.16f : 0.26f));
            g.fillRoundedRectangle(launch.reduced(3.0f, 4.0f), 2.5f);
            triangle(v.playing ? ink : textColour.withAlpha(0.9f));

            auto textArea = r.withTrimmedLeft(launchWidth + 6.0f).withTrimmedRight(6.0f);
            if (v.kind == Kind::audio) {
                // Tiny waveform badge marks audio clips.
                auto badge = textArea.removeFromRight(14.0f).withSizeKeepingCentre(12.0f, 12.0f);
                const float hs[] = {0.3f, 0.8f, 0.5f, 1.0f, 0.4f};
                g.setColour(textColour.withAlpha(0.7f));
                for (int i = 0; i < 5; ++i) {
                    const float bh = badge.getHeight() * hs[i];
                    g.fillRect(badge.getX() + static_cast<float>(i) * 2.6f, badge.getCentreY() - bh * 0.5f, 1.4f, bh);
                }
            }
            if (v.kind == Kind::offline) {
                auto badge = textArea.removeFromRight(14.0f);
                g.setColour(juce::Colour(0xffffb547));
                g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
                g.drawText("!", badge, juce::Justification::centred);
            }
            g.setColour(textColour);
            g.setFont(juce::FontOptions(12.5f, v.playing ? juce::Font::bold : juce::Font::plain));
            g.drawText(v.label, textArea, juce::Justification::centredLeft, true);

            if (v.playing && v.progress >= 0.0f) {
                auto bar = r.reduced(3.0f, 0.0f).removeFromBottom(3.0f).translated(0.0f, -2.0f);
                g.setColour(ink.withAlpha(0.25f));
                g.fillRoundedRectangle(bar, 1.5f);
                g.setColour(ink.withAlpha(0.7f));
                g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, v.progress)), 1.5f);
            }
        }

        if (v.queued) {
            const bool phase = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
            g.setColour(juce::Colour(0xffffb547).withAlpha(phase ? 1.0f : 0.35f));
            g.drawRoundedRectangle(r.reduced(0.5f), radius, 2.0f);
        }
        if (v.selected) {
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.drawRoundedRectangle(r.reduced(1.0f), radius, 1.5f);
        }
        if (hasKeyboardFocus(false)) {
            g.setColour(juce::Colour(0xff4fe0b6));
            g.drawRoundedRectangle(r.reduced(2.5f), radius, 1.0f);
        }
    }

private:
    Visual visual;
};

} // namespace alien
