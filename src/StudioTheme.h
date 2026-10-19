#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>

// Alien Studio visual system: one set of design tokens, a LookAndFeel that
// draws every stock JUCE control in the studio style, and a small vector
// glyph library so icons stay crisp at any DPI without bundling image files.
namespace alien::ui {

namespace colours {
// Neutral graphite surfaces, darkest to lightest.
inline const juce::Colour window   {0xff0f1013};
inline const juce::Colour surface  {0xff16171b};
inline const juce::Colour raised   {0xff1d1f24};
inline const juce::Colour control  {0xff272a30};
inline const juce::Colour hover    {0xff31343b};
inline const juce::Colour inset    {0xff0b0c0e};
inline const juce::Colour line     {0xff26282d};
inline const juce::Colour lineStrong{0xff363940};
// Text.
inline const juce::Colour text     {0xffe8eaed};
inline const juce::Colour textDim  {0xff9ea3ab};
inline const juce::Colour textFaint{0xff686d75};
// Brand accent and semantic colours.
inline const juce::Colour accent   {0xff4fe0b6};
inline const juce::Colour accentInk{0xff06231a};
inline const juce::Colour warning  {0xffffb547};
inline const juce::Colour danger   {0xffff5d5d};
inline const juce::Colour mute     {0xffffc94a};
inline const juce::Colour solo     {0xff5aa9ff};
inline const juce::Colour lcd      {0xff0a1411};
inline const juce::Colour lcdText  {0xff8ff5d6};
} // namespace colours

inline juce::Colour trackColour(int index) {
    static const std::array<juce::Colour, 8> palette{
        juce::Colour(0xff4fe0b6), juce::Colour(0xff9d85ff), juce::Colour(0xffffb547), juce::Colour(0xffff6f9f),
        juce::Colour(0xff5aa9ff), juce::Colour(0xffa6e05a), juce::Colour(0xffff8a5c), juce::Colour(0xff4fd3e0)};
    return palette[static_cast<std::size_t>(((index % 8) + 8) % 8)];
}

inline juce::String sansTypefaceName() {
#if JUCE_WINDOWS
    return "Segoe UI";
#elif JUCE_MAC
    return "Helvetica Neue";
#else
    return "Inter";
#endif
}

inline juce::String monoTypefaceName() {
#if JUCE_WINDOWS
    return "Consolas";
#elif JUCE_MAC
    return "Menlo";
#else
    return "DejaVu Sans Mono";
#endif
}

// Build a juce::String from UTF-8 bytes (write non-ASCII as \x escapes so the
// source stays portable across compilers and code pages).
inline juce::String utf8(const char* bytes) { return juce::String(juce::CharPointer_UTF8(bytes)); }

inline juce::Font font(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font monoFont(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(monoTypefaceName(), height,
        bold ? juce::Font::bold : juce::Font::plain));
}

// Small uppercase caption used for section labels throughout the UI.
inline void drawCaption(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                        juce::Colour colour = colours::textFaint,
                        juce::Justification just = juce::Justification::centredLeft) {
    g.setColour(colour);
    auto f = font(10.5f, true);
    f.setExtraKerningFactor(0.08f);
    g.setFont(f);
    g.drawText(text.toUpperCase(), area, just, true);
}

// Card surface with a hairline border, used for grouped panels.
inline void drawCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = colours::raised,
                     float radius = 6.0f) {
    g.setColour(fill);
    g.fillRoundedRectangle(r, radius);
    g.setColour(colours::line);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
}

enum class Glyph { none, play, stop, plus, minus, open, save, collect, devices, edit,
                   waveform, learn, close, chevronDown };

