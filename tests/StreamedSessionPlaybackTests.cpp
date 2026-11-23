#include "SessionAudioClip.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
        Reader(bool fail, const Source& owner) : fail(fail), owner(owner) {}
        bool read(std::int64_t start, float* left, float* right, int count) override {
            if (fail) return false;
            // Past stallFrom the disk is "slow": the read waits until released (or gives up after 5 s).
            const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (start >= owner.stallFrom.load() && !owner.released.load() && std::chrono::steady_clock::now() < giveUp)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (start >= owner.failFrom.load()) return false;
            for (int i=0;i<count;++i) {
                left[i]=static_cast<float>(start+i)*0.001f;
                right[i]=-left[i];
            }
            return true;
        }
        bool fail;
        const Source& owner;
    };
    std::unique_ptr<alien::DiskAudioReader> open() const override {
        return std::make_unique<Reader>(fail.load(), *this);
    }
    std::atomic<bool> fail{}, released{};
    std::atomic<std::int64_t> failFrom{std::numeric_limits<std::int64_t>::max()}, stallFrom{std::numeric_limits<std::int64_t>::max()};
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
    first.bind(streams.reserve(0),streams.reserve(1));
    duplicate.bind(streams.reserve(2),streams.reserve(3));
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

    // T1-04 D12: a stream that could not be read is counted once, and the count outlives the next queue.
    require(first.failureCount()==1 && first.underrunCount()==0,"a failed launch is one failure, not an underrun");
    require(first.queue(&clip,1) && !first.streamFailed() && first.failureCount()==1,"queueing again clears the flag but keeps the count");
    first.stop();
    // A stream that fails while it plays: one failure, and its silence is not counted as underruns.
    {
        // The read at 2048 waits until the stream has started, then fails, so the launch always sees it ready.
        auto dying=std::make_shared<Source>(20000); dying->failFrom.store(2048); dying->stallFrom.store(2048);
        require(streams.retain(dying)==dying.get(),"dying source retained");
        alien::SessionAudioClip clipOf; clipOf.sampleRate=48000; clipOf.channels=2; clipOf.frames=20000; clipOf.streamSource=dying;
        alien::SessionAudioPlayer player; player.bind(streams.reserve(4),streams.reserve(5));
        require(player.queue(&clipOf,1) && waitFor([&]{return player.pendingReady();}) && player.launchPendingNow(48000),"dying stream starts");
        dying->released.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the worker reaches the unreadable part
        for(int i=0;i<12000;++i) player.nextSample();
        require(player.failureCount()==1 && player.streamFailed(),"a stream that fails while playing is one failure");
        require(player.underrunCount()==0,"and its silence is not an underrun");
    }
    // A stream the disk is too slow for: the silent samples are underruns, not a failure.
    {
        auto slow=std::make_shared<Source>(40000); slow->stallFrom.store(12288);
        require(streams.retain(slow)==slow.get(),"slow source retained");
        alien::SessionAudioClip clipOf; clipOf.sampleRate=48000; clipOf.channels=2; clipOf.frames=40000; clipOf.streamSource=slow;
        alien::SessionAudioPlayer player; player.bind(streams.reserve(6),streams.reserve(7));
        require(player.queue(&clipOf,1) && waitFor([&]{return player.pendingReady();}) && player.launchPendingNow(48000),"slow stream starts");
        for(int i=0;i<20000;++i) player.nextSample();
        require(player.underrunCount()>0 && player.failureCount()==0 && !player.streamFailed(),"a stalled stream underruns without failing");
        slow->released.store(true);
        player.stop();
    }
    std::cout<<"PASS: streamed Session playback\n";
}
