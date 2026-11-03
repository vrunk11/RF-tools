/******************************************************************************
 * segment.h
 * rfx library - one segment of one track: samples against the predictor,
 * residuals through the range coder (specification 6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "predictor.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rfx {

// A segment starts from nothing (empty history, zero weights, fresh model), so
// segments are independent and are coded and decoded in parallel.

// The residuals of a segment of `n` samples from its coded bytes; false if the
// bytes are not valid.
bool decodeResiduals(const uint8_t *bytes, size_t size, size_t n, std::vector<int32_t> &residuals);

// The coded bytes of `residuals` (the model reads past residuals only, so the
// bytes can be parsed without knowing any prediction).
std::vector<uint8_t> encodeResiduals(const std::vector<int32_t> &residuals);

// The samples (shifted and mean-removed) whose residuals are `residuals`, with
// the reference window `window` (channels `windowStride` apart; unused without
// references).
std::vector<int32_t> reconstruct(const std::vector<int32_t> &residuals, const Preset &preset, bool useRef,
                                 const float *window, size_t windowStride);

// The residuals of the samples `y`.
std::vector<int32_t> predictionErrors(const std::vector<int32_t> &y, const Preset &preset, bool useRef,
                                      const float *window, size_t windowStride);

} // namespace rfx
