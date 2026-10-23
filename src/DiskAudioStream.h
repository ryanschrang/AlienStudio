#pragma once
#include "StreamingAudioRing.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace alien {
// Immutable source metadata, independent of playback state and of JUCE.
// The owner registers sources before publishing pointers to the audio thread.
struct DiskAudioReader {
    virtual ~DiskAudioReader() = default;
    virtual bool read(std::int64_t start, float* left, float* right, int count) = 0;
};
struct DiskAudioFile {
    DiskAudioFile(double rate, int channelCount, std::int64_t frameCount)
        : sampleRate(rate), channels(channelCount), frames(frameCount) {}
    virtual ~DiskAudioFile() = default;
    virtual std::unique_ptr<DiskAudioReader> open() const = 0;
    const double sampleRate;
    const int channels;
    const std::int64_t frames;
    bool valid() const noexcept {
        return std::isfinite(sampleRate) && sampleRate >= 8000 && sampleRate <= 192000
            && channels >= 1 && channels <= 2 && frames > 0;
    }
};

// Fixed admission: at most two independently prebuffered voices per DAW track.
// One worker owns all readers. Endpoint methods below are single-consumer,
// allocation-free audio-side operations. Registration and destruction are not.
class DiskAudioStreamService {
public:
    static constexpr std::size_t voiceCount = 48;   // per Session track: two for its clips, two for a crossfade's outgoing clip
    static constexpr std::size_t ringFrames = 8192;
    static constexpr std::size_t prebufferFrames = 2048;
    struct Frame {
        float left{}, right{};
        std::uint64_t generation{};
        std::int64_t position{};
    };
    class Voice {
    public:
        // source must have been retained by this service. Passing nullptr stops.
        // Requests supersede old work. No ring indices are reset by either side.
        std::uint64_t request(const DiskAudioFile* source, std::int64_t startFrame = 0) noexcept {
            if (++nextGeneration == 0) ++nextGeneration;
            const auto generation = nextGeneration;
            const auto available = ring.available();
            ring.consume(available); // Consumer ownership; slots remain safe to reuse.
            if (deterministic.load(std::memory_order_acquire)) { serveHere({source, generation, startFrame}); return generation; }
            buffers[back] = {source, generation, startFrame};
            latest.store(generation, std::memory_order_release);
            back = middle.exchange(back | dirty, std::memory_order_acq_rel) & 3;
            return generation;
        }
        bool ready(std::uint64_t generation) const noexcept {
            return generation != 0 && latest.load(std::memory_order_acquire) == generation
                && readyGeneration.load(std::memory_order_acquire) == generation;
        }
        bool failed(std::uint64_t generation) const noexcept {
            return generation != 0 && failedGeneration.load(std::memory_order_acquire) == generation;
        }
        // No cursor advancement on an unavailable frame. The eventual Session
        // owner must explicitly choose its launch/underrun timing policy.
        bool sample(std::uint64_t generation, std::int64_t first, std::int64_t second,
                    float fraction, float& left, float& right) noexcept {
            if (!ready(generation) || failed(generation) || first < 0 || second < first
                || !std::isfinite(fraction) || fraction < 0 || fraction > 1) return false;
            if (deterministic.load(std::memory_order_acquire) && !readAhead(generation, first, second)) return false;
            Frame a, b;
            // One bounded drain discards stale generations or frames before first.
            // Producer never overwrites unread frames, including while peeking.
            for (std::size_t discarded = 0; discarded < ringFrames; ++discarded) {
                if (!ring.peek(0, a)) return false;
                if (a.generation == generation && a.position >= first) break;
                ring.consume(1);
            }
            if (!ring.peek(0, a) || a.generation != generation || a.position != first) return false;
            const auto delta = static_cast<std::uint64_t>(second - first);
            if (delta >= ringFrames || !ring.peek(static_cast<std::size_t>(delta), b)
                || b.generation != generation || b.position != second) return false;
            left = a.left + (b.left - a.left) * fraction;
            right = a.right + (b.right - a.right) * fraction;
            return true;
        }
        // Audio consumer releases samples strictly before its next first frame.
        // Kept separate from sample() so interpolation can reuse a source frame.
        void releaseBefore(std::uint64_t generation, std::int64_t next) noexcept {
            if (generation == 0 || latest.load(std::memory_order_acquire) != generation) return;
            Frame frame;
            for (std::size_t n = 0; n < ringFrames && ring.peek(0, frame); ++n) {
                if (frame.generation == generation && frame.position >= next) break;
                ring.consume(1);
            }
        }
        static constexpr std::size_t sampleStorageBytes = ringFrames * sizeof(Frame);
    private:
        friend class DiskAudioStreamService;
        struct Request { const DiskAudioFile* source{}; std::uint64_t generation{}; std::int64_t startFrame{}; };
        // Deterministic mode (offline rendering): the thread that plays the voice also reads for it. The ring
        // already holds everything a sample needs when sample() looks, so the result cannot depend on timing.
        void serveHere(const Request& request) noexcept {
            current = request;
            latest.store(request.generation, std::memory_order_release);
            readyGeneration.store(0, std::memory_order_release);
            reader.reset();
            position = request.startFrame;
            buffered = 0;
            if (!request.source) return;
            if (request.startFrame < 0 || request.startFrame >= request.source->frames) {
                failedGeneration.store(request.generation, std::memory_order_release);
                return;
            }
            try { reader = request.source->open(); } catch (...) { reader.reset(); }
            if (!reader || !fillHere(static_cast<std::int64_t>(prebufferFrames))) {
                failedGeneration.store(request.generation, std::memory_order_release);
                reader.reset();
                return;
            }
            readyGeneration.store(request.generation, std::memory_order_release);
        }
        // Reads until `wanted` frames lie ahead of the read position or the file ends. False if a read failed.
        bool fillHere(std::int64_t wanted) noexcept {
            const auto* source = current.source;
            const std::int64_t stop = std::min(source->frames, position + wanted);
            while (position < stop) {
                const auto room = ring.writable();
                if (room == 0) return true;
                const int count = static_cast<int>(std::min<std::uint64_t>(
                    {room, 1024, static_cast<std::uint64_t>(stop - position)}));
                std::array<float, 1024> left{}, right{};
                try {
                    if (!reader->read(position, left.data(), source->channels == 2 ? right.data() : nullptr, count)) return false;
                } catch (...) { return false; }
                std::array<Frame, 1024> block{};
                for (int i = 0; i < count; ++i) {
                    const float l = std::isfinite(left[static_cast<std::size_t>(i)]) ? left[static_cast<std::size_t>(i)] : 0.0f;
                    const float r = source->channels == 2
                        ? (std::isfinite(right[static_cast<std::size_t>(i)]) ? right[static_cast<std::size_t>(i)] : 0.0f) : l;
                    block[static_cast<std::size_t>(i)] = {l, r, current.generation, position + i};
                }
                const auto published = ring.write(std::span<const Frame>(block.data(), static_cast<std::size_t>(count)));
                position += static_cast<std::int64_t>(published);
                buffered += published;
                if (published == 0) return true;
            }
            return true;
        }
        bool readAhead(std::uint64_t generation, std::int64_t first, std::int64_t second) noexcept {
            if (current.generation != generation || !current.source || !reader) return false;
            releaseBefore(generation, first);
            if (position <= second || static_cast<std::int64_t>(ring.available()) < static_cast<std::int64_t>(prebufferFrames)) {
                if (!fillHere(second + 1 - position + static_cast<std::int64_t>(prebufferFrames))) {
                    failedGeneration.store(generation, std::memory_order_release);
                    reader.reset();
                    return false;
                }
            }
            return true;
        }
        bool consumeRequest(Request& request) noexcept {
            if ((middle.load(std::memory_order_acquire) & dirty) == 0) return false;
            front = middle.exchange(front, std::memory_order_acq_rel) & 3;
            request = buffers[front];
            return true;
        }
        StreamingAudioRing<ringFrames, Frame> ring;
        static constexpr unsigned dirty = 4;
        std::array<Request, 3> buffers{};
        std::atomic<unsigned> middle{1};
        unsigned back{2}, front{};
        std::uint64_t nextGeneration{}; // Audio side only.
        std::atomic<std::uint64_t> latest{}, readyGeneration{}, failedGeneration{};
        Request current; // Worker only from here down.
        std::unique_ptr<DiskAudioReader> reader;
        std::int64_t position{};
        std::size_t buffered{};
        std::atomic<bool> deterministic{};   // see DiskAudioStreamService::setDeterministic
    };

