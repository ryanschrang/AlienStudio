#pragma once
// Custom-styled editor panels for the stock effects. Controls are bound to the
// effects' host parameters, so dragging, typing a value, double-clicking to reset
// and (soon) automation all go through the same parameters the audio thread reads.
#include "StockEffects.h"
#include "StudioTheme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace alien::stock {

using Attachments = juce::AudioProcessorValueTreeState;

// A captioned rotary knob bound to one parameter.
class Knob final : public juce::Component {
public:
    Knob(Effect& effect, const char* parameterId, const juce::String& captionText, juce::Colour accent)
        : caption(captionText) {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 82, 18);
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
        slider.setName(captionText);
        addAndMakeVisible(slider);
        attachment = std::make_unique<Attachments::SliderAttachment>(effect.parameters, parameterId, slider);
        if (auto* p = effect.parameters.getParameter(parameterId)) {
            slider.setDoubleClickReturnValue(true, p->convertFrom0to1(p->getDefaultValue()));
            slider.setTitle(p->getName(64)); // the full name, for screen readers and tooltips
            slider.setTooltip(p->getName(64) + ": double-click to reset");
        }
    }
    juce::Slider slider;
    void setEnabledLook(bool on) { slider.setEnabled(on); }
    void paint(juce::Graphics& g) override {
        ui::drawCaption(g, caption, getLocalBounds().removeFromTop(14), slider.isEnabled() ? ui::colours::textDim : ui::colours::textFaint,
                        juce::Justification::centred);
    }
    void resized() override { slider.setBounds(getLocalBounds().withTrimmedTop(14)); }
private:
    juce::String caption;
    std::unique_ptr<Attachments::SliderAttachment> attachment;
};

// Shared frame: title bar, content area, and a 30 Hz tick for live displays.
class EffectEditor : public juce::AudioProcessorEditor, protected juce::Timer {
public:
    static constexpr int headerHeight = 46, margin = 18;
    EffectEditor(Effect& e, juce::Colour accentColour, int width, int height)
        : juce::AudioProcessorEditor(e), effect(e), accent(accentColour), desiredWidth(width), desiredHeight(height) {}
    ~EffectEditor() override { stopTimer(); }
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        g.fillAll(surface);
        auto bar = getLocalBounds().removeFromTop(headerHeight).toFloat();
        g.setColour(raised);
        g.fillRect(bar);
        g.setColour(line);
        g.fillRect(bar.removeFromBottom(1.0f));
        g.setColour(accent);
        g.fillRoundedRectangle(juce::Rectangle<float>(margin, 14.0f, 4.0f, 18.0f), 2.0f);
        g.setColour(text);
        g.setFont(ui::font(16.0f, true));
        g.drawText(effect.getName(), juce::Rectangle<int>(margin + 12, 0, 300, headerHeight), juce::Justification::centredLeft);
        ui::drawCaption(g, "Stock effect", getLocalBounds().removeFromTop(headerHeight).reduced(margin, 0), textFaint,
                        juce::Justification::centredRight);
    }
    juce::Rectangle<int> content() const { return getLocalBounds().withTrimmedTop(headerHeight).reduced(margin); }
    std::size_t knobCount() const noexcept { return knobs.size(); }
    Knob& knob(std::size_t i) { return *knobs[i]; }
protected:
    // Each editor calls this last, once its children exist, so the first layout sees them.
    void finishSetup() { setSize(desiredWidth, desiredHeight); startTimerHz(30); }
    Knob& addKnob(const char* id, const juce::String& caption) {
        knobs.push_back(std::make_unique<Knob>(effect, id, caption, accent));
        addAndMakeVisible(*knobs.back());
        return *knobs.back();
    }
    // Lay knobs out in a row inside `area`, each cell a fixed width.
    void layoutKnobs(juce::Rectangle<int> area, std::size_t first, std::size_t count, int cell = 92) {
        for (std::size_t i = 0; i < count && first + i < knobs.size(); ++i)
            knobs[first + i]->setBounds(area.removeFromLeft(cell).reduced(4, 0));
    }
    void timerCallback() override { tick(); }
    virtual void tick() {}
    Effect& effect;
    juce::Colour accent;
    int desiredWidth, desiredHeight;
    std::vector<std::unique_ptr<Knob>> knobs;
};

// ---- Equaliser -------------------------------------------------------------------

