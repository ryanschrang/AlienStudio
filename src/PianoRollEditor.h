#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PianoRollSequence.h"
#include <cmath>
#include <functional>

namespace alien {

// Message-thread editor. Publishing snapshots and playing notes belong to the
// owner; drawing and pointer gestures never access the audio engine directly.
class PianoRollEditor final : public juce::Component, private juce::ScrollBar::Listener {
public:
    PianoRollEditor() {
        setName("Piano roll note editor");
        setTitle("Piano roll");
        setDescription("Four bar MIDI note grid with a piano keyboard and velocity lane");
        setHelpText("Double-click to draw a note. Drag to move; drag its right edge to resize. "
                    "Delete erases the selected note. Arrow keys move a selected note; "
                    "Shift Left or Right changes its length. Tab moves between controls.");
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);

        addAndMakeVisible(snapLabel);
        snapLabel.setText("Snap", juce::dontSendNotification);
        snapLabel.attachToComponent(&snap, true);
        addAndMakeVisible(snap);
        snap.setName("Piano roll grid snap");
        snap.setTooltip("Snap note starts and ends to sixteenth, eighth or quarter notes");
        snap.addItem("1/16", 1);
        snap.addItem("1/8", 2);
        snap.addItem("1/4", 4);
        snap.setSelectedId(1, juce::dontSendNotification);
        snap.onChange = [this] { repaint(); };

        addAndMakeVisible(velocityLabel);
        velocityLabel.setText("Velocity", juce::dontSendNotification);
        velocityLabel.attachToComponent(&velocity, true);
        addAndMakeVisible(velocity);
        velocity.setName("Selected note velocity");
        velocity.setTooltip("Velocity of the selected note, from 1 to 127");
        velocity.setSliderStyle(juce::Slider::LinearHorizontal);
        velocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 24);
        velocity.setRange(1, 127, 1);
        velocity.setValue(102, juce::dontSendNotification);
        velocity.setEnabled(false);
        velocity.onValueChange = [this] {
            if (const auto* note = pattern.find(selected)) {
                const auto value = static_cast<float>(velocity.getValue() / 127.0);
                if (note->velocity != value && pattern.setVelocity(selected, value)) changed();
            }
        };

        addAndMakeVisible(clearButton);
        clearButton.setButtonText("Clear");
        clearButton.setName("Clear piano roll notes");
        clearButton.setTooltip("Remove all notes from the piano roll");
        clearButton.onClick = [this] {
            if (pattern.count == 0) return;
            releaseAudition();
            pattern.clear();
            selected = 0;
            drag = Drag::none;
            changed();
        };
        addAndMakeVisible(exampleButton);
        exampleButton.setButtonText("Load example");
        exampleButton.setName("Load example piano roll melody");
        exampleButton.setTooltip("Add an original example to an empty loop; clear existing notes first");
        exampleButton.onClick = [this] { loadExample(); };

        addAndMakeVisible(pitchScroll);
        pitchScroll.setName("Piano roll pitch range");
        pitchScroll.setTitle("Piano roll pitch range");
        pitchScroll.setRangeLimits(0, highestPitch - lowestPitch + 1);
        pitchScroll.setSingleStepSize(1);
        pitchScroll.addListener(this);
        updatePitchScroll();

