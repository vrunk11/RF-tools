/******************************************************************************
 * gpupipeline.cpp
 * rfx library - the GPU path of pack and unpack
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "gpupipeline.h"

#include "error.h"
#include "gpupredictor.h"
#include "parallel.h"
#include "segment.h"

#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rfx {

namespace {

// Segments per chunk.
constexpr size_t kChunkSegments = 384;
// Sets of device buffers: one uploading, one running, one coming back.
constexpr size_t kBufferSets = 3;

// Seconds each stage has been busy (printed when RFX_TIMING is set): read and window,
// upload, kernel, download, code and write.
std::atomic<uint64_t> g_busyMicros[5];

class Busy
{
public:
    explicit Busy(int stage) : m_stage(stage), m_start(std::chrono::steady_clock::now()) {}
    ~Busy() { g_busyMicros[m_stage] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start).count()); }

private:
    int m_stage;
    std::chrono::steady_clock::time_point m_start;
};

void reportBusy(const char *what, std::chrono::steady_clock::time_point wall)
{
    if (qEnvironmentVariableIsEmpty("RFX_TIMING")) {
        return;
    }
    double s[5];
    for (int i = 0; i < 5; ++i) {
        s[i] = static_cast<double>(g_busyMicros[i].exchange(0)) / 1e6;
    }
    std::fprintf(stderr, "  %s: wall %.2f s, read+window %.2f, upload %.2f, kernel %.2f, download %.2f, code+write %.2f\n", what,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count(), s[0], s[1], s[2], s[3], s[4]);
}

// A queue of bounded length between two stages; closing it releases both sides.
template<typename T>
class Channel
{
public:
    explicit Channel(size_t capacity) : m_capacity(capacity) {}

    // False when the queue was closed (the item is dropped).
    bool push(T item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_items.size() < m_capacity || m_closed; });
        if (m_closed) {
            return false;
        }
        m_items.push_back(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    // Nothing when the queue is closed and empty.
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return !m_items.empty() || m_closed; });
        if (m_items.empty()) {
            return std::nullopt;
        }
        T item = std::move(m_items.front());
        m_items.pop_front();
        m_notFull.notify_one();
        return item;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    size_t m_capacity;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<T> m_items;
    bool m_closed = false;
};

// The host side of a chunk: its segments, real lengths, padded samples (or
// residuals) and reference windows.
struct Chunk
{
    std::vector<size_t> segments;
    std::vector<int32_t> lens;
    std::vector<int32_t> input;
    std::vector<float> window;
    // What the GPU returned.
    std::vector<int32_t> output;
    // The device buffers it holds from its upload to its download.
    std::unique_ptr<GpuChunkBuffers> buffers;
    std::exception_ptr error;
};

using ChunkPtr = std::unique_ptr<Chunk>;

// Build the chunks of the segments [first, first + count) with `fill(segment,
// input, window)` (the segment's padded samples and its window), hand each to the
// GPU thread, then give what comes back to `consume`.
void runPipeline(size_t nseg, size_t segment, size_t windowLength, bool useRef, uint64_t words, unsigned threads, GpuPredictor &gpu, int mode,
                 const std::function<void(size_t, int32_t *, float *)> &fill, const std::function<void(Chunk &)> &consume)
{
    Channel<ChunkPtr> built(1);
    Channel<ChunkPtr> predicted(1);
    std::thread producer([&] {
        for (size_t first = 0; first < nseg; first += kChunkSegments) {
            auto c = std::make_unique<Chunk>();
            try {
                const size_t count = std::min(kChunkSegments, nseg - first);
                for (size_t i = 0; i < count; ++i) {
                    c->segments.push_back(first + i);
                    c->lens.push_back(static_cast<int32_t>(std::min<uint64_t>(segment, words - (first + i) * segment)));
                }
                c->input.assign(count * segment, 0);
                if (useRef) {
                    c->window.assign(count * windowLength, 0.0f);
                }
                const Busy busy(0);
                parallelFor(count, threads, [&](size_t i) {
                    fill(first + i, &c->input[i * segment], useRef ? &c->window[i * windowLength] : nullptr);
                });
            } catch (...) {
                c->error = std::current_exception();
            }
            const bool failed = c->error != nullptr;
            if (!built.push(std::move(c)) || failed) {
                break;
            }
        }
        built.close();
    });
    // The device: upload, kernel and download are three threads with a queue
    // each, over three sets of device buffers that go round, so that a chunk can be
    // going up while another runs and a third comes back.
    Channel<ChunkPtr> uploaded(1);
    Channel<ChunkPtr> ran(1);
    Channel<std::unique_ptr<GpuChunkBuffers>> pool(kBufferSets);
    for (size_t i = 0; i < kBufferSets; ++i) {
        pool.push(gpu.newBuffers());
    }
    auto stage = [](Channel<ChunkPtr> &from, Channel<ChunkPtr> &to, const std::function<void(Chunk &)> &work) {
        while (std::optional<ChunkPtr> item = from.pop()) {
            ChunkPtr c = std::move(*item);
            if (!c->error) {
                try {
                    work(*c);
                } catch (...) {
                    c->error = std::current_exception();
                }
            }
            const bool failed = c->error != nullptr;
            if (!to.push(std::move(c)) || failed) {
                break;
            }
        }
        to.close();
    };
    std::thread uploader(stage, std::ref(built), std::ref(uploaded), [&](Chunk &c) {
        std::optional<std::unique_ptr<GpuChunkBuffers>> buffers = pool.pop();
        if (!buffers) {
            throw Error("the device buffers are gone");
        }
        c.buffers = std::move(*buffers);
        const Busy busy(1);
        gpu.upload(*c.buffers, c.input, c.window, c.lens);
    });
    std::thread runner(stage, std::ref(uploaded), std::ref(ran), [&](Chunk &c) {
        const Busy busy(2);
        gpu.predict(*c.buffers, mode);
    });
    std::thread downloader(stage, std::ref(ran), std::ref(predicted), [&](Chunk &c) {
        const Busy busy(3);
        c.output = gpu.download(*c.buffers);
        pool.push(std::move(c.buffers));
    });
    std::exception_ptr failure;
    try {
        while (std::optional<ChunkPtr> item = predicted.pop()) {
            if ((*item)->error) {
                std::rethrow_exception((*item)->error);
            }
            const Busy busy(4);
            consume(**item);
        }
    } catch (...) {
        failure = std::current_exception();
    }
    built.close();
    uploaded.close();
    ran.close();
    predicted.close();
    pool.close();
    producer.join();
    uploader.join();
    runner.join();
    downloader.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace

uint64_t gpuPackTrack(size_t k, const ArchiveMeta &meta, const std::vector<PosFile *> &files, const ReferenceWindows &windows, bool verify,
                      unsigned threads, const std::function<void(uint64_t, const std::vector<uint8_t> &)> &sink)
{
    const SourceMeta &sm = meta.sources[k];
    const Preset pre = sm.preset();
    const bool useRef = !sm.refs.empty();
    const size_t s = meta.segment;
    const size_t nseg = static_cast<size_t>((sm.words + s - 1) / s);
    GpuPredictor predictor(pre, useRef, s);
    std::unique_ptr<GpuPredictor> verifier;
    if (verify) {
        verifier = std::make_unique<GpuPredictor>(pre, useRef, s);
    }
    const size_t stride = predictor.windowStride();
    uint64_t packed = 0;
    auto fill = [&](size_t q, int32_t *input, float *window) {
        const size_t s0 = q * s;
        const size_t n = std::min<size_t>(s, static_cast<size_t>(sm.words) - s0);
        loadSamples(sm.format(), *files[k], sm.words, static_cast<int64_t>(s0), input, n);
        for (size_t i = 0; i < n; ++i) {
            input[i] = (input[i] >> sm.shift) - sm.mean;
        }
        if (useRef) {
            windows.windowInto(k, static_cast<int64_t>(s0), n, stride, window);
        }
    };
    auto consume = [&](Chunk &c) {
        const size_t b = c.segments.size();
        std::vector<std::vector<int32_t>> residuals(b);
        std::vector<std::vector<uint8_t>> coded(b);
        parallelFor(b, threads, [&](size_t i) {
            const size_t n = static_cast<size_t>(c.lens[i]);
            residuals[i].resize(n);
            for (size_t j = 0; j < n; ++j) {
                residuals[i][j] = static_cast<int32_t>(static_cast<uint32_t>(c.input[i * s + j]) - static_cast<uint32_t>(c.output[i * s + j]));
            }
            coded[i] = encodeResiduals(residuals[i]);
        });
        if (verifier) {
            std::vector<int32_t> flat(b * s, 0);
            parallelFor(b, threads, [&](size_t i) {
                std::vector<int32_t> back;
                if (!decodeResiduals(coded[i].data(), coded[i].size(), static_cast<size_t>(c.lens[i]), back) || back != residuals[i]) {
                    throw Error("self-check failed: segment " + std::to_string(c.segments[i]) + " of " + sm.name + " does not decode to its residuals");
                }
                std::copy(back.begin(), back.end(), flat.begin() + static_cast<std::ptrdiff_t>(i * s));
            });
            const std::vector<int32_t> again = verifier->run(1, flat, c.window, c.lens);
            for (size_t i = 0; i < b; ++i) {
                const size_t n = static_cast<size_t>(c.lens[i]);
                if (!std::equal(again.begin() + static_cast<std::ptrdiff_t>(i * s), again.begin() + static_cast<std::ptrdiff_t>(i * s + n), c.input.begin() + static_cast<std::ptrdiff_t>(i * s))) {
                    throw Error("self-check failed: segment " + std::to_string(c.segments[i]) + " of " + sm.name + " does not decode to itself on the GPU");
                }
            }
        }
        for (size_t i = 0; i < b; ++i) {
            packed += coded[i].size();
            sink(c.segments[i], coded[i]);
        }
    };
    const auto wall = std::chrono::steady_clock::now();
    runPipeline(nseg, s, predictor.windowLength(), useRef, sm.words, threads, predictor, 0, fill, consume);
    reportBusy("pack", wall);
    return packed;
}

void gpuUnpackTrack(size_t k, Archive &archive, const ReferenceWindows &windows, PosFile &out, unsigned threads)
{
    const ArchiveMeta &meta = archive.meta();
    const SourceMeta &sm = meta.sources[k];
    const Preset pre = sm.preset();
    const bool useRef = !sm.refs.empty();
    const size_t s = meta.segment;
    const size_t nseg = static_cast<size_t>((sm.words + s - 1) / s);
    GpuPredictor predictor(pre, useRef, s);
    const size_t stride = predictor.windowStride();
    const auto &blocks = archive.blocks(k);
    auto fill = [&](size_t q, int32_t *input, float *window) {
        const size_t s0 = q * s;
        const size_t n = std::min<size_t>(s, static_cast<size_t>(sm.words) - s0);
        const auto block = blocks.find(q);
        if (block == blocks.end()) {
            throw Error(sm.name + ": segment " + std::to_string(q) + " is missing");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(block->second.length));
        if (!bytes.empty()) {
            archive.file().readAt(bytes.data(), bytes.size(), block->second.offset);
        }
        std::vector<int32_t> residuals;
        if (!decodeResiduals(bytes.data(), bytes.size(), n, residuals)) {
            throw Error(sm.name + ": segment " + std::to_string(q) + " is corrupt");
        }
        std::copy(residuals.begin(), residuals.end(), input);
        if (useRef) {
            windows.windowInto(k, static_cast<int64_t>(s0), n, stride, window);
        }
    };
    auto consume = [&](Chunk &c) {
        parallelFor(c.segments.size(), threads, [&](size_t i) {
            const size_t n = static_cast<size_t>(c.lens[i]);
            std::vector<int32_t> vals(n);
            for (size_t j = 0; j < n; ++j) {
                vals[j] = static_cast<int32_t>((static_cast<uint32_t>(c.output[i * s + j]) + static_cast<uint32_t>(sm.mean)) << sm.shift);
            }
            storeSamples(sm.format(), out, static_cast<uint64_t>(c.segments[i]) * s, vals.data(), n);
        });
    };
    const auto wall = std::chrono::steady_clock::now();
    runPipeline(nseg, s, predictor.windowLength(), useRef, sm.words, threads, predictor, 1, fill, consume);
    reportBusy("unpack", wall);
}

} // namespace rfx
