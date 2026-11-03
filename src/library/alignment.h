/******************************************************************************
 * alignment.h
 * rfx library - where the captures stand against each other: the line
 * positions of ld-decode's pass 0 and the lag tables built from them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "lagtable.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// A field of a capture as ld-decode's pass 0 located it (LD_DUMP_PASS0): its
// frame code (VBI), if it read one, and the sample position of every line.
struct Field
{
    bool first = false;
    std::optional<std::string> code;
    std::vector<double> lines;
};

// Read a pass 0 dump (`pass0_<i>.txt`). Throws rfx::Error.
std::vector<Field> parsePass0(const QString &path);

// Run ld-decode's pass 0 on the captures, so that the dumps are written to `dir`.
void runPass0(const QString &ldDecode, const std::vector<QString> &inputs, double rateMhz, const std::string &format,
              const QString &dir);

// The lag must leave the nearest integer by more than this to change.
constexpr double kHysteresisFine = 0.75;
// Coarser tables (half as many steps, a fifth of the bytes) for captures whose
// predictor has a channel for each of its best references, which absorb the
// misalignment.
constexpr double kHysteresisCoarse = 1.5;

// The lag of `target` (sample positions in the later capture) against
// `reference`, from the fields both located with the same frame code; nothing
// when they share no field.
std::optional<LagTable> buildLagTable(const std::vector<Field> &target, const std::vector<Field> &reference,
                                      uint64_t targetWords, double hysteresis);

} // namespace rfx