        for (auto* label : { &snapLabel, &velocityLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffc5d6df));
        velocity.setColour(juce::Slider::thumbColourId, juce::Colour(0xff7ee8a2));
        velocity.setColour(juce::Slider::trackColourId, juce::Colour(0xff3bc8c8));
        pitchScroll.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff477683));
        updateSelection();
    }

    ~PianoRollEditor() override {
        releaseAudition();
        pitchScroll.removeListener(this);
    }

    std::function<void()> onChange;
    // Positive velocity starts a preview; zero releases that same pitch.
    std::function<void(int, float)> onAudition;

    const PianoRollPattern& getPattern() const noexcept { return pattern; }

    void setPattern(const PianoRollPattern& replacement) {
        if (!replacement.valid()) return;
        releaseAudition();
        pattern = replacement;
        selected = 0;
        drag = Drag::none;
        changed();
    }

    void setPlayheadStep(double step) {
        if (playhead == step) return;
        playhead = step;
        repaint();
    }

    void releaseAudition() {
        releasePreviewNote();
        if (drag == Drag::audition) drag = Drag::none;
    }

    void resized() override {
        snap.setBounds(184, 8, 76, 28);
        snapLabel.setBounds(145, 8, 35, 28);
        velocity.setBounds(332, 8, 155, 28);
        velocityLabel.setBounds(272, 8, 56, 28);
        clearButton.setBounds(503, 8, 70, 28);
        exampleButton.setBounds(583, 8, 112, 28);
        const auto a = areas();
        pitchScroll.setBounds(a.grid.getRight() + 4, a.grid.getY(), 12, a.grid.getHeight());
    }

    void paint(juce::Graphics& g) override {
        const auto a = areas();
        g.fillAll(juce::Colour(0xff111b28));
        g.setColour(juce::Colour(0xff8ff2b2));
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText("PIANO ROLL", 12, 7, 132, 29, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff1c2d3d));
        g.fillRect(a.ruler);
        g.setFont(juce::FontOptions(10.5f));
        const auto rowHeight = rowSize(a);
        for (int row = 0; row < visiblePitches; ++row) {
            const auto pitch = topPitch() - row;
            const auto y = a.grid.getY() + row * rowHeight;
            const juce::Rectangle<float> rowBounds(static_cast<float>(a.grid.getX()), y,
                                                  static_cast<float>(a.grid.getWidth()), rowHeight);
            g.setColour(isBlack(pitch) ? juce::Colour(0xff152331) : juce::Colour(0xff1b2c39));
            g.fillRect(rowBounds);
            g.setColour(pitch % 12 == 0 ? juce::Colour(0xff43666e) : juce::Colour(0xff293c49));
            g.drawHorizontalLine(juce::roundToInt(y + rowHeight),
                                 static_cast<float>(a.grid.getX()), static_cast<float>(a.grid.getRight()));

            const juce::Rectangle<float> key(static_cast<float>(a.piano.getX()), y,
                                             static_cast<float>(a.piano.getWidth()), rowHeight);
            g.setColour(pitch == auditionPitch ? juce::Colour(0xff68e6bb)
                                              : juce::Colour(0xffc2d5d2));
            g.fillRect(key);
            if (isBlack(pitch)) {
                g.setColour(pitch == auditionPitch ? juce::Colour(0xff289e9e) : juce::Colour(0xff253443));
                g.fillRect(key.withWidth(key.getWidth() * 0.72f));
            }
            g.setColour(juce::Colour(0xff637b80));
            g.drawHorizontalLine(juce::roundToInt(y + rowHeight), key.getX(), key.getRight());
            if (pitch % 12 == 0) {
                g.setColour(juce::Colour(0xff142b33));
                g.drawText(noteName(pitch), key.reduced(3.0f, 0.0f), juce::Justification::centredRight);
            }
        }

        for (int step = 0; step <= pattern.loopSteps; ++step) {
            const auto x = stepX(step, a);
            const bool bar = step % 16 == 0;
            const bool beat = step % 4 == 0;
            g.setColour(bar ? juce::Colour(0xff5c8089) : beat ? juce::Colour(0xff3a5664)
                                                                        : juce::Colour(0xff253b49));
            g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(a.grid.getY()),
                               static_cast<float>(a.grid.getBottom()));
            if (step < pattern.loopSteps && beat) {
                g.setColour(bar ? juce::Colour(0xffd7ece9) : juce::Colour(0xff8fabb5));
                g.drawText(bar ? juce::String(step / 16 + 1)
                               : juce::String(step / 16 + 1) + "." + juce::String((step % 16) / 4 + 1),
                           juce::roundToInt(x) + 4, a.ruler.getY(),
                           juce::roundToInt(stepSize(a) * 4) - 4, a.ruler.getHeight(),
                           juce::Justification::centredLeft);
            }
        }
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (pattern.notes[i].id != selected) paintNote(g, pattern.notes[i], a, false);
        if (const auto* note = pattern.find(selected)) paintNote(g, *note, a, true);

        g.setColour(juce::Colour(0xff172735));
        g.fillRect(a.velocity);
        g.setFont(juce::FontOptions(10.5f));
        g.setColour(juce::Colour(0xff9bb7c0));
        g.drawText("VELOCITY", a.piano.getX(), a.velocity.getY() + 3, a.piano.getWidth(), 16,
                   juce::Justification::centredLeft);
        for (int level = 0; level <= 2; ++level) {
            g.setColour(juce::Colour(0xff2b414e));
            const auto y = velocityY(level / 2.0f, a);
            g.drawHorizontalLine(juce::roundToInt(y), static_cast<float>(a.velocity.getX()),
                                 static_cast<float>(a.velocity.getRight()));
        }
        for (std::size_t i = 0; i < pattern.count; ++i)
            if (pattern.notes[i].id != selected) paintVelocity(g, pattern.notes[i], a, false);
        if (const auto* note = pattern.find(selected)) paintVelocity(g, *note, a, true);

        if (std::isfinite(playhead) && playhead >= 0 && playhead < pattern.loopSteps) {
            const auto x = stepX(playhead, a);
            g.setColour(juce::Colour(0xff60ebdf));
            g.drawLine(x, static_cast<float>(a.ruler.getY()), x,
                       static_cast<float>(a.velocity.getBottom()), 1.8f);
        }
        g.setColour(hasKeyboardFocus(false) ? juce::Colour(0xff75d9cf) : juce::Colour(0xff405a68));
        g.drawRect(a.grid);
        g.setFont(juce::FontOptions(11.0f));
        g.setColour(juce::Colour(0xffc0d7db));
        juce::String status = juce::String(pattern.loopSteps / 16.0, 0) + "-bar loop (session only)  |  "
            + noteName(lowestVisiblePitch) + " - " + noteName(topPitch()) + "  |  "
            + juce::String(static_cast<int>(pattern.count)) + "/128 notes";
        if (const auto* note = pattern.find(selected))
            status += "  |  Selected " + noteName(note->pitch) + ": step " + juce::String(note->startStep + 1)
                   + ", length " + juce::String(note->lengthSteps) + ", velocity "
                   + juce::String(juce::roundToInt(note->velocity * 127.0f));
        g.drawText(status, 12, a.velocity.getBottom() + 4, getWidth() - 24, 17,
                   juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff91abb8));
        g.drawText("Double-click: draw   |   Drag: move   |   Right edge: resize   |   Del / right-click: erase   |   Wheel: pitch range",
                   12, a.velocity.getBottom() + 21, getWidth() - 24, 17, juce::Justification::centredLeft);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        const auto a = areas();
        drag = Drag::none;
        if (a.piano.contains(e.getPosition())) {
            if (!e.mods.isPopupMenu()) {
                drag = Drag::audition;
                preview(pitchAt(e.position.y, a));
            }
            return;
        }
        if (a.velocity.contains(e.getPosition())) {
            selected = hitVelocity(e.position.x, a);
            if (selected != 0 && !e.mods.isPopupMenu()) {
                drag = Drag::velocity;
                changeVelocity(e.position.y, a);
            }
            updateSelection();
            return;
        }
        if (!a.grid.contains(e.getPosition())) return;
        selected = hitNote(e.position, a);
        if (e.mods.isPopupMenu()) {
            eraseSelected();
            return;
        }
        if (const auto* note = pattern.find(selected)) {
            dragOriginal = *note;
            dragOrigin = e.position;
            const auto bounds = noteBounds(*note, a);
            drag = e.position.x >= bounds.getRight() - juce::jmin(6.0f, bounds.getWidth() * 0.4f)
                 ? Drag::resize : Drag::move;
        }
        updateSelection();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        const auto a = areas();
        if (e.mods.isPopupMenu() || !a.grid.contains(e.getPosition()) || hitNote(e.position, a) != 0) return;
        const auto start = snapStep(e.position.x, a);
        selected = pattern.add(pitchAt(e.position.y, a), start, snapSteps(), 0.8f);
        drag = Drag::none;
        if (selected != 0) changed();
        else updateSelection();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        const auto a = areas();
        if (drag == Drag::audition) {
            if (a.piano.contains(e.getPosition())) preview(pitchAt(e.position.y, a));
            else releaseAudition();
            return;
        }
        if (drag == Drag::velocity) { changeVelocity(e.position.y, a); return; }
        const auto* current = pattern.find(selected);
        if (current == nullptr) return;
        const auto delta = (e.position.x - dragOrigin.x) / stepSize(a);
        if (drag == Drag::move) {
            const auto lastSnappedStart = (pattern.loopSteps - dragOriginal.lengthSteps) / snapSteps() * snapSteps();
            const auto start = juce::jlimit(0, lastSnappedStart,
                juce::roundToInt((dragOriginal.startStep + delta) / snapSteps()) * snapSteps());
            const auto pitch = juce::jlimit(lowestPitch, highestPitch,
                dragOriginal.pitch - juce::roundToInt((e.position.y - dragOrigin.y) / rowSize(a)));
            if ((current->startStep != start || current->pitch != pitch) && pattern.move(selected, pitch, start))
                changed();
        } else if (drag == Drag::resize) {
            const auto end = juce::roundToInt((dragOriginal.startStep + dragOriginal.lengthSteps + delta)
                                            / snapSteps()) * snapSteps();
            const auto length = juce::jlimit(1, pattern.loopSteps - current->startStep, end - current->startStep);
            if (current->lengthSteps != length && pattern.resize(selected, length)) changed();
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        releaseAudition();
        drag = Drag::none;
    }

    void mouseExit(const juce::MouseEvent&) override { releaseAudition(); }

    void mouseMove(const juce::MouseEvent& e) override {
        const auto a = areas();
        if (const auto* note = pattern.find(hitNote(e.position, a))) {
            const auto bounds = noteBounds(*note, a);
            setMouseCursor(e.position.x >= bounds.getRight() - juce::jmin(6.0f, bounds.getWidth() * 0.4f)
                               ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
        } else setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
        if (wheel.deltaY != 0.0f) setLowestVisible(lowestVisiblePitch + (wheel.deltaY > 0 ? 3 : -3));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (!hasKeyboardFocus(false) || key.getModifiers().isCtrlDown()
            || key.getModifiers().isAltDown() || key.getModifiers().isCommandDown()) return false;
        const auto code = key.getKeyCode();
        if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey) {
            setLowestVisible(lowestVisiblePitch + (code == juce::KeyPress::pageUpKey ? 12 : -12));
            return true;
        }
        const auto* note = pattern.find(selected);
        if (note == nullptr) return false;
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
            eraseSelected();
            return true;
        }
        if (code != juce::KeyPress::leftKey && code != juce::KeyPress::rightKey
            && code != juce::KeyPress::upKey && code != juce::KeyPress::downKey) return false;
        if (key.getModifiers().isShiftDown() && (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)) {
            const auto length = juce::jlimit(1, pattern.loopSteps - note->startStep,
                note->lengthSteps + (code == juce::KeyPress::rightKey ? snapSteps() : -snapSteps()));
            if (length != note->lengthSteps && pattern.resize(selected, length)) changed();
        } else {
            const auto pitch = juce::jlimit(lowestPitch, highestPitch, note->pitch
                + (code == juce::KeyPress::upKey ? 1 : code == juce::KeyPress::downKey ? -1 : 0));
            const auto start = juce::jlimit(0, pattern.loopSteps - note->lengthSteps, note->startStep
                + (code == juce::KeyPress::rightKey ? snapSteps() : code == juce::KeyPress::leftKey ? -snapSteps() : 0));
            if ((pitch != note->pitch || start != note->startStep) && pattern.move(selected, pitch, start)) {
                if (pitch < lowestVisiblePitch) setLowestVisible(pitch);
                if (pitch > topPitch()) setLowestVisible(pitch - visiblePitches + 1);
                changed();
            }
        }
        return true;
    }

    void focusLost(FocusChangeType) override { releaseAudition(); repaint(); }
    void focusGained(FocusChangeType) override { repaint(); }
    void visibilityChanged() override { if (!isShowing()) releaseAudition(); }

