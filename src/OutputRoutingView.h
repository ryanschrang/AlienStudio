#pragma once
#include "ProjectController.h"
#include "StudioWidgets.h"
#include <functional>
#include <array>
#include <memory>
#include <vector>
#include <cmath>
#include <charconv>
#include <optional>

namespace alien {
// A view of the document's output graph, never a second routing model.
class OutputRoutingView final : public juce::Component {
public:
    explicit OutputRoutingView(ProjectController& c) : controller(c) { setName("Track output routing"); }
    std::function<bool(int, int)> onOutputRequested;
    std::function<bool(int, int, double)> onSendRequested;
    std::function<bool(int, int, bool)> onSendModeRequested;
    bool showsSends() const noexcept { return sendView; }
    void showSends(bool shown) { sendView = shown; refresh(); }
    struct Request {
        std::uint32_t scope{};
        TrackId track;
        std::array<TrackId, maxBuses> buses{};
        std::array<TrackId, maxReturns> returns{};
    };
    Request requestFor(int track) const {
        const auto& d = controller.document();
        Request request{d.identityScope, d.trackId(TrackRole::session, track)};
        for (int b = 0; b < maxBuses; ++b)
            if (d.buses[static_cast<std::size_t>(b)].used) request.buses[static_cast<std::size_t>(b)] = d.trackId(TrackRole::bus, b);
        for (int r = 0; r < maxReturns; ++r)
            if (d.returns[static_cast<std::size_t>(r)].used) request.returns[static_cast<std::size_t>(r)] = d.trackId(TrackRole::returnTrack, r);
        return request;
    }
    bool choose(const Request& request, int bus) {
        const auto& d = controller.document();
        if (sendView || !isEnabled() || request.scope != d.identityScope || bus < -1 || bus >= maxBuses) return false;
        const int track = d.trackSlot(request.track, TrackRole::session);
        if (track < 0) return false;
        if (bus >= 0 && (!d.buses[static_cast<std::size_t>(bus)].used
            || !request.buses[static_cast<std::size_t>(bus)]
            || request.buses[static_cast<std::size_t>(bus)] != d.trackId(TrackRole::bus, bus))) return false;
        return onOutputRequested && onOutputRequested(track, bus);
    }
    bool chooseSend(const Request& request, int r, double level) {
        const int track = sendTrack(request, r);
        return track >= 0 && onSendRequested && onSendRequested(track, r, level);
    }
    bool chooseSendMode(const Request& request, int r, bool pre) {
        const int track = sendTrack(request, r);
        return track >= 0 && onSendModeRequested && onSendModeRequested(track, r, pre);
    }
    bool editSendLevel(const Request& request, int r) {
        const int track = sendTrack(request, r);
        if (track < 0 || controller.isBusy()) return false;
        const auto initial = juce::String(sendFor(track, r) * 100, 9).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        const auto& d = controller.document();
        auto* window = new juce::AlertWindow("Set send level",
            d.tracks[static_cast<std::size_t>(track)].name + " to " + d.returns[static_cast<std::size_t>(r)].name
                + ": enter a percentage from 0 to 100.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("percent", initial, "Level (%)");
        window->getTextEditor("percent")->setTitle("Send level percent");
        window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<juce::AlertWindow> dialog(window);
        window->getTextEditor("percent")->onTextChange = [dialog] {
            if (dialog) dialog->getButton(0)->setEnabled(sendPercent(dialog->getTextEditorContents("percent")).has_value());
        };
        juce::Component::SafePointer<OutputRoutingView> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create(
            [safe, dialog, request, r, initial](int result) {
                if (!safe || !dialog || result != 1) return;
                const auto text = dialog->getTextEditorContents("percent").trim();
                if (text == initial) return;
                if (const auto percent = sendPercent(text)) safe->chooseSend(request, r, *percent / 100.0);
            }), true);
        return true;
    }
    double sendFor(int track, int r) const {
        const auto& d = controller.document();
        return track >= 0 && track < d.trackCount() && r >= 0 && r < maxReturns && d.returns[static_cast<std::size_t>(r)].used
            ? d.tracks[static_cast<std::size_t>(track)].settings.send[static_cast<std::size_t>(r)] : 0.0;
    }
    void refresh() {
        tracks.clear();
        const auto& d = controller.document();
        for (int t = 0; t < d.trackCount(); ++t) {
            auto button = std::make_unique<juce::TextButton>(d.tracks[static_cast<std::size_t>(t)].name);
            button->setName("Route " + d.tracks[static_cast<std::size_t>(t)].name);
            button->setTooltip(sendView ? "Choose a return send level and pre/post-fader mode" : "Choose this track's output: Master or a group bus");
            button->onClick = [this, t] { showOutputs(t); };
            addAndMakeVisible(*button); tracks.push_back(std::move(button));
        }
        resized(); repaint();
    }
    int contentHeight() const { return 64 + juce::jmax(juce::jmax(maxBuses, maxReturns), static_cast<int>(tracks.size())) * 52; }
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
        g.drawText(sendView ? "Tracks (click for sends)" : "Tracks (click to route)", 24, 10, 180, 24, juce::Justification::centredLeft);
        g.drawText(sendView ? "Returns" : "Group buses", getWidth() / 2 - 80, 10, 160, 24, juce::Justification::centredLeft);
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
            if (sendView) {
                for (int r = 0; r < maxReturns; ++r) if (sendFor(static_cast<int>(t), r) > 0)
                    connect(tracks[t]->getBounds(), busBounds(r));
                continue;
            }
            const int output = outputFor(static_cast<int>(t));
            connect(tracks[t]->getBounds(), output < 0 ? masterBounds() : busBounds(output), output < 0);
        }
        const auto node = [&](juce::Rectangle<int> bounds, const juce::String& title) {
            g.setColour(ui::colours::raised); g.fillRoundedRectangle(bounds.toFloat(), 5);
            g.setColour(ui::colours::text); g.drawFittedText(title, bounds.reduced(6), juce::Justification::centred, 1);
        };
        if (sendView) for (int r = 0; r < maxReturns; ++r) if (d.returns[static_cast<std::size_t>(r)].used) {
            connect(busBounds(r), masterBounds());
            node(busBounds(r), juce::String::charToString(static_cast<juce::juce_wchar>('A' + r)) + ": " + d.returns[static_cast<std::size_t>(r)].name);
        }
        if (!sendView) for (int b = 0; b < maxBuses; ++b) if (d.buses[static_cast<std::size_t>(b)].used) {
            connect(busBounds(b), masterBounds()); node(busBounds(b), d.buses[static_cast<std::size_t>(b)].name);
        }
        node(masterBounds(), "Master");
    }
