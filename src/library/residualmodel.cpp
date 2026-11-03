/******************************************************************************
 * residualmodel.cpp
 * rfx library - the model of the prediction errors (specification 8.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "residualmodel.h"

#include "bits.h"

namespace rfx {

size_t ResidualModel::context() const
{
    const uint32_t bl = bitLength(m_avg);
    const uint32_t second = bl >= 2 ? (m_avg >> (bl - 2)) & 1u : 0u;
    const size_t c = 2 * bl + second;
    return c < kContexts - 1 ? c : kContexts - 1;
}

void ResidualModel::adapt(uint32_t u)
{
    m_avg = m_avg - (m_avg >> 4) + (u < kAvgCap ? u : kAvgCap);
}

void ResidualModel::encode(RangeEncoder &enc, int32_t e)
{
    const uint32_t u = (static_cast<uint32_t>(e) << 1) ^ static_cast<uint32_t>(e >> 31);
    const uint32_t v = u + 1;
    const uint32_t nb = bitLength(v) - 1;
    const size_t c = context();
    for (uint32_t i = 0; i < nb; ++i) {
        enc.bit(m_unary[c][i], 1);
    }
    enc.bit(m_unary[c][nb], 0);
    if (nb >= 1) {
        const uint32_t top = (v >> (nb - 1)) & 1u;
        enc.bit(m_m1[nb], top);
        if (nb >= 2) {
            const uint32_t second = (v >> (nb - 2)) & 1u;
            enc.bit(m_m2[nb][top], second);
            if (nb >= 3) {
                enc.direct(v & ((1u << (nb - 2)) - 1), nb - 2);
            }
        }
    }
    adapt(u);
}

int32_t ResidualModel::decode(RangeDecoder &dec)
{
    const size_t c = context();
    uint32_t nb = 0;
    while (nb < kClasses - 1 && dec.bit(m_unary[c][nb]) == 1) {
        ++nb;
    }
    uint32_t v = 1;
    if (nb >= 1) {
        const uint32_t top = dec.bit(m_m1[nb]);
        v = (v << 1) | top;
        if (nb >= 2) {
            const uint32_t second = dec.bit(m_m2[nb][top]);
            v = (v << 1) | second;
            if (nb >= 3) {
                v = (v << (nb - 2)) | dec.direct(nb - 2);
            }
        }
    }
    const uint32_t u = v - 1;
    adapt(u);
    return static_cast<int32_t>(u >> 1) ^ -static_cast<int32_t>(u & 1u);
}

} // namespace rfx
