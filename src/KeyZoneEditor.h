#pragma once
// The key zone editor (T2-01): a small panel listing a Channel Rack channel's key zones (its slices on keys or a
// multisample's files), each with pickers for its low key, high key and root, and a button that takes it away.
// Every change is one named command (SetRackChannelKeyZone, RemoveRackChannelKeyZone), so each is one Undo step and
// a refused edit says why in the panel and changes nothing. The channel's frame ranges are not edited here.
#include "CommandEngine.h"
#include "ProjectController.h"
#include "StudioWidgets.h"
#include <array>
#include <functional>
#include <memory>

namespace alien {

class KeyZoneEditor final : public juce::Component, private juce::Timer {
public:
    static constexpr int rowHeight = 28, headerHeight = 24, messageHeight = 40, panelWidth = 560;

    KeyZoneEditor(ProjectController& projectController, int row)
        : controller(projectController), track(projectController.document().trackId(TrackRole::rack, row)),
          scope(projectController.document().identityScope) {
        setName("Key zone editor");
        heading.setFont(ui::font(12.0f, true));
        heading.setColour(juce::Label::textColourId, ui::colours::text);
        addAndMakeVisible(heading);
        status.setFont(ui::font(11.0f));
        status.setColour(juce::Label::textColourId, ui::colours::textDim);
        status.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(status);
        for (int z = 0; z < rack::maxKeyZones; ++z) {
            auto& r = rows[static_cast<std::size_t>(z)];
            r.name.setFont(ui::font(11.0f));
            r.name.setColour(juce::Label::textColourId, ui::colours::text);
            addChildComponent(r.name);
            for (auto* box : {&r.low, &r.high, &r.root}) {
                for (int note = 0; note <= 127; ++note) box->addItem(noteName(note), note + 1);
                box->onChange = [this, z] { pickerChanged(z); };
                addChildComponent(*box);
            }
            r.low.setTooltip("The zone's lowest key");
            r.high.setTooltip("The zone's highest key");
            r.root.setTooltip("The key the zone plays at its recorded pitch");
            r.remove.setButtonText("Remove");
            r.remove.setTooltip("Take this zone off the keys (one Undo step)");
            r.remove.onClick = [this, z] { removeZone(z); };
            addChildComponent(r.remove);
        }
        refresh();
        startTimerHz(5);   // follows Undo, Redo and edits made elsewhere
    }

    std::function<void()> onChanged;   // a zone was changed or removed

