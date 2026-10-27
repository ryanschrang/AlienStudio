#pragma once
#include "FlLook.h"
#include "ProjectController.h"
#include "CommandEngine.h"
#include "StudioTheme.h"
#include "SynthEngine.h"
#include "TrackMixerPanel.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace alien {

// The track column of the Session view, laid out like a row of FL Studio's channel rack: a light (mute), pan and
// volume knobs, the track's name button, solo, and a stop button. One row per Session track, lined up with that
// track's row of clips. Knobs write the engine's lock-free controls and ask the owner to commit.
// Mute/solo use named commands, which own their undo step, and notify the owner only to refresh.
class SessionTrackStrip final : public juce::Component, public juce::TooltipClient {
public:
    // Where the parts sit, in pixels from the strip's left edge (centres, except nameLeft).
    static constexpr int width = 284, ledX = 17, panX = 46, volumeX = 78, nameLeft = 98, nameWidth = 118, soloX = 236, stopX = 264;

    SessionTrackStrip(ProjectController& projectController, ToneEngine& toneEngine)
        : controller(projectController), engine(toneEngine) {}

    // A mixer value changed from here (committed is false while a knob is being dragged and true when it is final).
    std::function<void(int, bool)> onMixEdited;
    std::function<void(int)> onMixCommandExecuted; // already committed; refresh only
    std::function<void(int)> onSelect;   // the track's header was clicked
    std::function<void(int)> onRename;   // double-click
    std::function<void(int)> onMenu;     // right-click
    std::function<void(int)> onStop;     // after the track was told to stop

    // Row geometry, shared with the clip grid beside it; the offset is the grid's vertical scroll position.
    void setRows(int rowHeight, int gapBetween) { rowH = rowHeight; gap = gapBetween; repaint(); }
    void setOffsetY(int y) { if (offsetY != y) { offsetY = y; repaint(); } }

    int trackAt(int y) const {
        const int stride = rowH + gap, content = y + offsetY;
        if (stride <= 0 || content < 0) return -1;
        const int track = content / stride;
        return content - track * stride < rowH && track < controller.document().trackCount() ? track : -1;
    }
    juce::Rectangle<int> rowBounds(int track) const { return {0, track * (rowH + gap) - offsetY, width, rowH}; }
    juce::Rectangle<int> ledRect(int track) const { return centred(16, 16, ledX, track); }
    juce::Rectangle<int> panRect(int track) const { return centred(26, 26, panX, track); }
    juce::Rectangle<int> volumeRect(int track) const { return centred(26, 26, volumeX, track); }
    juce::Rectangle<int> nameRect(int track) const { auto r = rowBounds(track); return {nameLeft, r.getY() + 3, nameWidth, r.getHeight() - 6}; }
    juce::Rectangle<int> soloRect(int track) const { return centred(24, 18, soloX, track); }
    juce::Rectangle<int> stopRect(int track) const { return centred(24, 18, stopX, track); }

    void refresh() { repaint(); }

    // What the small bubble over a knob says while it is being dragged ("Pan L 30", "Pan centre", "Volume -6.0 dB"); empty otherwise.
    juce::String knobReadout() const {
        if (drag != Drag::knob || knobTrack < 0 || knobTrack >= controller.document().trackCount()) return {};
        const auto mix = engine.tracks[static_cast<std::size_t>(knobTrack)].snapshot();
        const int percent = juce::roundToInt(mix.pan * 100.0f);
        return knobPan ? "Pan " + (percent == 0 ? juce::String("centre") : (percent < 0 ? "L " : "R ") + juce::String(std::abs(percent)))
                       : "Volume " + mixer_text::gainToText(static_cast<double>(mix.gain) * 100.0);
    }

    juce::String getTooltip() override {
        const int track = trackAt(hover.y);
        if (track < 0 || hover.x < 0) return {};
        const auto& d = controller.document();
        if (ledRect(track).expanded(4).contains(hover)) return "Mute (left click) or solo (right click) this track";
        if (soloRect(track).contains(hover)) return "Solo this track";
        if (stopRect(track).contains(hover)) return "Stop this track at the next launch boundary";
        if (panRect(track).contains(hover)) return "Track pan: drag up or down, double-click to centre";
        if (volumeRect(track).contains(hover)) return "Track volume: drag up or down, double-click to reset to 0 dB";
        const auto& info = d.tracks[static_cast<std::size_t>(track)];
        return info.name + " (" + presetText(info.settings.preset) + "). Click to select, double-click to rename, right-click for more.";
    }

