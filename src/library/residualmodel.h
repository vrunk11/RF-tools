/******************************************************************************
 * residualmodel.h
 * rfx library - the model of the prediction errors (specification 8.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "rangecoder.h"

#include <array>
#include <cstdint>
#include <vector>

namespace rfx {

// A prediction error is zigzagged, shifted by one and sent as an Elias-gamma
// class (adaptive unary, one probability set per context) and the bits under
// the leading one (the first two adaptive, the rest raw). The context is the
// magnitude of the recent residuals; the model reads past residuals only.
class ResidualModel
{
public:
    ResidualModel() : m_unary(kContexts) {}

    void encode(RangeEncoder &enc, int32_t e);
    int32_t decode(RangeDecoder &dec);

private:
    static constexpr size_t kContexts = 48;
    static constexpr size_t kClasses = 33;
    static constexpr uint32_t kAvgStart = 64;
    static constexpr uint32_t kAvgCap = 1u << 20;

    size_t context() const;
    void adapt(uint32_t u);

    std::vector<std::array<P2, kClasses>> m_unary;
    std::array<P2, kClasses> m_m1{};
    std::array<std::array<P2, 2>, kClasses> m_m2{};
    uint32_t m_avg = kAvgStart;
};

} // namespace rfx
