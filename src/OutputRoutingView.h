#pragma once
#include "ProjectController.h"
#include "StudioWidgets.h"
#include <functional>
#include <array>
#include <memory>
#include <vector>

namespace alien {
// A view of the document's output graph, never a second routing model.
class OutputRoutingView final : public juce::Component {
public:
    explicit OutputRoutingView(ProjectController& c) : controller(c) { setName("Track output routing"); }
    std::function<bool(int, int)> onOutputRequested;
    struct Request {
        std::uint32_t scope{};
        TrackId track;
        std::array<TrackId, maxBuses> buses{};
    };
    Request requestFor(int track) const {
        const auto& d = controller.document();
        Request request{d.identityScope, d.trackId(TrackRole::session, track)};
        for (int b = 0; b < maxBuses; ++b)
            if (d.buses[static_cast<std::size_t>(b)].used) request.buses[static_cast<std::size_t>(b)] = d.trackId(TrackRole::bus, b);
        return request;
    }
    bool choose(const Request& request, int bus) {
        const auto& d = controller.document();
        if (!isEnabled() || request.scope != d.identityScope || bus < -1 || bus >= maxBuses) return false;
        const int track = d.trackSlot(request.track, TrackRole::session);
        if (track < 0) return false;
        if (bus >= 0 && (!d.buses[static_cast<std::size_t>(bus)].used
            || !request.buses[static_cast<std::size_t>(bus)]
            || request.buses[static_cast<std::size_t>(bus)] != d.trackId(TrackRole::bus, bus))) return false;
        return onOutputRequested && onOutputRequested(track, bus);
    }
    void refresh() {
        tracks.clear();
        const auto& d = controller.document();
        for (int t = 0; t < d.trackCount(); ++t) {
            auto button = std::make_unique<juce::TextButton>(d.tracks[static_cast<std::size_t>(t)].name);
            button->setName("Route " + d.tracks[static_cast<std::size_t>(t)].name);
            button->setTooltip("Choose this track's output: Master or a group bus");
            button->onClick = [this, t] { showOutputs(t); };
            addAndMakeVisible(*button); tracks.push_back(std::move(button));
        }
        resized(); repaint();
    }
    int contentHeight() const { return 64 + juce::jmax(maxBuses, static_cast<int>(tracks.size())) * 52; }
    int outputFor(int track) const {
        const auto& d = controller.document();
        return track >= 0 && track < d.trackCount() ? d.tracks[static_cast<std::size_t>(track)].settings.output : -1;
    }
    juce::Rectangle<int> busBounds(int bus) const { return {getWidth() / 2 - 80, 44 + bus * 52, 160, 34}; }
    juce::Rectangle<int> masterBounds() const { return {getWidth() - 184, 44, 160, 34}; }
    void resized() override {
        for (std::size_t t = 0; t < tracks.size(); ++t) tracks[t]->setBounds(24, 44 + static_cast<int>(t) * 52, 160, 34);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::surface);
        g.setFont(ui::font(12)); g.setColour(ui::colours::textFaint);
        g.drawText("Tracks (click to route)", 24, 10, 180, 24, juce::Justification::centredLeft);
        g.drawText("Group buses", getWidth() / 2 - 80, 10, 160, 24, juce::Justification::centredLeft);
        const auto& d = controller.document();
        const auto connect = [&](juce::Rectangle<int> from, juce::Rectangle<int> to, bool bypassBuses = false) {
            const juce::Point<float> start{static_cast<float>(from.getRight()), static_cast<float>(from.getCentreY())};
            const juce::Point<float> end{static_cast<float>(to.getX()), static_cast<float>(to.getCentreY())};
            juce::Path path; path.startNewSubPath(start);
            if (bypassBuses) {
                // Direct outputs travel above the bus column, never through an unrelated node.
                const float corridor = static_cast<float>(getWidth() / 2 - 104);
                path.cubicTo(corridor, start.y, corridor, 36, corridor + 16, 36);
                path.lineTo(end.x - 12, 36);
                path.quadraticTo(end.x - 12, end.y, end.x, end.y);
            } else {
                const float bend = (end.x - start.x) * .5f;
                path.cubicTo(start.x + bend, start.y, end.x - bend, end.y, end.x, end.y);
            }
            g.setColour(ui::colours::accent.withAlpha(.7f)); g.strokePath(path, juce::PathStrokeType(2));
        };
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            const int output = outputFor(static_cast<int>(t));
            connect(tracks[t]->getBounds(), output < 0 ? masterBounds() : busBounds(output), output < 0);
        }
        const auto node = [&](juce::Rectangle<int> bounds, const juce::String& title) {
            g.setColour(ui::colours::raised); g.fillRoundedRectangle(bounds.toFloat(), 5);
            g.setColour(ui::colours::text); g.drawFittedText(title, bounds.reduced(6), juce::Justification::centred, 1);
        };
        for (int b = 0; b < maxBuses; ++b) if (d.buses[static_cast<std::size_t>(b)].used) {
            connect(busBounds(b), masterBounds()); node(busBounds(b), d.buses[static_cast<std::size_t>(b)].name);
        }
        node(masterBounds(), "Master");
    }
private:
    void showOutputs(int track) {
        const auto request = requestFor(track);
        juce::PopupMenu menu;
        menu.addItem(1, "Master", true, outputFor(track) == -1);
        const auto& d = controller.document();
        for (int b = 0; b < maxBuses; ++b) if (d.buses[static_cast<std::size_t>(b)].used)
            menu.addItem(b + 2, d.buses[static_cast<std::size_t>(b)].name, true, outputFor(track) == b);
        juce::Component::SafePointer<OutputRoutingView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tracks[static_cast<std::size_t>(track)].get()),
            [safe, request](int result) { if (safe && result > 0) safe->choose(request, result - 2); });
    }
    ProjectController& controller;
    std::vector<std::unique_ptr<juce::TextButton>> tracks;
};
} // namespace alien