namespace eqmap {
inline constexpr double lowestHz = 20.0, highestHz = 20000.0, rangeDb = 18.0;
inline float frequencyToX(double hz, float width) {
    return width * static_cast<float>(std::log(std::clamp(hz, lowestHz, highestHz) / lowestHz) / std::log(highestHz / lowestHz));
}
inline double xToFrequency(float x, float width) {
    return lowestHz * std::pow(highestHz / lowestHz, std::clamp(static_cast<double>(x / width), 0.0, 1.0));
}
inline float gainToY(double db, float height) { return height * static_cast<float>(0.5 - std::clamp(db, -rangeDb, rangeDb) / (2.0 * rangeDb)); }
inline double yToGain(float y, float height) { return std::clamp((0.5 - static_cast<double>(y / height)) * 2.0 * rangeDb, -rangeDb, rangeDb); }
} // namespace eqmap

// Response curve with a draggable handle per band: drag to set frequency and gain,
// wheel to change the width (Q) of a bell, double-click to flatten a band.
class EqCurve final : public juce::Component {
public:
    explicit EqCurve(EqEffect& e) : eqEffect(e) {
        static const std::array<std::array<const char*, 3>, 4> ids{{
            {eq::lowFreq, eq::lowGain, nullptr}, {eq::m1Freq, eq::m1Gain, eq::m1Q},
            {eq::m2Freq, eq::m2Gain, eq::m2Q}, {eq::highFreq, eq::highGain, nullptr}}};
        for (std::size_t b = 0; b < 4; ++b) {
            frequency[b] = eqEffect.parameters.getParameter(ids[b][0]);
            gain[b] = eqEffect.parameters.getParameter(ids[b][1]);
            quality[b] = ids[b][2] ? eqEffect.parameters.getParameter(ids[b][2]) : nullptr;
        }
    }
    static juce::Colour bandColour(int band) {
        static const std::array<juce::Colour, 4> c{juce::Colour(0xffff6f9f), juce::Colour(0xffffb547), juce::Colour(0xff4fe0b6), juce::Colour(0xff5aa9ff)};
        return c[static_cast<std::size_t>(band)];
    }
    // Total response of the four bands at one frequency, in dB.
    double responseDb(double hz) const {
        const auto s = eqEffect.settings();
        const double rate = eqEffect.currentSampleRate();
        double total = 0;
        for (int b = 0; b < dsp::ParametricEq::bandCount; ++b) {
            const auto& band = s.band[static_cast<std::size_t>(b)];
            total += dsp::biquadMagnitudeDb(dsp::designBiquad(dsp::ParametricEq::kinds[static_cast<std::size_t>(b)], band.frequency, band.gainDb, band.q, rate), hz, rate);
        }
        return total;
    }
    juce::Point<float> handlePosition(int band) const {
        const auto s = eqEffect.settings();
        const auto& b = s.band[static_cast<std::size_t>(band)];
        return {eqmap::frequencyToX(b.frequency, plotWidth()), eqmap::gainToY(b.gainDb, plotHeight())};
    }
    float plotWidth() const { return static_cast<float>(getWidth()); }
    float plotHeight() const { return static_cast<float>(getHeight()); }

    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        const auto area = getLocalBounds().toFloat();
        g.setColour(inset);
        g.fillRoundedRectangle(area, 6.0f);
        g.setFont(ui::monoFont(10.0f));
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            const float x = eqmap::frequencyToX(hz, plotWidth());
            g.setColour(line);
            g.drawVerticalLine(juce::roundToInt(x), 0.0f, plotHeight());
            g.setColour(textFaint);
            g.drawText(hz >= 1000.0 ? juce::String(static_cast<int>(hz / 1000.0)) + "k" : juce::String(static_cast<int>(hz)),
                       juce::Rectangle<float>(x + 3.0f, plotHeight() - 14.0f, 40.0f, 12.0f), juce::Justification::left);
        }
        for (double db : {-12.0, -6.0, 0.0, 6.0, 12.0}) {
            const float y = eqmap::gainToY(db, plotHeight());
            g.setColour(db == 0.0 ? lineStrong : line);
            g.drawHorizontalLine(juce::roundToInt(y), 0.0f, plotWidth());
            g.setColour(textFaint);
            g.drawText((db > 0 ? "+" : "") + juce::String(static_cast<int>(db)), juce::Rectangle<float>(3.0f, y - 12.0f, 28.0f, 12.0f), juce::Justification::left);
        }
        juce::Path curve;
        const float zero = eqmap::gainToY(0.0, plotHeight());
        for (int x = 0; x <= getWidth(); x += 2) {
            const float y = eqmap::gainToY(responseDb(eqmap::xToFrequency(static_cast<float>(x), plotWidth())), plotHeight());
            if (x == 0) curve.startNewSubPath(0.0f, y); else curve.lineTo(static_cast<float>(x), y);
        }
        juce::Path fill(curve);
        fill.lineTo(plotWidth(), zero); fill.lineTo(0.0f, zero); fill.closeSubPath();
        g.setColour(accent.withAlpha(0.14f));
        g.fillPath(fill);
        g.setColour(accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
        for (int b = 0; b < 4; ++b) {
            const auto p = handlePosition(b);
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre(p.translated(0.0f, 1.0f)));
            g.setColour(bandColour(b).withMultipliedBrightness(b == dragged ? 1.3f : 1.0f));
            g.fillEllipse(juce::Rectangle<float>(14.0f, 14.0f).withCentre(p));
            g.setColour(surface);
            g.setFont(ui::font(9.5f, true));
            g.drawText(juce::String(b + 1), juce::Rectangle<float>(14.0f, 14.0f).withCentre(p), juce::Justification::centred);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        dragged = -1;
        float best = 14.0f;
        for (int b = 0; b < 4; ++b) {
            const float d = handlePosition(b).getDistanceFrom(e.position);
            if (d < best) { best = d; dragged = b; }
        }
        if (dragged >= 0) { begin(frequency[static_cast<std::size_t>(dragged)]); begin(gain[static_cast<std::size_t>(dragged)]); }
        repaint();
    }
    // Moves one band to a frequency and gain (what dragging its handle does).
    void moveBand(int band, double hz, double db) {
        set(frequency[static_cast<std::size_t>(band)], hz);
        set(gain[static_cast<std::size_t>(band)], db);
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragged < 0) return;
        moveBand(dragged, eqmap::xToFrequency(e.position.x, plotWidth()), eqmap::yToGain(e.position.y, plotHeight()));
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (dragged >= 0) { end(frequency[static_cast<std::size_t>(dragged)]); end(gain[static_cast<std::size_t>(dragged)]); }
        dragged = -1;
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        for (int b = 0; b < 4; ++b)
            if (handlePosition(b).getDistanceFrom(e.position) < 14.0f) {
                auto* g = gain[static_cast<std::size_t>(b)];
                g->beginChangeGesture(); g->setValueNotifyingHost(g->getDefaultValue()); g->endChangeGesture();
            }
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        for (int b = 0; b < 4; ++b) {
            auto* q = quality[static_cast<std::size_t>(b)];
            if (q && handlePosition(b).getDistanceFrom(e.position) < 18.0f) {
                q->beginChangeGesture();
                q->setValueNotifyingHost(std::clamp(q->getValue() + wheel.deltaY * 0.15f, 0.0f, 1.0f));
                q->endChangeGesture();
                return;
            }
        }
    }
