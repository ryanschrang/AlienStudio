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

// With "Song mode" on, every view that would play something of its own (the 16-step sequencer, the piano roll and
// Session clips, the arrangement, the Channel Rack) plays the whole song instead, so a beat, a synth line and a
// vocal can be heard together while you work on any of them. Views that play nothing keep the current mode.
inline int patternModeForTab(int tab, int currentMode, bool songPlayback, bool rackPerformMode = false) noexcept {
    // Editing a Perform source does not switch away from its Session transport.
    if (!songPlayback && rackPerformMode && (tab == 1 || tab == 2 || tab == rackTabIndex)) return sessionMode;
    if (!songPlayback) return patternModeForTab(tab, currentMode);
    switch (tab) {
    case 0: case 1: case 2: case 5: case rackTabIndex: return songMode;
    default: return currentMode;
    }
}

} // namespace alien
