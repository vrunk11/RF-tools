/******************************************************************************
 * predictor.h
 * rfx library - the predictor of the .rfx format (specification 5)
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

// The parameters of the predictor of one track (specification 5.1).
struct Preset
{
    // Own past samples used with references.
    size_t own = 0;
    // Own past samples used when there are none (0 means `own`).
    size_t solo = 0;
    // Own past samples used with the plain mean of the references (the writer's).
    size_t plain = 0;
    // The mean of the references has 2 * taps + 1 values around the sample.
    size_t taps = 0;
    // References that also get a channel of their own, with 2 * ttaps + 1 values each.
    size_t ntop = 0;
    size_t ttaps = 0;
    // Powers of two: the covariance is updated every `stride` samples, the
    // weights solved every `every` samples.
    size_t stride = 1;
    size_t every = 1;
    // Forgetting factor per covariance update, as f32 bits.
    uint32_t lamBits = 0;

    // The writer's levels 1 (fast) to 4 (strongest); false for any other.
    static bool level(int level, Preset &out);

    size_t ownFor(bool useRef) const { return useRef ? own : (solo != 0 ? solo : own); }
    // Channels of the reference window: the mean, then one per top reference.
    size_t channels() const { return 1 + ntop; }
    // Values of a reference window for `n` samples, all channels.
    size_t windowLength(size_t n) const { return channels() * (n + 2 * taps); }
    // Number of regressors.
    size_t dimension(bool useRef) const;
    // Leaves of the dot product tree: the dimension rounded up to a power of two.
    size_t leaves(bool useRef) const;
};

// Exponentially weighted least squares on the past only, so a writer and a
// reader compute the same weights and none are stored. Every operation is an
// f32 operation rounded on its own: no fused multiply-add, no reassociation.
class Predictor
{
public:
    static constexpr size_t kMaxDimension = 64;

    Predictor(const Preset &preset, bool useRef);

    // Prediction of sample `i` from the samples learnt so far (`update`) and the
    // reference window: channel c starts at c * stride; the mean (channel 0) is
    // read at [i, i + 2*taps + 1), a top reference (channels 1..) at the middle
    // 2*ttaps + 1 of the same span.
    int32_t predict(const float *window, size_t stride, size_t i);

    // Learn sample `i` (value `y`) against the regressors of the last predict.
    void update(size_t i, int32_t y);

private:
    bool solve();

    Preset m_pre;
    bool m_useRef;
    size_t m_own;
    size_t m_d;
    size_t m_dp;
    float m_lam;
    std::vector<float> m_a;
    std::vector<float> m_b;
    // Weights and regressors: the part past d stays 0, which makes the padded
    // leaves of the dot product 0 without a test.
    std::vector<float> m_w;
    std::vector<float> m_r;
    std::vector<float> m_lc;
    std::vector<float> m_mr;
    std::vector<float> m_rdiag;
    std::vector<float> m_s;
};

} // namespace rfx
