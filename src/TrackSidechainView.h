#pragma once
#include "ProjectController.h"
#include "StudioWidgets.h"
#include <functional>
#include <memory>
#include <vector>
#include <utility>

namespace alien {
// Track and master FX share the existing sidechain command. This increment
// offers pre-FX track keys; other key stages remain editable in Plugins.
class TrackSidechainView final : public juce::Component {
public:
    explicit TrackSidechainView(ProjectController& c) : controller(c) { setName("Track and master FX sidechains"); }
    std::function<bool(RackStage,int,int)> hasInput;
    std::function<bool(RackStage,int,int,int)> onKeyRequested;
    struct Request { std::uint32_t scope{}; TrackId destination; std::uint64_t instance{}; std::vector<TrackId> sources; RackStage stage{RackStage::track}; };
    Request requestFor(int track, int fx, RackStage stage = RackStage::track) const {
        const auto& d = controller.document();
        Request request{d.identityScope,d.trackId(TrackRole::session,track)};
        request.stage = stage;
        if (stage == RackStage::track || stage == RackStage::master)
            if (const auto* rack = sidechain_edit::rackOf(d,stage,track); rack && fx >= 0 && fx < static_cast<int>(rack->effects.size()))
                request.instance = rack->effects[static_cast<std::size_t>(fx)].instanceId;
        for (int t = 0; t < d.trackCount(); ++t) request.sources.push_back(d.trackId(TrackRole::session,t));
        return request;
    }
    bool choose(const Request& request, int sourceIndex) {
        const auto [track,fx] = resolve(request);
        if (!isEnabled() || track < 0 || !hasInput || !hasInput(request.stage,track,fx) || sourceIndex < -1) return false;
        int source = -1;
        if (sourceIndex >= 0) {
            if (sourceIndex >= static_cast<int>(request.sources.size())) return false;
            source = controller.document().trackSlot(request.sources[static_cast<std::size_t>(sourceIndex)],TrackRole::session);
            if (source < 0 || (request.stage == RackStage::track && source == track)) return false;
        }
        return onKeyRequested && onKeyRequested(request.stage,track,fx,source);
    }
    void refresh() {
        const auto& d = controller.document();
        std::vector<juce::String> names;
        std::vector<TrackId> ids;
        std::vector<Effect> next;
        for (int t = 0; t < d.trackCount(); ++t) {
            const auto& track = d.tracks[static_cast<std::size_t>(t)];
            names.push_back(track.name); ids.push_back(d.trackId(TrackRole::session,t));
            for (int fx = 0; fx < static_cast<int>(track.plugins.effects.size()); ++fx) {
                const auto& effect = track.plugins.effects[static_cast<std::size_t>(fx)];
                if (effect.descriptionXml.isNotEmpty()) next.push_back({t,fx,effect.instanceId,effect.sidechain,
                    hasInput && hasInput(RackStage::track,t,fx),effect.descriptionXml,RackStage::track,effect.sidechainFrom});
            }
        }
        for (int fx = 0; fx < static_cast<int>(d.masterPlugins.effects.size()); ++fx) {
            const auto& effect = d.masterPlugins.effects[static_cast<std::size_t>(fx)];
            if (effect.descriptionXml.isNotEmpty()) next.push_back({0,fx,effect.instanceId,effect.sidechain,
                hasInput && hasInput(RackStage::master,0,fx),effect.descriptionXml,RackStage::master,effect.sidechainFrom});
        }
        if (scope == d.identityScope && names == trackNames && ids == trackIds && next == effects) return;
        scope = d.identityScope; trackNames = std::move(names); trackIds = std::move(ids); effects = std::move(next);
        buttons.clear();
        for (const auto& effect : effects) {
            juce::PluginDescription description;
            if (const auto xml = juce::parseXML(effect.description)) description.loadFromXml(*xml);
            auto label = (effect.stage == RackStage::master ? juce::String("Master") : trackNames[static_cast<std::size_t>(effect.track)]) + " / FX " + juce::String(effect.fx + 1);
            if (description.name.isNotEmpty()) label += ": " + description.name;
            if (effect.source >= 0 && effect.from != KeySource::track) label += " (key set in Plugins)";
            auto button = std::make_unique<juce::TextButton>(label);
            button->setName(label); button->setEnabled(effect.capable);
            button->setTooltip(effect.capable ? "Choose a pre-FX track key or Off. Other key stages are available in Plugins." : "This loaded effect has no sidechain input (or is unavailable)");
            button->onClick = [this, request=requestFor(effect.track,effect.fx,effect.stage), anchor=button.get()] { showKeys(request,anchor); };
            addAndMakeVisible(*button); buttons.push_back(std::move(button));
        }
        resized(); repaint();
    }
    int contentHeight() const { return 64 + juce::jmax(4,juce::jmax(static_cast<int>(trackNames.size()),static_cast<int>(effects.size()))) * 52; }
    void resized() override {
        for (std::size_t i = 0; i < buttons.size(); ++i) buttons[i]->setBounds(getWidth()-300,44+static_cast<int>(i)*52,276,34);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::surface); g.setFont(ui::font(12)); g.setColour(ui::colours::textFaint);
        g.drawText("Key tracks (before FX)",24,10,200,24,juce::Justification::centredLeft);
        g.drawText("Track / master FX (click to choose key)",getWidth()-300,10,276,24,juce::Justification::centredLeft);
        if (effects.empty()) g.drawFittedText("Add a track or master effect in Plugins to choose its sidechain here.",{getWidth()-300,44,276,70},juce::Justification::centred,3);
        for (std::size_t i = 0; i < trackNames.size(); ++i) {
            const juce::Rectangle<int> bounds{24,44+static_cast<int>(i)*52,160,34};
            g.setColour(ui::colours::raised); g.fillRoundedRectangle(bounds.toFloat(),5);
            g.setColour(ui::colours::text); g.drawFittedText(trackNames[i],bounds.reduced(6),juce::Justification::centred,1);
        }
        for (std::size_t i = 0; i < effects.size(); ++i) {
            const auto& effect = effects[i];
            if (!effect.capable || effect.from != KeySource::track || effect.source < 0 || effect.source >= static_cast<int>(trackNames.size())) continue;
            const float y = 61.0f + effect.source * 52.0f, endY = 61.0f + static_cast<float>(i)*52;
            const float endX = static_cast<float>(getWidth()-300), middle = (184+endX)*.5f;
            juce::Path path; path.startNewSubPath(184,y); path.cubicTo(middle,y,middle,endY,endX,endY);
            g.setColour(ui::colours::accent.withAlpha(.7f)); g.strokePath(path,juce::PathStrokeType(2));
        }
    }
private:
    struct Effect { int track{},fx{}; std::uint64_t instance{}; int source{-1}; bool capable{}; juce::String description; RackStage stage{RackStage::track}; KeySource from{KeySource::track}; bool operator==(const Effect&) const = default; };
    std::pair<int,int> resolve(const Request& request) const {
        const auto& d = controller.document();
        if (request.scope != d.identityScope || request.instance == 0) return {-1,-1};
        if (request.stage != RackStage::track && request.stage != RackStage::master) return {-1,-1};
        const int track = request.stage == RackStage::master ? 0 : d.trackSlot(request.destination,TrackRole::session);
        if (track < 0) return {-1,-1};
        const auto& fx = sidechain_edit::rackOf(d,request.stage,track)->effects;
        for (std::size_t i = 0; i < fx.size(); ++i) if (fx[i].instanceId == request.instance) return {track,static_cast<int>(i)};
        return {-1,-1};
    }
    void showKeys(const Request& request, juce::Component* anchor) {
        const auto [track,fx] = resolve(request); if (track < 0) return;
        const auto& d = controller.document();
        juce::PopupMenu menu; const auto& effect = sidechain_edit::rackOf(d,request.stage,track)->effects[static_cast<std::size_t>(fx)];
        const int current = effect.sidechain;
        menu.addItem(1,"Off",true,current < 0);
        for (std::size_t i = 0; i < request.sources.size(); ++i) {
            const int source = d.trackSlot(request.sources[i],TrackRole::session);
            if (source >= 0 && (request.stage != RackStage::track || source != track)) menu.addItem(static_cast<int>(i)+2,d.tracks[static_cast<std::size_t>(source)].name,true,effect.sidechainFrom == KeySource::track && current == source);
        }
        juce::Component::SafePointer<TrackSidechainView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),[safe,request](int result) { if (safe && result > 0) safe->choose(request,result-2); });
    }
    ProjectController& controller;
    std::uint32_t scope{};
    std::vector<juce::String> trackNames;
    std::vector<TrackId> trackIds;
    std::vector<Effect> effects;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};
} // namespace alien
