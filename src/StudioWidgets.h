#pragma once
#include "StudioTheme.h"
#include <cmath>
#include <functional>

// Reusable studio UI building blocks. All of these run on the message thread
// only and read nothing from the audio thread directly.
namespace alien::ui {

// A plain container whose painting and layout are supplied by its owner.
class PaintedPanel final : public juce::Component {
public:
    std::function<void(juce::Graphics&)> onPaint;
    std::function<void()> onLayout;
    void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
    void resized() override { if (onLayout) onLayout(); }
};

// TextButton with an optional vector glyph to the left of its label. When
// iconOnly is set the button text is kept for accessibility and tooltips but
// only the glyph is drawn.
class GlyphButton : public juce::TextButton {
public:
    GlyphButton() = default;
    explicit GlyphButton(Glyph g, bool onlyIcon = false) : glyph(g), iconOnly(onlyIcon) {}

    void setGlyph(Glyph g) { if (glyph != g) { glyph = g; repaint(); } }
    void setIconOnly(bool b) { iconOnly = b; repaint(); }
    void setGlyphColour(juce::Colour c) { glyphColour = c; hasGlyphColour = true; repaint(); }

    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto& lf = getLookAndFeel();
        lf.drawButtonBackground(g, *this, findColour(getToggleState() ? buttonOnColourId : buttonColourId),
                                over, down);
        const auto style = getProperties().getWithDefault("style", {}).toString();
        auto textColour = findColour(getToggleState() ? textColourOnId : textColourOffId);
        if (style == "tab" || style == "ghost")
            textColour = getToggleState() ? colours::text : (over ? colours::text : colours::textDim);
        textColour = textColour.withMultipliedAlpha(isEnabled() ? 1.0f : 0.45f);
        auto area = getLocalBounds().toFloat();
        const float glyphSize = juce::jmin(16.0f, area.getHeight() * 0.5f);
        if (iconOnly || getButtonText().isEmpty()) {
            drawGlyph(g, glyph, area.withSizeKeepingCentre(glyphSize, glyphSize),
                      hasGlyphColour && !getToggleState() ? glyphColour : textColour);
            return;
        }
        const auto f = font(juce::jmin(13.5f, area.getHeight() * 0.48f), style == "accent");
        const float textWidth = juce::GlyphArrangement::getStringWidth(f, getButtonText());
        const float gap = glyph == Glyph::none ? 0.0f : 7.0f;
        const float total = (glyph == Glyph::none ? 0.0f : glyphSize) + gap + textWidth;
        float left = juce::jmax(area.getX() + 8.0f, area.getCentreX() - total * 0.5f);
        if (glyph != Glyph::none) {
            drawGlyph(g, glyph, juce::Rectangle<float>(left, area.getCentreY() - glyphSize * 0.5f, glyphSize, glyphSize),
                      hasGlyphColour && !getToggleState() ? glyphColour : textColour);
            left += glyphSize + gap;
        }
        g.setColour(textColour);
        g.setFont(f);
        g.drawText(getButtonText(), juce::Rectangle<float>(left, area.getY(), area.getRight() - left - 6.0f, area.getHeight()),
                   juce::Justification::centredLeft, true);
    }

private:
    Glyph glyph{Glyph::none};
    bool iconOnly{};
    juce::Colour glyphColour;
    bool hasGlyphColour{};
};

// Small original brand mark drawn as vectors: a rounded tile with an
// abstract visitor silhouette (dome head and two eyes).
class BrandMark final : public juce::Component {
public:
    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds().toFloat();
        const float s = juce::jmin(r.getWidth(), r.getHeight());
        const auto tile = juce::Rectangle<float>(s, s).withCentre(r.getCentre()).reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(colours::accent, tile.getX(), tile.getY(),
                                               juce::Colour(0xff2a9d8f), tile.getRight(), tile.getBottom(), false));
        g.fillRoundedRectangle(tile, s * 0.24f);
        juce::Path head;
        const auto h = tile.reduced(s * 0.2f);
        head.addEllipse(h.getX(), h.getY(), h.getWidth(), h.getHeight() * 1.08f);
        g.setColour(colours::accentInk);
        g.fillPath(head);
        g.setColour(colours::accent.brighter(0.4f));
        const float eyeW = h.getWidth() * 0.26f, eyeH = h.getHeight() * 0.2f;
        g.fillEllipse(h.getCentreX() - eyeW - h.getWidth() * 0.04f, h.getCentreY() - eyeH * 0.2f, eyeW, eyeH);
        g.fillEllipse(h.getCentreX() + h.getWidth() * 0.04f, h.getCentreY() - eyeH * 0.2f, eyeW, eyeH);
    }
};

// Vertical peak meter with a smooth ballistic fall, peak-hold line and a
// latched clip indicator. Feed it linear peak values from the GUI timer.
class LevelMeter final : public juce::Component {
public:
    void setPeak(float linear) {
        const auto now = juce::Time::getMillisecondCounter();
        const float target = juce::jmax(0.0f, linear);
        level = target > level ? target : level * 0.82f;
        if (level > hold || now > holdUntil) { hold = level; holdUntil = now + 900; }
        if (target >= 1.0f) clipUntil = now + 1500;
        repaint();
    }

