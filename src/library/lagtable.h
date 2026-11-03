/******************************************************************************
 * lagtable.h
 * rfx library - lag tables: where a track's positions are in a reference
 * track (specification 7)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rfx {

// An integer lag of a track against an earlier one, as a step function of the
// position: lag[i] holds from pos[i] up to pos[i + 1].
struct LagTable
{
    std::vector<uint64_t> pos;
    std::vector<int64_t> lag;

    // The lag at position `p` (the first lag before the table starts).
    int64_t lagAt(int64_t p) const;

    // Compact form: a few varints, then a range-coded stream.
    std::vector<uint8_t> encode() const;
    // Throws rfx::Error for a table that is not valid.
    static LagTable decode(const uint8_t *data, size_t size);
};

// LEB128, 7 bits per byte, low group first.
void putVarint(std::vector<uint8_t> &out, uint64_t v);
// Throws rfx::Error at a truncated or too long varint.
uint64_t getVarint(const uint8_t *data, size_t size, size_t &at);

} // namespace rfx
