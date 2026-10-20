#pragma once
#include "SessionAudioClip.h"
#include "WaveformCache.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace alien {

class AudioClipWaveform final : public juce::Component {
public:
    enum class State { empty, offline, loading, readyDecoded, readyStreaming, error };

    AudioClipWaveform() { setTooltip("Ctrl+mouse wheel zooms; mouse wheel scrolls a zoomed waveform"); }
    ~AudioClipWaveform() override { alive.reset(); jobs.removeAllJobs(true, -1); }

    void setClip(const SessionAudioClip* replacement, juce::String displayName,
                 juce::String sourcePath = {}, juce::File project = {}, bool isOffline = false) {
        const juce::File incomingSource(sourcePath);
        const auto incomingSize = incomingSource.existsAsFile() ? incomingSource.getSize() : -1;
        const auto incomingTime = incomingSource.existsAsFile() ? incomingSource.getLastModificationTime().toMilliseconds() : -1;
        const auto changed = clip != replacement || source != sourcePath || projectFile != project
            || incomingSize != sourceSize || incomingTime != sourceTime || isOffline != (state == State::offline);
        clip = replacement; name = std::move(displayName); source = std::move(sourcePath); projectFile = std::move(project);
        sourceSize = incomingSize; sourceTime = incomingTime;
        if (!changed) { repaint(); return; }
        ++generation; detail.reset(); errorText.clear(); zoom = 1.0; viewStart = 0.0;
        if (isOffline) state = State::offline;
        else if (!clip) state = State::empty;
        else if (!clip->streamed()) state = State::readyDecoded;
        else {
            state = State::loading;
            const auto sourceFile = juce::File(source);
            const auto cacheFile = LongWaveformCache::fileFor(projectFile, sourceFile);
            const auto request = generation.load();
            std::weak_ptr<int> token = alive;
            std::function<juce::ThreadPoolJob::JobStatus()> job = [this, token, sourceFile, cacheFile, request] {
                auto result = std::make_shared<WaveformCacheResult>(
                    LongWaveformCache::loadOrCreate(sourceFile, cacheFile, [this, token, request] {
                        return !token.lock() || request != generation.load();
                    }));
                juce::MessageManager::callAsync([this, token, result, request] {
                    if (!token.lock() || request != generation.load()) return;
                    detail = result->data; errorText = result->error;
                    state = detail ? State::readyStreaming : State::error;
                    repaint();
                });
                return juce::ThreadPoolJob::jobHasFinished;
            };
            jobs.addJob(std::move(job));
        }
        repaint();
    }