private:
    static std::optional<double> sendPercent(const juce::String& text) {
        const auto value = text.trim().toStdString();
        double percent{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), percent);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
            || !std::isfinite(percent) || percent < 0 || percent > 100) return std::nullopt;
        return percent;
    }
    int sendTrack(const Request& request, int r) const {
        const auto& d = controller.document();
        if (!sendView || !isVisible() || !isEnabled() || request.scope != d.identityScope || r < 0 || r >= maxReturns
            || !d.returns[static_cast<std::size_t>(r)].used || !request.returns[static_cast<std::size_t>(r)]
            || request.returns[static_cast<std::size_t>(r)] != d.trackId(TrackRole::returnTrack, r)) return -1;
        return d.trackSlot(request.track, TrackRole::session);
    }
    void showOutputs(int track) {
        const auto request = requestFor(track);
        juce::PopupMenu menu;
        const auto& d = controller.document();
        if (sendView) {
            for (int r = 0; r < maxReturns; ++r) if (d.returns[static_cast<std::size_t>(r)].used) {
                const auto& settings = d.tracks[static_cast<std::size_t>(track)].settings;
                juce::PopupMenu send;
                const std::array<double, 4> levels{0, .25, .5, 1};
                for (int i = 0; i < 4; ++i) send.addItem(1000 + r * 10 + i,
                    i == 0 ? juce::String("Off") : juce::String(levels[static_cast<std::size_t>(i)] * 100, 0) + "%",
                    true, std::abs(sendFor(track, r) - levels[static_cast<std::size_t>(i)]) < .00001);
                send.addItem(1000 + r * 10 + 6, "Set level...");
                send.addSeparator();
                send.addItem(1000 + r * 10 + 4, "Pre-fader", true, settings.sendPre[static_cast<std::size_t>(r)]);
                send.addItem(1000 + r * 10 + 5, "Post-fader", true, !settings.sendPre[static_cast<std::size_t>(r)]);
                menu.addSubMenu(juce::String::charToString(static_cast<juce::juce_wchar>('A' + r)) + ": "
                    + d.returns[static_cast<std::size_t>(r)].name + " - " + juce::String(sendFor(track, r) * 100, 1)
                    + "% " + (settings.sendPre[static_cast<std::size_t>(r)] ? "pre" : "post"), send);
            }
            if (menu.getNumItems() == 0) menu.addItem(999, "Add a return to create a send", false);
        } else {
            menu.addItem(1, "Master", true, outputFor(track) == -1);
            for (int b = 0; b < maxBuses; ++b) if (d.buses[static_cast<std::size_t>(b)].used)
                menu.addItem(b + 2, d.buses[static_cast<std::size_t>(b)].name, true, outputFor(track) == b);
        }
        juce::Component::SafePointer<OutputRoutingView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tracks[static_cast<std::size_t>(track)].get()),
            [safe, request](int result) {
                if (!safe || result <= 0) return;
                if (result >= 1000) {
                    const int r = (result - 1000) / 10, action = (result - 1000) % 10;
                    if (action < 4) safe->chooseSend(request, r, std::array<double, 4>{0,.25,.5,1}[static_cast<std::size_t>(action)]);
                    else if (action < 6) safe->chooseSendMode(request, r, action == 4);
                    else if (action == 6) safe->editSendLevel(request, r);
                } else if (result != 999) safe->choose(request, result - 2);
            });
    }
    ProjectController& controller;
    std::vector<std::unique_ptr<juce::TextButton>> tracks;
    bool sendView{};
};
} // namespace alien
