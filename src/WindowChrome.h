#pragma once
#include "StudioTheme.h"
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <dwmapi.h>
 #pragma comment(lib, "dwmapi.lib")
#endif

// Paints the native title bar of an Alien Studio window in the logo blue.
// Windows 11 (build 22000+) honours the DWM caption, text and border colours;
// older Windows ignores them and keeps the system title bar.
namespace alien::ui {
inline void applyBrandTitleBar([[maybe_unused]] juce::Component& window) {
   #if JUCE_WINDOWS
    auto* peer = window.getPeer();
    if (peer == nullptr) return;
    const auto hwnd = static_cast<HWND>(peer->getNativeHandle());
    if (hwnd == nullptr) return;
    const auto colourRef = [](juce::Colour c) -> COLORREF { return RGB(c.getRed(), c.getGreen(), c.getBlue()); };
    // DWMWA_BORDER_COLOR, DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR; numbered so
    // the code builds against SDKs that predate the names.
    const COLORREF caption = colourRef(colours::titleBar), text = colourRef(colours::titleBarText);
    DwmSetWindowAttribute(hwnd, 34, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
   #endif
}
} // namespace alien::ui
