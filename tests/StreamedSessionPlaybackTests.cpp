#include "SessionAudioClip.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

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
class Source final : public alien::DiskAudioFile {
public:
    explicit Source(std::int64_t count) : DiskAudioFile(48000, 2, count) {}
    struct Reader final : alien::DiskAudioReader {
        explicit Reader(bool fail) : fail(fail) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            if (fail) return false;
            for (int i=0;i<count;++i) {
                left[i]=static_cast<float>(start+i)*0.001f;
                right[i]=-left[i];
            }
            return true;
        }
        bool fail;
    };
    std::unique_ptr<alien::DiskAudioReader> open() const override {
        return std::make_unique<Reader>(fail.load());
    }
    std::atomic<bool> fail{};
};

int main() {
    alien::DiskAudioStreamService streams;
    auto source=std::make_shared<Source>(10000);
    require(streams.retain(source)==source.get(),"source retained before audio publication");

    alien::SessionAudioClip clip;
    clip.sampleRate=source->sampleRate; clip.channels=source->channels;
    clip.frames=static_cast<std::size_t>(source->frames); clip.streamSource=source;
    require(clip.valid() && clip.streamed() && clip.decodedBytes()==0,
        "large Session clip validates without decoded sample storage");

    alien::SessionAudioPlayer first, duplicate;
    first.bind(streams.voice(0),streams.voice(1));
    duplicate.bind(streams.voice(2),streams.voice(3));
    require(first.queue(&clip,16) && duplicate.queue(&clip,16),"duplicate launches receive independent endpoints");
    require(!first.launchPendingNow(48000),"launch waits instead of replacing the current clip before prebuffer");
    require(waitFor([&]{return first.pendingReady()&&duplicate.pendingReady();}),"both playback instances prebuffer");
    require(first.launchPendingNow(48000)&&duplicate.launchPendingNow(48000),"ready streams launch");
    const auto a=first.nextSample(); const auto b=duplicate.nextSample();
    require(a[0]==0&&a[1]==0&&b==a,"duplicates start from independent frame-zero cursors");
    const auto next=first.nextSample();
    require(std::abs(next[0]-0.001f)<1e-7f&&std::abs(next[1]+0.001f)<1e-7f,
        "streamed stereo samples reach the player in order");
    require(duplicate.nextSample()==next,"duplicate playback is not consumed by the first instance");

    first.stop();
    require(!first.isPlaying()&&first.queue(&clip,1),"device stop clears playback and accepts a clean relaunch");
    require(waitFor([&]{return first.pendingReady();})&&first.launchPendingNow(44100),"relaunch prebuffers after stop");
    require(first.nextSample()[0]==0,"relaunch resets the source cursor");

    auto broken=std::make_shared<Source>(10000); broken->fail.store(true);
    require(streams.retain(broken)==broken.get(),"failing source retained");
    alien::SessionAudioClip bad;
    bad.sampleRate=broken->sampleRate; bad.channels=2; bad.frames=10000; bad.streamSource=broken;
    require(first.queue(&bad,1),"decoder failure request accepted asynchronously");
    require(waitFor([&]{return first.pendingFailed();}),"decoder failure becomes visible");
    require(!first.launchPendingNow(48000)&&first.streamFailed(),"failed prebuffer is rejected and reported");
    require(first.isPlaying(),"failed replacement leaves the active clip playing");
}