    static juce::String noteName(int note) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); }

    // The channel the panel edits, or -1 once it is gone.
    int channel() const {
        const auto& d = controller.document();
        return d.identityScope == scope ? d.trackSlot(track, TrackRole::rack) : -1;
    }
    int zoneCount() const { return shownCount; }
    juce::ComboBox& lowBox(int zone) { return rows[static_cast<std::size_t>(zone)].low; }
    juce::ComboBox& highBox(int zone) { return rows[static_cast<std::size_t>(zone)].high; }
    juce::ComboBox& rootBox(int zone) { return rows[static_cast<std::size_t>(zone)].root; }
    juce::TextButton& removeButton(int zone) { return rows[static_cast<std::size_t>(zone)].remove; }
    juce::String zoneName(int zone) const { return rows[static_cast<std::size_t>(zone)].name.getText(); }
    juce::String message() const { return status.getText(); }

    // Gives `zone` new keys and a root, as one Undo step. False, with the reason shown, when refused.
    bool setZone(int zone, int low, int high, int root) {
        return run(command::SetRackChannelKeyZone{channel(), zone, low, high, root});
    }
    bool removeZone(int zone) { return run(command::RemoveRackChannelKeyZone{channel(), zone}); }

    // Shows the channel's zones as they are now.
    void refresh() {
        const int row = channel();
        const auto& rack = controller.document().channelRack;
        shownZones = rack.validChannel(row) ? rack.info[static_cast<std::size_t>(row)].zones : nullptr;
        shownCount = shownZones != nullptr ? static_cast<int>(shownZones->size()) : 0;
        if (!rack.validChannel(row)) heading.setText("The channel is gone.", juce::dontSendNotification);
        else heading.setText("Key zones of " + juce::String::fromUTF8(rack.info[static_cast<std::size_t>(row)].name.c_str())
                                 + (shownCount == 0 ? juce::String(": none (the sample plays on every key)") : juce::String()),
                             juce::dontSendNotification);
        for (int z = 0; z < rack::maxKeyZones; ++z) {
            auto& r = rows[static_cast<std::size_t>(z)];
            const bool shown = z < shownCount;
            for (juce::Component* part : std::initializer_list<juce::Component*>{&r.name, &r.low, &r.high, &r.root, &r.remove}) part->setVisible(shown);
            if (!shown) continue;
            const auto& zone = (*shownZones)[static_cast<std::size_t>(z)];
            r.name.setText(juce::String(z + 1) + ". " + describe(rack.info[static_cast<std::size_t>(row)], zone), juce::dontSendNotification);
            r.low.setSelectedId(zone.low + 1, juce::dontSendNotification);
            r.high.setSelectedId(zone.high + 1, juce::dontSendNotification);
            r.root.setSelectedId(zone.root + 1, juce::dontSendNotification);
        }
        setSize(panelWidth, headerHeight * 2 + std::max(shownCount, 1) * rowHeight + messageHeight);
        resized();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(ui::colours::raised);
        if (shownCount == 0) return;
        g.setColour(ui::colours::textDim);
        g.setFont(ui::font(9.5f, true));
        const int y = headerHeight;
        g.drawText("ZONE", 8, y, nameWidth, headerHeight, juce::Justification::centredLeft);
        g.drawText("LOW", pickerX(0), y, pickerWidth, headerHeight, juce::Justification::centred);
        g.drawText("HIGH", pickerX(1), y, pickerWidth, headerHeight, juce::Justification::centred);
        g.drawText("ROOT", pickerX(2), y, pickerWidth, headerHeight, juce::Justification::centred);
    }
    void resized() override {
        heading.setBounds(8, 0, getWidth() - 16, headerHeight);
        for (int z = 0; z < rack::maxKeyZones; ++z) {
            auto& r = rows[static_cast<std::size_t>(z)];
            const int y = headerHeight * 2 + z * rowHeight;
            r.name.setBounds(8, y, nameWidth, rowHeight - 4);
            r.low.setBounds(pickerX(0), y, pickerWidth, rowHeight - 4);
            r.high.setBounds(pickerX(1), y, pickerWidth, rowHeight - 4);
            r.root.setBounds(pickerX(2), y, pickerWidth, rowHeight - 4);
            r.remove.setBounds(pickerX(3), y, removeWidth, rowHeight - 4);
        }
        status.setBounds(8, getHeight() - messageHeight, getWidth() - 16, messageHeight - 4);
    }

private:
    static constexpr int nameWidth = 200, pickerWidth = 72, removeWidth = 70, gap = 6;
    static int pickerX(int column) { return 8 + nameWidth + gap + column * (pickerWidth + gap); }

    struct Row {
        juce::Label name;
        juce::ComboBox low, high, root;
        juce::TextButton remove;
    };

    // What a zone plays: its own file, the channel's file, or a slice of it.
    static juce::String describe(const rack::ChannelInfo& info, const rack::KeyZone& zone) {
        const auto file = [](const std::string& path) { return juce::File::createFileWithoutCheckingPath(juce::String::fromUTF8(path.c_str())).getFileName(); };
        if (!zone.file.empty()) return file(zone.file);
        if (zone.wholeFile()) return file(info.sample);
        return "Slice, frames " + juce::String(zone.start) + " to " + juce::String(zone.end);
    }

    void pickerChanged(int zone) {
        auto& r = rows[static_cast<std::size_t>(zone)];
        setZone(zone, r.low.getSelectedId() - 1, r.high.getSelectedId() - 1, r.root.getSelectedId() - 1);
    }
    bool run(const command::Command& edit) {
        const auto result = command::CommandEngine(controller).execute(edit);
        status.setText(result.ok ? juce::String() : result.error, juce::dontSendNotification);
        refresh();   // a refused edit puts the pickers back
        if (result.changed && onChanged) onChanged();
        return result.ok && result.changed;
    }
    void timerCallback() override {
        const int row = channel();
        const auto& rack = controller.document().channelRack;
        const auto zones = rack.validChannel(row) ? rack.info[static_cast<std::size_t>(row)].zones : nullptr;
        if (zones != shownZones || (row < 0) != heading.getText().startsWith("The channel is gone")) refresh();
    }

    ProjectController& controller;
    TrackId track;
    std::uint32_t scope{};
    juce::Label heading, status;
    std::array<Row, rack::maxKeyZones> rows;
    std::shared_ptr<const std::vector<rack::KeyZone>> shownZones;
    int shownCount{};
};

} // namespace alien
