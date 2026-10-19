#pragma once
#include "SessionAudioClip.h"
#include <memory>
#include <stdexcept>

namespace alien {
// Allocate on the message thread; exactly one audio writer. Stop/detach the
// callback before reading sample storage or destroying this object.
class AudioCapture {
public:
    AudioCapture(double rate,int channelCount,int seconds=60)
        : sampleRate(rate),channels(std::clamp(channelCount,1,2)),
          capacity(checkedCapacity(rate,channelCount,seconds)),
          left(capacity),right(channels==2?capacity:0) {}
    void append(const float* const* inputs,int inputChannels,int count) noexcept {
        if(done.load(std::memory_order_relaxed)) return;
        const auto available=std::min(static_cast<std::size_t>(std::max(0,count)),capacity-written);
        float peak=0;
        for(std::size_t i=0;i<available;++i) {
            const float l=inputChannels>0 && inputs && inputs[0]?inputs[0][i]:0;
            const float r=inputChannels>1 && inputs && inputs[1]?inputs[1][i]:l;
            left[written+i]=std::isfinite(l)?l:0;
            if(channels==2) right[written+i]=std::isfinite(r)?r:0;
            peak=std::max(peak,std::abs(left[written+i]));
            if(channels==2)peak=std::max(peak,std::abs(right[written+i]));
        }
        written+=available;frames.store(written,std::memory_order_release);level.store(peak);
        if(written==capacity) done.store(true,std::memory_order_release);
    }
    std::shared_ptr<SessionAudioClip> finish() {
        if(written==0) return {};
        auto clip=std::make_shared<SessionAudioClip>();clip->sampleRate=sampleRate;clip->channels=channels;clip->frames=written;
        left.resize(written);if(channels==2)right.resize(written);
        clip->left=std::move(left);clip->right=std::move(right);clip->buildWaveform();return clip;
    }
    const double sampleRate;
    const int channels;
    const std::size_t capacity;
    std::atomic<std::size_t> frames{};
    std::atomic<bool> done{};
    std::atomic<float> level{};
private:
    static std::size_t checkedCapacity(double rate,int channels,int seconds) {
        if(!std::isfinite(rate) || rate<8000 || rate>192000 || channels<1 || channels>2 || seconds<1 || seconds>60)
            throw std::invalid_argument("Invalid audio capture format or duration");
        return static_cast<std::size_t>(std::ceil(rate*seconds));
    }
    std::size_t written{};
    std::vector<float> left,right;
};
} // namespace alien
