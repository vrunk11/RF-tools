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
#include <map>
#include <utility>
#include <vector>

namespace rfx {

// The reference tracks of a track, as files of their original samples (the
// captures when packing, the files being unpacked when unpacking).
class ReferenceWindows
{
public:
    ReferenceWindows(const std::vector<PosFile *> &files, const ArchiveMeta &meta,
                     const std::map<std::pair<size_t, size_t>, LagTable> &tables)
        : m_files(files), m_meta(meta), m_tables(tables)
    {
    }

    // The window over the positions [s0 - taps, s0 + n + taps) of track `k`, the
    // channels one after the other, each n + 2 * taps long. Channel 0 is the
    // weighted sum of the references, added in the order of `refs`, each term a
    // product rounded and then added (no fused multiply-add); channel c is the
    // reference tops()[c - 1] alone.
    std::vector<float> window(size_t k, int64_t s0, size_t n) const;

    // A window of one channel (n + 2 * taps long) over the references `which`
    // (track, weight) only, which may be any subset of the references with any
    // weights: to try references out.
    std::vector<float> windowWith(size_t k, int64_t s0, size_t n, const std::vector<std::pair<size_t, float>> &which) const;

private:
    void addRange(size_t j, int64_t lo, int64_t hi, float weight, float *dst) const;
    // Add `weight` times reference `j` of track `k` over the positions [p0, p0 + len) to `dst`.
    void addReference(size_t k, size_t j, float weight, int64_t p0, size_t len, float *dst) const;

    const std::vector<PosFile *> &m_files;
    const ArchiveMeta &m_meta;
    const std::map<std::pair<size_t, size_t>, LagTable> &m_tables;
};

} // namespace rfx
