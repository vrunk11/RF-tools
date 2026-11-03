/******************************************************************************
 * order.cpp
 * rfx library - the order to code the captures in
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "order.h"

#include "error.h"
#include "parallel.h"
#include "posfile.h"
#include "predictor.h"
#include "segment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace rfx {

namespace {

// Frames two captures must share to count as the same programme.
constexpr size_t kMinShared = 50;
// Windows measured per capture, and their length.
constexpr size_t kWindows = 8;
constexpr size_t kWindowLength = 262144;
// The windows lie between these fractions (in fifths) of the shared frames.
constexpr size_t kFirstFifth = 1;
constexpr size_t kLastFifth = 4;
constexpr int kNoiseLevel = 3;
constexpr int kMaxShift = 15;
constexpr double kMinRms = 1e-9;

// The key of a field: its frame code and whether it is the first of the frame.
std::string keyOf(const Field &f)
{
    return *f.code + (f.first ? '1' : '0');
}

// The first field of each key.
std::unordered_map<std::string, size_t> firstByKey(const std::vector<Field> &fields)
{
    std::unordered_map<std::string, size_t> m;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].code) {
            m.emplace(keyOf(fields[i]), i);
        }
    }
    return m;
}

int trailingZeros(uint32_t v)
{
    if (v == 0) {
        return 32;
    }
    int n = 0;
    while ((v & 1u) == 0) {
        v >>= 1;
        ++n;
    }
    return n;
}

// The cost of the windows at frames all the captures share (taken evenly between
// 20% and 80% of the first capture's frames). Throws rfx::Error if they cannot be
// compared.
std::vector<NoiseScore> scores(const std::vector<QString> &inputs, const std::vector<SampleFormat> &fmts,
                               const std::vector<const std::vector<Field> *> &fields, unsigned threads)
{
    std::vector<std::unordered_map<std::string, size_t>> maps;
    for (const auto *f : fields) {
        maps.push_back(firstByKey(*f));
    }
    std::vector<std::pair<double, std::string>> common;
    for (const auto &kv : maps[0]) {
        const bool inAll = std::all_of(maps.begin(), maps.end(), [&](const auto &m) { return m.count(kv.first) != 0; });
        if (inAll) {
            const std::vector<double> &lines = (*fields[0])[kv.second].lines;
            common.emplace_back(lines.empty() ? 0.0 : lines.front(), kv.first);
        }
    }
    std::stable_sort(common.begin(), common.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (common.size() < kWindows * 4) {
        throw Error("the captures share only " + std::to_string(common.size()) + " frames: not enough to compare their noise");
    }
    const size_t lo = common.size() * kFirstFifth / 5;
    const size_t hi = common.size() * kLastFifth / 5;
    std::vector<std::string> picks;
    for (size_t w = 0; w < kWindows; ++w) {
        picks.push_back(common[lo + (hi - lo) * w / kWindows].second);
    }
    Preset pre;
    Preset::level(kNoiseLevel, pre);
    std::vector<std::unique_ptr<PosFile>> files;
    for (const QString &p : inputs) {
        files.push_back(std::make_unique<PosFile>(p, PosFile::Mode::Read));
    }
    struct Measure
    {
        bool ok = false;
        double bits = 0;
        double rms = 0;
    };
    std::vector<Measure> measured(inputs.size() * kWindows);
    parallelFor(measured.size(), threads, [&](size_t job) {
        const size_t i = job / kWindows;
        const size_t w = job % kWindows;
        const uint64_t words = formatSamples(fmts[i], files[i]->size());
        const Field &f = (*fields[i])[maps[i][picks[w]]];
        if (f.lines.empty()) {
            return;
        }
        const int64_t start = static_cast<int64_t>(f.lines.front());
        if (start < 0 || static_cast<uint64_t>(start) + kWindowLength > words) {
            return;
        }
        std::vector<int32_t> probe(kWindowLength);
        loadSamples(fmts[i], *files[i], words, start, probe.data(), probe.size());
        uint32_t orAll = 0;
        for (const int32_t v : probe) {
            orAll |= static_cast<uint32_t>(v);
        }
        const int shift = std::min(trailingZeros(orAll), kMaxShift);
        std::vector<int32_t> y(probe.size());
        int64_t sum = 0;
        for (size_t k = 0; k < y.size(); ++k) {
            y[k] = probe[k] >> shift;
            sum += y[k];
        }
        const int32_t mean = static_cast<int32_t>(std::llround(static_cast<double>(sum) / static_cast<double>(kWindowLength)));
        double squares = 0;
        for (int32_t &v : y) {
            v -= mean;
            squares += static_cast<double>(v) * static_cast<double>(v);
        }
        const double rms = std::sqrt(squares / static_cast<double>(kWindowLength));
        const std::vector<int32_t> residuals = predictionErrors(y, pre, false, nullptr, 0);
        const double bits = static_cast<double>(encodeResiduals(residuals).size()) * 8.0 / static_cast<double>(kWindowLength);
        measured[job] = {true, bits, rms};
    });
    std::vector<NoiseScore> out;
    for (size_t i = 0; i < inputs.size(); ++i) {
        double bits = 0;
        double rms = 0;
        size_t n = 0;
        for (size_t w = 0; w < kWindows; ++w) {
            const Measure &m = measured[i * kWindows + w];
            if (m.ok) {
                bits += m.bits;
                rms += m.rms;
                ++n;
            }
        }
        if (n == 0) {
            throw Error(inputs[i].toStdString() + ": no window inside the file");
        }
        out.push_back({bits / static_cast<double>(n), rms / static_cast<double>(n)});
    }
    return out;
}

size_t findRoot(std::vector<size_t> &g, size_t i)
{
    if (g[i] != i) {
        const size_t r = findRoot(g, g[i]);
        g[i] = r;
    }
    return g[i];
}

} // namespace

double NoiseScore::cost() const
{
    return bits - std::log2(std::max(rms, kMinRms));
}

Plan planOrder(const std::vector<QString> &inputs, const std::vector<SampleFormat> &fmts,
               const std::vector<std::vector<Field>> &fields, unsigned threads)
{
    const size_t n = inputs.size();
    std::vector<std::unordered_map<std::string, size_t>> maps;
    for (const auto &f : fields) {
        maps.push_back(firstByKey(f));
    }
    std::vector<size_t> group(n);
    for (size_t i = 0; i < n; ++i) {
        group[i] = i;
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            size_t shared = 0;
            for (const auto &kv : maps[i]) {
                shared += maps[j].count(kv.first);
            }
            if (shared >= kMinShared) {
                const size_t a = findRoot(group, i);
                const size_t b = findRoot(group, j);
                group[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    std::vector<std::vector<size_t>> members;
    for (size_t i = 0; i < n; ++i) {
        const size_t root = findRoot(group, i);
        if (root == i) {
            members.push_back({i});
        } else {
            for (auto &m : members) {
                if (m[0] == root) {
                    m.push_back(i);
                    break;
                }
            }
        }
    }
    Plan plan;
    plan.noise.assign(n, std::nullopt);
    for (const auto &m : members) {
        if (m.size() > 1) {
            std::vector<QString> subInputs;
            std::vector<const std::vector<Field> *> subFields;
            std::vector<SampleFormat> subFmts;
            for (const size_t i : m) {
                subInputs.push_back(inputs[i]);
                subFields.push_back(&fields[i]);
                subFmts.push_back(fmts[i]);
            }
            try {
                const std::vector<NoiseScore> sc = scores(subInputs, subFmts, subFields, threads);
                std::vector<std::pair<size_t, double>> ranked;
                for (size_t k = 0; k < m.size(); ++k) {
                    ranked.emplace_back(m[k], sc[k].cost());
                    plan.noise[m[k]] = sc[k].cost();
                }
                std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
                for (const auto &r : ranked) {
                    plan.order.push_back(r.first);
                }
                continue;
            } catch (const Error &e) {
                std::fprintf(stderr, "order: keeping the given order for %zu captures: %s\n", m.size(), e.what());
            }
        }
        plan.order.insert(plan.order.end(), m.begin(), m.end());
    }
    return plan;
}

} // namespace rfx
