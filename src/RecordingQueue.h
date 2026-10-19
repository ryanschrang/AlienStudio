#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace alien {
// Shared capture contract. Only one audio producer; ownership/detachment is
// coordinated by AudioDeviceManager before the owner finishes/destroys a take.
struct AudioCaptureSink {
    AudioCaptureSink(double rate, int count) : sampleRate(rate), channels(count) {}
    virtual ~AudioCaptureSink() = default;
    virtual void append(const float* const*, int, int) noexcept = 0;
    const double sampleRate;
    const int channels;
    std::atomic<std::uint64_t> frames{};
    std::atomic<bool> done{};
    std::atomic<float> level{};
};

// Fixed-memory SPSC FIFO. No locks, signalling, allocation or file access on
// its producer. An overrun seals the accepted prefix instead of hiding a gap.
class RecordingQueue final : public AudioCaptureSink {
public:
    static constexpr std::size_t defaultCapacity = 262144;
    RecordingQueue(double rate, int channelCount, std::size_t bufferFrames = defaultCapacity)
        : AudioCaptureSink(rate, channelCount), capacity(checked(rate,channelCount,bufferFrames)),
          left(capacity), right(channelCount == 2 ? capacity : 0) {}
    void append(const float* const* inputs, int inputChannels, int count) noexcept override {
        if (done.load(std::memory_order_acquire) || count <= 0) return;
        const auto start = written.load(std::memory_order_relaxed);
        const auto used = start - consumed.load(std::memory_order_acquire);
        const auto n = std::min<std::uint64_t>(static_cast<unsigned>(count), capacity - used);
        float peak = 0;
        for (std::uint64_t i=0;i<n;++i) {
            const auto index=static_cast<std::size_t>((start+i)%capacity);
            float l=inputs && inputChannels>0 && inputs[0]?inputs[0][i]:0;
            float r=inputs && inputChannels>1 && inputs[1]?inputs[1][i]:l;
            l=std::isfinite(l)?l:0; r=std::isfinite(r)?r:0;
            left[index]=l; if(channels==2)right[index]=r;
            peak=std::max(peak,std::abs(l)); if(channels==2)peak=std::max(peak,std::abs(r));
        }
        written.store(start+n,std::memory_order_release);
        frames.store(start+n,std::memory_order_release); level.store(peak);
        if(n<static_cast<unsigned>(count)) { overrun.store(true,std::memory_order_release);done.store(true,std::memory_order_release); }
    }
    // Consumer callback runs on the disk thread. Slots aren't reused until the
    // callback returns, so the writer can encode directly from the ring.
    template<class Writer> bool drain(Writer&& write, int maximum=4096) {
        if(maximum<=0)return false;
        const auto begin=consumed.load(std::memory_order_relaxed);
        const auto end=written.load(std::memory_order_acquire);
        if(begin==end)return false;
        const auto index=static_cast<std::size_t>(begin%capacity);
        const auto n=static_cast<int>(std::min<std::uint64_t>({end-begin,capacity-index,static_cast<std::uint64_t>(maximum)}));
        const float* data[]{left.data()+index,channels==2?right.data()+index:nullptr};
        if(!write(data,channels,n))return false;
        consumed.store(begin+static_cast<unsigned>(n),std::memory_order_release);return true;
    }
    bool empty() const noexcept { return consumed.load(std::memory_order_acquire)==written.load(std::memory_order_acquire); }
    std::size_t memoryBytes() const noexcept { return (left.size()+right.size())*sizeof(float); }
    const std::size_t capacity;
    std::atomic<bool> overrun{};
private:
    static std::size_t checked(double rate,int channels,std::size_t capacity) {
        if(!std::isfinite(rate)||rate<8000||rate>192000||channels<1||channels>2||capacity<1||capacity>1048576)
            throw std::invalid_argument("Invalid recording format or buffer size");
        return capacity;
    }
    std::vector<float> left,right;
    std::atomic<std::uint64_t> written{},consumed{};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"Recording needs lock-free 64-bit positions");
}
