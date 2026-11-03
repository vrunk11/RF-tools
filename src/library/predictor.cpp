/******************************************************************************
 * predictor.cpp
 * rfx library - the predictor of the .rfx format (specification 5)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "predictor.h"

#include "error.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rfx {

namespace {

constexpr float kRidgeScale = 1e-6f;
constexpr float kRidgeFloor = 1e-6f;
constexpr float kPredictionLimit = 4194304.0f;
constexpr size_t kSolveWarmup = 4;

// Dot product of r and w (2 * H leaves) as the specified binary tree: leaf q
// plus leaf q + H, then the halves of the result, down to one.
template<size_t H>
inline float tree(const float *r, const float *w)
{
    float t[H];
    for (size_t q = 0; q < H; ++q) {
        const float x = r[q] * w[q];
        const float y = r[q + H] * w[q + H];
        t[q] = x + y;
    }
    for (size_t n = H / 2; n >= 1; n /= 2) {
        for (size_t q = 0; q < n; ++q) {
            const float v = t[q + n];
            t[q] += v;
        }
    }
    return t[0];
}

size_t roundUpToPowerOfTwo(size_t v)
{
    size_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

} // namespace

bool Preset::level(int level, Preset &out)
{
    size_t own, solo, plain, taps, ntop, stride, every;
    switch (level) {
    case 1: own = 6; solo = 8; plain = 8; taps = 3; ntop = 2; stride = 8; every = 128; break;
    case 2: own = 8; solo = 16; plain = 16; taps = 4; ntop = 3; stride = 4; every = 64; break;
    case 3: own = 8; solo = 32; plain = 16; taps = 6; ntop = 3; stride = 1; every = 16; break;
    case 4: own = 12; solo = 48; plain = 24; taps = 6; ntop = 4; stride = 1; every = 16; break;
    default: return false;
    }
    // lam = (1 - 1/1024)^stride, in double precision (by repeated squaring, as
    // the reference's powi does), rounded to f32.
    double lam = 1.0;
    double base = 1.0 - 1.0 / 1024.0;
    for (size_t e = stride; e != 0; e >>= 1) {
        if (e & 1) {
            lam *= base;
        }
        base *= base;
    }
    const float lamF = static_cast<float>(lam);
    out = Preset();
    out.own = own;
    out.solo = solo;
    out.plain = plain;
    out.taps = taps;
    out.ntop = ntop;
    out.ttaps = 1;
    out.stride = stride;
    out.every = every;
    std::memcpy(&out.lamBits, &lamF, sizeof(float));
    return true;
}

size_t Preset::dimension(bool useRef) const
{
    return ownFor(useRef) + (useRef ? 2 * taps + 1 + ntop * (2 * ttaps + 1) : 0);
}

size_t Preset::leaves(bool useRef) const
{
    return std::max<size_t>(roundUpToPowerOfTwo(dimension(useRef)), 2);
}

Predictor::Predictor(const Preset &preset, bool useRef)
    : m_pre(preset),
      m_useRef(useRef),
      m_own(preset.ownFor(useRef)),
      m_d(preset.dimension(useRef)),
      m_dp(preset.leaves(useRef))
{
    if (m_d > kMaxDimension) {
        throw Error("the predictor has at most 64 regressors");
    }
    if (m_own == 0 || m_pre.stride == 0 || m_pre.every == 0) {
        throw Error("the predictor parameters are not valid");
    }
    std::memcpy(&m_lam, &m_pre.lamBits, sizeof(float));
    const size_t width = std::max(m_dp, m_d);
    m_a.assign(m_d * m_d, 0.0f);
    m_b.assign(m_d, 0.0f);
    m_w.assign(width, 0.0f);
    m_r.assign(width, 0.0f);
    m_lc.assign(m_d * m_d, 0.0f);
    m_mr.assign(m_d * m_d, 0.0f);
    m_rdiag.assign(m_d, 0.0f);
    m_s.assign(m_d, 0.0f);
}

int32_t Predictor::predict(const float *window, size_t stride, size_t i)
{
    const size_t own = m_own;
    if (m_useRef) {
        const size_t n = 2 * m_pre.taps + 1;
        std::memcpy(&m_r[own], window + i, n * sizeof(float));
        const size_t m = 2 * m_pre.ttaps + 1;
        const size_t skip = m_pre.taps - m_pre.ttaps;
        for (size_t c = 1; c <= m_pre.ntop; ++c) {
            std::memcpy(&m_r[own + n + (c - 1) * m], window + c * stride + i + skip, m * sizeof(float));
        }
    }
    float dot;
    switch (m_dp) {
    case 2: dot = tree<1>(m_r.data(), m_w.data()); break;
    case 4: dot = tree<2>(m_r.data(), m_w.data()); break;
    case 8: dot = tree<4>(m_r.data(), m_w.data()); break;
    case 16: dot = tree<8>(m_r.data(), m_w.data()); break;
    case 32: dot = tree<16>(m_r.data(), m_w.data()); break;
    default: dot = tree<32>(m_r.data(), m_w.data()); break;
    }
    const float p = std::floor(dot + 0.5f);
    if (!(p == p)) {
        return 0;
    }
    return static_cast<int32_t>(std::min(std::max(p, -kPredictionLimit), kPredictionLimit));
}

void Predictor::update(size_t i, int32_t y)
{
    const size_t d = m_d;
    const float yf = static_cast<float>(y);
    if ((i & (m_pre.stride - 1)) == 0) {
        const float lam = m_lam;
        for (size_t p = 0; p < d; ++p) {
            const float rp = m_r[p];
            const float t = rp * yf;
            const float lb = lam * m_b[p];
            m_b[p] = lb + t;
            float *row = &m_a[p * d];
            for (size_t q = 0; q <= p; ++q) {
                const float u = rp * m_r[q];
                const float la = lam * row[q];
                row[q] = la + u;
            }
        }
    }
    if ((i & (m_pre.every - 1)) == m_pre.every - 1 && i > kSolveWarmup * d) {
        std::memcpy(m_s.data(), m_b.data(), d * sizeof(float));
        if (solve()) {
            std::memcpy(m_w.data(), m_s.data(), d * sizeof(float));
        }
    }
    // The newest sample becomes the first of the own history.
    std::memmove(&m_r[1], &m_r[0], (m_own - 1) * sizeof(float));
    m_r[0] = yf;
}

// Solve (a + ridge I) x = b for the symmetric positive definite a (d x d, row
// major, the lower triangle is read); m_s receives x. False if the matrix is
// not positive definite (specification 5.5).
//
// The factor is kept by column in m_lc (m_lc[k * d + i] is row i of column k)
// so that the work below a pivot runs along contiguous memory, and by row in
// m_mr for the backward substitution. Each element still sees the same
// operations in the same order as the textbook row loops.
bool Predictor::solve()
{
    const size_t d = m_d;
    float *lc = m_lc.data();
    float *mr = m_mr.data();
    float *s = m_s.data();
    const float *a = m_a.data();

    float trace = 0.0f;
    for (size_t i = 0; i < d; ++i) {
        trace += a[i * d + i];
    }
    const float mean = trace / static_cast<float>(d);
    const float scaled = mean * kRidgeScale;
    const float ridge = scaled + kRidgeFloor;
    for (size_t p = 0; p < d; ++p) {
        for (size_t q = 0; q <= p; ++q) {
            lc[q * d + p] = a[p * d + q];
        }
        lc[p * d + p] += ridge;
    }
    for (size_t j = 0; j < d; ++j) {
        float v = lc[j * d + j];
        for (size_t k = 0; k < j; ++k) {
            const float p = lc[k * d + j] * lc[k * d + j];
            v -= p;
        }
        if (v <= 0.0f) {
            return false;
        }
        const float dj = std::sqrt(v);
        const float rd = 1.0f / dj;
        lc[j * d + j] = dj;
        m_rdiag[j] = rd;
        float *col = &lc[j * d + j + 1];
        const size_t below = d - j - 1;
        for (size_t k = 0; k < j; ++k) {
            const float ljk = lc[k * d + j];
            const float *src = &lc[k * d + j + 1];
            for (size_t x = 0; x < below; ++x) {
                const float p = src[x] * ljk;
                col[x] -= p;
            }
        }
        for (size_t x = 0; x < below; ++x) {
            col[x] *= rd;
        }
        mr[j * d + j] = dj;
        for (size_t x = 0; x < below; ++x) {
            mr[(j + 1 + x) * d + j] = col[x];
        }
    }
    for (size_t i = 0; i < d; ++i) {
        s[i] *= m_rdiag[i];
        const float si = s[i];
        const float *col = &lc[i * d + i + 1];
        for (size_t x = 0; x < d - i - 1; ++x) {
            const float p = col[x] * si;
            s[i + 1 + x] -= p;
        }
    }
    for (size_t i = d; i-- > 0;) {
        s[i] *= m_rdiag[i];
        const float si = s[i];
        const float *row = &mr[i * d];
        for (size_t x = 0; x < i; ++x) {
            const float p = row[x] * si;
            s[x] -= p;
        }
    }
    return true;
}

} // namespace rfx
