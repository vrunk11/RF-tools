/******************************************************************************
 * order.h
 * rfx library - the order to code the captures in: the cleanest of each
 * programme first
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "alignment.h"
#include "sampleformat.h"

#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

namespace rfx {

// How noisy each capture is. Captures of one programme carry the same picture,
// so on the same frames of the disc a capture that costs more bits to code on its
// own past alone is the noisier one. The windows are taken at frames (VBI codes,
// from pass 0) that every capture has, so the content cancels out of the
// comparison.
struct NoiseScore
{
    // Mean cost of coding it from its own past, bits per sample.
    double bits = 0;
    // RMS of the samples around their mean (gain and amplitude).
    double rms = 0;

    // The cost normalised by the amplitude: noise against signal, smaller is
    // cleaner. (The raw cost is wrong: a capture with a stronger signal costs more
    // bits and is the better reference.)
    double cost() const;
};

// The order to pack `inputs` in: captures that share frames form a group; groups
// keep the place of their first capture and each is sorted from the cleanest
// capture to the noisiest, so that the best reference comes first. A group whose
// noise cannot be compared keeps the order given. Returns the permutation (the
// original index of each place) and the scores by original index.
struct Plan
{
    std::vector<size_t> order;
    std::vector<std::optional<double>> noise;
};
Plan planOrder(const std::vector<QString> &inputs, const std::vector<SampleFormat> &fmts,
               const std::vector<std::vector<Field>> &fields, unsigned threads);

} // namespace rfx
