/******************************************************************************
 * diagnostics.h
 * rfx library - the commands that tell how the captures and the predictor behave
 * (noise, reference choice, speed of each stage, CPU against GPU)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "packer.h"

#include <QString>

#include <cstddef>
#include <vector>

namespace rfx {

// How noisy each capture is: the cost of coding it on its own past alone, on the
// same frames of the disc (from the pass 0 dumps `pass0_<i>.txt` of `pass0Dir`, in
// the order of the inputs). The cleaner a capture, the better a reference. Prints a
// table to standard output.
void printNoise(const std::vector<QString> &inputs, const QString &pass0Dir, size_t windows, size_t windowLength, unsigned threads);

// Would other references have cost less for the capture `target` (its place in the
// order of `inputs`)? Prints the cost of the mean of all its references (what pack
// does), of each one alone, of the best few together, and of the best choice
// segment by segment, on `segments` segments spread through the capture.
void printReferenceChoice(const std::vector<QString> &inputs, const PackOptions &options, size_t target, size_t segments);

// Single-thread speed of each stage (predict, entropy encode and decode,
// reconstruct) on the first `segments` segments of a 16-bit capture, and where the
// predictor's time goes.
void printSpeed(const QString &capture, int level, size_t segments, size_t segment);

// The predictions of the first `segments` segments of a 16-bit capture, CPU against
// GPU, and the first sample where they differ. With `withReference`, from a
// synthetic reference window too.
void printGpuCheck(const QString &capture, int level, size_t segments, size_t segment, bool withReference);

} // namespace rfx