    DiskAudioStreamService() : voices(std::make_unique<std::array<Voice, voiceCount>>()),
        worker([this] { run(); }) {}
    ~DiskAudioStreamService() {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
        // Release readers before their source metadata, after the worker exits.
        for (auto& voice : *voices) voice.reader.reset();
    }
    DiskAudioStreamService(const DiskAudioStreamService&) = delete;
    DiskAudioStreamService& operator=(const DiskAudioStreamService&) = delete;
    // Control thread only, before publishing this source to any endpoint.
    const DiskAudioFile* retain(std::shared_ptr<const DiskAudioFile> source) {
        if (!source || !source->valid()) return nullptr;
        const auto* result = source.get();
        retained.push_back(std::move(source));
        return result;
    }
    Voice* voice(std::size_t index) noexcept {
        return index < voiceCount ? &(*voices)[index] : nullptr;
    }
    // Offline rendering: serve every voice on the thread that plays it, so a render is the same
    // however the machine happens to schedule threads. For a service that has not played anything
    // yet (a render's own engine); the worker then leaves these voices alone for good.
    void setDeterministic() noexcept {
        for (auto& voice : *voices) voice.deterministic.store(true, std::memory_order_release);
    }
    static constexpr std::size_t totalSampleStorageBytes = voiceCount * Voice::sampleStorageBytes;
private:
    bool step(Voice& voice) {
        if (voice.deterministic.load(std::memory_order_acquire)) return false;   // served by the thread that plays it
        Voice::Request request;
        if (voice.consumeRequest(request)) {
            voice.current = request;
            voice.readyGeneration.store(0, std::memory_order_release);
            voice.reader.reset();
            voice.position = request.startFrame;
            voice.buffered = 0;
            if (request.source) {
                if (request.startFrame < 0 || request.startFrame >= request.source->frames) {
                    voice.failedGeneration.store(request.generation, std::memory_order_release);
                    return false;
                }
                voice.reader = request.source->open();
                if (!voice.reader) voice.failedGeneration.store(request.generation, std::memory_order_release);
            }
        }
        const auto* source = voice.current.source;
        const auto generation = voice.current.generation;
        if (!source || !voice.reader || voice.failed(generation)
            || voice.latest.load(std::memory_order_acquire) != generation
            || voice.position >= source->frames) return false;
        const auto free = voice.ring.writable();
        const int count = static_cast<int>(std::min<std::uint64_t>(
            {free, 1024, static_cast<std::uint64_t>(source->frames - voice.position)}));
        if (count == 0) return false;
        std::array<float, 1024> left{}, right{};
        if (!voice.reader->read(voice.position, left.data(), source->channels == 2 ? right.data() : nullptr, count)) {
            voice.failedGeneration.store(generation, std::memory_order_release);
            voice.reader.reset();
            return false;
        }
        // A request arriving during decoding invalidates the result before publication.
        if (voice.latest.load(std::memory_order_acquire) != generation) return true;
        std::array<Frame, 1024> block{};
        for (int i = 0; i < count; ++i) {
            const float l = std::isfinite(left[i]) ? left[i] : 0.0f;
            const float r = source->channels == 2 ? (std::isfinite(right[i]) ? right[i] : 0.0f) : l;
            block[static_cast<std::size_t>(i)] = {l, r, generation, voice.position + i};
        }
        const auto published = voice.ring.write(std::span<const Frame>(block.data(), static_cast<std::size_t>(count)));
        voice.position += static_cast<std::int64_t>(published);
        voice.buffered += published;
        if (voice.buffered >= prebufferFrames || voice.position == source->frames)
            voice.readyGeneration.store(generation, std::memory_order_release);
        return published != 0;
    }
    void run() noexcept {
        while (!stopping.load(std::memory_order_acquire)) {
            bool work = false;
            for (auto& voice : *voices) {
                if (stopping.load(std::memory_order_acquire)) return;
                try { work = step(voice) || work; }
                catch (...) {
                    voice.failedGeneration.store(voice.current.generation, std::memory_order_release);
                    voice.reader.reset();
                }
            }
            if (!work) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Destruction order: thread, retained sources, voices (worker joined first).
    std::unique_ptr<std::array<Voice, voiceCount>> voices;
    std::vector<std::shared_ptr<const DiskAudioFile>> retained;
    std::atomic<bool> stopping{};
    std::thread worker;
};
} // namespace alien