    void paint(juce::Graphics& g) override {
        namespace col = ui::colours;
        const auto& d = controller.document();
        const bool soloing = anySolo();
        for (int track = 0; track < d.trackCount(); ++track) {
            const auto row = rowBounds(track);
            if (row.getBottom() < 0 || row.getY() > getHeight()) continue;
            const auto index = static_cast<std::size_t>(track);
            const auto mix = engine.tracks[index].snapshot();
            const bool selected = track == d.selectedTrack;
            const bool audible = !mix.mute && (!soloing || mix.solo);
            const auto colour = ui::trackColour(d.tracks[index].colour);

            g.setColour(selected ? col::hover : col::raised);
            g.fillRoundedRectangle(row.toFloat(), 5.0f);
            if (selected) { g.setColour(colour); g.fillRoundedRectangle(static_cast<float>(row.getX()), static_cast<float>(row.getY() + 3), 3.0f, static_cast<float>(row.getHeight() - 6), 1.5f); }

            ui::fl::paintLight(g, ledRect(track).toFloat().withSizeKeepingCentre(10.0f, 10.0f), colour, audible, mix.mute);
            ui::fl::paintKnob(g, panRect(track).toFloat(), (mix.pan + 1.0f) * 0.5f, std::abs(mix.pan) > 0.02f, colour, audible);
            ui::fl::paintKnob(g, volumeRect(track).toFloat(), mix.gain * 0.5f, std::abs(mix.gain - 1.0f) > 0.01f, colour, audible);

            const auto name = nameRect(track).toFloat();
            ui::fl::paintRaised(g, name, selected ? juce::Colour(0xff4d535d) : juce::Colour(0xff3f444c), 3.0f);
            ui::fl::paintGrip(g, name);
            g.setColour(audible ? col::text : col::textDim);
            g.setFont(ui::font(13.0f, true));
            g.drawText(d.tracks[index].name, name.toNearestInt().withTrimmedLeft(8).withTrimmedRight(20), juce::Justification::centred, true);

            const auto solo = soloRect(track).toFloat();
            ui::fl::paintRaised(g, solo, mix.solo ? col::solo : col::control, 3.0f, 0.2f, 0.3f);
            g.setColour(mix.solo ? juce::Colour(0xff06162a) : col::textDim);
            g.setFont(ui::font(10.5f, true));
            g.drawText("S", solo.toNearestInt(), juce::Justification::centred);

            const bool playing = engine.activePianoRollClip[index].load() >= 0;
            const auto stop = stopRect(track).toFloat();
            ui::fl::paintRaised(g, stop, playing ? juce::Colour(0xff1d3a31) : col::control, 3.0f, 0.2f, 0.3f);
            g.setColour(playing ? col::accent : col::textFaint);
            g.fillRect(stop.withSizeKeepingCentre(8.0f, 8.0f));
        }
        if (drag == Drag::knob) paintKnobReadout(g);
    }

    void mouseMove(const juce::MouseEvent& e) override { hover = e.getPosition(); }
    void mouseExit(const juce::MouseEvent&) override { hover = {-1, -1}; }

    void mouseDown(const juce::MouseEvent& e) override {
        const auto point = e.getPosition();
        drag = Drag::none;
        const int track = trackAt(point.y);
        if (track < 0) return;
        if (ledRect(track).expanded(4).contains(point)) { if (e.mods.isPopupMenu()) toggleSolo(track); else toggleMute(track); return; }
        if (soloRect(track).contains(point)) { toggleSolo(track); return; }
        if (stopRect(track).contains(point)) { controller.stopTrack(track); repaint(rowBounds(track)); if (onStop) onStop(track); return; }
        if (panRect(track).contains(point) || volumeRect(track).contains(point)) {
            const bool pan = panRect(track).contains(point);
            if (e.getNumberOfClicks() >= 2) { setKnob(track, pan, pan ? 0.0f : 1.0f, true, true); return; }
            if (e.mods.isPopupMenu()) { if (onMenu) onMenu(track); return; }
            beginKnob(track, pan, point.y);
            return;
        }
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(track); return; }
        if (onSelect) onSelect(track);
        if (e.getNumberOfClicks() >= 2 && onRename) onRename(track);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (drag == Drag::knob) dragKnob(e.getPosition().y, e.mods.isShiftDown());
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (drag != Drag::knob) return;
        drag = Drag::none;
        repaint(rowBounds(knobTrack).expanded(0, 30));
        if (onMixEdited) onMixEdited(knobTrack, true);
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const auto point = e.getPosition();
        const int track = trackAt(point.y);
        if (track >= 0 && (panRect(track).contains(point) || volumeRect(track).contains(point))) {
            const bool pan = panRect(track).contains(point);
            const auto mix = engine.tracks[static_cast<std::size_t>(track)].snapshot();
            setKnob(track, pan, (pan ? mix.pan : mix.gain) + wheel.deltaY * (pan ? 0.4f : 0.5f), true, true);
            return;
        }
        juce::Component::mouseWheelMove(e, wheel);   // the rows scroll with the clips
    }

