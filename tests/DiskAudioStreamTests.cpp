#include "DiskAudioStream.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <limits>
#include <stdexcept>

thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template<class Predicate> bool waitFor(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
class VirtualFile final : public alien::DiskAudioFile {
public:
    VirtualFile(std::int64_t frames, int channels = 2) : DiskAudioFile(48000, channels, frames) {}
    static float value(std::int64_t frame) { return static_cast<float>(frame % 1000) * 0.0005f; }
    struct Reader final : alien::DiskAudioReader {
        explicit Reader(const VirtualFile& owner) : owner(owner) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            owner.reads.fetch_add(1);
            auto prior = owner.largestRead.load();
            while (prior < count && !owner.largestRead.compare_exchange_weak(prior, count)) {}
            if (owner.throwRead.load()) throw std::runtime_error("injected decoder failure");
            if (owner.failRead.load()) return false;
            for (int i = 0; i < count; ++i) {
                left[i] = owner.nonFinite.load() ? std::numeric_limits<float>::infinity() : value(start+i);
                if (right) right[i] = -left[i];
            }
            return true;
        }
        const VirtualFile& owner;
    };
    std::unique_ptr<alien::DiskAudioReader> open() const override {
        opens.fetch_add(1);
        if (failOpen.load()) return {};
        return std::make_unique<Reader>(*this);
    }
    mutable std::atomic<int> opens{}, reads{}, largestRead{};
    std::atomic<bool> failOpen{}, failRead{}, throwRead{}, nonFinite{};
};
int main() {
    using Service = alien::DiskAudioStreamService;
    Service service;
    require(service.voice(Service::voiceCount) == nullptr, "bounded admission rejects excess endpoint");
    require(service.retain({}) == nullptr, "null source rejected");
    require(service.retain(std::make_shared<VirtualFile>(0)) == nullptr, "invalid source rejected");
    auto source = std::make_shared<VirtualFile>(5000000000LL);
    const auto* registered = service.retain(source);
    auto* a = service.voice(0); auto* b = service.voice(1);
    countAllocations = true;
    const auto first = a->request(registered);
    const auto other = b->request(registered, 200);
    countAllocations = false;
    require(allocations == 0, "request does not allocate on the calling thread");
    require(waitFor([&] { return a->ready(first) && b->ready(other); }), "independent decoders prebuffer");
    require(source->opens.load() == 2, "one reader per playback, not per shared file");
    float left = 7, right = 8;
    require(b->sample(other, 200, 201, 0.5f, left, right), "second playback starts at its own position");
    require(std::abs(left - (VirtualFile::value(200)+VirtualFile::value(201))*.5f) < 1e-7f
        && right == -left, "stereo interpolation");
    for (std::int64_t n = 0; n < 20000; ++n) {
        require(waitFor([&] { return a->sample(first, n, n+1, 0, left, right); }), "wrapped source frame available");
        require(left == VirtualFile::value(n) && right == -left, "worker/ring order and stereo agree");
        countAllocations = true;
        a->releaseBefore(first, n+1);
        a->sample(first, n+1, n+1, 0, left, right);
        countAllocations = false;
    }
    require(allocations == 0, "sample and release do not allocate on the caller");
    require(b->sample(other, 200, 200, 0, left, right) && left == VirtualFile::value(200), "other voice cursor untouched");
    require(source->largestRead.load() <= 1024, "decoder read size bounded independently of duration");
    const auto seek = a->request(registered, 4999999984LL);
    require(!a->sample(first, 0, 0, 0, left, right), "superseded generation rejected immediately");
    require(waitFor([&] { return a->ready(seek); }), "64-bit tail seek prebuffer");
    a->releaseBefore(first, 5000000000LL); // A superseded consumer must not drain new data.
    require(a->sample(seek, 4999999999LL, 4999999999LL, 0, left, right)
        && left == VirtualFile::value(4999999999LL), "last frame without out-of-range lookahead");
    for (int i = 0; i < 1000; ++i) a->request(registered, i);
    const auto latest = a->request(registered, 12345);
    require(waitFor([&] { return a->ready(latest); }), "rapid restart reaches latest request");
    require(a->sample(latest, 12345, 12345, 0, left, right) && left == VirtualFile::value(12345), "no stale samples after rapid restarts");
    const auto stopped = a->request(nullptr);
    left = 7; right = 8;
    require(!a->sample(stopped, 0, 0, 0, left, right) && left == 7 && right == 8, "stopped stream returns unavailable without changing output");
    const auto invalid = a->request(registered, -1);
    require(waitFor([&] { return a->failed(invalid); }), "negative seek rejected");
    const auto pastEnd = a->request(registered, source->frames);
    require(waitFor([&] { return a->failed(pastEnd); }), "seek beyond EOF rejected");
    auto broken = std::make_shared<VirtualFile>(10000);
    broken->failOpen = true;
    const auto* missing = service.retain(broken);
    auto failure = a->request(missing);
    require(waitFor([&] { return a->failed(failure); }), "open failure reported");
    broken->failOpen = false; broken->failRead = true;
    failure = a->request(missing);
    require(waitFor([&] { return a->failed(failure); }), "read failure reported");
    broken->failRead = false; broken->throwRead = true;
    failure = a->request(missing);
    require(waitFor([&] { return a->failed(failure); }), "decoder exception contained on worker");
    broken->throwRead = false; broken->nonFinite = true;
    const auto sanitized = a->request(missing);
    require(waitFor([&] { return a->ready(sanitized); }), "new request recovers after failure");
    require(a->sample(sanitized, 0, 1, 0.5f, left, right) && left == 0 && right == 0, "non-finite source samples sanitized");
    auto mono = std::make_shared<VirtualFile>(1, 1);
    const auto single = a->request(service.retain(mono));
    require(waitFor([&] { return a->ready(single); }), "one-frame mono source prebuffers at EOF");
    require(a->sample(single, 0, 0, 0, left, right) && left == right, "mono duplicates into stereo");
    static_assert(Service::Voice::sampleStorageBytes == Service::ringFrames * sizeof(Service::Frame));
    // Local shared pointers may disappear while readers still use retained sources.
    source.reset(); broken.reset(); mono.reset();
    std::cout << "Independent streaming readers, bounded memory, wrap, seek, restart, failures and callback allocations passed\n";
}
