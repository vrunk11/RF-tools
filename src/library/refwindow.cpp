/******************************************************************************
 * refwindow.cpp
 * rfx library - the reference window of a segment (specification 5.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "refwindow.h"

#include "error.h"

#include <algorithm>

namespace rfx {

namespace {

// A run of constant lag over the positions [from, to).
struct Run
{
    int64_t from;
    int64_t to;
    int64_t lag;
};

// The runs of constant lag covering the positions [p0, p1).
std::vector<Run> runs(const LagTable &t, int64_t p0, int64_t p1)
{
    std::vector<Run> out;
    int64_t p = p0;
    if (p < 0) {
        const int64_t e = std::min<int64_t>(p1, 0);
        out.push_back({p, e, t.lag[0]});
        p = e;
    }
    while (p < p1) {
        const auto it = std::upper_bound(t.pos.begin(), t.pos.end(), static_cast<uint64_t>(p));
        const size_t i = it == t.pos.begin() ? 0 : static_cast<size_t>(it - t.pos.begin()) - 1;
        const int64_t end = i + 1 < t.pos.size() ? std::min<int64_t>(static_cast<int64_t>(t.pos[i + 1]), p1) : p1;
        out.push_back({p, end, t.lag[i]});
        p = end;
    }
    return out;
}

} // namespace

void ReferenceWindows::addRange(size_t j, int64_t lo, int64_t hi, float weight, float *dst) const
{
    const SourceMeta &rj = m_meta.sources[j];
    std::vector<int32_t> buf(static_cast<size_t>(hi - lo));
    loadSamples(rj.format(), *m_files[j], rj.words, lo, buf.data(), buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        const int32_t v = (buf[i] >> rj.shift) - rj.mean;
        const float term = weight * static_cast<float>(v);
        dst[i] += term;
    }
}

void ReferenceWindows::addReference(size_t k, size_t j, float weight, int64_t p0, size_t len, float *dst) const
{
    const SourceMeta &rj = m_meta.sources[j];
    const auto table = m_tables.find({k, j});
    if (table == m_tables.end()) {
        throw Error("the lag table of track " + std::to_string(k) + " against track " + std::to_string(j) + " is missing");
    }
    for (const Run &run : runs(table->second, p0, p0 + static_cast<int64_t>(len))) {
        const int64_t lo = std::max<int64_t>(run.from + run.lag, 0);
        const int64_t hi = std::min<int64_t>(run.to + run.lag, static_cast<int64_t>(rj.words));
        if (hi <= lo) {
            continue;
        }
        const int64_t base = lo - run.lag - p0;
        addRange(j, lo, hi, weight, dst + base);
    }
}

std::vector<float> ReferenceWindows::window(size_t k, int64_t s0, size_t n) const
{
    const SourceMeta &sm = m_meta.sources[k];
    const Preset pre = sm.preset();
    const size_t len = n + 2 * sm.taps;
    std::vector<float> out(pre.channels() * len, 0.0f);
    const int64_t p0 = s0 - static_cast<int64_t>(sm.taps);
    const std::vector<size_t> tops = sm.tops();
    const float equal = 1.0f / static_cast<float>(std::max<size_t>(sm.refs.size(), 1));
    for (size_t idx = 0; idx < sm.refs.size(); ++idx) {
        const float w = sm.weights.size() == sm.refs.size() ? sm.weights[idx] : equal;
        addReference(k, sm.refs[idx], w, p0, len, out.data());
    }
    for (size_t c = 0; c < tops.size(); ++c) {
        addReference(k, tops[c], 1.0f, p0, len, out.data() + (c + 1) * len);
    }
    return out;
}

std::vector<float> ReferenceWindows::windowWith(size_t k, int64_t s0, size_t n, const std::vector<std::pair<size_t, float>> &which) const
{
    const SourceMeta &sm = m_meta.sources[k];
    const size_t len = n + 2 * sm.taps;
    std::vector<float> out(len, 0.0f);
    const int64_t p0 = s0 - static_cast<int64_t>(sm.taps);
    for (const auto &r : which) {
        addReference(k, r.first, r.second, p0, len, out.data());
    }
    return out;
}

} // namespace rfx
