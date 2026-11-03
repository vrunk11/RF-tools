/******************************************************************************
 * sha256.cpp
 * rfx library - SHA-256, with the processor's SHA instructions when it has them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "sha256.h"

#include <algorithm>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define RFX_SHA_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace rfx {

namespace {

constexpr size_t kBlockBytes = 64;

alignas(16) const uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t kInitialState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void blocksPortable(uint32_t *state, const uint8_t *data, size_t count)
{
    for (; count > 0; --count, data += kBlockBytes) {
        uint32_t w[64];
        for (int t = 0; t < 16; ++t) {
            w[t] = (uint32_t(data[4 * t]) << 24) | (uint32_t(data[4 * t + 1]) << 16) | (uint32_t(data[4 * t + 2]) << 8) | data[4 * t + 3];
        }
        for (int t = 16; t < 64; ++t) {
            const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int t = 0; t < 64; ++t) {
            const uint32_t big1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t t1 = h + big1 + choose + kRoundConstants[t] + w[t];
            const uint32_t big0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = big0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

#ifdef RFX_SHA_X86

#if defined(__GNUC__) || defined(__clang__)
#define RFX_SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#else
#define RFX_SHA_TARGET
#endif

// The block function with the SHA extensions: four rounds per step, the message
// schedule kept in four registers that take turns.
RFX_SHA_TARGET void blocksX86(uint32_t *state, const uint8_t *data, size_t count)
{
    const __m128i byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    // The state is held as ABEF and CDGH, which is how the instructions take it.
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&state[0]));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&state[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);
    state1 = _mm_shuffle_epi32(state1, 0x1B);
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);

    for (; count > 0; --count, data += kBlockBytes) {
        const __m128i saved0 = state0;
        const __m128i saved1 = state1;
        __m128i w[4];
        for (int i = 0; i < 16; ++i) {
            if (i < 4) {
                w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(data + 16 * i)), byteSwap);
            } else {
                // Four new words of the schedule from the sixteen before them.
                __m128i t = _mm_sha256msg1_epu32(w[i % 4], w[(i + 1) % 4]);
                t = _mm_add_epi32(t, _mm_alignr_epi8(w[(i + 3) % 4], w[(i + 2) % 4], 4));
                w[i % 4] = _mm_sha256msg2_epu32(t, w[(i + 3) % 4]);
            }
            __m128i msg = _mm_add_epi32(w[i % 4], _mm_load_si128(reinterpret_cast<const __m128i *>(&kRoundConstants[4 * i])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        }
        state0 = _mm_add_epi32(state0, saved0);
        state1 = _mm_add_epi32(state1, saved1);
    }

    tmp = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&state[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&state[4]), state1);
}

bool cpuHasSha()
{
    constexpr unsigned kShaBit = 1u << 29;
    constexpr unsigned kSse41Bit = 1u << 19;
    constexpr unsigned kSsse3Bit = 1u << 9;
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) {
        return false;
    }
    __cpuid(regs, 1);
    ecx = static_cast<unsigned>(regs[2]);
    __cpuidex(regs, 7, 0);
    ebx = static_cast<unsigned>(regs[1]);
#else
    if (__get_cpuid_max(0, nullptr) < 7) {
        return false;
    }
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    unsigned ecx1 = ecx;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    ecx = ecx1;
#endif
    return (ebx & kShaBit) != 0 && (ecx & kSse41Bit) != 0 && (ecx & kSsse3Bit) != 0;
}

#endif // RFX_SHA_X86

} // namespace

bool Sha256::hardwareAvailable()
{
#ifdef RFX_SHA_X86
    static const bool available = cpuHasSha();
    return available;
#else
    return false;
#endif
}

Sha256::Sha256(bool allowHardware) : m_blocks(blocksPortable)
{
    std::memcpy(m_state, kInitialState, sizeof(m_state));
#ifdef RFX_SHA_X86
    if (allowHardware && hardwareAvailable()) {
        m_blocks = blocksX86;
    }
#else
    (void) allowHardware;
#endif
}

void Sha256::update(const void *data, size_t length)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    m_length += length;
    if (m_buffered > 0) {
        const size_t take = std::min(length, kBlockBytes - m_buffered);
        std::memcpy(m_buffer + m_buffered, p, take);
        m_buffered += take;
        p += take;
        length -= take;
        if (m_buffered == kBlockBytes) {
            m_blocks(m_state, m_buffer, 1);
            m_buffered = 0;
        }
    }
    const size_t whole = length / kBlockBytes;
    if (whole > 0) {
        m_blocks(m_state, p, whole);
        p += whole * kBlockBytes;
        length -= whole * kBlockBytes;
    }
    if (length > 0) {
        std::memcpy(m_buffer, p, length);
        m_buffered = length;
    }
}

std::array<uint8_t, 32> Sha256::finish()
{
    const uint64_t bits = m_length * 8;
    uint8_t padding[kBlockBytes * 2] = {0x80};
    const size_t padLength = (m_buffered < 56 ? 56 : 120) - m_buffered;
    for (int i = 0; i < 8; ++i) {
        padding[padLength + static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    // update() would count the padding in the length: feed the blocks directly.
    uint8_t tail[kBlockBytes * 2];
    std::memcpy(tail, m_buffer, m_buffered);
    std::memcpy(tail + m_buffered, padding, padLength + 8);
    m_blocks(m_state, tail, (m_buffered + padLength + 8) / kBlockBytes);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(4 * i)] = static_cast<uint8_t>(m_state[i] >> 24);
        out[static_cast<size_t>(4 * i + 1)] = static_cast<uint8_t>(m_state[i] >> 16);
        out[static_cast<size_t>(4 * i + 2)] = static_cast<uint8_t>(m_state[i] >> 8);
        out[static_cast<size_t>(4 * i + 3)] = static_cast<uint8_t>(m_state[i]);
    }
    return out;
}

std::string Sha256::finishHex()
{
    static const char digits[] = "0123456789abcdef";
    const std::array<uint8_t, 32> d = finish();
    std::string out;
    for (const uint8_t b : d) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

} // namespace rfx