    State displayState() const noexcept { return state; }
    double zoomFactor() const noexcept { return zoom; }
    double visibleStart() const noexcept { return viewStart; }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        if (state != State::readyDecoded && state != State::readyStreaming) return;
        if (event.mods.isCtrlDown() || event.mods.isCommandDown()) {
            const double anchor = juce::jlimit(0.0, 1.0, event.position.x / std::max(1.0f, static_cast<float>(getWidth())));
            const double before = viewStart + anchor / zoom;
            zoom = juce::jlimit(1.0, 128.0, zoom * std::pow(2.0, wheel.deltaY * 2.0));
            viewStart = juce::jlimit(0.0, 1.0 - 1.0 / zoom, before - anchor / zoom);
        } else if (zoom > 1.0) {
            const double delta = wheel.deltaX != 0 ? wheel.deltaX : wheel.deltaY;
            viewStart = juce::jlimit(0.0, 1.0 - 1.0 / zoom, viewStart - delta / zoom);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0d0e11));
        g.setColour(juce::Colour(0xff26282d));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 5.0f, 1.0f);
        if (state == State::empty || state == State::offline || state == State::loading || state == State::error) {
            g.setColour(state == State::error || state == State::offline ? juce::Colour(0xffff8a65) : juce::Colour(0xff8b919a));
            g.drawText(statusText(), getLocalBounds().reduced(10), juce::Justification::centred);
            return;
        }

        const auto area = getLocalBounds().reduced(8, 18).toFloat();
        const int channels = clip ? clip->channels : (detail ? detail->channels : 1);
        const float laneHeight = channels == 2 ? area.getHeight() * 0.5f : area.getHeight();
        g.setColour(juce::Colour(0xff26282d));
        g.drawHorizontalLine(static_cast<int>(area.getY() + laneHeight * 0.5f), area.getX(), area.getRight());
        if (channels == 2)
            g.drawHorizontalLine(static_cast<int>(area.getY() + laneHeight * 1.5f), area.getX(), area.getRight());

        g.setColour(juce::Colour(0xff4fe0b6));
        const int columns = juce::jmax(1, static_cast<int>(area.getWidth()));
        const auto count = peakTotal();
        const double visible = 1.0 / zoom;
        for (int x=0;x<columns;++x) {
            const double a = viewStart + visible * x / columns;
            const double b = viewStart + visible * (x + 1) / columns;
            const auto first = std::min(count-1, static_cast<std::size_t>(a*count));
            const auto last = std::min(count-1, std::max(first, static_cast<std::size_t>(std::ceil(b*count))-1));
            auto peak = peakAt(first);
            for (auto i=first+1;i<=last;++i) {
                const auto p=peakAt(i);
                peak.leftMin=std::min(peak.leftMin,p.leftMin); peak.leftMax=std::max(peak.leftMax,p.leftMax);
                peak.rightMin=std::min(peak.rightMin,p.rightMin); peak.rightMax=std::max(peak.rightMax,p.rightMax);
            }
            drawPeak(g,area.getX()+static_cast<float>(x),area.getY(),laneHeight,peak.leftMin,peak.leftMax);
            if(channels==2) drawPeak(g,area.getX()+static_cast<float>(x),area.getY()+laneHeight,laneHeight,peak.rightMin,peak.rightMax);
        }
        g.setColour(juce::Colour(0xff9ea3ab));
        g.drawText(name,8,2,getWidth()-16,16,juce::Justification::centredLeft,true);
        const juce::String badge = state == State::readyStreaming ? "STREAMING  " : "";
        g.drawText(badge + (zoom > 1.01 ? juce::String(zoom,1) + "x" : juce::String{}),
            8,2,getWidth()-16,16,juce::Justification::centredRight,true);
    }

private:
    juce::String statusText() const {
        if (state == State::offline) return "Audio offline. Use Relink audio to locate the source.";
        if (state == State::loading) return "Generating waveform in the background...";
        if (state == State::error) return "Waveform unavailable: " + errorText;
        return "No audio in this clip. Use Import audio to add a WAV or MP3.";
    }
    std::size_t peakTotal() const noexcept {
        if (state == State::readyStreaming && detail) return detail->peaks.size();
        return clip ? clip->waveformCount : 0;
    }
    AudioWaveformPeak peakAt(std::size_t index) const noexcept {
        if (state == State::readyStreaming && detail && index < detail->peaks.size()) return detail->peaks[index];
        return clip && index < clip->waveformCount ? clip->waveform[index] : AudioWaveformPeak{};
    }
    static void drawPeak(juce::Graphics& g,float x,float y,float height,float minimum,float maximum) {
        const float centre=y+height*0.5f, amplitude=height*0.43f;
        g.drawVerticalLine(static_cast<int>(x),centre-maximum*amplitude,centre-minimum*amplitude);
    }

    const SessionAudioClip* clip{};
    std::shared_ptr<const WaveformPeakData> detail;
    juce::String name, source, errorText;
    juce::File projectFile;
    juce::ThreadPool jobs{1};
    std::shared_ptr<int> alive=std::make_shared<int>(0);
    std::atomic<std::uint64_t> generation{};
    State state{State::empty};
    double zoom{1.0}, viewStart{};
    juce::int64 sourceSize{-1}, sourceTime{-1};
};

} // namespace alien
