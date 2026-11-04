/******************************************************************************
 * refwindow.h
 * rfx library - the reference window of a segment (specification 5.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "lagtable.h"
#include "metadata.h"
#include "posfile.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace rfx {

// The reference tracks of a track, as files of their original samples (the
// captures when packing, the files being unpacked when unpacking), or through a
// loader that gives the samples of a track where they are asked for (a reader that
// decodes only what it is asked).
class ReferenceWindows
{
public:
    // Fills `out` with the `n` canonical samples of track `track` from sample
    // `start` (zero outside the track).
    using Loader = std::function<void(size_t track, int64_t start, int32_t *out, size_t n)>;

    ReferenceWindows(const std::vector<PosFile *> &files, const ArchiveMeta &meta,
                     const std::map<std::pair<size_t, size_t>, LagTable> &tables)
        : m_files(&files), m_meta(meta), m_tables(tables)
    {
    }

    ReferenceWindows(Loader loader, const ArchiveMeta &meta, const std::map<std::pair<size_t, size_t>, LagTable> &tables)
        : m_files(nullptr), m_loader(std::move(loader)), m_meta(meta), m_tables(tables)
    {
    }

    // The window over the positions [s0 - taps, s0 + n + taps) of track `k`, the
    // channels one after the other, each n + 2 * taps long. Channel 0 is the
    // weighted sum of the references, added in the order of `refs`, each term a
    // product rounded and then added (no fused multiply-add); channel c is the
    // reference tops()[c - 1] alone.
    std::vector<float> window(size_t k, int64_t s0, size_t n) const;

    // The same into `out`, whose channels are `stride` apart (at least n + 2 * taps)
    // and which holds channels * stride values: what the GPU predictor reads.
    void windowInto(size_t k, int64_t s0, size_t n, size_t stride, float *out) const;

    // A window of one channel (n + 2 * taps long) over the references `which`
    // (track, weight) only, which may be any subset of the references with any
    // weights: to try references out.
    std::vector<float> windowWith(size_t k, int64_t s0, size_t n, const std::vector<std::pair<size_t, float>> &which) const;

private:
    void addRange(size_t j, int64_t lo, int64_t hi, float weight, float *dst) const;
    // Add `weight` times reference `j` of track `k` over the positions [p0, p0 + len) to `dst`.
    void addReference(size_t k, size_t j, float weight, int64_t p0, size_t len, float *dst) const;

    const std::vector<PosFile *> *m_files;
    Loader m_loader;
    const ArchiveMeta &m_meta;
    const std::map<std::pair<size_t, size_t>, LagTable> &m_tables;
};

} // namespace rfx