private:
    static void begin(juce::RangedAudioParameter* p) { if (p) p->beginChangeGesture(); }
    static void end(juce::RangedAudioParameter* p) { if (p) p->endChangeGesture(); }
    static void set(juce::RangedAudioParameter* p, double value) {
        if (p) p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(value)));
    }
    EqEffect& eqEffect;
    std::array<juce::RangedAudioParameter*, 4> frequency{}, gain{}, quality{};
    int dragged{-1};
    juce::Colour accent{ui::colours::accent};
};

class EqEditor final : public EffectEditor {
public:
    EqCurve& response() { return curve; }
    explicit EqEditor(EqEffect& e) : EffectEditor(e, ui::colours::accent, 700, 450), curve(e) {
        addAndMakeVisible(curve);
        for (auto [id, name] : std::initializer_list<std::pair<const char*, const char*>>{
                 {eq::lowFreq, "Freq"}, {eq::lowGain, "Gain"},
                 {eq::m1Freq, "Freq"}, {eq::m1Gain, "Gain"}, {eq::m1Q, "Q"},
                 {eq::m2Freq, "Freq"}, {eq::m2Gain, "Gain"}, {eq::m2Q, "Q"},
                 {eq::highFreq, "Freq"}, {eq::highGain, "Gain"}, {eq::output, "Level"}})
            addKnob(id, name);
        finishSetup();
    }
    void resized() override {
        auto area = content();
        curve.setBounds(area.removeFromTop(210));
        area.removeFromTop(10);
        auto headings = area.removeFromTop(18);
        const int cell = area.getWidth() / 11;
        layoutKnobs(area, 0, 11, cell);
        struct Group { int first, count; const char* title; };
        groups.clear();
        for (auto g : {Group{0, 2, "1  Low shelf"}, Group{2, 3, "2  Low mid"}, Group{5, 3, "3  High mid"}, Group{8, 2, "4  High shelf"}, Group{10, 1, "Output"}})
            groups.push_back({headings.withX(area.getX() + g.first * cell).withWidth(g.count * cell).reduced(4, 0), g.title, g.first});
    }
    void paint(juce::Graphics& g) override {
        EffectEditor::paint(g);
        for (const auto& group : groups) {
            const auto colour = group.first < 10 ? EqCurve::bandColour(group.first < 2 ? 0 : group.first < 5 ? 1 : group.first < 8 ? 2 : 3) : ui::colours::textDim;
            ui::drawCaption(g, group.title, group.area, colour);
            g.setColour(colour.withAlpha(0.5f));
            g.fillRect(group.area.withTop(group.area.getBottom() - 1).toFloat());
        }
    }
    void tick() override {
        const auto s = static_cast<EqEffect&>(effect).settings();
        float hash = static_cast<float>(s.outputDb);
        for (const auto& b : s.band) hash = hash * 1.0001f + static_cast<float>(b.frequency * 0.001 + b.gainDb + b.q);
        if (hash != lastHash) { lastHash = hash; curve.repaint(); }
    }
private:
    struct Heading { juce::Rectangle<int> area; juce::String title; int first; };
    EqCurve curve;
    std::vector<Heading> groups;
    float lastHash{-1.0f};
};