    static float toPosition(float linear) {
        if (linear <= 0.0f) return 0.0f;
        const float db = 20.0f * std::log10(linear);
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 6.0f, 0.0f, 1.0f));
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        const auto clipLed = r.removeFromTop(5.0f);
        r.removeFromTop(2.0f);
        const bool clipping = juce::Time::getMillisecondCounter() < clipUntil;
        g.setColour(clipping ? colours::danger : colours::inset);
        g.fillRoundedRectangle(clipLed, 1.5f);
        g.setColour(colours::inset);
        g.fillRoundedRectangle(r, 2.0f);
        const float pos = toPosition(level);
        if (pos > 0.0f) {
            auto lit = r.reduced(1.0f);
            lit = lit.removeFromBottom(lit.getHeight() * pos);
            juce::ColourGradient grad(juce::Colour(0xff35d08a), 0.0f, r.getBottom(),
                                      colours::danger, 0.0f, r.getY(), false);
            grad.addColour(0.72, juce::Colour(0xff9be36b));
            grad.addColour(0.88, juce::Colour(0xffffc94a));
            g.setGradientFill(grad);
            g.fillRect(lit);
        }
        const float holdPos = toPosition(hold);
        if (holdPos > 0.01f) {
            g.setColour(colours::text.withAlpha(0.8f));
            g.fillRect(r.getX() + 1.0f, r.getBottom() - 1.0f - (r.getHeight() - 2.0f) * holdPos, r.getWidth() - 2.0f, 1.5f);
        }
    }

private:
    float level{}, hold{};
    juce::uint32 holdUntil{}, clipUntil{};
};

// LCD-style transport readout: position, tempo (a draggable child slider),
// meter and transport state.
class TransportDisplay final : public juce::Component {
public:
    explicit TransportDisplay(juce::Slider& tempoSlider) : tempo(tempoSlider) {
        addAndMakeVisible(tempo);
        setInterceptsMouseClicks(false, true);
    }

    void setPosition(int step, bool isPlaying, const juce::String& source) {
        if (step == currentStep && isPlaying == playing && source == mode) return;
        currentStep = step; playing = isPlaying; mode = source;
        repaint();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(6, 4);
        r.removeFromLeft(positionWidth());
        r.removeFromLeft(14);
        tempo.setBounds(r.removeFromLeft(74).withTrimmedBottom(12));
    }

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(colours::lcd);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(colours::accent.withAlpha(0.08f));
        g.drawRoundedRectangle(bounds.reduced(1.5f), 5.0f, 1.0f);

        auto r = getLocalBounds().reduced(6, 4);
        auto pos = r.removeFromLeft(positionWidth());
        const int s = juce::jmax(0, currentStep);
        const juce::String text = currentStep < 0 ? "1 . 1 . 1"
            : juce::String(s / 16 + 1) + " . " + juce::String((s % 16) / 4 + 1) + " . " + juce::String(s % 4 + 1);
        g.setColour(playing ? colours::lcdText : colours::lcdText.withAlpha(0.75f));
        g.setFont(monoFont(21.0f, true));
        g.drawText(text, pos.withTrimmedBottom(12), juce::Justification::centred, false);
        drawCaption(g, "Bar  Beat  16th", pos.removeFromBottom(12), colours::lcdText.withAlpha(0.45f),
                    juce::Justification::centred);

        r.removeFromLeft(4);
        g.setColour(colours::lcdText.withAlpha(0.18f));
        g.fillRect(static_cast<float>(r.getX()), static_cast<float>(r.getY()) + 4.0f, 1.0f,
                   static_cast<float>(r.getHeight()) - 8.0f);
        r.removeFromLeft(10);
        drawCaption(g, "BPM", r.removeFromLeft(74).removeFromBottom(12), colours::lcdText.withAlpha(0.45f),
                    juce::Justification::centred);
        g.setColour(colours::lcdText.withAlpha(0.18f));
        g.fillRect(static_cast<float>(r.getX()), static_cast<float>(r.getY()) + 4.0f, 1.0f,
                   static_cast<float>(r.getHeight()) - 8.0f);
        r.removeFromLeft(8);
        if (r.getWidth() > 40) {
            g.setColour(colours::lcdText.withAlpha(0.8f));
            g.setFont(monoFont(15.0f, true));
            g.drawText("4/4", r.withTrimmedBottom(12), juce::Justification::centred, false);
            drawCaption(g, mode, r.removeFromBottom(12), colours::lcdText.withAlpha(0.45f),
                        juce::Justification::centred);
        }
    }

private:
    int positionWidth() const { return juce::jmin(118, getWidth() / 2); }

    juce::Slider& tempo;
    int currentStep{-1};
    bool playing{};
    juce::String mode{"Steps"};
};

} // namespace alien::ui
