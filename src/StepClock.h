#pragma once
#include <algorithm>

namespace alien {
// One call per audio sample; fractional intervals preserve long-term tempo.
class StepClock {
public:
    struct Tick { int step{-1}; bool noteOff{false}; };
    void reset() noexcept { untilStep = untilOff = 0; step = 0; }
    Tick next(double sampleRate, int bpm) noexcept {
        Tick tick;
        if (untilOff > 0 && --untilOff <= 0) tick.noteOff = true;
        if (untilStep <= 0) {
            tick.step = step;
            tick.noteOff = true;
            step = (step + 1) % 16;
            const auto interval = sampleRate * 60.0 / (std::max(1, bpm) * 4.0);
            untilStep += interval;
            untilOff = interval * 0.5;
        }
        untilStep -= 1;
        return tick;
    }
private:
    double untilStep{}, untilOff{};
    int step{};
};
}