// ---- Dynamics ----------------------------------------------------------------------

// Vertical gain-reduction bar (0 at the top, down to `range` dB).
class ReductionMeter final : public juce::Component {
public:
    explicit ReductionMeter(double rangeDb) : range(rangeDb) {}
    void setReduction(double db) {
        const double target = std::clamp(db, 0.0, range);
        shown = target > shown ? target : shown * 0.85 + target * 0.15;
        if (std::abs(shown - painted) > 0.02) { painted = shown; repaint(); }
    }
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        auto r = getLocalBounds().toFloat();
        ui::drawCaption(g, "Reduction", r.removeFromTop(14.0f).toNearestInt(), textDim, juce::Justification::centred);
        auto readout = r.removeFromBottom(18.0f);
        g.setColour(inset);
        g.fillRoundedRectangle(r, 3.0f);
        const float fill = static_cast<float>(painted / range) * r.getHeight();
        g.setColour(warning);
        g.fillRoundedRectangle(r.reduced(3.0f).removeFromTop(std::max(0.0f, fill - 3.0f)), 2.0f);
        g.setColour(textDim);
        g.setFont(ui::monoFont(11.0f));
        g.drawText("-" + juce::String(painted, 1) + " dB", readout, juce::Justification::centred);
    }
private:
    double range, shown{}, painted{};
};

// Input level against output level for the current threshold, ratio, knee and make-up gain.
class TransferCurve final : public juce::Component {
public:
    explicit TransferCurve(CompressorEffect& e) : compressor(e) {}
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        const auto area = getLocalBounds().toFloat();
        g.setColour(inset);
        g.fillRoundedRectangle(area, 6.0f);
        const auto s = compressor.settings();
        const auto toX = [&](double db) { return static_cast<float>((db + 60.0) / 60.0) * area.getWidth(); };
        const auto toY = [&](double db) { return area.getHeight() - static_cast<float>((std::clamp(db, -60.0, 0.0) + 60.0) / 60.0) * area.getHeight(); };
        g.setColour(line);
        for (double db = -48; db < 0; db += 12) { g.drawVerticalLine(juce::roundToInt(toX(db)), 0, area.getHeight()); g.drawHorizontalLine(juce::roundToInt(toY(db)), 0, area.getWidth()); }
        g.setColour(lineStrong);
        g.drawLine(toX(-60), toY(-60), toX(0), toY(0), 1.0f);               // 1:1
        g.setColour(textFaint);
        g.drawVerticalLine(juce::roundToInt(toX(s.thresholdDb)), 0, area.getHeight());
        juce::Path curve;
        for (int i = 0; i <= 120; ++i) {
            const double in = -60.0 + i * 0.5;
            const double out = in - dsp::compressorReductionDb(in, s.thresholdDb, s.ratio, s.kneeDb) + s.makeupDb;
            if (i == 0) curve.startNewSubPath(toX(in), toY(out)); else curve.lineTo(toX(in), toY(out));
        }
        g.setColour(accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));
        ui::drawCaption(g, "In -> out", getLocalBounds().reduced(8, 4).removeFromTop(14), textFaint);
    }
    juce::Colour accent{ui::colours::warning};
private:
    CompressorEffect& compressor;
};

