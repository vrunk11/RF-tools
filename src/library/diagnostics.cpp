/******************************************************************************
 * diagnostics.cpp
 * rfx library - the commands that tell how the captures and the predictor behave
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "diagnostics.h"

#include "alignment.h"
#include "capture.h"
#include "error.h"
#include "gpupredictor.h"
#include "order.h"
#include "packer_internal.h"
#include "parallel.h"
#include "predictor.h"
#include "refwindow.h"
#include "sampleformat.h"
#include "segment.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>

namespace rfx {

namespace {

// Where the first window of the reference choice and the last one lie, in tenths
// of the capture, and the numbers of references tried together.
constexpr size_t kFirstTenth = 1;
constexpr size_t kSpreadTenths = 8;
constexpr size_t kTopCounts[] = {2, 3, 5, 8};
constexpr float kSteepness[] = {2.0f, 4.0f, 8.0f};
// A synthetic reference window follows the samples, shifted by this many samples
// (and by 3 more for each channel).
constexpr size_t kSyntheticShift = 5;
constexpr size_t kSyntheticChannelShift = 3;
// A solve that never happens, and an update that never happens after the first.
constexpr size_t kNever = size_t(1) << 30;

double seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

Preset levelOrThrow(int level)
{
    Preset pre;
    if (!Preset::level(level, pre)) {
        throw Error("level 1 to 4");
    }
    return pre;
}

// Samples [s0, s0 + n) of a 16-bit capture, shifted and with the mean taken off.
std::vector<int32_t> shifted(PosFile &file, SampleFormat fmt, uint64_t words, const CaptureStats &st, int64_t s0, size_t n)
{
    std::vector<int32_t> y(n);
    loadSamples(fmt, file, words, s0, y.data(), n);
    for (int32_t &v : y) {
        v = (v >> st.shift) - st.mean;
    }
    return y;
}

// Cost in bits per sample of the samples `y` with the window `window` (one channel).
double costWith(const std::vector<int32_t> &y, const Preset &pre, const std::vector<float> &window)
{
    const std::vector<int32_t> residuals = predictionErrors(y, pre, true, window.data(), window.size());
    return static_cast<double>(encodeResiduals(residuals).size()) * 8.0 / static_cast<double>(y.size());
}

std::vector<int32_t> predictionsOf(const std::vector<int32_t> &y, const Preset &pre, bool useRef, const float *window, size_t stride)
{
    Predictor predictor(pre, useRef);
    std::vector<int32_t> out(y.size());
    for (size_t i = 0; i < y.size(); ++i) {
        out[i] = predictor.predict(window, stride, i);
        predictor.update(i, y[i]);
    }
    return out;
}

} // namespace

void printNoise(const std::vector<QString> &inputs, const QString &pass0Dir, size_t windows, size_t windowLength, unsigned threads)
{
    std::vector<std::vector<Field>> fields;
    for (size_t i = 0; i < inputs.size(); ++i) {
        fields.push_back(parsePass0(QDir(pass0Dir).filePath(QStringLiteral("pass0_%1.txt").arg(static_cast<qulonglong>(i)))));
    }
    std::vector<SampleFormat> fmts;
    for (const QString &p : inputs) {
        SampleFormat f = SampleFormat::S16;
        parseFormat(QFileInfo(p).suffix().toLower().toStdString(), f);
        fmts.push_back(f);
    }
    const std::vector<NoiseScore> scores = measureNoise(inputs, fmts, fields, windows, windowLength, threads == 0 ? defaultThreads() : threads);
    std::printf("%-34s %9s %8s %8s %7s %4s\n", "capture", "bits/smp", "min", "max", "rms", "win");
    for (size_t i = 0; i < inputs.size(); ++i) {
        QString name = QFileInfo(inputs[i]).fileName();
        const QString suffix = QStringLiteral(".ddd.s16");
        while (name.endsWith(suffix)) {
            name.chop(suffix.size());
        }
        const NoiseScore &s = scores[i];
        std::printf("%-34s %9.3f %8.3f %8.3f %7.1f %4zu\n", name.toStdString().c_str(), s.bits, s.lowest, s.highest, s.rms, s.windows);
    }
}

void printReferenceChoice(const std::vector<QString> &inputs, const PackOptions &options, size_t k, size_t nseg)
{
    PackOptions o = options;
    o.order = OrderMode::Given;
    const std::unique_ptr<PreparedPack> prepared = prepareCaptures(inputs, std::vector<PackSpec>(inputs.size()), {}, QStringLiteral("refsel.rfx"), o);
    const ArchiveMeta &meta = prepared->meta;
    if (k >= meta.sources.size()) {
        throw Error("the target is the place of a capture: 0 to " + std::to_string(meta.sources.size() - 1));
    }
    const ReferenceWindows windows(prepared->files, meta, prepared->tables);
    const SourceMeta &sm = meta.sources[k];
    Preset pre = sm.preset();
    pre.ntop = 0;
    const size_t s = meta.segment;
    const size_t total = static_cast<size_t>((sm.words + s - 1) / s);
    const size_t nr = sm.refs.size();
    if (nr < 2) {
        throw Error(sm.name + " has " + std::to_string(nr) + " reference(s): nothing to choose from");
    }
    std::vector<size_t> segs;
    for (size_t i = 0; i < nseg; ++i) {
        segs.push_back(total * kFirstTenth / 10 + (total * kSpreadTenths / 10) * i / std::max<size_t>(nseg, 1));
    }
    const unsigned threads = o.threads != 0 ? o.threads : defaultThreads();
    // [segment][configuration] costs: all, then each reference, then the best few,
    // then all of them weighted by how well each does alone.
    const size_t configs = 1 + nr + std::size(kTopCounts) + std::size(kSteepness);
    std::vector<std::vector<double>> rows(segs.size());
    parallelFor(segs.size(), threads, [&](size_t row) {
        const size_t s0 = segs[row] * s;
        const size_t n = std::min<size_t>(s, static_cast<size_t>(sm.words) - s0);
        const std::vector<int32_t> y = shifted(*prepared->files[k], sm.format(), sm.words, {sm.size, "", sm.shift, sm.mean, ""}, static_cast<int64_t>(s0), n);
        std::vector<std::pair<size_t, float>> every;
        for (const size_t j : sm.refs) {
            every.emplace_back(j, 1.0f / static_cast<float>(nr));
        }
        std::vector<double> costs;
        costs.push_back(costWith(y, pre, windows.windowWith(k, static_cast<int64_t>(s0), n, every)));
        std::vector<double> single;
        for (const size_t j : sm.refs) {
            single.push_back(costWith(y, pre, windows.windowWith(k, static_cast<int64_t>(s0), n, {{j, 1.0f}})));
        }
        costs.insert(costs.end(), single.begin(), single.end());
        std::vector<size_t> rank(nr);
        for (size_t r = 0; r < nr; ++r) {
            rank[r] = r;
        }
        std::stable_sort(rank.begin(), rank.end(), [&](size_t a, size_t b) { return single[a] < single[b]; });
        for (const size_t top : kTopCounts) {
            const size_t m = std::min(top, nr);
            std::vector<std::pair<size_t, float>> which;
            for (size_t r = 0; r < m; ++r) {
                which.emplace_back(sm.refs[rank[r]], 1.0f / static_cast<float>(m));
            }
            costs.push_back(costWith(y, pre, windows.windowWith(k, static_cast<int64_t>(s0), n, which)));
        }
        for (const float e : kSteepness) {
            const double best = *std::min_element(single.begin(), single.end());
            std::vector<float> raw(nr);
            float sum = 0.0f;
            for (size_t r = 0; r < nr; ++r) {
                raw[r] = std::pow(2.0f, -e * static_cast<float>(single[r] - best));
                sum += raw[r];
            }
            std::vector<std::pair<size_t, float>> which;
            for (size_t r = 0; r < nr; ++r) {
                which.emplace_back(sm.refs[r], raw[r] / sum);
            }
            costs.push_back(costWith(y, pre, windows.windowWith(k, static_cast<int64_t>(s0), n, which)));
        }
        rows[row] = std::move(costs);
    });
    auto mean = [&](size_t c) {
        double sum = 0;
        for (const auto &r : rows) {
            sum += r[c];
        }
        return sum / static_cast<double>(rows.size());
    };
    std::printf("target %s (%zu references), %zu segments of %zu samples\n", sm.name.c_str(), nr, segs.size(), s);
    std::printf("  mean of all references (current)      %.4f bits/sample\n", mean(0));
    for (size_t i = 0; i < nr; ++i) {
        std::printf("  reference %-28s alone %.4f\n", meta.sources[sm.refs[i]].name.c_str(), mean(1 + i));
    }
    for (size_t i = 0; i < std::size(kTopCounts); ++i) {
        std::printf("  best %-2zu of them, chosen per segment   %.4f\n", std::min(kTopCounts[i], nr), mean(1 + nr + i));
    }
    for (size_t i = 0; i < std::size(kSteepness); ++i) {
        std::printf("  all of them, weights 2^(-%s x excess cost)   %.4f\n", QString::number(static_cast<double>(kSteepness[i]), 'g', 6).toStdString().c_str(),
                    mean(1 + nr + std::size(kTopCounts) + i));
    }
    size_t bestConfig = 0;
    for (size_t c = 1; c < configs; ++c) {
        if (mean(c) < mean(bestConfig)) {
            bestConfig = c;
        }
    }
    double oracle = 0;
    for (const auto &r : rows) {
        oracle += *std::min_element(r.begin(), r.end());
    }
    oracle /= static_cast<double>(rows.size());
    std::printf("  best single configuration for all segments  %.4f (config %zu)\n", mean(bestConfig), bestConfig);
    std::printf("  best configuration segment by segment       %.4f  (%+.2f%% against the mean of all)\n", oracle, 100.0 * (oracle / mean(0) - 1.0));
}

void printSpeed(const QString &capture, int level, size_t nseg, size_t segment)
{
    const Preset pre = levelOrThrow(level);
    PosFile file(capture, PosFile::Mode::Read);
    const CaptureStats st = captureStats(file, SampleFormat::S16);
    const uint64_t words = st.size / 2;
    for (const bool useRef : {false, true}) {
        double tp = 0, te = 0, td = 0, tr = 0;
        size_t total = 0;
        for (size_t g = 0; g < nseg; ++g) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(segment, words > g * segment ? words - g * segment : 0));
            const std::vector<int32_t> y = shifted(file, SampleFormat::S16, words, st, static_cast<int64_t>(g * segment), n);
            std::vector<float> window(pre.windowLength(n));
            for (size_t i = 0; i < window.size(); ++i) {
                window[i] = static_cast<float>(y[std::min(i + kSyntheticShift, n - 1)]);
            }
            const size_t stride = window.size() / pre.channels();
            auto t = std::chrono::steady_clock::now();
            const std::vector<int32_t> residuals = predictionErrors(y, pre, useRef, window.data(), stride);
            tp += seconds(t);
            t = std::chrono::steady_clock::now();
            const std::vector<uint8_t> bytes = encodeResiduals(residuals);
            te += seconds(t);
            t = std::chrono::steady_clock::now();
            std::vector<int32_t> back;
            if (!decodeResiduals(bytes.data(), bytes.size(), n, back)) {
                throw Error("decode");
            }
            td += seconds(t);
            t = std::chrono::steady_clock::now();
            const std::vector<int32_t> again = reconstruct(back, pre, useRef, window.data(), stride);
            tr += seconds(t);
            if (again != y) {
                throw Error("the samples do not decode to themselves");
            }
            total += n;
        }
        std::printf("-- %s reference\n", useRef ? "with" : "without");
        const std::pair<const char *, double> stages[] = {{"predict (coding)", tp}, {"entropy encode", te}, {"entropy decode", td}, {"reconstruct (decoding)", tr}};
        for (const auto &stage : stages) {
            std::printf("%-28s %7.1f Msmp/s\n", stage.first, static_cast<double>(total) / stage.second / 1e6);
        }
    }

    // Where the predictor's time goes (single thread, with reference).
    std::vector<std::vector<int32_t>> ys;
    for (size_t g = 0; g < nseg; ++g) {
        ys.push_back(shifted(file, SampleFormat::S16, words, st, static_cast<int64_t>(g * segment), segment));
    }
    std::vector<float> window(pre.windowLength(segment));
    for (size_t i = 0; i < window.size(); ++i) {
        window[i] = static_cast<float>(ys[0][std::min(i + kSyntheticShift, segment - 1)]);
    }
    const size_t stride = window.size() / pre.channels();
    Preset noSolve = pre;
    noSolve.every = kNever;
    Preset noUpdate = noSolve;
    noUpdate.stride = kNever;
    const std::pair<const char *, Preset> variants[] = {{"full", pre}, {"no solve", noSolve}, {"no solve, no update", noUpdate}};
    for (const auto &v : variants) {
        const auto t = std::chrono::steady_clock::now();
        volatile int32_t sink = 0;
        for (const auto &y : ys) {
            sink = sink + predictionErrors(y, v.second, true, window.data(), stride).back();
        }
        const double dt = seconds(t);
        const double count = static_cast<double>(nseg * segment);
        std::printf("%-22s %7.1f Msmp/s  (%.0f ns/sample)\n", v.first, count / dt / 1e6, dt * 1e9 / count);
    }
}

void printGpuCheck(const QString &capture, int level, size_t nseg, size_t segment, bool withRef)
{
    const Preset pre = levelOrThrow(level);
    PosFile file(capture, PosFile::Mode::Read);
    const CaptureStats st = captureStats(file, SampleFormat::S16);
    const uint64_t words = st.size / 2;
    const size_t l = segment + 2 * pre.taps;
    const size_t rw = pre.channels() * l;
    RawArray<int32_t> input(nseg * segment);
    std::fill_n(input.data(), input.size(), 0);
    RawArray<float> refw(withRef ? nseg * rw : 0);
    if (withRef) {
        std::fill_n(refw.data(), refw.size(), 0.0f);
    }
    std::vector<int32_t> lens;
    std::vector<std::vector<int32_t>> cpu;
    for (size_t g = 0; g < nseg; ++g) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(segment, words > g * segment ? words - g * segment : 0));
        const std::vector<int32_t> y = shifted(file, SampleFormat::S16, words, st, static_cast<int64_t>(g * segment), n);
        std::copy(y.begin(), y.end(), input.data() + g * segment);
        lens.push_back(static_cast<int32_t>(n));
        const size_t ln = n + 2 * pre.taps;
        std::vector<float> win;
        if (withRef) {
            for (size_t c = 0; c < pre.channels(); ++c) {
                for (size_t i = 0; i < ln; ++i) {
                    const float v = static_cast<float>(y[std::min(i + kSyntheticShift + kSyntheticChannelShift * c, n - 1)]);
                    win.push_back(v);
                    refw[g * rw + c * l + i] = v;
                }
            }
        }
        cpu.push_back(predictionsOf(y, pre, withRef, win.data(), ln));
    }
    GpuPredictor gpu(pre, withRef, segment);
    std::unique_ptr<GpuChunkBuffers> buffers = gpu.newBuffers();
    // The first run builds the buffers and warms the device up; the second is timed.
    gpu.upload(*buffers, input, refw, lens);
    gpu.predict(*buffers, 0);
    (void) gpu.download(*buffers);
    auto t = std::chrono::steady_clock::now();
    gpu.upload(*buffers, input, refw, lens);
    const double up = seconds(t);
    t = std::chrono::steady_clock::now();
    gpu.predict(*buffers, 0);
    const double kernel = seconds(t);
    t = std::chrono::steady_clock::now();
    const RawArray<int32_t> out = gpu.download(*buffers);
    const double down = seconds(t);
    double total = 0;
    for (const int32_t n : lens) {
        total += n;
    }
    std::fprintf(stderr, "GPU: kernel %.1f Msmp/s (%.3f s), upload %.3f s, download %.3f s\n", total / kernel / 1e6, kernel, up, down);
    for (size_t g = 0; g < nseg; ++g) {
        const size_t n = static_cast<size_t>(lens[g]);
        size_t differing = 0;
        size_t first = 0;
        for (size_t i = 0; i < n; ++i) {
            if (cpu[g][i] != out[g * segment + i]) {
                if (differing++ == 0) {
                    first = i;
                }
            }
        }
        if (differing == 0) {
            std::printf("segment %zu: identical (%zu samples)\n", g, n);
        } else {
            std::printf("segment %zu: %zu of %zu samples differ, first at %zu (cpu %d gpu %d) ; solve due at %zu\n", g, differing, n, first,
                        cpu[g][first], out[g * segment + first], (first / pre.every) * pre.every + pre.every - 1);
        }
    }
}

} // namespace rfx
