/******************************************************************************
 * lagtable.cpp
 * rfx library - lag tables (specification 7)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "lagtable.h"

#include "bits.h"
#include "error.h"
#include "rangecoder.h"

#include <algorithm>
#include <array>

namespace rfx {

namespace {

constexpr size_t kGapContexts = 24;
constexpr size_t kGapClasses = 65;
constexpr size_t kMaxVarintShift = 63;
constexpr uint32_t kSmallestEscape = 4;

uint64_t zigzag(int64_t v)
{
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

int64_t unzigzag(uint64_t v)
{
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1u);
}

uint64_t gcd(uint64_t a, uint64_t b)
{
    while (b != 0) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// `n` (at most 64) equiprobable bits of `v`, high first.
void putDirect(RangeEncoder &enc, uint64_t v, uint32_t n)
{
    if (n > 32) {
        const uint32_t high = static_cast<uint32_t>(v >> 32) & static_cast<uint32_t>((uint64_t(1) << (n - 32)) - 1);
        enc.direct(high, n - 32);
        enc.direct(static_cast<uint32_t>(v), 32);
    } else if (n > 0) {
        enc.direct(static_cast<uint32_t>(v) & static_cast<uint32_t>((uint64_t(1) << n) - 1), n);
    }
}

uint64_t getDirect(RangeDecoder &dec, uint32_t n)
{
    if (n > 32) {
        const uint64_t high = dec.direct(n - 32);
        return (high << 32) | dec.direct(32);
    }
    if (n > 0) {
        return dec.direct(n);
    }
    return 0;
}

// The adaptive model of a table's gaps and steps (specification 7).
class TableModel
{
public:
    TableModel() : m_cls(kGapContexts) {}

    void putGap(RangeEncoder &enc, uint64_t gap)
    {
        const int64_t d = static_cast<int64_t>(gap) - static_cast<int64_t>(m_prevGap);
        const uint64_t mag = d < 0 ? 0 - static_cast<uint64_t>(d) : static_cast<uint64_t>(d);
        const uint32_t nb = bitLength(mag);
        const size_t c = context();
        for (uint32_t j = 0; j < nb; ++j) {
            enc.bit(m_cls[c][j], 1);
        }
        if (nb < 64) {
            enc.bit(m_cls[c][nb], 0);
        }
        if (nb >= 2) {
            enc.bit(m_mant[nb], static_cast<uint32_t>((mag >> (nb - 2)) & 1u));
            putDirect(enc, mag & ((uint64_t(1) << (nb - 2)) - 1), nb - 2);
        }
        if (nb >= 1) {
            const uint32_t neg = d < 0 ? 1 : 0;
            enc.bit(m_sign[m_prevSign], neg);
            m_prevSign = 1 + neg;
        } else {
            m_prevSign = 0;
        }
        m_prevGap = gap;
    }

    uint64_t getGap(RangeDecoder &dec)
    {
        const size_t c = context();
        uint32_t nb = 0;
        while (nb < 64 && dec.bit(m_cls[c][nb]) == 1) {
            ++nb;
        }
        uint64_t mag = nb >= 1 ? 1 : 0;
        if (nb >= 2) {
            mag = (mag << 1) | dec.bit(m_mant[nb]);
            mag = (mag << (nb - 2)) | getDirect(dec, nb - 2);
        }
        int64_t d = static_cast<int64_t>(mag);
        if (nb >= 1) {
            const uint32_t neg = dec.bit(m_sign[m_prevSign]);
            if (neg == 1) {
                d = static_cast<int64_t>(0 - static_cast<uint64_t>(d));
            }
            m_prevSign = 1 + neg;
        } else {
            m_prevSign = 0;
        }
        m_prevGap += static_cast<uint64_t>(d);
        return m_prevGap;
    }

    void putStep(RangeEncoder &enc, int64_t step)
    {
        const uint64_t mag = step < 0 ? 0 - static_cast<uint64_t>(step) : static_cast<uint64_t>(step);
        if (mag >= 1 && mag <= kSmallestEscape) {
            enc.bit(m_esc, 0);
            for (uint64_t j = 1; j < mag; ++j) {
                enc.bit(m_smag[m_prevMag][j - 1], 1);
            }
            if (mag < kSmallestEscape) {
                enc.bit(m_smag[m_prevMag][mag - 1], 0);
            }
            const uint32_t up = step > 0 ? 1 : 0;
            enc.bit(m_same[m_prevUp], up);
            m_prevUp = up;
            m_prevMag = static_cast<size_t>(mag) - 1;
        } else {
            enc.bit(m_esc, 1);
            const uint64_t z = zigzag(step);
            enc.direct(bitLength(z), 7);
            putDirect(enc, z, bitLength(z));
        }
    }

    int64_t getStep(RangeDecoder &dec)
    {
        if (dec.bit(m_esc) == 0) {
            size_t mag = 1;
            while (mag < kSmallestEscape && dec.bit(m_smag[m_prevMag][mag - 1]) == 1) {
                ++mag;
            }
            const uint32_t up = dec.bit(m_same[m_prevUp]);
            m_prevUp = up;
            m_prevMag = mag - 1;
            return up == 1 ? static_cast<int64_t>(mag) : -static_cast<int64_t>(mag);
        }
        const uint32_t nb = std::min<uint32_t>(dec.direct(7), 64);
        return unzigzag(getDirect(dec, nb));
    }

private:
    size_t context() const { return std::min<size_t>(bitLength(m_prevGap), kGapContexts - 1); }

    std::vector<std::array<P2, kGapClasses>> m_cls;
    std::array<P2, kGapClasses> m_mant{};
    std::array<P2, 3> m_sign{};
    P2 m_esc;
    std::array<P2, 2> m_same{};
    std::array<std::array<P2, 3>, 4> m_smag{};
    size_t m_prevMag = 0;
    uint64_t m_prevGap = 1;
    size_t m_prevSign = 0;
    size_t m_prevUp = 0;
};

} // namespace

void putVarint(std::vector<uint8_t> &out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v) | 0x80);
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

uint64_t getVarint(const uint8_t *data, size_t size, size_t &at)
{
    uint64_t v = 0;
    uint32_t shift = 0;
    for (;;) {
        if (at >= size) {
            throw Error("truncated varint");
        }
        const uint8_t b = data[at++];
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return v;
        }
        shift += 7;
        if (shift > kMaxVarintShift) {
            throw Error("varint too long");
        }
    }
}

int64_t LagTable::lagAt(int64_t p) const
{
    const auto it = std::upper_bound(pos.begin(), pos.end(), p < 0 ? 0 : static_cast<uint64_t>(p));
    const size_t i = static_cast<size_t>(it - pos.begin());
    return lag[i == 0 ? 0 : i - 1];
}

std::vector<uint8_t> LagTable::encode() const
{
    const size_t n = pos.size();
    uint64_t unit = 0;
    for (size_t i = 1; i < n; ++i) {
        unit = gcd(unit, pos[i] - pos[i - 1]);
    }
    unit = std::max<uint64_t>(unit, 1);
    std::vector<uint8_t> out;
    putVarint(out, n);
    putVarint(out, unit);
    if (n == 0) {
        return out;
    }
    putVarint(out, pos[0]);
    putVarint(out, zigzag(lag[0]));
    RangeEncoder enc;
    TableModel model;
    for (size_t i = 1; i < n; ++i) {
        model.putGap(enc, (pos[i] - pos[i - 1]) / unit);
        model.putStep(enc, static_cast<int64_t>(static_cast<uint64_t>(lag[i]) - static_cast<uint64_t>(lag[i - 1])));
    }
    const std::vector<uint8_t> coded = enc.finish();
    out.insert(out.end(), coded.begin(), coded.end());
    return out;
}

LagTable LagTable::decode(const uint8_t *data, size_t size)
{
    size_t at = 0;
    const uint64_t n = getVarint(data, size, at);
    const uint64_t unit = getVarint(data, size, at);
    if (n == 0) {
        throw Error("a lag table must start at position 0");
    }
    const uint64_t p0 = getVarint(data, size, at);
    const int64_t l0 = unzigzag(getVarint(data, size, at));
    if (p0 != 0) {
        throw Error("a lag table must start at position 0");
    }
    LagTable t;
    t.pos.push_back(p0);
    t.lag.push_back(l0);
    RangeDecoder dec(data + at, size - at);
    TableModel model;
    for (uint64_t i = 1; i < n; ++i) {
        const uint64_t gap = model.getGap(dec);
        const int64_t step = model.getStep(dec);
        if (dec.overrun()) {
            throw Error("truncated lag table");
        }
        t.pos.push_back(t.pos.back() + gap * unit);
        t.lag.push_back(static_cast<int64_t>(static_cast<uint64_t>(t.lag.back()) + static_cast<uint64_t>(step)));
    }
    return t;
}

} // namespace rfx