class CompressorEditor final : public EffectEditor {
public:
    explicit CompressorEditor(CompressorEffect& e) : EffectEditor(e, ui::colours::warning, 640, 370), curve(e), meter(24.0) {
        addAndMakeVisible(curve); addAndMakeVisible(meter);
        for (auto [id, name] : std::initializer_list<std::pair<const char*, const char*>>{
                 {comp::threshold, "Threshold"}, {comp::ratio, "Ratio"}, {comp::attack, "Attack"}, {comp::release, "Release"},
                 {comp::knee, "Knee"}, {comp::makeup, "Make-up"}, {comp::mix, "Mix"}})
            addKnob(id, name);
        finishSetup();
    }
    void resized() override {
        auto area = content();
        auto left = area.removeFromLeft(210);
        curve.setBounds(left.removeFromTop(210));
        area.removeFromLeft(12);
        meter.setBounds(area.removeFromRight(70));
        area.removeFromRight(8);
        // knobs in two rows beside the curve
        auto top = area.removeFromTop(area.getHeight() / 2);
        layoutKnobs(top.withTrimmedBottom(6), 0, 4, top.getWidth() / 4);
        layoutKnobs(area.withTrimmedTop(6), 4, 3, area.getWidth() / 4);
    }
    void tick() override { meter.setReduction(static_cast<CompressorEffect&>(effect).gainReductionDb()); curve.repaint(); }
private:
    TransferCurve curve;
    ReductionMeter meter;
};

class LimiterEditor final : public EffectEditor {
public:
    explicit LimiterEditor(LimiterEffect& e) : EffectEditor(e, juce::Colour(0xffff6f9f), 460, 300), meter(18.0) {
        addAndMakeVisible(meter);
        addKnob(limiter::gain, "Gain"); addKnob(limiter::ceiling, "Ceiling"); addKnob(limiter::release, "Release");
        finishSetup();
    }
    void resized() override {
        auto area = content();
        meter.setBounds(area.removeFromRight(80));
        area.removeFromRight(12);
        const int cell = area.getWidth() / 3;
        layoutKnobs(area.removeFromTop(150).withTrimmedTop(8), 0, 3, cell);
        noteArea = area.withTrimmedTop(10);
    }
    void paint(juce::Graphics& g) override {
        EffectEditor::paint(g);
        g.setColour(ui::colours::textFaint);
        g.setFont(ui::font(12.0f));
        g.drawFittedText("Sample peaks never pass the ceiling. The 2 ms look-ahead adds latency, which Alien Studio compensates on every other track.",
                         noteArea, juce::Justification::topLeft, 3);
    }
    void tick() override { meter.setReduction(static_cast<LimiterEffect&>(effect).gainReductionDb()); }
private:
    ReductionMeter meter;
    juce::Rectangle<int> noteArea;
};

// ---- Time-based -----------------------------------------------------------------------

// Bars for the first repeats, decaying with the feedback setting; they alternate
// sides in ping-pong mode.
class EchoDisplay final : public juce::Component {
public:
    explicit EchoDisplay(DelayEffect& e) : delayEffect(e) {}
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        const auto area = getLocalBounds().toFloat();
        g.setColour(inset);
        g.fillRoundedRectangle(area, 6.0f);
        const auto& p = delayEffect.parameters;
        const double fb = *p.getRawParameterValue(delay::feedback);
        const bool ping = *p.getRawParameterValue(delay::pingPong) > 0.5f;
        const int count = 8;
        const float slot = (area.getWidth() - 24.0f) / static_cast<float>(count + 1);
        for (int i = 0; i <= count; ++i) {
            const double level = i == 0 ? 1.0 : std::pow(fb, i - 1) * 0.9;
            const float h = static_cast<float>(level) * (area.getHeight() - 36.0f);
            const bool right = ping && (i % 2 == 0) && i > 0 ? true : false;
            auto bar = juce::Rectangle<float>(12.0f + slot * i + slot * 0.2f, area.getBottom() - 12.0f - h, slot * 0.6f, h);
            g.setColour((i == 0 ? textFaint : right ? solo : mute).withAlpha(i == 0 ? 0.6f : 0.9f));
            g.fillRoundedRectangle(bar, 2.0f);
        }
        ui::drawCaption(g, ping ? "Repeats - ping-pong" : "Repeats", getLocalBounds().reduced(8, 4).removeFromTop(14), textFaint);
    }
private:
    DelayEffect& delayEffect;
};

