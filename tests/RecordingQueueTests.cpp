#include "RecordingQueue.h"
#include <cstdlib>
#include <iostream>
#include <thread>

static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    // A stalled disk seals a continuous prefix, rather than silently dropping
    // a block and then accepting later audio with a gap.
    alien::RecordingQueue overflow(48000, 2, 5);
    float left[]{1, 2, 3, 4, 5, 6, 7}, right[]{-1, -2, -3, -4, -5, -6, -7};
    const float* input[]{left, right};
    overflow.append(input, 2, 7);
    check(overflow.done && overflow.overrun && overflow.frames == 5, "overflow must seal accepted prefix");
    int offset = 0;
    while (overflow.drain([&](const float* const* data, int channels, int count) {
        check(channels == 2, "stereo format lost");
        for (int i = 0; i < count; ++i, ++offset)
            check(data[0][i] == left[offset] && data[1][i] == right[offset], "overflow prefix corrupted");
        return true;
    }, 2)) {}
    overflow.append(input, 2, 2);
    check(offset == 5 && overflow.empty() && overflow.frames == 5, "sealed recording accepted later audio");

    // A failed write must leave the same frames available to the consumer.
    alien::RecordingQueue retry(48000, 1, 5);
    retry.append(input, 1, 3);
    check(!retry.drain([](const float* const*, int, int) { return false; }), "write failure not reported");
    check(!retry.empty(), "failed write discarded audio");
    check(retry.drain([&](const float* const* data, int channels, int count) {
        check(channels == 1 && count == 3, "retry frame count changed");
        for (int i = 0; i < count; ++i) check(data[0][i] == left[i], "retry payload changed");
        return true;
    }), "retry failed");
    check(retry.empty(), "successful write did not release frames");

    alien::RecordingQueue sanitize(48000, 2, 5);
    float bad[]{NAN, INFINITY, -INFINITY, 0.25f};
    const float* badInput[]{bad};
    sanitize.append(badInput, 1, 4);
    check(sanitize.drain([](const float* const* data, int, int count) {
        check(count == 4, "sanitization lost frames");
        for (int i = 0; i < count; ++i)
            check(data[0][i] == (i == 3 ? 0.25f : 0.0f) && data[1][i] == data[0][i], "nonfinite or mono input mishandled");
        return true;
    }), "sanitization drain failed");

    // Exercise many wraps with a real producer and consumer. Acknowledgement
    // throttles only this test producer; production capture never waits.
    alien::RecordingQueue concurrent(48000, 2, 257);
    constexpr unsigned total = 1000000;
    std::atomic<unsigned> acknowledged{};
    std::thread consumer([&] {
        unsigned received = 0;
        while (received < total) {
            if (concurrent.drain([&](const float* const* data, int channels, int count) {
                check(channels == 2, "concurrent format changed");
                for (int i = 0; i < count; ++i, ++received) {
                    const auto expected = static_cast<float>(received % 1024) / 1024.0f;
                    check(data[0][i] == expected && data[1][i] == -expected, "concurrent audio reordered or corrupted");
                }
                return true;
            }, 31)) acknowledged.store(received, std::memory_order_release);
            else std::this_thread::yield();
        }
    });
    unsigned sent = 0;
    float l[17], r[17]; const float* block[]{l, r};
    while (sent < total) {
        const unsigned count = std::min(17u, total - sent);
        if (sent - acknowledged.load(std::memory_order_acquire) + count > concurrent.capacity) {
            std::this_thread::yield(); continue;
        }
        for (unsigned i = 0; i < count; ++i) {
            l[i] = static_cast<float>((sent + i) % 1024) / 1024.0f; r[i] = -l[i];
        }
        concurrent.append(block, 2, static_cast<int>(count)); sent += count;
    }
    consumer.join();
    check(concurrent.empty() && concurrent.frames == total && !concurrent.overrun, "concurrent recording lost frames");
    std::cout << "Recording queue reliability checks passed\n";
}
