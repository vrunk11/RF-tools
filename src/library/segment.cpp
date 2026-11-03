/******************************************************************************
 * segment.cpp
 * rfx library - one segment of one track: samples against the predictor,
 * residuals through the range coder (specification 6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "segment.h"

#include "rangecoder.h"
#include "residualmodel.h"

namespace rfx {

bool decodeResiduals(const uint8_t *bytes, size_t size, size_t n, std::vector<int32_t> &residuals)
{
    RangeDecoder dec(bytes, size);
    ResidualModel model;
    residuals.resize(n);
    for (size_t i = 0; i < n; ++i) {
        residuals[i] = model.decode(dec);
    }
    return !dec.overrun();
}

std::vector<uint8_t> encodeResiduals(const std::vector<int32_t> &residuals)
{
    RangeEncoder enc;
    ResidualModel model;
    for (const int32_t e : residuals) {
        model.encode(enc, e);
    }
    return enc.finish();
}

std::vector<int32_t> reconstruct(const std::vector<int32_t> &residuals, const Preset &preset, bool useRef,
                                 const float *window, size_t windowStride)
{
    Predictor predictor(preset, useRef);
    std::vector<int32_t> y(residuals.size());
    for (size_t i = 0; i < residuals.size(); ++i) {
        const int32_t p = predictor.predict(window, windowStride, i);
        y[i] = static_cast<int32_t>(static_cast<uint32_t>(p) + static_cast<uint32_t>(residuals[i]));
        predictor.update(i, y[i]);
    }
    return y;
}

std::vector<int32_t> predictionErrors(const std::vector<int32_t> &y, const Preset &preset, bool useRef,
                                      const float *window, size_t windowStride)
{
    Predictor predictor(preset, useRef);
    std::vector<int32_t> residuals(y.size());
    for (size_t i = 0; i < y.size(); ++i) {
        const int32_t p = predictor.predict(window, windowStride, i);
        residuals[i] = static_cast<int32_t>(static_cast<uint32_t>(y[i]) - static_cast<uint32_t>(p));
        predictor.update(i, y[i]);
    }
    return residuals;
}

} // namespace rfx