inline void drawGlyph(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> r, juce::Colour colour) {
    const float s = juce::jmin(r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre(s, s);
    const float x = r.getX(), y = r.getY();
    const float stroke = juce::jmax(1.4f, s * 0.11f);
    const juce::PathStrokeType pen(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour(colour);
    juce::Path p;
    switch (glyph) {
    case Glyph::none: return;
    case Glyph::play:
        p.addTriangle(x + s * 0.22f, y + s * 0.12f, x + s * 0.22f, y + s * 0.88f, x + s * 0.88f, y + s * 0.5f);
        g.fillPath(p);
        return;
    case Glyph::stop:
        g.fillRoundedRectangle(r.reduced(s * 0.18f), s * 0.08f);
        return;
    case Glyph::plus:
        p.startNewSubPath(x + s * 0.5f, y + s * 0.18f); p.lineTo(x + s * 0.5f, y + s * 0.82f);
        p.startNewSubPath(x + s * 0.18f, y + s * 0.5f); p.lineTo(x + s * 0.82f, y + s * 0.5f);
        g.strokePath(p, pen);
        return;
    case Glyph::minus:
        p.startNewSubPath(x + s * 0.18f, y + s * 0.5f); p.lineTo(x + s * 0.82f, y + s * 0.5f);
        g.strokePath(p, pen);
        return;
    case Glyph::close:
        p.startNewSubPath(x + s * 0.24f, y + s * 0.24f); p.lineTo(x + s * 0.76f, y + s * 0.76f);
        p.startNewSubPath(x + s * 0.76f, y + s * 0.24f); p.lineTo(x + s * 0.24f, y + s * 0.76f);
        g.strokePath(p, pen);
        return;
    case Glyph::chevronDown:
        p.startNewSubPath(x + s * 0.25f, y + s * 0.38f); p.lineTo(x + s * 0.5f, y + s * 0.64f);
        p.lineTo(x + s * 0.75f, y + s * 0.38f);
        g.strokePath(p, pen);
        return;
    case Glyph::open:
        p.startNewSubPath(x + s * 0.1f, y + s * 0.8f);
        p.lineTo(x + s * 0.1f, y + s * 0.22f); p.lineTo(x + s * 0.38f, y + s * 0.22f);
        p.lineTo(x + s * 0.47f, y + s * 0.32f); p.lineTo(x + s * 0.82f, y + s * 0.32f);
        p.lineTo(x + s * 0.82f, y + s * 0.42f);
        p.startNewSubPath(x + s * 0.1f, y + s * 0.8f);
        p.lineTo(x + s * 0.26f, y + s * 0.46f); p.lineTo(x + s * 0.94f, y + s * 0.46f);
        p.lineTo(x + s * 0.78f, y + s * 0.8f); p.closeSubPath();
        g.strokePath(p, juce::PathStrokeType(stroke * 0.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    case Glyph::save:
    case Glyph::collect: {
        // A tray with a downward arrow; "collect" adds a second stacked tray.
        p.startNewSubPath(x + s * 0.5f, y + s * 0.12f); p.lineTo(x + s * 0.5f, y + s * 0.6f);
        p.startNewSubPath(x + s * 0.3f, y + s * 0.42f); p.lineTo(x + s * 0.5f, y + s * 0.62f);
        p.lineTo(x + s * 0.7f, y + s * 0.42f);
        p.startNewSubPath(x + s * 0.14f, y + s * 0.62f); p.lineTo(x + s * 0.14f, y + s * 0.84f);
        p.lineTo(x + s * 0.86f, y + s * 0.84f); p.lineTo(x + s * 0.86f, y + s * 0.62f);
        if (glyph == Glyph::collect) {
            p.startNewSubPath(x + s * 0.06f, y + s * 0.72f); p.lineTo(x + s * 0.06f, y + s * 0.94f);
            p.lineTo(x + s * 0.78f, y + s * 0.94f);
        }
        g.strokePath(p, juce::PathStrokeType(stroke * 0.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    case Glyph::devices: {
        // Three vertical sliders: a device/rack symbol.
        for (int i = 0; i < 3; ++i) {
            const float cx = x + s * (0.22f + 0.28f * static_cast<float>(i));
            p.startNewSubPath(cx, y + s * 0.14f); p.lineTo(cx, y + s * 0.86f);
        }
        g.strokePath(p, juce::PathStrokeType(stroke * 0.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const float knobs[] = {0.62f, 0.32f, 0.5f};
        for (int i = 0; i < 3; ++i)
            g.fillRoundedRectangle(x + s * (0.22f + 0.28f * static_cast<float>(i)) - s * 0.1f,
                y + s * knobs[i] - s * 0.06f, s * 0.2f, s * 0.12f, s * 0.03f);
        return;
    }
    case Glyph::edit:
        p.startNewSubPath(x + s * 0.18f, y + s * 0.82f); p.lineTo(x + s * 0.24f, y + s * 0.6f);
        p.lineTo(x + s * 0.66f, y + s * 0.18f); p.lineTo(x + s * 0.82f, y + s * 0.34f);
        p.lineTo(x + s * 0.4f, y + s * 0.76f); p.closeSubPath();
        g.strokePath(p, juce::PathStrokeType(stroke * 0.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    case Glyph::waveform: {
        const float h[] = {0.2f, 0.45f, 0.75f, 0.4f, 0.6f, 0.3f, 0.15f};
        for (int i = 0; i < 7; ++i) {
            const float cx = x + s * (0.14f + 0.12f * static_cast<float>(i));
            p.startNewSubPath(cx, y + s * (0.5f - h[i] * 0.5f));
            p.lineTo(cx, y + s * (0.5f + h[i] * 0.5f));
        }
        g.strokePath(p, juce::PathStrokeType(stroke * 0.75f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    case Glyph::learn:
        g.drawEllipse(r.reduced(s * 0.16f), stroke * 0.8f);
        g.fillEllipse(r.reduced(s * 0.36f));
        return;
    }
}

class StudioLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    StudioLookAndFeel() {
        using namespace colours;
        setDefaultSansSerifTypefaceName(sansTypefaceName());
        setColour(juce::ResizableWindow::backgroundColourId, window);
        setColour(juce::DocumentWindow::textColourId, text);
        setColour(juce::Label::textColourId, text);
        setColour(juce::TextButton::buttonColourId, control);
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, accentInk);
        setColour(juce::ToggleButton::textColourId, textDim);
        setColour(juce::ToggleButton::tickColourId, accentInk);
        setColour(juce::ToggleButton::tickDisabledColourId, textFaint);
        setColour(juce::ComboBox::backgroundColourId, control);
        setColour(juce::ComboBox::outlineColourId, lineStrong);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::arrowColourId, textDim);
        setColour(juce::ComboBox::focusedOutlineColourId, accent);
        setColour(juce::Slider::trackColourId, accent);
        setColour(juce::Slider::backgroundColourId, inset);
        setColour(juce::Slider::thumbColourId, juce::Colour(0xfff2f4f6));
        setColour(juce::Slider::rotarySliderFillColourId, accent);
        setColour(juce::Slider::rotarySliderOutlineColourId, inset);
        setColour(juce::Slider::textBoxTextColourId, text);
        setColour(juce::Slider::textBoxBackgroundColourId, inset);
        setColour(juce::Slider::textBoxOutlineColourId, line);
        setColour(juce::Slider::textBoxHighlightColourId, accent.withAlpha(0.35f));
        setColour(juce::TextEditor::backgroundColourId, inset);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::TextEditor::outlineColourId, line);
        setColour(juce::TextEditor::focusedOutlineColourId, accent);
        setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.35f));
        setColour(juce::CaretComponent::caretColourId, accent);
        setColour(juce::TabbedComponent::backgroundColourId, surface);
        setColour(juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TabbedButtonBar::tabOutlineColourId, line);
        setColour(juce::TabbedButtonBar::frontOutlineColourId, accent);
        setColour(juce::TabbedButtonBar::tabTextColourId, textDim);
        setColour(juce::TabbedButtonBar::frontTextColourId, text);
        setColour(juce::PopupMenu::backgroundColourId, raised);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::headerTextColourId, textFaint);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.18f));
        setColour(juce::PopupMenu::highlightedTextColourId, text);
        setColour(juce::ScrollBar::thumbColourId, hover);
        setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff26292f));
        setColour(juce::TooltipWindow::textColourId, text);
        setColour(juce::TooltipWindow::outlineColourId, lineStrong);
        setColour(juce::AlertWindow::backgroundColourId, raised);
        setColour(juce::AlertWindow::textColourId, text);
        setColour(juce::AlertWindow::outlineColourId, lineStrong);
        setColour(juce::ListBox::backgroundColourId, surface);
        setColour(juce::ListBox::textColourId, text);
        setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, accent.withAlpha(0.25f));
        setColour(juce::DirectoryContentsDisplayComponent::textColourId, text);
        setColour(juce::FileBrowserComponent::currentPathBoxBackgroundColourId, control);
        setColour(juce::FileBrowserComponent::currentPathBoxTextColourId, text);
        setColour(juce::FileBrowserComponent::filenameBoxBackgroundColourId, inset);
        setColour(juce::FileBrowserComponent::filenameBoxTextColourId, text);
        setColour(juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour(0xffe9ebee));
        setColour(juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour(0xff1a1b1f));
        setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour(0xff9aa0a8));
        setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accent.withAlpha(0.22f));
        setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, accent.withAlpha(0.75f));
        setColour(juce::MidiKeyboardComponent::textLabelColourId, juce::Colour(0xff5b6068));
        setColour(juce::MidiKeyboardComponent::shadowColourId, juce::Colour(0x66000000));
        setColour(juce::MidiKeyboardComponent::upDownButtonBackgroundColourId, raised);
        setColour(juce::MidiKeyboardComponent::upDownButtonArrowColourId, textDim);
        setColour(juce::CaretComponent::caretColourId, accent);
    }

    // Buttons ------------------------------------------------------------
    // Optional per-button "style" property: "tab" (segmented view switch),
    // "ghost" (transparent toolbar button) or "accent" (primary action).
    static juce::String styleOf(const juce::Component& c) {
        return c.getProperties().getWithDefault("style", {}).toString();
    }

    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override {
        const auto style = styleOf(button);
        return font(juce::jmin(13.5f, static_cast<float>(buttonHeight) * 0.5f),
            style == "tab" || style == "accent");
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool over, bool down) override {
        using namespace colours;
        const auto style = styleOf(button);
        const bool on = button.getToggleState();
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const float radius = 5.0f;
        const float alpha = button.isEnabled() ? 1.0f : 0.4f;

        if (style == "tab" || style == "ghost") {
            if (on) {
                g.setColour(style == "tab" ? control.withAlpha(alpha) : hover.withAlpha(alpha));
                g.fillRoundedRectangle(bounds, radius);
                if (style == "tab") {
                    g.setColour(accent.withAlpha(alpha));
                    g.fillRoundedRectangle(bounds.removeFromBottom(2.0f).reduced(bounds.getWidth() * 0.22f, 0.0f), 1.0f);
                }
            } else if (over || down) {
                g.setColour(juce::Colours::white.withAlpha(down ? 0.09f : 0.05f));
                g.fillRoundedRectangle(bounds, radius);
            }
        } else {
            auto base = button.findColour(on ? juce::TextButton::buttonOnColourId
                                             : juce::TextButton::buttonColourId);
            if (down) base = base.brighter(0.14f);
            else if (over) base = base.brighter(0.07f);
            base = base.withMultipliedAlpha(alpha);
            g.setGradientFill(juce::ColourGradient(base.brighter(0.05f), 0.0f, bounds.getY(),
                                                   base.darker(0.06f), 0.0f, bounds.getBottom(), false));
            g.fillRoundedRectangle(bounds, radius);
            g.setColour(juce::Colours::black.withAlpha(0.45f * alpha));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.06f * alpha));
            g.drawHorizontalLine(static_cast<int>(bounds.getY() + 1.0f), bounds.getX() + radius,
                                 bounds.getRight() - radius);
        }
        if (button.hasKeyboardFocus(false) && button.isEnabled()) {
            g.setColour(accent.withAlpha(0.75f));
            g.drawRoundedRectangle(button.getLocalBounds().toFloat().reduced(1.0f), radius, 1.2f);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override {
        const auto style = styleOf(button);
        const bool on = button.getToggleState();
        auto colour = button.findColour(on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
        if (style == "tab" || style == "ghost")
            colour = on ? colours::text : colours::textDim;
        g.setColour(colour.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.45f));
        g.setFont(getTextButtonFont(button, button.getHeight()));
        const int inset = juce::jmin(8, button.getHeight() / 3);
        auto area = button.getLocalBounds().reduced(inset, 0);
        const auto lines = juce::jmax(1, button.getButtonText().length() > 0 ? 2 : 1);
        g.drawFittedText(button.getButtonText(), area, juce::Justification::centred, lines);
    }

    void drawTickBox(juce::Graphics& g, juce::Component& component, float x, float y, float w, float h,
                     bool ticked, bool isEnabled, bool over, bool) override {
        using namespace colours;
        const auto box = juce::Rectangle<float>(x, y, w, h).reduced(1.0f);
        g.setColour(ticked ? accent : (over ? hover : control));
        g.fillRoundedRectangle(box, 3.5f);
        g.setColour(ticked ? accent.darker(0.3f) : lineStrong);
        g.drawRoundedRectangle(box, 3.5f, 1.0f);
        if (ticked) {
            juce::Path tick;
            tick.startNewSubPath(box.getX() + box.getWidth() * 0.24f, box.getCentreY());
            tick.lineTo(box.getX() + box.getWidth() * 0.43f, box.getBottom() - box.getHeight() * 0.27f);
            tick.lineTo(box.getRight() - box.getWidth() * 0.22f, box.getY() + box.getHeight() * 0.27f);
            g.setColour(accentInk.withMultipliedAlpha(isEnabled ? 1.0f : 0.5f));
            g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        juce::ignoreUnused(component);
    }

    // Combo boxes and menus ---------------------------------------------
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override {
        using namespace colours;
        const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                                   static_cast<float>(height)).reduced(0.5f);
        auto fill = box.findColour(juce::ComboBox::backgroundColourId);
        if (isButtonDown) fill = fill.brighter(0.08f);
        else if (box.isMouseOver(true)) fill = fill.brighter(0.04f);
        g.setColour(fill.withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.5f));
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(box.hasKeyboardFocus(true) ? accent.withAlpha(0.45f) : juce::Colours::black.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
        drawGlyph(g, Glyph::chevronDown,
                  juce::Rectangle<float>(static_cast<float>(buttonX), static_cast<float>(buttonY),
                                         static_cast<float>(buttonW), static_cast<float>(buttonH))
                      .withSizeKeepingCentre(10.0f, 10.0f),
                  box.findColour(juce::ComboBox::arrowColourId));
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override {
        return font(juce::jmin(13.5f, static_cast<float>(box.getHeight()) * 0.52f));
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override {
        label.setBounds(2, 1, box.getWidth() - 22, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
    }

    juce::Font getPopupMenuFont() override { return font(14.0f); }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override {
        g.fillAll(colours::raised);
        g.setColour(colours::lineStrong);
        g.drawRect(0, 0, width, height, 1);
    }

    juce::Font getLabelFont(juce::Label& label) override { return label.getFont(); }

    // Sliders ------------------------------------------------------------
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override {
        using namespace colours;
        const float alpha = slider.isEnabled() ? 1.0f : 0.45f;
        if (style == juce::Slider::LinearBarVertical || style == juce::Slider::LinearBar) {
            // LCD-style value field: drag to change, value drawn as large digits.
            const auto r = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                                  static_cast<float>(width), static_cast<float>(height));
            if (slider.isMouseOverOrDragging()) {
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.fillRoundedRectangle(r, 3.0f);
            }
            g.setColour(lcdText.withMultipliedAlpha(alpha));
            g.setFont(monoFont(juce::jmin(22.0f, static_cast<float>(height) * 0.72f), true));
            g.drawText(slider.getTextFromValue(slider.getValue()), r, juce::Justification::centred, false);
            return;
        }
        if (style == juce::Slider::LinearVertical) {
            // Console fader: recessed slot and a ribbed cap.
            const auto slot = juce::Rectangle<float>(static_cast<float>(x) + static_cast<float>(width) * 0.5f - 2.5f,
                static_cast<float>(y), 5.0f, static_cast<float>(height));
            g.setColour(inset);
            g.fillRoundedRectangle(slot, 2.5f);
            const auto fillTop = juce::jlimit(slot.getY(), slot.getBottom(), sliderPos);
            g.setColour(slider.findColour(juce::Slider::trackColourId).withMultipliedAlpha(0.55f * alpha));
            g.fillRoundedRectangle(slot.withTop(fillTop), 2.5f);
            auto cap = juce::Rectangle<float>(0.0f, 0.0f, juce::jmin(34.0f, static_cast<float>(width) - 2.0f), 44.0f)
                           .withCentre({slot.getCentreX(), sliderPos});
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(cap.translated(0.0f, 2.0f), 4.0f);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a4e57), 0.0f, cap.getY(),
                                                   juce::Colour(0xff2a2d33), 0.0f, cap.getBottom(), false));
            g.fillRoundedRectangle(cap, 4.0f);
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.drawRoundedRectangle(cap, 4.0f, 1.0f);
            for (int i = -2; i <= 2; ++i) {
                if (i == 0) continue;
                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.drawHorizontalLine(static_cast<int>(cap.getCentreY()) + i * 4, cap.getX() + 6.0f, cap.getRight() - 6.0f);
            }
            g.setColour(juce::Colours::white.withAlpha(0.9f * alpha));
            g.fillRect(cap.getX() + 4.0f, cap.getCentreY() - 0.75f, cap.getWidth() - 8.0f, 1.5f);
            return;
        }
        if (style != juce::Slider::LinearHorizontal)
            return juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                                          minSliderPos, maxSliderPos, style, slider);
        auto track = juce::Rectangle<float>(static_cast<float>(x) + 6.0f,
            static_cast<float>(y) + static_cast<float>(height) * 0.5f - 2.0f,
            static_cast<float>(width) - 12.0f, 4.0f);
        g.setColour(inset);
        g.fillRoundedRectangle(track, 2.0f);
        const auto filled = juce::jlimit(0.0f, track.getWidth(), sliderPos - track.getX());
        g.setColour(slider.findColour(juce::Slider::trackColourId).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track.withWidth(filled), 2.0f);
        const auto thumb = juce::Rectangle<float>(12.0f, 12.0f).withCentre({sliderPos, track.getCentreY()});
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillEllipse(thumb.translated(0.0f, 1.0f).expanded(1.0f));
        g.setColour(slider.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(alpha));
        g.fillEllipse(thumb);
        if (slider.hasKeyboardFocus(false)) {
            g.setColour(accent);
            g.drawEllipse(thumb.expanded(2.0f), 1.2f);
        }
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider& slider) override {
        using namespace colours;
        const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                                   static_cast<float>(width), static_cast<float>(height)).reduced(3.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float arcRadius = radius - 2.0f;
        const float angle = startAngle + pos * (endAngle - startAngle);
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float originAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

        juce::Path background;
        background.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour(inset);
        g.strokePath(background, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (std::abs(angle - originAngle) > 0.01f) {
            juce::Path value;
            value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                juce::jmin(originAngle, angle), juce::jmax(originAngle, angle), true);
            g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId)
                            .withMultipliedAlpha(slider.isEnabled() ? 1.0f : 0.45f));
            g.strokePath(value, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        const float knobRadius = arcRadius - 6.0f;
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillEllipse(juce::Rectangle<float>(knobRadius * 2.0f, knobRadius * 2.0f).withCentre(centre.translated(0.0f, 1.5f)));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3d4048), centre.x, centre.y - knobRadius,
                                               juce::Colour(0xff24262b), centre.x, centre.y + knobRadius, false));
        g.fillEllipse(juce::Rectangle<float>(knobRadius * 2.0f, knobRadius * 2.0f).withCentre(centre));
        juce::Path pointer;
        pointer.addRoundedRectangle(-1.25f, -knobRadius + 2.0f, 2.5f, knobRadius * 0.55f, 1.0f);
        g.setColour(text);
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre));
        if (slider.hasKeyboardFocus(false)) {
            g.setColour(accent.withAlpha(0.7f));
            g.drawEllipse(juce::Rectangle<float>(knobRadius * 2.0f + 3.0f, knobRadius * 2.0f + 3.0f).withCentre(centre), 1.0f);
        }
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(monoFont(12.0f));
        label->setColour(juce::Label::outlineColourId, colours::line);
        label->setColour(juce::Label::backgroundColourId, colours::inset);
        return label;
    }

    // Scrollbars, tooltips ---------------------------------------------
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                       bool isVertical, int thumbStart, int thumbSize, bool over, bool down) override {
        juce::Rectangle<float> thumb = isVertical
            ? juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(thumbStart),
                                     static_cast<float>(width), static_cast<float>(thumbSize))
            : juce::Rectangle<float>(static_cast<float>(thumbStart), static_cast<float>(y),
                                     static_cast<float>(thumbSize), static_cast<float>(height));
        thumb = thumb.reduced(isVertical ? 2.5f : 1.0f, isVertical ? 1.0f : 2.5f);
        g.setColour(colours::hover.brighter(down ? 0.3f : over ? 0.18f : 0.0f));
        g.fillRoundedRectangle(thumb, juce::jmin(thumb.getWidth(), thumb.getHeight()) * 0.5f);
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override {
        juce::TextLayout layout;
        layout.createLayout(tooltipText(tipText), 360.0f);
        const int w = static_cast<int>(layout.getWidth() + 20.0f);
        const int h = static_cast<int>(layout.getHeight() + 14.0f);
        return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                    screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 18,
                                    w, h).constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override {
        const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
        g.setColour(findColour(juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(findColour(juce::TooltipWindow::outlineColourId));
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
        juce::TextLayout layout;
        layout.createLayout(tooltipText(text), 360.0f);
        layout.draw(g, r.reduced(10.0f, 7.0f));
    }

private:
    static juce::AttributedString tooltipText(const juce::String& text) {
        juce::AttributedString s;
        s.setJustification(juce::Justification::centredLeft);
        s.append(text, font(13.0f), colours::text);
        return s;
    }
};

} // namespace alien::ui
