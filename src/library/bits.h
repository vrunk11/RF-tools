/******************************************************************************
 * bits.h
 * rfx library - bit tricks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <cstdint>

namespace rfx {

// Number of bits needed to write `v` (0 for 0).
inline uint32_t bitLength(uint64_t v)
{
    uint32_t n = 0;
    while (v != 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

} // namespace rfx
