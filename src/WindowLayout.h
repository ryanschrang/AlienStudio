#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <optional>
#include <vector>
#include <cmath>

// Local UI preferences, in JUCE logical desktop coordinates. Never part of a
// song, undo state, audio callback, plugin state or device configuration.
namespace alien::layout {
inline constexpr int viewCount = 9;
inline constexpr std::array<const char*, viewCount> viewKeys{"steps", "pianoRoll", "session", "mixer", "drums", "song", "audioLab", "plugins", "rack"};
struct WindowState {
    juce::Rectangle<int> bounds{80, 80, 1280, 800};
    juce::Rectangle<int> workArea{0, 0, 1920, 1080};
    bool maximized{};
    bool operator==(const WindowState&) const = default;
};
struct PanelState {
    bool detached{};
    std::optional<WindowState> window;
    bool operator==(const PanelState&) const = default;
};
struct State {
    WindowState mainWindow;
    std::array<PanelState, viewCount> panels{};
    int selectedView{};
    bool devices{}, keyboard{};
    bool operator==(const State&) const = default;
};
struct Limits { int minWidth, minHeight, maxWidth, maxHeight; };
inline constexpr Limits mainLimits{960, 650, 1920, 1440}, panelLimits{960, 360, 3840, 2160};
inline bool validRect(juce::Rectangle<int> r) {
    return r.getWidth() > 0 && r.getHeight() > 0 && r.getWidth() <= 100000 && r.getHeight() <= 100000
        && std::abs(static_cast<double>(r.getX())) <= 1000000 && std::abs(static_cast<double>(r.getY())) <= 1000000;
}
inline bool valid(const WindowState& w) { return validRect(w.bounds) && validRect(w.workArea); }
inline bool valid(const State& s) {
    if (!valid(s.mainWindow) || s.selectedView < 0 || s.selectedView >= viewCount) return false;
    for (const auto& p : s.panels) if ((p.detached && !p.window) || (p.window && !valid(*p.window))) return false;
    return true;
}
inline std::vector<juce::Rectangle<int>> displays() {
    std::vector<juce::Rectangle<int>> areas;
    const auto& screens = juce::Desktop::getInstance().getDisplays();
    if (const auto* primary = screens.getPrimaryDisplay()) areas.push_back(primary->userArea);
    for (const auto& screen : screens.displays) if (!screen.isMain) areas.push_back(screen.userArea);
    if (areas.empty()) areas.push_back({0, 0, 1280, 800});
    return areas;
}
inline juce::Rectangle<int> areaFor(juce::Rectangle<int> bounds) {
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds)) return display->userArea;
    return displays().front();
}
// Select a surviving screen by overlap with the saved work area, otherwise the
// primary screen. Reposition proportionally when its logical work area changes.
inline juce::Rectangle<int> survivingArea(juce::Rectangle<int> previous, const std::vector<juce::Rectangle<int>>& screens) {
    auto area = screens.empty() ? juce::Rectangle<int>{0, 0, 1280, 800} : screens.front();
    std::int64_t best = 0;
    for (const auto screen : screens) {
        if (!validRect(screen)) continue;
        const auto overlap = screen.getIntersection(previous);
        const auto score = static_cast<std::int64_t>(overlap.getWidth()) * overlap.getHeight();
        if (score > best) { best = score; area = screen; }
    }
    if (!validRect(area)) area = {0, 0, 1280, 800};
    return area;
}
inline juce::Rectangle<int> fit(const WindowState& saved, const std::vector<juce::Rectangle<int>>& screens, Limits limits) {
    const auto area = survivingArea(saved.workArea, screens);
    const auto clampSize = [](int size, int available, int minimum, int maximum) {
        return juce::jlimit(juce::jmin(minimum, available), juce::jmin(maximum, available), size);
    };
    const int width = clampSize(saved.bounds.getWidth(), area.getWidth(), limits.minWidth, limits.maxWidth);
    const int height = clampSize(saved.bounds.getHeight(), area.getHeight(), limits.minHeight, limits.maxHeight);
    auto result = saved.bounds.withSize(width, height);
    if (area != saved.workArea) {
        const auto relative = [](int position, int origin, int available, int size) {
            return juce::jlimit(0.0, 1.0, static_cast<double>(position - origin) / juce::jmax(1, available - size));
        };
        const double x = relative(saved.bounds.getX(), saved.workArea.getX(), saved.workArea.getWidth(), saved.bounds.getWidth());
        const double y = relative(saved.bounds.getY(), saved.workArea.getY(), saved.workArea.getHeight(), saved.bounds.getHeight());
        result.setPosition(area.getX() + juce::roundToInt(x * (area.getWidth() - width)), area.getY() + juce::roundToInt(y * (area.getHeight() - height)));
    }
    return result.withPosition(juce::jlimit(area.getX(), area.getRight() - width, result.getX()),
                               juce::jlimit(area.getY(), area.getBottom() - height, result.getY()));
}
inline juce::Rectangle<int> fittedBounds(juce::DocumentWindow& window, const WindowState& state, Limits limits,
                                         const std::vector<juce::Rectangle<int>>& screens) {
    auto bounds = fit(state, screens, limits);
    // Component bounds exclude native title bars/borders. Keep the entire
    // native frame reachable too, using logical sizes reported by the peer.
    if (const auto* peer = window.getPeer()) {
        if (const auto frame = peer->getFrameSizeIfPresent()) {
            const auto clientArea = frame->subtractedFrom(survivingArea(bounds, screens));
            if (validRect(clientArea)) bounds = fit({bounds, clientArea, false}, {clientArea}, limits);
        }
    }
    return bounds;
}
inline void apply(juce::DocumentWindow& window, const WindowState& state, Limits limits,
                  const std::vector<juce::Rectangle<int>>& screens) {
    window.setFullScreen(false); window.setMinimised(false);
    const auto bounds = fittedBounds(window, state, limits, screens);
    window.setResizeLimits(juce::jmin(limits.minWidth, bounds.getWidth()), juce::jmin(limits.minHeight, bounds.getHeight()), limits.maxWidth, limits.maxHeight);
    window.setBounds(bounds);
    // JUCE records its native unmaximize bounds only while a window is showing.
    // Hidden startup windows apply maximization immediately after being shown.
    if (window.isShowing()) window.setFullScreen(state.maximized);
}
inline void apply(juce::DocumentWindow& window, const WindowState& state, Limits limits) {
    apply(window, state, limits, displays());
}
// Track normal geometry while maximized/minimized, so restart never treats a
// maximized screen rectangle as the user's normal window size.
class Tracker final : private juce::ComponentListener, private juce::Timer {
public:
    explicit Tracker(juce::DocumentWindow& w, Limits sizeLimits = mainLimits)
        : window(w), limits(sizeLimits), knownDisplays(displays()) {
        window.addComponentListener(this); startTimerHz(2);
    }
    ~Tracker() override { stopTimer(); window.removeComponentListener(this); }
    // Message thread. Defer hidden/minimized windows without acknowledging the
    // topology change, so restoring them recovers geometry without stealing focus.
    bool refreshDisplays(const std::vector<juce::Rectangle<int>>& screens) {
        if (screens == knownDisplays || !window.isShowing() || window.isMinimised()) return false;
        auto state = normal;
        if (!window.isFullScreen()) state.bounds = window.getBounds();
        state.maximized = window.isFullScreen();
        const auto area = survivingArea(state.workArea, screens);
        bool needsFit = state.workArea != area || fit(state, screens, limits) != state.bounds;
        if (const auto* peer = window.getPeer())
            if (const auto frame = peer->getFrameSizeIfPresent()) needsFit |= !area.contains(frame->addedTo(state.bounds));
        knownDisplays = screens;
        if (!needsFit) return false;
        if (state.maximized) {
            // The OS manages the maximized frame on display changes. Repair its
            // restore rectangle without ShowWindow restore/maximize activating it.
            if (auto* peer = window.getPeer()) {
                normal.bounds = fittedBounds(window, state, limits, screens);
                normal.workArea = area;
                peer->setNonFullScreenBounds(normal.bounds);
            }
        } else apply(window, state, limits, screens);
        return true;
    }
    WindowState capture() const {
        auto state = normal;
        if (!window.isFullScreen() && !window.isMinimised() && validRect(window.getBounds())) {
            state.bounds = window.getBounds(); state.workArea = areaFor(state.bounds);
        }
        if (window.isFullScreen() && !window.isMinimised()) {
            const auto area = areaFor(window.getBounds());
            if (area != state.workArea) {
                state.bounds = fit(state, {area}, {1, 1, 100000, 100000}); state.workArea = area;
            }
        }
        state.maximized = window.isFullScreen(); return state;
    }
private:
    void timerCallback() override { refreshDisplays(displays()); }
    void componentMovedOrResized(juce::Component&, bool, bool) override {
        if (!window.isFullScreen() && !window.isMinimised() && validRect(window.getBounds())) {
            normal.bounds = window.getBounds(); normal.workArea = areaFor(normal.bounds);
        }
    }
    juce::DocumentWindow& window;
    Limits limits;
    std::vector<juce::Rectangle<int>> knownDisplays;
    WindowState normal;
};
inline juce::var rectToVar(juce::Rectangle<int> r) {
    juce::Array<juce::var> values; for (int n : {r.getX(), r.getY(), r.getWidth(), r.getHeight()}) values.add(n);
    return juce::var(values);
}
inline bool readRect(const juce::var& value, juce::Rectangle<int>& result) {
    const auto* a = value.getArray(); if (!a || a->size() != 4) return false;
    for (const auto& n : *a) if (!n.isInt()) return false;
    const int x = static_cast<int>((*a)[0]), y = static_cast<int>((*a)[1]);
    const int width = static_cast<int>((*a)[2]), height = static_cast<int>((*a)[3]);
    if (width <= 0 || height <= 0 || width > 100000 || height > 100000
        || std::abs(static_cast<double>(x)) > 1000000 || std::abs(static_cast<double>(y)) > 1000000) return false;
    result = {x, y, width, height}; return true;
}
inline juce::var windowToVar(const WindowState& w) {
    auto v = juce::var(new juce::DynamicObject()); auto* o = v.getDynamicObject();
    o->setProperty("bounds", rectToVar(w.bounds)); o->setProperty("workArea", rectToVar(w.workArea)); o->setProperty("maximized", w.maximized); return v;
}
inline bool readWindow(const juce::var& value, WindowState& w) {
    const auto* o = value.getDynamicObject();
    if (!o || !o->getProperty("maximized").isBool() || !readRect(o->getProperty("bounds"), w.bounds)
        || !readRect(o->getProperty("workArea"), w.workArea)) return false;
    w.maximized = static_cast<bool>(o->getProperty("maximized")); return true;
}
inline juce::String serialize(const State& s) {
    if (!valid(s)) return {};
    auto root = juce::var(new juce::DynamicObject()); auto* o = root.getDynamicObject();
    o->setProperty("format", "AlienStudioWindowLayout"); o->setProperty("version", 1);
    o->setProperty("main", windowToVar(s.mainWindow)); o->setProperty("selectedView", viewKeys[static_cast<std::size_t>(s.selectedView)]);
    o->setProperty("devices", s.devices); o->setProperty("keyboard", s.keyboard);
    auto panels = juce::var(new juce::DynamicObject());
    for (std::size_t i = 0; i < s.panels.size(); ++i) {
        auto p = juce::var(new juce::DynamicObject()); p.getDynamicObject()->setProperty("detached", s.panels[i].detached);
        if (s.panels[i].window) p.getDynamicObject()->setProperty("window", windowToVar(*s.panels[i].window));
        panels.getDynamicObject()->setProperty(viewKeys[i], p);
    }
    o->setProperty("panels", panels); return juce::JSON::toString(root);
}
inline bool parse(const juce::String& json, State& destination) {
    const auto root = juce::JSON::parse(json); const auto* o = root.getDynamicObject();
    if (!o || o->getProperty("format").toString() != "AlienStudioWindowLayout" || !o->getProperty("version").isInt()
        || static_cast<int>(o->getProperty("version")) != 1 || !o->getProperty("devices").isBool() || !o->getProperty("keyboard").isBool()) return false;
    State candidate;
    if (!readWindow(o->getProperty("main"), candidate.mainWindow)) return false;
    candidate.selectedView = -1;
    for (int i = 0; i < viewCount; ++i) if (o->getProperty("selectedView").toString() == viewKeys[static_cast<std::size_t>(i)]) candidate.selectedView = i;
    candidate.devices = static_cast<bool>(o->getProperty("devices")); candidate.keyboard = static_cast<bool>(o->getProperty("keyboard"));
    const auto panels = o->getProperty("panels"); const auto* list = panels.getDynamicObject();
    if (!list || list->getProperties().size() != viewCount) return false;
    for (std::size_t i = 0; i < candidate.panels.size(); ++i) {
        const auto entry = list->getProperty(viewKeys[i]); const auto* p = entry.getDynamicObject();
        if (!p || !p->getProperty("detached").isBool()) return false;
        candidate.panels[i].detached = static_cast<bool>(p->getProperty("detached"));
        if (p->hasProperty("window")) { WindowState w; if (!readWindow(p->getProperty("window"), w)) return false; candidate.panels[i].window = w; }
    }
    if (!valid(candidate)) return false; destination = std::move(candidate); return true;
}
inline juce::File defaultFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio").getChildFile("WindowLayout.json");
}
inline State defaultState() {
    State result;
    result.mainWindow.workArea = displays().front();
    result.mainWindow.bounds = juce::Rectangle<int>(1280, 800).withCentre(result.mainWindow.workArea.getCentre());
    return result;
}
inline State load(const juce::File& file) {
    State result = defaultState();
    if (file.getSize() > 0 && file.getSize() <= 65536) parse(file.loadFileAsString(), result);
    return result;
}
inline bool save(const juce::File& file, const State& state) {
    const auto json = serialize(state);
    if (json.isEmpty() || !file.getParentDirectory().createDirectory().wasOk()) return false;
    juce::TemporaryFile temporary(file);
    return temporary.getFile().replaceWithText(json) && temporary.overwriteTargetFileWithTemporary();
}
} // namespace alien::layout
