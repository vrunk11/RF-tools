/******************************************************************************
 * sampleformat.h
 * rfx library - the sample formats of a capture file (specification 1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "posfile.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace rfx {

// The raw formats of ld-decode (its --format): a capture is read as a series of
// integer samples by index, and written back by index, so that unpack gives the
// very bytes pack was given. Bytes past the last whole group are the tail.
enum class SampleFormat { S16, U16, S8, U8, Lds };

// The ld-decode name: "s16", "r16", "s8", "r8", "lds".
const char *formatName(SampleFormat fmt);
// Accepts the names above (and "u16", "u8" for "r16", "r8"); false for any other.
bool parseFormat(const std::string &name, SampleFormat &fmt);

// Samples in a file of `size` bytes.
uint64_t formatSamples(SampleFormat fmt, uint64_t size);
// Bytes of a file of `size` bytes that its samples cover (the rest is the tail).
uint64_t formatCovered(SampleFormat fmt, uint64_t size);

// Samples [start, start + n) of a file of `count` samples into `out`; zero
// outside the file.
void loadSamples(SampleFormat fmt, PosFile &file, uint64_t count, int64_t start, int32_t *out, size_t n);

// Write the samples from sample index `start` (a whole group of samples, and a
// whole number of groups unless they end the file). Throws rfx::Error for a
// sample the format cannot hold.
void storeSamples(SampleFormat fmt, PosFile &file, uint64_t start, const int32_t *values, size_t n);

} // namespace rfx
