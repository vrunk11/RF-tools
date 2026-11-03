/******************************************************************************
 * rangecoder.h
 * rfx library - range coder and adaptive bit probabilities (specification 8.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rfx {

// A bit probability kept at two speeds (16-bit states, shifts 5 and 8); the
// coder uses their mean. Integer only.
class P2
{
public:
    // Probability of a 0 bit in 1/32768, kept away from the ends.
    uint32_t p15() const
    {
        const uint32_t p = (static_cast<uint32_t>(m_fast) + m_slow) >> 2;
        return p < kMinP15 ? kMinP15 : (p > kMaxP15 ? kMaxP15 : p);
    }

    void learn(uint32_t bit)
    {
        if (bit == 0) {
            m_fast = static_cast<uint16_t>(m_fast + ((65535u - m_fast) >> 5));
            m_slow = static_cast<uint16_t>(m_slow + ((65535u - m_slow) >> 8));
        } else {
            m_fast = static_cast<uint16_t>(m_fast - (m_fast >> 5));
            m_slow = static_cast<uint16_t>(m_slow - (m_slow >> 8));
        }
    }

private:
    static constexpr uint32_t kMinP15 = 24;
    static constexpr uint32_t kMaxP15 = 32768 - 24;

    uint16_t m_fast = 32768;
    uint16_t m_slow = 32768;
};

constexpr uint32_t kRangeTop = 1u << 24;

class RangeEncoder
{
public:
    void bit(P2 &prob, uint32_t bit)
    {
        const uint32_t bound = (m_range >> 15) * prob.p15();
        if (bit == 0) {
            m_range = bound;
        } else {
            m_low += bound;
            m_range -= bound;
        }
        prob.learn(bit);
        normalise();
    }

    // `n` equiprobable bits of `value`, high first.
    void direct(uint32_t value, uint32_t n)
    {
        for (uint32_t i = n; i-- > 0;) {
            m_range >>= 1;
            if ((value >> i) & 1u) {
                m_low += m_range;
            }
            normalise();
        }
    }

    std::vector<uint8_t> finish()
    {
        for (int i = 0; i < 5; ++i) {
            shiftLow();
        }
        return std::move(m_out);
    }

private:
    void normalise()
    {
        while (m_range < kRangeTop) {
            m_range <<= 8;
            shiftLow();
        }
    }

    void shiftLow()
    {
        if (static_cast<uint32_t>(m_low) < 0xFF000000u || (m_low >> 32) != 0) {
            const uint8_t carry = static_cast<uint8_t>(m_low >> 32);
            uint8_t temp = m_cache;
            do {
                m_out.push_back(static_cast<uint8_t>(temp + carry));
                temp = 0xFF;
            } while (--m_cacheSize != 0);
            m_cache = static_cast<uint8_t>((m_low >> 24) & 0xFF);
        }
        ++m_cacheSize;
        m_low = (m_low & 0x00FFFFFFu) << 8;
    }

    uint64_t m_low = 0;
    uint32_t m_range = 0xFFFFFFFFu;
    uint8_t m_cache = 0;
    uint64_t m_cacheSize = 1;
    std::vector<uint8_t> m_out;
};

class RangeDecoder
{
public:
    RangeDecoder(const uint8_t *data, size_t size) : m_data(data), m_size(size), m_pos(1)
    {
        for (int i = 0; i < 4; ++i) {
            m_code = (m_code << 8) | next();
        }
    }

    uint32_t bit(P2 &prob)
    {
        const uint32_t bound = (m_range >> 15) * prob.p15();
        uint32_t bit;
        if (m_code < bound) {
            m_range = bound;
            bit = 0;
        } else {
            m_code -= bound;
            m_range -= bound;
            bit = 1;
        }
        prob.learn(bit);
        normalise();
        return bit;
    }

    uint32_t direct(uint32_t n)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < n; ++i) {
            m_range >>= 1;
            uint32_t b = 0;
            if (m_code >= m_range) {
                m_code -= m_range;
                b = 1;
            }
            value = (value << 1) | b;
            normalise();
        }
        return value;
    }

    // A corrupt stream runs far past the end of its data.
    bool overrun() const { return m_pos > m_size + kOverrunBytes; }

private:
    static constexpr size_t kOverrunBytes = 8;

    uint8_t next()
    {
        const uint8_t b = m_pos < m_size ? m_data[m_pos] : 0;
        ++m_pos;
        return b;
    }

    void normalise()
    {
        while (m_range < kRangeTop) {
            m_range <<= 8;
            m_code = (m_code << 8) | next();
        }
    }

    const uint8_t *m_data;
    size_t m_size;
    size_t m_pos;
    uint32_t m_range = 0xFFFFFFFFu;
    uint32_t m_code = 0;
};

} // namespace rfx