private:
    struct Areas { juce::Rectangle<int> ruler, piano, grid, velocity; };
    enum class Drag { none, move, resize, velocity, audition };
    static constexpr int lowestPitch = 36, highestPitch = 95, visiblePitches = 24;

    Areas areas() const {
        const int gridX = 64, gridY = 68, gridWidth = juce::jmax(64, getWidth() - gridX - 26);
        const int gridHeight = juce::jmax(48, getHeight() - gridY - 76 - 44 - 6);
        return { { gridX, 44, gridWidth, 24 }, { 10, gridY, 54, gridHeight },
                 { gridX, gridY, gridWidth, gridHeight }, { gridX, gridY + gridHeight + 6, gridWidth, 76 } };
    }
    float stepSize(const Areas& a) const { return a.grid.getWidth() / static_cast<float>(juce::jmax(1, pattern.loopSteps)); }
    static float rowSize(const Areas& a) { return a.grid.getHeight() / static_cast<float>(visiblePitches); }
    float stepX(double step, const Areas& a) const { return a.grid.getX() + static_cast<float>(step) * stepSize(a); }
    int topPitch() const { return lowestVisiblePitch + visiblePitches - 1; }
    int pitchAt(float y, const Areas& a) const {
        return juce::jlimit(lowestVisiblePitch, topPitch(), topPitch() - static_cast<int>((y - a.grid.getY()) / rowSize(a)));
    }
    int snapSteps() const { return juce::jmax(1, snap.getSelectedId()); }
    int snapStep(float x, const Areas& a) const {
        return juce::jlimit(0, pattern.loopSteps - 1,
            static_cast<int>((x - a.grid.getX()) / stepSize(a) / snapSteps()) * snapSteps());
    }
    static bool isBlack(int pitch) {
        const auto p = pitch % 12;
        return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
    }
    static juce::String noteName(int pitch) {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String(names[pitch % 12]) + juce::String(pitch / 12 - 1);
    }
    juce::Rectangle<float> noteBounds(const PianoRollNote& note, const Areas& a) const {
        return { stepX(note.startStep, a) + 1.0f,
                 a.grid.getY() + (topPitch() - note.pitch) * rowSize(a) + 1.0f,
                 juce::jmax(2.0f, note.lengthSteps * stepSize(a) - 2.0f), juce::jmax(2.0f, rowSize(a) - 2.0f) };
    }
    std::uint32_t hitNote(juce::Point<float> position, const Areas& a) const {
        if (!a.grid.toFloat().contains(position)) return 0;
        if (const auto* note = pattern.find(selected))
            if (noteBounds(*note, a).contains(position)) return selected;
        for (auto i = pattern.count; i > 0; --i)
            if (noteBounds(pattern.notes[i - 1], a).contains(position)) return pattern.notes[i - 1].id;
        return 0;
    }
    float velocityX(const PianoRollNote& note, const Areas& a) const { return stepX(note.startStep, a) + 3.0f; }
    static float velocityY(float value, const Areas& a) {
        return a.velocity.getBottom() - 8.0f - value * (a.velocity.getHeight() - 18.0f);
    }
    std::uint32_t hitVelocity(float x, const Areas& a) const {
        const auto radius = juce::jmax(7.0f, stepSize(a) * 0.45f);
        if (const auto* note = pattern.find(selected))
            if (std::abs(velocityX(*note, a) - x) <= radius) return selected;
        auto distance = radius;
        std::uint32_t found = 0;
        for (auto i = pattern.count; i > 0; --i) {
            const auto delta = std::abs(velocityX(pattern.notes[i - 1], a) - x);
            if (delta <= distance) { distance = delta; found = pattern.notes[i - 1].id; }
        }
        return found;
    }
    void paintNote(juce::Graphics& g, const PianoRollNote& note, const Areas& a, bool isSelected) const {
        if (note.pitch < lowestVisiblePitch || note.pitch > topPitch()) return;
        const auto bounds = noteBounds(note, a);
        g.setColour(isSelected ? juce::Colour(0xff71ead8) : juce::Colour(0xff8bdc98));
        g.fillRoundedRectangle(bounds, 2.0f);
        g.setColour(isSelected ? juce::Colour(0xffe0fff7) : juce::Colour(0xffb4edbc));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 1.5f, 1.0f);
        g.setColour(juce::Colour(0xff235655));
        g.drawVerticalLine(juce::roundToInt(bounds.getRight() - 3.0f), bounds.getY() + 2.0f, bounds.getBottom() - 2.0f);
        if (bounds.getWidth() > 35.0f && bounds.getHeight() > 12.0f) {
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(noteName(note.pitch), bounds.reduced(4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }
    void paintVelocity(juce::Graphics& g, const PianoRollNote& note, const Areas& a, bool isSelected) const {
        const auto x = velocityX(note, a), y = velocityY(note.velocity, a);
        g.setColour(isSelected ? juce::Colour(0xff7ff4df) : juce::Colour(0xff83c993));
        g.drawLine(x, velocityY(0.0f, a), x, y, isSelected ? 2.4f : 1.4f);
        g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
    }
    void changeVelocity(float y, const Areas& a) {
        const auto* note = pattern.find(selected);
        if (note == nullptr) return;
        const auto value = juce::jlimit(1.0f / 127.0f, 1.0f,
            (a.velocity.getBottom() - 8.0f - y) / (a.velocity.getHeight() - 18.0f));
        if (note->velocity != value && pattern.setVelocity(selected, value)) changed();
    }
    void eraseSelected() {
        if (!pattern.erase(selected)) { updateSelection(); return; }
        selected = 0;
        drag = Drag::none;
        changed();
    }
    void updateSelection() {
        const auto* note = pattern.find(selected);
        velocity.setEnabled(note != nullptr);
        if (note != nullptr) velocity.setValue(juce::roundToInt(note->velocity * 127.0f), juce::dontSendNotification);
        clearButton.setEnabled(pattern.count != 0);
        exampleButton.setEnabled(pattern.count == 0);
        repaint();
    }
    void changed() {
        updateSelection();
        if (onChange) onChange();
    }
    void preview(int pitch) {
        if (pitch == auditionPitch) return;
        releasePreviewNote();
        auditionPitch = pitch;
        if (onAudition) onAudition(pitch, 0.8f);
        repaint();
    }
    void releasePreviewNote() {
        const auto note = auditionPitch;
        auditionPitch = -1;
        if (note >= 0 && onAudition) onAudition(note, 0.0f);
        if (note >= 0) repaint();
    }
    void loadExample() {
        if (pattern.count != 0) return;
        static constexpr int pitches[] { 60, 64, 67, 64, 62, 65, 69, 65, 64, 67, 71, 67, 62, 65, 67, 60 };
        for (int i = 0; i < 16; ++i)
            pattern.add(pitches[i], i * 4, i == 15 ? 4 : 3, i % 4 == 0 ? 0.86f : 0.65f);
        selected = pattern.count != 0 ? pattern.notes[0].id : 0;
        changed();
    }
    void setLowestVisible(int pitch) {
        const auto next = juce::jlimit(lowestPitch, highestPitch - visiblePitches + 1, pitch);
        if (next == lowestVisiblePitch) return;
        releaseAudition();
        lowestVisiblePitch = next;
        updatePitchScroll();
        repaint();
    }
    void updatePitchScroll() {
        pitchScroll.setCurrentRange(highestPitch - topPitch(), visiblePitches, juce::dontSendNotification);
    }
    void scrollBarMoved(juce::ScrollBar*, double newStart) override {
        setLowestVisible(highestPitch - juce::roundToInt(newStart) - visiblePitches + 1);
    }

    PianoRollPattern pattern;
    juce::ComboBox snap;
    juce::Label snapLabel, velocityLabel;
    juce::Slider velocity;
    juce::TextButton clearButton, exampleButton;
    juce::ScrollBar pitchScroll { true };
    std::uint32_t selected {};
    PianoRollNote dragOriginal {};
    juce::Point<float> dragOrigin;
    Drag drag { Drag::none };
    int lowestVisiblePitch { 48 }, auditionPitch { -1 };
    double playhead { -1.0 };
};

} // namespace alien