private:
    enum class Drag { none, knob };

    juce::Rectangle<int> centred(int w, int h, int x, int track) const {
        return juce::Rectangle<int>(0, 0, w, h).withCentre({x, rowBounds(track).getCentreY()});
    }
    bool anySolo() const {
        for (int track = 0; track < controller.document().trackCount(); ++track)
            if (engine.tracks[static_cast<std::size_t>(track)].solo.load()) return true;
        return false;
    }
    static juce::String presetText(int preset) {
        const int index = juce::jlimit(0, static_cast<int>(presetNames.size()) - 1, preset);
        return juce::String(presetNames[static_cast<std::size_t>(index)]).upToFirstOccurrenceOf(" (", false, false);
    }

    void toggleMute(int track) {
        const auto result = command::CommandEngine(controller).execute(command::SetTrackMute{
            track, !engine.tracks[static_cast<std::size_t>(track)].mute.load()});
        if (!result.ok || !result.changed) return;
        repaint(rowBounds(track));
        if (onMixCommandExecuted) onMixCommandExecuted(track);
    }
    void toggleSolo(int track) {
        const auto result = command::CommandEngine(controller).execute(command::SetTrackSolo{
            track, !engine.tracks[static_cast<std::size_t>(track)].solo.load()});
        if (!result.ok || !result.changed) return;
        repaint();   // a solo changes which other rows are audible
        if (onMixCommandExecuted) onMixCommandExecuted(track);
    }

    void beginKnob(int track, bool pan, int y) {
        drag = Drag::knob; knobTrack = track; knobPan = pan; knobStartY = y;
        const auto mix = engine.tracks[static_cast<std::size_t>(track)].snapshot();
        knobStart = pan ? mix.pan : mix.gain;
    }
    void dragKnob(int y, bool fine) {
        setKnob(knobTrack, knobPan, knobStart + static_cast<float>(knobStartY - y) * (fine ? 0.003f : 0.012f), false, fine);
    }
    // Pan runs -1..1 and snaps to the centre; volume runs 0..2 (+6 dB) and snaps to unity. Both are stored in whole percent,
    // as the Mixer's own controls do.
    void setKnob(int track, bool pan, float value, bool committed, bool fine) {
        auto& controls = engine.tracks[static_cast<std::size_t>(track)];
        if (pan) {
            value = juce::jlimit(-1.0f, 1.0f, value);
            if (!fine && std::abs(value) < 0.04f) value = 0.0f;
            controls.pan.store(std::round(value * 100.0f) / 100.0f);
        } else {
            value = juce::jlimit(0.0f, 2.0f, value);
            if (!fine && std::abs(value - 1.0f) < 0.02f) value = 1.0f;
            controls.gain.store(std::round(value * 100.0f) / 100.0f);
        }
        repaint(rowBounds(track).expanded(0, 30));
        if (onMixEdited) onMixEdited(track, committed);
    }

    void paintKnobReadout(juce::Graphics& g) const {
        const auto text = knobReadout();
        if (text.isEmpty()) return;
        const auto anchor = knobPan ? panRect(knobTrack) : volumeRect(knobTrack);
        const auto box = juce::Rectangle<int>(110, 20).withCentre({anchor.getCentreX(), anchor.getY() - 12}).constrainedWithin(getLocalBounds());
        g.setColour(ui::colours::inset.withAlpha(0.95f));
        g.fillRoundedRectangle(box.toFloat(), 4.0f);
        g.setColour(ui::colours::text);
        g.setFont(ui::font(12.0f, true));
        g.drawText(text, box, juce::Justification::centred);
    }

    ProjectController& controller;
    ToneEngine& engine;
    int rowH{36}, gap{6}, offsetY{};
    Drag drag{Drag::none};
    int knobTrack{-1}, knobStartY{};
    bool knobPan{};
    float knobStart{};
    juce::Point<int> hover{-1, -1};
};

} // namespace alien
