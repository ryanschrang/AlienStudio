#pragma once

#include "PianoRollScale.h"
#include <array>
#include <bitset>

namespace alien {

// Which pitches the piano roll shows as rows (PROD-06): every pitch, only the pitches notes use, or the selected
// scale's tones. Hiding a row is a view; it never changes or removes a note.
enum class PitchView { all, used, scale };

// How a pitch sits in the selected scale, for the keyboard and the grid row alike.
enum class ScaleRole { none, tone, root, outside };
inline ScaleRole scaleRole(int pitch, const ScaleSettings& scale) noexcept {
    if (pitch < 0 || pitch > 127) return ScaleRole::none;
    if (!isPitchInScale(pitch, scale)) return ScaleRole::outside;
    return pitch % 12 == scale.rootPitchClass ? ScaleRole::root : ScaleRole::tone;
}

// The pitches shown as rows, lowest first.
struct PitchRows {
    std::array<int, 128> pitches{};
    int count{};

    // The row showing pitch, or -1 when its row is hidden.
    int rowOf(int pitch) const noexcept {
        if (pitch < 0 || pitch > 127) return -1;
        int low = 0, high = count;
        while (low < high) { const int mid = (low + high) / 2; if (pitches[mid] < pitch) low = mid + 1; else high = mid; }
        return low < count && pitches[low] == pitch ? low : -1;
    }
    // The lowest row at or above pitch (the top row when every row is below it).
    int rowAtOrAbove(int pitch) const noexcept {
        for (int row = 0; row < count; ++row) if (pitches[row] >= pitch) return row;
        return count - 1;
    }
    bool operator==(const PitchRows& other) const noexcept {
        if (count != other.count) return false;
        for (int row = 0; row < count; ++row) if (pitches[row] != other.pitches[row]) return false;
        return true;
    }
};

// used holds every pitch a note uses. Used shows those pitches; Scale shows the scale's tones plus any pitch a note
// uses, so an out-of-scale note stays visible. A view with nothing to show (an empty clip in Used) shows every pitch.
inline PitchRows pitchRows(PitchView view, const std::bitset<128>& used, const ScaleSettings& scale) noexcept {
    PitchRows rows;
    for (int pitch = 0; pitch < 128; ++pitch) {
        const bool shown = view == PitchView::all
                        || used.test(static_cast<std::size_t>(pitch))
                        || (view == PitchView::scale && isPitchInScale(pitch, scale));
        if (shown) rows.pitches[static_cast<std::size_t>(rows.count++)] = pitch;
    }
    if (rows.count == 0) return pitchRows(PitchView::all, used, scale);
    return rows;
}

} // namespace alien