class DelayEditor final : public EffectEditor {
public:
    explicit DelayEditor(DelayEffect& e) : EffectEditor(e, ui::colours::solo, 620, 340), echoes(e) {
        addAndMakeVisible(echoes);
        timeKnob = &addKnob(delay::time, "Time");
        addKnob(delay::feedback, "Feedback"); addKnob(delay::tone, "Tone"); addKnob(delay::mix, "Mix");
        sync.setButtonText("Tempo sync"); pingPong.setButtonText("Ping-pong");
        addAndMakeVisible(sync); addAndMakeVisible(pingPong); addAndMakeVisible(division);
        division.addItemList(juce::StringArray{"1/16", "1/8 triplet", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2", "1 bar"}, 1);
        syncAttachment = std::make_unique<Attachments::ButtonAttachment>(e.parameters, delay::sync, sync);
        pingAttachment = std::make_unique<Attachments::ButtonAttachment>(e.parameters, delay::pingPong, pingPong);
        divisionAttachment = std::make_unique<Attachments::ComboBoxAttachment>(e.parameters, delay::division, division);
        sync.onStateChange = [this] { updateEnabled(); };
        updateEnabled();
        finishSetup();
    }
    void resized() override {
        auto area = content();
        echoes.setBounds(area.removeFromTop(120));
        area.removeFromTop(12);
        auto row = area.removeFromTop(32);
        sync.setBounds(row.removeFromLeft(130)); division.setBounds(row.removeFromLeft(130)); row.removeFromLeft(12); pingPong.setBounds(row.removeFromLeft(130));
        area.removeFromTop(8);
        layoutKnobs(area, 0, 4, area.getWidth() / 4);
    }
private:
    void updateEnabled() { const bool on = sync.getToggleState(); timeKnob->setEnabledLook(!on); division.setEnabled(on); }
    EchoDisplay echoes;
    Knob* timeKnob{};
    juce::ToggleButton sync, pingPong;
    juce::ComboBox division;
    std::unique_ptr<Attachments::ButtonAttachment> syncAttachment, pingAttachment;
    std::unique_ptr<Attachments::ComboBoxAttachment> divisionAttachment;
};

// Shape of the decay for the current size and pre-delay.
class DecayDisplay final : public juce::Component {
public:
    explicit DecayDisplay(ReverbEffect& e) : reverbEffect(e) {}
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        const auto area = getLocalBounds().toFloat();
        g.setColour(inset);
        g.fillRoundedRectangle(area, 6.0f);
        const auto& p = reverbEffect.parameters;
        const double size = *p.getRawParameterValue(reverb::size), pre = *p.getRawParameterValue(reverb::preDelay);
        const double tail = ReverbEffect::tailSecondsFor(size, pre), span = std::max(tail * 1.1, 0.5);
        const float preX = static_cast<float>(pre * 0.001 / span) * area.getWidth();
        juce::Path decay;
        decay.startNewSubPath(0.0f, area.getBottom() - 10.0f);
        decay.lineTo(preX, area.getBottom() - 10.0f);
        decay.lineTo(preX, 24.0f);
        for (int x = 0; x <= 100; ++x) {
            const double t = (pre * 0.001) + (tail - pre * 0.001) * x / 100.0;
            const float px = static_cast<float>(t / span) * area.getWidth();
            const float level = static_cast<float>(std::pow(10.0, -3.0 * x / 100.0)); // -60 dB over the tail
            decay.lineTo(px, area.getBottom() - 10.0f - level * (area.getHeight() - 34.0f));
        }
        g.setColour(accent.withAlpha(0.18f));
        juce::Path filled(decay); filled.lineTo(area.getWidth() * static_cast<float>(tail / span), area.getBottom() - 10.0f); filled.closeSubPath();
        g.fillPath(filled);
        g.setColour(accent);
        g.strokePath(decay, juce::PathStrokeType(2.0f));
        g.setColour(textDim);
        g.setFont(ui::monoFont(11.0f));
        g.drawText("~" + juce::String(tail, 1) + " s tail", area.reduced(10.0f, 6.0f).toNearestInt(), juce::Justification::topRight);
        ui::drawCaption(g, "Decay", getLocalBounds().reduced(8, 4).removeFromTop(14), textFaint);
    }
    juce::Colour accent{juce::Colour(0xff9d85ff)};
private:
    ReverbEffect& reverbEffect;
};

class ReverbEditor final : public EffectEditor {
public:
    explicit ReverbEditor(ReverbEffect& e) : EffectEditor(e, juce::Colour(0xff9d85ff), 560, 340), decay(e) {
        addAndMakeVisible(decay);
        addKnob(reverb::size, "Size"); addKnob(reverb::damping, "Damping"); addKnob(reverb::width, "Width");
        addKnob(reverb::preDelay, "Pre-delay"); addKnob(reverb::mix, "Mix");
        finishSetup();
    }
    void resized() override {
        auto area = content();
        decay.setBounds(area.removeFromTop(130));
        area.removeFromTop(14);
        layoutKnobs(area, 0, 5, area.getWidth() / 5);
    }
    void tick() override {
        const auto& p = effect.parameters;
        const float key = *p.getRawParameterValue(reverb::size) * 7.0f + *p.getRawParameterValue(reverb::preDelay);
        if (key != lastKey) { lastKey = key; decay.repaint(); }
    }
private:
    DecayDisplay decay;
    float lastKey{-1.0f};
};

// ---- Filter ------------------------------------------------------------------------------

namespace filtermap {
inline constexpr double topDb = 18.0, bottomDb = -42.0;
inline float gainToY(double db, float height) { return height * static_cast<float>((topDb - std::clamp(db, bottomDb, topDb)) / (topDb - bottomDb)); }
} // namespace filtermap

// Response curve with one handle at the cutoff: drag sideways for the cutoff, up and down
// for the resonance (the mouse wheel also changes it); double-click resets both.
class FilterCurve final : public juce::Component {
public:
    explicit FilterCurve(FilterEffect& e)
        : filterEffect(e), cutoff(e.parameters.getParameter(filter::cutoff)), resonance(e.parameters.getParameter(filter::resonance)) {}
    // Response of the filtered path at one frequency, in dB.
    double responseDb(double hz) const {
        return dsp::MultimodeFilter::responseDb(filterEffect.settings(), hz, filterEffect.currentSampleRate());
    }
    juce::Point<float> handlePosition() const {
        const double hz = filterEffect.settings().frequency;
        return {eqmap::frequencyToX(hz, static_cast<float>(getWidth())), filtermap::gainToY(responseDb(hz), static_cast<float>(getHeight()))};
    }
    // Sets the cutoff and resonance together (what dragging the handle does).
    void moveHandle(double hz, double q) {
        set(cutoff, hz); set(resonance, q);
        repaint();
    }
    void paint(juce::Graphics& g) override {
        using namespace ui::colours;
        const auto area = getLocalBounds().toFloat();
        const float w = area.getWidth(), h = area.getHeight();
        g.setColour(inset);
        g.fillRoundedRectangle(area, 6.0f);
        g.setFont(ui::monoFont(10.0f));
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            const float x = eqmap::frequencyToX(hz, w);
            g.setColour(line);
            g.drawVerticalLine(juce::roundToInt(x), 0.0f, h);
            g.setColour(textFaint);
            g.drawText(hz >= 1000.0 ? juce::String(static_cast<int>(hz / 1000.0)) + "k" : juce::String(static_cast<int>(hz)),
                       juce::Rectangle<float>(x + 3.0f, h - 14.0f, 40.0f, 12.0f), juce::Justification::left);
        }
        for (double db : {-36.0, -24.0, -12.0, 0.0, 12.0}) {
            const float y = filtermap::gainToY(db, h);
            g.setColour(db == 0.0 ? lineStrong : line);
            g.drawHorizontalLine(juce::roundToInt(y), 0.0f, w);
            g.setColour(textFaint);
            g.drawText((db > 0 ? "+" : "") + juce::String(static_cast<int>(db)), juce::Rectangle<float>(3.0f, y - 12.0f, 28.0f, 12.0f), juce::Justification::left);
        }
        juce::Path curve;
        for (int x = 0; x <= getWidth(); x += 2) {
            const float y = filtermap::gainToY(responseDb(eqmap::xToFrequency(static_cast<float>(x), w)), h);
            if (x == 0) curve.startNewSubPath(0.0f, y); else curve.lineTo(static_cast<float>(x), y);
        }
        juce::Path fill(curve);
        fill.lineTo(w, h); fill.lineTo(0.0f, h); fill.closeSubPath();
        g.setColour(accent.withAlpha(0.14f));
        g.fillPath(fill);
        g.setColour(accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
        const auto p = handlePosition();
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre(p.translated(0.0f, 1.0f)));
        g.setColour(accent.withMultipliedBrightness(dragging ? 1.3f : 1.0f));
        g.fillEllipse(juce::Rectangle<float>(14.0f, 14.0f).withCentre(p));
        g.setColour(textDim);
        g.drawText(filter::modeNames()[static_cast<int>(filterEffect.settings().mode)] + "  " + formatFrequency(static_cast<float>(filterEffect.settings().frequency)),
                   area.reduced(10.0f, 6.0f).toNearestInt(), juce::Justification::topRight);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        dragging = true;
        startResonance = resonance->getValue();
        cutoff->beginChangeGesture(); resonance->beginChangeGesture();
        mouseDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging) return;
        const float up = static_cast<float>(e.getDistanceFromDragStartY()) / static_cast<float>(std::max(1, getHeight()));
        cutoff->setValueNotifyingHost(cutoff->convertTo0to1(static_cast<float>(eqmap::xToFrequency(e.position.x, static_cast<float>(getWidth())))));
        resonance->setValueNotifyingHost(std::clamp(startResonance - up, 0.0f, 1.0f));
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (dragging) { cutoff->endChangeGesture(); resonance->endChangeGesture(); }
        dragging = false;
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override {
        for (auto* p : {cutoff, resonance}) { p->beginChangeGesture(); p->setValueNotifyingHost(p->getDefaultValue()); p->endChangeGesture(); }
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
        resonance->beginChangeGesture();
        resonance->setValueNotifyingHost(std::clamp(resonance->getValue() + wheel.deltaY * 0.15f, 0.0f, 1.0f));
        resonance->endChangeGesture();
    }
    juce::Colour accent{juce::Colour(0xff4fe0b6)};
private:
    static void set(juce::RangedAudioParameter* p, double v) { p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(v))); p->endChangeGesture(); }
    FilterEffect& filterEffect;
    juce::RangedAudioParameter *cutoff, *resonance;
    float startResonance{};
    bool dragging{false};
};

