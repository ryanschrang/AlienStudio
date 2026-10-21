#include "StreamingAudioRing.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    using Ring = alien::StreamingAudioRing<8>;
    Ring ring;
    Ring::Frame sample{9, -9};
    require(!ring.peek(0, sample) && sample.left == 9, "empty peek preserves destination");
    require(!ring.consume(1), "cannot consume unavailable audio");
    std::array<Ring::Frame, 10> input{};
    for (int i = 0; i < 10; ++i) input[i] = {static_cast<float>(i), static_cast<float>(-i)};
    require(ring.write(input) == 8 && ring.write(input) == 0, "full ring never overwrites unread audio");
    require(ring.peek(1, sample) && sample.left == 1 && sample.right == -1, "interpolation lookahead preserves stereo");
    require(!ring.consume(9) && ring.consume(6), "over-consumption leaves the cursor unchanged");
    require(ring.write(std::span<const Ring::Frame>(input).subspan(8)) == 2, "producer wraps");
    for (int i = 6; i < 10; ++i) {
        require(ring.peek(0, sample) && sample.left == i && sample.right == -i, "wrapped sample order");
        require(ring.consume(1), "consumer advances");
    }
    require(!ring.peek(0, sample), "drained ring is empty");
    Ring independent;
    require(!independent.peek(0, sample), "independent playback cursors share no samples");

    // Exercise enough samples to wrap many times with real producer/consumer
    // threads. A bounded test timeout catches stalls instead of hanging CI.
    using Concurrent = alien::StreamingAudioRing<1024>;
    auto concurrent = std::make_unique<Concurrent>();
    constexpr int total = 200000;
    std::thread decoder([&] {
        for (int i = 0; i < total;) {
            const Concurrent::Frame frame{static_cast<float>(i), static_cast<float>(-i)};
            if (concurrent->write(std::span<const Concurrent::Frame>(&frame, 1)) == 1) ++i;
            else std::this_thread::yield();
        }
    });
    for (int i = 0; i < total;) {
        Concurrent::Frame frame;
        if (concurrent->peek(0, frame)) {
            require(frame.left == i && frame.right == -i, "concurrent samples remain ordered and stereo aligned");
            require(concurrent->consume(1), "concurrent consumption");
            ++i;
        } else std::this_thread::yield();
    }
    decoder.join();
    static_assert(Concurrent::sampleStorageBytes == 1024 * 2 * sizeof(float));
    std::cout << "Streaming ring boundaries, lookahead, wrapping and concurrent stereo passed\n";
}
