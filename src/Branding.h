#pragma once
#include "StudioTheme.h"
#include "BrandingData.h"
#include <functional>
#include <cmath>
#include <map>

// Approved Alien Studio artwork (assets/branding/AlienStudio_Icon_Corrected.png),
// embedded as binary data and used for the in-app logo and the startup splash.
// The Windows executable icon comes from the ICO resource, not from here.
namespace alien::branding {

inline juce::Image artwork() {
    return juce::ImageCache::getFromMemory(BrandingData::AlienStudio_Icon_Corrected_png,
                                           BrandingData::AlienStudio_Icon_Corrected_pngSize);
}

// High-quality downscale, cached per pixel size, so small renderings of the
// 1536 px source stay smooth rather than aliased.
inline juce::Image artworkAtSize(int pixels) {
    static std::map<int, juce::Image> cache;
    pixels = juce::jlimit(16, 1024, pixels);
    if (auto it = cache.find(pixels); it != cache.end()) return it->second;
    auto source = artwork();
    if (source.isNull()) return {};
    auto scaled = source.rescaled(pixels, pixels, juce::Graphics::highResamplingQuality);
    cache[pixels] = scaled;
    return scaled;
}

inline void drawArtwork(juce::Graphics& g, juce::Rectangle<float> area) {
    const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto image = artworkAtSize(juce::roundToInt(juce::jmin(area.getWidth(), area.getHeight()) * scale));
    if (image.isValid())
        g.drawImage(image, area, juce::RectanglePlacement::centred);
}

// Top-bar logo.
class LogoMark final : public juce::Component {
public:
    LogoMark() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override { drawArtwork(g, getLocalBounds().toFloat()); }
};

// Borderless startup splash. It shows the artwork, product name, version and
// a status line; the owner closes it with dismiss() once the main window is
// ready. Clicking it requests an early dismissal through onClick.
class SplashWindow final : public juce::Component, private juce::Timer {
public:
    explicit SplashWindow(juce::String versionText) : version(std::move(versionText)) {
        setOpaque(false);
        setSize(720, 380);
        shadow.setOwner(this);
        startTimerHz(60);
    }
    ~SplashWindow() override { stopTimer(); }

    void showCentred() {
        addToDesktop(juce::ComponentPeer::windowIsTemporary);
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            setCentrePosition(display->userArea.getCentre());
        setAlwaysOnTop(true);
        setVisible(true);
        toFront(false);
    }

    void setStatus(const juce::String& text) { status = text; repaint(); }

    // Fades the window out, hides it and then calls onDismissed (asynchronously,
    // so the owner may delete the splash from that callback).
    void dismiss() {
        if (fading) return;
        fading = true;
        fadeStart = juce::Time::getMillisecondCounter();
    }

    std::function<void()> onDismissed;

    std::function<void()> onClick;
    void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }

    void paint(juce::Graphics& g) override {
        using namespace ui;
        const auto card = getLocalBounds().toFloat().reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1b1d23), card.getX(), card.getY(),
                                               juce::Colour(0xff0d0e11), card.getRight(), card.getBottom(), false));
        g.fillRoundedRectangle(card, 14.0f);
        g.setColour(colours::lineStrong);
        g.drawRoundedRectangle(card.reduced(0.5f), 14.0f, 1.0f);

        auto content = getLocalBounds().reduced(36, 32);
        auto art = content.removeFromLeft(content.getHeight()).toFloat();
        // Soft glow behind the artwork.
        g.setGradientFill(juce::ColourGradient(juce::Colour(0x334fe0b6), art.getCentreX(), art.getCentreY(),
                                               juce::Colour(0x004fe0b6), art.getRight() + 40.0f, art.getCentreY(), true));
        g.fillEllipse(art.expanded(30.0f));
        drawArtwork(g, art);

        content.removeFromLeft(34);
        auto text = content.withTrimmedTop(46);
        g.setColour(colours::text);
        g.setFont(font(36.0f, true));
        g.drawText("Alien Studio", text.removeFromTop(44), juce::Justification::centredLeft, true);
        auto caption = font(12.0f, true);
        caption.setExtraKerningFactor(0.18f);
        g.setFont(caption);
        g.setColour(colours::accent);
        g.drawText("Digital Audio Workstation", text.removeFromTop(22), juce::Justification::centredLeft, true);
        text.removeFromTop(16);
        g.setColour(colours::textDim);
        g.setFont(font(14.0f));
        g.drawText("Free and open-source digital audio workstation", text.removeFromTop(22),
                   juce::Justification::centredLeft, true);
        g.setColour(colours::textFaint);
        g.setFont(monoFont(12.5f));
        g.drawText("Version " + version, text.removeFromTop(22), juce::Justification::centredLeft, true);

        auto footer = content.removeFromBottom(30);
        g.setColour(colours::inset);
        auto track = footer.removeFromBottom(3).toFloat();
        g.fillRoundedRectangle(track, 1.5f);
        g.setColour(colours::accent);
        g.fillRoundedRectangle(track.withWidth(track.getWidth() * progress), 1.5f);
        g.setColour(colours::textFaint);
        g.setFont(font(12.5f));
        g.drawText(status, footer, juce::Justification::centredLeft, true);
    }

    // Target progress; the bar eases towards it.
    void setProgress(float p) { target = juce::jlimit(0.0f, 1.0f, p); }

private:
    juce::String version, status{"Starting"};
    float progress{0.0f}, target{0.15f};
    bool fading{};
    juce::uint32 fadeStart{};
    void timerCallback() override {
        if (fading) {
            const float t = static_cast<float>(juce::Time::getMillisecondCounter() - fadeStart) / 220.0f;
            if (t < 1.0f) { setAlpha(1.0f - t); return; }
            stopTimer();
            setVisible(false);
            juce::Component::SafePointer<SplashWindow> safe(this);
            juce::MessageManager::callAsync([safe] { if (safe != nullptr && safe->onDismissed) safe->onDismissed(); });
            return;
        }
        if (std::abs(target - progress) < 0.002f) return;
        progress += (target - progress) * 0.12f;
        repaint();
    }
    juce::DropShadower shadow{juce::DropShadow(juce::Colours::black.withAlpha(0.6f), 28, {0, 8})};
};

} // namespace alien::branding