class FilterEditor final : public EffectEditor {
public:
    explicit FilterEditor(FilterEffect& e) : EffectEditor(e, juce::Colour(0xff4fe0b6), 560, 390), curve(e) {
        addAndMakeVisible(curve);
        addKnob(filter::cutoff, "Cutoff"); addKnob(filter::resonance, "Resonance"); addKnob(filter::mix, "Mix"); addKnob(filter::output, "Level");
        mode.addItemList(filter::modeNames(), 1); slope.addItemList(filter::slopeNames(), 1);
        mode.setTitle("Filter mode"); slope.setTitle("Filter slope");
        addAndMakeVisible(mode); addAndMakeVisible(slope);
        modeAttachment = std::make_unique<Attachments::ComboBoxAttachment>(e.parameters, filter::mode, mode);
        slopeAttachment = std::make_unique<Attachments::ComboBoxAttachment>(e.parameters, filter::slope, slope);
        finishSetup();
    }
    FilterCurve& response() { return curve; }
    juce::ComboBox& modeBox() { return mode; }
    juce::ComboBox& slopeBox() { return slope; }
    void resized() override {
        auto area = content();
        curve.setBounds(area.removeFromTop(170));
        area.removeFromTop(12);
        auto row = area.removeFromTop(28);
        mode.setBounds(row.removeFromLeft(150)); row.removeFromLeft(12); slope.setBounds(row.removeFromLeft(130));
        area.removeFromTop(8);
        layoutKnobs(area, 0, 4, area.getWidth() / 4);
    }
    void tick() override {
        const auto s = static_cast<FilterEffect&>(effect).settings();
        const double key = s.frequency + s.resonance * 1.0e5 + static_cast<int>(s.mode) * 1.0e7 + (s.steep ? 1.0e8 : 0.0);
        if (key != lastKey) { lastKey = key; curve.repaint(); }
    }
private:
    FilterCurve curve;
    juce::ComboBox mode, slope;
    std::unique_ptr<Attachments::ComboBoxAttachment> modeAttachment, slopeAttachment;
    double lastKey{-1.0};
};

// ---- Registration ----------------------------------------------------------------------

inline juce::AudioProcessorEditor* makeEditor(Effect& effect) {
    const juce::String id(effect.effectInfo().id);
    if (id == catalog[0].id) return new EqEditor(static_cast<EqEffect&>(effect));
    if (id == catalog[1].id) return new CompressorEditor(static_cast<CompressorEffect&>(effect));
    if (id == catalog[2].id) return new ReverbEditor(static_cast<ReverbEffect&>(effect));
    if (id == catalog[3].id) return new DelayEditor(static_cast<DelayEffect&>(effect));
    if (id == catalog[4].id) return new LimiterEditor(static_cast<LimiterEffect&>(effect));
    if (id == catalog[5].id) return new FilterEditor(static_cast<FilterEffect&>(effect));
    return nullptr;
}
// Including this header installs the editors; without it the host shows its generic one.
inline const bool editorsRegistered = (Effect::editorFactory = &makeEditor, true);

} // namespace alien::stock
