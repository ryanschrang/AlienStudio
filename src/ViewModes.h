#pragma once
// Which tab is showing decides what the audio engine plays (ToneEngine::patternMode).
namespace alien {

inline constexpr int rackTabIndex = 8;   // the Channel Rack tab: appended last so earlier tab numbers never change

enum PatternMode { stepsMode = 0, sessionMode = 1, songMode = 2, rackMode = 3 };

// The mode for the tab that was just shown. Tabs that play nothing of their own
// (Mixer, Drums, Audio lab, Plugins) leave whatever was playing as it was.
inline int patternModeForTab(int tab, int currentMode) noexcept {
    switch (tab) {
    case 0: return stepsMode;                 // the 16-step synth sequencer
    case 1: case 2: return sessionMode;       // piano roll and Session clips
    case 5: return songMode;                  // the arrangement
    case rackTabIndex: return rackMode;       // the Channel Rack's selected pattern
    default: return currentMode;
    }
}

} // namespace alien
