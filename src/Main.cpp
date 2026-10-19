#include <JuceHeader.h>
#include "MainComponent.h"
#include "Branding.h"

// Application entry point: startup splash, main window and lifetime.
class AlienStudioApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Alien Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String& commandLine) override {
        auto args = juce::StringArray::fromTokens(commandLine, true);
        args.removeEmptyStrings();
        if (args.size() == 3 && args[0] == "--scan-vst3") {
            setApplicationReturnValue(alien::scanPluginBundle(args[1].unquoted(), juce::File(args[2].unquoted())));
            quit(); return;
        }
        splash = std::make_unique<alien::branding::SplashWindow>(getApplicationVersion());
        splash->onClick = [this] { finishStartup(); };
        splash->setStatus("Loading interface and audio engine");
        splash->setProgress(0.55f);
        splash->showCentred();
        startedAt = juce::Time::getMillisecondCounter();
        // Let the splash paint before the (synchronous) main window construction.
        juce::Timer::callAfterDelay(90, [this] { createMainWindow(); });
    }
    void shutdown() override { splash.reset(); window.reset(); }

    // Closing the window, Alt+F4 or a system shutdown request: offer to save
    // unsaved changes first. The user can cancel.
    void systemRequestedQuit() override {
        if (window != nullptr)
            if (auto* content = dynamic_cast<MainComponent*>(window->getContentComponent())) {
                content->requestQuit([] { juce::JUCEApplication::quit(); });
                return;
            }
        quit();
    }
private:
    static constexpr juce::uint32 minimumSplashMs = 1600;

    void createMainWindow() {
        if (window != nullptr) return;
        window = std::make_unique<Window>(getApplicationName());
        // Keep it hidden behind the splash. JUCE's Linux peer can map the native
        // window while it is being sized, so hide the peer explicitly as well.
        window->setVisible(false);
        if (auto* peer = window->getPeer()) peer->setVisible(false);
        if (splash != nullptr) { splash->setStatus("Ready"); splash->setProgress(1.0f); }
        const auto elapsed = juce::Time::getMillisecondCounter() - startedAt;
        const int remaining = elapsed >= minimumSplashMs ? 0 : static_cast<int>(minimumSplashMs - elapsed);
        juce::Timer::callAfterDelay(remaining, [this] { finishStartup(); });
    }

    void finishStartup() {
        if (window == nullptr || startupFinished) return;
        startupFinished = true;
        window->setVisible(true);
        window->toFront(true);
        if (splash != nullptr) {
            splash->onDismissed = [this] { splash.reset(); };
            splash->dismiss();
        }
    }

    class Window : public juce::DocumentWindow {
    public:
        explicit Window(juce::String name) : juce::DocumentWindow(name, alien::ui::colours::window,
            juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(960, 650, 1920, 1440);
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                centreWithSize(juce::jmin(getWidth(), display->userArea.getWidth() - 40),
                               juce::jmin(getHeight(), display->userArea.getHeight() - 40));
            else centreWithSize(getWidth(), getHeight());
           #if ! JUCE_WINDOWS
            // Windows takes the icon from the embedded ICO resource; elsewhere set it here.
            setIcon(alien::branding::artworkAtSize(256));
           #endif
            // Shown by the application once the startup splash finishes.
        }
        void activeWindowStatusChanged() override {
            if (!isActiveWindow())
                if (auto* content = dynamic_cast<MainComponent*>(getContentComponent())) content->releaseNotes();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
    std::unique_ptr<alien::branding::SplashWindow> splash;
    juce::uint32 startedAt{};
    bool startupFinished{};
};
START_JUCE_APPLICATION(AlienStudioApp)
